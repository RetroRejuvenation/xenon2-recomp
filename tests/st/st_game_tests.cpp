// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Atari ST release's hooks and options (runtime/st/st_game.*) on SYNTHETIC inputs: the high-score
// table check, the death-routine and new-game PC hooks on hand-assembled code, the score file's save, restore, hash key and
// cheat rules, and the poke walk over a hand-built object list. No game bytes: the code lives at the engine's addresses
// only because that is where the hooks are.
#include "st/st_game.h"
#include "st/st_machine.h"
#include "st/st_run.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

using namespace x2::st;
using x2::HiscoreTable;
namespace fs = std::filesystem;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

uint16_t rd16(const StMachine& m, uint32_t a) { return uint16_t((m.ram()[a] << 8) | m.ram()[a + 1]); }
void wr16(StMachine& m, uint32_t a, uint16_t v) {
    m.ram()[a] = uint8_t(v >> 8);
    m.ram()[a + 1] = uint8_t(v);
}
void wr32(StMachine& m, uint32_t a, uint32_t v) {
    wr16(m, a, uint16_t(v >> 16));
    wr16(m, a + 2, uint16_t(v));
}
void put(StMachine& m, uint32_t at, std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) m.ram()[at++] = x;
}

// A machine booted from a boot sector that just loops, so the tests can put code anywhere in RAM and point the PC at it.
StDisk loop_disk() {
    StDisk d;
    d.sides = 2;
    d.sectors_per_track = 10;
    d.tracks = 80;
    d.data.assign(d.expected_size(), 0);
    d.data[0] = 0x60;
    d.data[1] = 0xFE; // bra.s *
    uint16_t sum = 0;
    for (int i = 0; i < 510; i += 2) sum = uint16_t(sum + ((d.data[i] << 8) | d.data[i + 1]));
    const uint16_t fix = uint16_t(0x1234 - sum);
    d.data[510] = uint8_t(fix >> 8);
    d.data[511] = uint8_t(fix);
    return d;
}

struct Rig {
    StMachine m;
    Rig() {
        m.insert_disk(loop_disk());
        std::string why;
        if (!m.boot(why)) {
            std::fprintf(stderr, "  boot: %s\n", why.c_str());
            ++g_failures;
        }
    }
};

HiscoreTable pristine_table() {
    HiscoreTable t{};
    for (int line = 0; line < 10; ++line) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%2d. 0000000 :::", line + 1);
        std::memcpy(&t[40 + size_t(line) * 15], buf, 15);
    }
    return t;
}

HiscoreTable played_table() {
    HiscoreTable t = pristine_table();
    t[3] = 0x58; // the top score long: 0x00000258 = 600
    t[2] = 0x02;
    std::memcpy(&t[40], " 1. 0000600 AAA", 15);
    return t;
}

void test_pristine_table() {
    HiscoreTable t = pristine_table();
    CHECK(hiscores_pristine(t));
    t[3] = 1;
    CHECK(!hiscores_pristine(t));
    t = pristine_table();
    t[40 + 12] = 'X';
    CHECK(!hiscores_pristine(t));
    CHECK(!hiscores_pristine(played_table()));
    // The table round-trips through machine RAM.
    Rig r;
    write_hiscores(r.m, played_table());
    CHECK(read_hiscores(r.m) == played_table());
}

void test_god_mode_hook() {
    for (int mode = 0; mode < 3; ++mode) { // 0: god mode on, 1: hotkey only (off), 2: no hooks installed
        Rig r;
        StGameConfig cfg;
        cfg.god_mode = mode == 0;
        cfg.god_hotkey = mode == 1;
        StGame g(cfg);
        g.install(r.m);
        // jsr $6796 ; move.w #1,$600.w ; bra.s *   with the death routine at $6796: move.w #2,$602.w ; rts
        put(r.m, 0x7000, {0x4E, 0xB9, 0x00, 0x00, 0x67, 0x96, 0x31, 0xFC, 0x00, 0x01, 0x06, 0x00, 0x60, 0xFE});
        put(r.m, 0x6796, {0x31, 0xFC, 0x00, 0x02, 0x06, 0x02, 0x4E, 0x75});
        wr16(r.m, addr::kEnergy, 3);
        wr16(r.m, addr::kPushBack, 0xFFEF);
        r.m.set_pc(0x7000);
        r.m.run_frame();
        CHECK(rd16(r.m, 0x600) == 1); // control came back after the call either way
        if (mode == 0) {
            CHECK(rd16(r.m, 0x602) == 0); // the routine did not run
            CHECK(rd16(r.m, addr::kEnergy) == kEnergyFull && rd16(r.m, addr::kPushBack) == 0);
            CHECK(g.god_saves() == 1 && g.cheated());
        } else {
            CHECK(rd16(r.m, 0x602) == 2); // the game's own routine ran
            CHECK(rd16(r.m, addr::kEnergy) == 3 && g.god_saves() == 0);
            if (mode == 1) {
                g.command(StHostCommand::GodMode, r.m); // Backspace in a window
                CHECK(g.god_mode() && g.cheated());
                wr16(r.m, 0x602, 0);
                r.m.set_pc(0x7000);
                r.m.run_frame();
                CHECK(rd16(r.m, 0x602) == 0 && g.god_saves() == 1);
            }
        }
    }
    // God mode keeps the energy full from frame to frame, only during a game.
    Rig r;
    StGameConfig cfg;
    cfg.god_mode = true;
    StGame g(cfg);
    g.install(r.m);
    wr16(r.m, addr::kEnergy, 5);
    g.begin_frame(r.m);
    CHECK(rd16(r.m, addr::kEnergy) == 5); // no game in progress (ships 0)
    wr16(r.m, addr::kShips, 3);
    g.begin_frame(r.m);
    CHECK(rd16(r.m, addr::kEnergy) == kEnergyFull);
}

