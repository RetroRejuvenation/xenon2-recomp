// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Tests of the decrunchers, the Power Pack rebuild and the importer's build / one-disk / cracked-retail recipes with
// SYNTHETIC data only (no game bytes). Every stream here is produced by a small encoder in this file and decoded by the
// library; the decoders themselves were checked byte for byte against the real stubs and the Python versions (docs/re-notes/
// cracks.md, single-disk-variants.md), so these tests cover bounds, damage and the importer logic around them.
//
//   x2_builds_tests WORKDIR
#include "import/crunch.h"
#include "import/importer.h"
#include "import/powerpack.h"
#include "import/retail.h"
#include "import/sha1.h"
#include "import/variants.h"

#include "container_fixtures.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace x2::import;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

bool has_message(const Result& r, Message::Level level, const std::string& needle) {
    for (const Message& m : r.messages)
        if (m.level == level && m.text.find(needle) != std::string::npos) return true;
    return false;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void remove_tree(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec); // the non-throwing overload: a Windows directory with an open file cannot be removed
}

Bytes reversed(const Bytes& b) { return Bytes(b.rbegin(), b.rend()); }

void put32(Bytes& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24);
    b[at + 1] = uint8_t(v >> 16);
    b[at + 2] = uint8_t(v >> 8);
    b[at + 3] = uint8_t(v);
}
uint32_t get32(const Bytes& b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}
void append32(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}

// --- stream writers -----------------------------------------------------------------------------------------------

// Longs in read order -> the buffer the decoders read from its end.
Bytes backwards_longs(const std::vector<uint32_t>& longs) {
    Bytes b(longs.size() * 4);
    for (size_t i = 0; i < longs.size(); ++i) put32(b, b.size() - 4 * (i + 1), longs[i]);
    return b;
}

// Bits (in the order the decoder consumes them) as longs, LSB first: the first long holds up to 31 bits and a sentinel one above
// them, each later long 32 bits.
std::vector<uint32_t> longs_lsb(const std::vector<uint8_t>& bits) {
    std::vector<uint32_t> out;
    size_t p = 0;
    uint32_t first = 0;
    unsigned k = 0;
    for (; k < 31 && p < bits.size(); ++k, ++p) first |= uint32_t(bits[p]) << k;
    out.push_back(first | (1u << k));
    while (p < bits.size()) {
        uint32_t v = 0;
        for (unsigned i = 0; i < 32 && p < bits.size(); ++i, ++p) v |= uint32_t(bits[p]) << i;
        out.push_back(v);
    }
    return out;
}
// The same, MSB first (the Power Pack's intro/main cruncher).
std::vector<uint32_t> longs_msb(const std::vector<uint8_t>& bits) {
    std::vector<uint32_t> out;
    size_t p = 0;
    uint32_t first = 0;
    unsigned k = 0;
    for (; k < 31 && p < bits.size(); ++k, ++p) first |= uint32_t(bits[p]) << (31 - k);
    out.push_back(first | (1u << (31 - k)));
    while (p < bits.size()) {
        uint32_t v = 0;
        for (unsigned i = 0; i < 32 && p < bits.size(); ++i, ++p) v |= uint32_t(bits[p]) << (31 - i);
        out.push_back(v);
    }
    return out;
}

// The token set shared by the Power Pack's two crunchers and BS1's (literal groups and byte copies); `S` is the output in
// decode order (the highest address first).
struct BitTokens {
    std::vector<uint8_t> bits;
    Bytes S;
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bits.push_back(uint8_t((v >> i) & 1));
    }
    void copy_bytes(size_t count, size_t off) { // a source before the start of the output (or the byte itself) reads as zero
        for (size_t i = 0; i < count; ++i) S.push_back(off >= 1 && S.size() >= off ? S[S.size() - off] : uint8_t(0));
    }
    Bytes expected() const { return reversed(S); }
};

// --- ONYX ---------------------------------------------------------------------------------------------------------

struct OnyxWriter {
    Bytes cons; // bytes in the order the decoder consumes them (highest address first)
    int slot = -1, n = 0, cap = 0;
    bool first = true;
    void bit(unsigned b) {
        if (slot < 0) {
            cons.push_back(0);
            slot = int(cons.size()) - 1;
            n = 0;
            cap = first ? 7 : 8;
            if (first) cons.back() = 0x01; // the sentinel at bit 0; data bits go to bits 1..7
            first = false;
        }
        int shift = cap == 7 ? n + 1 : n;
        cons[size_t(slot)] = uint8_t(cons[size_t(slot)] | ((b & 1u) << shift));
        if (++n == cap) slot = -1;
    }
    void bits(uint32_t v, int nb) {
        for (int k = nb - 1; k >= 0; --k) bit((v >> k) & 1u);
    }
    void raw(uint8_t b) { cons.push_back(b); }
    Bytes finish(const uint8_t params[4], size_t ulen) const {
        Bytes stream = reversed(cons);
        Bytes out(params, params + 4);
        append32(out, uint32_t(ulen));
        append32(out, uint32_t(stream.size()));
        out.insert(out.end(), stream.begin(), stream.end());
        return out;
    }
};

struct OnyxTok {
    enum Kind { Lit, Short, Long, Fill } kind;
    Bytes data;     // Lit
    uint32_t a = 0; // Short: offset; Long: length; Fill: count
    uint32_t b = 0; // Long: offset; Fill: byte
};

Bytes onyx_pack(const std::vector<OnyxTok>& tokens, Bytes& expected, const uint8_t params[4]) {
    OnyxWriter w;
    Bytes S;
    auto copy = [&](size_t dist, size_t count) {
        for (size_t i = 0; i < count; ++i) S.push_back(S.size() >= dist && dist >= 1 ? S[S.size() - dist] : uint8_t(0));
    };
    bool prev_lit = false;
    for (const OnyxTok& t : tokens) {
        if (t.kind == OnyxTok::Lit) {
            w.bit(1);
            if (t.data.size() == 1) w.bit(0);
            else { w.bit(1); w.bits(uint32_t(t.data.size() - 2), 2); }
            for (uint8_t b : t.data) { w.raw(b); S.push_back(b); }
            prev_lit = true;
            continue;
        }
        if (!prev_lit) w.bit(0); // match family
        if (t.kind == OnyxTok::Short) {
            w.bit(0);
            w.bits(t.a, 8);
            copy(t.a + 2, 2);
        } else if (t.kind == OnyxTok::Long) {
            w.bit(1);
            w.bits(t.a - 3, 2);
            w.bit(0);
            w.bits(t.b, 6);
            copy(t.b + t.a, t.a);
        } else { // Fill
            w.bit(1);
            w.bits(3, 2);
            w.bits(7, 3);
            w.bit(0);
            w.bit(0); // d3 = 1 in the default parameters: a selector bit selects the count width p3 and base $85
            w.bits(t.a - 0x85, params[0]);
            w.bits(t.b, 8);
            S.insert(S.end(), t.a, uint8_t(t.b));
        }
        prev_lit = false;
    }
    expected = reversed(S);
    return w.finish(params, expected.size());
}

const uint8_t kOnyxParams[4] = {9, 8, 13, 0x18};

