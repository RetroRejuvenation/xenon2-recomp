// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Builds IPF (SPS/CAPS) files from synthetic data for the importer tests (never game data): standard Amiga tracks
// written the way an encoder-type-1 IPF stores them (a raw sync block, one MFM data element, a type-0 gap), raw
// non-Amiga tracks, noise tracks, and the record framing with its CRCs. Corruptions are made on the result.
#pragma once
#include "container_fixtures.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace fx {

inline uint32_t ipf_crc32(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

inline void set32(Bytes& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24);
    b[at + 1] = uint8_t(v >> 16);
    b[at + 2] = uint8_t(v >> 8);
    b[at + 3] = uint8_t(v);
}

// "TAG" + size + crc + `fields` (the bytes after the 12-byte header, already sized to the record's length - 12),
// with the CRC filled in. A DATA record's payload is appended by the caller after calling this.
inline Bytes ipf_record(const char* tag, const Bytes& fields) {
    Bytes r(tag, tag + 4);
    put32(r, uint32_t(12 + fields.size()));
    put32(r, 0);
    r.insert(r.end(), fields.begin(), fields.end());
    set32(r, 8, ipf_crc32(r.data(), r.size()));
    return r;
}

struct IpfInfoSpec {
    uint32_t encoder_type = 1, min_cyl = 0, max_cyl = 83, min_head = 0, max_head = 1;
};

inline Bytes ipf_caps() { return ipf_record("CAPS", {}); }

inline Bytes ipf_info(const IpfInfoSpec& s = {}) {
    Bytes f;
    put32(f, 1);               // type: floppy
    put32(f, s.encoder_type);  // encoder type
    put32(f, 1);               // encoder revision
    put32(f, 297);             // release
    put32(f, 1);               // revision
    put32(f, 0);               // origin
    put32(f, s.min_cyl);
    put32(f, s.max_cyl);
    put32(f, s.min_head);
    put32(f, s.max_head);
    put32(f, 20260101);        // credit date
    put32(f, 0);               // credit time
    put32(f, 1);               // platform 1 = Amiga
    for (int i = 0; i < 3; ++i) put32(f, 0);
    for (int i = 0; i < 5; ++i) put32(f, 0);
    f.resize(96 - 12, 0);
    return ipf_record("INFO", f);
}

struct IpfTrackSpec {
    uint32_t cylinder = 0, head = 0, type = 2, index_cells = 0, size_cells = 0, block_count = 0, weak = 0, data_index = 0;
    uint32_t signal_type = 1;
};

inline Bytes ipf_imge(const IpfTrackSpec& t) {
    Bytes f;
    put32(f, t.cylinder);
    put32(f, t.head);
    put32(f, t.type);
    put32(f, t.signal_type);
    put32(f, (t.size_cells + 7) / 8); // size in bytes
    put32(f, 0);
    put32(f, t.index_cells);
    put32(f, 0);
    put32(f, 0);
    put32(f, t.size_cells);
    put32(f, t.block_count);
    put32(f, 0); // encoder process
    put32(f, t.weak);
    put32(f, t.data_index);
    f.resize(80 - 12, 0);
    return ipf_record("IMGE", f);
}

inline Bytes ipf_data(const Bytes& payload, uint32_t data_index) {
    Bytes f;
    put32(f, uint32_t(payload.size()));
    put32(f, uint32_t(payload.size() * 8));
    put32(f, ipf_crc32(payload.data(), payload.size()));
    put32(f, data_index);
    Bytes r = ipf_record("DATA", f);
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
}

