// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// DMS ("Disk Masher System") archives: the common Amiga container for a single compressed floppy. A DMS file is
// a 56-byte header and then "tracks" (each a whole cylinder, 11264 bytes unpacked) with their own 20-byte
// headers, CRC-16 over the packed data and a 16-bit byte sum over the unpacked data. Compression modes: 0 none,
// 1 simple (RLE), 2 quick, 3 medium, 4 deep (adaptive Huffman), 5 heavy1 and 6 heavy2 (LZH, 4 and 8 KB window).
// This is our own decoder, written from the format as implemented by the public-domain xDMS (Andre Rodrigues
// de la Rocha, maintained by Heikki Orsila). Docs: docs/re-notes/dms.md.
#pragma once
#include "import/disk_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

struct DmsInfo {
    uint16_t creator_version = 0; // major * 100 + minor
    uint32_t date = 0;            // Unix time
    uint16_t first_track = 0, last_track = 0;
    uint32_t packed_size = 0, unpacked_size = 0;
    uint16_t geninfo = 0, disk_type = 0, default_mode = 0;
    bool no_zero = false, encrypted = false, high_density = false;
    int tracks_decoded = 0;
    int modes_used = 0; // bit n set: some track used compression mode n
    std::string banner, file_id;
};

enum class DmsStatus { Ok, NotDms, Truncated, Corrupt, Unsupported };

struct DmsResult {
    DmsStatus status = DmsStatus::NotDms;
    std::string why;
    DmsInfo info;
};

// Decodes a DMS archive into `out` (data tracks only; the banner and FILEID.DIZ go to info). Every track's CRC
// and checksum is verified; any mismatch is Corrupt. Unsupported: encrypted archives, high-density disks, FMS
// files, tracks of an unusual size.
DmsResult read_dms(const std::vector<uint8_t>& data, DiskImage& out);

// CRC-16/ARC (init 0) and the 16-bit byte sum DMS uses; exposed for the tests.
uint16_t dms_crc16(const uint8_t* d, size_t n);
uint16_t dms_checksum16(const uint8_t* d, size_t n);
// The static distance tables of MEDIUM and DEEP (256 entries each); the tests' encoder needs them.
const uint8_t* dms_d_code();
const uint8_t* dms_d_len();

} // namespace x2::import
