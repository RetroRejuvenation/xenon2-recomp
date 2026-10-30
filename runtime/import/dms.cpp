// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/dms.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace x2::import {

namespace {

constexpr size_t kMaxTrackBytes = 32000; // packed, intermediate and unpacked sizes above this are refused
constexpr size_t kCylinderBytes = 2 * kTrackBytes; // one DMS "track" of a DD disk: both sides, 11264 bytes
constexpr uint16_t kTrackFileId = 80;
constexpr uint16_t kTrackBanner = 0xFFFF;
constexpr size_t kMinDataTrack = 2048; // smaller "track 0" entries are fake boot blocks (advertising), not disk data

uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Match-distance tables of MEDIUM and DEEP (the LZHUF d_code/d_len tables): an 8-bit prefix gives the high bits
// of the distance and how many more bits to read.
const uint8_t kDCode[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
    0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F,
    0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13,
    0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x17,
    0x18, 0x18, 0x19, 0x19, 0x1A, 0x1A, 0x1B, 0x1B, 0x1C, 0x1C, 0x1D, 0x1D, 0x1E, 0x1E, 0x1F, 0x1F,
    0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27,
    0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
};
const uint8_t kDLen[256] = {
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
};

const std::array<uint16_t, 256>& crc_table() {
    static const std::array<uint16_t, 256> table = [] {
        std::array<uint16_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint16_t c = uint16_t(i);
            for (int b = 0; b < 8; ++b) c = (c & 1) ? uint16_t((c >> 1) ^ 0xA001) : uint16_t(c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// MSB-first bit reader. Reading past the end yields zero bits (the format relies on that padding).
class Bits {
public:
    Bits(const uint8_t* d, size_t n) : d_(d), n_(n) { refill(); }
    uint32_t peek(int n) const { return n == 0 ? 0 : (buf_ >> (count_ - n)) & ((1u << n) - 1); }
    void consume(int n) {
        while (n > 0) {
            int k = std::min(n, 16);
            count_ -= k;
            buf_ &= (1u << count_) - 1;
            n -= k;
            refill();
        }
    }
    uint32_t read(int n) {
        uint32_t v = peek(n);
        consume(n);
        return v;
    }

private:
    void refill() {
        while (count_ < 16) {
            buf_ = (buf_ << 8) | (pos_ < n_ ? d_[pos_] : 0u);
            ++pos_;
            count_ += 8;
        }
    }
    const uint8_t* d_;
    size_t n_, pos_ = 0;
    uint32_t buf_ = 0;
    int count_ = 0;
};

// Second stage of most modes, and all of "simple": 0x90 is the escape (0x90 0x00 = a literal 0x90; 0x90 n v = n
// copies of v; 0x90 0xFF v hi lo = a 16-bit count). Fills exactly `out_n` bytes.
bool unpack_rle(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
    size_t i = 0, o = 0;
    auto next = [&](uint32_t& v) {
        if (i >= in_n) return false;
        v = in[i++];
        return true;
    };
    while (o < out_n) {
        uint32_t b;
        if (!next(b)) return false;
        if (b != 0x90) { out[o++] = uint8_t(b); continue; }
        uint32_t run;
        if (!next(run)) return false;
        if (run == 0) { out[o++] = 0x90; continue; }
        uint32_t value;
        if (!next(value)) return false;
        size_t count = run;
        if (run == 0xFF) {
            uint32_t hi, lo;
            if (!next(hi) || !next(lo)) return false;
            count = size_t(hi) << 8 | lo;
        }
        if (count > out_n - o) return false;
        std::memset(out + o, int(value), count);
        o += count;
    }
    return true;
}

constexpr uint16_t kQuickMask = 0xFF, kMediumMask = 0x3FFF, kDeepMask = 0x3FFF;
constexpr uint16_t kQuickInit = 251, kMediumInit = 0x3FBE, kDeepInit = 0x3FC4;
constexpr size_t kWindow = 0x4000, kWindowClear = 0x3FC8;
constexpr size_t kNC = 510, kNPT = 20; // HEAVY character/length codes, position-tree codes
constexpr size_t kDeepNChar = 256 - 2 + 60, kDeepT = kDeepNChar * 2 - 1;
constexpr uint16_t kDeepR = kDeepT - 1, kDeepMaxFreq = 0x8000;

// All the state that persists from one DMS track to the next (a track may ask to keep it).
class Decoder {
public:
    Decoder() { reset(); }

    void reset() {
        quick_pos_ = kQuickInit;
        medium_pos_ = kMediumInit;
        deep_pos_ = kDeepInit;
        heavy_pos_ = 0;
        heavy_last_ = 0;
        deep_fresh_ = true;
        std::memset(window_, 0, kWindowClear);
    }

    bool unpack_track(int mode, uint8_t flags, const uint8_t* packed, size_t packed_n, size_t intermediate_n,
                      uint8_t* out, size_t out_n) {
        switch (mode) {
        case 0:
            if (packed_n < out_n) return false;
            std::memcpy(out, packed, out_n);
            return true;
        case 1: return unpack_rle(packed, packed_n, out, out_n);
        case 2:
        case 3:
        case 4:
        case 5:
        case 6: {
            stage1_.assign(intermediate_n, 0);
            bool ok = false;
            if (mode == 2) ok = quick(packed, packed_n, stage1_.data(), intermediate_n);
            else if (mode == 3) ok = medium(packed, packed_n, stage1_.data(), intermediate_n);
            else if (mode == 4) ok = deep(packed, packed_n, stage1_.data(), intermediate_n);
            else ok = heavy(mode == 6, (flags & 2) != 0, packed, packed_n, stage1_.data(), intermediate_n);
            if (!ok) return false;
            if (mode >= 5 && !(flags & 4)) {
                if (stage1_.size() < out_n) return false;
                std::memcpy(out, stage1_.data(), out_n);
                return true;
            }
            return unpack_rle(stage1_.data(), stage1_.size(), out, out_n);
        }
        default: return false;
        }
    }

private:
    void push(uint16_t& pos, uint16_t mask, uint8_t b) { window_[pos & mask] = b; ++pos; }

    // Copies `length` bytes from `distance + 1` behind `pos` in the window to the window and the output.
    bool copy_match(uint16_t& pos, uint16_t mask, uint32_t distance, uint32_t length, uint8_t* out, size_t out_n,
                    size_t& o) {
        uint16_t src = uint16_t(pos - distance - 1);
        for (uint32_t k = 0; k < length; ++k) {
            uint8_t b = window_[src & mask];
            push(pos, mask, b);
            ++src;
            if (o >= out_n) return false;
            out[o++] = b;
        }
        return true;
    }

    bool quick(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
        Bits bits(in, in_n);
        size_t o = 0;
        while (o < out_n) {
            if (bits.read(1)) {
                uint8_t b = uint8_t(bits.read(8));
                push(quick_pos_, kQuickMask, b);
                out[o++] = b;
            } else {
                uint32_t length = bits.read(2) + 2, distance = bits.read(8);
                if (!copy_match(quick_pos_, kQuickMask, distance, length, out, out_n, o)) return false;
            }
        }
        quick_pos_ = uint16_t((quick_pos_ + 5) & kQuickMask);
        return true;
    }

    bool medium(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
        Bits bits(in, in_n);
        size_t o = 0;
        while (o < out_n) {
            if (bits.read(1)) {
                uint8_t b = uint8_t(bits.read(8));
                push(medium_pos_, kMediumMask, b);
                out[o++] = b;
            } else {
                uint32_t prefix = bits.read(8);
                uint32_t length = kDCode[prefix] + 3u;
                uint32_t extra = kDLen[prefix];
                uint32_t mid = ((prefix << extra) | bits.read(int(extra))) & 0xFF;
                extra = kDLen[mid];
                uint32_t low = ((mid << extra) | bits.read(int(extra))) & 0xFF;
                uint32_t distance = (uint32_t(kDCode[mid]) << 8) | low;
                if (!copy_match(medium_pos_, kMediumMask, distance, length, out, out_n, o)) return false;
            }
        }
        medium_pos_ = uint16_t((medium_pos_ + 66) & kMediumMask);
        return true;
    }

    // DEEP: the character/length symbol comes from an adaptive Huffman tree (Okumura's LZHUF) that is updated
    // after every symbol and rebuilt when a frequency would overflow; distances use the static tables.
    void deep_init() {
        for (size_t i = 0; i < kDeepNChar; ++i) {
            freq_[i] = 1;
            son_[i] = uint16_t(i + kDeepT);
            prnt_[i + kDeepT] = uint16_t(i);
        }
        size_t i = 0, j = kDeepNChar;
        while (j <= kDeepR) {
            freq_[j] = uint16_t(freq_[i] + freq_[i + 1]);
            son_[j] = uint16_t(i);
            prnt_[i] = prnt_[i + 1] = uint16_t(j);
            i += 2;
            ++j;
        }
        freq_[kDeepT] = 0xFFFF;
        prnt_[kDeepR] = 0;
        deep_fresh_ = false;
    }

    void deep_reconstruct() {
        size_t j = 0;
        for (size_t i = 0; i < kDeepT; ++i)
            if (son_[i] >= kDeepT) {
                freq_[j] = uint16_t(uint16_t(freq_[i] + 1) / 2);
                son_[j] = son_[i];
                ++j;
            }
        size_t i = 0;
        j = kDeepNChar;
        while (j < kDeepT) {
            uint16_t f = uint16_t(freq_[i] + freq_[i + 1]);
            freq_[j] = f;
            size_t k = j - 1;
            while (f < freq_[k]) --k;
            ++k;
            std::memmove(&freq_[k + 1], &freq_[k], (j - k) * sizeof(uint16_t));
            freq_[k] = f;
            std::memmove(&son_[k + 1], &son_[k], (j - k) * sizeof(uint16_t));
            son_[k] = uint16_t(i);
            i += 2;
            ++j;
        }
        for (size_t n = 0; n < kDeepT; ++n) {
            size_t k = son_[n];
            prnt_[k] = uint16_t(n);
            if (k < kDeepT) prnt_[k + 1] = uint16_t(n);
        }
    }

    void deep_update(uint16_t symbol) {
        if (freq_[kDeepR] == kDeepMaxFreq) deep_reconstruct();
        uint16_t c = prnt_[size_t(symbol) + kDeepT];
        do {
            uint16_t k = uint16_t(freq_[c] + 1);
            freq_[c] = k;
            uint16_t l = uint16_t(c + 1);
            if (k > freq_[l]) {
                while (k > freq_[++l]) {}
                --l;
                freq_[c] = freq_[l];
                freq_[l] = k;
                uint16_t i = son_[c];
                prnt_[i] = l;
                if (i < kDeepT) prnt_[i + 1] = l;
                uint16_t j = son_[l];
                son_[l] = i;
                prnt_[j] = c;
                if (j < kDeepT) prnt_[j + 1] = c;
                son_[c] = j;
                c = l;
            }
            c = prnt_[c];
        } while (c != 0);
    }

    bool deep(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
        Bits bits(in, in_n);
        if (deep_fresh_) deep_init();
        size_t o = 0;
        while (o < out_n) {
            uint16_t node = son_[kDeepR];
            while (node < kDeepT) node = son_[size_t(node) + bits.read(1)];
            uint16_t code = uint16_t(node - kDeepT);
            deep_update(code);
            if (code < 256) {
                push(deep_pos_, kDeepMask, uint8_t(code));
                out[o++] = uint8_t(code);
            } else {
                uint32_t length = code - 253u;
                uint32_t prefix = bits.read(8);
                uint32_t extra = kDLen[prefix];
                uint32_t low = ((prefix << extra) | bits.read(int(extra))) & 0xFF;
                uint32_t distance = (uint32_t(kDCode[prefix]) << 8) | low;
                if (!copy_match(deep_pos_, kDeepMask, distance, length, out, out_n, o)) return false;
            }
        }
        deep_pos_ = uint16_t((deep_pos_ + 60) & kDeepMask);
        return true;
    }

    // HEAVY: LZ77 with canonical Huffman codes for characters/lengths (510 symbols) and positions, the trees
    // read from the stream when the track says so (flag 2) and otherwise kept from the previous track.
    bool make_table(unsigned nchar, const uint8_t* bitlen, unsigned tablebits, uint16_t* table) {
        const unsigned table_size = 1u << tablebits;
        struct S {
            const uint8_t* blen;
            uint16_t* table;
            uint16_t* left;
            uint16_t* right;
            unsigned n, avail, table_size, bit, max_depth, depth, len, codeword;
            int c;
        } st{bitlen, table, left_, right_, nchar, nchar, table_size, table_size >> 1, tablebits + 1, 1, 1, 0, -1};
        // Walks one node of the canonical tree: fills `table` for short codes, threads left/right for long ones.
        struct Walk {
            static bool build(S& s, unsigned& node_out) {
                unsigned node = 0;
                if (s.len == s.depth) {
                    while (true) {
                        if (++s.c >= int(s.n)) break;
                        if (s.blen[s.c] == s.len) {
                            unsigned start = s.codeword;
                            s.codeword += s.bit;
                            if (s.codeword > s.table_size) return false;
                            for (unsigned k = start; k < s.codeword; ++k) s.table[k] = uint16_t(s.c);
                            node_out = unsigned(s.c);
                            return true;
                        }
                    }
                    s.c = -1;
                    ++s.len;
                    s.bit >>= 1;
                }
                ++s.depth;
                unsigned tmp;
                if (s.depth < s.max_depth) {
                    if (!build(s, tmp) || !build(s, tmp)) return false;
                } else if (s.depth > 32) {
                    return false;
                } else {
                    node = s.avail++;
                    if (node >= 2 * s.n - 1) return false;
                    unsigned l, r;
                    if (!build(s, l)) return false;
                    s.left[node] = uint16_t(l);
                    if (!build(s, r)) return false;
                    s.right[node] = uint16_t(r);
                    if (s.codeword >= s.table_size) return false;
                    if (s.depth == s.max_depth) s.table[s.codeword++] = uint16_t(node);
                }
                --s.depth;
                node_out = node;
                return true;
            }
        };
        unsigned t;
        if (!Walk::build(st, t) || !Walk::build(st, t)) return false;
        return st.codeword == table_size;
    }

    bool read_tree_c(Bits& bits) {
        uint32_t count = bits.read(9);
        if (count > 0) {
            if (count > kNC) return false;
            for (uint32_t i = 0; i < count; ++i) c_len_[i] = uint8_t(bits.read(5));
            std::memset(c_len_ + count, 0, kNC - count);
            return make_table(unsigned(kNC), c_len_, 12, c_table_);
        }
        uint32_t symbol = bits.read(9);
        if (symbol >= kNC) return false;
        std::memset(c_len_, 0, kNC);
        std::fill(c_table_, c_table_ + 4096, uint16_t(symbol));
        return true;
    }

    bool read_tree_p(Bits& bits, unsigned np) {
        uint32_t count = bits.read(5);
        if (count > 0) {
            if (count > kNPT) return false;
            for (uint32_t i = 0; i < count; ++i) pt_len_[i] = uint8_t(bits.read(4));
            std::memset(pt_len_ + count, 0, kNPT - count);
            return make_table(np, pt_len_, 8, pt_table_);
        }
        uint32_t symbol = bits.read(5);
        if (symbol >= np) return false;
        std::memset(pt_len_, 0, kNPT);
        std::fill(pt_table_, pt_table_ + 256, uint16_t(symbol));
        return true;
    }

    // One symbol through a table of `tablebits` bits, falling back to a walk of the left/right tree for longer codes.
    bool decode_symbol(Bits& bits, const uint16_t* table, int tablebits, unsigned limit, const uint8_t* len,
                       unsigned& out) {
        unsigned node = table[bits.peek(tablebits)];
        if (node < limit) {
            bits.consume(len[node]);
            out = node;
            return true;
        }
        bits.consume(tablebits);
        uint32_t path = bits.peek(16), probe = 0x8000;
        for (int steps = 0; steps < 17; ++steps) {
            if (node >= 2 * kNC) return false;
            node = (path & probe) ? right_[node] : left_[node];
            probe >>= 1;
            if (node < limit) {
                if (len[node] < tablebits) return false;
                bits.consume(len[node] - tablebits);
                out = node;
                return true;
            }
        }
        return false;
    }

    bool heavy(bool big_dict, bool rebuild, const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n) {
        const unsigned np = big_dict ? 15 : 14;
        const uint16_t mask = big_dict ? 0x1FFF : 0x0FFF;
        Bits bits(in, in_n);
        if (rebuild && (!read_tree_c(bits) || !read_tree_p(bits, np))) return false;
        size_t o = 0;
        while (o < out_n) {
            unsigned code;
            if (!decode_symbol(bits, c_table_, 12, unsigned(kNC), c_len_, code)) return false;
            if (code < 256) {
                push(heavy_pos_, mask, uint8_t(code));
                out[o++] = uint8_t(code);
                continue;
            }
            unsigned node;
            if (!decode_symbol(bits, pt_table_, 8, np, pt_len_, node)) return false;
            if (node != np - 1) {
                if (node > 0) {
                    unsigned extra = node - 1;
                    node = bits.peek(int(extra)) | (1u << extra);
                    bits.consume(int(extra));
                }
                heavy_last_ = uint16_t(node);
            }
            if (!copy_match(heavy_pos_, mask, heavy_last_, code - 253u, out, out_n, o)) return false;
        }
        return true;
    }

    uint8_t window_[kWindow] = {};
    uint16_t quick_pos_, medium_pos_, deep_pos_, heavy_pos_, heavy_last_;
    bool deep_fresh_;
    std::vector<uint8_t> stage1_;
    // HEAVY trees. The character tree's internal nodes (>= 510) and the position tree's (< 30) never overlap, so
    // both live in the same left/right arrays.
    uint8_t c_len_[kNC] = {}, pt_len_[kNPT] = {};
    uint16_t c_table_[4096] = {}, pt_table_[256] = {};
    uint16_t left_[2 * kNC] = {}, right_[2 * kNC] = {};
    // DEEP's adaptive tree.
    uint16_t freq_[kDeepT + 1] = {}, prnt_[kDeepT + kDeepNChar] = {}, son_[kDeepT + 1] = {};
};

// Banner and FILEID.DIZ text: decoded on a fresh decoder so it cannot disturb the disk tracks' shared state.
std::string decode_text(int mode, uint8_t flags, const uint8_t* packed, size_t packed_n, size_t inter_n, size_t out_n) {
    if (out_n == 0 || out_n > kMaxTrackBytes) return {};
    auto dec = std::make_unique<Decoder>();
    std::vector<uint8_t> out(out_n);
    if (!dec->unpack_track(mode, flags, packed, packed_n, inter_n, out.data(), out_n)) return {};
    size_t end = out.size();
    while (end > 0 && out[end - 1] == 0) --end;
    return std::string(out.begin(), out.begin() + long(end));
}

} // namespace

uint16_t dms_crc16(const uint8_t* d, size_t n) {
    const auto& t = crc_table();
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) crc = uint16_t(t[(crc ^ d[i]) & 0xFF] ^ (crc >> 8));
    return crc;
}

const uint8_t* dms_d_code() { return kDCode; }
const uint8_t* dms_d_len() { return kDLen; }

uint16_t dms_checksum16(const uint8_t* d, size_t n) {
    uint16_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum = uint16_t(sum + d[i]);
    return sum;
}

DmsResult read_dms(const std::vector<uint8_t>& d, DiskImage& out) {
    DmsResult r;
    out = DiskImage();
    if (d.size() < 4 || std::memcmp(d.data(), "DMS!", 4) != 0) { r.why = "not a DMS archive"; return r; }
    if (d.size() < 56) { r.status = DmsStatus::Truncated; r.why = "truncated header"; return r; }
    if (dms_crc16(&d[4], 50) != be16(&d[54])) { r.status = DmsStatus::Corrupt; r.why = "header CRC mismatch"; return r; }
    DmsInfo& in = r.info;
    in.geninfo = uint16_t(be16(&d[10]));
    in.date = be32(&d[12]);
    in.first_track = uint16_t(be16(&d[16]));
    in.last_track = uint16_t(be16(&d[18]));
    in.packed_size = be24(&d[21]);
    in.unpacked_size = be24(&d[25]);
    in.creator_version = uint16_t(be16(&d[46]));
    in.disk_type = uint16_t(be16(&d[50]));
    in.default_mode = uint16_t(be16(&d[52]));
    in.no_zero = (in.geninfo & 1) != 0;
    in.encrypted = (in.geninfo & 2) != 0;
    in.high_density = (in.geninfo & 0x10) != 0;
    if (in.encrypted) { r.status = DmsStatus::Unsupported; r.why = "encrypted DMS archives are not supported"; return r; }
    if (in.disk_type == 7) { r.status = DmsStatus::Unsupported; r.why = "an FMS file, not a disk image"; return r; }
    if (in.high_density) { r.status = DmsStatus::Unsupported; r.why = "a high-density (1.76 MB) disk; Xenon 2 discs are 880 KB"; return r; }

    auto dec = std::make_unique<Decoder>();
    std::vector<uint8_t> unpacked;
    size_t p = 56;
    while (p < d.size()) {
        const uint8_t* h = &d[p];
        if (d.size() - p < 2 || h[0] != 'T' || h[1] != 'R') break; // trailing junk after the last track is normal
        if (d.size() - p < 20) { r.status = DmsStatus::Truncated; r.why = "truncated track header"; return r; }
        if (dms_crc16(h, 18) != be16(h + 18)) { r.status = DmsStatus::Corrupt; r.why = "track header CRC mismatch"; return r; }
        uint16_t number = uint16_t(be16(h + 2));
        size_t packed_n = be16(h + 6), inter_n = be16(h + 8), unpacked_n = be16(h + 10);
        uint8_t flags = h[12], mode = h[13];
        uint16_t checksum = uint16_t(be16(h + 14)), data_crc = uint16_t(be16(h + 16));
        p += 20;
        if (packed_n > kMaxTrackBytes || inter_n > kMaxTrackBytes || unpacked_n > kMaxTrackBytes) {
            r.status = DmsStatus::Corrupt;
            r.why = "track " + std::to_string(number) + " declares an oversized length";
            return r;
        }
        if (d.size() - p < packed_n) { r.status = DmsStatus::Truncated; r.why = "truncated data of track " + std::to_string(number); return r; }
        const uint8_t* packed = &d[p];
        p += packed_n;
        if (dms_crc16(packed, packed_n) != data_crc) {
            r.status = DmsStatus::Corrupt;
            r.why = "CRC mismatch in the data of track " + std::to_string(number);
            return r;
        }
        if (number == kTrackBanner) { in.banner = decode_text(mode, flags, packed, packed_n, inter_n, unpacked_n); continue; }
        if (number == kTrackFileId) { in.file_id = decode_text(mode, flags, packed, packed_n, inter_n, unpacked_n); continue; }
        if (number >= kTrackFileId || unpacked_n <= kMinDataTrack) continue;
        if (mode > 6) {
            r.status = DmsStatus::Unsupported;
            r.why = "track " + std::to_string(number) + " uses the unknown compression mode " + std::to_string(mode);
            return r;
        }
        if (unpacked_n != kCylinderBytes) {
            r.status = DmsStatus::Unsupported;
            r.why = "track " + std::to_string(number) + " unpacks to " + std::to_string(unpacked_n) + " bytes (a DD cylinder is " +
                    std::to_string(kCylinderBytes) + ")";
            return r;
        }
        unpacked.assign(unpacked_n, 0);
        if (!dec->unpack_track(mode, flags, packed, packed_n, inter_n, unpacked.data(), unpacked_n)) {
            r.status = DmsStatus::Corrupt;
            r.why = "track " + std::to_string(number) + " does not decompress (corrupted data, compression mode " + std::to_string(mode) + ")";
            return r;
        }
        if (!(flags & 1)) dec->reset();
        if (dms_checksum16(unpacked.data(), unpacked_n) != checksum) {
            r.status = DmsStatus::Corrupt;
            r.why = "checksum mismatch after unpacking track " + std::to_string(number);
            return r;
        }
        in.modes_used |= 1 << mode;
        ++in.tracks_decoded;
        for (int side = 0; side < 2; ++side) {
            Track& tr = out.track(int(number) * 2 + side);
            tr.kind = Track::Kind::Decoded;
            tr.sectors.assign(unpacked.begin() + long(size_t(side) * kTrackBytes), unpacked.begin() + long(size_t(side + 1) * kTrackBytes));
            tr.status.fill(SectorStatus::Ok);
        }
    }
    if (in.tracks_decoded == 0) { r.status = DmsStatus::Corrupt; r.why = "no disk tracks found"; return r; }
    r.status = DmsStatus::Ok;
    return r;
}

} // namespace x2::import
