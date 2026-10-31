// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/ym2149.h"

namespace x2::st {

namespace {
// Significant bits of each register on a read (the chip's registers are narrower than a byte).
constexpr uint8_t kMask[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};
} // namespace

void Ym2149::reset() {
    regs_.fill(0);
    regs_[7] = 0xFF;  // every channel off
    regs_[14] = 0xFF; // port A: side 0, no drive selected, the other lines high
    sel_ = 0;
    log_.clear();
    writes_ = 0;
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
}

} // namespace x2::st
