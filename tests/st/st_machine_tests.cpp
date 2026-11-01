// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Atari ST machine (runtime/st) on SYNTHETIC inputs: the MFP's timers and interrupt controller, the WD1772 and DMA
// chip, the IKBD, the Shifter, the disk geometry rules, and the StMachine itself booting hand-assembled boot sectors
// (vector setup, VBL and Timer A interrupts, a faulting program, a disk that does not boot). No game bytes.
#include "st/fdc.h"
#include "st/ikbd.h"
#include "st/mfp.h"
#include "st/shifter.h"
#include "st/st_disk.h"
#include "st/st_machine.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using namespace x2::st;

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

uint64_t cyc_ms(uint64_t ms) { return ms * kCpuHz / 1000; }

StDisk blank_disk(int sides = 2, int spt = 10, int tracks = 80) {
    StDisk d;
    d.sides = sides;
    d.sectors_per_track = spt;
    d.tracks = tracks;
    d.data.assign(d.expected_size(), 0);
    return d;
}

// Every sector filled with a pattern that names it: (track, side, sector) in the first three bytes, then a counter.
void fill_pattern(StDisk& d) {
    for (int t = 0; t < d.tracks; ++t)
        for (int s = 0; s < d.sides; ++s)
            for (int sec = 1; sec <= d.sectors_per_track; ++sec) {
                uint8_t* p = const_cast<uint8_t*>(d.sector(t, s, sec));
                for (int i = 0; i < 512; ++i) p[i] = uint8_t(i * 7 + t + s * 3 + sec);
                p[0] = uint8_t(t);
                p[1] = uint8_t(s);
                p[2] = uint8_t(sec);
            }
}

void put_bytes(std::vector<uint8_t>& v, size_t at, std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) v[at++] = x;
}

// Makes the boot sector's word sum $1234 by setting the last word.
void fix_checksum(uint8_t* sec) {
    uint32_t sum = 0;
    for (int i = 0; i < 510; i += 2) sum += (uint32_t(sec[i]) << 8) | sec[i + 1];
    const uint32_t want = (0x1234u - sum) & 0xFFFF;
    sec[510] = uint8_t(want >> 8);
    sec[511] = uint8_t(want);
}

uint8_t* boot_sector(StDisk& d) { return const_cast<uint8_t*>(d.sector(0, 0, 1)); }

// ---------------------------------------------------------------------------------------------------------------------
void test_disk_geometry() {
    std::string why;
    StDisk d;
    // 819,200 bytes is a double-sided 80 x 10 disk even when its boot sector's BPB says one side (the second side holds data).
    std::vector<uint8_t> img(819200, 0);
    put_bytes(img, 0x0B, {0x00, 0x02, 0x02, 0x01, 0x00, 0x02, 0x70, 0x00, 0x20, 0x03, 0xF8, 0x05, 0x00, 0x0A, 0x00, 0x01, 0x00});
    CHECK(make_st_disk(img, d, why));
    CHECK(d.sides == 2 && d.sectors_per_track == 10 && d.tracks == 80);
    CHECK(d.sector(79, 1, 10) != nullptr);
    CHECK(d.sector(80, 0, 1) == nullptr && d.sector(0, 2, 1) == nullptr && d.sector(0, 0, 11) == nullptr && d.sector(0, 0, 0) == nullptr);
    // Track-interleaved layout: side 1 of track 0 follows side 0 of track 0.
    CHECK(d.sector(0, 1, 1) == d.data.data() + 10 * 512);
    CHECK(d.sector(1, 0, 1) == d.data.data() + 20 * 512);
    std::vector<uint8_t> single(368640, 0);
    CHECK(make_st_disk(single, d, why) && d.sides == 1 && d.sectors_per_track == 9 && d.tracks == 80);
    std::vector<uint8_t> dd9(737280, 0);
    CHECK(make_st_disk(dd9, d, why) && d.sides == 2 && d.sectors_per_track == 9);
    // A non-standard size falls back to the BPB; without one it is refused.
    std::vector<uint8_t> odd(512 * 1700, 0);
    CHECK(!make_st_disk(odd, d, why) && !why.empty());
    put_bytes(odd, 0x0B, {0x00, 0x02, 0x02, 0x01, 0x00, 0x02, 0x70, 0x00, 0xA4, 0x06, 0xF9, 0x05, 0x00, 0x0A, 0x00, 0x02, 0x00}); // 1700 sectors
    CHECK(make_st_disk(odd, d, why) && d.sides == 2 && d.sectors_per_track == 10 && d.tracks == 85);
    std::vector<uint8_t> tiny(1000, 0);
    CHECK(!make_st_disk(tiny, d, why));
    // The boot sector checksum.
    std::vector<uint8_t> bs(512, 0);
    CHECK(!boot_sector_executable(bs.data()));
    fix_checksum(bs.data());
    CHECK(boot_sector_executable(bs.data()));
}

