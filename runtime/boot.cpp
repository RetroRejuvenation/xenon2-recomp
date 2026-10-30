// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "boot.h"

#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace x2 {

namespace {
bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}
} // namespace

bool load_game_assets(const std::string& dir, GameAssets& out, std::string& error) {
    std::string game = dir + "/game_0400.bin", disc = dir + "/disc2.bin";
    if (!read_file(game, out.game_image)) { error = "cannot read " + game + " (import your disk images: x2recomp --import PATH)"; return false; }
    if (!read_file(disc, out.disc2)) { error = "cannot read " + disc + " (import your disk images: x2recomp --import PATH)"; return false; }
    if (out.game_image.size() != kGameImageSize) { error = game + ": expected 0x3FC00 bytes"; return false; }
    // Entry is `bra.w $7032` (60 00 6C 30).
    if (std::memcmp(out.game_image.data(), "\x60\x00\x6C\x30", 4) != 0) { error = game + ": unexpected entry code"; return false; }
    if (out.disc2.size() != kDisc2Size) { error = disc + ": expected 901120 bytes"; return false; }
    if (std::memcmp(out.disc2.data() + 2, "X2B ", 4) != 0) { error = disc + ": missing \"X2B \" disc id"; return false; }
    return true;
}

void boot_game(Machine& m, const GameAssets& assets) {
    std::fill(m.chip.begin(), m.chip.end(), 0);
    m.load(kGameBase, assets.game_image.data(), assets.game_image.size());
    m.start_cpu(kGameBase, kGameInitialSsp, 0x2700);
}

} // namespace x2
