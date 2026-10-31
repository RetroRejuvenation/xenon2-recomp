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
    // The game variables the front end reads and writes (assists, cheats, the in-game test). They sit at the same
    // place in every disk build; the CDTV release's engine variables sit at the disk addresses minus $3FA.
    uint32_t var_energy = 0xCC6;   // ship energy, refilled to 0x27 on respawn
    uint32_t var_ships = 0xCCA;    // ships left; 0 whenever no game is in progress
    uint32_t var_credits = 0xCF0;  // continues; the display shows one ASCII digit
    uint32_t var_pushback = 0x42E; // push-back counter; the ship is crushed at -$11
    bool cdtv = false;             // the CDTV release: a pre-linked hunk image at $400 and the disc's files, no disc 2
    uint32_t var_objlist = 0xB4C;  // head pointer of the hittable-object list (the `pokeobj` coverage walk, debug_dump.h)
    // A build whose disk is not the standard 1760-sector disc 2 (the PDos release keeps its packed files on a disk of
    // 11 + 159 x 12 sectors that the game's own loader reads):
    uint32_t disc_bytes = 0;            // size of disc2.bin; 0 = the standard 901120
    int32_t disc_sector_bias = 0;       // added to the sector number the game's loader is called with
    uint32_t disc_sectors_per_track = 11;
    // The loader of such a build reads a small table the original start-up left in memory: `table_len` bytes of the disc
    // at `table_disc_off` are copied to `table_dst` before the game starts (0 = none).
    uint32_t table_disc_off = 0, table_dst = 0, table_len = 0;
};
extern const BuildInfo kBuildCanonical;  // "canonical": entry bra.w $7032, loader $921A
extern const BuildInfo kBuildPowerPack;  // "power-pack-2234": SPS 2234, starts at $762E, same loader address
extern const BuildInfo kBuild1762;       // "compilation-1762": SPS 1762, entry bra.w $703C
extern const BuildInfo kBuildCdtv;       // "cdtv": the CDTV release, X2 linked at $400 (docs/re-notes/cdtv-c0.md)
extern const BuildInfo kBuildPdos;       // "pdos-v5": the Rob Northen PDos release (imager floppy version v5), entry bra.w $7382

// The build whose manifest tag is `id` (empty = none), or nullptr if the runtime does not know it.
const BuildInfo* find_build(const std::string& id);
// Recognises a build from the image itself (entry word and start-up code shape); nullptr if it is none of them.
const BuildInfo* detect_build(const uint8_t* image, size_t size);

// The manifest tag of the CDTV release's assets (import/cdtv_game.h): a pre-linked image of 809,564 bytes at $400 and the disc's
// files under cd0/, with no disc2.bin. It starts bare-metal: the runtime skips X2's OS start-up block and answers its file loads and
// CD-audio calls itself (cdtv/cdtv_game.h).
constexpr const char* kCdtvBuildTag = "cdtv";
constexpr uint32_t kCdtvOsLevel3Stub = 0x350C4; // the program's own `move.w #$70,INTREQ ; rte`, stored in the level-3 vector at boot
constexpr uint32_t kCdtvImageSize = 809564;   // hunk 0 (437,404 B) + hunk 1 (372,160 B) linked at $400 and $6B09C

struct GameAssets {
    std::vector<uint8_t> game_image; // run-time image, $3FC00 bytes (CDTV: 809,564), lives at $400
    std::vector<uint8_t> disc2;      // flat 1760-sector disc 2 (901120 bytes); empty for the CDTV build
    const BuildInfo* build = &kBuildCanonical;
    std::string cd0_dir;             // CDTV build: <assets>/cd0, the disc's files as the game opens them
};

constexpr uint32_t kGameBase = 0x400;
constexpr uint32_t kGameImageSize = 0x3FC00;
constexpr uint32_t kDisc2Size = 1760 * 512;
constexpr uint32_t kGameInitialSsp = 0x435FC; // $763C sets the same value

// Reads <dir>/game_0400.bin and <dir>/disc2.bin (written by the importer or tools/prep_assets.py; a CDTV import has no disc2.bin and
// is loaded as the CDTV build instead, kCdtvBuildTag) and works out
// which build the image is: from the image itself, checked against the `build=` line of <dir>/manifest.txt when
// there is one. Returns false with a message if a file is missing or malformed, the image is no known build, or
// the manifest names a different build than the image shows.
// True if `image` is the CDTV release's X2 as the importer links it (the entry, start-up, OS block and PAL check the traps are keyed to).
bool is_cdtv_image(const uint8_t* image, size_t size);
bool load_game_assets(const std::string& dir, GameAssets& out, std::string& error);

// Place the image at $400 and start the CPU where the crack stub's `jmp $400` would: supervisor mode,
// SR=$2700, everything else in chip RAM zero. A build with its own start-up (BuildInfo::entry_pc) starts there.
void boot_game(Machine& m, const GameAssets& assets);

} // namespace x2
