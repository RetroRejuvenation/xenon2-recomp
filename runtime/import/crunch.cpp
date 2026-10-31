// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/crunch.h"

#include <algorithm>
#include <cstring>

namespace x2::import {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

constexpr unsigned kMaxWidth = 24; // widest variable field accepted; real streams use 5 to 14 bits

bool fail(std::string& why, const char* text) {
    why = text;
    return false;
}

// Output written backwards from its end, with every access checked.
struct BackOut {
    std::vector<uint8_t> out;
    size_t a = 0; // next write position + 1 (the write pointer, pre-decremented)
    explicit BackOut(size_t n) : out(n, 0), a(n) {}
    bool room(size_t count) const { return count <= a; }
};

} // namespace

// --- ONYX ---------------------------------------------------------------------------------------------------------

bool onyx_decrunch(const uint8_t* blob, size_t size, size_t offset, std::vector<uint8_t>& out, std::string& why) {
    if (offset > size || size - offset < 12) return fail(why, "ONYX block: too short for its header");
    const uint8_t* h = blob + offset;
    const unsigned p3 = h[0], p2 = h[1], p1 = h[2], p0 = h[3] & 0xFu, d3 = h[3] >> 4;
    const size_t ulen = be32(h + 4), plen = be32(h + 8);
    const size_t start = offset + 12;
    if (plen > size - start || ulen > kMaxCrunchedSize) return fail(why, "ONYX block does not fit the file");
    if (p3 > kMaxWidth || p2 > kMaxWidth || p1 > kMaxWidth || p0 > kMaxWidth) return fail(why, "ONYX block: unsupported field width");
    size_t pos = start + plen;
    BackOut o(ulen);
    bool underrun = false;
    auto rd = [&]() -> uint8_t {
        if (pos <= start) { underrun = true; return 0; }
        return blob[--pos];
    };
    unsigned d7 = rd();
    while (d7 == 0 && !underrun) d7 = rd();
    unsigned x = 1; // move #$10,ccr
    while (!underrun) { // find the sentinel bit
        unsigned carry = d7 & 1u;
        d7 = (x << 7) | (d7 >> 1);
        x = carry;
        if (d7 == 0) {
            d7 = rd();
            while (d7 == 0 && !underrun) d7 = rd();
            x = 1;
            continue;
        }
        if (carry) break;
    }
    if (underrun) return fail(why, "ONYX block: read past the start of the stream");
    auto bit = [&]() -> unsigned {
        unsigned c = d7 & 1u;
        d7 >>= 1;
        if (d7 == 0) { // that bit was the sentinel: refill
            d7 = rd();
            c = d7 & 1u;
            d7 = (1u << 7) | (d7 >> 1);
        }
        return c;
    };
    auto get = [&](unsigned n) -> uint32_t {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    };
    bool bad = false;
    auto copy_back = [&](size_t count, size_t dist) {
        if (!o.room(count) || dist > size_t(kMaxCrunchedSize) || o.a - count + dist + count > ulen) { bad = true; return; }
        for (size_t i = 0; i < count; ++i) {
            --o.a;
            o.out[o.a] = o.out[o.a + dist];
        }
    };
    auto literal = [&](size_t count) {
        if (!o.room(count) || size_t(pos - start) < count) { bad = true; return; }
        for (size_t i = 0; i < count; ++i) o.out[--o.a] = blob[--pos];
    };
    unsigned d2 = 0;
    while (o.a > 0 && !bad && !underrun) {
        bool take_match = false;
        if (d2 & 1u) take_match = true;
        d2 >>= 1;
        if (!take_match && get(1) == 0) take_match = true;
        if (!take_match) { // literal / run-length family
            if (get(1) == 0) { literal(1); d2 = 1; continue; }
            uint32_t n = get(2);
            if (n != 3) { literal(n + 1 + 1); d2 = 1; continue; }
            n = get(2);
            if (n != 3) { literal(n + 4 + 1); d2 = 1; continue; }
            n = get(3);
            if (n != 7) { literal(n + 7 + 1); d2 = 1; continue; }
            unsigned d4 = p2;
            uint32_t d2x = 0x10E;
            if (d3 & 4u) d2x = 0x0E;
            else if (get(1) == 0) { d4 = 8; d2x = 0x0E; }
            literal(size_t(get(d4)) + d2x + 1);
            d2 = 1;
            continue;
        }
        // match family
        if (get(1) == 0) { // short match: 2 bytes
            uint32_t off = get(8);
            copy_back(2, size_t(off) + 2);
            d2 = 0;
            continue;
        }
        uint32_t n = get(2), length;
        if (n != 3) {
            length = n + 3;
        } else {
            n = get(3);
            if (n != 7) {
                length = n + 6;
            } else if (get(1) != 0) {
                length = get(p0) + 13;
            } else { // run-length fill
                unsigned d4 = 7;
                uint32_t d2x = 5;
                if (d3 & 2u) d4 = p3;
                else if (get(1) == 0) { d2x = 0x85; d4 = p3; }
                size_t cnt = size_t(get(d4)) + d2x;
                uint8_t fill = uint8_t(get(8));
                if (!o.room(cnt)) { bad = true; break; }
                for (size_t i = 0; i < cnt; ++i) o.out[--o.a] = fill;
                d2 = 0;
                continue;
            }
        }
        uint32_t off;
        if (get(1) == 0) off = get(6);
        else if (!(d3 & 1u)) off = get(p1) + 0x40;
        else if (get(1) == 0) off = get(8) + 0x40;
        else off = get(p1) + 0x140;
        copy_back(length, size_t(off) + length);
        d2 = 0;
    }
    if (bad) return fail(why, "ONYX block: a run or match leaves the output (damaged data)");
    if (underrun) return fail(why, "ONYX block: read past the start of the stream");
    out = std::move(o.out);
    return true;
}

