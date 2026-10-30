// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Extended ADF ("UAE-1ADF" and the older "UAE--ADF"): a disk image that can hold raw MFM tracks next to normal
// AmigaDOS ones. Layout as documented in WinUAE's disk.cpp:
//
//   UAE-1ADF   8 bytes "UAE-1ADF", W reserved, W number of tracks (normally 160), then per track
//              W reserved, W type (low byte 0 = normal AmigaDOS track, 1 = raw MFM; high byte = revolutions - 1),
//              L space for the track in bytes, L track length in bits; the tracks' data follows in order.
//   UAE--ADF   8 bytes "UAE--ADF", then 160 x (W sync word, W length in bytes); sync 0 = normal track, otherwise
//              raw MFM that follows the sync word. The data follows in order.
//
// Normal tracks become Decoded tracks. Raw tracks that hold standard Amiga sectors are decoded
// (decode_amiga_mfm_track); anything else stays Raw, and DiskImage::to_adf reports it.
#pragma once
#include "import/disk_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

// False with `why` for a truncated or inconsistent file. Sets `high_density` for a 22-sector (HD) image, which
// is not read.
bool read_ext_adf(const std::vector<uint8_t>& data, DiskImage& out, std::string& why, bool& high_density);

} // namespace x2::import
