// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// WHDLoad slave and retail disk 1 identification; see whdload.h.
#include "import/whdload.h"

#include "import/lha.h"

#include <cstring>

namespace x2::import {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

constexpr size_t kAdfSize = 1760 * 512;
constexpr size_t kPowerPackDisk1Size = 1085184; // the Power Pack's custom WHDLoad `Disk.1`

// Boot-block CRCs of the six floppy versions (the install's imager keys on them; see docs/re-notes/sources.md).
const RetailDisk1 kRetail[] = {
    {0x7f8f, "v1", "retail two-disk release, second pressing (SPS 2874 [a])", true},
    {0xc0fb, "v2", "retail two-disk release as on the Bombuzal + Xenon 2 compilation disk 1 (SPS 1762)", true},
    {0x7ea7, "v3", "retail two-disk release, first pressing (SPS 0297)", true},
    {0x7e53, "v4", "one-disk budget release with standard tracks", false},
    {0xbdce, "v5", "one-disk budget release (Rob Northen PDos)", false},
    {0x6827, "v6", "one-disk The Power Pack release (Speedlock)", false},
};

bool readable(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != '\n' && p[i] != '\t' && (p[i] < 0x20 || p[i] > 0x7E)) return false;
    return true;
}

std::string string_at(const uint8_t* code, size_t code_len, size_t off) {
    if (off == 0 || off >= code_len) return {};
    size_t end = off;
    while (end < code_len && code[end] && end - off < 160) ++end;
    if (end >= code_len || end - off >= 160 || !readable(code + off, end - off)) return {};
    return std::string(reinterpret_cast<const char*>(code + off), end - off);
}

} // namespace

bool parse_whdload_slave(const uint8_t* d, size_t n, WhdSlave& out) {
    // HUNK_HEADER, no resident libraries, table size, first and last hunk, hunk sizes, then HUNK_CODE.
    if (n < 40 || be32(d) != 0x3F3 || be32(d + 4) != 0) return false;
    uint32_t first = be32(d + 12), last = be32(d + 16);
    if (last < first || last - first > 16) return false;
    size_t p = 20 + 4 * size_t(last - first + 1);
    if (p + 8 > n || (be32(d + p) & 0x3FFFFFFF) != 0x3E9) return false;
    size_t code_len = size_t(be32(d + p + 4) & 0x3FFFFFFF) * 4;
    const uint8_t* code = d + p + 8;
    if (code_len > n - (p + 8) || code_len < 44) return false;
    // ws_Security `moveq #-1,d0 ; rts`, then ws_ID "WHDLOADS".
    if (be16(code) != 0x70FF || be16(code + 2) != 0x4E75 || std::memcmp(code + 4, "WHDLOADS", 8) != 0) return false;
    out.version = be16(code + 12);
    out.flags = be16(code + 14);
    if (out.version >= 10) {
        out.name = string_at(code, code_len, be16(code + 36));
        out.copyright = string_at(code, code_len, be16(code + 38));
        out.info = string_at(code, code_len, be16(code + 40));
    }
    return true;
}

const RetailDisk1* identify_retail_disk1(const uint8_t* d, size_t n) {
    if (n != kAdfSize && n != kPowerPackDisk1Size) return nullptr;
    uint16_t crc = lha_crc16(d, 1024); // CRC-16/ARC, the same polynomial the imager uses
    for (const RetailDisk1& r : kRetail)
        if (r.boot_crc == crc) return &r;
    return nullptr;
}

} // namespace x2::import
