// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/st_game.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace x2::st {

namespace {
uint32_t ram_word(const StMachine& m, uint32_t a) { return (uint32_t(m.ram()[a]) << 8) | m.ram()[a + 1]; }
void put_word(StMachine& m, uint32_t a, uint16_t v) {
    m.ram()[a] = uint8_t(v >> 8);
    m.ram()[a + 1] = uint8_t(v);
}
constexpr const char* kScoresFile = "highscores-st.bin";
} // namespace

HiscoreTable read_hiscores(const StMachine& m) {
    HiscoreTable t{};
    if (addr::kHiscoreTable + kHiscoreSize <= m.ram_size()) std::memcpy(t.data(), m.ram() + addr::kHiscoreTable, kHiscoreSize);
    return t;
}

void write_hiscores(StMachine& m, const HiscoreTable& t) {
    if (addr::kHiscoreTable + kHiscoreSize <= m.ram_size()) std::memcpy(m.ram() + addr::kHiscoreTable, t.data(), kHiscoreSize);
}

bool hiscores_pristine(const HiscoreTable& t) {
    for (uint32_t i = 0; i < 40; ++i)
        if (t[i] != 0) return false;
    for (int line = 0; line < 10; ++line) {
        char want[16];
        std::snprintf(want, sizeof want, "%2d. 0000000 :::", line + 1);
        if (std::memcmp(&t[40 + size_t(line) * 15], want, 15) != 0) return false;
    }
    return true;
}

StGame::StGame(const StGameConfig& cfg) : cfg_(cfg), god_mode_(cfg.god_mode) {
    cfg_.credits = std::clamp(cfg_.credits, 1, kMaxCredits);
    cheated_ = cfg_.god_mode || cfg_.credits > kGameCredits;
}

std::string StGame::scores_path() const {
    return cfg_.persist_dir.empty() ? std::string() : (std::filesystem::path(cfg_.persist_dir) / kScoresFile).string();
}

void StGame::install(StMachine& m) {
    restore_done_ = false;
    scores_loaded_ = false;
    saved_ = HiscoreTable{};
    // God mode, the part energy cannot cover: the death routine has two callers, $6838 when the energy runs out and
    // $6AF0 when the push-back sequence reaches its last step (no energy lost at all). Returning from the routine leaves
    // the dying flag clear, so the ship's handler carries on as if the hit had been survived; clearing the push-back counter
    // ends the sequence so it cannot sit on its last step and die again at once. A hook, not a byte patch.
    // Installed only when god mode could be used: a headless run without it is left completely alone, so no replay or
    // golden hash can move.
    if (cfg_.god_mode || cfg_.god_hotkey)
        m.add_pc_hook(addr::kDeathRoutine, [this](StMachine& mm) {
            if (!god_mode_) return;
            ++god_saves_;
            put_word(mm, addr::kEnergy, kEnergyFull);
            put_word(mm, addr::kPushBack, 0);
            mm.do_rts();
        });
    // Credits: the new-game initialiser ($7790) sets `move.w #3,$cf0` at $77C6; the hook after it replaces the three. A continue ($81E2 subq)
    // decrements whatever is there and is left alone.
    if (cfg_.credits != kGameCredits)
        m.add_pc_hook(addr::kNewGameCreditsDone, [this](StMachine& mm) {
            put_word(mm, addr::kCredits, uint16_t(cfg_.credits));
            std::printf("credits: %d\n", cfg_.credits);
        });
    if (!cfg_.persist_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cfg_.persist_dir, ec);
        if (ec) std::fprintf(stderr, "x2recomp: high scores off: cannot create %s: %s\n", cfg_.persist_dir.c_str(), ec.message().c_str());
        else m.add_pc_hook(addr::kHiscoreSavedPc, [this](StMachine& mm) { save_scores(mm); });
    }
}

void StGame::restore_scores(StMachine& m) {
    const std::string path = scores_path();
    restore_done_ = true;
    if (path.empty()) return;
    HiscoreTable t;
    std::string why;
    if (load_hiscore_file(path, cfg_.image_hash, t, why)) {
        write_hiscores(m, t);
        scores_loaded_ = true;
        std::printf("high scores: loaded %s\n", path.c_str());
    } else {
        std::printf("high scores: %s: %s; using the game's own table\n", path.c_str(), why.c_str());
    }
    saved_ = read_hiscores(m);
}

void StGame::save_scores(const StMachine& m) {
    const std::string path = scores_path();
    if (path.empty() || !restore_done_) return;
    const HiscoreTable t = read_hiscores(m);
    if (t == saved_) return;
    if (cheated_ && !cfg_.cheat_scores) {
        // The game's own table still shows the score; it just does not outlive the session.
        if (!warned_cheat_) std::printf("high scores: not saved, a cheat was used this session (cheat_scores = on to save anyway)\n");
        warned_cheat_ = true;
        saved_ = t;
        return;
    }
    std::string why;
    if (save_hiscore_file(path, cfg_.image_hash, t, why)) {
        ++saves_;
        std::printf("high scores: saved %s\n", path.c_str());
    } else {
        std::fprintf(stderr, "x2recomp: high scores not saved: %s\n", why.c_str());
    }
    saved_ = t; // do not retry on every call; the next change tries again
}

bool StGame::begin_frame(StMachine& m) {
    // A saved table goes back once, as soon as the engine has loaded its own (pristine) one.
    if (!restore_done_ && !cfg_.persist_dir.empty() && hiscores_pristine(read_hiscores(m))) restore_scores(m);
    const uint16_t ships = uint16_t(ram_word(m, addr::kShips));
    const bool in_game = ships != 0;
    if (god_mode_ && in_game && ram_word(m, addr::kEnergy) < kEnergyFull) put_word(m, addr::kEnergy, kEnergyFull);
    return in_game;
}

void StGame::command(StHostCommand c, StMachine& m) {
    switch (c) {
    case StHostCommand::GodMode:
        god_mode_ = !god_mode_;
        if (god_mode_) cheated_ = true; // stays set: the session is flagged once a cheat is used
        std::printf("god mode: %s%s\n", god_mode_ ? "on" : "off", god_mode_ ? " (CHEAT: the ship's energy never runs out; no high scores saved)" : "");
        break;
    case StHostCommand::AddCredit: {
        const uint16_t ships = uint16_t(ram_word(m, addr::kShips));
        if (ships == 0) { std::printf("credits: only during a game\n"); break; }
        const uint32_t now = ram_word(m, addr::kCredits);
        if (int(now) >= kMaxCredits) { std::printf("credits: already %u, the most the display can show\n", now); break; }
        put_word(m, addr::kCredits, uint16_t(now + 1));
        cheated_ = true;
        std::printf("credits: %u (CHEAT: no high scores saved)\n", now + 1);
        break;
    }
    }
}

} // namespace x2::st