void test_credits_hook() {
    Rig r;
    StGameConfig cfg;
    cfg.credits = 6;
    StGame g(cfg);
    CHECK(g.cheated());
    g.install(r.m);
    // The new-game initialiser: move.w #3,$cf0.l ; (hook) ; move.w #1,$600.w ; bra.s *
    put(r.m, 0x77C6, {0x33, 0xFC, 0x00, 0x03, 0x00, 0x00, 0x0C, 0xF0});
    put(r.m, addr::kNewGameCreditsDone, {0x31, 0xFC, 0x00, 0x01, 0x06, 0x00, 0x60, 0xFE});
    r.m.set_pc(0x77C6);
    r.m.run_frame();
    CHECK(rd16(r.m, 0x600) == 1 && rd16(r.m, addr::kCredits) == 6);
    // 3 is the game's own number: nothing to hook, no cheat.
    Rig r3;
    StGameConfig c3;
    StGame g3(c3);
    CHECK(!g3.cheated());
    g3.install(r3.m);
    put(r3.m, 0x77C6, {0x33, 0xFC, 0x00, 0x03, 0x00, 0x00, 0x0C, 0xF0});
    put(r3.m, addr::kNewGameCreditsDone, {0x60, 0xFE});
    r3.m.set_pc(0x77C6);
    r3.m.run_frame();
    CHECK(rd16(r3.m, addr::kCredits) == 3);
    // The Insert key: only during a game, capped at the display's one digit, and a cheat.
    Rig r4;
    StGame g4{StGameConfig{}};
    g4.install(r4.m);
    g4.command(StHostCommand::AddCredit, r4.m);
    CHECK(rd16(r4.m, addr::kCredits) == 0 && !g4.cheated());
    wr16(r4.m, addr::kShips, 3);
    wr16(r4.m, addr::kCredits, 8);
    g4.command(StHostCommand::AddCredit, r4.m);
    CHECK(rd16(r4.m, addr::kCredits) == 9 && g4.cheated());
    g4.command(StHostCommand::AddCredit, r4.m);
    CHECK(rd16(r4.m, addr::kCredits) == 9);
}

