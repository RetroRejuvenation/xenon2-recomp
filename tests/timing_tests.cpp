// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Accurate bus timing (runtime/machine_timing.cpp, docs/re-notes/timing.md §8) on synthetic programs and
// blits (no game data): frame length, blitter cycle counts per channel combination, bitplane DMA slowing the
// blitter, BBUSY and the blitter interrupt, BLTPRI, CPU wait states on display lines, the E-clock, and the 68000
// cycle-table corrections. Expected blit times are those of the reference emulator's cycle-exact blitter for the
// same blits on lines without bitplane DMA (generated/ref blits.log, docs/re-notes/timing.md §8.3).
#include "machine.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <vector>

using namespace x2;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        ++g_checks;                                                                                     \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                           \
        if (va_ != vb_) {                                                                               \
            ++g_failures;                                                                               \
            std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                               \
    } while (0)

void put_words(Machine& m, uint32_t addr, std::initializer_list<uint16_t> words) {
    for (uint16_t w : words) {
        m.chip[addr] = uint8_t(w >> 8);
        m.chip[addr + 1] = uint8_t(w);
        addr += 2;
    }
}

// A machine whose CPU sits in STOP, so only DMA uses the bus.
void stopped_cpu(Machine& m) {
    put_words(m, 0x1000, {0x4E72, 0x2700}); // stop #$2700
    m.start_cpu(0x1000, 0x8000);
}

// A copper list that waits for (line, hpos) and then writes BLTSIZE: the blit starts at a known beam position.
// `tail` further MOVEs to COLOR00 follow it, to measure what the copper's fetches cost the blit.
void blit_at(Machine& m, int line, int hpos, uint16_t con0, uint16_t con1, int w, int h, int tail = 0) {
    m.custom_write(reg::BLTCON0, con0);
    m.custom_write(reg::BLTCON1, con1);
    m.custom_write(reg::BLTAFWM, 0xFFFF);
    m.custom_write(reg::BLTALWM, 0xFFFF);
    for (uint16_t r : {reg::BLTAPTH, reg::BLTBPTH, reg::BLTCPTH, reg::BLTDPTH}) {
        m.custom_write(r, 0x0006);
        m.custom_write(uint16_t(r + 2), 0x0000);
    }
    uint32_t a = 0x2000;
    put_words(m, a, {uint16_t((line << 8) | (hpos & 0xFE) | 1), 0xFFFE, reg::BLTSIZE, uint16_t((h << 6) | w)});
    a += 8;
    for (int i = 0; i < tail; ++i, a += 4) put_words(m, a, {reg::COLOR00, 0x0000});
    put_words(m, a, {0xFFFF, 0xFFFE});
    m.custom_write(reg::COPCON, 2); // the copper may write the blitter registers
    m.custom_write(reg::COP1LCH, 0);
    m.custom_write(reg::COP1LCH + 2, 0x2000);
}

// Colour clocks from BLTSIZE to BBUSY clear for one blit started on a blank line (no bitplane DMA).
int blit_time(uint16_t con0, uint16_t con1, int w, int h, int tail = 0) {
    Machine m;
    stopped_cpu(m);
    m.custom_write(reg::DMACON, 0x8000 | reg::DMAF_MASTER | reg::DMAF_BLITTER | reg::DMAF_COPPER);
    blit_at(m, 20, 0x40, con0, con1, w, h, tail);
    m.run_frame(); // (the copper list would start the blit again in the next frame)
    return int(m.blit_cck);
}

void test_frame_length() {
    Machine a;
    CHECK(a.timing() == Timing::Accurate);
    CHECK_EQ(a.lines_per_frame(), 313); // long fields only: LOF = 1, as the reference's PAL A500
    stopped_cpu(a);
    a.run_frame();
    CHECK_EQ(a.paula.now(), 313 * kColorClocksPerLine);
    CHECK(a.read16(0xDFF004) & 0x8000);
    Machine f;
    f.set_timing(Timing::Fast);
    CHECK_EQ(f.lines_per_frame(), 312);
    CHECK(!(f.read16(0xDFF004) & 0x8000));
}

