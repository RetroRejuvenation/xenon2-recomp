// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "boot.h"

#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace x2 {

const BuildInfo kBuildCanonical{"canonical", 0, 0x7032, 0x921A, 0x20C2, 0x78CE, 0x8FDE, 0x5D00, 0x891E, 0x8B1A, true};
// The Power Pack is the first retail pressing's layout with its own start-up: no disc-id check ($8FD2 and $8FDE are
// patched out of the loader call), everything else at the canonical addresses.
const BuildInfo kBuildPowerPack{"power-pack-2234", 0x762E, 0x7032, 0x921A, 0x20C2, 0x78CE, 0, 0x5D00, 0x891E, 0x8B1A, true};
// A different compile of the game: the code after $D000 is shifted, so every address below moved.
const BuildInfo kBuild1762{"compilation-1762", 0, 0x703C, 0x9012, 0x20D0, 0x72FE, 0x8DD6, 0x5D0A, 0x8340, 0x853C, false};

namespace {
const BuildInfo* const kBuilds[] = {&kBuildCanonical, &kBuildPowerPack, &kBuild1762};

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

// The `build=` line of manifest.txt, if the file and the line exist.
std::string manifest_build(const std::string& dir) {
    std::string text;
    {
        std::vector<uint8_t> raw;
        if (!read_file(dir + "/manifest.txt", raw)) return {};
        text.assign(raw.begin(), raw.end());
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, 6, "build=") == 0) return line.substr(6);
    }
    return {};
}
} // namespace

const BuildInfo* find_build(const std::string& id) {
    for (const BuildInfo* b : kBuilds)
        if (id == b->id) return b;
    return nullptr;
}

const BuildInfo* detect_build(const uint8_t* image, size_t size) {
    if (!image || size != kGameImageSize) return nullptr;
    auto at = [&](uint32_t addr) { return image + (addr - kGameBase); };
    static const uint8_t kReset[] = {0x46, 0xFC, 0x27, 0x00, 0x60, 0x00}; // move #$2700,sr ; bra.w
    static const uint8_t kReset1762[] = {0x46, 0xFC, 0x27, 0x00, 0x4F, 0xF9}; // move #$2700,sr ; lea x.l,a7
    if (std::memcmp(image, "\x60\x00\x6C\x30", 4) == 0) {
        if (std::memcmp(at(0x7032), kReset, sizeof kReset) == 0) return &kBuildCanonical;
        // The Power Pack's own start-up at $762E: move.l #key,d0 ; moveq #1,d1 ; move.l d0,$24.w ; add.w d0,d1 ;
        // lea $435FC.l,a7. The immediate (the Copylock key the original loader passes on) is not compared.
        static const uint8_t kPowerPack[] = {0x20, 0x3C, 0, 0, 0, 0, 0x72, 0x01, 0x21, 0xC0, 0x00, 0x24,
                                              0xD2, 0x40, 0x4F, 0xF9, 0x00, 0x04, 0x35, 0xFC};
        const uint8_t* p = at(0x762E);
        for (size_t i = 0; i < sizeof kPowerPack; ++i)
            if ((i < 2 || i > 5) && p[i] != kPowerPack[i]) return nullptr;
        return &kBuildPowerPack;
    }
    if (std::memcmp(image, "\x60\x00\x6C\x3A", 4) == 0 && std::memcmp(at(0x703C), kReset1762, sizeof kReset1762) == 0)
        return &kBuild1762;
    return nullptr;
}

bool load_game_assets(const std::string& dir, GameAssets& out, std::string& error) {
    std::string game = dir + "/game_0400.bin", disc = dir + "/disc2.bin";
    if (!read_file(game, out.game_image)) { error = "cannot read " + game + " (import your disk images: x2recomp --import PATH)"; return false; }
    if (!read_file(disc, out.disc2)) { error = "cannot read " + disc + " (import your disk images: x2recomp --import PATH)"; return false; }
    if (out.game_image.size() != kGameImageSize) { error = game + ": expected 0x3FC00 bytes"; return false; }
    const BuildInfo* build = detect_build(out.game_image.data(), out.game_image.size());
    if (!build) { error = game + ": unexpected entry code (not a build this runtime knows)"; return false; }
    const std::string tag = manifest_build(dir);
    if (!tag.empty()) {
        const BuildInfo* named = find_build(tag);
        if (!named) { error = dir + "/manifest.txt names the build \"" + tag + "\", which this runtime does not know"; return false; }
        if (named != build) {
            error = dir + "/manifest.txt names the build \"" + tag + "\" but " + game + " is the build \"" + build->id + "\"";
            return false;
        }
    }
    out.build = build;
    if (out.disc2.size() != kDisc2Size) { error = disc + ": expected 901120 bytes"; return false; }
    if (std::memcmp(out.disc2.data() + 2, "X2B ", 4) != 0) { error = disc + ": missing \"X2B \" disc id"; return false; }
    return true;
}

void boot_game(Machine& m, const GameAssets& assets) {
    std::fill(m.chip.begin(), m.chip.end(), 0);
    m.load(kGameBase, assets.game_image.data(), assets.game_image.size());
    m.restart_pc = assets.build->restart_pc;
    if (assets.build->entry_pc) {
        // Entering past the image's own `bra.w` (a build whose start-up is elsewhere) skips the visit to the
        // restart vector that the counter discounts, so count it here.
        ++m.restarts;
        m.start_cpu(assets.build->entry_pc, kGameInitialSsp, 0x2700);
    } else {
        m.start_cpu(kGameBase, kGameInitialSsp, 0x2700);
    }
}

} // namespace x2
