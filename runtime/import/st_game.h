// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Import of the Atari ST releases of Xenon 2 Megablast (docs/re-notes/atari-st-importer.md; plan: .omc/plans/xenon2-atari-st-target-plan.md,
// phase S1). The ST game does not use TOS after the boot sector: its loader reads raw WD1772 sectors, so the runtime (a later
// phase) boots the real loader on an emulated disk controller. The importer therefore does not pre-link anything; it validates
// the user's disk image, normalises it to one logical image, and records what it found.
//
// Input: .st, .msa or Pasti .stx files, also from a ZIP or a folder (one level), in any number; a disk is recognised by the
// SHA-1 of its logical image (variants.h: st_releases), so the three containers of one disk agree. Only the unprotected Kixx
// re-release is importable; the Copylock ST releases (Image Works, Mirror Image) and the ST Format preview are recognised and
// refused with the reason.
//
// Output (write_st_assets), in its own folder (<data dir>/assets-st by default):
//   disk.bin        the logical image: cylinder-major, sides interleaved, `sectors_per_track` sectors of 512 bytes per track
//                   (the layout of a plain .st; FAT's logical block order); 819,200 bytes for Kixx
//   manifest.txt    importer version, build=atari-st-kixx, game=st, the geometry, the boot sector checksum, the source and image
//                   hashes and an index of the RNC ProPack blocks found on the disk; written last, so a folder without one is
//                   incomplete
// There is no game_0400.bin and no disc2.bin. Manifest keys the ST machine reads: build, game, container, cylinders, sides,
// sectors_per_track, sector_bytes, layout, disk.bin_sha1, disk.bin_size, boot_checksum, bootable.
#pragma once
#include "import/importer.h"
#include "import/st_disk.h"
#include "import/variants.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr const char* kStBuildPrefix = "atari-st";
constexpr const char* kStGameTag = "st";

// One RNC ProPack method 1 block found on the disk (its header, both CRC16s and the bit stream all check out).
struct StRncStream {
    int side = 0;
    int cylinder = 0;
    size_t track_offset = 0; // bytes into the track (spt * 512 per track, one side)
    size_t side_offset = 0;  // bytes into the side's logical stream (cylinder-major)
    uint32_t packed = 0, unpacked = 0;
    uint16_t packed_crc = 0, unpacked_crc = 0;
};

// What the importer found out about one disk image (also the body of --diagnose-image).
struct StDiskReport {
    std::string name;        // input name as given (with " > member" for archive members)
    StContainer container = StContainer::None;
    int cylinders = 0, sides = 0, sectors_per_track = 0;
    std::string logical_sha1;      // of the logical image
    uint16_t boot_checksum = 0;    // 0x1234: an executable boot sector
    bool executable_boot = false;
    std::string oem;               // the boot sector's OEM field, printable characters
    std::vector<std::string> problems;   // sectors missing, not found or with CRC errors (capped)
    std::vector<std::string> findings;   // copy-protection findings: Copylock ST key sector, weak bits, a slow sector
    std::vector<std::string> nonstandard; // tracks that are not clean 512-byte sectors (capped)
    size_t nonstandard_total = 0;
    std::vector<std::string> files;      // FAT12 root listing (capped), empty when the disk has no FAT12 volume
    std::string fat_note;                // why there is no listing, or the volume label
    std::vector<StRncStream> rnc;
    const StRelease* release = nullptr;
};

struct StGame {
    Status status = Status::BadInput;
    std::vector<Message> messages;
    std::vector<std::string> sources;  // the inputs, as given (absolute paths for the automatic re-import)
    StDiskReport disk;                 // the disk that is imported (when status == Ok)
    std::vector<StDiskReport> others;  // every other ST disk found
    std::vector<uint8_t> image;        // disk.bin
    std::string image_sha1, source_sha1;
};

struct StInput {
    std::string name;
    std::vector<uint8_t> data;
    bool in_folder = false;
};

// True if any of the paths is an ST disk image (a file, a ZIP member or a file of a folder, by content).
bool looks_like_st_image(const std::vector<std::string>& paths);

// Reads the files and imports. Never writes anything.
StGame import_st_paths(const std::vector<std::string>& paths);
StGame import_st_inputs(const std::vector<StInput>& inputs);

// Reports on every ST disk in `paths` without importing (diagnose). Empty when `paths` hold none.
std::vector<StDiskReport> diagnose_st_inputs(const std::vector<StInput>& inputs, std::vector<Message>& messages);
// `x2recomp --diagnose-image` lines for a disk report (no game bytes).
std::vector<std::string> format_st_report(const StDiskReport& r);
// For a path: false when it holds no ST disk image (the lines are then empty).
bool diagnose_st(const std::string& path, std::vector<std::string>& lines);

// Writes disk.bin and manifest.txt into `dir` (created if needed; files of another build's import in that folder are removed).
bool write_st_assets(const std::string& dir, const StGame& g, std::string& error);
// verify_assets for a manifest that says build=atari-st*: disk.bin must match its recorded size and SHA-1.
bool verify_st_assets(const std::string& dir, const std::string& manifest_text, std::string& reason);
bool manifest_is_st(const std::string& manifest_text);

// `x2recomp --import` for ST disk images: exit code 0 imported, 1 cannot write, 2 bad input, 3 not an importable ST release.
int import_st_cli(const std::vector<std::string>& paths, const std::string& assets_dir);

} // namespace x2::import
