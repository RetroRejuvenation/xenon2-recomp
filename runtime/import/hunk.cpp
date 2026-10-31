// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/hunk.h"

#include <cstdio>

namespace x2::import {

namespace {

constexpr uint32_t kHunkNameBlock = 0x3E8, kHunkCodeBlock = 0x3E9, kHunkDataBlock = 0x3EA, kHunkBssBlock = 0x3EB,
                   kHunkReloc32Block = 0x3EC, kHunkSymbolBlock = 0x3F0, kHunkDebugBlock = 0x3F1, kHunkEndBlock = 0x3F2,
                   kHunkHeaderBlock = 0x3F3;

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%X", v);
    return b;
}

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool u32(uint32_t& v) {
        if (n_ - pos_ < 4 || pos_ > n_) return false;
        v = uint32_t(d_[pos_]) << 24 | uint32_t(d_[pos_ + 1]) << 16 | uint32_t(d_[pos_ + 2]) << 8 | d_[pos_ + 3];
        pos_ += 4;
        return true;
    }
    // Skips `longs` longwords (false if they are not all there).
    bool skip_longs(uint64_t longs) {
        uint64_t bytes = longs * 4;
        if (bytes > n_ - pos_) return false;
        pos_ += size_t(bytes);
        return true;
    }
    bool bytes(uint64_t count, std::vector<uint8_t>& out) {
        if (count > n_ - pos_) return false;
        out.assign(d_ + pos_, d_ + pos_ + size_t(count));
        pos_ += size_t(count);
        return true;
    }
    size_t left() const { return n_ - pos_; }

private:
    const uint8_t* d_;
    size_t n_, pos_ = 0;
};

} // namespace

bool parse_hunk_file(const uint8_t* d, size_t size, HunkFile& out, std::string& why) {
    out = HunkFile{};
    Reader r(d, size);
    uint32_t id = 0;
    if (!d || !r.u32(id) || id != kHunkHeaderBlock) { why = "not an AmigaDOS load file (no HUNK_HEADER)"; return false; }
    // Resident library names: a list of (length in longwords, name) ended by a zero length. A game has none.
    uint32_t names = 0;
    if (!r.u32(names)) { why = "truncated hunk header"; return false; }
    if (names != 0) { why = "resident libraries in the hunk header are not supported"; return false; }
    uint32_t table = 0, first = 0, last = 0;
    if (!r.u32(table) || !r.u32(first) || !r.u32(last)) { why = "truncated hunk header"; return false; }
    if (table == 0 || table > kHunkMaxHunks) { why = "hunk table size " + std::to_string(table) + " is out of range"; return false; }
    if (first != 0 || last != table - 1) { why = "partial hunk tables (overlays) are not supported"; return false; }
    out.hunks.resize(table);
    uint64_t total = 0;
    for (uint32_t i = 0; i < table; ++i) {
        uint32_t s = 0;
        if (!r.u32(s)) { why = "truncated hunk size table"; return false; }
        uint32_t flags = s >> 30;
        if (flags == 3) { why = "hunk " + std::to_string(i) + ": extended memory flags are not supported"; return false; }
        uint64_t bytes = uint64_t(s & 0x3FFFFFFFu) * 4;
        if (bytes > kHunkMaxBytes) { why = "hunk " + std::to_string(i) + " is larger than " + std::to_string(kHunkMaxBytes >> 20) + " MB"; return false; }
        total += bytes;
        if (total > kHunkMaxTotalBytes) { why = "the hunks together are larger than " + std::to_string(kHunkMaxTotalBytes >> 20) + " MB"; return false; }
        out.hunks[i].mem_bytes = uint32_t(bytes);
        out.hunks[i].mem_flags = flags;
    }
    uint32_t started = 0, ended = 0;
    bool open = false; // between a CODE/DATA/BSS block and its HUNK_END
    while (ended < table) {
        uint32_t raw = 0;
        if (!r.u32(raw)) { why = "truncated: the file ends before hunk " + std::to_string(ended) + " is complete"; return false; }
        id = raw & 0x3FFFFFFFu; // the top two bits are memory flags that the table already gave
        switch (id) {
        case kHunkCodeBlock:
        case kHunkDataBlock:
        case kHunkBssBlock: {
            if (open) { why = "hunk " + std::to_string(started - 1) + " has no HUNK_END"; return false; }
            if (started >= table) { why = "more hunks than the table lists"; return false; }
            Hunk& h = out.hunks[started];
            uint32_t longs = 0;
            if (!r.u32(longs)) { why = "truncated hunk block"; return false; }
            uint64_t bytes = uint64_t(longs & 0x3FFFFFFFu) * 4;
            if (bytes > h.mem_bytes) { why = "hunk " + std::to_string(started) + ": block longer than its allocation"; return false; }
            h.kind = id == kHunkCodeBlock ? Hunk::Kind::Code : id == kHunkDataBlock ? Hunk::Kind::Data : Hunk::Kind::Bss;
            if (id != kHunkBssBlock && !r.bytes(bytes, h.data)) { why = "hunk " + std::to_string(started) + " is truncated"; return false; }
            ++started;
            open = true;
            break;
        }
        case kHunkReloc32Block: {
            if (!open) { why = "HUNK_RELOC32 outside a hunk"; return false; }
            Hunk& h = out.hunks[started - 1];
            if (h.kind == Hunk::Kind::Bss) { why = "relocations in a BSS hunk"; return false; }
            for (;;) {
                uint32_t count = 0, target = 0;
                if (!r.u32(count)) { why = "truncated relocation block"; return false; }
                if (count == 0) break;
                if (!r.u32(target)) { why = "truncated relocation block"; return false; }
                if (target >= table) { why = "relocation to hunk " + std::to_string(target) + ", which does not exist"; return false; }
                if (count > h.mem_bytes / 4 || uint64_t(count) * 4 > r.left()) { why = "relocation block longer than its hunk or the file"; return false; }
                HunkReloc rel;
                rel.target = target;
                rel.offsets.resize(count);
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t off = 0;
                    if (!r.u32(off)) { why = "truncated relocation block"; return false; }
                    if (uint64_t(off) + 4 > h.mem_bytes) { why = "relocation at offset " + hex(off) + " lies outside hunk " + std::to_string(started - 1); return false; }
                    rel.offsets[i] = off;
                }
                out.reloc_total += count;
                h.relocs.push_back(std::move(rel));
            }
            break;
        }
        case kHunkNameBlock:
        case kHunkDebugBlock: {
            uint32_t longs = 0;
            if (!r.u32(longs) || !r.skip_longs(longs)) { why = "truncated name or debug block"; return false; }
            break;
        }
        case kHunkSymbolBlock: {
            for (int guard = 0;; ++guard) {
                uint32_t len = 0;
                if (!r.u32(len)) { why = "truncated symbol block"; return false; }
                len &= 0x00FFFFFFu;
                if (len == 0) break;
                if (guard > 1 << 20 || !r.skip_longs(uint64_t(len) + 1)) { why = "truncated symbol block"; return false; }
            }
            break;
        }
        case kHunkEndBlock:
            if (!open) { why = "HUNK_END outside a hunk"; return false; }
            open = false;
            ++ended;
            break;
        default:
            why = "unsupported hunk block type " + hex(id) + " (only CODE, DATA, BSS, RELOC32 and END are handled)";
            return false;
        }
    }
    return true;
}