// A block that decrunches to `size` bytes: random literal/copy pairs first, then zero-ish fills.
Bytes onyx_block(size_t size, uint32_t seed, Bytes& expected, size_t random_bytes = 400) {
    fx::Rng r{seed};
    std::vector<OnyxTok> toks;
    size_t have = 0;
    while (have < random_bytes) {
        OnyxTok l{OnyxTok::Lit, {}, 0, 0};
        size_t n = have == 0 ? 4 : 1 + r.next() % 4; // the first literal run is long enough for the copy after it
        for (size_t i = 0; i < n; ++i) l.data.push_back(uint8_t(r.next() >> 24));
        toks.push_back(l);
        have += n;
        uint32_t length = 3 + r.next() % 3;
        if (length > have) length = 3;
        uint32_t off = uint32_t(r.next() % 8);
        if (off + length > have) off = 0; // the ONYX decoder rejects a copy from before the start of the output
        toks.push_back({OnyxTok::Long, {}, length, off});
        have += length;
    }
    size_t remaining = size - have;
    while (remaining > 644) {
        size_t take = remaining > 644 + 133 ? 644 : remaining - 133;
        toks.push_back({OnyxTok::Fill, {}, uint32_t(take), uint32_t(r.next() >> 24)});
        remaining -= take;
    }
    toks.push_back({OnyxTok::Fill, {}, uint32_t(remaining), uint32_t(r.next() >> 24)});
    return onyx_pack(toks, expected, kOnyxParams);
}

// --- Ice-style ----------------------------------------------------------------------------------------------------

