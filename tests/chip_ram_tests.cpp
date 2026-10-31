// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Chip RAM size tests on synthetic memory (no game data): a Machine is 512 KB by default and 1 MB when asked. The
// 512 KB model keeps its mirror at $80000 and its 19-bit DMA pointers; the 1 MB model has real RAM there and
// 20-bit pointers. Covered: construction, the CPU bus mapping, blitter, copper, bitplane (Video) and audio DMA
// pointers above $7FFFF, and the difference between the two models for the same program.
#include "audio/paula.h"
#include "gfx/video.h"
#include "machine.h"

#include <cstdio>
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
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

constexpr uint32_t k512 = 0x80000, k1M = 0x100000;

void poke(Machine& m, uint32_t addr, std::initializer_list<uint16_t> words) {
    for (uint16_t w : words) { m.write16(addr, w); addr += 2; }
}
void idle_cpu(Machine& m) {
    poke(m, 0x1000, {0x60FE}); // bra.s *
    m.start_cpu(0x1000, 0x8000);
}
void ptr_write(Machine& m, uint16_t reg_off, uint32_t addr) {
    m.write16(uint32_t(0xDFF000 + reg_off), uint16_t(addr >> 16));
    m.write16(uint32_t(0xDFF000 + reg_off + 2), uint16_t(addr));
}

void test_construction() {
    Machine d;
    CHECK(d.chip_size == k512 && d.chip_mask == k512 - 1 && d.chip.size() == k512);
    Machine a(k512);
    CHECK(a.chip_size == k512 && a.chip_mask == 0x7FFFF);
    Machine b(k1M);
    CHECK(b.chip_size == k1M && b.chip_mask == 0xFFFFF && b.chip.size() == k1M);
    // Anything else is the default size.
    Machine c(0x40000), e(0x200000), z(0), o(k1M + 1);
    CHECK(c.chip_size == k512 && e.chip_size == k512 && z.chip_size == k512 && o.chip_size == k512);
    CHECK(valid_chip_size(k512) && valid_chip_size(k1M) && !valid_chip_size(0x180000) && !valid_chip_size(0));
}

void test_bus_512() {
    Machine m;
    m.write32(0x1000, 0x11223344);
    CHECK(m.read32(0x81000) == 0x11223344);  // mirror at $80000
    CHECK(m.read32(0x181000) == 0x11223344); // and every 512 KB up to $1FFFFF
    m.write16(0x90000, 0xBEEF);
    CHECK(m.read16(0x10000) == 0xBEEF);      // a write above $7FFFF lands on the mirror
    const uint8_t b[2] = {0xAA, 0xBB};
    m.load(0x80010, b, 2);
    CHECK(m.read16(0x10) == 0xAABB);
    m.write16(0x7FFFF, 0x1234); // a word straddling the top wraps to $00000
    CHECK(m.read8(0x7FFFF) == 0x12 && m.read8(0x0) == 0x34);
    m.read8(0x200000);          // beyond chip space: unmapped, as before
    CHECK(m.unmapped_accesses == 1);
}

void test_bus_1m() {
    Machine m(k1M);
    m.write32(0x1000, 0x11223344);
    CHECK(m.read32(0x81000) == 0);           // real, separate RAM at $80000
    CHECK(m.read32(0x101000) == 0x11223344); // mirror every 1 MB
    m.write16(0x90000, 0xBEEF);
    CHECK(m.read16(0x90000) == 0xBEEF && m.read16(0x10000) == 0);
    CHECK(m.read16(0x190000) == 0xBEEF);     // $1xxxxx mirrors $0xxxxx
    const uint8_t b[2] = {0xAA, 0xBB};
    m.load(0x80010, b, 2);
    CHECK(m.read16(0x80010) == 0xAABB && m.read16(0x10) == 0);
    m.write16(0xFFFFF, 0x1234);
    CHECK(m.read8(0xFFFFF) == 0x12 && m.read8(0x0) == 0x34);
    // The CPU runs code above $7FFFF: move.w #$1234,$A0000 ; bra.s *
    poke(m, 0xC0000, {0x33FC, 0x1234, 0x000A, 0x0000, 0x60FE});
    m.start_cpu(0xC0000, 0x8000);
    m.run_frame();
    CHECK(m.read16(0xA0000) == 0x1234 && m.read16(0x20000) == 0);
    CHECK(m.pc() == 0xC0008); // spinning on the bra.s
    m.read8(0x200000);
    CHECK(m.unmapped_accesses == 1);
}

// A -> D copy of two words with the source and destination pointers programmed with high words that need the 4th bit.
void blit_copy(Machine& m, uint32_t src, uint32_t dst) {
    m.set_timing(Timing::Fast);
    m.write16(0xDFF096, 0x8240); // DMAEN | BLTEN
    ptr_write(m, reg::BLTAPTH, src);
    ptr_write(m, reg::BLTDPTH, dst);
    m.write16(0xDFF064, 0);
    m.write16(0xDFF066, 0);
    m.write16(0xDFF044, 0xFFFF);
    m.write16(0xDFF046, 0xFFFF);
    m.write16(0xDFF040, 0x09F0); // A -> D
    m.write16(0xDFF042, 0x0000);
    m.write16(0xDFF058, (1 << 6) | 2);
}