// --- Ice-style ----------------------------------------------------------------------------------------------------

bool ice_depack(const uint8_t* blob, size_t size, size_t stream_start, size_t unpacked_len, size_t packed_len,
                std::vector<uint8_t>& out, std::string& why) {
    if (stream_start > size || packed_len > size - stream_start || packed_len < 8 || unpacked_len > kMaxCrunchedSize)
        return fail(why, "Ice block does not fit the file");
    BackOut o(unpacked_len);
    size_t a2 = stream_start + packed_len;
    int over = 0;
    bool underrun = false;
    auto word = [&]() -> uint32_t {
        if (a2 < stream_start + 2) { // the stub's last refill reads one word ahead, in front of the stream
            a2 = a2 >= 2 ? a2 - 2 : 0;
            if (++over > 4) underrun = true;
            return 0;
        }
        a2 -= 2;
        return be16(blob + a2);
    };
    a2 -= 2;
    const uint32_t d0 = be16(blob + a2); // move.w -(a2),d0
    a2 -= 4;
    uint32_t d6 = be32(blob + a2); // move.l -(a2),d6
    unsigned shift = (0x10u - d0) & 63u;
    d6 = shift >= 32 ? 0 : d6 >> shift;
    uint32_t d7 = d0;
    const uint32_t d3 = 0x10;
    auto swap = [](uint32_t v) { return (v << 16) | (v >> 16); };
    auto get1 = [&]() -> uint32_t {
        d7 = (d7 - 1) & 0xFFFFu;
        if (d7 != 0) {
            uint32_t c = d6 & 1u;
            d6 >>= 1;
            return c;
        }
        d7 = 0x10;
        uint32_t low = d6 & 0xFFFFu;
        d6 >>= 1;
        d6 = swap(d6);
        d6 = (d6 & 0xFFFF0000u) | word();
        d6 = swap(d6);
        return low & 1u;
    };
    auto getn = [&](unsigned n) -> uint32_t {
        uint32_t v = d6 & 0xFFFFu;
        d6 >>= n;
        d7 = (d7 - n) & 0xFFFFu;
        int32_t s = (d7 & 0x8000u) ? int32_t(d7) - 0x10000 : int32_t(d7);
        if (s <= 0) {
            d7 = (d7 + d3) & 0xFFFFu;
            unsigned r = d7 & 31u;
            if (r) d6 = (d6 >> r) | (d6 << (32 - r)); // ror.l d7,d6
            d6 = (d6 & 0xFFFF0000u) | word();
            if (r) d6 = (d6 << r) | (d6 >> (32 - r)); // rol.l d7,d6
        }
        return v & ((1u << n) - 1u);
    };
    bool bad = false;
    auto lits = [&](size_t count) {
        if (!o.room(count)) { bad = true; return; }
        for (size_t i = 0; i < count; ++i) o.out[--o.a] = uint8_t(getn(8));
    };
    while (o.a > 0 && !bad && !underrun) {
        if (get1()) { lits(1); continue; }
        uint32_t d1, d4;
        if (get1()) {
            if (get1()) {
                if (get1()) { d1 = 8; d4 = 0x17; }
                else { d1 = 4; d4 = 7; }
            } else { d1 = 2; d4 = 3; }
        } else { d1 = 1; d4 = 1; }
        d4 += getn(d1);
        if (d4 == 0x16) { // long literal run
            d4 = 14;
            d1 = get1() ? 5 : 14;
            d4 += getn(d1);
            lits(size_t(d4) + 1);
            continue;
        }
        if (d4 > 0x16) --d4;
        uint32_t d2;
        if (!get1()) { d1 = 9; d2 = 0x20; }
        else if (!get1()) { d1 = 5; d2 = 0; }
        else { d1 = 14; d2 = 0x220; }
        size_t off = size_t(getn(d1)) + d2, count = size_t(d4) + 1;
        if (!o.room(count) || o.a - count + off + count > unpacked_len) { bad = true; break; }
        for (size_t i = 0; i < count; ++i) {
            --o.a;
            o.out[o.a] = o.out[o.a + off];
        }
    }
    if (bad) return fail(why, "Ice block: a run or match leaves the output (damaged data)");
    if (underrun) return fail(why, "Ice block: read past the start of the stream");
    out = std::move(o.out);
    return true;
}

