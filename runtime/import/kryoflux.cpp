// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// KryoFlux stream files (DTC output: track00.0.raw, track00.1.raw, ...). Encoding, from the KryoFlux stream
// protocol document (rev 1.1):
//
//   0x00-0x07  Flux2    two bytes: (b0 << 8) + next byte
//   0x08       Nop1     one byte, ignored
//   0x09       Nop2     two bytes, 0x0A Nop3 three bytes
//   0x0B       Ovl16    the next flux value gets 0x10000 added (may repeat)
//   0x0C       Flux3    three bytes: (b1 << 8) + b2
//   0x0D       OOB      type, 16-bit little-endian size, data: 1 StreamInfo, 2 Index, 3 StreamEnd, 4 KFInfo (text),
//                       0x0D end of file
//   0x0E-0xFF  Flux1    one byte: the value
//
// A flux value counts ticks of the sample clock (default 24.027428 MHz, "sck=" in the KFInfo text). Index blocks give
// the position in the stream data (out-of-band bytes excluded) of the flux value that follows the index pulse, and
// the sample counter: ticks from the preceding flux transition to the index pulse.
#include "import/flux.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace x2::import {

namespace {
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
constexpr double kDefaultSck = 18432000.0 * 73.0 / 14.0 / 4.0; // 24027428.57 Hz
constexpr size_t kMaxCells = 16u << 20;
constexpr size_t kMaxIndexes = 4096;

struct IndexMark {
    uint32_t stream_pos, sample_counter;
};
} // namespace

bool kryoflux_track_from_name(const std::string& name, int& cylinder, int& side) {
    size_t slash = name.find_last_of("/\\");
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    std::string low;
    for (char c : base) low.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    // trackNN.S.raw
    if (low.size() < 12 || low.compare(0, 5, "track") != 0) return false;
    size_t i = 5, digits = 0;
    int cyl = 0;
    while (i < low.size() && std::isdigit(static_cast<unsigned char>(low[i])) && digits < 3) cyl = cyl * 10 + (low[i++] - '0'), ++digits;
    if (digits < 2 || i + 6 != low.size() || low[i] != '.' || (low[i + 1] != '0' && low[i + 1] != '1') || low.compare(i + 2, 4, ".raw") != 0)
        return false;
    if (cyl > 83) return false;
    cylinder = cyl;
    side = low[i + 1] - '0';
    return true;
}

