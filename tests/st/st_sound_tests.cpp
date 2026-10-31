// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The ST's sound and input on SYNTHETIC inputs: the YM2149 generator (tone, noise, envelope, volume steps, sample-accurate
// register writes, decimation, determinism), the IKBD's pause/resume and command rules, the input script reader (the
// project's format and the Hatari harness's), the script player driving a machine, and the key code table. No game bytes.
#include "st/ikbd.h"
#include "st/st_input.h"
#include "st/st_machine.h"
#include "st/ym2149.h"

#include <cstdio>
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

// The CPU cycle that is the start of YM tick `tick` (the smallest cycle whose tick index is `tick`).
uint64_t cycle_of_tick(uint64_t tick) { return (tick * kCpuHz + kYmTickHz - 1) / kYmTickHz; }

void wr(Ym2149& y, int reg, int val, uint64_t cycle) {
    y.write8(0, uint8_t(reg), cycle);
    y.write8(2, uint8_t(val), cycle);
}

// A generator that outputs one sample per tick, unfiltered: sample = (mix * 5) >> 3 with mix = the sum of the channel levels / 3.
constexpr int kTick = 1;
int16_t level1(int step5) { return int16_t((uint32_t(Ym2149::dac_level(step5)) / 3 * 5) >> 3); }
int16_t level_of(uint32_t mix) { return int16_t((mix / 3 * 5) >> 3); }

Ym2149 raw() {
    Ym2149 y;
    y.set_sound(kYmTickHz, false);
    return y;
}

void render_ticks(Ym2149& y, uint64_t ticks) { y.render_to(cycle_of_tick(y.ticks_done() + ticks)); }

// ---------------------------------------------------------------------------------------------------------------------
void test_tables() {
    CHECK(Ym2149::dac_level(0) == 0);
    CHECK(Ym2149::dac_level(31) == 65535);
    for (int i = 1; i < 32; ++i) CHECK(Ym2149::dac_level(i) > Ym2149::dac_level(i - 1));
    // The conversion from CPU cycles to ticks: 8,021,247 cycles are one second, 250,000 ticks.
    CHECK(Ym2149::tick_of(0) == 0);
    CHECK(Ym2149::tick_of(kCpuHz) == kYmTickHz);
    CHECK(Ym2149::tick_of(cycle_of_tick(12345)) == 12345);
    CHECK(Ym2149::tick_of(cycle_of_tick(12345) - 1) == 12344);
}

void test_registers_and_mirrors() {
    Ym2149 y;
    wr(y, 1, 0xFF, 0);
    CHECK(y.reg(1) == 0xFF);
    y.write8(0, 1, 0);
    CHECK(y.read8(0) == 0x0F); // register 1 has four bits
    y.write8(4, 1, 0);         // the chip mirrors every four bytes
    CHECK(y.read8(4) == 0x0F);
    CHECK(y.read8(1) == 0xFF); // odd addresses read as open bus
    y.write8(0, 7, 0);
    CHECK(y.read8(0) == 0xFF); // reset: every channel off
}

void test_tone() {
    for (int period : {1, 2, 7, 100, 4095}) {
        Ym2149 y = raw();
        wr(y, 0, period & 0xFF, 0);
        wr(y, 1, period >> 8, 0);
        wr(y, 7, 0x3E, 0); // tone A on, everything else off
        wr(y, 8, 15, 0);   // full volume
        y.samples().clear();
        render_ticks(y, uint64_t(period) * 8);
        const auto& s = y.samples();
        CHECK(s.size() == uint64_t(period) * 8);
        // The first tick toggles at count == period, so the output is high for `period` ticks, low for `period`, ...
        int toggles = 0, run = 0, first_run = 0;
        for (size_t i = 1; i < s.size(); ++i) {
            if (s[i] != s[i - 1]) {
                ++toggles;
                if (!first_run) first_run = run + 1;
                run = 0;
            } else ++run;
        }
        CHECK(toggles >= 7 && toggles <= 8);
        CHECK(s[0] == 0 || s[0] == level1(31) / 1);
        bool two_levels = true;
        for (int16_t v : s)
            if (v != 0 && v != level1(31)) two_levels = false;
        CHECK(two_levels);
        if (period > 1) CHECK(first_run == period || first_run == period - 1 || first_run == period + 1);
    }
    // Period 0 counts as 1.
    Ym2149 a = raw(), b = raw();
    for (Ym2149* y : {&a, &b}) {
        wr(*y, 7, 0x3E, 0);
        wr(*y, 8, 15, 0);
    }
    wr(b, 0, 1, 0);
    render_ticks(a, 400);
    render_ticks(b, 400);
    CHECK(a.samples() == b.samples());
}

