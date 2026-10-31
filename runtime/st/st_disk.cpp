// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/st_disk.h"

namespace x2::st {

namespace {
unsigned le16(const uint8_t* p) { return unsigned(p[0]) | unsigned(p[1]) << 8; }
} // namespace

bool boot_sector_executable(const uint8_t* s) {
    unsigned sum = 0;
    for (int i = 0; i < 256; ++i) sum += (unsigned(s[2 * i]) << 8) | s[2 * i + 1];
    return (sum & 0xFFFF) == 0x1234;
}

bool make_st_disk(std::vector<uint8_t> bytes, StDisk& out, std::string& why) {
    if (bytes.size() < 512 * 9) { why = "too small to be a floppy disk image"; return false; }
    StDisk d;
    // The file size decides the geometry when it is a standard one: a single-sided BPB on a double-sided disk is common
    // (Kixx's says one side, and the second side holds the level data), so the BPB cannot be trusted over the size.
    struct Geo { size_t size; int sides, spt, tracks; };
    static const Geo geos[] = {
        {368640, 1, 9, 80}, {409600, 1, 10, 80}, {414720, 1, 10, 81}, {737280, 2, 9, 80}, {746496, 2, 9, 81},
        {819200, 2, 10, 80}, {829440, 2, 10, 81}, {839680, 2, 10, 82}, {901120, 2, 11, 80},
    };
    bool found = false;
    for (const Geo& g : geos)
        if (bytes.size() == g.size) { d.sides = g.sides; d.sectors_per_track = g.spt; d.tracks = g.tracks; found = true; break; }
    if (!found) { // not a standard size: the BPB of a formatted disk (sectors per track at $18, sides at $1A, total at $13)
        const unsigned spt = le16(&bytes[0x18]), sides = le16(&bytes[0x1A]), total = le16(&bytes[0x13]), bps = le16(&bytes[0x0B]);
        if (bps == 512 && (sides == 1 || sides == 2) && spt >= 9 && spt <= 11 && total && total % (spt * sides) == 0 &&
            size_t(total) * 512 <= bytes.size() && total / (spt * sides) >= 40 && total / (spt * sides) <= 86) {
            d.sides = int(sides);
            d.sectors_per_track = int(spt);
            d.tracks = int(total / (spt * sides));
            found = true;
        }
    }
    if (!found) { why = "not a standard floppy size and no usable BPB"; return false; }
    d.data = std::move(bytes);
    if (!d.valid()) { why = "the image is shorter than its geometry"; return false; }
    out = std::move(d);
    return true;
}

} // namespace x2::st
