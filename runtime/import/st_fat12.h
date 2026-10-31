// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A read-only FAT12 reader for the logical image of an ST disk (st_disk.h: logical blocks in cylinder-major, side-interleaved
// order, which is also FAT's block order), for diagnostics and for releases that keep their files in a TOS file system (the
// ST Format cover disk). The Kixx release does not use it: its loader reads raw tracks. Bounded: the BPB must be consistent
// with the image, chains are walked with loop guards, directories nest at most four deep, and a listing holds at most 1024
// entries.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

struct FatEntry {
    std::string path;        // "X2.PRG", "AUTO/X.PRG"
    uint32_t size = 0;
    uint16_t first_cluster = 0;
    bool directory = false;
};

struct FatInfo {
    std::string label;       // the volume label from the root directory (may be empty)
    unsigned bytes_per_sector = 0, sectors_per_cluster = 0, reserved = 0, fats = 0, root_entries = 0, fat_sectors = 0;
    unsigned total_sectors = 0, clusters = 0;
    uint8_t media = 0;
    std::string oem;         // the 6-character OEM field of the boot sector, printable characters only
};

// Reads the BPB. False with the reason when the boot sector is not a usable FAT12 BPB for an image of `size` bytes.
bool fat12_info(const uint8_t* image, size_t size, FatInfo& info, std::string& why);

// Lists the files and directories (the volume label goes to info.label). False with the reason for an unusable volume;
// a damaged chain or directory is reported in `warnings` and the listing so far is kept.
bool fat12_list(const uint8_t* image, size_t size, FatInfo& info, std::vector<FatEntry>& out, std::vector<std::string>& warnings,
                std::string& why);

// The contents of one file (at most `max_bytes`). False with the reason (not found, bad chain, too large).
bool fat12_read(const uint8_t* image, size_t size, const FatEntry& entry, std::vector<uint8_t>& out, std::string& why,
                size_t max_bytes = size_t(8) << 20);

} // namespace x2::import