void test_volume_steps() {
    // Mixer all disabled: the output is the channel level itself (the digi-sample technique).
    for (int v = 0; v < 16; ++v) {
        Ym2149 y = raw();
        wr(y, 7, 0x3F, 0);
        wr(y, 9, v, 0);
        render_ticks(y, 10);
        static const int map4to5[16] = {0, 1, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31};
        CHECK(!y.samples().empty() && y.samples().back() == level1(map4to5[v]));
    }
    // Two channels add (a third each).
    Ym2149 y = raw();
    wr(y, 7, 0x3F, 0);
    wr(y, 8, 15, 0);
    wr(y, 10, 15, 0);
    render_ticks(y, 4);
    CHECK(y.samples().back() == level_of(2u * Ym2149::dac_level(31)));
}

void test_noise() {
    // Noise only on channel C at full volume, period 1: the LFSR (17 bits, taps 17 and 14) shifts every 2 ticks.
    Ym2149 y = raw();
    wr(y, 7, 0x37, 0); // tone off everywhere (bits 0-2), noise on for C only (bit 5 clear), A and B noise off
    wr(y, 10, 15, 0);
    wr(y, 6, 1, 0);
    render_ticks(y, 400);
    uint32_t rnd = 1;
    std::vector<int16_t> want;
    int half = 0, count = 0;
    uint8_t val = 0;
    for (int t = 0; t < 400; ++t) {
        half ^= 1;
        if (!half) ++count;
        if (count >= 1) {
            count = 0;
            if (rnd & 1) { rnd = (rnd >> 1) ^ 0x12000; val = 1; } else { rnd >>= 1; val = 0; }
        }
        want.push_back(val ? level_of(Ym2149::dac_level(31)) : int16_t(0));
    }
    CHECK(y.samples().size() == want.size());
    CHECK(y.samples() == want);
    // A longer period slows it: period 4 shifts every 8 ticks, so runs are multiples of 8 ticks.
    Ym2149 z = raw();
    wr(z, 7, 0x37, 0);
    wr(z, 10, 15, 0);
    wr(z, 6, 4, 0);
    render_ticks(z, 2000);
    int changes = 0;
    for (size_t i = 1; i < z.samples().size(); ++i) changes += z.samples()[i] != z.samples()[i - 1];
    Ym2149 w = raw();
    wr(w, 7, 0x37, 0);
    wr(w, 10, 15, 0);
    wr(w, 6, 1, 0);
    render_ticks(w, 2000);
    int changes1 = 0;
    for (size_t i = 1; i < w.samples().size(); ++i) changes1 += w.samples()[i] != w.samples()[i - 1];
    CHECK(changes < changes1 && changes > 0);
}

