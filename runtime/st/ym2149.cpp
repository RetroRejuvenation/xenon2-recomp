// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/ym2149.h"

#include "st/mfp.h" // kCpuHz

#include <algorithm>

namespace x2::st {

namespace {
// Significant bits of each register on a read (the chip's registers are narrower than a byte).
constexpr uint8_t kMask[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};

// The single-channel D/A output level for each of the chip's 32 volume steps, as measured on a real ST (16-bit scale;
// step 0 is forced to silence). The same measurements, taken from the sc68 player by Benjamin Gerard, are used by Hatari
// (src/sound.c, GPL-2.0-or-later); they are physical constants, listed here with that attribution.
constexpr uint16_t kDac[32] = {0,     369,   438,   521,   619,   735,   874,   1039,  1234,  1467,  1744,  2072,  2463,  2927,  3479, 4135,
                               4914,  5841,  6942,  8250,  9806,  11654, 13851, 16462, 19565, 23253, 27636, 32845, 39037, 46395, 55141, 65535};
// A 4-bit amplitude register value mapped onto the 5-bit steps (0 and 1 stay, the rest are 2v+1).
constexpr uint8_t kVol4to5[16] = {0, 1, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31};

// Envelope shapes: block 0 plays once, then blocks 1 and 2 repeat. 0 = 31 down to 0, 1 = 0 up to 31, 2 = held at 0, 3 = held at 31.
constexpr uint8_t kEnvDef[16][3] = {{0, 2, 2}, {0, 2, 2}, {0, 2, 2}, {0, 2, 2}, {1, 2, 2}, {1, 2, 2}, {1, 2, 2}, {1, 2, 2},
                                    {0, 0, 0}, {0, 2, 2}, {0, 1, 0}, {0, 3, 3}, {1, 1, 1}, {1, 3, 3}, {1, 0, 1}, {1, 2, 2}};

uint8_t env_level(int shape, uint32_t pos) {
    const uint32_t block = pos / 32, i = pos % 32;
    switch (kEnvDef[shape & 15][block % 3]) {
    case 0: return uint8_t(31 - i);
    case 1: return uint8_t(i);
    case 2: return 0;
    default: return 31;
    }
}
} // namespace

uint16_t Ym2149::dac_level(int step5) { return kDac[step5 & 31]; }

uint64_t Ym2149::tick_of(uint64_t cpu_cycle) { return cpu_cycle * kYmTickHz / kCpuHz; }

void Ym2149::reset() {
    regs_.fill(0);
    regs_[7] = 0xFF;  // every channel off
    regs_[14] = 0xFF; // port A: side 0, no drive selected, the other lines high
    sel_ = 0;
    log_.clear();
    writes_ = 0;
    queue_.clear();
    samples_.clear();
    g_.fill(0);
    g_[7] = 0xFF;
    ticks_done_ = 0;
    tone_count_[0] = tone_count_[1] = tone_count_[2] = 0;
    tone_val_[0] = tone_val_[1] = tone_val_[2] = 0;
    noise_count_ = 0;
    noise_half_ = noise_val_ = 0;
    rnd_ = 1;
    env_count_ = 0;
    env_pos_ = 0;
    acc_ = sum_ = 0;
    n_ = 0;
    dc_ = lp_ = 0;
}

uint8_t Ym2149::read8(unsigned off) const {
    if (off & 1) return 0xFF;
    switch ((off >> 1) & 1) {
    case 0: return sel_ < 16 ? uint8_t(regs_[sel_] & kMask[sel_]) : 0;
    default: return 0xFF; // $FF8802 is write-only (reads give the same as $FF8800 on real chips; the games do not read it)
    }
}

void Ym2149::write8(unsigned off, uint8_t v, uint64_t now) {
    if (off & 1) return;
    if (((off >> 1) & 1) == 0) { // select
        sel_ = v & 0x0F;
        return;
    }
    regs_[sel_] = v;
    ++writes_;
    if (log_on_) log_.push_back({now, sel_, v});
    if (rate_ && sel_ < 14) queue_.push_back({tick_of(now), sel_, v});
}

void Ym2149::set_sound(uint32_t rate_hz, bool filters) {
    rate_ = std::min(rate_hz, kYmTickHz);
    filters_ = filters;
    queue_.clear();
    samples_.clear();
    acc_ = sum_ = 0;
    n_ = 0;
    dc_ = lp_ = 0;
    // The generator picks up the current registers from here on.
    g_ = regs_;
}

void Ym2149::apply(uint8_t reg, uint8_t value) {
    g_[reg & 15] = value;
    if (reg == 13) { // a write to the shape register restarts the envelope
        env_pos_ = 0;
        env_count_ = 0;
    }
}

void Ym2149::run_ticks(uint64_t n) {
    for (; n; --n) {
        ++ticks_done_;
        for (int c = 0; c < 3; ++c) {
            uint32_t per = (uint32_t(g_[2 * c + 1] & 0x0F) << 8) | g_[2 * c];
            if (per == 0) per = 1;
            if (++tone_count_[c] >= per) {
                tone_count_[c] = 0;
                tone_val_[c] ^= 1;
            }
        }
        noise_half_ ^= 1;
        if (!noise_half_) ++noise_count_; // the noise counter runs at 125 kHz
        uint32_t nper = g_[6] & 0x1F;
        if (nper == 0) nper = 1;
        if (noise_count_ >= nper) {
            noise_count_ = 0;
            if (rnd_ & 1) {
                rnd_ = (rnd_ >> 1) ^ 0x12000;
                noise_val_ = 1;
            } else {
                rnd_ >>= 1;
                noise_val_ = 0;
            }
        }
        uint32_t eper = (uint32_t(g_[12]) << 8) | g_[11];
        if (eper == 0) eper = 1;
        if (++env_count_ >= eper) {
            env_count_ = 0;
            if (++env_pos_ >= 96) env_pos_ -= 64; // blocks 1 and 2 repeat
        }
        const int env = env_level(g_[13] & 15, env_pos_);
        uint32_t mix = 0;
        for (int c = 0; c < 3; ++c) {
            const bool tone_off = (g_[7] >> c) & 1, noise_off = (g_[7] >> (3 + c)) & 1;
            const bool high = (tone_val_[c] || tone_off) && (noise_val_ || noise_off);
            if (!high) continue;
            const uint8_t a = g_[8 + c];
            mix += kDac[(a & 0x10) ? env : kVol4to5[a & 15]];
        }
        sum_ += mix;
        ++n_;
        acc_ += rate_;
        if (acc_ >= kYmTickHz) {
            acc_ -= kYmTickHz;
            emit(uint32_t(sum_ / (3ull * n_)));
            sum_ = 0;
            n_ = 0;
        }
    }
}

void Ym2149::emit(uint32_t avg) {
    int32_t x = int32_t(avg);
    int32_t out;
    if (filters_) {
        lp_ += ((x - lp_) * 13) >> 4; // the ST's output low-pass, mild
        dc_ += ((lp_ << 8) - dc_) >> 10; // AC coupling (the average is kept with 8 fractional bits so a steady level decays to silence)
        out = ((lp_ - (dc_ >> 8)) * 5) >> 3;
    } else {
        out = (x * 5) >> 3;
    }
    samples_.push_back(int16_t(std::clamp(out, -32768, 32767)));
}

void Ym2149::render_to(uint64_t cycle) {
    if (!rate_) return;
    const uint64_t target = tick_of(cycle);
    while (!queue_.empty() && queue_.front().tick <= target) {
        const Pending p = queue_.front();
        queue_.pop_front();
        if (p.tick > ticks_done_) run_ticks(p.tick - ticks_done_);
        apply(p.reg, p.value);
    }
    if (target > ticks_done_) run_ticks(target - ticks_done_);
}

} // namespace x2::st