void test_scores() {
    const fs::path dir = fs::temp_directory_path() / "x2_st_game_tests_scores";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path file = dir / "highscores-st.bin";
    auto config = [&](uint64_t hash) {
        StGameConfig c;
        c.persist_dir = dir.string();
        c.image_hash = hash;
        return c;
    };
    // First session: no file; the game's table is untouched; the name entry's end saves the new table.
    {
        Rig r;
        StGame g(config(0xABCD));
        g.install(r.m);
        write_hiscores(r.m, pristine_table());
        g.begin_frame(r.m);
        CHECK(!g.scores_loaded() && read_hiscores(r.m) == pristine_table());
        g.save_scores(r.m);
        CHECK(!fs::exists(file) && g.scores_saved_count() == 0); // unchanged: nothing to write
        write_hiscores(r.m, played_table());
        g.save_scores(r.m);
        CHECK(fs::exists(file) && g.scores_saved_count() == 1);
        g.save_scores(r.m);
        CHECK(g.scores_saved_count() == 1); // the same table is not written twice
    }
    // The hook at the name entry's rts does the same from inside the emulation.
    {
        fs::remove(file, ec);
        Rig r;
        StGame g(config(0xABCD));
        g.install(r.m);
        write_hiscores(r.m, pristine_table());
        g.begin_frame(r.m);
        write_hiscores(r.m, played_table());
        put(r.m, addr::kHiscoreSavedPc, {0x60, 0xFE}); // rts replaced by a loop: the hook fires before it
        r.m.set_pc(addr::kHiscoreSavedPc);
        r.m.run_frame();
        CHECK(fs::exists(file) && g.scores_saved_count() == 1);
    }
    // A new session puts the table back when the engine's own table is pristine, and only then.
    {
        Rig r;
        StGame g(config(0xABCD));
        g.install(r.m);
        write_hiscores(r.m, played_table()); // not pristine yet when the first frame begins: wait
        g.begin_frame(r.m);
        CHECK(!g.scores_loaded());
        write_hiscores(r.m, pristine_table());
        g.begin_frame(r.m);
        CHECK(g.scores_loaded() && read_hiscores(r.m) == played_table());
    }
    // A different disk image does not get another's scores.
    {
        Rig r;
        StGame g(config(0x1111));
        g.install(r.m);
        write_hiscores(r.m, pristine_table());
        g.begin_frame(r.m);
        CHECK(!g.scores_loaded() && read_hiscores(r.m) == pristine_table());
    }
    // A corrupt file is refused, not half-loaded.
    {
        std::FILE* f = std::fopen(file.string().c_str(), "r+b");
        CHECK(f != nullptr);
        if (f) {
            std::fseek(f, 20, SEEK_SET);
            std::fputc(0x55, f);
            std::fclose(f);
        }
        Rig r;
        StGame g(config(0xABCD));
        g.install(r.m);
        write_hiscores(r.m, pristine_table());
        g.begin_frame(r.m);
        CHECK(!g.scores_loaded());
    }
    // A session that used a cheat does not save, unless cheat_scores says to.
    for (int allow = 0; allow < 2; ++allow) {
        fs::remove_all(dir, ec);
        Rig r;
        StGameConfig c = config(0xABCD);
        c.god_mode = true;
        c.cheat_scores = allow == 1;
        StGame g(c);
        g.install(r.m);
        write_hiscores(r.m, pristine_table());
        g.begin_frame(r.m);
        write_hiscores(r.m, played_table());
        g.save_scores(r.m);
        CHECK(fs::exists(file) == (allow == 1));
    }
    // No persist dir: nothing is read or written.
    {
        Rig r;
        StGame g{StGameConfig{}};
        g.install(r.m);
        write_hiscores(r.m, pristine_table());
        g.begin_frame(r.m);
        write_hiscores(r.m, played_table());
        g.save_scores(r.m);
        CHECK(!g.scores_loaded() && g.scores_saved_count() == 0);
    }
    fs::remove_all(dir, ec);
}

// The object list as the game keeps it: head pointer at $B4C, next at +$12, hit handler at +$A, type word 0 ends it.
void test_pokes() {
    Rig r;
    auto obj = [&](uint32_t at, uint16_t type, uint32_t handler, uint32_t next) {
        wr16(r.m, at, type);
        wr32(r.m, at + 0xA, handler);
        wr32(r.m, at + 0x12, next);
        wr16(r.m, at + 0x32, 99);
    };
    wr32(r.m, 0xB4C, 0x9000);
    obj(0x9000, 1, 0x50360, 0x9100);
    obj(0x9100, 1, 0x1234, 0x9200);
    obj(0x9200, 1, 0x50360, 0x9300);
    obj(0x9300, 0, 0, 0x9300); // type 0 ends the walk even though it points at itself
    RunOptions::Poke p{0, 0, 0x32, 1, 2, 0x50360};
    CHECK(apply_poke(r.m, p) == 2);
    CHECK(rd16(r.m, 0x9032) == 1 && rd16(r.m, 0x9132) == 99 && rd16(r.m, 0x9232) == 1 && rd16(r.m, 0x9332) == 99);
    // A self-linked list does not hang the walk.
    obj(0x9300, 1, 0x50360, 0x9300);
    CHECK(apply_poke(r.m, p) > 2);
    // A plain poke is one write; sizes 1, 2 and 4.
    CHECK(apply_poke(r.m, {0, 0, 0xE04, 3, 2, 0}) == 1 && rd16(r.m, 0xE04) == 3);
    CHECK(apply_poke(r.m, {0, 0, 0xA07, 0xFF, 1, 0}) == 1 && r.m.ram()[0xA07] == 0xFF);
    CHECK(apply_poke(r.m, {0, 0, 0xC92, 0x12345678, 4, 0}) == 1 && r.m.read32_ram(0xC92) == 0x12345678);
}
} // namespace

int main() {
    test_pristine_table();
    test_god_mode_hook();
    test_credits_hook();
    test_scores();
    test_pokes();
    std::printf("st game tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