void test_envelope() {
    // Shape 0 (\___), period 1: one step per tick through the 32 levels, then silence.
    {
        Ym2149 y = raw();
        wr(y, 7, 0x3F, 0); // all disabled: the output is the channel level
        wr(y, 8, 0x10, 0); // channel A on the envelope
        wr(y, 11, 1, 0);
        wr(y, 12, 0, 0);
        wr(y, 13, 0, 0);
        render_ticks(y, 80);
        const auto& s = y.samples();
        // The write to register 13 restarts the envelope at level 31; each tick steps one level down.
        CHECK(s[0] == level1(30) || s[0] == level1(31));
        for (int i = 1; i < 32; ++i) CHECK(s[size_t(i)] <= s[size_t(i - 1)]);
        CHECK(s[40] == 0 && s[79] == 0);
        CHECK(s[5] > s[20] && s[20] > s[30]);
    }
    // Shape 10 (\/\/): 32 down, 32 up, repeating; a 64-tick period.
    {
        Ym2149 y = raw();
        wr(y, 7, 0x3F, 0);
        wr(y, 8, 0x10, 0);
        wr(y, 11, 1, 0);
        wr(y, 12, 0, 0);
        wr(y, 13, 10, 0);
        render_ticks(y, 400);
        const auto& s = y.samples();
        CHECK(s[5] > s[20] && s[40] > s[20] && s[60] > s[40]); // down, then up
        CHECK(s[10] == s[10 + 64] && s[100] == s[100 + 64]);   // repeats with period 64
    }
    // Shape 11 (\---): down once then held at 31 (the loudest). Shape 13 (/---): up once and held.
    {
        Ym2149 y = raw();
        wr(y, 7, 0x3F, 0);
        wr(y, 8, 0x10, 0);
        wr(y, 11, 1, 0);
        wr(y, 12, 0, 0);
        wr(y, 13, 13, 0);
        render_ticks(y, 100);
        CHECK(y.samples()[0] < y.samples()[20] && y.samples()[99] == level1(31));
    }
    // A longer period stretches the steps: period 8 gives 8 ticks per level.
    {
        Ym2149 y = raw();
        wr(y, 7, 0x3F, 0);
        wr(y, 8, 0x10, 0);
        wr(y, 11, 8, 0);
        wr(y, 12, 0, 0);
        wr(y, 13, 0, 0);
        render_ticks(y, 64);
        int steps = 0;
        for (size_t i = 1; i < y.samples().size(); ++i) steps += y.samples()[i] != y.samples()[i - 1];
        CHECK(steps >= 6 && steps <= 8);
    }
}

void test_sample_accurate_writes() {
    // The digi-sample technique: volume register writes at sample rate. A ramp written 31 ticks apart appears in the
    // output within one tick of its time.
    Ym2149 y = raw();
    wr(y, 7, 0x3F, 0);
    const int ramp[8] = {1, 3, 5, 7, 9, 11, 13, 15};
    const int map4to5[16] = {0, 1, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31};
    for (int i = 0; i < 8; ++i) wr(y, 8, ramp[i], cycle_of_tick(100 + uint64_t(i) * 31));
    render_ticks(y, 400);
    const auto& s = y.samples();
    int bad = 0;
    for (int i = 0; i < 8; ++i)
        for (int t = 100 + i * 31 + 1; t < 100 + (i + 1) * 31 - 1; ++t)
            if (s[size_t(t)] != level1(map4to5[ramp[i]])) ++bad;
    CHECK(bad == 0);
    CHECK(s[50] == 0);
    // The write at tick 100 takes effect within a tick of it.
    CHECK(s[99] == 0 && s[101] == level1(map4to5[1]));
    // Writes arriving out of order across a render boundary are applied in order (a late stamp runs at the current tick).
    Ym2149 z = raw();
    wr(z, 7, 0x3F, 0);
    render_ticks(z, 50);
    wr(z, 8, 15, 0); // stamped in the past (tick 0): applied at the current tick
    render_ticks(z, 10);
    CHECK(z.samples().back() == level1(31));
}

void test_decimation_and_filters() {
    // 48 kHz: 250,000 ticks give 48,000 samples (5 or 6 ticks each).
    Ym2149 y;
    y.set_sound(48000, false);
    wr(y, 7, 0x3F, 0);
    wr(y, 8, 15, 0);
    y.render_to(kCpuHz); // one second
    CHECK(y.samples().size() >= 47999 && y.samples().size() <= 48001);
    for (size_t i = 100; i < 200; ++i) CHECK(y.samples()[i] == level1(31));
    // Filters: a constant level is AC-coupled away.
    Ym2149 f;
    f.set_sound(48000, true);
    wr(f, 7, 0x3F, 0);
    wr(f, 8, 15, 0);
    f.render_to(3 * kCpuHz);
    int16_t last = f.samples().back();
    CHECK(last > -200 && last < 200);
    CHECK(f.samples()[200] > 1000); // the step is heard first
    // Off: no queue, no samples.
    Ym2149 off;
    for (int i = 0; i < 1000; ++i) wr(off, 8, i & 15, uint64_t(i) * 100);
    off.render_to(1000 * 100);
    CHECK(off.samples().empty() && !off.sound_enabled());
}

