// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Atari ST release's services on top of the StMachine: the assists and cheats the other builds have (god mode,
// credits, the cheat flag; autofire is the shared input layer's) and the high-score table the original never saved. Everything here talks to the game
// through its RAM and a handful of PC hooks, never by patching the image (docs/re-notes/atari-st-hooks.md).
//
// The Kixx resident keeps the engine's variable block at the Amiga disk build's addresses (energy $CC6, ships $CCA, credits
// $CF0, level $E04, ...), its death routine at $6796 with the same two callers (energy gone at $6838; the push-back
// sequence at $6AF0, counter $42E, crush at -$11) and its high-score table, laid out like the Amiga one (ten score longs and
// ten 15-character lines, 190 bytes, runtime/persist.h), at $8FAA. The name entry ends at the rts at $91BC.
//
// High scores are our addition (decision D6): the original keeps the table in RAM only and never writes a disk. A saved
// table is put back when the engine's own table first holds its pristine default (all scores 0, names ":::"), and written
// when the name entry returns (or at exit) unless a cheat was used this session. One file per release, highscores-st.bin,
// keyed by the hash of the logical disk image, in the shared format of runtime/persist.h.
#pragma once
#include "persist.h"
#include "st/st_machine.h"

#include <cstdint>
#include <string>

namespace x2::st {

// The Kixx resident's addresses (word/long variables are big-endian in RAM).
namespace addr {
constexpr uint32_t kLoopCount = 0xE02;
constexpr uint32_t kLevel = 0xE04;
constexpr uint32_t kEnergy = 0xCC6;      // full is $27
constexpr uint32_t kShips = 0xCCA;       // 0 whenever no game is in progress
constexpr uint32_t kCredits = 0xCF0;
constexpr uint32_t kScore = 0xC92;       // long
constexpr uint32_t kCash = 0xC9A;        // long
constexpr uint32_t kPushBack = 0x42E;    // counts down from -1; the ship is crushed at -$11
constexpr uint32_t kDying = 0xD84;       // byte
constexpr uint32_t kDeathRoutine = 0x6796;
constexpr uint32_t kObjList = 0xB4C;     // head of the hittable-object list (next at +$12, hit handler at +$A)
constexpr uint32_t kJoyHeld = 0xA00;     // byte: the IKBD joystick state (bit 7 fire)
constexpr uint32_t kFireEdge = 0xA01;    // byte: the fire press edge latch
constexpr uint32_t kNewGameCreditsDone = 0x77CE; // just after the new-game initialiser's `move.w #3,$cf0` ($77C6); a continue never runs it
constexpr uint32_t kHiscoreTable = 0x8FAA;
constexpr uint32_t kHiscoreSavedPc = 0x91BC; // the rts that ends the insertion and name entry routine ($90A4)
} // namespace addr

constexpr uint16_t kEnergyFull = 0x27;
constexpr int kGameCredits = 3, kMaxCredits = 9; // the display has one digit

// The table as the shared persistence code sees it, read from and written to ST RAM.
HiscoreTable read_hiscores(const StMachine& m);
void write_hiscores(StMachine& m, const HiscoreTable& t);
// True for the game's own default: ten zero scores and lines " 1. 0000000 :::" ... "10. 0000000 :::".
bool hiscores_pristine(const HiscoreTable& t);

struct StGameConfig {
    bool god_mode = false;
    bool cheat_scores = false;   // save high scores even in a session that used a cheat
    int credits = kGameCredits;  // credits a new game starts with, 1..9
    std::string persist_dir;     // "" = high scores off
    uint64_t image_hash = 0;     // fnv1a64 of the logical disk image: the key of the score file
    bool god_hotkey = false;     // the god-mode hook is installed even when god mode starts off (a window can turn it on)
};

enum class StHostCommand { GodMode, AddCredit };

class StGame {
public:
    explicit StGame(const StGameConfig& cfg);

    // Call once after every boot (the first and after a reset): installs the PC hooks and forgets the per-boot state.
    void install(StMachine& m);
    // At the start of each frame, before it runs: puts a saved high-score table back (once, when the engine has loaded its own)
    // and keeps the ship's energy full in god mode. Returns whether a game is in progress (ships > 0), which the shared autofire
    // policy needs (input.h AutofirePolicy::set_in_game).
    bool begin_frame(StMachine& m);
    // Host commands (input.h HostCommand::GodMode and AddCredit). Prints what changed, like the other builds.
    void command(StHostCommand c, StMachine& m);

    bool god_mode() const { return god_mode_; }
    bool cheated() const { return cheated_; }
    uint64_t god_saves() const { return god_saves_; }
    bool scores_loaded() const { return scores_loaded_; }
    uint64_t scores_saved_count() const { return saves_; }
    // Write the table if it changed since the last save and no cheat blocks it (the name-entry hook does this too).
    void save_scores(const StMachine& m);

private:
    std::string scores_path() const;
    void restore_scores(StMachine& m);

    StGameConfig cfg_;
    bool god_mode_ = false, cheated_ = false;
    uint64_t god_saves_ = 0, saves_ = 0;
    bool scores_loaded_ = false, restore_done_ = false, warned_cheat_ = false;
    HiscoreTable saved_{};
};

} // namespace x2::st