void test_blitter_pointers() {
    {   // 1 MB: source $90000, destination $A0000, and the sources at the 512 KB aliases hold other data.
        Machine m(k1M);
        poke(m, 0x90000, {0x1357, 0x2468});
        poke(m, 0x10000, {0xDEAD, 0xBEEF}); // what a 19-bit pointer would read
        blit_copy(m, 0x90000, 0xA0000);
        CHECK(m.read16(0xA0000) == 0x1357 && m.read16(0xA0002) == 0x2468);
        CHECK(m.read16(0x20000) == 0);
        // The pointer registers keep the 4-bit high word after the blit (A advanced by 4 bytes).
        CHECK(m.regs[reg::BLTAPTH >> 1] == 0x9 && m.regs[(reg::BLTAPTH >> 1) + 1] == 0x0004);
        CHECK(m.regs[reg::BLTDPTH >> 1] == 0xA && m.regs[(reg::BLTDPTH >> 1) + 1] == 0x0004);
    }
    {   // 512 KB: the same program reads and writes the aliases, and the high word is 3 bits.
        Machine m;
        poke(m, 0x10000, {0xDEAD, 0xBEEF});
        blit_copy(m, 0x90000, 0xA0000);
        CHECK(m.read16(0x20000) == 0xDEAD && m.read16(0x20002) == 0xBEEF);
        CHECK(m.read16(0xA0000) == 0xDEAD); // the same memory through the mirror
        CHECK(m.regs[reg::BLTAPTH >> 1] == 0x1 && m.regs[(reg::BLTAPTH >> 1) + 1] == 0x0004);
        CHECK(m.regs[reg::BLTDPTH >> 1] == 0x2 && m.regs[(reg::BLTDPTH >> 1) + 1] == 0x0004);
    }
    {   // 512 KB: a high word of $F reaches $70000, not $F0000.
        Machine m;
        poke(m, 0x70000, {0x0A0B, 0x0C0D});
        blit_copy(m, 0xF0000, 0x5000);
        CHECK(m.read16(0x5000) == 0x0A0B && m.read16(0x5002) == 0x0C0D);
    }
    {   // 1 MB: a high word of $F reaches $F0000.
        Machine m(k1M);
        poke(m, 0xF0000, {0x0A0B, 0x0C0D});
        poke(m, 0x70000, {0x1111, 0x2222});
        blit_copy(m, 0xF0000, 0x5000);
        CHECK(m.read16(0x5000) == 0x0A0B && m.read16(0x5002) == 0x0C0D);
    }
}

// A copper list that sets COLOR00, jumped to through COP1LC.
void run_copper(Machine& m, uint32_t list) {
    idle_cpu(m);
    ptr_write(m, reg::COP1LCH, list);
    m.write16(0xDFF096, 0x8280); // DMAEN | COPEN
    m.write16(0xDFF088, 0);      // COPJMP1
    m.run_frame();
}

void test_copper_lists() {
    {   // 1 MB: two different lists 512 KB apart.
        Machine m(k1M);
        poke(m, 0x10000, {0x0180, 0x0111, 0xFFFF, 0xFFFE});
        poke(m, 0x90000, {0x0180, 0x0222, 0xFFFF, 0xFFFE});
        run_copper(m, 0x90000);
        CHECK(m.regs[reg::COLOR00 >> 1] == 0x0222);
        Machine n(k1M);
        poke(n, 0x10000, {0x0180, 0x0111, 0xFFFF, 0xFFFE});
        poke(n, 0x90000, {0x0180, 0x0222, 0xFFFF, 0xFFFE});
        run_copper(n, 0x10000);
        CHECK(n.regs[reg::COLOR00 >> 1] == 0x0111);
    }
    {   // 512 KB: the second list overwrote the first through the mirror, and $90000 reaches it.
        Machine m;
        poke(m, 0x10000, {0x0180, 0x0111, 0xFFFF, 0xFFFE});
        poke(m, 0x90000, {0x0180, 0x0222, 0xFFFF, 0xFFFE});
        run_copper(m, 0x90000);
        CHECK(m.regs[reg::COLOR00 >> 1] == 0x0222);
        CHECK(m.read16(0x10002) == 0x0222);
    }
    {   // 1 MB: a list that runs across $7FFFE into $80000 keeps fetching real RAM (no wrap to 0).
        Machine m(k1M);
        poke(m, 0x7FFFC, {0x0180, 0x0333});
        poke(m, 0x80000, {0x0182, 0x0444, 0xFFFF, 0xFFFE});
        poke(m, 0x0, {0x0182, 0x0999, 0xFFFF, 0xFFFE}); // where a 19-bit copper would wrap to
        run_copper(m, 0x7FFFC);
        CHECK(m.regs[reg::COLOR00 >> 1] == 0x0333 && m.regs[(reg::COLOR00 >> 1) + 1] == 0x0444);
    }
}

