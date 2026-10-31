// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "boot.h"

#include "custom_regs.h"
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
// The CDTV release: the program is a hunk file linked at $400 by the importer, so every address is the hunk offset plus $400
// (cdtv-c0.md). No disk loader, no disc-id check and no restart vector: the program never exits, reset is a host operation. Its
// engine variables are the disk build's minus $3FA in hunk terms, i.e. disk + 6 here.
const BuildInfo kBuildCdtv{"cdtv", 0, 0, 0, 0x34FB2, 0x3AE38, 0, 0x391B2, 0x3C33E, 0x3C54C, false, 1024,
                           0xCCC, 0xCD0, 0xCF6, 0x434, true, 0xB52};

// The PDos release (WHDLoad imager v5): the game proper (the second stage the intro loads) is another assembly of the same
// engine with long absolute addressing throughout, so the code addresses differ while the RAM variables keep theirs. Its
// loader at $94E2 is the same routine as $921A but reads a disk of 12-sector tracks, numbering the game's sectors one above
// the disk image's (track 0 has only 11): sector n is image sector n-1. Its file table (11 entries of 8 bytes: first
// sector, sector count, offset in the first sector, offset in the last) is read into $380 by the intro, so the boot copies it
// from its place in track 0.
const BuildInfo kBuildPdos{"pdos-v5", 0, 0x7382, 0x94E2, 0x1EB4, 0x7624, 0, 0x5DB4, 0x87E6, 0x89F4, false, 512,
                           0xCC6, 0xCCA, 0xCF0, 0x42E, false, 0xB4C, 982528, -1, 12, 0x87C, 0x380, 88};

namespace {
const BuildInfo* const kBuilds[] = {&kBuildCanonical, &kBuildPowerPack, &kBuild1762, &kBuildCdtv, &kBuildPdos};

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
    // The PDos game: `bra.w $7382`, whose first instructions are move #$2700,sr ; lea $435FC.l,a7 ; lea $8.l,a0.
    static const uint8_t kPdosStart[] = {0x46, 0xFC, 0x27, 0x00, 0x4F, 0xF9, 0x00, 0x04, 0x35, 0xFC, 0x41, 0xF9, 0x00, 0x00, 0x00, 0x08};
    if (std::memcmp(image, "\x60\x00\x6F\x80", 4) == 0 && std::memcmp(at(0x7382), kPdosStart, sizeof kPdosStart) == 0)
        return &kBuildPdos;
    return nullptr;
}

bool is_cdtv_image(const uint8_t* image, size_t size) {
    if (!image || size != kCdtvImageSize) return false;
    auto at = [&](uint32_t addr) { return image + (addr - kGameBase); };
    // The entry `jmp $3AA12.l`, the program's own first instructions (it saves SP and sets its stack), the OS block that starts
    // `movea.l $4.l,a6`, and the PAL check `movea.l $4.w,a6 ; cmpi.b #$32,$212(a6)`: the sites the runtime's traps are keyed to.
    static const uint8_t kEntry[] = {0x4E, 0xF9, 0x00, 0x03, 0xAA, 0x12};
    static const uint8_t kStart[] = {0x23, 0xCF, 0x00, 0x03, 0x3D, 0x4E, 0x4F, 0xF9, 0x00, 0x08, 0x4F, 0x9C};
    static const uint8_t kOsBlock[] = {0x2C, 0x79, 0x00, 0x00, 0x00, 0x04, 0x43, 0xFA};
    static const uint8_t kPal[] = {0x2C, 0x78, 0x00, 0x04, 0x0C, 0x2E, 0x00, 0x32, 0x02, 0x12, 0x67, 0x08};
    return std::memcmp(image, kEntry, sizeof kEntry) == 0 && std::memcmp(at(0x3AA12), kStart, sizeof kStart) == 0 &&
           std::memcmp(at(0x3AA4A), kOsBlock, sizeof kOsBlock) == 0 && std::memcmp(at(0x3AB2C), kPal, sizeof kPal) == 0;
}

bool load_game_assets(const std::string& dir, GameAssets& out, std::string& error) {
    // The CDTV release has no disc 2 and its own image size: a pre-linked hunk file (X2) at $400 and the disc's files in cd0/.
    if (manifest_build(dir) == kCdtvBuildTag) {
        const std::string image = dir + "/game_0400.bin";
        if (!read_file(image, out.game_image)) { error = "cannot read " + image + " (import your CDTV disc: x2recomp --import PATH)"; return false; }
        if (out.game_image.size() != kCdtvImageSize) {
            error = image + ": expected " + std::to_string(kCdtvImageSize) + " bytes for the CDTV build, found " + std::to_string(out.game_image.size());
            return false;
        }
        if (!is_cdtv_image(out.game_image.data(), out.game_image.size())) {
            error = image + ": not the CDTV release's X2 program as the importer links it (the start-up code differs; a different release of the CDTV disc?)";
            return false;
        }
        out.build = &kBuildCdtv;
        out.cd0_dir = dir + "/cd0";
        out.disc2.clear();
        return true;
    }
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
    if (build->disc_bytes) {
        if (out.disc2.size() != build->disc_bytes) { error = disc + ": expected " + std::to_string(build->disc_bytes) + " bytes for the " + build->id + " build"; return false; }
        return true;
    }
    if (out.disc2.size() != kDisc2Size) { error = disc + ": expected 901120 bytes"; return false; }
    if (std::memcmp(out.disc2.data() + 2, "X2B ", 4) != 0) { error = disc + ": missing \"X2B \" disc id"; return false; }
    return true;
}

void boot_game(Machine& m, const GameAssets& assets) {
    std::fill(m.chip.begin(), m.chip.end(), 0);
    m.load(kGameBase, assets.game_image.data(), assets.game_image.size());
    m.restart_pc = assets.build->restart_pc;
    if (assets.build->table_len && size_t(assets.build->table_disc_off) + assets.build->table_len <= assets.disc2.size())
        m.load(assets.build->table_dst, assets.disc2.data() + assets.build->table_disc_off, assets.build->table_len);
    if (assets.build->cdtv) {
        // The program's `jmp` at $400 runs its own start-up, which sets its stack, clears its buffers and then asks the OS for
        // a few things (skipped by traps, cdtv/cdtv_game.h). What the OS would have left behind that the program relies on:
        // the level-3 vector (the program saves it and chains to it, and never clears the interrupt request itself, so it
        // must be a handler that does: the program's own 8-byte one at $350C4) and the vertical-blank interrupt enabled
        // (the program only touches the audio enables and the master bit).
        m.write32(0x6C, kCdtvOsLevel3Stub);
        m.custom_write(reg::INTENA, 0xC000 | 0x0020);
        // Supervisor mode with the interrupt mask at 0: the OS started the program in user mode and the program never touches
        // SR, so it relies on interrupts being open once it enables them in INTENA.
        m.start_cpu(kGameBase, kGameBase, 0x2000);
        return;
    }
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
