// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The one-disk "The Power Pack" release (SPS 2234; imager floppy version v6): standard track 0, a Speedlock key track on
// track 1 and custom data on tracks 2-159, one raw MFM block per track. This module decodes the 157 data tracks (3..159),
// decrunches the main game image and the six game files, and builds the two inputs the runtime needs
// (docs/re-notes/single-disk-variants.md, build-variants.md). The layout follows the public WHDLoad imager source for
// Xenon 2; nothing from the game is stored here.
#pragma once
#include "import/disk_image.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr int kPowerPackFirstDataTrack = 3;
constexpr int kPowerPackDataTracks = 157;                // tracks 3..159
constexpr size_t kPowerPackTrackBytes = 0x1B00;
constexpr size_t kPowerPackDataSize = size_t(kPowerPackDataTracks) * kPowerPackTrackBytes; // 1,085,184: the WHDLoad Disk.1

// One data track (raw MFM) -> kPowerPackTrackBytes bytes. False when the track is not a raw track with a sync word, or its
// checksum fails (`why` says which).
bool speedlock_decode_track(const Track& t, uint8_t* out, std::string& why);

// The data tracks 3..159 of an image, concatenated. False with the reason (the first track that does not decode).
bool powerpack_data_from_image(const DiskImage& img, std::vector<uint8_t>& data, std::string& why);

struct PowerPackBuild {
    std::vector<uint8_t> game_image; // $3FC00 bytes: the entry stub, zeros and the main image at $E0C
    std::vector<uint8_t> disc2;      // 901120 bytes: the disc id, then the six level modules at their start sectors
    int files = 0;                   // game files rebuilt (six for an intact disk)
};

// `data` is the 157 data tracks (kPowerPackDataSize bytes). False with the reason if any stage does not check out: the
// main image's stream is not consumed exactly, the file table is damaged, or a game file does not decrunch intact.
bool powerpack_build(const std::vector<uint8_t>& data, PowerPackBuild& out, std::string& why);

} // namespace x2::import