// ---------------------------------------------------------------------------------------------------------------------
void test_mfp_timers() {
    Mfp m;
    // Timer A: delay mode, prescale 4 (mode 1), data 192: an underflow every 768 MFP clocks.
    m.write(3, 0x20, 0);   // IERA: timer A (channel 13 = bit 5 of IERA)
    m.write(9, 0x20, 0);   // IMRA
    m.write(11, 0x48, 0);  // VR: base $40, software end of interrupt
    m.write(15, 192, 0);   // TADR
    m.write(12, 0x01, 0);  // TACR: start, /4
    CHECK(m.timer_running(0));
    const uint64_t period = (768ull * kCpuHz + kMfpHz - 1) / kMfpHz; // ceil
    CHECK(m.next_event() == period);
    CHECK(!m.irq());
    m.advance(period - 1);
    CHECK(!m.irq() && m.timer_underflows(0) == 0);
    m.advance(period);
    CHECK(m.irq() && m.timer_underflows(0) == 1);
    CHECK(m.acknowledge() == 0x4D); // VR base $40 | channel 13: vector $134
    CHECK(!m.irq());                // the pending bit went at acknowledge
    CHECK((m.isr() & 0x2000) != 0); // software EOI: in service until the handler clears it
    // The long-run rate is exact: ten thousand underflows take 10,000 x 768 MFP clocks.
    m.write(7, 0xDF, period);       // ISRA: the handler clears channel 13's in-service bit
    CHECK((m.isr() & 0x2000) == 0);
    Mfp n;
    n.write(15, 192, 0);
    n.write(12, 0x01, 0);
    n.advance(10000ull * 768 * kCpuHz / kMfpHz);
    CHECK(n.timer_underflows(0) >= 9999 && n.timer_underflows(0) <= 10000);
    // A data value of 0 counts 256; stopping a timer stops its events.
    Mfp z;
    z.write(15, 0, 0);
    z.write(12, 0x01, 0);
    CHECK(z.next_event() == (256ull * 4 * kCpuHz + kMfpHz - 1) / kMfpHz);
    z.write(12, 0x00, 100);
    CHECK(z.next_event() == ~uint64_t(0));
}

void test_mfp_interrupts() {
    Mfp m;
    m.write(11, 0x48, 0);
    // Priorities: channel 13 (timer A) outranks channel 6 (GPIP 4, the ACIA).
    m.write(3, 0x20, 0);  // IERA bit 5: timer A
    m.write(4, 0x40, 0);  // IERB bit 6: channel 6 (GPIP 4)... the ACIA line
    m.write(9, 0x20, 0);  // IMRA
    m.write(10, 0x40, 0); // IMRB
    m.write(1, 0x00, 0);  // AER: falling edge on every input
    m.set_input(4, true, 0);
    m.set_input(4, false, 10); // a falling edge on GPIP 4: the ACIA interrupt request asserts
    CHECK(m.irq());
    CHECK(m.acknowledge() == 0x46); // $40 | 6
    // The channel is in service (software EOI): a second falling edge sets pending but the same-level service blocks it.
    m.set_input(4, true, 20);
    m.set_input(4, false, 30);
    CHECK(!m.irq());
    m.write(8, 0xBF, 40); // ISRB: the handler ends the interrupt; the pending second edge now gets through
    CHECK(m.irq());
    // Clearing IER clears the pending bit as well.
    Mfp c;
    c.write(11, 0x48, 0);
    c.write(3, 0x20, 0);
    c.write(9, 0x20, 0);
    c.write(15, 1, 0);
    c.write(12, 0x01, 0);
    c.advance(1000);
    CHECK(c.irq());
    c.write(3, 0x00, 1000);
    CHECK(!c.irq() && c.ipr() == 0);
    // Automatic end of interrupt (VR bit 3 clear): nothing is left in service.
    Mfp a;
    a.write(11, 0x40, 0);
    a.write(3, 0x20, 0);
    a.write(9, 0x20, 0);
    a.write(15, 1, 0);
    a.write(12, 0x01, 0);
    a.advance(1000);
    CHECK(a.acknowledge() == 0x4D && a.isr() == 0);
    // Spurious acknowledge: nothing pending.
    Mfp s;
    CHECK(s.acknowledge() == -1);
}

