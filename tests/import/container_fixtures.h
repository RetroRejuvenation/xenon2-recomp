// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Encoders for the importer tests: they build DMS archives, extended ADFs and raw Amiga MFM tracks from synthetic
// data (never game data), so the decoders can be tested without any input file. The DMS encoders cover the
// modes that are simple to encode (none, simple, quick, medium, and heavy with a degenerate one-symbol tree).
#pragma once
#include "import/dms.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fx {

using Bytes = std::vector<uint8_t>;

inline void put16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
inline void put32(Bytes& b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }

struct Rng {
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
};

// MSB-first bit writer, the order the DMS decoders read in.
struct BitWriter {
    Bytes out;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t v, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            acc = (acc << 1) | ((v >> i) & 1);
            if (++n == 8) { out.push_back(uint8_t(acc)); acc = 0; n = 0; }
        }
    }
    Bytes finish() {
        if (n) put(0, 8 - n);
        return out;
    }
};

// --- DMS -----------------------------------------------------------------------------------------------

struct DmsTrack {
    uint16_t number;
    uint8_t mode, flags;
    Bytes packed;
    Bytes unpacked;
    size_t intermediate = 0; // 0: packed.size()
};

inline Bytes dms_track_bytes(const DmsTrack& t, bool break_crc = false, bool break_checksum = false) {
    Bytes b = {'T', 'R'};
    put16(b, t.number);
    put16(b, 0);
    put16(b, uint32_t(t.packed.size()));
    put16(b, uint32_t(t.intermediate ? t.intermediate : t.packed.size()));
    put16(b, uint32_t(t.unpacked.size()));
    b.push_back(t.flags);
    b.push_back(t.mode);
    put16(b, uint32_t(x2::import::dms_checksum16(t.unpacked.data(), t.unpacked.size())) ^ (break_checksum ? 1u : 0u));
    put16(b, uint32_t(x2::import::dms_crc16(t.packed.data(), t.packed.size())) ^ (break_crc ? 1u : 0u));
    put16(b, x2::import::dms_crc16(b.data(), 18));
    b.insert(b.end(), t.packed.begin(), t.packed.end());
    return b;
}

inline Bytes dms_archive(const std::vector<Bytes>& tracks, uint16_t geninfo = 0, uint16_t disk_type = 2, uint16_t first = 0,
                         uint16_t last = 79) {
    Bytes h = {'D', 'M', 'S', '!', 0, 0, 0, 0, 0, 0};
    put16(h, geninfo);
    put32(h, 0x12345678);
    put16(h, first);
    put16(h, last);
    h.resize(46, 0);
    put16(h, 111);
    put16(h, 0);
    put16(h, disk_type);
    put16(h, 0);
    put16(h, x2::import::dms_crc16(&h[4], 50));
    for (const Bytes& t : tracks) h.insert(h.end(), t.begin(), t.end());
    return h;
}

// Simple mode: 0x90 escapes. Runs of 4 or more become 0x90 n v (n < 0xFF) or 0x90 0xFF v hi lo.
inline Bytes rle_encode(const Bytes& in) {
    Bytes out;
    for (size_t i = 0; i < in.size();) {
        size_t run = 1;
        while (i + run < in.size() && in[i + run] == in[i] && run < 60000) ++run;
        if (run >= 4) {
            if (run < 0xFF) { out.push_back(0x90); out.push_back(uint8_t(run)); out.push_back(in[i]); }
            else { out.push_back(0x90); out.push_back(0xFF); out.push_back(in[i]); put16(out, uint32_t(run)); }
            i += run;
        } else {
            for (size_t k = 0; k < run; ++k) {
                out.push_back(in[i]);
                if (in[i] == 0x90) out.push_back(0);
            }
            i += run;
        }
    }
    return out;
}

// A mirror of the sliding window the QUICK and MEDIUM decoders keep, so the encoder can find matches.
struct LzWindow {
    std::vector<uint8_t> w;
    uint16_t pos, mask;
    LzWindow(size_t size, uint16_t init_pos, uint16_t m) : w(size, 0), pos(init_pos), mask(m) {}
    void push(uint8_t b) { w[pos & mask] = b; ++pos; }
    // Would a match (distance, length) reproduce data[at..at+length)? (byte-wise, so overlapping copies work)
    bool matches(const Bytes& data, size_t at, uint32_t distance, uint32_t length) const {
        std::vector<uint8_t> copy = w;
        uint16_t p = pos, src = uint16_t(pos - distance - 1);
        for (uint32_t k = 0; k < length; ++k) {
            uint8_t b = copy[src & mask];
            if (at + k >= data.size() || data[at + k] != b) return false;
            copy[p & mask] = b;
            ++p;
            ++src;
        }
        return true;
    }
};