// Words x the channel combination's cycles per word (WinUAE blit_cycle_diagram), plus the two start-up cycles
// after the BLTSIZE write cycle; D blits end with the pipeline's last D write two cycles after BBUSY clears, which
// is not part of the busy time. The 10x10 blits start at hpos $40 and run into the next line(s), where each line
// start costs them the four refresh slots.
//
// kCopper is this harness's own cost: the copper list that writes BLTSIZE fetches its next instruction (the end
// marker) in the two copper slots right after the write, and the copper has priority over the blitter, so the
// blit loses its first two cycles to it (timing.md §9.2). The reference's figures below are its game blits,
// which the CPU starts with the copper stopped, so they carry no such cost.
void test_blit_cycles() {
    constexpr int kCopper = 2;
    struct Case { uint16_t con0, con1; int w, h, cck; } cases[] = {
        {0xEFCA, 0xE000, 2, 6, 50 + kCopper},   // ABCD 4 cycles/word: the reference measures 50 on blank lines
        {0x7FCA, 0x7000, 1, 4, 18 + kCopper},   // the reference: 18
        {0x0100, 0, 10, 10, 206 + kCopper},     // -D:   2 (+4 refresh)
        {0x0900, 0, 10, 10, 206 + kCopper},     // A-D:  2
        {0x0500, 0, 10, 10, 306 + kCopper},     // -BD:  3
        {0x0300, 0, 10, 10, 306 + kCopper},     // -CD:  3
        {0x0B00, 0, 10, 10, 306 + kCopper},     // AC-D: 3
        {0x0D00, 0, 10, 10, 306 + kCopper},     // AB-D: 3
        {0x0700, 0, 10, 10, 410 + kCopper},     // -BCD: 4 (two line starts, +8)
        {0x0F00, 0, 10, 10, 410 + kCopper},     // ABCD: 4
        {0x0800, 0, 10, 10, 206 + kCopper},     // A only: 2
        {0x0100, 0x0012, 10, 10, 306 + kCopper}, // fill (D without C) adds an idle cycle: 3
    };
    for (const Case& c : cases) {
        int t = blit_time(c.con0, c.con1, c.w, c.h);
        if (t != c.cck) std::printf("  blit %04X/%04X %dx%d: %d cck, expected %d\n", c.con0, c.con1, c.w, c.h, t, c.cck);
        CHECK_EQ(t, c.cck);
    }
}

// Copper DMA slots (timing.md §9.2): the copper fetches two words per instruction, on even colour clocks, with
// priority over the blitter. Each extra MOVE in the list behind the one that starts the blit must therefore cost
// the blit two more colour clocks.
void test_copper_dma_slots() {
    int base = blit_time(0xEFCA, 0xE000, 2, 6);
    for (int tail = 1; tail <= 4; ++tail) {
        int t = blit_time(0xEFCA, 0xE000, 2, 6, tail);
        if (t != base + 2 * tail)
            std::printf("  %d copper MOVEs behind the blit: %d cck, expected %d\n", tail, t, base + 2 * tail);
        CHECK_EQ(t, base + 2 * tail);
    }
}

// The game's display: 4 lores planes, DDF $38-$D0. Bitplane DMA takes 4 of every 8 slots in the fetch window, and
// the blitter cannot use them.
void test_bitplanes_slow_the_blitter() {
    auto run = [](bool display) {
        Machine m;
        stopped_cpu(m);
        m.custom_write(reg::BPLCON0, 0x4000);
        m.custom_write(reg::DDFSTRT, 0x38);
        m.custom_write(reg::DDFSTOP, 0xD0);
        m.custom_write(reg::DIWSTRT, 0x3A81);
        m.custom_write(reg::DIWSTOP, 0x02C1);
        m.custom_write(reg::DMACON, 0x8000 | reg::DMAF_MASTER | reg::DMAF_BLITTER | reg::DMAF_COPPER |
                                        (display ? reg::DMAF_RASTER : 0));
        blit_at(m, 100, 0x10, 0x09F0, 0, 20, 40); // A->D, 800 words: lines 100-~111
        m.run_frame();
        return int(m.blit_cck);
    };
    int blank = run(false), shown = run(true);
    std::printf("  A->D 20x40: %d cck on blank lines, %d under 4 lores planes\n", blank, shown);
    CHECK(blank > 1600 && blank < 1650);
    // 80 of the 227 slots per line are bitplane fetches: the blit takes about 1.5 times as long.
    CHECK(shown > blank * 14 / 10 && shown < blank * 16 / 10);
}