// ---------------------------------------------------------------------------------------------------------------------
struct FdcRig {
    StDisk disk;
    std::vector<uint8_t> ram;
    FdcDma fdc;
    bool intrq = false;
    uint64_t intrq_at = 0;
    FdcRig() : disk(blank_disk()), ram(0x20000, 0) {
        fill_pattern(disk);
        fdc.set_disk(&disk);
        fdc.set_ram(ram.data(), uint32_t(ram.size()));
        fdc.set_irq_callback([this](bool on, uint64_t at) { intrq = on; if (on) intrq_at = at; });
    }
    void set_dma(uint32_t a, uint64_t t) {
        fdc.write8(0x09, uint8_t(a >> 16), t);
        fdc.write8(0x0B, uint8_t(a >> 8), t);
        fdc.write8(0x0D, uint8_t(a), t);
    }
    void reg(unsigned select_word, uint16_t value, uint64_t t) { fdc.write16(0x06, uint16_t(select_word), t); fdc.write16(0x04, value, t); }
    // The boot sector loader's own sequence for "read one sector to a0".
    void read_one(uint8_t track, uint8_t sector, uint32_t addr, uint8_t cmd, uint64_t t) {
        reg(0x82, track, t);
        reg(0x84, sector, t);
        set_dma(addr, t);
        fdc.write16(0x06, 0x90, t);
        fdc.write16(0x06, 0x190, t);
        fdc.write16(0x06, 0x90, t);
        fdc.write16(0x04, 1, t); // sector count 1
        fdc.write16(0x06, 0x80, t);
        fdc.write16(0x04, cmd, t);
    }
};

