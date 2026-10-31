// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A tiny RNC ProPack method 1 ENCODER for the tests (the inverse of rnc_unpack; all three Huffman tables are the flat 31-leaf,
// 5-bit code), so that synthetic disks can carry real, CRC-valid RNC blocks. Copied from pdos_tests.cpp, which keeps its own.
#pragma once
#include "import/rnc.h"

#include "container_fixtures.h"

#include <algorithm>
#include <vector>

namespace fx {

using x2::import::rnc_crc16;
using x2::import::kRncHeaderBytes;

struct RncWriter {
    Bytes out;
    size_t wpos = 0;
    int wbits = 16; // 16 = the current word is full: the next bit starts a word after whatever was appended meanwhile
    void bit(uint32_t b) {
        if (wbits == 16) {
            wpos = out.size();
            out.push_back(0);
            out.push_back(0);
            wbits = 0;
        }
        if (b) out[wpos + size_t(wbits >> 3)] = uint8_t(out[wpos + size_t(wbits >> 3)] | (1u << (wbits & 7)));
        ++wbits;
    }
    void bits(uint32_t v, int n) { for (int i = 0; i < n; ++i) bit((v >> i) & 1); }
    void code(uint32_t sym) { for (int i = 4; i >= 0; --i) bit((sym >> i) & 1); } // canonical code of leaf `sym`, MSB first
    void value(uint32_t v) {
        uint32_t s = v;
        if (v >= 2) { s = 0; for (uint32_t t = v; t; t >>= 1) ++s; }
        code(s);
        if (s >= 2) bits(v & ((1u << (s - 1)) - 1), int(s) - 1);
    }
    void table() {
        bits(31, 5);
        for (int i = 0; i < 31; ++i) bits(5, 4);
    }
};

struct Token { size_t lit_at = 0, lit_n = 0, dist = 0, len = 0; }; // literals, then a match (len 0: none)

inline Bytes rnc_pack(const Bytes& in, int chunks = 1) {
    RncWriter w;
    w.bits(0, 2);
    const size_t per = (in.size() + size_t(chunks) - 1) / size_t(chunks);
    for (int c = 0; c < chunks; ++c) {
        const size_t a = size_t(c) * per, b = std::min(in.size(), a + per);
        if (a >= b) break;
        std::vector<Token> toks;
        size_t i = a, lit = a;
        while (i < b) {
            size_t best = 0, best_d = 0;
            for (size_t d = 1; d <= std::min<size_t>(i, 300); ++d) {
                size_t n = 0;
                while (i + n < b && n < 700 && in[i + n] == in[i + n - d]) ++n;
                if (n > best) { best = n; best_d = d; }
            }
            if (best >= 3) {
                toks.push_back({lit, i - lit, best_d, best});
                i += best;
                lit = i;
            } else {
                ++i;
            }
        }
        toks.push_back({lit, b - lit, 0, 0});
        w.table();
        w.table();
        w.table();
        w.bits(uint32_t(toks.size()), 16);
        for (const Token& t : toks) {
            w.value(uint32_t(t.lit_n));
            w.out.insert(w.out.end(), in.begin() + long(t.lit_at), in.begin() + long(t.lit_at + t.lit_n));
            if (t.len) {
                w.value(uint32_t(t.dist - 1));
                w.value(uint32_t(t.len - 2));
            }
        }
    }
    Bytes r = {'R', 'N', 'C', 1};
    fx::put32(r, uint32_t(in.size()));
    fx::put32(r, uint32_t(w.out.size()));
    fx::put16(r, rnc_crc16(in.data(), in.size()));
    fx::put16(r, rnc_crc16(w.out.data(), w.out.size()));
    r.push_back(0);
    r.push_back(uint8_t(chunks));
    r.insert(r.end(), w.out.begin(), w.out.end());
    return r;
}


} // namespace fx
