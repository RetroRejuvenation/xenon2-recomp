// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Atari ST floppy disk images (docs/re-notes/atari-st-importer.md): the three containers the Xenon 2 ST releases come in,
// decoded to one in-memory form, StDisk. The Amiga DiskImage (disk_image.h) is a fixed 11 sectors of 512 bytes per track;
// an ST disk has 9 to 11 sectors per track on 1 or 2 sides, sector ids that need not be in order, and (Pasti) per-sector
// controller status, so it has its own geometry-carrying type and the Amiga paths stay as they are.
//
//   .st   raw sectors, track-interleaved (cylinder 0 side 0, cylinder 0 side 1, cylinder 1 side 0, ...); the geometry
//         comes from the BPB when it is consistent with the size, else from the size
//   .msa  Magic Shadow Archiver: a 10-byte header and per-track blocks, raw or run-length coded (marker $E5)
//   .stx  Pasti: track records with sector descriptors (id field, WD1772 status, bit position, read time), fuzzy masks
//         and sector data; keeps what a plain image cannot: CRC-error sectors, odd sector counts, weak bits
//
// Every reader is bounds-checked and capped (tracks, sectors per track, bytes per disk), and an STX whose descriptors
// point many sectors at the same bytes is charged for every copy, so a crafted file cannot make the importer allocate
// without bound.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr int kStMaxCylinders = 86;
constexpr int kStMaxSides = 2;
constexpr size_t kStMaxSectorsPerTrack = 64;
constexpr size_t kStMaxDiskBytes = size_t(8) << 20; // sector data held for one disk
constexpr size_t kStSectorBytes = 512;

struct StSector {
    uint8_t cylinder = 0, side = 0, number = 0, size_code = 2; // the id field as written on the disk
    uint8_t fdc = 0;                  // WD1772 status (Pasti): 0x08 CRC error, 0x10 record not found
    uint16_t crc = 0;                 // the id/data CRC as recorded (STX only)
    uint16_t bit_position = 0, read_time = 0; // STX only: where the sector starts on the track, and how long it took to read
    std::vector<uint8_t> data;        // 128 << size_code bytes, empty when the record was not found
    size_t size() const { return size_t(128) << size_code; }
    bool crc_error() const { return (fdc & 0x08) != 0; }
    bool not_found() const { return (fdc & 0x10) != 0; }
    bool intact() const { return !(fdc & 0x18) && data.size() == size(); }
};

struct StTrack {
    std::vector<StSector> sectors;
    uint16_t flags = 0;        // STX track record flags, as stored
    uint32_t fuzzy_bytes = 0;  // STX: length of the weak-bit mask stored with the track
};

struct StDisk {
    enum class Format { St, Msa, Stx };
    Format format = Format::St;
    int cylinders = 0, sides = 1;
    int stx_version = 0;
    // tracks[cylinder * sides + side]
    std::vector<StTrack> tracks;
    StTrack& track(int cyl, int side) { return tracks[size_t(cyl * sides + side)]; }
    const StTrack& track(int cyl, int side) const { return tracks[size_t(cyl * sides + side)]; }

    // The most common number of 512-byte sectors on a track (what a plain image would call sectors per track).
    int sectors_per_track() const;

    // The logical image: cylinder-major, sides interleaved, sector ids 1..spt of 512 bytes (the layout of a plain .st and
    // of FAT's logical blocks). Sectors that are missing, not found or have the wrong size are zero-filled and named in
    // `problems`; a sector with a CRC error keeps its data and is also named. Returns true when there are no problems.
    bool logical_image(int spt, std::vector<uint8_t>& out, std::vector<std::string>& problems) const;

    // Tracks that are not `spt` intact 512-byte sectors numbered 1..spt (STX copy-protection candidates), as
    // "cylinder 0 side 0: sector 6 CRC error" lines, at most `limit` of them; `total` (if given) gets the full count.
    std::vector<std::string> nonstandard_tracks(int spt, size_t limit, size_t* total = nullptr) const;
};

enum class StContainer { None, St, Msa, Stx };

// Recognises an ST disk image by its structure, never by its name: STX by its "RSY\0" header, MSA by $0E0F and a consistent
// header, a plain .st by a size that matches a standard geometry together with a plausible boot sector/BPB.
StContainer detect_st_container(const uint8_t* data, size_t size);
const char* st_container_name(StContainer c);

// Decoders. False with the reason (nothing partial is returned). `max_bytes` bounds the sector data held.
bool read_st_image(const uint8_t* data, size_t size, StDisk& out, std::string& why);
bool read_msa(const uint8_t* data, size_t size, StDisk& out, std::string& why, size_t max_bytes = kStMaxDiskBytes);
bool read_stx(const uint8_t* data, size_t size, StDisk& out, std::string& why, size_t max_bytes = kStMaxDiskBytes);

// Geometry of a plain .st image of `size` bytes (BPB first, then the standard sizes). False when neither fits.
bool st_geometry(const uint8_t* data, size_t size, int& cylinders, int& sides, int& spt);

// The 16-bit big-endian word sum of a boot sector: 0x1234 means the sector is executable.
uint16_t st_boot_checksum(const uint8_t* sector512);

} // namespace x2::import
