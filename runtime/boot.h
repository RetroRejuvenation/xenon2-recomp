// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Boot of the resident game image (bypasses the cracker intro, the depacker and the trainer stub).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x2 {

class Machine;

// The game builds the runtime can start (docs/re-notes/build-variants.md). Canonical, every retail pressing and the
// "4 Others" disk all share the first one; the others differ in their start-up code or even their layout.
struct BuildInfo {
    const char* id;           // the `build=` value the importer writes into manifest.txt
    uint32_t entry_pc;        // first instruction when it is not the image's own `bra.w` at $400 (0 = start at $400)
    uint32_t restart_pc;      // target of the exception vectors the game installs; the restart counter watches it
    uint32_t load_sectors_pc; // the game's disk loader, which the disk shim replaces
    // Where this build keeps the things the front end watches. All are code or table addresses of the image and
    // move when the build is laid out differently; the game's RAM variables ($CC6 energy, $E04 level, ...) do not.
    uint32_t vbl_pc;          // level-3 (vertical blank) handler entry, for the milestone report
    uint32_t main_loop_pc;    // the main loop's back-edge target, once per game-logic frame
    uint32_t discid_cmp_pc;   // the compare that follows the disc-id read (0: this build has no disc-id check)
    uint32_t death_pc;        // the ship's death routine, hooked by god mode
    uint32_t hiscore_addr;    // the high-score table inside the image (persist.h)
    uint32_t hiscore_saved_pc;// the end of the name entry, where the table is saved
    bool canonical_layout;    // code addresses match the canonical image, so the report's function names apply
    uint32_t chip_kb = 512;   // chip RAM the build runs in (512 or 1024); the front end builds the Machine with it
};
extern const BuildInfo kBuildCanonical;  // "canonical": entry bra.w $7032, loader $921A
extern const BuildInfo kBuildPowerPack;  // "power-pack-2234": SPS 2234, starts at $762E, same loader address
extern const BuildInfo kBuild1762;       // "compilation-1762": SPS 1762, entry bra.w $703C

// The build whose manifest tag is `id` (empty = none), or nullptr if the runtime does not know it.
const BuildInfo* find_build(const std::string& id);
// Recognises a build from the image itself (entry word and start-up code shape); nullptr if it is none of them.
const BuildInfo* detect_build(const uint8_t* image, size_t size);

// The manifest tag of the CDTV release's assets (import/cdtv_game.h): a pre-linked image of 809,564 bytes at $400 and the disc's
// files under cd0/, with no disc2.bin. The runtime cannot start that build yet (phase C3 of the CDTV plan), so loading such an assets
// folder is refused with an explicit message instead of failing on the missing disc 2 or the size of the image.
constexpr const char* kCdtvBuildTag = "cdtv";

struct GameAssets {
    std::vector<uint8_t> game_image; // run-time image, $3FC00 bytes, lives at $400
    std::vector<uint8_t> disc2;      // flat 1760-sector disc 2 (901120 bytes)
    const BuildInfo* build = &kBuildCanonical;
};

constexpr uint32_t kGameBase = 0x400;
constexpr uint32_t kGameImageSize = 0x3FC00;
constexpr uint32_t kDisc2Size = 1760 * 512;
constexpr uint32_t kGameInitialSsp = 0x435FC; // $763C sets the same value

// Reads <dir>/game_0400.bin and <dir>/disc2.bin (written by the importer or tools/prep_assets.py; a CDTV import has no disc2.bin and is
// refused up front, kCdtvBuildTag) and works out
// which build the image is: from the image itself, checked against the `build=` line of <dir>/manifest.txt when
// there is one. Returns false with a message if a file is missing or malformed, the image is no known build, or
// the manifest names a different build than the image shows.
bool load_game_assets(const std::string& dir, GameAssets& out, std::string& error);

// Place the image at $400 and start the CPU where the crack stub's `jmp $400` would: supervisor mode,
// SR=$2700, everything else in chip RAM zero. A build with its own start-up (BuildInfo::entry_pc) starts there.
void boot_game(Machine& m, const GameAssets& assets);

} // namespace x2
