// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "cdtv/inflate_stream.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace x2::cdtv {

namespace {

constexpr int kMaxBits = 15;
constexpr int kFastBits = 10; // codes up to this length decode with one table lookup
constexpr size_t kWindow = 1u << 16; // output ring: 32 KB history + up to 32 KB not yet passed to the sink
constexpr size_t kFlush = 1u << 15;

// Canonical Huffman code (as vendor/puff: counts per length and symbols ordered by code), plus a lookup table
// for the short codes: entry = length << 9 | symbol, 0 = longer code, decoded bit by bit.
struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, 320> symbol{};
    std::array<uint16_t, 1u << kFastBits> fast{};

    // False for an over-subscribed set of lengths. Incomplete codes are accepted (a single distance code is
    // legal); decoding an unused code is then reported as corrupt.
    bool build(const uint8_t* lengths, int n) {
        count.fill(0);
        fast.fill(0);
        for (int s = 0; s < n; ++s) ++count[lengths[s]];
        if (count[0] == n) return true;
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left = (left << 1) - count[size_t(len)];
            if (left < 0) return false;
        }
        std::array<uint16_t, kMaxBits + 2> offs{};
        for (int len = 1; len < kMaxBits; ++len) offs[size_t(len + 1)] = uint16_t(offs[size_t(len)] + count[size_t(len)]);
        for (int s = 0; s < n; ++s)
            if (lengths[s]) symbol[offs[lengths[s]]++] = uint16_t(s);
        // Fast table: canonical codes are assigned in symbol-table order within each length.
        uint32_t code = 0;
        size_t index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            for (int i = 0; i < count[size_t(len)]; ++i, ++code, ++index) {
                if (len > kFastBits) continue;
                uint32_t rev = 0; // deflate sends Huffman codes most significant bit first
                for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1u) << (len - 1 - b);
                for (uint32_t f = rev; f < (1u << kFastBits); f += 1u << len)
                    fast[f] = uint16_t(len << 9 | symbol[index]);
            }
            code <<= 1;
        }
        return true;
    }
};

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class Decoder {
public:
    Decoder(const InflateSource& src, const InflateSink& sink) : src_(src), sink_(sink), in_(1u << 16), win_(kWindow) {}

    InflateResult run(uint64_t* total) {
        InflateResult r = blocks();
        if (r == InflateResult::Done && !flush()) r = InflateResult::Stopped;
        if (total) *total = flushed_;
        return r;
    }

private:
    struct Error {
        InflateResult r;
    };

    // Bit input, least significant bit first. At the end of the input zero bits are padded so that a code can
    // always be peeked; consuming a padded bit means the stream was truncated.
    void need(int n) {
        while (bits_ < n) {
            if (in_pos_ == in_len_) {
                in_len_ = eof_ ? 0 : src_(in_.data(), in_.size());
                in_pos_ = 0;
                if (in_len_ == 0) {
                    eof_ = true;
                    padded_ += 8;
                    bits_ += 8;
                    continue;
                }
            }
            buf_ |= uint64_t(in_[in_pos_++]) << bits_;
            bits_ += 8;
        }
    }
    void drop(int n) {
        buf_ >>= n;
        bits_ -= n;
        if (bits_ < padded_) throw Error{InflateResult::Truncated};
    }
    uint32_t take(int n) {
        if (n == 0) return 0;
        need(n);
        uint32_t v = uint32_t(buf_ & ((1ull << n) - 1));
        drop(n);
        return v;
    }

    int decode(const Huffman& h) {
        need(kMaxBits);
        if (uint16_t e = h.fast[buf_ & ((1u << kFastBits) - 1)]) {
            drop(e >> 9);
            return e & 0x1FF;
        }
        int code = 0, first = 0, index = 0;
        uint64_t b = buf_;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= int(b & 1);
            b >>= 1;
            int count = h.count[size_t(len)];
            if (code - count < first) {
                drop(len);
                return h.symbol[size_t(index + (code - first))];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Error{InflateResult::Corrupt};
    }

    void put(uint8_t b) {
        win_[pos_++ & (kWindow - 1)] = b;
        if (pos_ - flushed_ >= kFlush && !flush()) throw Error{InflateResult::Stopped};
    }
    bool flush() {
        while (flushed_ < pos_) {
            size_t at = size_t(flushed_ & (kWindow - 1));
            size_t n = size_t(std::min<uint64_t>(pos_ - flushed_, kWindow - at));
            if (!sink_(&win_[at], n)) {
                flushed_ += n;
                return false;
            }
            flushed_ += n;
        }
        return true;
    }

    void stored() {
        drop(bits_ & 7); // to a byte boundary (bits_ - padded_ is a multiple of 8 once aligned)
        uint32_t len = take(16), nlen = take(16);
        if ((len ^ 0xFFFF) != nlen) throw Error{InflateResult::Corrupt};
        while (len--) put(uint8_t(take(8)));
    }

    void codes(const Huffman& lit, const Huffman& dist) {
        for (;;) {
            int sym = decode(lit);
            if (sym < 256) {
                put(uint8_t(sym));
                continue;
            }
            if (sym == 256) return;
            sym -= 257;
            if (sym >= 29) throw Error{InflateResult::Corrupt};
            uint32_t len = kLenBase[sym] + take(kLenExtra[sym]);
            int ds = decode(dist);
            if (ds >= 30) throw Error{InflateResult::Corrupt};
            uint32_t d = kDistBase[ds] + take(kDistExtra[ds]);
            if (d > pos_) throw Error{InflateResult::Corrupt}; // before the start of the output
            while (len--) put(win_[(pos_ - d) & (kWindow - 1)]);
        }
    }

    void fixed() {
        if (!fixed_built_) {
            uint8_t l[288];
            int s = 0;
            for (; s < 144; ++s) l[s] = 8;
            for (; s < 256; ++s) l[s] = 9;
            for (; s < 280; ++s) l[s] = 7;
            for (; s < 288; ++s) l[s] = 8;
            fixed_lit_.build(l, 288);
            for (s = 0; s < 30; ++s) l[s] = 5;
            fixed_dist_.build(l, 30);
            fixed_built_ = true;
        }
        codes(fixed_lit_, fixed_dist_);
    }

    void dynamic() {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen = int(take(5)) + 257, ndist = int(take(5)) + 1, ncode = int(take(4)) + 4;
        if (nlen > 286 || ndist > 30) throw Error{InflateResult::Corrupt};
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(take(3));
        Huffman lencode;
        if (!lencode.build(lengths, 19)) throw Error{InflateResult::Corrupt};
        std::memset(lengths, 0, sizeof lengths);
        for (int i = 0; i < nlen + ndist;) {
            int sym = decode(lencode);
            if (sym < 16) {
                lengths[i++] = uint8_t(sym);
                continue;
            }
            uint8_t v = 0;
            int rep;
            if (sym == 16) {
                if (i == 0) throw Error{InflateResult::Corrupt};
                v = lengths[i - 1];
                rep = 3 + int(take(2));
            } else if (sym == 17) rep = 3 + int(take(3));
            else rep = 11 + int(take(7));
            if (i + rep > nlen + ndist) throw Error{InflateResult::Corrupt};
            while (rep--) lengths[i++] = v;
        }
        if (lengths[256] == 0) throw Error{InflateResult::Corrupt}; // no end-of-block code
        if (!lit_.build(lengths, nlen) || !dist_.build(lengths + nlen, ndist)) throw Error{InflateResult::Corrupt};
        codes(lit_, dist_);
    }

    InflateResult blocks() {
        try {
            bool last;
            do {
                last = take(1);
                switch (take(2)) {
                case 0: stored(); break;
                case 1: fixed(); break;
                case 2: dynamic(); break;
                default: return InflateResult::Corrupt;
                }
            } while (!last);
            return InflateResult::Done;
        } catch (const Error& e) {
            return e.r;
        }
    }

    const InflateSource& src_;
    const InflateSink& sink_;
    std::vector<uint8_t> in_;
    size_t in_pos_ = 0, in_len_ = 0;
    bool eof_ = false;
    uint64_t buf_ = 0;
    int bits_ = 0, padded_ = 0;
    std::vector<uint8_t> win_;
    uint64_t pos_ = 0, flushed_ = 0;
    Huffman lit_, dist_, fixed_lit_, fixed_dist_;
    bool fixed_built_ = false;
};

} // namespace

const char* inflate_result_name(InflateResult r) {
    switch (r) {
    case InflateResult::Done: return "ok";
    case InflateResult::Stopped: return "stopped";
    case InflateResult::Corrupt: return "corrupted deflate data";
    case InflateResult::Truncated: return "truncated deflate data";
    }
    return "?";
}

InflateResult inflate_stream(const InflateSource& source, const InflateSink& sink, uint64_t* out_total) {
    auto d = std::make_unique<Decoder>(source, sink);
    return d->run(out_total);
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

} // namespace x2::cdtv