// --- BS1 ByteKiller flavour ---------------------------------------------------------------------------------------

bool bs1_depack(const uint8_t* blob, size_t size, size_t hdr, std::vector<uint8_t>& out, std::string& why) {
    if (hdr > size || size - hdr < 12) return fail(why, "BS1 block: too short for its header");
    const size_t plen = be32(blob + hdr), ulen = be32(blob + hdr + 4);
    uint32_t crc = be32(blob + hdr + 8);
    const size_t start = hdr + 12;
    if (plen > size - start || ulen > kMaxCrunchedSize || plen < 4) return fail(why, "BS1 block does not fit the file");
    size_t pos = start + plen;
    bool underrun = false;
    auto rl = [&]() -> uint32_t {
        if (pos < start + 4) { underrun = true; return 1; }
        pos -= 4;
        uint32_t v = be32(blob + pos);
        crc ^= v;
        return v;
    };
    uint32_t d0 = rl();
    auto bit = [&]() -> uint32_t {
        uint32_t c = d0 & 1u;
        d0 >>= 1;
        if (d0 == 0) {
            uint32_t v = rl();
            c = v & 1u;
            d0 = (v >> 1) | 0x80000000u;
        }
        return c;
    };
    auto bits = [&](unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    };
    BackOut o(ulen);
    bool bad = false;
    auto literals = [&](size_t count) {
        if (!o.room(count)) { bad = true; return; }
        for (size_t i = 0; i < count; ++i) o.out[--o.a] = uint8_t(bits(8));
    };
    auto copy = [&](size_t count, size_t off, bool zero_beyond) {
        if (!o.room(count)) { bad = true; return; }
        for (size_t i = 0; i < count; ++i) {
            --o.a;
            if (o.a + off < ulen) o.out[o.a] = o.out[o.a + off];
            else if (zero_beyond) o.out[o.a] = 0;
            else { bad = true; return; }
        }
    };
    while (o.a > 0 && !bad && !underrun) {
        if (bit()) {
            uint32_t n = bits(2);
            if (n < 2) {
                unsigned nb = 9 + n;
                size_t cnt = n + 3, off = bits(nb);
                copy(cnt, off, true);
            } else if (n == 3) {
                literals(size_t(bits(8)) + 8 + 1);
            } else {
                size_t cnt = size_t(bits(8)) + 1, off = bits(12);
                copy(cnt, off, false);
            }
        } else if (bit()) {
            size_t off = bits(8);
            copy(2, off, false);
        } else {
            literals(size_t(bits(3)) + 1);
        }
    }
    if (bad) return fail(why, "BS1 block: a run or match leaves the output (damaged data)");
    if (underrun) return fail(why, "BS1 block: the packed stream is truncated or damaged");
    if (crc != 0) return fail(why, "BS1 block: the checksum of the packed stream does not match");
    out = std::move(o.out);
    return true;
}

