// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Runtime smoke tests that need no game data: bus, custom registers, beam counters, CIA timers, interrupt
// delivery, keyboard serial, copper, blitter and the disk shim. Each test builds a fresh Machine and, where a
// CPU is needed, hand-assembled 68000 code in chip RAM.
#include "cia.h"
#include "debug_dump.h"
#include "disk_shim.h"
#include "machine.h"

#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
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
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

void poke(Machine& m, uint32_t addr, std::initializer_list<uint16_t> words) {
    for (uint16_t w : words) { m.write16(addr, w); addr += 2; }
}

void test_bus() {
    Machine m;
    m.write32(0x1000, 0x12345678);
    CHECK(m.read8(0x1000) == 0x12 && m.read8(0x1003) == 0x78);
    CHECK(m.read16(0x1002) == 0x5678);
    CHECK(m.read32(0x81000) == 0x12345678); // 512 KB chip RAM mirrors at $80000
    CHECK(m.read32(0xFF001000) == 0x12345678); // 24-bit bus
    m.read8(0xE80000);
    CHECK(m.unmapped_accesses == 1);
}

void test_custom_setclr() {
    Machine m;
    m.write16(0xDFF096, 0x8000 | 0x0280);
    CHECK(m.read16(0xDFF002) == 0x0280 + 0x2000); // + BZERO (no blit yet)
    m.write16(0xDFF096, 0x0080);
    CHECK((m.read16(0xDFF002) & 0x07FF) == 0x0200);
    m.write16(0xDFF09A, 0xC020);
    CHECK(m.read16(0xDFF01C) == 0x4020);
    m.write16(0xDFF09C, 0x8024);
    CHECK(m.read16(0xDFF01E) == 0x0024);
    m.write16(0xDFF09C, 0x0004);
    CHECK(m.read16(0xDFF01E) == 0x0020);
    CHECK(m.read8(0xDFF01F) == 0x20); // btst #5,$dff01f as used by the game's VBL handler
    m.write16(0xDFF09E, 0x8000 | 0x1100);
    CHECK(m.read16(0xDFF010) == 0x1100);
    m.write8(0xDFF09F, 0x00); // byte write: both halves -> $0000 (clear nothing)
    CHECK(m.read16(0xDFF010) == 0x1100);
    CHECK(m.read16(0xDFF016) == 0xFF00); // POTGOR: buttons released
}

void test_beam_counters() {
    Machine m;
    // lea $2000,a0 ; loop: move.l $dff004,d0 ; move.l d0,(a0)+ ; cmpa.l #$3000,a0 ; blt.s loop ; bra.s *
    poke(m, 0x1000, {0x41F9, 0x0000, 0x2000, 0x2039, 0x00DF, 0xF004, 0x20C0, 0xB1FC, 0x0000, 0x3000, 0x6DF0, 0x60FE});
    m.start_cpu(0x1000, 0x8000);
    m.run_frame();
    uint32_t prev = 0;
    bool monotonic = true;
    for (uint32_t a = 0x2000; a < 0x3000; a += 4) {
        uint32_t v = m.read32(a);
        uint32_t beam = ((v >> 16) & 1) << 16 | (v & 0xFFFF); // V8:V7..V0:H
        if (beam < prev) monotonic = false;
        prev = beam;
    }
    uint32_t first = m.read32(0x2000), last = m.read32(0x2FFC);
    CHECK(monotonic);
    CHECK(((last >> 8) & 0xFF) > ((first >> 8) & 0xFF) + 50); // >50 lines elapsed over 1024 samples
    CHECK((last & 0xFF) < kColorClocksPerLine);
}