// Literals and 2-3 byte matches. Returns the block as the importer's executable holds it: ulen, plen, then the stream.
Bytes ice_block(const Bytes& decode_order_literals, Bytes& expected, size_t matches, uint32_t seed) {
    std::vector<uint8_t> bits;
    Bytes S;
    auto emit = [&](uint32_t v, int n) {
        for (int k = 0; k < n; ++k) bits.push_back(uint8_t((v >> k) & 1));
    };
    fx::Rng r{seed};
    size_t li = 0;
    for (size_t m = 0; m <= matches; ++m) {
        for (int i = 0; i < 12 && li < decode_order_literals.size(); ++i, ++li) {
            emit(1, 1);
            emit(decode_order_literals[li], 8);
            S.push_back(decode_order_literals[li]);
        }
        if (m == matches) break;
        uint32_t length = 2 + r.next() % 2, off = 1 + r.next() % 11;
        if (S.size() < off) continue;
        emit(0, 1);
        emit(0, 1);
        emit(length - 2, 1);
        emit(1, 1);
        emit(0, 1);
        emit(off, 5);
        for (uint32_t i = 0; i < length; ++i) S.push_back(S[S.size() - off]);
    }
    while (li < decode_order_literals.size()) {
        emit(1, 1);
        emit(decode_order_literals[li], 8);
        S.push_back(decode_order_literals[li++]);
    }
    while (bits.size() % 16) bits.push_back(0);
    std::vector<uint32_t> words;
    for (size_t i = 0; i < bits.size(); i += 16) {
        uint32_t w = 0;
        for (int k = 0; k < 16; ++k) w |= uint32_t(bits[i + size_t(k)]) << k;
        words.push_back(w);
    }
    Bytes body;
    for (size_t i = words.size(); i-- > 0;) {
        body.push_back(uint8_t(words[i] >> 8));
        body.push_back(uint8_t(words[i]));
    }
    body.push_back(0);
    body.push_back(16);
    expected = reversed(S);
    Bytes out;
    append32(out, uint32_t(expected.size()));
    append32(out, uint32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// --- BS1 ByteKiller flavour ---------------------------------------------------------------------------------------

// A stream of literal groups (header = packed length, unpacked length, checksum).
Bytes bs1_block(const Bytes& image_decode_order, Bytes& expected) {
    BitTokens w;
    size_t i = 0;
    while (i < image_decode_order.size()) {
        size_t n = std::min<size_t>(8, image_decode_order.size() - i);
        w.put(0, 1);
        w.put(0, 1);
        w.put(uint32_t(n - 1), 3);
        for (size_t k = 0; k < n; ++k) { w.put(image_decode_order[i + k], 8); w.S.push_back(image_decode_order[i + k]); }
        i += n;
    }
    expected = w.expected();
    std::vector<uint32_t> longs = longs_lsb(w.bits);
    uint32_t crc = 0;
    for (uint32_t v : longs) crc ^= v;
    Bytes stream = backwards_longs(longs);
    Bytes out;
    append32(out, uint32_t(stream.size()));
    append32(out, uint32_t(expected.size()));
    append32(out, crc);
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

// All the BS1 token kinds: a long literal run, copies of 3-4 bytes (zero beyond the start), a 2-byte copy and a 12-bit-offset copy.
Bytes bs1_block_all_kinds(Bytes& expected) {
    BitTokens w;
    fx::Rng r{5};
    auto lit = [&](size_t n) {
        w.put(0, 1);
        w.put(0, 1);
        w.put(uint32_t(n - 1), 3);
        for (size_t k = 0; k < n; ++k) { uint8_t b = uint8_t(r.next() >> 24); w.put(b, 8); w.S.push_back(b); }
    };
    auto lit_long = [&](size_t n) { // 9..264 literals
        w.put(1, 1);
        w.put(3, 2);
        w.put(uint32_t(n - 9), 8);
        for (size_t k = 0; k < n; ++k) { uint8_t b = uint8_t(r.next() >> 24); w.put(b, 8); w.S.push_back(b); }
    };
    w.put(1, 1); // a copy before anything is written: reads zeros
    w.put(0, 2); // n = 0: 9-bit offset, 3 bytes
    w.put(3, 9);
    w.copy_bytes(3, 3);
    lit(5);
    lit_long(40);
    w.put(0, 1); // 2-byte copy, 8-bit offset
    w.put(1, 1);
    w.put(7, 8);
    w.copy_bytes(2, 7);
    w.put(1, 1); // n = 1: 10-bit offset, 4 bytes
    w.put(1, 2);
    w.put(300, 10);
    w.copy_bytes(4, 300);
    lit(8);
    w.put(1, 1); // n = 2: (count + 1) bytes, 12-bit offset
    w.put(2, 2);
    w.put(9, 8);
    w.put(33, 12);
    w.copy_bytes(10, 33);
    lit(1);
    expected = w.expected();
    std::vector<uint32_t> longs = longs_lsb(w.bits);
    uint32_t crc = 0;
    for (uint32_t v : longs) crc ^= v;
    Bytes stream = backwards_longs(longs);
    Bytes out;
    append32(out, uint32_t(stream.size()));
    append32(out, uint32_t(expected.size()));
    append32(out, crc);
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

// --- the Power Pack's crunchers -----------------------------------------------------------------------------------

struct PpTokens : BitTokens {
    void lit(const Bytes& b) { // 1..8 literals, in decode order
        put(0, 1);
        put(0, 1);
        put(uint32_t(b.size() - 1), 3);
        for (uint8_t v : b) { put(v, 8); S.push_back(v); }
    }
    void lit_long(const Bytes& b) { // 9..264
        put(1, 1);
        put(3, 2);
        put(uint32_t(b.size() - 9), 8);
        for (uint8_t v : b) { put(v, 8); S.push_back(v); }
    }
    void copy2(uint32_t off) {
        put(0, 1);
        put(1, 1);
        put(off, 8);
        copy_bytes(2, off);
    }
    void copy_a(uint32_t sel, uint32_t off) { // sel + 3 bytes
        put(1, 1);
        put(sel, 2);
        put(off, 8 + int(sel));
        copy_bytes(sel + 3, off);
    }
    void copy_b(uint32_t n, uint32_t off) { // n + 1 bytes
        put(1, 1);
        put(2, 2);
        put(n, 8);
        put(off, 8);
        copy_bytes(n + 1, off);
    }
};

// A literal-only stream of `decode_order` (the output reversed), as MSB- or LSB-first longs, stored backwards.
Bytes pp_literals(const Bytes& output, bool msb) {
    PpTokens t;
    Bytes decode = reversed(output);
    size_t i = 0;
    while (i < decode.size()) {
        size_t n = std::min<size_t>(i + 8 <= decode.size() ? 8 : decode.size() - i, 8);
        t.lit(Bytes(decode.begin() + long(i), decode.begin() + long(i + n)));
        i += n;
    }
    return backwards_longs(msb ? longs_msb(t.bits) : longs_lsb(t.bits));
}

// --- the tests ----------------------------------------------------------------------------------------------------

void test_onyx() {
    Bytes expected;
    Bytes blob = onyx_block(3000, 11, expected);
    Bytes out;
    std::string why;
    CHECK(onyx_decrunch(blob.data(), blob.size(), 0, out, why) && out == expected);
    // at an offset inside a larger file
    Bytes padded(7, 0xAA);
    padded.insert(padded.end(), blob.begin(), blob.end());
    CHECK(onyx_decrunch(padded.data(), padded.size(), 7, out, why) && out == expected);
    // a fill of the longest and shortest counts
    Bytes e2;
    std::vector<OnyxTok> toks = {{OnyxTok::Lit, {0x11, 0x22}, 0, 0}, {OnyxTok::Fill, {}, 0x90, 0x5A}, {OnyxTok::Lit, {0x7F}, 0, 0}, {OnyxTok::Short, {}, 3, 0},
                                 {OnyxTok::Fill, {}, 0x284, 0x00}, {OnyxTok::Fill, {}, 0x85, 0xFF}};
    Bytes b2 = onyx_pack(toks, e2, kOnyxParams);
    CHECK(onyx_decrunch(b2.data(), b2.size(), 0, out, why) && out == e2);
    {
        Bytes run(0x90, 0x5A);
        CHECK(std::search(e2.begin(), e2.end(), run.begin(), run.end()) != e2.end());
    }
    // damage: a block that does not fit, a truncated file, an absurd unpacked length, a too-short header
    CHECK(!onyx_decrunch(blob.data(), 20, 0, out, why));
    Bytes bad = blob;
    put32(bad, 4, 0x7FFFFFFF);
    CHECK(!onyx_decrunch(bad.data(), bad.size(), 0, out, why));
    CHECK(!onyx_decrunch(blob.data(), 8, 0, out, why) && !onyx_decrunch(blob.data(), blob.size(), blob.size() + 1, out, why));
    bad = blob;
    bad[0] = 99; // a field width beyond what any real stream uses
    CHECK(!onyx_decrunch(bad.data(), bad.size(), 0, out, why));
    // an unpacked length that is too short for the stream's tokens: the output is exceeded, not overrun
    bad = blob;
    put32(bad, 4, 100);
    CHECK(!onyx_decrunch(bad.data(), bad.size(), 0, out, why) || out.size() == 100); // the stream is simply not read to its end
    // random bytes never escape their buffers (run under ASan/UBSan in the sanitizer jobs)
    fx::Rng r{7};
    for (int i = 0; i < 400; ++i) {
        Bytes junk(16 + r.next() % 200);
        for (uint8_t& b : junk) b = uint8_t(r.next());
        if (i % 2) { put32(junk, 4, r.next() % 600); put32(junk, 8, uint32_t(junk.size() - 12)); }
        junk[0] = uint8_t(r.next() % 25); junk[1] = uint8_t(r.next() % 25); junk[2] = uint8_t(r.next() % 25);
        bool ok = onyx_decrunch(junk.data(), junk.size(), 0, out, why);
        CHECK(!ok || out.size() == get32(junk, 4));
    }
    // mutations of a valid block
    for (int i = 0; i < 400; ++i) {
        Bytes m = blob;
        for (int k = 0; k < 3; ++k) m[r.next() % m.size()] ^= uint8_t(1u << (r.next() % 8));
        bool ok = onyx_decrunch(m.data(), m.size(), 0, out, why);
        CHECK(!ok || out.size() == get32(m, 4));
    }
}

void test_ice() {
    fx::Rng r{2};
    Bytes lits(300);
    for (uint8_t& b : lits) b = uint8_t(r.next());
    Bytes expected;
    Bytes blob = ice_block(lits, expected, 30, 3);
    Bytes out;
    std::string why;
    CHECK(ice_depack(blob.data(), blob.size(), 8, get32(blob, 0), get32(blob, 4), out, why) && out == expected);
    CHECK(!ice_depack(blob.data(), blob.size(), 8, get32(blob, 0), get32(blob, 4) + 100, out, why)); // packed length beyond the file
    CHECK(!ice_depack(blob.data(), blob.size(), 8, 0x7FFFFFFF, get32(blob, 4), out, why));
    CHECK(!ice_depack(blob.data(), blob.size(), 8, get32(blob, 0), 4, out, why));
    CHECK(!ice_depack(blob.data(), blob.size(), blob.size() + 1, 10, 8, out, why));
    CHECK(!ice_depack(blob.data(), blob.size(), 8, 10, get32(blob, 4), out, why) || out.size() == 10); // an output too short for the stream
    for (int i = 0; i < 400; ++i) {
        Bytes m = blob;
        for (int k = 0; k < 3; ++k) m[8 + r.next() % (m.size() - 8)] ^= uint8_t(1u << (r.next() % 8));
        bool ok = ice_depack(m.data(), m.size(), 8, get32(m, 0), get32(m, 4), out, why);
        CHECK(!ok || out.size() == get32(m, 0));
        Bytes junk(16 + r.next() % 120);
        for (uint8_t& b : junk) b = uint8_t(r.next());
        ok = ice_depack(junk.data(), junk.size(), 0, r.next() % 500, junk.size(), out, why);
        (void)ok;
    }
}

void test_bs1() {
    fx::Rng r{9};
    Bytes image(1000);
    for (uint8_t& b : image) b = uint8_t(r.next());
    Bytes expected;
    Bytes blob = bs1_block(reversed(image), expected);
    CHECK(expected == image);
    Bytes out;
    std::string why;
    CHECK(bs1_depack(blob.data(), blob.size(), 0, out, why) && out == image);
    // at an offset
    Bytes padded(0xE8, 0x55);
    padded.insert(padded.end(), blob.begin(), blob.end());
    CHECK(bs1_depack(padded.data(), padded.size(), 0xE8, out, why) && out == image);
    // every token kind
    Bytes e2;
    Bytes b2 = bs1_block_all_kinds(e2);
    CHECK(bs1_depack(b2.data(), b2.size(), 0, out, why) && out == e2);
    // a wrong checksum, a truncated stream, a length beyond the file, a stream that is longer than its output wants
    Bytes bad = blob;
    put32(bad, 8, get32(bad, 8) ^ 1);
    CHECK(!bs1_depack(bad.data(), bad.size(), 0, out, why) && why.find("checksum") != std::string::npos);
    CHECK(!bs1_depack(blob.data(), 40, 0, out, why));
    bad = blob;
    put32(bad, 0, 0x7FFFFFF0);
    CHECK(!bs1_depack(bad.data(), bad.size(), 0, out, why));
    CHECK(!bs1_depack(blob.data(), 8, 0, out, why));
    bad = blob;
    put32(bad, 4, 100);
    CHECK(!bs1_depack(bad.data(), bad.size(), 0, out, why));
    for (int i = 0; i < 400; ++i) {
        Bytes m = blob;
        for (int k = 0; k < 3; ++k) m[12 + r.next() % (m.size() - 12)] ^= uint8_t(1u << (r.next() % 8));
        bool ok = bs1_depack(m.data(), m.size(), 0, out, why);
        CHECK(!ok || out.size() == get32(m, 4));
        Bytes junk(16 + r.next() % 120);
        for (uint8_t& b : junk) b = uint8_t(r.next());
        put32(junk, 0, uint32_t(junk.size() - 12));
        put32(junk, 4, r.next() % 400);
        ok = bs1_depack(junk.data(), junk.size(), 0, out, why);
        CHECK(!ok || out.size() == get32(junk, 4));
    }
}

void test_powerpack_crunchers() {
    fx::Rng r{3};
    Bytes output(2000);
    for (uint8_t& b : output) b = uint8_t(r.next());
    for (bool msb : {true, false}) {
        Bytes stream = pp_literals(output, msb);
        Bytes out;
        bool consumed = false;
        std::string why;
        bool ok = msb ? powerpack_decrunch_msb(stream.data(), stream.size(), output.size(), out, consumed, why)
                      : powerpack_decrunch_lsb(stream.data(), stream.size(), output.size(), out, consumed, why);
        CHECK(ok && consumed && out == output);
        // a stream with an unread long in front is not "consumed exactly"
        Bytes padded(4, 0);
        padded.insert(padded.end(), stream.begin(), stream.end());
        ok = msb ? powerpack_decrunch_msb(padded.data(), padded.size(), output.size(), out, consumed, why)
                 : powerpack_decrunch_lsb(padded.data(), padded.size(), output.size(), out, consumed, why);
        CHECK(ok && !consumed && out == output);
        // truncated or damaged
        Bytes cut(stream.begin() + 64, stream.end());
        ok = msb ? powerpack_decrunch_msb(cut.data(), cut.size(), output.size(), out, consumed, why)
                 : powerpack_decrunch_lsb(cut.data(), cut.size(), output.size(), out, consumed, why);
        CHECK(!ok || out != output);
        CHECK(!(msb ? powerpack_decrunch_msb(stream.data(), stream.size(), 0x7FFFFFFF, out, consumed, why)
                    : powerpack_decrunch_lsb(stream.data(), stream.size(), 0x7FFFFFFF, out, consumed, why)));
        CHECK(!(msb ? powerpack_decrunch_msb(stream.data(), 0, 10, out, consumed, why) : powerpack_decrunch_lsb(stream.data(), 2, 10, out, consumed, why)));
        for (int i = 0; i < 300; ++i) {
            Bytes m = stream;
            for (int k = 0; k < 3; ++k) m[r.next() % m.size()] ^= uint8_t(1u << (r.next() % 8));
            ok = msb ? powerpack_decrunch_msb(m.data(), m.size(), output.size(), out, consumed, why)
                     : powerpack_decrunch_lsb(m.data(), m.size(), output.size(), out, consumed, why);
            CHECK(!ok || out.size() == output.size());
        }
    }
    // every token kind, both bit orders
    for (bool msb : {true, false}) {
        PpTokens t;
        t.copy_a(0, 4); // before anything is written: zeros
        t.lit(Bytes{1, 2, 3, 4, 5});
        t.lit_long(Bytes(20, 0x66));
        t.copy2(3);
        t.copy_a(1, 200);
        t.lit(Bytes{9});
        t.copy_b(9, 11);
        Bytes expected = t.expected(), stream = backwards_longs(msb ? longs_msb(t.bits) : longs_lsb(t.bits)), out;
        bool consumed = false;
        std::string why;
        bool ok = msb ? powerpack_decrunch_msb(stream.data(), stream.size(), expected.size(), out, consumed, why)
                      : powerpack_decrunch_lsb(stream.data(), stream.size(), expected.size(), out, consumed, why);
        CHECK(ok && consumed && out == expected);
    }
    // the RLE stage: marker, value, count (0 = 256) and literals, read from the end
    Bytes src = {1, 2, 3, 30, 0x55, 0xFF, 4, 0, 0x77, 0xFF};
    Bytes out;
    size_t left = 1;
    std::string why;
    CHECK(powerpack_unrle(src.data(), src.size(), 0xFF, 4 + 30 + 256, out, left, why) && left == 0);
    CHECK(out.size() == 290 && out[0] == 1 && out[2] == 3 && out[3] == 0x55 && out[32] == 0x55 && out[33] == 4 && out[34] == 0x77 && out[289] == 0x77);
    CHECK(!powerpack_unrle(src.data(), src.size(), 0xFF, 10, out, left, why));        // a run leaves the output
    CHECK(!powerpack_unrle(src.data(), 2, 1, 10, out, left, why) || left != 0);
    Bytes dangling = {5, 0xFF};
    CHECK(!powerpack_unrle(dangling.data(), dangling.size(), 0xFF, 10, out, left, why)); // a marker without value and count
    CHECK(powerpack_unrle(src.data(), 3, 0xFF, 10, out, left, why) && left == 7);          // short data: the rest is not filled
}

// --- a custom data track ------------------------------------------------------------------------------------------

constexpr uint32_t kM55 = 0x55555555u;

Track speedlock_track(const Bytes& data, bool break_checksum = false, bool with_sync = true) {
    std::vector<uint8_t> bits;
    auto put = [&](uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bits.push_back(uint8_t((v >> i) & 1));
    };
    fx::Rng r{uint32_t(data.size()) * 31u + data[0]};
    for (int i = 0; i < 200; ++i) bits.push_back(uint8_t(r.next() & 1u) & 0); // leading cells without a sync pattern
    if (with_sync) put(0x4489, 16);
    else put(0x5555, 16);
    put(0xAAAA5555u, 32); // the four raw bytes after the sync word (skipped)
    const size_t nl = data.size() / 4;
    std::vector<uint32_t> out(nl);
    uint32_t sum = 0;
    for (size_t k = 0; k < nl; ++k) {
        out[k] = get32(data, 4 * k);
        sum += out[k];
    }
    if (break_checksum) sum ^= 1;
    // the even block carries the low bit of every pair, the odd block the high bit; so does each checksum
    for (size_t k = 0; k < nl; ++k) put(out[k] & kM55, 32);
    put(sum & kM55, 32);
    for (size_t k = 0; k < nl; ++k) put((out[k] >> 1) & kM55, 32);
    put((sum >> 1) & kM55, 32);
    const uint32_t n = uint32_t(bits.size()) + 100;
    Track t;
    t.kind = Track::Kind::Raw;
    t.bit_length = n;
    t.mfm.assign((n + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) t.mfm[i >> 3] = uint8_t(t.mfm[i >> 3] | (0x80u >> (i & 7)));
    return t;
}

void test_speedlock_tracks() {
    fx::Rng r{4};
    Bytes data(kPowerPackTrackBytes);
    for (uint8_t& b : data) b = uint8_t(r.next());
    Bytes got(kPowerPackTrackBytes);
    std::string why;
    CHECK(speedlock_decode_track(speedlock_track(data), got.data(), why) && got == data);
    CHECK(!speedlock_decode_track(speedlock_track(data, true), got.data(), why) && why.find("checksum") != std::string::npos);
    CHECK(!speedlock_decode_track(speedlock_track(data, false, false), got.data(), why) && why.find("sync") != std::string::npos);
    Track decoded;
    decoded.kind = Track::Kind::Decoded;
    CHECK(!speedlock_decode_track(decoded, got.data(), why));
    Track empty;
    CHECK(!speedlock_decode_track(empty, got.data(), why));
    Track shortt = speedlock_track(data);
    shortt.bit_length = 60;
    CHECK(!speedlock_decode_track(shortt, got.data(), why)); // a track far shorter than the block is a checksum failure, never a read past the end
    Track lying = speedlock_track(data);
    lying.mfm.resize(10);
    CHECK(!speedlock_decode_track(lying, got.data(), why)); // bit_length beyond the bytes it has
    // powerpack_data_from_image: all 157 data tracks, or the first that fails
    DiskImage img;
    Bytes all;
    for (int i = 0; i < kPowerPackDataTracks; ++i) {
        Bytes d(kPowerPackTrackBytes);
        for (uint8_t& b : d) b = uint8_t(r.next());
        all.insert(all.end(), d.begin(), d.end());
        img.track(kPowerPackFirstDataTrack + i) = speedlock_track(d);
    }
    Bytes blob;
    CHECK(powerpack_data_from_image(img, blob, why) && blob == all);
    img.track(60) = speedlock_track(Bytes(kPowerPackTrackBytes, 7), true);
    CHECK(!powerpack_data_from_image(img, blob, why) && why.find("track 60") != std::string::npos);
}

// --- a synthetic Power Pack disk ----------------------------------------------------------------------------------

struct PpFixture {
    Bytes data = Bytes(kPowerPackDataSize, 0);
    Bytes disc2 = Bytes(901120, 0);
    Bytes game = Bytes(0x3FC00, 0);
    PowerPackLayout layout{};
    std::vector<Bytes> payloads;
};

PpFixture make_power_pack(uint32_t seed, int corrupt_file = -1, bool pad_main = false, int files = 6) {
    PpFixture f;
    fx::Rng r{seed};
    f.layout.main_track = 10;
    f.layout.main_len = 0x1000;
    f.layout.main_base = 0x40000 - 0x1000;
    f.layout.file_table = f.layout.main_base + 0x100;
    f.layout.addr_table = f.layout.main_base + 0x200;
    // the main image: random, with the file table and the crunch parameters in place
    Bytes main_image(f.layout.main_len);
    for (uint8_t& b : main_image) b = uint8_t(r.next());
    const uint16_t fids[6] = {11, 250, 514, 799, 1154, 1476};
    int track = 20;
    for (int i = 0; i < files; ++i) {
        // the file: random bytes below 0x80 with one run (value 0x55, 30 times) that the RLE stage restores
        Bytes payload(500 + size_t(r.next() % 300));
        for (uint8_t& b : payload) b = uint8_t(r.next() % 0x80);
        for (int k = 0; k < 30; ++k) payload[100 + size_t(k)] = 0x55;
        Bytes stage1(payload.begin(), payload.begin() + 100);
        stage1.push_back(30);
        stage1.push_back(0x55);
        stage1.push_back(0xFF); // marker
        stage1.insert(stage1.end(), payload.begin() + 130, payload.end());
        Bytes stream = pp_literals(stage1, false);
        if (i == corrupt_file) stream[stream.size() / 2] ^= 0x10;
        const size_t ct = (stream.size() + kPowerPackTrackBytes - 1) / kPowerPackTrackBytes;
        std::memcpy(&f.data[size_t(track - kPowerPackFirstDataTrack) * kPowerPackTrackBytes], stream.data(), stream.size());
        const size_t t = size_t(f.layout.file_table - f.layout.main_base) + 4 * size_t(i), a = size_t(f.layout.addr_table - f.layout.main_base) + 16 * size_t(i);
        main_image[t] = uint8_t(fids[i] >> 8);
        main_image[t + 1] = uint8_t(fids[i]);
        main_image[t + 2] = uint8_t(track);
        main_image[t + 3] = uint8_t(ct);
        put32(main_image, a, uint32_t(stream.size()));
        put32(main_image, a + 4, uint32_t(stage1.size()));
        put32(main_image, a + 8, 0xFF000000u | uint32_t(stage1.size()));
        put32(main_image, a + 12, uint32_t(payload.size()));
        std::memcpy(&f.disc2[size_t(fids[i]) * 512], payload.data(), payload.size());
        f.payloads.push_back(payload);
        track += 6;
    }
    // the terminator
    const size_t t = size_t(f.layout.file_table - f.layout.main_base) + 4 * size_t(files);
    main_image[t] = 0xFF;
    main_image[t + 1] = 0x00;
    main_image[t + 2] = 0;
    main_image[t + 3] = 0;
    Bytes stream = pp_literals(main_image, true);
    if (pad_main) stream.insert(stream.begin(), 4, 0);
    f.layout.main_packed = stream.size();
    std::memcpy(&f.data[size_t(f.layout.main_track - kPowerPackFirstDataTrack) * kPowerPackTrackBytes], stream.data(), stream.size());
    f.disc2[0] = f.disc2[1] = 0;
    std::memcpy(&f.disc2[2], "X2B ", 4);
    const uint8_t stub[4] = {0x60, 0x00, 0x6C, 0x30};
    std::memcpy(f.game.data(), stub, 4);
    std::memcpy(&f.game[f.layout.main_base - 0x400], main_image.data(), main_image.size());
    return f;
}

void test_powerpack_build() {
    PpFixture f = make_power_pack(1);
    PowerPackBuild b;
    std::string why;
    CHECK(powerpack_build(f.data, b, why, f.layout));
    CHECK(b.files == 6 && b.game_image == f.game && b.disc2 == f.disc2);
    // the real layout does not match a synthetic disk: refused, never read out of range
    CHECK(!powerpack_build(f.data, b, why));
    // damage: a flipped bit in a game file's stream, an unread long before the main image, fewer than six files, a short blob
    PpFixture bad = make_power_pack(2, 3);
    CHECK(!powerpack_build(bad.data, b, why, bad.layout) || b.disc2 != bad.disc2); // a literal-only stream has no checksum: a flipped data bit shows in the data
    PpFixture padded = make_power_pack(3, -1, true);
    CHECK(!powerpack_build(padded.data, b, why, padded.layout) && why.find("not used exactly") != std::string::npos);
    PpFixture five = make_power_pack(4, -1, false, 5);
    CHECK(!powerpack_build(five.data, b, why, five.layout) && why.find("six") != std::string::npos);
    Bytes shortdata(f.data.begin(), f.data.end() - 1);
    CHECK(!powerpack_build(shortdata, b, why, f.layout));
    PowerPackLayout wrong = f.layout;
    wrong.main_base += 4;
    CHECK(!powerpack_build(f.data, b, why, wrong));
    wrong = f.layout;
    wrong.main_packed = f.data.size();
    CHECK(!powerpack_build(f.data, b, why, wrong));
    // random data tracks never escape
    fx::Rng r{8};
    for (int i = 0; i < 20; ++i) {
        Bytes junk(kPowerPackDataSize);
        for (uint8_t& v : junk) v = uint8_t(r.next());
        CHECK(!powerpack_build(junk, b, why, kPowerPackLayout));
    }
}

// --- importer: disks built here -----------------------------------------------------------------------------------

constexpr size_t kAdf = 901120;

struct Ofs {
    Bytes d = Bytes(kAdf, 0);
    uint32_t next = 882;
    Ofs() {
        std::memcpy(d.data(), "DOS\0", 4);
        put32(d, 8, 880);
        size_t r = 880 * 512;
        put32(d, r, 2);
        put32(d, r + 12, 72);
        put32(d, r + 508, 1);
        name(880, "SYNTHETIC");
    }
    void name(uint32_t blk, const std::string& n) {
        d[blk * 512 + 432] = uint8_t(n.size());
        std::memcpy(&d[blk * 512 + 433], n.data(), n.size());
    }
    void checksum(uint32_t blk) {
        put32(d, blk * 512 + 20, 0);
        uint32_t sum = 0;
        for (int i = 0; i < 128; ++i) sum += get32(d, blk * 512 + size_t(i) * 4);
        put32(d, blk * 512 + 20, uint32_t(0) - sum);
    }
    uint32_t alloc() { return next++; }
    static uint32_t hash(const std::string& n) {
        uint32_t h = uint32_t(n.size());
        for (char c : n) h = (h * 13 + uint32_t(std::toupper(static_cast<unsigned char>(c)))) & 0x7FF;
        return h % 72;
    }
    void add_file(const std::string& fname, const Bytes& data) {
        uint32_t hdr = alloc();
        size_t slot = 880 * 512 + 24 + 4 * hash(fname);
        put32(d, hdr * 512 + 496, get32(d, slot));
        put32(d, slot, hdr);
        checksum(880);
        size_t nblocks = (data.size() + 487) / 488;
        std::vector<uint32_t> blocks;
        for (size_t i = 0; i < nblocks; ++i) blocks.push_back(alloc());
        for (size_t i = 0; i < nblocks; ++i) {
            size_t b = blocks[i] * 512, len = std::min<size_t>(488, data.size() - i * 488);
            put32(d, b, 8);
            put32(d, b + 4, hdr);
            put32(d, b + 8, uint32_t(i + 1));
            put32(d, b + 12, uint32_t(len));
            put32(d, b + 16, i + 1 < nblocks ? blocks[i + 1] : 0);
            std::memcpy(&d[b + 24], &data[i * 488], len);
            checksum(blocks[i]);
        }
        uint32_t tbl = hdr;
        for (size_t first = 0; first < nblocks || tbl == hdr; first += 72) {
            size_t t = tbl * 512, n = std::min<size_t>(72, nblocks - first);
            put32(d, t, tbl == hdr ? 2 : 16);
            put32(d, t + 4, tbl);
            put32(d, t + 8, uint32_t(n));
            if (n) put32(d, t + 16, blocks[first]);
            for (size_t i = 0; i < n; ++i) put32(d, t + 308 - 4 * i, blocks[first + i]);
            put32(d, t + 500, tbl == hdr ? 880 : hdr);
            put32(d, t + 508, 0xFFFFFFFD);
            uint32_t ext = first + 72 < nblocks ? alloc() : 0;
            put32(d, t + 504, ext);
            if (tbl == hdr) { put32(d, t + 324, uint32_t(data.size())); name(hdr, fname); }
            checksum(tbl);
            if (!ext) break;
            tbl = ext;
        }
    }
};

// A hunk executable: one CODE hunk per entry of `codes`; hunk 0 also carries a RELOC32 block, the last a symbol hunk and a debug hunk.
Bytes hunk_exe(const std::vector<Bytes>& codes) {
    Bytes e;
    auto w32 = [&](uint32_t v) { append32(e, v); };
    w32(0x3F3);
    w32(0);
    w32(uint32_t(codes.size()));
    w32(0);
    w32(uint32_t(codes.size() - 1));
    std::vector<Bytes> padded = codes;
    for (Bytes& c : padded)
        while (c.size() % 4) c.push_back(0);
    for (const Bytes& c : padded) w32(uint32_t(c.size() / 4));
    for (size_t i = 0; i < padded.size(); ++i) {
        w32(0x3E9);
        w32(uint32_t(padded[i].size() / 4));
        e.insert(e.end(), padded[i].begin(), padded[i].end());
        if (i == 0) { // HUNK_RELOC32: one offset into hunk 0
            w32(0x3EC);
            w32(1);
            w32(0);
            w32(0);
            w32(0);
        }
        if (i + 1 == padded.size()) {
            w32(0x3F0); // HUNK_SYMBOL: one symbol
            w32(1);
            w32(0x41424344);
            w32(0);
            w32(0);
            w32(0x3F1); // HUNK_DEBUG
            w32(1);
            w32(0xDEADBEEF);
        }
        w32(0x3F2);
    }
    return e;
}

Bytes disc2_adf(uint32_t seed) {
    Bytes adf(kAdf, 0);
    fx::Rng r{seed};
    for (uint8_t& b : adf) b = uint8_t(r.next());
    std::memcpy(&adf[2], "X2B ", 4);
    return adf;
}

Input input(const std::string& name, const Bytes& data) { return Input{name, data, false}; }

void test_hunk_data() {
    Bytes a(40, 0xA1), b(24, 0xB2), c(60, 0xC3);
    Bytes exe = hunk_exe({a, b, c});
    const uint8_t* p = nullptr;
    size_t n = 0;
    CHECK(hunk_data(exe.data(), exe.size(), 0, p, n) && n == 40 && p[0] == 0xA1);
    CHECK(hunk_data(exe.data(), exe.size(), 1, p, n) && n == 24 && p[0] == 0xB2);
    CHECK(hunk_data(exe.data(), exe.size(), 2, p, n) && n == 60 && p[59] == 0xC3);
    CHECK(!hunk_data(exe.data(), exe.size(), 3, p, n));
    CHECK(!hunk_data(exe.data(), 20, 0, p, n) && !hunk_data(exe.data(), 3, 0, p, n));
    Bytes bad = exe;
    put32(bad, 0, 0x3F4);
    CHECK(!hunk_data(bad.data(), bad.size(), 0, p, n));
    for (size_t cut = 0; cut < exe.size(); cut += 3) CHECK(!hunk_data(exe.data(), cut, 2, p, n) || n == 60); // truncations never read out of range
    fx::Rng r{6};
    for (int i = 0; i < 300; ++i) {
        Bytes m = exe;
        for (int k = 0; k < 4; ++k) m[r.next() % m.size()] = uint8_t(r.next());
        (void)hunk_data(m.data(), m.size(), r.next() % 4, p, n);
    }
}

// Black Monks (cracks.md 3.4): a plain body, the tail under the retail chain with its first words damaged, the head of the tail also
// stored as a plain block earlier in the image (at disk offset $AB74F).
Bytes chain_encode_init(const Bytes& plain, uint32_t init) {
    Bytes out(plain.size());
    uint32_t prev = init;
    for (size_t i = 0; i + 1 < plain.size(); i += 2) {
        uint32_t w = (uint32_t(plain[i]) << 8) | plain[i + 1];
        uint32_t e = w ^ prev;
        out[i] = uint8_t(e >> 8);
        out[i + 1] = uint8_t(e);
        prev = w;
    }
    return out;
}

Bytes plain_image(uint32_t seed) {
    fx::Rng r{seed};
    Bytes img(kRetailImageSize);
    for (uint8_t& b : img) b = uint8_t(r.next());
    const uint8_t entry[4] = {0x60, 0x00, 0x6C, 0x30};
    std::memcpy(img.data(), entry, 4);
    return img;
}

void test_cracked_recipes() {
    constexpr size_t kTail = 0x3B000, kCopyDisk = 0xAB74F, kCopyLen = 1088;
    Bytes plain = plain_image(21);
    // the head of the tail is also in the body at $3AB4F
    std::memcpy(&plain[kCopyDisk - kRetailImageOffset], &plain[kTail], kCopyLen);
    Bytes disk(kAdf, 0);
    std::memcpy(disk.data(), "DOS\0", 4);
    std::memcpy(&disk[kRetailImageOffset], plain.data(), plain.size());
    // the tail as the crack left it: chain-encoded from the plain word before the copy's last word; the first stored words are junk
    const uint32_t init = (uint32_t(plain[kTail + kCopyLen - 2]) << 8) | plain[kTail + kCopyLen - 1];
    Bytes rest(plain.begin() + long(kTail + kCopyLen), plain.end());
    Bytes enc = chain_encode_init(rest, init);
    std::memcpy(&disk[kRetailImageOffset + kTail + kCopyLen], enc.data(), enc.size());
    for (size_t i = 0; i < kCopyLen; ++i) disk[kRetailImageOffset + kTail + i] = uint8_t(0xA5 ^ i); // damaged stored head
    Bytes d2 = disc2_adf(31);
    Catalogue cat;
    cat.cracked.push_back({sha1_hex(disk.data(), disk.size()), "crack-bm-test", "synthetic Black Monks", true, "synthetic",
                           CrackRecipe::BlackMonksTail, sha1_hex(plain.data(), plain.size())});
    ImportOptions opt;
    opt.catalogue = &cat;
    Result r = import_inputs({input("bm.adf", disk), input("d2.adf", d2)}, opt);
    CHECK(r.status == Status::Ok && r.game_image == plain && r.disc2 == d2);
    CHECK(!r.canonical && r.trainer && r.variant_id == "crack-bm-test" && r.build_tag.empty() && !r.disc2_rebuilt);
    CHECK(has_message(r, Message::Warn, "carries a trainer"));
    // the plain recipe on the same disk would leave a wrong tail: the expected-image check catches a damaged copy block
    Bytes damaged = disk;
    damaged[kCopyDisk + 7] ^= 0x40;
    Catalogue cat2 = cat;
    cat2.cracked[0].sha1 = sha1_hex(damaged.data(), damaged.size());
    opt.catalogue = &cat2;
    Result bad = import_inputs({input("bm.adf", damaged), input("d2.adf", d2)}, opt);
    CHECK(bad.status == Status::BadInput && has_message(bad, Message::Error, "expected SHA-1"));
    // the plain recipe: the expected image is checked as well
    Catalogue cat3;
    Bytes plain_disk(kAdf, 0);
    std::memcpy(plain_disk.data(), "DOS\0", 4);
    Bytes img = plain_image(22);
    std::memcpy(&plain_disk[kRetailImageOffset], img.data(), img.size());
    cat3.cracked.push_back({sha1_hex(plain_disk.data(), plain_disk.size()), "crack-plain-test", "synthetic plain crack", false, "synthetic",
                            CrackRecipe::Plain, sha1_hex(img.data(), img.size())});
    opt.catalogue = &cat3;
    Result ok = import_inputs({input("p.adf", plain_disk), input("d2.adf", d2)}, opt);
    CHECK(ok.status == Status::Ok && ok.game_image == img && !ok.trainer);
    cat3.cracked[0].image_sha1 = std::string(40, '0');
    Result wrong = import_inputs({input("p.adf", plain_disk), input("d2.adf", d2)}, opt);
    CHECK(wrong.status == Status::BadInput && has_message(wrong, Message::Error, "expected SHA-1"));
    // a manifest records the trainer flag
    fs::path dir = fs::temp_directory_path() / "x2_builds_crack_assets";
    remove_tree(dir);
    std::string err, why;
    CHECK(write_assets(dir.string(), r, err) && verify_assets(dir.string(), why));
    std::string mt = read_text(dir / "manifest.txt");
    CHECK(mt.find("trainer=true\n") != std::string::npos && mt.find("canonical=false\n") != std::string::npos && mt.find("build=") == std::string::npos);
    remove_tree(dir);
}

// A one-disk crunch: an executable holding the snapshot and six data files, for each cruncher.
struct OneDisk {
    Bytes disk, image, rebuilt_disc2;
    OneDiskCrunch entry;
};

OneDisk make_onedisk(Cruncher c, uint32_t seed) {
    OneDisk o;
    fx::Rng r{seed};
    const uint32_t base = c == Cruncher::Bs1 ? 0 : 0x80;
    const size_t snap_len = size_t(0x400 - base) + 0x3FC00 + 0x200;
    // the snapshot's decode-order content: random up front (the end of memory), a long quiet stretch after
    Bytes snap(snap_len);
    for (uint8_t& b : snap) b = uint8_t(r.next());
    Bytes block;
    Bytes expected_snap;
    if (c == Cruncher::Onyx) {
        block = onyx_block(snap_len, seed, expected_snap, 600);
    } else if (c == Cruncher::Ice) {
        block = ice_block(reversed(snap), expected_snap, 20, seed);
    } else {
        Bytes dec = reversed(snap);
        block = bs1_block(dec, expected_snap);
    }
    // the executable: hunk layout and the block offset depend on the cruncher
    size_t hunk = 0, off = 0;
    std::vector<Bytes> codes;
    if (c == Cruncher::Ice) {
        hunk = 2;
        off = 0x196;
        codes = {Bytes(64, 1), Bytes(32, 2), Bytes()};
        Bytes code(off, 0xEE);
        code.insert(code.end(), block.begin(), block.end()); // ulen, plen, then the stream, as the Ice header is read
        codes[2] = code;
    } else {
        hunk = 0;
        off = c == Cruncher::Onyx ? 0x20A : 0xE8;
        Bytes code(off, 0xEE);
        code.insert(code.end(), block.begin(), block.end());
        codes = {code};
    }
    Bytes exe = hunk_exe(codes);
    // the six data files (sector start and count in the name)
    Ofs ofs;
    ofs.add_file("s", Bytes{'S', 'T', 'A', 'R', 'T', '\n'});
    ofs.add_file("exe", exe);
    const uint32_t starts[6] = {11, 250, 514, 799, 1154, 1476};
    std::map<size_t, Bytes> chunks;
    for (int i = 0; i < 6; ++i) {
        Bytes expected;
        Bytes b = onyx_block(1024, seed + 100u + uint32_t(i), expected);
        char name[16];
        std::snprintf(name, sizeof name, "%04x%04x", starts[i], 2u);
        ofs.add_file(name, b);
        chunks[starts[i]] = expected;
    }
    o.disk = ofs.d;
    const size_t lo = size_t(0x400 - base);
    o.image.assign(expected_snap.begin() + long(lo), expected_snap.begin() + long(lo + 0x3FC00));
    // disc 2 as the importer rebuilds it: the id in sector 0, $E5 between the files, zeros after the last
    o.rebuilt_disc2.assign(kAdf, 0);
    std::fill(o.rebuilt_disc2.begin() + 512, o.rebuilt_disc2.begin() + long(1478 * 512), uint8_t(0xE5));
    for (auto& kv : chunks) std::copy(kv.second.begin(), kv.second.end(), o.rebuilt_disc2.begin() + long(kv.first * 512));
    std::fill(o.rebuilt_disc2.begin(), o.rebuilt_disc2.begin() + 512, uint8_t(0xE5));
    o.rebuilt_disc2[0] = o.rebuilt_disc2[1] = 0;
    std::memcpy(&o.rebuilt_disc2[2], "X2B ", 4);
    o.entry.sha1 = sha1_hex(o.disk.data(), o.disk.size());
    o.entry.id = "crunch-test";
    o.entry.name = "synthetic one-disk crunch";
    o.entry.exe_sha1 = sha1_hex(exe.data(), exe.size());
    o.entry.cruncher = c;
    o.entry.hunk = int(hunk);
    o.entry.block = off;
    o.entry.base = base;
    o.entry.trainer = c == Cruncher::Bs1;
    o.entry.image_sha1 = sha1_hex(o.image.data(), o.image.size());
    o.entry.note = "synthetic";
    return o;
}

void test_one_disk_crunches(const fs::path& work) {
    for (Cruncher c : {Cruncher::Onyx, Cruncher::Ice, Cruncher::Bs1}) {
        OneDisk o = make_onedisk(c, 50u + uint32_t(c));
        Catalogue cat;
        cat.onedisk.push_back(o.entry);
        ImportOptions opt;
        opt.catalogue = &cat;
        Result r = import_inputs({input("one.adf", o.disk)}, opt);
        CHECK(r.status == Status::Ok);
        CHECK(r.game_image == o.image && r.disc2 == o.rebuilt_disc2 && r.disc2_rebuilt && !r.canonical && r.build_tag.empty());
        CHECK(r.trainer == (c == Cruncher::Bs1) && r.variant_id == "crunch-test");
        CHECK(has_message(r, Message::Info, "one-disk crunch") && has_message(r, Message::Info, "rebuilt from the data files"));
        Bytes std2 = disc2_adf(77);
        // a disc 2 that is not the standard one is not used
        Result w = import_inputs({input("one.adf", o.disk), input("d2.adf", std2)}, opt);
        CHECK(w.status == Status::Ok && w.disc2 == o.rebuilt_disc2 && has_message(w, Message::Info, "disc 2 is rebuilt"));
        // the manifest
        fs::path dir = work / ("onedisk_" + std::to_string(int(c)));
        remove_tree(dir);
        std::string err, why;
        CHECK(write_assets(dir.string(), r, err) && verify_assets(dir.string(), why));
        std::string mt = read_text(dir / "manifest.txt");
        CHECK(mt.find("disc2_rebuilt=true\n") != std::string::npos && mt.find("variant=crunch-test\n") != std::string::npos);
        CHECK((mt.find("trainer=true\n") != std::string::npos) == (c == Cruncher::Bs1));
        remove_tree(dir);
        // damage: the wrong expected image, a missing executable, a bad hunk, a bad block offset
        Catalogue wrong = cat;
        wrong.onedisk[0].image_sha1 = std::string(40, '0');
        ImportOptions wo;
        wo.catalogue = &wrong;
        Result wr = import_inputs({input("one.adf", o.disk)}, wo);
        CHECK(wr.status == Status::BadInput && has_message(wr, Message::Error, "expected SHA-1"));
        Catalogue badexe = cat;
        badexe.onedisk[0].exe_sha1 = std::string(40, '1');
        wo.catalogue = &badexe;
        Result be = import_inputs({input("one.adf", o.disk)}, wo);
        CHECK(be.status == Status::BadInput && has_message(be, Message::Error, "executable is not on the disk"));
        Catalogue badhunk = cat;
        badhunk.onedisk[0].hunk = 9;
        wo.catalogue = &badhunk;
        Result bh = import_inputs({input("one.adf", o.disk)}, wo);
        CHECK(bh.status == Status::BadInput && has_message(bh, Message::Error, "hunk layout"));
        Catalogue badblock = cat;
        badblock.onedisk[0].block += 4;
        wo.catalogue = &badblock;
        Result bb = import_inputs({input("one.adf", o.disk)}, wo);
        CHECK(bb.status == Status::BadInput);
    }
}

// The Power Pack's and the 1762 compilation's registry rows and the pairing rules, with a synthetic catalogue.
void test_builds_and_pairing() {
    Bytes image = plain_image(61);
    Bytes disk1(kAdf, 0);
    std::memcpy(disk1.data(), "DOS\0", 4);
    std::memcpy(&disk1[kRetailImageOffset], image.data(), image.size());
    Bytes disk2 = disc2_adf(62);
    Catalogue cat;
    GameBuild gb;
    gb.id = "build-test";
    gb.name = "synthetic compilation";
    gb.build_tag = "build-test";
    gb.game_sha1 = sha1_hex(image.data(), image.size());
    gb.disk1_image_sha1 = gb.game_sha1;
    gb.disc2_sha1 = sha1_hex(disk2.data(), disk2.size());
    gb.note = "synthetic note";
    cat.builds.push_back(gb);
    ImportOptions opt;
    opt.catalogue = &cat;
    Result r = import_inputs({input("d1.adf", disk1), input("d2.adf", disk2)}, opt);
    CHECK(r.status == Status::Ok && r.game_image == image && r.disc2 == disk2 && r.build_tag == "build-test" && !r.canonical && !r.disc2_rebuilt);
    CHECK(r.variant_id == "build-test" && has_message(r, Message::Info, "manifest build=build-test"));
    // disk 1 alone: it needs its own disk 2
    Result alone = import_inputs({input("d1.adf", disk1)}, opt);
    CHECK(alone.status == Status::Unsupported && has_message(alone, Message::Error, "needs its own disk 2"));
    // another disc 2 (a different build's, or the standard one) is not interchangeable
    Bytes other2 = disc2_adf(63);
    Result mism = import_inputs({input("d1.adf", disk1), input("d2.adf", other2)}, opt);
    CHECK(mism.status == Status::Unsupported && has_message(mism, Message::Warn, "different build"));
    // the build's disc 2 with a canonical-shaped disc 1 (another game image) is refused as well
    Bytes canon1(kAdf, 0);
    std::memcpy(canon1.data(), "DOS\0", 4);
    Bytes cimg = plain_image(64);
    std::memcpy(&canon1[kRetailImageOffset], cimg.data(), cimg.size());
    Catalogue cat2 = cat;
    cat2.cracked.push_back({sha1_hex(canon1.data(), canon1.size()), "crack-x", "synthetic crack", false, "x"});
    opt.catalogue = &cat2;
    Result cm = import_inputs({input("c1.adf", canon1), input("d2.adf", disk2)}, opt);
    CHECK(cm.status == Status::Unsupported && has_message(cm, Message::Warn, "different build") && has_message(cm, Message::Error, "disc 2 not found"));
    // the manifest carries the build tag
    fs::path dir = fs::temp_directory_path() / "x2_builds_build_assets";
    remove_tree(dir);
    std::string err, why;
    CHECK(write_assets(dir.string(), r, err) && verify_assets(dir.string(), why));
    std::string mt = read_text(dir / "manifest.txt");
    CHECK(mt.find("build=build-test\n") != std::string::npos && mt.find("variant=build-test\n") != std::string::npos && mt.find("trainer=") == std::string::npos);
    remove_tree(dir);
}

void test_registry() {
    int builds = 0, onedisk = 0;
    for (const VariantInfo& v : all_variants()) {
        if (v.recipe == Recipe::GameBuildPowerPack || v.recipe == Recipe::GameBuildCompilation) {
            ++builds;
            CHECK(v.supported && !v.canonical && !v.build.empty() && v.game_sha1.size() == 40 && v.disc2_sha1.size() == 40);
        }
        if (v.recipe == Recipe::OneDiskCrunch) {
            ++onedisk;
            CHECK(v.supported && !v.canonical && v.game_sha1.size() == 40);
        }
    }
    CHECK(builds == 2 && onedisk == 3);
    const Catalogue& cat = builtin_catalogue();
    CHECK(cat.find_build("power-pack-2234") && cat.find_build("compilation-1762") && !cat.find_build("nonsense"));
    CHECK(cat.find_build_by_disk1_image(cat.find_build("compilation-1762")->disk1_image_sha1) == cat.find_build("compilation-1762"));
    CHECK(cat.find_build_by_disc2(cat.find_build("compilation-1762")->disc2_sha1) == cat.find_build("compilation-1762"));
    CHECK(!cat.find_build_by_disc2(cat.find_build("power-pack-2234")->disc2_sha1)); // the Power Pack's disc 2 is not a separate disk
    CHECK(cat.find_build_by_game(cat.find_build("power-pack-2234")->game_sha1) == cat.find_build("power-pack-2234"));
    // the cracks that used to be refused are importable now; the two that cannot be recovered are still refused
    CHECK(!find_refused_disc1("12b7bd53288d3764af062a71923c9ec8189e61f2") && !find_refused_disc1("8f50b895f344400787c34929bb29ef3e123b0a4e"));
    CHECK(find_refused_disc1("2405da96182c63fc6a7e48b7f19f2904f0a44ed9") && find_refused_disc1("1b4855f98f7df8d5052e5578d387dbf486068f8f"));
    CHECK(cat.find_cracked("12b7bd53288d3764af062a71923c9ec8189e61f2") && cat.find_cracked("12b7bd53288d3764af062a71923c9ec8189e61f2")->trainer);
    CHECK(cat.find_cracked("0df9822509837bce3999806e3ae2e218cea4ddd9")->recipe == CrackRecipe::BlackMonksTail);
    CHECK(cat.find_onedisk("1181e872781906466335349b2bfa6c72337b3096")->cruncher == Cruncher::Bs1);
    CHECK(!find_refused_disc2("c294d6644f9d5b20056b46f004cd8ec891b01440")); // the 1762 disk 2 is the compilation's own now
}

} // namespace

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_builds_test";
    fs::create_directories(work);
    test_onyx();
    test_ice();
    test_bs1();
    test_powerpack_crunchers();
    test_speedlock_tracks();
    test_powerpack_build();
    test_hunk_data();
    test_cracked_recipes();
    test_one_disk_crunches(work);
    test_builds_and_pairing();
    test_registry();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