// CPU program at $1000.
void load_program(Machine& m, std::initializer_list<uint16_t> code) {
    put_words(m, 0x1000, code);
    m.start_cpu(0x1000, 0x8000);
}

void test_bbusy_and_interrupt() {
    for (Timing t : {Timing::Accurate, Timing::Fast}) {
        Machine m;
        m.set_timing(t);
        // move.w #$8240,$dff096 ; move.w #$0101,$dff058 (D only, 4 words x 4 rows, BLTCON0 $0100)
        // loop: btst #6,$dff002 ; bne.s loop ; move.w $dff01e,d0 ; stop #$2700
        load_program(m, {0x33FC, 0x8240, 0x00DF, 0xF096, 0x33FC, 0x0104, 0x00DF, 0xF058,
                         0x0839, 0x0006, 0x00DF, 0xF002, 0x66F6, 0x3039, 0x00DF, 0xF01E, 0x4E72, 0x2700});
        m.custom_write(reg::BLTCON0, 0x0100);
        m.custom_write(reg::BLTCON1, 0);
        m.custom_write(reg::DMACON, 0x8000 | reg::DMAF_MASTER);
        uint32_t polls_before = m.custom_reads[reg::DMACONR >> 1];
        m.run_frame();
        uint32_t polls = m.custom_reads[reg::DMACONR >> 1] - polls_before;
        CHECK(m.intreq & reg::INTF_BLIT);
        CHECK(m.dreg(0) & reg::INTF_BLIT); // the interrupt was raised by the time BBUSY read 0
        if (t == Timing::Accurate) {
            CHECK(polls > 1); // 16 words x 2 cycles: BBUSY reads 1 for several polls
            CHECK(m.blit_cck >= 32 && m.blit_cck < 50);
        } else {
            CHECK_EQ(polls, 1); // zero-time blitter
        }
    }
}

// A CPU chip-RAM write during a long A->D blit (every slot used): without BLTPRI the blitter gives the CPU a slot
// after it has waited 3; with BLTPRI the CPU waits for the end of the blit.
void test_blitter_nasty() {
    for (bool nasty : {false, true}) {
        Machine m;
        // move.w #$0A1A,$dff058 (A->D 26x40) ; move.w d0,$3000 ; stop #$2700
        load_program(m, {0x33FC, uint16_t((40 << 6) | 26), 0x00DF, 0xF058, 0x31C0, 0x3000, 0x4E72, 0x2700});
        m.custom_write(reg::BLTCON0, 0x09F0);
        m.custom_write(reg::BLTCON1, 0);
        m.custom_write(reg::DMACON, uint16_t(0x8000 | reg::DMAF_MASTER | reg::DMAF_BLITTER | (nasty ? reg::DMAF_BLITHOG : 0)));
        m.run_cycles(4000); // the blit (1040 words, ~2100 cck = 4200 CPU cycles) is still running
        if (nasty) CHECK(m.cpu_wait_cycles > 3000);
        else CHECK(m.cpu_wait_cycles < 40);
    }
}

// CPU cycles from the start of instruction `first` (counted from 0) to the start of instruction `last`.
int cycles_between(Machine& m, int first, int last) {
    int64_t t0 = -1, t1 = -1;
    int n = 0;
    m.on_insn_start = [&](Machine& mm, uint32_t) {
        if (n == first) t0 = mm.cpu_now();
        if (n == last) t1 = mm.cpu_now();
        ++n;
    };
    while (t1 < 0) m.run_cycles(4);
    m.on_insn_start = nullptr;
    return int(t1 - t0);
}