void test_cia_timer() {
    Cia c;
    c.write(Cia::ICR, 0x81);  // enable TA
    c.write(Cia::TALO, 100);
    c.write(Cia::CRA, 0x08);  // one-shot, stopped
    c.write(Cia::TAHI, 0);    // one-shot: load + start
    CHECK(c.cra() & 1);
    c.tick(50);
    CHECK(c.timer_a() == 50);
    CHECK(!c.irq());
    c.tick(51);
    CHECK(c.irq());
    CHECK(!(c.cra() & 1)); // one-shot stopped
    uint8_t icr = c.read(Cia::ICR);
    CHECK(icr == 0x81);
    CHECK(!c.irq()); // read clears
    // Continuous mode, multiple underflows in one step.
    c.write(Cia::TALO, 9);
    c.write(Cia::TAHI, 0);
    c.write(Cia::CRA, 0x11); // force load, start continuous
    c.tick(100);
    CHECK(c.irq());
    // TOD
    c.write(Cia::TODHI, 0);
    c.write(Cia::TODMID, 0);
    c.write(Cia::TODLO, 0);
    for (int i = 0; i < 300; ++i) c.tod_pulse();
    CHECK(c.read(Cia::TODHI) == 0 && c.read(Cia::TODMID) == 1 && c.read(Cia::TODLO) == 44);
}

void test_interrupts() {
    Machine m;
    // main: move #$2000,sr ; bra.s *
    poke(m, 0x1000, {0x46FC, 0x2000, 0x60FE});
    // L3 handler: addq.w #1,$2000 ; move.w #$20,$dff09c ; rte
    poke(m, 0x1100, {0x5279, 0x0000, 0x2000, 0x33FC, 0x0020, 0x00DF, 0xF09C, 0x4E73});
    // L2 handler: addq.w #1,$2002 ; move.b $bfed01,d0 ; move.w #8,$dff09c ; rte
    poke(m, 0x1200, {0x5279, 0x0000, 0x2002, 0x1039, 0x00BF, 0xED01, 0x33FC, 0x0008, 0x00DF, 0xF09C, 0x4E73});
    m.write32(0x6C, 0x1100);
    m.write32(0x68, 0x1200);
    m.start_cpu(0x1000, 0x8000);
    m.write16(0xDFF09A, 0xC028); // INTEN | VERTB | PORTS
    // CIA-A timer A continuous, 7094 ticks = 10 ms -> 100 Hz
    m.write8(0xBFED01, 0x81);
    m.write8(0xBFE401, 7094 & 0xFF);
    m.write8(0xBFE501, 7094 >> 8);
    m.write8(0xBFEE01, 0x11 | 0x80);
    for (int i = 0; i < 3; ++i) m.run_frame();
    CHECK(m.read16(0x2000) == 3);
    uint16_t l2 = m.read16(0x2002);
    CHECK(l2 >= 5 && l2 <= 7);
    CHECK(m.pc() >= 0x1000 && m.pc() < 0x1006); // back in the main loop
    // Masking with INTENA stops delivery.
    m.write16(0xDFF09A, 0x0020);
    m.run_frame();
    CHECK(m.read16(0x2000) == 3);
}

void test_keyboard() {
    Machine m;
    poke(m, 0x1000, {0x46FC, 0x2000, 0x60FE});
    // Keyboard handler shaped like the game's $0FAC.
    poke(m, 0x1300, {0x0839, 0x0003, 0x00BF, 0xED01, 0x6720, 0x1039, 0x00BF, 0xEC01, 0x4600, 0xE218,
                     0x13C0, 0x0000, 0x2010, 0x08F9, 0x0006, 0x00BF, 0xEE01, 0x08B9, 0x0006, 0x00BF, 0xEE01,
                     0x33FC, 0x0008, 0x00DF, 0xF09C, 0x4E73});
    m.write32(0x68, 0x1300);
    m.start_cpu(0x1000, 0x8000);
    m.write16(0xDFF09A, 0xC008);
    m.write8(0xBFED01, 0x88); // enable SP
    m.key_event(0x45, false);
    m.key_event(0x45, true);
    m.run_frame();
    m.run_frame();
    CHECK(m.read8(0x2010) == 0xC5); // both bytes delivered in order; last one is the release (0x80|0x45)
    m.key_event(0x19, false);
    m.run_frame();
    CHECK(m.read8(0x2010) == 0x19);
}