bool link_hunk_file(const HunkFile& file, uint32_t base, uint32_t limit, LinkedImage& out, std::string& why) {
    out = LinkedImage{};
    if (base % 4) { why = "link base " + hex(base) + " is not longword aligned"; return false; }
    uint64_t total = 0;
    for (const Hunk& h : file.hunks) total += h.mem_bytes;
    if (file.hunks.empty() || uint64_t(base) + total > limit) {
        why = "the hunks (" + std::to_string(total) + " bytes at " + hex(base) + ") do not fit below " + hex(limit);
        return false;
    }
    out.base = base;
    out.image.assign(size_t(total), 0);
    uint32_t at = base;
    for (const Hunk& h : file.hunks) {
        out.hunk_base.push_back(at);
        out.hunk_bytes.push_back(h.mem_bytes);
        const size_t pos = at - base;
        for (size_t i = 0; i < h.data.size(); ++i) out.image[pos + i] = h.data[i];
        at += h.mem_bytes;
    }
    out.reloc_count.assign(file.hunks.size(), 0);
    out.target_count.assign(file.hunks.size(), 0);
    for (size_t hi = 0; hi < file.hunks.size(); ++hi) {
        const size_t pos = out.hunk_base[hi] - base;
        for (const HunkReloc& rel : file.hunks[hi].relocs) {
            for (uint32_t off : rel.offsets) {
                if (uint64_t(off) + 4 > file.hunks[hi].mem_bytes) { why = "relocation outside its hunk"; return false; }
                uint8_t* p = &out.image[pos + off];
                uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
                const uint32_t low = v & 0x00FFFFFFu;
                if (low > out.hunk_bytes[rel.target]) {
                    why = "relocated value " + hex(v) + " at hunk " + std::to_string(hi) + "+" + hex(off) + " lies beyond hunk " +
                          std::to_string(rel.target) + " (" + std::to_string(out.hunk_bytes[rel.target]) + " bytes)";
                    return false;
                }
                if (low == out.hunk_bytes[rel.target]) ++out.pointers_to_end;
                if (v >> 24) ++out.high_byte_values;
                if (off & 1) ++out.odd_offsets;
                v += out.hunk_base[rel.target];
                p[0] = uint8_t(v >> 24);
                p[1] = uint8_t(v >> 16);
                p[2] = uint8_t(v >> 8);
                p[3] = uint8_t(v);
                ++out.reloc_count[hi];
                ++out.target_count[rel.target];
                ++out.reloc_total;
            }
        }
    }
    return true;
}

} // namespace x2::import
