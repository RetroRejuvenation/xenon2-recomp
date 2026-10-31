// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/rnc.h"

#include <array>
#include <cstring>

namespace x2::import {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct Crc16Table {
    std::array<uint16_t, 256> t{};
    Crc16Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xA001u : c >> 1;
            t[i] = uint16_t(c);
        }
    }
};

// LSB-first bit reader over 16-bit little-endian words. `pos` is the byte offset of the next word to load; literal bytes are
// copied from `pos` directly (they follow the words already consumed), leaving the bits already loaded in place.
struct Bits {
    const uint8_t* d;
    size_t end; // end of the packed data
    size_t pos;
    uint64_t buf = 0;
    int cnt = 0;
    bool bad = false;
    void fill(int n) {
        while (cnt < n) {
            if (pos + 1 >= end) { bad = true; buf |= 0; cnt += 16; continue; } // zeros past the end; `bad` stops the decode
            buf |= uint64_t(d[pos] | d[pos + 1] << 8) << cnt;
            pos += 2;
            cnt += 16;
        }
    }
    uint32_t peek(int n) {
        fill(n);
        return uint32_t(buf & ((uint64_t(1) << n) - 1));
    }
    void adv(int n) { buf >>= n; cnt -= n; }
    uint32_t read(int n) {
        if (n == 0) return 0;
        uint32_t v = peek(n);
        adv(n);
        return v;
    }
};

uint32_t mirror(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

struct Entry { uint8_t len; uint16_t code; uint8_t sym; };
using Table = std::vector<Entry>;

// 5-bit leaf count, 4-bit code length per leaf, canonical codes (shortest first, in symbol order), read MSB-first.
bool read_table(Bits& b, Table& out) {
    out.clear();
    const uint32_t num = b.read(5);
    if (!num) return !b.bad;
    uint8_t lens[32] = {};
    for (uint32_t i = 0; i < num; ++i) lens[i] = uint8_t(b.read(4));
    uint32_t code = 0;
    for (int length = 1; length <= 16; ++length) {
        for (uint32_t s = 0; s < num; ++s) {
            if (lens[s] != length) continue;
            if (code >= (1u << length)) return false; // over-subscribed code
            out.push_back({uint8_t(length), uint16_t(mirror(code, length)), uint8_t(s)});
            ++code;
        }
        code <<= 1;
    }
    return !b.bad;
}

bool read_symbol(Bits& b, const Table& t, uint32_t& value) {
    for (const Entry& e : t) {
        if (b.peek(e.len) == e.code) {
            b.adv(e.len);
            uint32_t s = e.sym;
            if (s >= 2) s = (1u << (s - 1)) | b.read(int(s) - 1);
            value = s;
            return !b.bad;
        }
    }
    return false;
}

} // namespace

uint16_t rnc_crc16(const uint8_t* data, size_t n) {
    static const Crc16Table kT;
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) crc = uint16_t((crc >> 8) ^ kT.t[(crc ^ data[i]) & 0xFF]);
    return crc;
}

bool rnc_header(const uint8_t* d, size_t n, size_t off, RncHeader& h) {
    if (off > n || n - off < kRncHeaderBytes || std::memcmp(d + off, "RNC\x01", 4) != 0) return false;
    h.unpacked = be32(d + off + 4);
    h.packed = be32(d + off + 8);
    h.unpacked_crc = be16(d + off + 12);
    h.packed_crc = be16(d + off + 14);
    h.leeway = d[off + 16];
    h.chunks = d[off + 17];
    return true;
}

bool rnc_unpack(const uint8_t* d, size_t n, size_t off, std::vector<uint8_t>& out, RncHeader& h, std::string& why, size_t max_unpacked) {
    out.clear();
    if (!rnc_header(d, n, off, h)) { why = "no RNC method 1 header"; return false; }
    if (h.unpacked > max_unpacked) { why = "the stream asks for " + std::to_string(h.unpacked) + " unpacked bytes"; return false; }
    if (h.packed > n - off - kRncHeaderBytes) { why = "the packed data runs past the end of the disk"; return false; }
    const uint8_t* src = d + off + kRncHeaderBytes;
    if (rnc_crc16(src, h.packed) != h.packed_crc) { why = "the CRC16 of the packed data does not match (unreadable or damaged sectors)"; return false; }
    // The bit reader may load a word or two past the packed bytes (a decoder reads ahead); they are the data that follows.
    const size_t avail = n - off - kRncHeaderBytes;
    Bits b{src, avail, 0};
    b.read(2);
    out.reserve(h.unpacked);
    const size_t limit = size_t(h.unpacked) + 0x10000;
    Table raw, dist, len;
    while (out.size() < h.unpacked) {
        if (!read_table(b, raw) || !read_table(b, dist) || !read_table(b, len)) { why = "malformed Huffman table"; return false; }
        int64_t count = b.read(16);
        for (;;) {
            uint32_t run = 0;
            if (!raw.empty() && !read_symbol(b, raw, run)) { why = "bad literal-run code"; return false; }
            if (run) {
                if (b.pos > avail || run > avail - b.pos || out.size() + run > limit) { why = "literal run past the end of the data"; return false; }
                out.insert(out.end(), src + b.pos, src + b.pos + run);
                b.pos += run;
            }
            if (--count <= 0) break;
            uint32_t dd = 0, ln = 0;
            if (!read_symbol(b, dist, dd) || !read_symbol(b, len, ln)) { why = "bad match code"; return false; }
            dd += 1;
            ln += 2;
            if (dd > out.size() || out.size() + ln > limit) { why = "match out of range"; return false; }
            for (uint32_t i = 0; i < ln; ++i) out.push_back(out[out.size() - dd]);
        }
        if (b.bad) { why = "the bit stream runs off the end of the data"; return false; }
    }
    out.resize(h.unpacked);
    if (rnc_crc16(out.data(), out.size()) != h.unpacked_crc) { why = "the CRC16 of the unpacked data does not match"; out.clear(); return false; }
    return true;
}

} // namespace x2::import
