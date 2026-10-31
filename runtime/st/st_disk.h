// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A logical Atari ST floppy image: 512-byte sectors, 1 or 2 sides, tracks of 9-11 sectors, in the order the common
// ".st" file stores them (track by track, side 0 then side 1 of each track). This is the normalised form the
// importer produces (S1); until then a raw .st file can be loaded directly for development.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::st {

struct StDisk {
    int sides = 2;
    int sectors_per_track = 10;
    int tracks = 80;
    std::vector<uint8_t> data;

    size_t expected_size() const { return size_t(tracks) * sides * sectors_per_track * 512; }
    bool valid() const { return (sides == 1 || sides == 2) && sectors_per_track >= 1 && tracks >= 1 && data.size() >= expected_size(); }
    // Sector `sec` (1-based, as the FDC numbers them) of `track`/`side`; null outside the disk.
    const uint8_t* sector(int track, int side, int sec) const {
        if (track < 0 || track >= tracks || side < 0 || side >= sides || sec < 1 || sec > sectors_per_track) return nullptr;
        const size_t off = ((size_t(track) * sides + side) * sectors_per_track + size_t(sec - 1)) * 512;
        return off + 512 <= data.size() ? data.data() + off : nullptr;
    }
};

// Geometry from the file size when it is a standard floppy size (360/400/720/800/820 KB and a few 81/82-track ones), else
// from the boot sector's BPB: a BPB can claim one side on a double-sided disk. False with `why` for a size that is not a floppy.
bool make_st_disk(std::vector<uint8_t> bytes, StDisk& out, std::string& why);

// Boot sector checksum: the sum of its 256 big-endian words is $1234 for a bootable disk.
bool boot_sector_executable(const uint8_t* sector512);

} // namespace x2::st
