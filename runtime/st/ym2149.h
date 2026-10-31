// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The ST's sound chip, YM2149 ($FF8800 select/read, $FF8802 write, mirrored every four bytes): at this stage the register
// file only. Register writes are timestamped in CPU cycles into a log (S4 feeds it to the sound generator), port A (register
// 14) carries the floppy side and drive selects, and the read-back masks follow the chip's register widths. No sound is
// produced yet.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace x2::st {

struct YmWrite {
    uint64_t cycle;
    uint8_t reg, value;
};

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

private:
    std::array<uint8_t, 16> regs_{};
    uint8_t sel_ = 0;
    bool log_on_ = false;
    std::vector<YmWrite> log_;
    uint64_t writes_ = 0;
};

} // namespace x2::st