struct LzState {
    LzWindow quick{256, 251, 0xFF};
    LzWindow medium{0x4000, 0x3FBE, 0x3FFF};
};

// Quick: flag 1 + 8 bits literal; flag 0 + 2 bits (length - 2) + 8 bits distance. State carries to the next
// track when the track's keep-state flag is set (then pass the same LzState and call end_quick_track).
inline Bytes quick_encode(const Bytes& in, LzState& st) {
    BitWriter bw;
    for (size_t i = 0; i < in.size();) {
        bool done = false;
        if (i % 3 != 2) // leave some literals even when a match exists
            for (uint32_t len = 5; len >= 2 && !done; --len)
                for (uint32_t dist = 0; dist < 256 && !done; ++dist)
                    if (i + len <= in.size() && st.quick.matches(in, i, dist, len)) {
                        bw.put(0, 1);
                        bw.put(len - 2, 2);
                        bw.put(dist, 8);
                        for (uint32_t k = 0; k < len; ++k) st.quick.push(in[i + k]);
                        i += len;
                        done = true;
                    }
        if (!done) {
            bw.put(1, 1);
            bw.put(in[i], 8);
            st.quick.push(in[i]);
            ++i;
        }
    }
    st.quick.pos = uint16_t((st.quick.pos + 5) & 0xFF);
    return bw.finish();
}

// The bit string that makes the MEDIUM decoder produce (length, distance): found by search over the decoder's
// two-level static table, which can express every pair.
inline uint32_t medium_code(uint32_t length, uint32_t distance) {
    static std::vector<uint32_t> map = [] {
        std::vector<uint32_t> m(size_t(64) * 16384, 0xFFFFFFFFu);
        const uint8_t* dc = x2::import::dms_d_code();
        const uint8_t* dl = x2::import::dms_d_len();
        for (uint32_t p = 0; p < 256; ++p)
            for (uint32_t e1 = 0; e1 < (1u << dl[p]); ++e1) {
                uint32_t mid = ((p << dl[p]) | e1) & 0xFF;
                for (uint32_t e2 = 0; e2 < (1u << dl[mid]); ++e2) {
                    uint32_t low = ((mid << dl[mid]) | e2) & 0xFF;
                    size_t key = size_t(dc[p]) * 16384 + (size_t(dc[mid]) << 8 | low);
                    if (m[key] == 0xFFFFFFFFu) m[key] = p << 16 | e1 << 8 | e2;
                }
            }
        return m;
    }();
    return map[size_t(length - 3) * 16384 + distance];
}

// Medium: flag 1 + 8 bits literal; flag 0 + the (length, distance) code.
inline Bytes medium_encode(const Bytes& in, LzState& st) {
    const uint8_t* dl = x2::import::dms_d_len();
    BitWriter bw;
    for (size_t i = 0; i < in.size();) {
        bool done = false;
        if (i % 3 != 2)
            for (uint32_t len = 66; len >= 3 && !done; len -= (len > 8 ? 7 : 1))
                for (uint32_t dist : {0u, 1u, 2u, 7u, 63u, 255u, 1023u, 4095u, 16000u})
                    if (i + len <= in.size() && st.medium.matches(in, i, dist, len)) {
                        uint32_t code = medium_code(len, dist);
                        uint32_t p = code >> 16, e1 = (code >> 8) & 0xFF, e2 = code & 0xFF;
                        uint32_t mid = ((p << dl[p]) | e1) & 0xFF;
                        bw.put(0, 1);
                        bw.put(p, 8);
                        bw.put(e1, int(dl[p]));
                        bw.put(e2, int(dl[mid]));
                        for (uint32_t k = 0; k < len; ++k) st.medium.push(in[i + k]);
                        i += len;
                        done = true;
                        break;
                    }
        if (!done) {
            bw.put(1, 1);
            bw.put(in[i], 8);
            st.medium.push(in[i]);
            ++i;
        }
    }
    st.medium.pos = uint16_t((st.medium.pos + 66) & 0x3FFF);
    return bw.finish();
}

// Heavy with a degenerate tree: no character tree and no position tree, every code is the one symbol `sym`
// (so the track is `size` copies of it). Needs the track flag "rebuild trees" (2).
inline Bytes heavy_single_symbol(uint8_t sym) {
    BitWriter bw;
    bw.put(0, 9);   // no character tree...
    bw.put(sym, 9); // ...every code is this symbol
    bw.put(0, 5);   // no position tree...
    bw.put(0, 5);   // ...every code is position 0
    return bw.finish();
}