// --- Power Pack crunchers -----------------------------------------------------------------------------------------

namespace {

struct LongReader {
    const uint8_t* packed;
    size_t pos;
    bool underrun = false;
    LongReader(const uint8_t* p, size_t n) : packed(p), pos(n) {}
    uint32_t next() {
        if (pos < 4) { underrun = true; return 0; }
        pos -= 4;
        return be32(packed + pos);
    }
};

template <class Bit>
bool crunch_body(Bit&& bit, LongReader& rd, BackOut& o, std::string& why) {
    auto bits = [&](unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    };
    const size_t total = o.out.size();
    bool bad = false;
    auto copy = [&](size_t count, size_t off) {
        if (!o.room(count)) { bad = true; return; }
        for (size_t i = 0; i < count; ++i) {
            --o.a;
            size_t s = o.a + off;
            o.out[o.a] = s < total ? o.out[s] : uint8_t(0);
        }
    };
    auto literals = [&](size_t count) {
        if (!o.room(count)) { bad = true; return; }
        for (size_t i = 0; i < count; ++i) o.out[--o.a] = uint8_t(bits(8));
    };
    while (o.a > 0 && !bad && !rd.underrun) {
        if (bit() == 0) {
            if (bit() == 1) {
                size_t off = bits(8);
                copy(2, off); // copy(1, off): count + 1 bytes
            } else {
                literals(size_t(bits(3)) + 1);
            }
        } else {
            uint32_t sel = bits(2);
            if (sel < 2) {
                size_t off = bits(8 + sel);
                copy(size_t(sel) + 3, off); // copy(sel + 2, off): count + 1 bytes
            } else if (sel == 3) {
                literals(size_t(bits(8)) + 8 + 1);
            } else {
                size_t n = bits(8);
                size_t off = bits(8);
                copy(n + 1, off);
            }
        }
    }
    if (bad) { why = "Power Pack stream: a run or match leaves the output (damaged data)"; return false; }
    if (rd.underrun) { why = "Power Pack stream: read past the start of the stream"; return false; }
    return true;
}

} // namespace

bool powerpack_decrunch_msb(const uint8_t* packed, size_t packed_len, size_t unpacked_len, std::vector<uint8_t>& out, bool& consumed,
                            std::string& why) {
    consumed = false;
    if (unpacked_len > kMaxCrunchedSize) return fail(why, "Power Pack stream: unpacked size out of range");
    LongReader rd(packed, packed_len);
    uint32_t d0 = rd.next();
    auto bit = [&]() -> uint32_t {
        uint32_t c = (d0 >> 31) & 1u;
        d0 <<= 1;
        if (d0 == 0) {
            d0 = rd.next();
            c = (d0 >> 31) & 1u;
            d0 = (d0 << 1) | 1u;
        }
        return c;
    };
    BackOut o(unpacked_len);
    if (!crunch_body(bit, rd, o, why)) return false;
    consumed = rd.pos == 0;
    out = std::move(o.out);
    return true;
}

bool powerpack_decrunch_lsb(const uint8_t* packed, size_t packed_len, size_t unpacked_len, std::vector<uint8_t>& out, bool& consumed,
                            std::string& why) {
    consumed = false;
    if (unpacked_len > kMaxCrunchedSize) return fail(why, "Power Pack stream: unpacked size out of range");
    LongReader rd(packed, packed_len);
    uint32_t d0 = rd.next();
    auto bit = [&]() -> uint32_t {
        uint32_t c = d0 & 1u;
        d0 >>= 1;
        if (d0 == 0) {
            d0 = rd.next();
            c = d0 & 1u;
            d0 = (d0 >> 1) | 0x80000000u;
        }
        return c;
    };
    BackOut o(unpacked_len);
    if (!crunch_body(bit, rd, o, why)) return false;
    consumed = rd.pos == 0;
    out = std::move(o.out);
    return true;
}

