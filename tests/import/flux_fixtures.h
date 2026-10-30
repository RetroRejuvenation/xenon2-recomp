// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A synthetic flux generator for the flux tests: turns an Amiga MFM track (container_fixtures.h) into the transition
// times a drive would deliver (spindle speed variation, timing jitter, a randomly placed index pulse per revolution),
// and wraps them as SCP, KryoFlux stream and HFE files. No game data: the sectors are random.
#pragma once
#include "container_fixtures.h"
#include "import/flux.h"

#include <cmath>
#include <vector>

namespace fx {

struct FluxModel {
    double cell_ns = 2000.0;    // nominal MFM cell
    double speed_offset = 0.0;  // constant spindle error (fraction), e.g. 0.015
    double wow = 0.005;         // slow sinusoidal speed variation (fraction)
    double jitter = 0.0;        // gaussian timing noise on every transition, as a fraction of the cell
    double index_jitter_cells = 6; // each revolution's index pulse lands this many cells (uniform +-) from nominal
    uint32_t seed = 1;
};

// A continuous transition timeline over `revs` revolutions of one circular MFM track, `pre` extra cells before the
// first index pulse and `post` after the last (a stream file starts and stops mid-revolution).
struct Timeline {
    std::vector<double> t;     // transition times, ns, strictly increasing
    std::vector<double> index; // index pulse times, ns
};

inline double gauss(Rng& r) {
    double u1 = (double(r.next() % 1000000u) + 1.0) / 1000001.0, u2 = double(r.next() % 1000000u) / 1000000.0;
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

inline Timeline make_timeline(const MfmTrack& trk, int revs, size_t pre, size_t post, const FluxModel& m) {
    Rng r{m.seed * 2654435761u + 12345u};
    Timeline tl;
    const size_t n = trk.bit_length;
    size_t total = pre + size_t(revs) * n + post;
    double clock = 0;
    auto bit_at = [&](size_t i) { size_t k = i % n; return (trk.bytes[k >> 3] >> (7 - (k & 7))) & 1; };
    // pre cells come from the end of the circular stream so that the data stays continuous
    size_t start = n - (pre % n);
    double phase = double(r.next() % 6283) / 1000.0;
    double last_t = -1e18;
    std::vector<size_t> index_cells;
    for (int k = 0; k < revs; ++k) index_cells.push_back(pre + size_t(k) * n + size_t(int(std::lround((double(r.next() % 2001) / 1000.0 - 1.0) * m.index_jitter_cells))) * (k > 0 ? 1 : 0));
    size_t next_index = 0;
    for (size_t c = 0; c < total; ++c) {
        double speed = 1.0 + m.speed_offset + m.wow * std::sin(clock / 1.9e8 * 6.283185307179586 + phase);
        double cell = m.cell_ns * speed;
        if (next_index < index_cells.size() && c == index_cells[next_index]) { tl.index.push_back(clock); ++next_index; }
        if (bit_at(start + c)) {
            double tt = clock + cell * 0.5;
            if (m.jitter > 0) tt += gauss(r) * m.jitter * m.cell_ns;
            if (tt <= last_t + 200.0) tt = last_t + 200.0;
            tl.t.push_back(tt);
            last_t = tt;
        }
        clock += cell;
    }
    return tl;
}

// Slices a timeline into revolutions between consecutive index pulses, as SCP stores them.
inline x2::import::FluxTrack scp_style_revs(const Timeline& tl) {
    x2::import::FluxTrack ft;
    for (size_t k = 0; k + 1 < tl.index.size(); ++k) {
        double a = tl.index[k], b = tl.index[k + 1];
        x2::import::FluxRev rev;
        double prev = a;
        for (double tt : tl.t) {
            if (tt < a) continue;
            if (tt >= b) break;
            rev.ns.push_back(uint32_t(std::lround(tt - prev)));
            prev = tt;
        }
        rev.tail_ns = uint32_t(std::lround(b - prev));
        ft.revs.push_back(std::move(rev));
    }
    return ft;
}

// --- SCP ------------------------------------------------------------------------------------------------

inline void le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }

// `tracks[t]` holds revolutions for track number t (cylinder * 2 + side); an empty entry is not written.
inline Bytes scp_file(const std::vector<x2::import::FluxTrack>& tracks, int revs, int resolution = 0, bool bad_checksum = false) {
    Bytes b = {'S', 'C', 'P', 0x24, 0x80, uint8_t(revs), 0, 0, 0x01, 0, 0, uint8_t(resolution)};
    int last = 0;
    for (size_t t = 0; t < tracks.size(); ++t) if (!tracks[t].revs.empty()) last = int(t);
    b[7] = uint8_t(last);
    le32(b, 0);
    size_t table = b.size();
    b.resize(table + 4 * 168, 0);
    const double tick = 25.0 * double(resolution + 1);
    for (size_t t = 0; t < tracks.size(); ++t) {
        const x2::import::FluxTrack& ft = tracks[t];
        if (ft.revs.empty()) continue;
        uint32_t off = uint32_t(b.size());
        for (int i = 0; i < 4; ++i) b[table + 4 * t + size_t(i)] = uint8_t(off >> (8 * i));
        b.push_back('T'); b.push_back('R'); b.push_back('K'); b.push_back(uint8_t(t));
        size_t entries = b.size();
        b.resize(entries + 12 * size_t(revs), 0);
        Bytes data;
        for (int r = 0; r < revs; ++r) {
            const x2::import::FluxRev& rev = ft.revs[size_t(r) % ft.revs.size()];
            uint32_t count = 0;
            double total = rev.tail_ns;
            size_t data_off = b.size() - off + data.size();
            double cum = 0;
            long prev_tick = 0;
            for (uint32_t ns : rev.ns) {
                total += ns;
                cum += ns;
                // Hardware timestamps flux in whole ticks, so the quantisation errors telescope instead of adding up.
                long at = std::lround(cum / tick);
                uint32_t ticks = uint32_t(at - prev_tick);
                prev_tick = at;
                while (ticks > 0xFFFF) { data.push_back(0); data.push_back(0); ticks -= 0x10000; ++count; }
                data.push_back(uint8_t(ticks >> 8));
                data.push_back(uint8_t(ticks));
                ++count;
            }
            size_t e = entries + 12 * size_t(r);
            uint32_t idx = uint32_t(std::lround(total / tick));
            for (int i = 0; i < 4; ++i) { b[e + size_t(i)] = uint8_t(idx >> (8 * i)); b[e + 4 + size_t(i)] = uint8_t(count >> (8 * i)); b[e + 8 + size_t(i)] = uint8_t(uint32_t(data_off) >> (8 * i)); }
        }
        b.insert(b.end(), data.begin(), data.end());
    }
    uint32_t sum = 0;
    for (size_t i = 0x10; i < b.size(); ++i) sum += b[i];
    if (bad_checksum) sum ^= 0x5A5A;
    for (int i = 0; i < 4; ++i) b[12 + size_t(i)] = uint8_t(sum >> (8 * i));
    return b;
}

// --- KryoFlux stream -------------------------------------------------------------------------------------

inline Bytes kryoflux_stream(const Timeline& tl, double sck = 24027428.5714285, bool with_info = true, bool with_eof = true) {
    Bytes b;
    auto oob = [&](uint8_t type, const Bytes& body) {
        b.push_back(0x0D);
        b.push_back(type);
        b.push_back(uint8_t(body.size()));
        b.push_back(uint8_t(body.size() >> 8));
        b.insert(b.end(), body.begin(), body.end());
    };
    if (with_info) {
        std::string s = "name=KryoFlux DiskSystem, version=3.00s, sck=24027428.5714285, ick=3003428.5714285625";
        Bytes body(s.begin(), s.end());
        body.push_back(0);
        oob(0x04, body);
    }
    const double ns_tick = 1e9 / sck;
    uint32_t pos = 0;
    size_t next_index = 0;
    double prev = tl.index.empty() ? 0 : std::min(tl.index[0], tl.t.empty() ? 0 : tl.t[0]) - 5000;
    const double t0 = prev;
    long prev_tick = 0;
    auto put_value = [&](uint32_t v) {
        Bytes cell;
        while (v > 0xFFFF) { cell.push_back(0x0B); v -= 0x10000; }
        if (v >= 0x0E && v <= 0xFF) cell.push_back(uint8_t(v));                                  // Flux1
        else if (v < 0x800) { cell.push_back(uint8_t(v >> 8)); cell.push_back(uint8_t(v)); }      // Flux2
        else { cell.push_back(0x0C); cell.push_back(uint8_t(v >> 8)); cell.push_back(uint8_t(v)); } // Flux3
        b.insert(b.end(), cell.begin(), cell.end());
        pos += uint32_t(cell.size());
    };
    double clock_acc = 0;
    for (double tt : tl.t) {
        // index pulses that fall before this transition are announced before the cell that completes after them
        while (next_index < tl.index.size() && tl.index[next_index] <= tt) {
            double since_last = tl.index[next_index] - prev;
            Bytes body;
            auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) body.push_back(uint8_t(v >> (8 * i))); };
            p32(pos);
            p32(uint32_t(std::lround(since_last / ns_tick)));
            p32(uint32_t((clock_acc + (tl.index[next_index] - prev)) / ns_tick / 8.0));
            oob(0x02, body);
            ++next_index;
        }
        // Whole-tick timestamps (the device counts a free-running clock), so rounding errors do not accumulate.
        long at = std::lround((tt - t0) / ns_tick);
        uint32_t ticks = uint32_t(at - prev_tick);
        if (ticks == 0) ticks = 1;
        prev_tick += ticks;
        put_value(ticks);
        clock_acc += tt - prev;
        prev = tt;
    }
    {
        Bytes body;
        for (int i = 0; i < 4; ++i) body.push_back(uint8_t(pos >> (8 * i)));
        for (int i = 0; i < 4; ++i) body.push_back(0);
        oob(0x03, body);
    }
    if (with_eof) {
        b.push_back(0x0D); b.push_back(0x0D); b.push_back(0x0D); b.push_back(0x0D);
    }
    return b;
}

