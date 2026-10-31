// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/shifter.h"

#include <algorithm>

namespace x2::st {

void Shifter::reset() {
    base_ = 0;
    sync_ = 0xFE; // 50 Hz, internal sync, as TOS leaves a PAL machine
    res_ = 0;
    mem_config_ = 0x0A;
    pal_.fill(0);
    pal_[0] = 0x0777; // TOS's white background
}

uint32_t Shifter::counter(int line, int cycle) const {
    if (line < kFirstDisplayLine) return base_;
    if (line >= kFirstDisplayLine + kDisplayLines) return base_ + 160u * kDisplayLines;
    uint32_t bytes = 160u * uint32_t(line - kFirstDisplayLine);
    // Within a line the Shifter fetches 8 bytes (one 16-pixel group) every 16 cycles once the display has started.
    if (cycle > kDisplayStartCycle) bytes += uint32_t(std::min(160, ((cycle - kDisplayStartCycle) / 16) * 8));
    return base_ + bytes;
}

uint8_t Shifter::read8(unsigned off, int line, int cycle) const {
    switch (off) {
    case 0x01: return uint8_t(base_ >> 16);
    case 0x03: return uint8_t(base_ >> 8);
    case 0x05: return uint8_t(counter(line, cycle) >> 16);
    case 0x07: return uint8_t(counter(line, cycle) >> 8);
    case 0x09: return uint8_t(counter(line, cycle) & 0xFE);
    case 0x0A: return sync_;
    case 0x60: return uint8_t(0xFC | (res_ & 3)); // unused bits float high
    case 0x61: return 0xFF;
    default: break;
    }
    if (off >= 0x40 && off < 0x60) {
        const uint16_t c = uint16_t(pal_[(off - 0x40) >> 1] & 0x0777);
        return (off & 1) ? uint8_t(c) : uint8_t(c >> 8);
    }
    if (off == 0x00 || off == 0x02 || off == 0x04 || off == 0x06 || off == 0x08) return 0xFF; // even byte of a byte register
    return 0xFF;
}

void Shifter::write8(unsigned off, uint8_t v) {
    switch (off) {
    case 0x01: base_ = (base_ & 0x00FFFF) | (uint32_t(v & 0x3F) << 16); return;
    case 0x03: base_ = (base_ & 0xFF0000) | (uint32_t(v) << 8); return; // the ST has no low byte: it is always 0
    case 0x0A: sync_ = v; return;
    case 0x60: res_ = uint8_t(v & 3); return;
    default: break;
    }
    if (off >= 0x40 && off < 0x60) {
        uint16_t& c = pal_[(off - 0x40) >> 1];
        if (off & 1) c = uint16_t((c & 0x0700) | (v & 0x77));
        else c = uint16_t((c & 0x0077) | ((v & 0x07) << 8));
    }
}

uint16_t Shifter::read16(unsigned off, int line, int cycle) const {
    if (off >= 0x40 && off < 0x60) return uint16_t(pal_[(off - 0x40) >> 1] & 0x0777);
    return uint16_t((read8(off, line, cycle) << 8) | read8(off + 1, line, cycle));
}

void Shifter::write16(unsigned off, uint16_t v) {
    if (off >= 0x40 && off < 0x60) { pal_[(off - 0x40) >> 1] = uint16_t(v & 0x0777); return; }
    write8(off, uint8_t(v >> 8));
    write8(off + 1, uint8_t(v));
}

void Shifter::render(const uint8_t* ram, uint32_t ram_size, uint32_t base, const std::array<uint16_t, 16>& palette,
                     uint8_t resolution, std::vector<uint8_t>& rgba) {
    rgba.assign(size_t(kFbWidth) * kFbHeight * 4, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 0xFF;
    if ((resolution & 3) != 0) return; // medium and high resolution: a black picture
    uint32_t colors[16];
    for (int i = 0; i < 16; ++i) {
        const uint16_t c = palette[i] & 0x0777;
        colors[i] = uint32_t(gun8((c >> 8) & 7)) | uint32_t(gun8((c >> 4) & 7)) << 8 | uint32_t(gun8(c & 7)) << 16 | 0xFF000000u;
    }
    for (int y = 0; y < kFbHeight; ++y) {
        const uint32_t row = base + uint32_t(y) * 160;
        if (row + 160 > ram_size) break;
        uint32_t* out = reinterpret_cast<uint32_t*>(&rgba[size_t(y) * kFbWidth * 4]);
        for (int g = 0; g < 20; ++g) { // 20 groups of 16 pixels, each four plane words
            const uint8_t* p = ram + row + uint32_t(g) * 8;
            const unsigned w0 = (unsigned(p[0]) << 8) | p[1], w1 = (unsigned(p[2]) << 8) | p[3];
            const unsigned w2 = (unsigned(p[4]) << 8) | p[5], w3 = (unsigned(p[6]) << 8) | p[7];
            for (int x = 0; x < 16; ++x) {
                const int b = 15 - x;
                const unsigned idx = ((w0 >> b) & 1) | (((w1 >> b) & 1) << 1) | (((w2 >> b) & 1) << 2) | (((w3 >> b) & 1) << 3);
                out[g * 16 + x] = colors[idx];
            }
        }
    }
}

} // namespace x2::st