// Bitplane DMA: one plane of ones read from kHigh makes the whole window colour 1.
struct Screen {
    Machine m;
    Video v;
    explicit Screen(uint32_t chip) : m(chip) {
        idle_cpu(m);
        v.attach(m);
    }
    void show(uint32_t plane_ptr) {
        m.write16(0xDFF08E, 0x2C81);
        m.write16(0xDFF090, 0x2CC1);
        m.write16(0xDFF092, 0x0038);
        m.write16(0xDFF094, 0x00D0);
        m.write16(0xDFF108, 0);
        m.write16(0xDFF10A, 0);
        m.write16(0xDFF100, 0x1200);
        m.write16(0xDFF182, 0x0FFF); // COLOR01 white
        m.write16(0xDFF180, 0x0000);
        // the copper restores the pointer every frame, as a game's list does
        poke(m, 0x2000, {uint16_t(reg::BPL1PTH), uint16_t(plane_ptr >> 16), uint16_t(reg::BPL1PTH + 2), uint16_t(plane_ptr),
                         0xFFFF, 0xFFFE});
        ptr_write(m, reg::COP1LCH, 0x2000);
        m.write16(0xDFF096, 0x8000 | reg::DMAF_MASTER | reg::DMAF_COPPER | reg::DMAF_RASTER);
        m.write16(0xDFF088, 0);
        m.run_frame();
        m.run_frame();
    }
    uint32_t pixel(int x, int y) const {
        const auto& f = v.front();
        size_t o = (size_t(y) * Video::kFbWidth + size_t(x)) * 4;
        return uint32_t(f[o]) | uint32_t(f[o + 1]) << 8 | uint32_t(f[o + 2]) << 16 | uint32_t(f[o + 3]) << 24;
    }
};

void fill_plane(Machine& m, uint32_t base, uint16_t word) {
    for (uint32_t i = 0; i < 40 * 256; i += 2) m.write16(base + i, word);
}

void test_bitplane_dma() {
    const uint32_t white = Video::rgba_of(0x0FFF), black = Video::rgba_of(0x0000);
    {   // 1 MB: the plane lives at $A0000; the 512 KB alias $20000 holds zeros.
        Screen s(k1M);
        fill_plane(s.m, 0xA0000, 0xFFFF);
        s.show(0xA0000);
        CHECK(s.m.display.valid && s.m.display.bplpt[0] >= 0xA0000 && s.m.display.bplpt[0] < 0xA0000 + 0x3000);
        CHECK(s.pixel(10, 10) == white && s.pixel(300, 200) == white);
    }
    {   // 1 MB: pointing at the alias shows black (the data is not there).
        Screen s(k1M);
        fill_plane(s.m, 0xA0000, 0xFFFF);
        s.show(0x20000);
        CHECK(s.pixel(10, 10) == black);
    }
    {   // 512 KB: $A0000 is the mirror of $20000: the same bytes.
        Screen s(k512);
        fill_plane(s.m, 0xA0000, 0xFFFF);
        s.show(0xA0000);
        CHECK(s.m.display.bplpt[0] < 0x80000);
        CHECK(s.pixel(10, 10) == white);
        Screen t(k512);
        fill_plane(t.m, 0x20000, 0xFFFF);
        t.show(0xA0000);
        CHECK(t.pixel(10, 10) == white);
    }
}

void test_audio_dma() {
    // One channel looping a two-byte sample: $40,$40 at $A0000 and $10,$10 at the alias $20000.
    auto run = [](uint32_t chip, uint32_t lc) {
        Machine m(chip);
        m.write16(0xA0000, 0x4040); // note: in the 512 KB model this is $20000
        if (chip == k1M) m.write16(0x20000, 0x1010);
        ptr_write(m, reg::AUD0LCH, lc);
        m.write16(0xDFF0A4, 1);       // length 1 word
        m.write16(0xDFF0A6, 200);     // period
        m.write16(0xDFF0A8, 64);      // volume
        m.write16(0xDFF096, 0x8201);  // DMAEN | AUD0EN
        m.paula.run_until(m.paula.now() + 1000);
        return m.paula.channel_sample(0);
    };
    const int hi = run(k1M, 0xA0000), alias = run(k1M, 0x20000);
    CHECK(hi != 0 && alias != 0 && hi != alias);
    CHECK(hi > alias);
    const int mirrored = run(k512, 0xA0000);
    CHECK(mirrored == hi); // 512 KB: $A0000 is $20000, which holds the $40 data
}

} // namespace

int main() {
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"construction", test_construction}, {"bus_512", test_bus_512}, {"bus_1m", test_bus_1m},
        {"blitter_pointers", test_blitter_pointers}, {"copper_lists", test_copper_lists},
        {"bitplane_dma", test_bitplane_dma}, {"audio_dma", test_audio_dma},
    };
    for (const T& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%-18s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