// --- HFE -------------------------------------------------------------------------------------------------

inline uint8_t rev8(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

// `side0[c]` and `side1[c]` hold the raw MFM bytes (most significant bit first, a whole number of bytes) of cylinder c.
inline Bytes hfe_file(const std::vector<MfmTrack>& side0, const std::vector<MfmTrack>& side1, int bitrate_kbps = 250, int encoding = 1) {
    size_t cyls = side0.size();
    Bytes b(512, 0xFF);
    std::memcpy(b.data(), "HXCPICFE", 8);
    b[8] = 0; b[9] = uint8_t(cyls); b[10] = 2; b[11] = uint8_t(encoding);
    b[12] = uint8_t(bitrate_kbps); b[13] = uint8_t(bitrate_kbps >> 8);
    b[14] = 300 & 0xFF; b[15] = 300 >> 8; b[16] = 4; b[17] = 0;
    b[18] = 1; b[19] = 0; // track list at block 1
    Bytes list(((cyls * 4 + 511) / 512) * 512, 0);
    Bytes data;
    size_t first_block = 1 + list.size() / 512;
    for (size_t c = 0; c < cyls; ++c) {
        size_t len = std::max(side0[c].bytes.size(), side1[c].bytes.size()) * 2;
        size_t blocks = (len + 511) / 512;
        size_t blk = first_block + data.size() / 512;
        list[4 * c] = uint8_t(blk); list[4 * c + 1] = uint8_t(blk >> 8);
        list[4 * c + 2] = uint8_t(len); list[4 * c + 3] = uint8_t(len >> 8);
        Bytes tr(blocks * 512, 0);
        for (int s = 0; s < 2; ++s) {
            const Bytes& src = (s ? side1[c] : side0[c]).bytes;
            for (size_t j = 0; j < src.size(); ++j) tr[(j / 256) * 512 + size_t(s) * 256 + (j % 256)] = rev8(src[j]);
        }
        data.insert(data.end(), tr.begin(), tr.end());
    }
    b.insert(b.end(), list.begin(), list.end());
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

} // namespace fx