void test_determinism() {
    auto run = [] {
        Ym2149 y;
        y.set_sound(44100, true);
        wr(y, 7, 0x00, 0);
        wr(y, 0, 0x34, 0);
        wr(y, 1, 0x01, 0);
        wr(y, 6, 9, 0);
        wr(y, 8, 0x10, 0);
        wr(y, 9, 12, 0);
        wr(y, 11, 20, 0);
        wr(y, 13, 14, 0);
        for (int i = 1; i < 200; ++i) wr(y, 9, i & 15, uint64_t(i) * 20000);
        y.render_to(5000000);
        return y.samples();
    };
    CHECK(run() == run());
    CHECK(run().size() > 1000);
}

// ---------------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> drain(Ikbd& k, uint64_t t) {
    std::vector<uint8_t> out;
    for (int i = 0; i < 64; ++i) {
        t += kIkbdByteCycles;
        k.run_to(t);
        if (k.status(t) & 1) out.push_back(k.read_data(t));
    }
    return out;
}

void test_ikbd_pause() {
    Ikbd k;
    k.write_control(0x96, 0);
    uint64_t t = 1000;
    k.write_data(0x13, t); // pause output
    CHECK(k.output_paused());
    t += 5000;
    k.key(0x1E, false, t);
    k.joystick(1, 0x81, t + 100);
    k.key(0x1E, true, t + 200);
    CHECK(k.held_bytes() == 1 + 2 + 1);
    auto none = drain(k, t + 300);
    CHECK(none.empty()); // nothing reaches the CPU while paused
    // A byte that is not a command does not resume the output.
    k.write_data(0x01, t + 400000);
    CHECK(k.output_paused() && k.held_bytes() == 4);
    // $11 resumes: everything held arrives, in order.
    const uint64_t t2 = t + 1000000;
    k.write_data(0x11, t2);
    CHECK(!k.output_paused() && k.held_bytes() == 0);
    const auto got = drain(k, t2);
    CHECK(got.size() == 4 && got[0] == 0x1E && got[1] == 0xFF && got[2] == 0x81 && got[3] == 0x9E);
    // Any valid command resumes too: pause, then $14 (joystick event reporting).
    k.write_data(0x13, t2 + 2000000);
    k.key(0x30, false, t2 + 2000100);
    CHECK(k.held_bytes() == 1);
    k.write_data(0x14, t2 + 3000000);
    CHECK(!k.output_paused());
    const auto got2 = drain(k, t2 + 3000000);
    CHECK(got2.size() == 1 && got2[0] == 0x30);
    // Reset replies with $F1.
    k.write_data(0x80, t2 + 9000000);
    k.write_data(0x01, t2 + 9000100);
    const auto got3 = drain(k, t2 + 9000100 + 8021247 / 4);
    bool found = false;
    for (uint8_t b : got3) found |= b == 0xF1;
    CHECK(found);
    // Interrogation ($16) answers with $FD and both joystick states even while events are off ($15).
    Ikbd j;
    j.write_control(0x96, 0);
    j.write_data(0x15, 100);
    j.joystick(1, 0x08, 200);
    CHECK(j.bytes_sent() == 0);
    j.write_data(0x16, 300);
    const auto g = drain(j, 300);
    CHECK(g.size() == 3 && g[0] == 0xFD && g[1] == 0x00 && g[2] == 0x08);
}

