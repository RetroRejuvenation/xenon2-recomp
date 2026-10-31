// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The "Rob Northen PDos" release of Xenon 2 (WHDLoad imager floppy version v5; docs/re-notes/acquisitions-flux-acorn-st.md,
// pdos-v5.md): a one-disk release whose cylinder 0 head 0 is ordinary Amiga MFM (bootable, a custom loader) and whose tracks
// 1-159 hold twelve 512-byte sectors in the PDos format, with the game and its data as RNC-packed files. This module decodes
// the PDos tracks from raw bit streams, assembles the disk image the way the WHDLoad imager lays it out, finds and checks the
// packed files, and builds the two inputs the runtime needs. The layout follows the public imager source; nothing from the
// game is stored here.
#pragma once
#include "import/disk_image.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr int kPdosSectors = 12;                    // sectors on each of tracks 1..159
constexpr int kPdosFirstTrack = 1;
constexpr int kPdosLastTrack = 159;
constexpr size_t kPdosTrack0Bytes = 11 * 512;       // cylinder 0 head 0: eleven standard sectors
constexpr size_t kPdosTrackBytes = kPdosSectors * 512;
constexpr size_t kPdosDiskBytes = kPdosTrack0Bytes + size_t(kPdosLastTrack) * kPdosTrackBytes; // 982,528: the WHDLoad Disk.1
constexpr uint16_t kPdosBootCrc = 0xBDCE;           // CRC16/ARC of the 1 KB boot block, the imager's identifier for v5

// Decodes the PDos sectors of one raw track: tries the track's own bit stream and then its alternates (Track::alt), and
// accepts a sector only when its number, track and checksum all check out. `have[s]` says which sectors were found.
// Returns the number found.
int pdos_decode_track(const Track& t, int track_number, std::array<std::array<uint8_t, 512>, kPdosSectors>& sectors,
                      std::array<bool, kPdosSectors>& have);

struct PdosDisk {
    std::vector<uint8_t> image;                 // kPdosDiskBytes, unreadable sectors left as zeros
    std::vector<std::pair<int, int>> missing;   // (track, sector) of each sector that could not be read
};

// Assembles the disk from an image whose track 0 is standard and whose tracks 1..159 are raw. False with the reason if track
// 0 is not readable or a track is not a raw track at all; sectors that fail their checksums are listed in `out.missing`, not
// an error here.
bool pdos_assemble(const DiskImage& img, PdosDisk& out, std::string& why);

// True if `image` starts with the PDos release's boot block (CRC16/ARC 0xBDCE).
bool pdos_is_v5_boot(const uint8_t* image, size_t size);

// The packed files of the release, found through the file table in track 0 (11 entries of 8 bytes at 0x87C: first sector in
// the game's numbering (image sector + 1), sector count, offset of the stream in its first sector, a trailing offset).
struct PdosFile {
    int index = 0;
    size_t image_offset = 0;     // where the "RNC\1" header lies in the disk image
    uint32_t packed = 0, unpacked = 0;
    bool ok = false;             // both CRC16s match
    std::string why;             // the reason when not ok
    std::vector<std::pair<int, int>> damaged; // unreadable (track, sector) that lie inside the stream
};
constexpr size_t kPdosFileTableOffset = 0x87C;
constexpr int kPdosFileCount = 11;
constexpr size_t kPdosStage2Offset = 0x58806;        // the game proper, unpacked to $400 (the intro in front of it is not needed)

struct PdosBuild {
    std::vector<uint8_t> game_image;   // $3FC00 bytes: the game at $400 followed by zeros
    std::vector<uint8_t> disc;         // kPdosDiskBytes: the disk the game's own loader reads
    std::vector<PdosFile> files;       // the eleven packed files
    int files_ok = 0;
    bool game_ok = false;
};

// Checks the boot block, the file table, every packed file and the game stream. `build` is filled as far as it goes; the
// result is usable (true) only when the game stream and all eleven files verify. When it is not, `why` names what failed
// and `build.files` says which files are damaged and which sectors lie inside them.
bool pdos_build(const PdosDisk& disk, PdosBuild& build, std::string& why);

// The same, for a disk image already assembled (the WHDLoad Disk.1 is exactly this).
bool pdos_build_from_image(const std::vector<uint8_t>& image, const std::vector<std::pair<int, int>>& missing, PdosBuild& build,
                           std::string& why);

} // namespace x2::import