bool powerpack_unrle(const uint8_t* src, size_t src_len, uint8_t marker, size_t out_len, std::vector<uint8_t>& out, size_t& left,
                     std::string& why) {
    left = out_len;
    if (out_len > kMaxCrunchedSize) return fail(why, "Power Pack RLE: output size out of range");
    BackOut o(out_len);
    size_t a2 = src_len;
    while (a2 > 0) {
        uint8_t b = src[--a2];
        if (b == marker) {
            if (a2 < 2) return fail(why, "Power Pack RLE: a marker without its value and count");
            uint8_t val = src[--a2];
            size_t cnt = src[--a2];
            if (cnt == 0) cnt = 256;
            if (!o.room(cnt)) return fail(why, "Power Pack RLE: a run leaves the output");
            for (size_t i = 0; i < cnt; ++i) o.out[--o.a] = val;
        } else {
            if (!o.room(1)) return fail(why, "Power Pack RLE: more data than the output holds");
            o.out[--o.a] = b;
        }
    }
    left = o.a;
    out = std::move(o.out);
    return true;
}

// --- hunks --------------------------------------------------------------------------------------------------------

bool hunk_data(const uint8_t* blob, size_t size, size_t index, const uint8_t*& data, size_t& len) {
    size_t p = 0;
    bool ok = true;
    auto u32 = [&]() -> uint32_t {
        if (p + 4 > size) { ok = false; return 0; }
        uint32_t v = be32(blob + p);
        p += 4;
        return v;
    };
    auto skip = [&](size_t n) {
        if (n > size - std::min(p, size)) ok = false;
        else p += n;
    };
    if (size < 4 || be32(blob) != 0x3F3) return false;
    p = 4;
    for (int guard = 0; ok && guard < 64; ++guard) { // resident library names (normally none)
        uint32_t n = u32();
        if (!ok || n == 0) break;
        if (n > 256) return false;
        skip(size_t(n) * 4);
    }
    u32(); // table size
    uint32_t first = u32(), last = u32();
    if (!ok || last < first || last - first > 1024) return false;
    const size_t count = size_t(last - first) + 1;
    for (size_t i = 0; ok && i < count; ++i) {
        uint32_t s = u32();
        if ((s >> 30) == 3) u32();
    }
    size_t cur = 0;
    while (ok && p < size && cur < count) {
        uint32_t t = u32() & 0x3FFFFFFFu;
        if (t == 0x3E9 || t == 0x3EA) {
            size_t n = size_t(u32() & 0x3FFFFFFFu) * 4;
            if (!ok || n > size - p) return false;
            if (cur == index) { data = blob + p; len = n; return true; }
            p += n;
        } else if (t == 0x3EB) {
            u32();
        } else if (t == 0x3EC || t == 0x3FC || t == 0x3F7) {
            for (int guard = 0; ok && guard < 100000; ++guard) {
                size_t n;
                if (t == 0x3FC) {
                    if (p + 2 > size) return false;
                    n = be16(blob + p);
                    p += 2;
                } else {
                    n = u32();
                }
                if (!ok || n == 0) break;
                if (t == 0x3FC) skip(2 + 2 * n);
                else skip(4 + 4 * n);
            }
            if (t == 0x3FC) p = (p + 3) & ~size_t(3);
        } else if (t == 0x3F0) {
            for (int guard = 0; ok && guard < 100000; ++guard) {
                uint32_t n = u32() & 0xFFFFFFu;
                if (!ok || n == 0) break;
                skip(size_t(n) * 4);
                u32();
            }
        } else if (t == 0x3F1 || t == 0x3E8) {
            skip(size_t(u32()) * 4);
        } else if (t == 0x3F2) {
            ++cur;
        } else {
            return false;
        }
    }
    return false;
}

} // namespace x2::import