void test_fdc() {
    {
        FdcRig r;
        // Restore: the head goes to track 0, INTRQ at the end, the status shows track 0.
        r.fdc.write16(0x06, 0x80, 0);
        r.fdc.write16(0x04, 0x00, 0);
        CHECK(r.fdc.busy() && !r.intrq);
        r.fdc.run_to(cyc_ms(100));
        CHECK(!r.fdc.busy() && r.intrq);
        const uint16_t st = r.fdc.read16(0x04, cyc_ms(100));
        CHECK((st & 0x04) != 0 && (st & 0x01) == 0 && !r.intrq); // track 0, not busy; the status read cleared INTRQ
        // Seek to track 25: 25 steps at 6 ms (command $10 | rate 0), finishing at 150 ms, no sooner.
        r.reg(0x86, 25, cyc_ms(200));
        r.fdc.write16(0x06, 0x80, cyc_ms(200));
        r.fdc.write16(0x04, 0x10, cyc_ms(200));
        r.fdc.run_to(cyc_ms(200 + 100));
        CHECK(r.fdc.busy() && !r.intrq);
        r.fdc.run_to(cyc_ms(200 + 160));
        CHECK(!r.fdc.busy() && r.intrq && r.fdc.head_track() == 25);
        CHECK(r.intrq_at >= cyc_ms(200 + 149) && r.intrq_at <= cyc_ms(200 + 151));
    }
    {
        FdcRig r;
        // Seek to track 7 first, then read sector 4 into $10000.
        r.reg(0x86, 7, 0);
        r.fdc.write16(0x06, 0x80, 0);
        r.fdc.write16(0x04, 0x10, 0);
        r.fdc.run_to(cyc_ms(100));
        r.fdc.read16(0x04, cyc_ms(100)); // clears INTRQ
        const uint64_t t0 = cyc_ms(100);
        r.read_one(7, 4, 0x10000, 0x80, t0);
        CHECK(r.fdc.busy() && !r.intrq);
        r.fdc.run_to(t0 + cyc_ms(10));
        CHECK(r.fdc.busy() && r.ram[0x10000] == 0 && !r.intrq);
        // Half-way through, the visible DMA address has advanced into the sector (16-byte steps).
        const uint32_t mid = (uint32_t(r.fdc.read8(0x09, t0 + cyc_ms(10))) << 16) | (uint32_t(r.fdc.read8(0x0B, t0 + cyc_ms(10))) << 8) |
                             r.fdc.read8(0x0D, t0 + cyc_ms(10));
        CHECK(mid > 0x10000 && mid < 0x10200 && (mid & 15) == 0);
        r.fdc.run_to(t0 + cyc_ms(21));
        CHECK(!r.fdc.busy() && r.intrq);
        CHECK(r.intrq_at >= t0 + cyc_ms(19) && r.intrq_at <= t0 + cyc_ms(21));
        const uint8_t* src = r.disk.sector(7, 0, 4);
        bool same = true;
        for (int i = 0; i < 512; ++i) same = same && r.ram[0x10000 + i] == src[i];
        CHECK(same);
        CHECK(r.ram[0x10200] == 0); // nothing beyond the sector
        CHECK(r.fdc.dma_address() == 0x10200 && r.fdc.dma_count() == 0);
        const uint16_t st = r.fdc.read16(0x04, t0 + cyc_ms(22));
        CHECK((st & 0x1C) == 0); // no record-not-found, CRC error or lost data
    }
    {
        FdcRig r;
        // Multi-sector read ($90): three sectors in a row from sector 3.
        r.reg(0x82, 0, 0);
        r.reg(0x84, 3, 0);
        r.set_dma(0x4000, 0);
        r.fdc.write16(0x06, 0x90, 0);
        r.fdc.write16(0x06, 0x190, 0);
        r.fdc.write16(0x06, 0x90, 0);
        r.fdc.write16(0x04, 3, 0);
        r.fdc.write16(0x06, 0x80, 0);
        r.fdc.write16(0x04, 0x90, 0);
        r.fdc.run_to(cyc_ms(70));
        CHECK(r.ram[0x4002] == 3 && r.ram[0x4202] == 4 && r.ram[0x4402] == 5);
        r.fdc.run_to(cyc_ms(300));
    }
    {
        FdcRig r;
        // A sector that does not exist: record not found after about a second.
        r.read_one(0, 11, 0x2000, 0x80, 0);
        r.fdc.run_to(cyc_ms(500));
        CHECK(r.fdc.busy());
        r.fdc.run_to(cyc_ms(1100));
        CHECK(!r.fdc.busy() && r.intrq);
        CHECK((r.fdc.read16(0x04, cyc_ms(1100)) & 0x10) != 0);
        CHECK(r.ram[0x2000] == 0);
    }
    {
        FdcRig r;
        // Side select: side 1 reads the other half of the track.
        r.fdc.set_side(1);
        r.read_one(0, 2, 0x3000, 0x80, 0);
        r.fdc.run_to(cyc_ms(30));
        CHECK(r.ram[0x3000] == 0 && r.ram[0x3001] == 1 && r.ram[0x3002] == 2);
        // Force interrupt ends a command in progress.
        r.read_one(0, 3, 0x5000, 0x80, cyc_ms(40));
        r.fdc.write16(0x06, 0x80, cyc_ms(45));
        r.fdc.write16(0x04, 0xD0, cyc_ms(45));
        CHECK(!r.fdc.busy());
        r.fdc.run_to(cyc_ms(100));
        CHECK(r.ram[0x5000] == 0);
    }
    {
        // A sector whose DMA runs past the top of RAM is stored as far as RAM goes (the Kixx level 2 loader's last read of a
        // 512 KB machine ends 16 bytes past $80000); one that starts above RAM is dropped.
        FdcRig r;
        const uint32_t top = uint32_t(r.ram.size());
        r.read_one(0, 4, top - 16, 0x80, 0);
        r.fdc.run_to(cyc_ms(30));
        const uint8_t* src = r.disk.sector(0, 0, 4);
        bool same = true;
        for (int i = 0; i < 16; ++i) same = same && r.ram[top - 16 + i] == src[i];
        CHECK(same && r.ram[top - 17] == 0);
        CHECK(r.fdc.dma_address() == top + 496 && r.fdc.dma_count() == 0);
        r.read_one(0, 5, top + 64, 0x80, cyc_ms(40));
        r.fdc.run_to(cyc_ms(80));
        CHECK(r.ram[top - 1] == src[15] && r.ram[0] == 0);
    }
    {
        // Faster than a real drive: speed 10 takes a tenth of the time.
        FdcRig r;
        r.fdc.set_speed(10);
        r.read_one(0, 1, 0x1000, 0x80, 0);
        r.fdc.run_to(cyc_ms(3));
        CHECK(!r.fdc.busy() && r.ram[0x1002] == 1);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
void test_ikbd() {
    Ikbd k;
    k.write_control(0x96, 0);
    CHECK(!k.irq());
    k.write_data(0x80, 100);
    k.write_data(0x01, 200);
    k.run_to(cyc_ms(100));
    CHECK((k.status(cyc_ms(100)) & 0x01) == 0); // the self test has not finished
    k.run_to(cyc_ms(400));
    CHECK((k.status(cyc_ms(400)) & 0x81) == 0x81 && k.irq()); // RDRF and the interrupt request
    CHECK(k.read_data(cyc_ms(400)) == 0xF1); // the HD6301 ROM version byte
    CHECK(!k.irq());
    // A joystick move reports $FF (joystick 1) and the state, one byte time apart.
    const uint64_t t = cyc_ms(500);
    k.joystick(1, 0x81, t);
    k.run_to(t + kIkbdByteCycles - 1);
    CHECK((k.status(t + kIkbdByteCycles - 1) & 0x01) == 0);
    k.run_to(t + kIkbdByteCycles);
    CHECK((k.status(t + kIkbdByteCycles) & 0x01) != 0);
    CHECK(k.read_data(t + kIkbdByteCycles) == 0xFF);
    CHECK((k.status(t + kIkbdByteCycles) & 0x01) == 0);
    k.run_to(t + 2 * kIkbdByteCycles);
    CHECK(k.read_data(t + 2 * kIkbdByteCycles) == 0x81);
    // A key press and release are single bytes ($1E, $9E).
    k.key(0x1E, false, t + cyc_ms(50));
    k.key(0x1E, true, t + cyc_ms(60));
    k.run_to(t + cyc_ms(200));
    CHECK(k.read_data(t + cyc_ms(200)) == 0x1E);
    k.run_to(t + cyc_ms(300));
    CHECK(k.read_data(t + cyc_ms(300)) == 0x9E);
    // Joystick events can be switched off ($1A) and on again ($14).
    k.write_data(0x1A, t + cyc_ms(400));
    k.joystick(1, 0x00, t + cyc_ms(410));
    k.run_to(t + cyc_ms(500));
    CHECK((k.status(t + cyc_ms(500)) & 0x01) == 0);
    CHECK(k.bytes_sent() >= 5);
}

// ---------------------------------------------------------------------------------------------------------------------
void test_shifter() {
    Shifter s;
    // The palette keeps three bits per gun.
    s.write16(0x40, 0xFFFF);
    CHECK(s.palette(0) == 0x0777);
    s.write16(0x42, 0x0123);
    CHECK(s.palette(1) == 0x0123 && s.read16(0x42, 0, 0) == 0x0123);
    // The base is written a byte at a time on the odd addresses.
    s.write8(0x01, 0x07);
    s.write8(0x03, 0xD0);
    CHECK(s.base() == 0x07D000);
    CHECK(s.is_50hz());
    // Render one pixel row: pixel 0 has plane bits 1,0,1,0 = colour 5; pixel 1 has 0,1,0,0 = colour 2.
    std::vector<uint8_t> ram(0x10000, 0);
    ram[0x100] = 0x80; // plane 0 (bytes 0-1): pixel 0 set
    ram[0x102] = 0x40; // plane 1 (bytes 2-3): pixel 1 set
    ram[0x104] = 0x80; // plane 2 (bytes 4-5): pixel 0 set
    std::array<uint16_t, 16> pal{};
    pal[5] = 0x0700; // red
    pal[2] = 0x0070; // green
    pal[0] = 0x0007; // blue
    std::vector<uint8_t> rgba;
    Shifter::render(ram.data(), uint32_t(ram.size()), 0x100, pal, 0, rgba);
    CHECK(rgba.size() == size_t(kFbWidth) * kFbHeight * 4);
    // Pixel 0: planes 0 and 2 -> colour 5 -> red; pixel 1: plane 1 -> colour 2 -> green; pixel 2: colour 0 -> blue.
    CHECK(rgba[0] == 0xFF && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 0xFF);
    CHECK(rgba[4] == 0 && rgba[5] == 0xFF && rgba[6] == 0);
    CHECK(rgba[8] == 0 && rgba[9] == 0 && rgba[10] == 0xFF);
    // The video counter advances 160 bytes per display line and is stable outside the display window.
    s.write8(0x01, 0x00);
    s.write8(0x03, 0x00);
    s.write8(0x05, 0x00);
    s.write8(0x0D, 0x00);
    const uint32_t before = s.counter(kFirstDisplayLine - 1, 100);
    const uint32_t line0_end = s.counter(kFirstDisplayLine, kLineCycles - 1);
    const uint32_t line1_mid = s.counter(kFirstDisplayLine + 1, kDisplayStartCycle + 100);
    CHECK(before == s.base());
    CHECK(line0_end == s.base() + 160);
    CHECK(line1_mid > s.base() + 160 && line1_mid < s.base() + 320);
    CHECK(s.counter(kFirstDisplayLine + kDisplayLines + 5, 0) == s.base() + 160u * kDisplayLines);
}

// ---------------------------------------------------------------------------------------------------------------------
uint32_t ram32(const StMachine& m, uint32_t a) { return m.read32_ram(a); }

// S3: the line-granular video model.
void test_video_lines() {
    Shifter s;
    s.write8(0x01, 0x01);
    s.write8(0x03, 0x00);
    s.write8(0x05, 0x00);
    // The counter is reloaded from the base only at line 310 cycle 56: a base written later shows a frame later.
    CHECK(s.counter(kFirstDisplayLine, kDisplayStartCycle) == 0);
    s.restart_counter();
    CHECK(s.counter_base() == 0x010000);
    s.write8(0x01, 0x02); // written after the reload
    CHECK(s.counter(kFirstDisplayLine, kDisplayStartCycle) == 0x010000);
    CHECK(s.counter(kFirstDisplayLine, 0) == 0x010000);               // before the display window: the line's start
    CHECK(s.counter(kFirstDisplayLine, kDisplayStartCycle + 8) == 0x010000 + 4); // one word per 4 cycles
    CHECK(s.counter(kFirstDisplayLine, kDisplayEndCycle + 20) == 0x010000 + 160); // held after the window
    CHECK(s.counter(kFirstDisplayLine + 3, kDisplayEndCycle) == 0x010000 + 3 * 160 + 160);
    CHECK(s.counter(kRestartLine, 0) == 0x010000 + 160u * kDisplayLines);

    // A palette write inside a line shows from the pixel the beam has reached (one pixel per cycle from cycle 56).
    std::vector<uint8_t> ram(0x10000, 0); // all pixels colour 0
    s.write16(0x40, 0x0000);
    s.begin_frame();
    const uint32_t line_start = uint32_t(kFirstDisplayLine) * kLineCycles;
    s.write16(0x40, 0x0700, line_start + kDisplayStartCycle + 100); // red from pixel 100 of the first display line
    std::vector<uint8_t> rgba(size_t(kFbWidth) * kFbHeight * 4, 0);
    s.render_line(ram.data(), uint32_t(ram.size()), 0, 0x100, line_start, 0, rgba);
    CHECK(rgba[99 * 4] == 0 && rgba[99 * 4 + 1] == 0 && rgba[99 * 4 + 2] == 0);
    CHECK(rgba[100 * 4] == 0xFF && rgba[100 * 4 + 1] == 0 && rgba[319 * 4] == 0xFF);
    // The next line starts red (the palette persists), and a write between lines applies from the next line's pixel 0.
    s.write16(0x40, 0x0070, line_start + kLineCycles + 10);
    s.render_line(ram.data(), uint32_t(ram.size()), 1, 0x100, line_start + kLineCycles, 0, rgba);
    CHECK(rgba[320 * 4] == 0 && rgba[320 * 4 + 1] == 0xFF);
    // Medium resolution gives a black line.
    s.render_line(ram.data(), uint32_t(ram.size()), 2, 0x100, line_start + 2 * kLineCycles, 1, rgba);
    CHECK(rgba[640 * 4] == 0 && rgba[640 * 4 + 1] == 0 && rgba[640 * 4 + 3] == 0xFF);
}

// A boot sector that runs `body` forever: addq.l #1,($608).w counts the iterations; the loop ends in bra.s.
StDisk loop_disk(const std::vector<uint8_t>& body) {
    StDisk d = blank_disk();
    std::vector<uint8_t> code;
    code.insert(code.end(), {0x52, 0xB8, 0x06, 0x08}); // addq.l #1,($608).w
    code.insert(code.end(), body.begin(), body.end());
    code.push_back(0x60);
    code.push_back(uint8_t(-int(code.size() + 1)));    // bra.s to the start
    std::copy(code.begin(), code.end(), boot_sector(d));
    fix_checksum(boot_sector(d));
    return d;
}

uint32_t loop_count(const std::vector<uint8_t>& body, StConfig cfg, uint64_t* waits = nullptr, uint64_t* rounded = nullptr) {
    StMachine m(cfg);
    m.insert_disk(loop_disk(body));
    std::string why;
    if (!m.boot(why)) { ++g_failures; return 0; }
    for (int i = 0; i < 4; ++i) m.run_frame();
    if (waits) *waits = m.io_wait_cycles();
    if (rounded) *rounded = m.rounded_instructions();
    return ram32(m, 0x608);
}

void test_cpu_timing() {
    // Bus rounding: the loop's instructions are 20 + 10 cycles; every instruction ends on a 4-cycle boundary.
    StConfig on, off;
    off.bus_rounding = false;
    uint64_t rounded = 0, none = 0;
    const uint32_t n_on = loop_count({}, on, nullptr, &rounded);
    const uint32_t n_off = loop_count({}, off, nullptr, &none);
    CHECK(n_off > n_on && none == 0 && rounded > 0);
    // A bare iteration is a multiple of 4 cycles with rounding: the count fits 4 frames of cycles to within the start-up.
    const double per = 4.0 * kCyclesPerFrame / n_on;
    CHECK(std::abs(per / 4.0 - std::round(per / 4.0)) < 0.02 && per > 28 && per < 40);

    // Device wait states: +4 per YM2149 access (once per instruction), +4 per MFP register access, 6 plus the E-clock wait
    // for the ACIA.
    uint64_t w = 0;
    uint32_t n = loop_count({0x13, 0xC0, 0x00, 0xFF, 0x88, 0x00}, on, &w); // move.b d0,$FF8800
    CHECK(n > 1000 && w >= 4ull * (n - 1) && w <= 4ull * (n + 1));
    n = loop_count({0x12, 0x39, 0x00, 0xFF, 0xFA, 0x01}, on, &w);          // move.b $FFFA01,d1
    CHECK(n > 1000 && w >= 4ull * (n - 1) && w <= 4ull * (n + 1));
    n = loop_count({0x12, 0x39, 0x00, 0xFF, 0xFC, 0x00}, on, &w);          // move.b $FFFC00,d1
    CHECK(n > 1000 && w >= 6ull * (n - 1) && w <= 16ull * (n + 1));
    StConfig nowait;
    nowait.io_waits = false;
    n = loop_count({0x13, 0xC0, 0x00, 0xFF, 0x88, 0x00}, nowait, &w);
    CHECK(w == 0 && n > 1000);
    // The waits slow the loop down.
    CHECK(loop_count({0x13, 0xC0, 0x00, 0xFF, 0x88, 0x00}, on) < n);
}

// ---------------------------------------------------------------------------------------------------------------------
// Hand-assembled boot sector (loaded and started at $70000, supervisor mode, SR $2300): installs a VBL handler and a Timer A
// handler that count in low memory, programs the MFP, sets a palette entry, lowers the interrupt mask and loops.
StDisk interrupt_disk() {
    StDisk d = blank_disk();
    std::vector<uint8_t> code(512, 0);
    size_t p = 0;
    auto emit = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) code[p++] = x; };
    emit({0x21, 0xFC, 0x00, 0x07, 0x01, 0x00, 0x00, 0x70});       // move.l #$70100,$70.w      (VBL)
    emit({0x21, 0xFC, 0x00, 0x07, 0x01, 0x20, 0x01, 0x34});       // move.l #$70120,$134.w     (Timer A)
    emit({0x11, 0xFC, 0x00, 0x20, 0xFA, 0x13});                   // move.b #$20,$FFFA13       IMRA
    emit({0x11, 0xFC, 0x00, 0x20, 0xFA, 0x07});                   // move.b #$20,$FFFA07       IERA
    emit({0x11, 0xFC, 0x00, 0xC0, 0xFA, 0x1F});                   // move.b #192,$FFFA1F       TADR
    emit({0x11, 0xFC, 0x00, 0x01, 0xFA, 0x19});                   // move.b #1,$FFFA19         TACR: /4
    emit({0x33, 0xFC, 0x07, 0x77, 0x00, 0xFF, 0x82, 0x40});       // move.w #$777,$FF8240      palette 0 white
    emit({0x46, 0xFC, 0x20, 0x00});                               // move.w #$2000,sr
    emit({0x60, 0xFE});                                           // bra.s *
    p = 0x100;
    emit({0x52, 0xB8, 0x06, 0x08, 0x4E, 0x73});                   // addq.l #1,($608).w ; rte
    p = 0x120;
    emit({0x52, 0xB8, 0x06, 0x0C, 0x11, 0xFC, 0x00, 0xDF, 0xFA, 0x0F, 0x4E, 0x73}); // addq.l #1,($60C).w; ISRA bit 5 clear; rte
    std::copy(code.begin(), code.end(), boot_sector(d));
    fix_checksum(boot_sector(d));
    return d;
}