void test_copper() {
    Machine m;
    poke(m, 0x1000, {0x60FE});
    // MOVE COLOR00=$F00 ; WAIT v=$40 ; MOVE COLOR00=$0F0 ; MOVE BPL1PTH/L ; END
    poke(m, 0x3000, {0x0180, 0x0F00, 0x4001, 0xFFFE, 0x0180, 0x00F0, 0x00E0, 0x0004, 0x00E2, 0x4000, 0xFFFF, 0xFFFE});
    m.start_cpu(0x1000, 0x8000);
    m.write32(0xDFF080, 0x3000);
    m.write16(0xDFF096, 0x8280);
    m.log_writes = true;
    m.run_frame();
    CHECK(m.regs[reg::COLOR00 >> 1] == 0x00F0);
    int first_line = -1, second_line = -1;
    for (auto& w : m.frame_writes) {
        if (w.source != 'P' || w.reg != reg::COLOR00) continue;
        if (w.value == 0x0F00) first_line = w.line;
        if (w.value == 0x00F0) second_line = w.line;
    }
    CHECK(first_line == 0);
    CHECK(second_line == 0x40);
    CHECK(m.regs[reg::BPL1PTH >> 1] == 0x0004 && m.regs[(reg::BPL1PTH >> 1) + 1] == 0x4000);
}

void test_blitter() {
    Machine m;
    m.set_timing(Timing::Fast); // zero-time blitter: BBUSY reads 0 right after BLTSIZE (timing_tests.cpp: accurate)
    m.write16(0xDFF096, 0x8240); // DMAEN | BLTEN
    // A->D copy, 2 words x 2 rows, shift A by 4, masks $FFFF/$FFF0.
    poke(m, 0x4000, {0x1234, 0x5678, 0x9ABC, 0xDEF0});
    m.write32(0xDFF050, 0x4000);
    m.write32(0xDFF054, 0x5000);
    m.write16(0xDFF064, 0);
    m.write16(0xDFF066, 0);
    m.write16(0xDFF044, 0xFFFF);
    m.write16(0xDFF046, 0xFFF0);
    m.write16(0xDFF040, 0x49F0); // ash=4, use A+D, D=A
    m.write16(0xDFF042, 0x0000);
    m.write16(0xDFF058, (2 << 6) | 2);
    CHECK(m.read16(0x5000) == 0x0123);
    CHECK(m.read16(0x5002) == 0x4567); // last word masked: $5670 -> shifted with previous $1234
    CHECK(m.read16(0x5004) == 0x09AB);  // shifter keeps the previous row's last (masked) A word $5670
    CHECK(m.read16(0x5006) == 0xCDEF);
    CHECK((m.read16(0xDFF002) & 0x6000) == 0); // not busy, not zero
    CHECK(m.read32(0xDFF054) == 0); // pointer registers are write-only (read back 0)
    // Inclusive fill, descending, one word: edges at bits 2 and 9 -> bits 2..9 set.
    m.write16(0x6000, 0x0204);
    m.write32(0xDFF050, 0x6000);
    m.write32(0xDFF054, 0x6100);
    m.write16(0xDFF044, 0xFFFF);
    m.write16(0xDFF046, 0xFFFF);
    m.write16(0xDFF040, 0x09F0);
    m.write16(0xDFF042, 0x000A); // IFE | DESC
    m.write16(0xDFF058, (1 << 6) | 1);
    CHECK(m.read16(0x6100) == 0x03FC);
}

void test_disk_shim() {
    Machine m;
    std::vector<uint8_t> disc(1760 * 512);
    for (size_t s = 0; s < 1760; ++s)
        for (size_t i = 0; i < 512; ++i) disc[s * 512 + i] = uint8_t(s ^ i);
    DiskShim shim(disc);
    shim.install(m);
    // lea $5000,a0 ; moveq #12,d0 ; moveq #3,d1 ; jsr $921A ; move.w d0,$2004 ; bra.s *
    poke(m, 0x1000, {0x41F9, 0x0000, 0x5000, 0x700C, 0x7203, 0x4EB9, 0x0000, 0x921A, 0x33C0, 0x0000, 0x2004, 0x60FE});
    poke(m, 0x921A, {0x4AFC}); // illegal: would fault if the trap did not intercept
    m.write16(0x2004, 0xBEEF);
    m.start_cpu(0x1000, 0x8000);
    m.run_cycles(2000);
    CHECK(shim.calls == 1);
    CHECK(m.read16(0x2004) == 0);
    bool ok = true;
    for (size_t i = 0; i < 3 * 512; ++i)
        if (m.read8(uint32_t(0x5000 + i)) != disc[12 * 512 + i]) ok = false;
    CHECK(ok);
    CHECK(m.read8(0x5000 + 3 * 512) == 0); // nothing beyond the request
    CHECK(m.read8(0x44000) == disc[11 * 512]); // staging buffer holds track 1 (sectors 11-21)
    CHECK(m.areg(0) == 0x5000 + 3 * 512);
    CHECK(m.pc() == 0x1016);
    CHECK(m.areg(7) == 0x8000);
}