// The data bits of one Amiga sector body (everything after the two sync words) as an encoder-type-1 IPF stores
// them: 16 data bits per longword of the raw layout, 270 of them (info, label, two checksums, odd then even data).
inline Bytes amiga_sector_data_bits(const uint8_t* sector, int track, int s) {
    auto odd = [](uint32_t l) { return (l >> 1) & 0x55555555u; };
    auto even = [](uint32_t l) { return l & 0x55555555u; };
    uint32_t info = 0xFF000000u | uint32_t(track) << 16 | uint32_t(s) << 8 | uint32_t(11 - s);
    std::vector<uint32_t> words = {odd(info), even(info), 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t hchk = 0;
    for (uint32_t w : words) hchk ^= w;
    hchk &= 0x55555555u;
    std::vector<uint32_t> longs(128), dwords;
    for (int i = 0; i < 128; ++i)
        longs[size_t(i)] = uint32_t(sector[4 * i]) << 24 | uint32_t(sector[4 * i + 1]) << 16 | uint32_t(sector[4 * i + 2]) << 8 | sector[4 * i + 3];
    for (uint32_t l : longs) dwords.push_back(odd(l));
    for (uint32_t l : longs) dwords.push_back(even(l));
    uint32_t dchk = 0;
    for (uint32_t w : dwords) dchk ^= w;
    dchk &= 0x55555555u;
    words.push_back(0);
    words.push_back(hchk);
    words.push_back(0);
    words.push_back(dchk);
    words.insert(words.end(), dwords.begin(), dwords.end());
    Bytes out;
    for (uint32_t w : words) {
        uint32_t v = 0;
        for (int i = 15; i >= 0; --i) v = (v << 1) | ((w >> (2 * i)) & 1);
        put16(out, v);
    }
    return out;
}

constexpr uint32_t kIpfSectorDataCells = 32 + 270 * 32; // sync + MFM data
constexpr uint32_t kIpfGapCells = 420;
constexpr uint32_t kIpfStandardTrackCells = 11 * (kIpfSectorDataCells + kIpfGapCells);

// An encoder-type-1 standard Amiga track payload (11 blocks). `data_offset_override` lets a test point a block's
// data at the wrong place.
inline Bytes ipf_standard_track_payload(const uint8_t* sectors, int track) {
    Bytes table(32 * 11, 0), streams;
    for (int s = 0; s < 11; ++s) {
        size_t th = size_t(s) * 32;
        set32(table, th, kIpfSectorDataCells);
        set32(table, th + 4, kIpfGapCells);
        set32(table, th + 16, 1); // encoder: MFM
        set32(table, th + 28, uint32_t(32 * 11 + streams.size()));
        streams.push_back((1 << 5) | 1); // op 1 (sync, unencoded cells), 1 byte of size
        streams.push_back(4);            // 4 bytes
        for (uint8_t b : {0x44, 0x89, 0x44, 0x89}) streams.push_back(b);
        streams.push_back((2 << 5) | 2); // op 2 (MFM data), 2 bytes of size
        put16(streams, 540);
        Bytes bits = amiga_sector_data_bits(sectors + size_t(s) * 512, track, s);
        streams.insert(streams.end(), bits.begin(), bits.end());
        streams.push_back(0); // end of the block's data elements
    }
    table.insert(table.end(), streams.begin(), streams.end());
    return table;
}

// A track of raw non-Amiga cells (encoder 2, op 4): one block, `data_bytes` of cells, then a type-0 gap.
inline Bytes ipf_raw_track_payload(uint32_t data_bytes, uint32_t gap_cells, uint8_t fill) {
    Bytes table(32, 0);
    set32(table, 0, data_bytes * 8);
    set32(table, 4, gap_cells);
    set32(table, 16, 2);
    set32(table, 28, 32);
    Bytes b = table;
    b.push_back((2 << 5) | 4); // op 4, 2 bytes of size
    put16(b, data_bytes);
    for (uint32_t i = 0; i < data_bytes; ++i) b.push_back(uint8_t(fill ^ (i * 37)));
    b.push_back(0);
    return b;
}

struct IpfBuild {
    uint32_t standard_index_base = 2300; // index cells for track n: base + n
    int noise_track = -1;                // a type-1 noise track at this track number (cells of weak bits)
    int raw_track = -1;                  // a non-Amiga raw track at this track number
    int tracks = 160;
};

// An IPF of a whole disk: `adf` supplies the sectors of the standard tracks.
inline Bytes ipf_of_adf(const Bytes& adf, const IpfBuild& cfg = {}) {
    Bytes out = ipf_caps();
    Bytes i = ipf_info();
    out.insert(out.end(), i.begin(), i.end());
    for (int t = 0; t < cfg.tracks; ++t) {
        IpfTrackSpec s;
        s.cylinder = uint32_t(t / 2);
        s.head = uint32_t(t % 2);
        s.data_index = uint32_t(t);
        Bytes payload;
        if (t == cfg.noise_track) {
            s.type = 1;
            s.size_cells = 100000;
            payload = Bytes(32, 0);
        } else if (t == cfg.raw_track) {
            s.type = 2;
            s.block_count = 1;
            s.size_cells = 12000 * 8 + 4000;
            s.index_cells = 3000;
            payload = ipf_raw_track_payload(12000, 4000, 0x5A);
        } else {
            s.type = 2;
            s.block_count = 11;
            s.size_cells = kIpfStandardTrackCells;
            s.index_cells = cfg.standard_index_base + uint32_t(t);
            payload = ipf_standard_track_payload(adf.data() + size_t(t) * 5632, t);
        }
        Bytes im = ipf_imge(s), da = ipf_data(payload, uint32_t(t));
        out.insert(out.end(), im.begin(), im.end());
        out.insert(out.end(), da.begin(), da.end());
    }
    return out;
}

// Recomputes every record CRC and payload CRC of `ipf` in place (after a test changed bytes the parser would
// otherwise reject on CRC alone). Stops at the first record that does not fit.
inline void ipf_fix_crcs(Bytes& ipf) {
    size_t pos = 0;
    while (ipf.size() - pos >= 12) {
        uint32_t tsize = uint32_t(ipf[pos + 4]) << 24 | uint32_t(ipf[pos + 5]) << 16 | uint32_t(ipf[pos + 6]) << 8 | ipf[pos + 7];
        if (tsize < 12 || ipf.size() - pos < tsize) return;
        size_t payload = 0;
        bool is_data = std::memcmp(&ipf[pos], "DATA", 4) == 0 && tsize == 28;
        if (is_data) {
            payload = uint32_t(ipf[pos + 12]) << 24 | uint32_t(ipf[pos + 13]) << 16 | uint32_t(ipf[pos + 14]) << 8 | ipf[pos + 15];
            if (payload > ipf.size() - pos - tsize) return;
            set32(ipf, pos + 20, ipf_crc32(&ipf[pos + tsize], payload));
        }
        set32(ipf, pos + 8, 0);
        set32(ipf, pos + 8, ipf_crc32(&ipf[pos], tsize));
        pos += tsize + payload;
    }
}

} // namespace fx
