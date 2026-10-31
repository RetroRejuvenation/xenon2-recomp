// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The ST's sound chip, YM2149 ($FF8800 select/read, $FF8802 write, mirrored every four bytes): the register file, the
// floppy select lines on port A, and a sample generator.
//
// Generator model (docs/re-notes/atari-st-sound.md): the chip is stepped at 250 kHz (its 2 MHz clock divided by 8), in
// whole numbers only, so the output is bit-identical on every host. Each tone channel toggles every TP ticks (TP = the 12-bit
// period, 0 counts as 1: f = 2 MHz / (16 TP)); the noise generator (17-bit LFSR, taps 17 and 14) shifts every 2 NP ticks
// (NP = the 5-bit period, 0 counts as 1); the envelope steps every EP ticks through 32 levels (EP = the 16-bit period,
// 0 counts as 1), restarted by every write to register 13. A channel's level is its amplitude register (4 bits, expanded to
// the chip's 5-bit DAC steps) or the envelope, gated by (tone | tone-disabled) & (noise | noise-disabled). The three channel
// levels are converted through the single-channel DAC table measured on a real ST and mixed linearly.
//
// Register writes are timestamped in CPU cycles (the machine's clock) and replayed at their tick, so a program that plays
// samples by writing a volume register thousands of times a second (the front end's 7,979 Hz player) sounds right.
// The 250 kHz stream is decimated to the host rate by averaging (a box filter over 5 or 6 ticks), then AC-coupled and
// given the ST's mild output low-pass.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace x2::st {

struct YmWrite {
    uint64_t cycle;
    uint8_t reg, value;
};

constexpr uint32_t kYmTickHz = 250000; // the generator's step rate (2 MHz / 8)

class Ym2149 {
public:
    Ym2149() { reset(); }
    void reset();

    // `off` is the address - $FF8800 (the registers mirror every four bytes; even addresses only).
    uint8_t read8(unsigned off) const;
    void write8(unsigned off, uint8_t v, uint64_t now);

    uint8_t reg(int r) const { return regs_[r & 15]; }
    uint8_t selected() const { return sel_; }
    // Port A as the floppy logic sees it: bit 0 low = side 1, bit 1 low = drive A selected, bit 2 low = drive B.
    uint8_t port_a() const { return regs_[14]; }

    void log_writes(bool on) { log_on_ = on; }
    const std::vector<YmWrite>& log() const { return log_; }
    uint64_t writes() const { return writes_; }

    // --- sound ---
    // Turn the generator on with an output rate (Hz, at most 250,000) or off (rate 0: register writes are not queued).
    // `filters` selects the host-rate AC coupling and low-pass; without them the output is the plain scaled mix (tests).
    void set_sound(uint32_t rate_hz, bool filters = true);
    bool sound_enabled() const { return rate_ != 0; }
    uint32_t sound_rate() const { return rate_; }
    // Replay the queued register writes and generate every tick up to the given CPU cycle (absolute, the machine's clock),
    // appending mono int16 samples at the output rate to samples().
    void render_to(uint64_t cycle);
    std::vector<int16_t>& samples() { return samples_; }
    const std::vector<int16_t>& samples() const { return samples_; }

    // The tick the generator has reached, and the CPU cycle -> tick conversion (exposed for tests).
    uint64_t ticks_done() const { return ticks_done_; }
    static uint64_t tick_of(uint64_t cpu_cycle);
    // The 5-bit single-channel DAC level (0..65535) for a 5-bit volume step.
    static uint16_t dac_level(int step5);

private:
    void apply(uint8_t reg, uint8_t value);       // a register write reaching the generator
    void run_ticks(uint64_t n);                    // generate n ticks with the current generator registers
    void emit(uint32_t avg_mix);                   // one output sample from the decimator's average

    std::array<uint8_t, 16> regs_{};
    uint8_t sel_ = 0;
    bool log_on_ = false;
    std::vector<YmWrite> log_;
    uint64_t writes_ = 0;

    // generator
    struct Pending {
        uint64_t tick;
        uint8_t reg, value;
    };
    uint32_t rate_ = 0;
    bool filters_ = true;
    std::deque<Pending> queue_;
    std::vector<int16_t> samples_;
    std::array<uint8_t, 16> g_{};      // the generator's copy of the registers, advanced as writes are replayed
    uint64_t ticks_done_ = 0;
    uint32_t tone_count_[3] = {0, 0, 0};
    uint8_t tone_val_[3] = {0, 0, 0};
    uint32_t noise_count_ = 0;
    uint8_t noise_half_ = 0, noise_val_ = 0;
    uint32_t rnd_ = 1; // 17-bit LFSR
    uint32_t env_count_ = 0;
    uint32_t env_pos_ = 0; // 0..95: block 0 then blocks 1 and 2 repeated
    uint64_t acc_ = 0;     // decimator: ticks * rate_ modulo kYmTickHz
    uint64_t sum_ = 0;
    uint32_t n_ = 0;
    int32_t dc_ = 0;       // AC-coupling state (host rate)
    int32_t lp_ = 0;       // output low-pass state
};

} // namespace x2::st
