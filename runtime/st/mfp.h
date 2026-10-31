// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// MC68901 MFP of the Atari ST ($FFFA00-$FFFA2F, odd bytes): the four timers (delay mode), the sixteen-channel interrupt
// controller (IER/IPR/ISR/IMR, software or automatic end of interrupt, priorities, the vector base in VR) and the
// general-purpose I/O port with its edge detection (the FDC and ACIA interrupt requests arrive on GPIP bits 5 and 4).
//
// Time is the CPU's: the MFP is clocked at 2,457,600 Hz and the ST's CPU at 8,021,247 Hz (PAL), so the machine passes
// absolute CPU cycle counts and the conversion is exact (a remainder is carried). Timers are catch-up models: advance(now)
// runs them to `now`; next_event() says when the next underflow falls so the machine can end its CPU slice there. Pulse
// width and event-count modes (timer A/B modes 8-15) are not modelled (nothing in the Xenon 2 ST build uses them).
#pragma once
#include <cstdint>

namespace x2::st {

constexpr uint64_t kCpuHz = 8021247; // the PAL ST's 68000 clock: 313 lines x 512 cycles x 50.0526 Hz
constexpr uint64_t kMfpHz = 2457600;

class Mfp {
public:
    // Interrupt channels, highest priority first is the highest number.
    enum Channel : int {
        Gpi0 = 0, Gpi1, Gpi2, Gpi3, TimerD, TimerC, Gpi4, Gpi5, TimerB, TxError, TxEmpty, RxError, RxFull, TimerA, Gpi6, Gpi7
    };

    Mfp() { reset(); }
    void reset();

    // CPU register access: `reg` is the register number (the odd byte address - $FFFA00) / 2: GPIP 0 ... UDR 23. Even
    // addresses and out-of-range offsets are the caller's business (bus error or open bus). `now` is the CPU cycle.
    uint8_t read(unsigned reg, uint64_t now);
    void write(unsigned reg, uint8_t value, uint64_t now);

    // Catch the timers up to `now` (CPU cycles).
    void advance(uint64_t now);
    // Absolute CPU cycle of the earliest timer underflow after the last advance(); ~0 if no timer runs.
    uint64_t next_event() const;

    // A GPIP input line changed (bit 0-7, level 0/1): edge detection per AER.
    void set_input(int bit, bool level, uint64_t now);
    bool input(int bit) const { return (inputs_ >> bit) & 1; }

    // The interrupt request to the CPU (IPL 6): a pending, unmasked channel with nothing of equal or higher priority in
    // service.
    bool irq() const;
    // The CPU acknowledges: returns the vector number (VR base | channel), clears the pending bit, and in software
    // end-of-interrupt mode marks the channel in service. -1 when nothing is pending (a spurious acknowledge).
    int acknowledge();

    // Introspection for tests and the driver.
    uint16_t ier() const { return ier_; }
    uint16_t ipr() const { return ipr_; }
    uint16_t isr() const { return isr_; }
    uint16_t imr() const { return imr_; }
    uint8_t vr() const { return vr_; }
    // The timer's current counter (what a read of its data register returns) and whether it runs.
    unsigned timer_counter(int t) const { return timer_[t].counter; }
    bool timer_running(int t) const { return timer_[t].mode != 0; }
    uint64_t timer_underflows(int t) const { return timer_[t].underflows; }

private:
    struct Timer {
        unsigned mode = 0;      // 0 stopped, 1-7 delay with prescale
        unsigned data = 0;      // the reload value (0 = 256)
        unsigned counter = 0;   // 1..256: decrements to 0 and reloads
        uint64_t phase = 0;     // MFP clock ticks into the current prescaler period
        uint64_t underflows = 0;
        unsigned prescale() const;
    };
    void raise(int channel);
    void set_mode(int t, unsigned mode);
    void write_data(int t, uint8_t v);
    static int gpip_channel(int bit);

    Timer timer_[4]; // A, B, C, D
    uint8_t gpip_out_ = 0, aer_ = 0, ddr_ = 0;
    uint8_t inputs_ = 0xFF; // external levels; the ST's colour monitor holds GPIP7 high
    uint16_t ier_ = 0, ipr_ = 0, isr_ = 0, imr_ = 0;
    uint8_t vr_ = 0, scr_ = 0, ucr_ = 0, rsr_ = 0, tsr_ = 0, udr_ = 0;
    uint8_t tacr_ = 0, tbcr_ = 0, tcdcr_ = 0;
    uint64_t last_ = 0, acc_ = 0; // last CPU cycle advanced to, and the remainder of cycles * kMfpHz mod kCpuHz
};

} // namespace x2::st