void test_machine_interrupts() {
    StMachine m;
    m.insert_disk(interrupt_disk());
    std::string why;
    CHECK(m.boot(why));
    CHECK(m.pc() == kBootBase);
    CHECK(ram32(m, 0x420) == 0x752019F3 && ram32(m, 0x43A) == 0x237698AA); // TOS's memory-valid magics
    CHECK(ram32(m, 0x42E) == m.ram_size());
    for (int i = 0; i < 10; ++i) m.run_frame();
    CHECK(m.frame() == 10);
    // The first vertical blank arrives before the boot code has installed its handler and lands on the stub's RTE.
    CHECK(ram32(m, 0x608) == 9 && m.vbl_taken() == 10);
    // 768 MFP clocks = 2506.4 CPU cycles per interrupt: 63.95 a frame.
    const uint32_t timer_a = ram32(m, 0x60C);
    CHECK(timer_a >= 636 && timer_a <= 642);
    CHECK(m.mfp_taken() == timer_a);
    CHECK(m.shifter().palette(0) == 0x0777);
    CHECK(m.picture().size() == size_t(kFbWidth) * kFbHeight * 4);
    CHECK(m.picture()[0] == 0xFF && m.picture()[1] == 0xFF && m.picture()[2] == 0xFF); // white background: video RAM is zero
    CHECK(m.fatal_vector_hits() == 0 && m.unmapped_accesses() == 0);
    // Frame boundaries are absolute: only the instruction in flight at the end of the last frame overshoots.
    CHECK(m.cycles() >= 10ull * kCyclesPerFrame && m.cycles() < 10ull * kCyclesPerFrame + 64);
    // Deterministic: a second machine reaches the same picture hash.
    StMachine m2;
    m2.insert_disk(interrupt_disk());
    CHECK(m2.boot(why));
    for (int i = 0; i < 10; ++i) m2.run_frame();
    CHECK(m2.picture_hash() == m.picture_hash());
    CHECK(ram32(m2, 0x60C) == timer_a);
}