// ---------------------------------------------------------------------------------------------------------------------
void test_script_parse() {
    StScript s;
    std::string err;
    CHECK(parse_st_script("# comment\nframes 500\n10 fire\n20 joy UL 5\n30 key 39\n40 fire 2 every 10 until 70\n", s, err));
    CHECK(s.frames_set && s.frames == 500);
    CHECK(s.events.size() == 3 + 4);
    CHECK(s.events[0].kind == ScriptEvent::Joy && s.events[0].bits == kJoyFire && s.events[0].hold == 3 && s.events[0].port == 1);
    CHECK(s.events[1].bits == (kJoyUp | kJoyLeft) && s.events[1].hold == 5 && s.events[1].frame == 20);
    CHECK(s.events[2].kind == ScriptEvent::Key && s.events[2].scancode == 0x39 && s.events[2].hold == 2);
    CHECK(s.events[6].frame == 70 && s.events[6].hold == 2);
    // The relative frame form.
    StScript r;
    CHECK(parse_st_script("100 fire\n+50 joy R\n", r, err));
    CHECK(r.events.size() == 2 && r.events[1].frame == 150);
    // The Hatari harness lines.
    StScript h;
    CHECK(parse_st_script("2000 JOY1_FIRE 1\n2006 JOY1_FIRE 0\n2100 JOY0_LEFT 1\n2200 KEY39 1\n2210 KEY39 0\n", h, err));
    CHECK(h.events.size() == 5 && h.events[0].edge && h.events[0].value && h.events[0].port == 1 && h.events[0].bits == kJoyFire);
    CHECK(h.events[2].port == 0 && h.events[2].bits == kJoyLeft);
    CHECK(h.events[3].kind == ScriptEvent::Key && h.events[3].scancode == 0x39 && h.events[3].value);
    // Bad input is rejected with the line number.
    for (const char* bad : {"x fire\n", "10\n", "10 fire 0\n", "10 joy\n", "10 joy Q\n", "10 key 99\n", "10 key\n", "10 poke 1 2\n",
                            "10 fire every 5\n", "10 fire every 0 until 20\n", "10 JOY1_FIRE 2\n", "10 JOY1_JUMP 1\n", "10 KEYZZ 1\n",
                            "frames\n", "10 bogus\n"}) {
        StScript b;
        std::string e;
        CHECK(!parse_st_script(bad, b, e) && e.find("line 1") == 0);
    }
}

void test_script_player() {
    StMachine m; // an unbooted machine is enough: input goes to the IKBD
    StScript s;
    std::string err;
    CHECK(parse_st_script("5 fire 3\n20 key 39\n", s, err));
    ScriptPlayer p(s);
    for (uint32_t f = 0; f < 30; ++f) p.apply(m, f);
    // fire down at frame 5 (0xFF, 0x80) and up at frame 8 (0xFF, 0x00); key press at 20 and release at 22.
    CHECK(m.ikbd().bytes_sent() == 2 + 2 + 1 + 1);
    // The Hatari form drives the same chip.
    StMachine m2;
    StScript h;
    CHECK(parse_st_script("5 JOY1_FIRE 1\n8 JOY1_FIRE 0\n20 KEY39 1\n22 KEY39 0\n", h, err));
    ScriptPlayer q(h);
    for (uint32_t f = 0; f < 30; ++f) q.apply(m2, f);
    CHECK(m2.ikbd().bytes_sent() == m.ikbd().bytes_sent());
    // Overlapping windows combine and release only when the last ends.
    StMachine m3;
    StScript o;
    CHECK(parse_st_script("2 joy L 10\n5 fire 3\n", o, err));
    ScriptPlayer r(o);
    for (uint32_t f = 0; f < 20; ++f) r.apply(m3, f);
    // left down (2), fire added (5): a packet each; fire released at 8: one; left released at 12: one => 4 packets of 2 bytes
    CHECK(m3.ikbd().bytes_sent() == 8);
}

void test_scancodes() {
    CHECK(st_scancode_for_amiga_raw(0x19) == 0x19); // P
    CHECK(st_scancode_for_amiga_raw(0x45) == 0x01); // Esc
    CHECK(st_scancode_for_amiga_raw(0x40) == 0x39); // Space
    CHECK(st_scancode_for_amiga_raw(0x20) == 0x1E); // A
    CHECK(st_scancode_for_amiga_raw(0x31) == 0x2C); // Z
    CHECK(st_scancode_for_amiga_raw(0x44) == 0x1C); // Return
    CHECK(st_scancode_for_amiga_raw(0x4C) == 0);    // cursor up: not a key the game reads (it uses the joystick)
    CHECK(joy_bits(true, false, true, false, true) == (kJoyUp | kJoyLeft | kJoyFire));
    CHECK(joy_bits(false, false, false, false, false) == 0);
}
} // namespace

int main() {
    (void)kTick;
    test_tables();
    test_registers_and_mirrors();
    test_tone();
    test_volume_steps();
    test_noise();
    test_envelope();
    test_sample_accurate_writes();
    test_decimation_and_filters();
    test_determinism();
    test_ikbd_pause();
    test_script_parse();
    test_script_player();
    test_scancodes();
    std::printf("st sound tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
