// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Builds LHA archives from synthetic data for the importer tests (never game data): member headers of levels 0-3
// with stored members, and hand-made -lh4-..-lh7- streams (one block, fixed code lengths, no Huffman encoder needed).
#pragma once
#include "import/lha.h"

#include "container_fixtures.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lhafx {

using fx::Bytes;
using x2::import::lha_crc16;

inline void le16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
inline void le32(Bytes& b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }
inline void append(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
inline void append(Bytes& b, const Bytes& s) { b.insert(b.end(), s.begin(), s.end()); }

// ---------------------------------------------------------------------------------------------------------
// Archive builder

struct Member {
    std::string method = "-lh0-";
    std::string name = "FILE";
    std::string dir; // path with '/'
    Bytes packed, original;
    int level = 0;
    bool bad_crc = false;
    uint16_t crc_override = 0;
    bool use_override = false;
};

// An ext header: [size][type][data][next size]; `size` counts itself. The chain's first size field is the last two
// bytes of the base header (levels 1) or sits inside the header (levels 2, 3).
inline Bytes ext_chain(const std::vector<std::pair<uint8_t, std::string>>& exts, int field) {
    Bytes out;
    for (size_t i = 0; i < exts.size(); ++i) {
        // this header's own size field was written by the previous element; here: type + data + next size
        out.push_back(exts[i].first);
        append(out, exts[i].second);
        size_t next = i + 1 < exts.size() ? field + 1 + exts[i + 1].second.size() : 0;
        if (field == 2) le16(out, uint32_t(next)); else le32(out, uint32_t(next));
    }
    return out;
}

inline Bytes build_member(const Member& m) {
    uint16_t crc = m.use_override ? m.crc_override : lha_crc16(m.original.data(), m.original.size());
    if (m.bad_crc) crc ^= 0x1234;
    std::string path = m.name;
    Bytes h;
    if (m.level == 0 || m.level == 1) {
        std::string nm = m.level == 0 ? (m.dir.empty() ? m.name : m.dir + m.name) : m.name;
        for (char& c : nm) if (c == '/') c = '\\';
        std::vector<std::pair<uint8_t, std::string>> exts;
        if (m.level == 1 && !m.dir.empty()) {
            std::string d = m.dir;
            for (char& c : d) if (c == '/') c = char(0xFF);
            exts.push_back({0x02, d});
        }
        Bytes ext = m.level == 1 ? ext_chain(exts, 2) : Bytes();
        size_t ext_bytes = 0;
        for (auto& e : exts) ext_bytes += 2 + 1 + e.second.size();
        // header body from offset 2
        Bytes body;
        append(body, m.method);
        le32(body, uint32_t(m.packed.size() + (m.level == 1 ? ext_bytes : 0)));
        le32(body, uint32_t(m.original.size()));
        le16(body, 0x4000);
        le16(body, 0x2021);
        body.push_back(0x20);
        body.push_back(uint8_t(m.level));
        body.push_back(uint8_t(nm.size()));
        append(body, nm);
        le16(body, crc);
        if (m.level == 1) {
            body.push_back('A'); // OS type
            // the first ext size field
            size_t first = exts.empty() ? 0 : 2 + 1 + exts[0].second.size();
            le16(body, uint32_t(first));
        }
        h.push_back(uint8_t(body.size()));
        unsigned sum = 0;
        for (uint8_t c : body) sum += c;
        h.push_back(uint8_t(sum));
        append(h, body);
        if (m.level == 1) append(h, ext);
        // note: ext_chain wrote [type][data][next] for each header, matching the layout after the first size field
    } else {
        std::vector<std::pair<uint8_t, std::string>> exts;
        exts.push_back({0x01, m.name});
        if (!m.dir.empty()) {
            std::string d = m.dir;
            for (char& c : d) if (c == '/') c = char(0xFF);
            exts.push_back({0x02, d});
        }
        int field = m.level == 2 ? 2 : 4;
        Bytes chain = ext_chain(exts, field);
        size_t first = field + 1 + exts[0].second.size();
        Bytes body;
        append(body, m.method);
        le32(body, uint32_t(m.packed.size()));
        le32(body, uint32_t(m.original.size()));
        le32(body, 0x5F000000);
        body.push_back(0x20);
        body.push_back(uint8_t(m.level));
        le16(body, crc);
        body.push_back('A');
        if (m.level == 2) {
            size_t total = 2 + body.size() + 2 + chain.size();
            le16(h, uint32_t(total));
            append(h, body);
            le16(h, uint32_t(first));
            append(h, chain);
        } else {
            size_t total = 2 + body.size() + 4 + 4 + chain.size();
            le16(h, 4);
            append(h, body);
            le32(h, uint32_t(total));
            le32(h, uint32_t(first));
            append(h, chain);
        }
    }
    append(h, m.packed);
    return h;
}

inline Bytes archive(const std::vector<Member>& ms, bool terminator = true) {
    Bytes a;
    for (const Member& m : ms) append(a, build_member(m));
    if (terminator) a.push_back(0);
    return a;
}

inline Bytes text(size_t n, uint32_t seed) {
    fx::Rng r{seed};
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = uint8_t('a' + (r.next() >> 5) % 8);
    return b;
}

inline Member stored(const std::string& name, const Bytes& data, int level, const std::string& dir = "") {
    Member m;
    m.name = name;
    m.dir = dir;
    m.level = level;
    m.original = data;
    m.packed = data;
    return m;
}

// ---------------------------------------------------------------------------------------------------------
// -lh5- .. -lh7- hand-built streams: one block; literals 0..255 and the first match code with fixed lengths so the
// stream can be written without a Huffman encoder. Codes: symbol 256 (match, length 3) has length 1 (code 0); the
// 256 literals have length 9 (codes 1<<8 .. in symbol order); that is a complete code.

struct BitOut {
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

// Tokens: literal byte, or a match (length 3 only) with distance d >= 1. Offset table: a single symbol `obits`
// (code length 0), so every match costs (obits > 1 ? obits - 1 : 0) extra bits and has a fixed top bit pattern.
// `offs_cnt` > 0 writes an offset table of that many codes (all of length 5, a valid but unusual table) instead of the single
// symbol `obits`; the data then selects offset symbol `obits` from it (its 5-bit canonical code is the symbol number).
inline Bytes lh_new_stream(const std::vector<std::pair<int, int>>& tokens, int offset_bits, int obits, int offs_cnt = 0) {
    BitOut b;
    b.put(uint32_t(tokens.size()), 16); // block size in commands
    // temp table: 11 entries, only symbol 10 (a code length of 8 after the +2) ... but literals here are length 9
    // (symbol 11) and the match code length 1 (symbol 3): temp symbols 3 and 11 used, others 0.
    b.put(12, 5); // 12 temp entries (0..11)
    // lengths of temp symbols: sym3 = 1, sym11 = 1 (a one-bit code each), the rest 0
    for (int i = 0; i < 12; ++i) {
        int len = (i == 3 || i == 11) ? 1 : 0;
        if (len < 7) b.put(uint32_t(len), 3); else { b.put(7, 3); for (int k = 7; k < len; ++k) b.put(1, 1); b.put(0, 1); }
        if (i == 2) b.put(0, 2); // skip field after the third length
    }
    // code table: 257 entries (0..256): 256 literals length 9 -> temp symbol 11, symbol 256 length 1 -> temp symbol 3.
    // temp code: symbol 3 = '0', symbol 11 = '1' (canonical by symbol order, both length 1)
    b.put(257, 9);
    for (int i = 0; i < 256; ++i) b.put(1, 1);
    b.put(0, 1);
    // offset table: a single symbol
    if (offs_cnt > 0) {
        b.put(uint32_t(offs_cnt), offset_bits);
        for (int i = 0; i < offs_cnt; ++i) b.put(5, 3);
    } else {
        b.put(0, offset_bits);
        b.put(uint32_t(obits), offset_bits);
    }
    // data
    for (auto& t : tokens) {
        if (t.first >= 0) {
            b.put(1, 1);                // literal codes: 9 bits, first code of length 9 after the 1-bit code '0' is '1' + 8 bits
            b.put(uint32_t(t.first), 8);
        } else {
            b.put(0, 1); // match symbol 256
            int d = t.second - 1;       // stored distance minus one
            // the single offset symbol `obits` decides the bit length; extra low bits follow
            if (offs_cnt > 0) b.put(uint32_t(obits), 5);
            if (obits > 1) b.put(uint32_t(d - (1 << (obits - 1))), obits - 1);
        }
    }
    return b.finish();
}


} // namespace lhafx