bool read_kryoflux_stream(const std::vector<uint8_t>& d, FluxTrack& out, std::string& why, std::vector<std::string>* warnings) {
    out = FluxTrack();
    double sck = kDefaultSck;
    std::vector<uint32_t> cell_ticks;     // flux cell lengths in sample-clock ticks
    std::vector<uint32_t> cell_start_pos; // stream position of each cell's first byte
    std::vector<IndexMark> idx;
    uint32_t pos = 0;      // stream data bytes consumed so far (OOB excluded)
    uint64_t overflow = 0; // pending Ovl16 additions
    uint32_t cell_first = 0;
    bool in_cell = false;
    size_t i = 0;
    bool eof = false, stream_error = false;
    auto begin_cell = [&]() { if (!in_cell) { cell_first = pos; in_cell = true; } };
    auto emit = [&](uint64_t v) {
        uint64_t total = overflow + v;
        overflow = 0;
        in_cell = false;
        if (cell_ticks.size() >= kMaxCells) return false;
        cell_ticks.push_back(uint32_t(std::min<uint64_t>(total, 0xFFFFFFFFu)));
        cell_start_pos.push_back(cell_first);
        return true;
    };
    while (i < d.size() && !eof) {
        uint8_t b = d[i];
        if (b <= 0x07) {
            if (i + 1 >= d.size()) { why = "KryoFlux stream is truncated in a flux value"; return false; }
            begin_cell();
            if (!emit((uint64_t(b) << 8) | d[i + 1])) { why = "KryoFlux stream holds an implausible number of flux cells"; return false; }
            i += 2, pos += 2;
        } else if (b == 0x08) {
            ++i, ++pos;
        } else if (b == 0x09) {
            if (i + 2 > d.size()) { why = "KryoFlux stream is truncated"; return false; }
            i += 2, pos += 2;
        } else if (b == 0x0A) {
            if (i + 3 > d.size()) { why = "KryoFlux stream is truncated"; return false; }
            i += 3, pos += 3;
        } else if (b == 0x0B) {
            begin_cell();
            overflow += 0x10000;
            ++i, ++pos;
        } else if (b == 0x0C) {
            if (i + 2 >= d.size()) { why = "KryoFlux stream is truncated in a flux value"; return false; }
            begin_cell();
            if (!emit((uint64_t(d[i + 1]) << 8) | d[i + 2])) { why = "KryoFlux stream holds an implausible number of flux cells"; return false; }
            i += 3, pos += 3;
        } else if (b == 0x0D) {
            if (i + 4 > d.size()) { why = "KryoFlux stream is truncated in an out-of-band block"; return false; }
            uint8_t type = d[i + 1];
            uint16_t size = rd16(&d[i + 2]);
            if (type == 0x0D) { eof = true; break; } // the end block's size field is a fixed marker, not a length
            if (size_t(i) + 4 + size > d.size()) { why = "KryoFlux stream is truncated in an out-of-band block"; return false; }
            const uint8_t* body = d.data() + i + 4;
            if (type == 0x02) { // Index
                if (size < 12) { why = "KryoFlux index block is too short"; return false; }
                if (idx.size() < kMaxIndexes) idx.push_back({rd32(body), rd32(body + 4)});
            } else if (type == 0x03) { // StreamEnd
                if (size >= 8 && rd32(body + 4) != 0) stream_error = true;
            } else if (type == 0x04) { // KFInfo: "name=..., sck=24027428.5714285, ick=..."
                std::string text(reinterpret_cast<const char*>(body), size);
                size_t at = text.find("sck=");
                if (at != std::string::npos) {
                    double v = std::strtod(text.c_str() + at + 4, nullptr);
                    if (v > 1e6 && v < 1e9) sck = v;
                }
            }
            i += 4 + size;
        } else {
            begin_cell();
            if (!emit(b)) { why = "KryoFlux stream holds an implausible number of flux cells"; return false; }
            ++i, ++pos;
        }
    }
    if (cell_ticks.empty()) { why = "KryoFlux stream holds no flux data"; return false; }
    if (warnings && stream_error) warnings->push_back("the KryoFlux device reported a stream error (buffering problem or missing index) for this track");
    if (warnings && !eof) warnings->push_back("KryoFlux stream has no end-of-file block (truncated?)");

    const double ns_per_tick = 1e9 / sck;
    auto to_ns = [&](uint64_t ticks) { return uint32_t(std::min<double>(double(ticks) * ns_per_tick + 0.5, 4e9)); };

    // Cell number at which each index pulse falls: the first cell that starts at or after the indicated position.
    std::vector<size_t> at;
    std::vector<uint32_t> before; // sample counter: ticks of the straddling cell that lay before the index
    for (const IndexMark& m : idx) {
        size_t c = size_t(std::lower_bound(cell_start_pos.begin(), cell_start_pos.end(), m.stream_pos) - cell_start_pos.begin());
        if (c >= cell_ticks.size()) continue;
        if (!at.empty() && c <= at.back()) continue;
        at.push_back(c);
        before.push_back(m.sample_counter);
    }
    auto make_rev = [&](size_t from, size_t to, uint32_t lead_ticks, uint32_t tail_ticks) {
        FluxRev r;
        r.ns.reserve(to - from);
        for (size_t c = from; c < to; ++c) {
            uint64_t t = cell_ticks[c];
            if (c == from) t = t > lead_ticks ? t - lead_ticks : 1;
            r.ns.push_back(to_ns(t));
        }
        r.tail_ns = to_ns(tail_ticks);
        return r;
    };
    if (at.size() >= 2) {
        for (size_t k = 0; k + 1 < at.size(); ++k) {
            // The cell straddling index k+1 is shared: its part before the index closes revolution k.
            out.revs.push_back(make_rev(at[k], at[k + 1], before[k], before[k + 1]));
        }
    } else {
        if (warnings) warnings->push_back(at.empty() ? "KryoFlux stream has no index blocks: treated as a single revolution of unknown start"
                                                       : "KryoFlux stream holds less than one full revolution between index pulses: treated as one revolution");
        size_t from = at.empty() ? 0 : at[0];
        out.revs.push_back(make_rev(from, cell_ticks.size(), at.empty() ? 0 : before[0], 0));
    }
    return true;
}

} // namespace x2::import
