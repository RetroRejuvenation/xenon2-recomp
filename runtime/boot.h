// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Boot of the resident game image (bypasses the cracker intro, the depacker and the trainer stub).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x2 {

class Machine;

struct GameAssets {
    std::vector<uint8_t> game_image; // run-time image, $3FC00 bytes, lives at $400
    std::vector<uint8_t> disc2;      // flat 1760-sector disc 2 (901120 bytes)
};

constexpr uint32_t kGameBase = 0x400;
constexpr uint32_t kGameImageSize = 0x3FC00;
constexpr uint32_t kDisc2Size = 1760 * 512;
constexpr uint32_t kGameInitialSsp = 0x435FC; // $763C sets the same value

// Reads <dir>/game_0400.bin and <dir>/disc2.bin (written by tools/prep_assets.py). Returns false with a
// message if either file is missing or malformed.
bool load_game_assets(const std::string& dir, GameAssets& out, std::string& error);

// Place the image at $400 and start the CPU where the crack stub's `jmp $400` would: supervisor mode,
// SR=$2700, everything else in chip RAM zero.
void boot_game(Machine& m, const GameAssets& assets);

} // namespace x2