// Coverage pokes: a plain poke, and pokeobj's walk of the hittable-object list ($B4C, next at +$12, type 0
// ends it), including a type-$B8 group whose members are reached through +$12, and a self-linked node.
void test_pokes() {
    Machine m;
    apply_poke(m, Poke{0, 0, 0xC92, 0x12345678, 4});
    CHECK(m.read32(0xC92) == 0x12345678);
    apply_poke(m, Poke{0, 0, 0xA07, 0xFF, 1});
    CHECK(m.read8(0xA07) == 0xFF && m.read8(0xA08) == 0);

    // list: $10000 (type $50, boss handler) -> $10100 ($B8 group head, +$5A = $10200) -> $10200 (member, boss
    // handler) -> $10300 (type 0: end) -> $10400 (boss handler, beyond the end: never visited)
    auto node = [&](uint32_t a, uint16_t type, uint32_t handler, uint32_t next) {
        m.write16(a, type);
        m.write32(a + 0xA, handler);
        m.write32(a + 0x12, next);
        m.write16(a + 0x32, 30);
    };
    m.write32(0xB4C, 0x10000);
    node(0x10000, 0x50, 0x561DA, 0x10100);
    node(0x10100, 0xB8, 0xE48, 0x10200);
    m.write32(0x10100 + 0x5A, 0x10200);
    node(0x10200, 0x60, 0x561DA, 0x10300);
    node(0x10300, 0, 0x561DA, 0x10400);
    node(0x10400, 0x50, 0x561DA, 0);
    CHECK(apply_poke(m, Poke{0, 0, 0x32, 1, 2, 0x561DA}) == 2);
    CHECK(m.read16(0x10032) == 1 && m.read16(0x10232) == 1);
    CHECK(m.read16(0x10132) == 30 && m.read16(0x10332) == 30 && m.read16(0x10432) == 30);

    // A node linked to itself (the obj_alloc hang of an exhausted pool) must not hang the walk.
    m.write32(0x10200 + 0x12, 0x10200);
    CHECK(apply_poke(m, Poke{0, 0, 0x32, 2, 2, 0x561DA}) == kObjWalkLimit - 1); // $10000, then $10200 on each step after $10100
    CHECK(m.read16(0x10232) == 2);
    m.write32(0xB4C, 0); // empty list
    CHECK(apply_poke(m, Poke{0, 0, 0x32, 3, 2, 0x561DA}) == 0);

    // The CDTV release keeps the list head at $B52 (BuildInfo::var_objlist): the walk follows the head it is given.
    m.write32(0xB52, 0x10000);
    m.write32(0x10200 + 0x12, 0x10300); // undo the self-link above; list: $10000 -> $10100 -> $10200 -> $10300 (type 0: end)
    m.write16(0x10032, 30);
    m.write16(0x10232, 30);
    CHECK(apply_poke(m, Poke{0, 0, 0x32, 7, 2, 0x561DA}, 0xB52) == 2);
    CHECK(m.read16(0x10032) == 7 && m.read16(0x10232) == 7);
    CHECK(apply_poke(m, Poke{0, 0, 0x32, 9, 2, 0x561DA}, 0xB4C) == 0); // $B4C is empty (0): nothing to walk
}

} // namespace

int main() {
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"bus", test_bus}, {"custom_setclr", test_custom_setclr}, {"beam_counters", test_beam_counters},
        {"cia_timer", test_cia_timer}, {"interrupts", test_interrupts}, {"keyboard", test_keyboard},
        {"copper", test_copper}, {"blitter", test_blitter}, {"disk_shim", test_disk_shim},
        {"pokes", test_pokes},
    };
    for (const T& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%-16s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
