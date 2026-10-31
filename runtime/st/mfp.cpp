// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/mfp.h"

#include <algorithm>

namespace x2::st {

namespace {
constexpr unsigned kPrescale[8] = {0, 4, 10, 16, 50, 64, 100, 200};
constexpr int kTimerChannel[4] = {Mfp::TimerA, Mfp::TimerB, Mfp::TimerC, Mfp::TimerD};
constexpr uint64_t kNever = ~uint64_t(0);

// Register numbers (the odd byte address - $FFFA00) / 2.
enum Reg : unsigned {
    GPIP = 0, AER, DDR, IERA, IERB, IPRA, IPRB, ISRA, ISRB, IMRA, IMRB, VR, TACR, TBCR, TCDCR, TADR, TBDR, TCDR, TDDR, SCR, UCR,
    RSR, TSR, UDR
};
} // namespace

unsigned Mfp::Timer::prescale() const { return kPrescale[mode < 8 ? mode : 0]; }

void Mfp::reset() {
    for (Timer& t : timer_) t = Timer{};
    gpip_out_ = aer_ = ddr_ = 0;
    inputs_ = 0xFF;
    ier_ = ipr_ = isr_ = imr_ = 0;
    vr_ = scr_ = ucr_ = rsr_ = tsr_ = udr_ = 0;
    tacr_ = tbcr_ = tcdcr_ = 0;
    last_ = 0;
    acc_ = 0;
}

int Mfp::gpip_channel(int bit) {
    static const int map[8] = {Gpi0, Gpi1, Gpi2, Gpi3, Gpi4, Gpi5, Gpi6, Gpi7};
    return map[bit & 7];
}

void Mfp::raise(int channel) {
    if (ier_ & (1u << channel)) ipr_ |= uint16_t(1u << channel);
}

void Mfp::advance(uint64_t now) {
    if (now <= last_) return;
    const uint64_t total = acc_ + (now - last_) * kMfpHz;
    const uint64_t ticks = total / kCpuHz;
    acc_ = total % kCpuHz;
    last_ = now;
    if (!ticks) return;
    for (int i = 0; i < 4; ++i) {
        Timer& t = timer_[i];
        if (t.mode == 0 || t.mode > 7) continue;
        const uint64_t p = t.prescale();
        t.phase += ticks;
        uint64_t steps = t.phase / p;
        t.phase %= p;
        while (steps) {
            if (steps >= t.counter) { // the counter reaches 0: underflow, reload
                steps -= t.counter;
                t.counter = t.data ? t.data : 256;
                ++t.underflows;
                raise(kTimerChannel[i]);
            } else {
                t.counter -= unsigned(steps);
                steps = 0;
            }
        }
    }
}

uint64_t Mfp::next_event() const {
    uint64_t best = kNever;
    for (const Timer& t : timer_) {
        if (t.mode == 0 || t.mode > 7) continue;
        const uint64_t p = t.prescale();
        const uint64_t ticks = (p - t.phase) + uint64_t(t.counter - 1) * p; // MFP ticks until the underflow
        const uint64_t need = ticks * kCpuHz;
        const uint64_t cycles = need > acc_ ? (need - acc_ + kMfpHz - 1) / kMfpHz : 0;
        best = std::min(best, last_ + cycles);
    }
    return best;
}

void Mfp::set_mode(int t, unsigned mode) {
    Timer& tm = timer_[t];
    if (mode > 7) mode = 0; // pulse-width and event-count modes are not modelled: the timer stays stopped
    if (tm.mode == 0 && mode != 0) {
        tm.counter = tm.data ? tm.data : 256;
        tm.phase = 0;
    }
    tm.mode = mode;
}

void Mfp::write_data(int t, uint8_t v) {
    Timer& tm = timer_[t];
    tm.data = v;
    if (tm.mode == 0) tm.counter = v ? v : 256; // a stopped timer loads at once; a running one reloads at its next underflow
}

uint8_t Mfp::read(unsigned reg, uint64_t now) {
    advance(now);
    switch (reg) {
    case GPIP: return uint8_t((inputs_ & ~ddr_) | (gpip_out_ & ddr_));
    case AER: return aer_;
    case DDR: return ddr_;
    case IERA: return uint8_t(ier_ >> 8);
    case IERB: return uint8_t(ier_);
    case IPRA: return uint8_t(ipr_ >> 8);
    case IPRB: return uint8_t(ipr_);
    case ISRA: return uint8_t(isr_ >> 8);
    case ISRB: return uint8_t(isr_);
    case IMRA: return uint8_t(imr_ >> 8);
    case IMRB: return uint8_t(imr_);
    case VR: return vr_;
    case TACR: return tacr_;
    case TBCR: return tbcr_;
    case TCDCR: return tcdcr_;
    case TADR: return uint8_t(timer_[0].counter == 256 ? 0 : timer_[0].counter);
    case TBDR: return uint8_t(timer_[1].counter == 256 ? 0 : timer_[1].counter);
    case TCDR: return uint8_t(timer_[2].counter == 256 ? 0 : timer_[2].counter);
    case TDDR: return uint8_t(timer_[3].counter == 256 ? 0 : timer_[3].counter);
    case SCR: return scr_;
    case UCR: return ucr_;
    case RSR: return rsr_;
    case TSR: return tsr_;
    case UDR: return udr_;
    }
    return 0xFF;
}

void Mfp::write(unsigned reg, uint8_t v, uint64_t now) {
    advance(now);
    switch (reg) {
    case GPIP: gpip_out_ = v; break;
    case AER: aer_ = v; break;
    case DDR: ddr_ = v; break;
    case IERA:
        ier_ = uint16_t((ier_ & 0x00FF) | (uint16_t(v) << 8));
        ipr_ &= ier_; // disabling a channel clears its pending bit
        break;
    case IERB:
        ier_ = uint16_t((ier_ & 0xFF00) | v);
        ipr_ &= ier_;
        break;
    case IPRA: ipr_ &= uint16_t((uint16_t(v) << 8) | 0x00FF); break; // writing 0 clears a bit, 1 leaves it
    case IPRB: ipr_ &= uint16_t(0xFF00 | v); break;
    case ISRA: isr_ &= uint16_t((uint16_t(v) << 8) | 0x00FF); break;
    case ISRB: isr_ &= uint16_t(0xFF00 | v); break;
    case IMRA: imr_ = uint16_t((imr_ & 0x00FF) | (uint16_t(v) << 8)); break;
    case IMRB: imr_ = uint16_t((imr_ & 0xFF00) | v); break;
    case VR:
        vr_ = uint8_t(v & 0xF8);
        if (!(vr_ & 0x08)) isr_ = 0; // automatic end of interrupt: nothing stays in service
        break;
    case TACR: tacr_ = v; set_mode(0, v & 0x0F); break;
    case TBCR: tbcr_ = v; set_mode(1, v & 0x0F); break;
    case TCDCR:
        tcdcr_ = v;
        set_mode(2, (v >> 4) & 7);
        set_mode(3, v & 7);
        break;
    case TADR: write_data(0, v); break;
    case TBDR: write_data(1, v); break;
    case TCDR: write_data(2, v); break;
    case TDDR: write_data(3, v); break;
    case SCR: scr_ = v; break;
    case UCR: ucr_ = v; break;
    case RSR: rsr_ = v; break;
    case TSR: tsr_ = v; break;
    case UDR: udr_ = v; break;
    }
}

void Mfp::set_input(int bit, bool level, uint64_t now) {
    advance(now);
    const bool before = (inputs_ >> bit) & 1;
    if (before == level) return;
    inputs_ = uint8_t((inputs_ & ~(1u << bit)) | (unsigned(level) << bit));
    // AER bit 0: interrupt on the falling edge (the ST's active-low IRQ lines), 1: on the rising edge.
    const bool rising_edge_mode = (aer_ >> bit) & 1;
    if (level == rising_edge_mode) raise(gpip_channel(bit));
}

bool Mfp::irq() const {
    const uint16_t pending = ipr_ & imr_;
    if (!pending) return false;
    int c = 15;
    while (!((pending >> c) & 1)) --c;
    // Blocked by an equal or higher priority channel still in service (software end of interrupt).
    return (isr_ >> c) == 0;
}

int Mfp::acknowledge() {
    const uint16_t pending = ipr_ & imr_;
    if (!pending) return -1;
    int c = 15;
    while (!((pending >> c) & 1)) --c;
    ipr_ &= uint16_t(~(1u << c));
    if (vr_ & 0x08) isr_ |= uint16_t(1u << c);
    return int(vr_ & 0xF0) | c;
}

} // namespace x2::st