void test_machine_faults() {
    {
        StDisk d = blank_disk();
        put_bytes(d.data, 0, {0x4A, 0xFC}); // illegal instruction
        fix_checksum(boot_sector(d));
        StMachine m;
        m.insert_disk(std::move(d));
        std::string why;
        CHECK(m.boot(why));
        m.run_frame();
        CHECK(m.fatal_vector_hits() >= 1);
        CHECK(!m.fault_report().empty());
    }
    {
        StDisk d = blank_disk(); // checksum wrong
        StMachine m;
        m.insert_disk(std::move(d));
        std::string why;
        CHECK(!m.boot(why));
        CHECK(why.find("checksum") != std::string::npos || why.find("executable") != std::string::npos);
    }
    {
        StMachine m; // no disk
        std::string why;
        CHECK(!m.boot(why) && !why.empty());
    }
    {
        // A read of nothing: $F00000 is unmapped; the machine counts it instead of faulting.
        StDisk d = blank_disk();
        put_bytes(d.data, 0, {0x20, 0x39, 0x00, 0xF0, 0x00, 0x00, 0x60, 0xFE}); // move.l $F00000,d0 ; bra.s *
        fix_checksum(boot_sector(d));
        StMachine m;
        m.insert_disk(std::move(d));
        std::string why;
        CHECK(m.boot(why));
        m.run_frame();
        CHECK(m.unmapped_accesses() >= 1 && m.fatal_vector_hits() == 0);
    }
}