void test_68000_table_fixes() {
    struct Case { std::initializer_list<uint16_t> code; uint32_t d0, d1; int cycles; const char* what; } cases[] = {
        {{0xD280}, 0, 0, 8, "add.l d0,d1"},
        {{0x5248}, 0, 0, 8, "addq.w #1,a0"},
        {{0xD1C0}, 0, 0, 8, "adda.l d0,a0"},
        {{0x0280, 0x0000, 0x00FF}, 0, 0, 16, "andi.l #$ff,d0"},
        {{0x0880, 0x0003}, 0, 0, 12, "bclr #3,d0"},
        {{0x0880, 0x0013}, 0, 0, 14, "bclr #19,d0"},
        {{0x82C0}, 1, 0x10000, 10, "divu.w d0,d1 (overflow)"},
        {{0x82C0}, 2, 1000, 124, "divu.w d0,d1"},  // Musashi: always 140
        {{0x83C0}, 7, 100, 144, "divs.w d0,d1"},   // Musashi: always 158
    };
    for (const auto& c : cases) {
        Machine m;
        // 20 NOPs first, past the refresh slots at the start of line 0; then the instruction and two NOPs.
        std::vector<uint16_t> code(20, 0x4E71);
        code.insert(code.end(), c.code);
        code.push_back(0x4E71);
        code.push_back(0x4E71);
        uint32_t a = 0x1000;
        for (uint16_t w : code) { m.chip[a] = uint8_t(w >> 8); m.chip[a + 1] = uint8_t(w); a += 2; }
        m.start_cpu(0x1000, 0x8000);
        m.set_dreg(0, c.d0);
        m.set_dreg(1, c.d1);
        int t = cycles_between(m, 20, 21);
        if (t != c.cycles) std::printf("  %s: %d cycles, expected %d\n", c.what, t, c.cycles);
        CHECK_EQ(t, c.cycles);
    }
}

// NOPs on display lines: 4 lores planes fetch in odd slots only and leave the CPU's even slots free; 6 planes
// also take even slots, so the CPU waits.
void test_cpu_display_contention() {
    auto run = [](int planes) {
        Machine m;
        std::vector<uint16_t> nops(400, 0x4E71);
        uint32_t a = 0x1000;
        for (uint16_t w : nops) { m.chip[a] = uint8_t(w >> 8); m.chip[a + 1] = uint8_t(w); a += 2; }
        m.chip[a] = 0x60; m.chip[a + 1] = 0xFE; // bra.s *
        m.start_cpu(0x1000, 0x8000);
        m.custom_write(reg::BPLCON0, uint16_t(planes << 12));
        m.custom_write(reg::DDFSTRT, 0x38);
        m.custom_write(reg::DDFSTOP, 0xD0);
        m.custom_write(reg::DIWSTRT, 0x0081); // window from line 0, so the program runs on display lines
        m.custom_write(reg::DIWSTOP, 0xF0C1);
        m.custom_write(reg::DMACON, 0x8000 | reg::DMAF_MASTER | reg::DMAF_RASTER);
        return cycles_between(m, 0, 300);
    };
    int four = run(4), six = run(6);
    std::printf("  300 NOPs on display lines: %d cycles with 4 lores planes, %d with 6\n", four, six);
    CHECK(four < 300 * 4 * 102 / 100);  // nearly free
    CHECK(six > 300 * 4 * 12 / 10);     // planes 5 and 6 take even slots
}

// CIA access: synchronised to the E clock (CPU/10): 6 to 15 cycles more than a chip-RAM read.
void test_cia_eclock() {
    Machine m;
    // 20 NOPs ; move.b $bfe001,d0 ; nop
    std::vector<uint16_t> code(20, 0x4E71);
    code.insert(code.end(), {0x1039, 0x00BF, 0xE001, 0x4E71, 0x4E71});
    uint32_t a = 0x1000;
    for (uint16_t w : code) { m.chip[a] = uint8_t(w >> 8); m.chip[a + 1] = uint8_t(w); a += 2; }
    m.start_cpu(0x1000, 0x8000);
    int t = cycles_between(m, 20, 21);
    CHECK(t >= 16 + 6 && t <= 16 + 15);
}

} // namespace

int main() {
    struct { const char* name; std::function<void()> fn; } tests[] = {
        {"frame_length", test_frame_length},
        {"blit_cycles", test_blit_cycles},
        {"copper_dma_slots", test_copper_dma_slots},
        {"bitplanes_slow_blitter", test_bitplanes_slow_the_blitter},
        {"bbusy_and_interrupt", test_bbusy_and_interrupt},
        {"blitter_nasty", test_blitter_nasty},
        {"68000_table_fixes", test_68000_table_fixes},
        {"cpu_display_contention", test_cpu_display_contention},
        {"cia_eclock", test_cia_eclock},
    };
    for (auto& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%-24s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