// --- Amiga MFM -----------------------------------------------------------------------------------------

// Encodes one track of 11 sectors into raw MFM bits the way an Amiga writes it, then rotates the bit stream
// left by `rotate` bits (so the sectors do not start at a byte boundary). Returns the bytes; bit_length is the
// exact number of bits (not a multiple of 8 when `extra_bits` is used to pad the gap).
struct MfmTrack {
    Bytes bytes;
    uint32_t bit_length = 0;
};

inline MfmTrack mfm_encode_track(const uint8_t* sectors, int track, uint32_t rotate = 0, uint32_t extra_bits = 0,
                                 int corrupt_data_sector = -1) {
    std::vector<uint8_t> bits; // one entry per bit, most significant first
    uint32_t prev = 0;
    auto put_word = [&](uint32_t w) { // 16 data bits in the even positions of w: one raw long of clock + data pairs
        for (int i = 15; i >= 0; --i) {
            uint32_t data = (w >> (2 * i)) & 1;
            uint32_t clock = (!prev && !data) ? 1 : 0;
            bits.push_back(uint8_t(clock));
            bits.push_back(uint8_t(data));
            prev = data;
        }
    };
    auto put_raw16 = [&](uint32_t v) { for (int i = 15; i >= 0; --i) bits.push_back(uint8_t((v >> i) & 1)); prev = v & 1; };
    auto odd = [](uint32_t l) { return (l >> 1) & 0x55555555u; };
    auto even = [](uint32_t l) { return l & 0x55555555u; };
    for (int s = 0; s < 11; ++s) {
        put_raw16(0xAAAA);
        put_raw16(0xAAAA);
        put_raw16(0x4489);
        put_raw16(0x4489);
        uint32_t info = 0xFF000000u | uint32_t(track) << 16 | uint32_t(s) << 8 | uint32_t(11 - s);
        std::vector<uint32_t> words = {odd(info), even(info), 0, 0, 0, 0, 0, 0, 0, 0}; // label: zeros
        uint32_t hchk = 0;
        for (uint32_t w : words) hchk ^= w;
        hchk &= 0x55555555u;
        const uint8_t* d = sectors + size_t(s) * 512;
        std::vector<uint32_t> longs(128);
        for (int i = 0; i < 128; ++i) longs[size_t(i)] = uint32_t(d[4 * i]) << 24 | uint32_t(d[4 * i + 1]) << 16 | uint32_t(d[4 * i + 2]) << 8 | d[4 * i + 3];
        std::vector<uint32_t> dwords;
        for (uint32_t l : longs) dwords.push_back(odd(l));
        for (uint32_t l : longs) dwords.push_back(even(l));
        uint32_t dchk = 0;
        for (uint32_t w : dwords) dchk ^= w;
        dchk &= 0x55555555u;
        if (s == corrupt_data_sector) dwords[5] ^= 0x00010000u;
        for (uint32_t w : words) put_word(w);
        put_word(0);
        put_word(hchk);
        put_word(0);
        put_word(dchk);
        for (uint32_t w : dwords) put_word(w);
    }
    for (uint32_t i = 0; i < 64 + extra_bits; ++i) bits.push_back(uint8_t(i & 1)); // gap
    MfmTrack out;
    out.bit_length = uint32_t(bits.size());
    out.bytes.assign((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) {
        size_t src = (i + rotate) % bits.size();
        if (bits[src]) out.bytes[i >> 3] = uint8_t(out.bytes[i >> 3] | (0x80 >> (i & 7)));
    }
    return out;
}

// --- extended ADF ---------------------------------------------------------------------------------------

struct ExtTrack {
    uint8_t type;   // 0 normal, 1 raw
    Bytes data;
    uint32_t bits = 0;
    uint8_t revolutions = 1;
};

inline Bytes ext_adf(const std::vector<ExtTrack>& tracks) {
    Bytes b = {'U', 'A', 'E', '-', '1', 'A', 'D', 'F'};
    put16(b, 0);
    put16(b, uint32_t(tracks.size()));
    for (const ExtTrack& t : tracks) {
        put16(b, 0);
        b.push_back(uint8_t(t.revolutions - 1));
        b.push_back(t.type);
        put32(b, uint32_t(t.data.size()));
        put32(b, t.type == 1 ? t.bits : uint32_t(t.data.size()) * 8);
    }
    for (const ExtTrack& t : tracks) b.insert(b.end(), t.data.begin(), t.data.end());
    return b;
}

} // namespace fx