// The boot sector reads sector 5 of track 2 through the real controller registers and the DMA chip, waiting on the MFP's
// GPIP bit 5, then stores a marker; the machine's FDC, DMA, MFP input and scheduler have to cooperate for it to finish.
void test_machine_fdc_read() {
    StDisk d = blank_disk();
    fill_pattern(d);
    std::vector<uint8_t> code(512, 0);
    size_t p = 0;
    auto emit = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) code[p++] = x; };
    emit({0x4D, 0xF8, 0x86, 0x06});                    // lea $FF8606.w,a6   (the DMA mode register)
    emit({0x4B, 0xF8, 0x86, 0x04});                    // lea $FF8604.w,a5   (data/command)
    emit({0x3C, 0xBC, 0x00, 0x82});                    // move.w #$82,(a6)   track register
    emit({0x3A, 0xBC, 0x00, 0x00});                    // move.w #0,(a5)     (the head is at track 0)
    emit({0x3C, 0xBC, 0x00, 0x86});                    // move.w #$86,(a6)   data register
    emit({0x3A, 0xBC, 0x00, 0x02});                    // move.w #2,(a5)
    emit({0x3C, 0xBC, 0x00, 0x80});                    // move.w #$80,(a6)
    emit({0x3A, 0xBC, 0x00, 0x10});                    // move.w #$10,(a5)   seek to the data register (track 2)
    emit({0x08, 0x38, 0x00, 0x05, 0xFA, 0x01});        // wait: btst #5,$FFFA01
    emit({0x66, 0xF8});                                // bne.s wait  (INTRQ is active low: bit 5 clear when done)
    emit({0x3C, 0xBC, 0x00, 0x84});                    // move.w #$84,(a6)   sector register
    emit({0x3A, 0xBC, 0x00, 0x05});                    // move.w #5,(a5)
    emit({0x11, 0xFC, 0x00, 0x00, 0x86, 0x09});        // move.b #0,$FF8609.w
    emit({0x11, 0xFC, 0x00, 0x08, 0x86, 0x0B});        // move.b #$08,$FF860B.w   address $0800
    emit({0x11, 0xFC, 0x00, 0x00, 0x86, 0x0D});        // move.b #0,$FF860D.w
    emit({0x3C, 0xBC, 0x00, 0x90});                    // move.w #$90,(a6)
    emit({0x3C, 0xBC, 0x01, 0x90});                    // move.w #$190,(a6)
    emit({0x3C, 0xBC, 0x00, 0x90});                    // move.w #$90,(a6)
    emit({0x3A, 0xBC, 0x00, 0x01});                    // move.w #1,(a5)     one sector
    emit({0x3C, 0xBC, 0x00, 0x80});                    // move.w #$80,(a6)
    emit({0x3A, 0xBC, 0x00, 0x80});                    // move.w #$80,(a5)   read sector
    const size_t wait2 = p;
    emit({0x08, 0x38, 0x00, 0x05, 0xFA, 0x01});        // wait: btst #5,$FFFA01
    emit({0x66, 0xF8});                                // bne.s wait
    (void)wait2;
    emit({0x21, 0xFC, 0xCA, 0xFE, 0xF0, 0x0D, 0x06, 0x00}); // move.l #$CAFEF00D,$600.w
    emit({0x60, 0xFE});
    std::copy(code.begin(), code.end(), boot_sector(d));
    fix_checksum(boot_sector(d));
    StMachine m;
    m.insert_disk(std::move(d));
    std::string why;
    CHECK(m.boot(why));
    m.fdc().log_reads(true);
    for (int i = 0; i < 40; ++i) m.run_frame();
    CHECK(ram32(m, 0x600) == 0xCAFEF00D);
    const uint8_t* ram = m.ram();
    CHECK(ram[0x800] == 2 && ram[0x801] == 0 && ram[0x802] == 5); // track 2, side 0, sector 5
    CHECK(m.fdc().read_log().size() == 1 && m.fdc().read_log()[0].track == 2 && m.fdc().read_log()[0].sector == 5);
    CHECK(m.fatal_vector_hits() == 0);
}

} // namespace

int main() {
    test_disk_geometry();
    test_mfp_timers();
    test_mfp_interrupts();
    test_fdc();
    test_ikbd();
    test_shifter();
    test_video_lines();
    test_machine_interrupts();
    test_cpu_timing();
    test_machine_faults();
    test_machine_fdc_read();
    std::printf("st tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
