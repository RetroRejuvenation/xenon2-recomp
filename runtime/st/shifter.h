// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The ST's video chips as the CPU sees them ($FF8200-$FF82FF, the GLUE/MMU/Shifter): the video base address, the video
// address counter (it advances as the screen is fetched, so the games can wait on it for the raster), the sync mode,
// the sixteen 9-bit palette entries and the resolution; and a decoder from screen memory to an RGBA picture.
//
// Registers sit on odd byte addresses ($FF8201 base high, $FF8203 base mid, $FF8205/07/09 counter high/mid/low, $FF820A
// sync, $FF8240-$FF825F palette words, $FF8260 resolution); an even byte read of a byte register is open bus ($FF). The
// palette is word-wide with three bits per gun (bits 11-15 and the low bit of each gun read back as zero). S2 renders
// low resolution (320x200, four interleaved bitplanes) at the end of a frame from the base the Shifter had when the
// display started; S3 makes the picture line-granular. Medium (640x200) and high (640x400) resolution are accepted but
// drawn as a blank picture with a count (nothing in the Xenon 2 ST build uses them).
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace x2::st {

constexpr int kLineCycles = 512;       // CPU cycles per scan line (PAL 50 Hz)
constexpr int kFrameLines = 313;
constexpr int kFirstDisplayLine = 63;  // the first of the 200 display lines at 50 Hz
constexpr int kDisplayLines = 200;
constexpr int kDisplayStartCycle = 56; // the line cycle the low-resolution display starts at (after the left border)
constexpr int kFbWidth = 320;
constexpr int kFbHeight = 200;

class Shifter {
public:
    Shifter() { reset(); }
    void reset();

    // CPU access: `off` is the address - $FF8200. `line`/`cycle` are the beam position (the counter depends on it).
    uint8_t read8(unsigned off, int line, int cycle) const;
    void write8(unsigned off, uint8_t v);
    uint16_t read16(unsigned off, int line, int cycle) const;
    void write16(unsigned off, uint16_t v);

    uint32_t base() const { return base_; }
    uint8_t sync_mode() const { return sync_; }
    uint8_t resolution() const { return res_ & 3; }
    uint16_t palette(int i) const { return pal_[i & 15]; }
    // The video address counter at a beam position: base plus the bytes fetched so far in the display window.
    uint32_t counter(int line, int cycle) const;
    // Frames-per-second selection: sync bit 1 set = 50 Hz.
    bool is_50hz() const { return (sync_ & 2) != 0; }

    // A screen decoded to RGBA8 (R,G,B,A bytes), 320x200: low resolution from `ram` at `base` with `palette`. Other
    // resolutions give a black picture. `ram` must hold at least base + 32000 bytes or the missing part is black.
    static void render(const uint8_t* ram, uint32_t ram_size, uint32_t base, const std::array<uint16_t, 16>& palette,
                       uint8_t resolution, std::vector<uint8_t>& rgba);
    // 3-bit gun to 8 bits (0..7 -> 0..255).
    static uint8_t gun8(unsigned g) { return uint8_t((g << 5) | (g << 2) | (g >> 1)); }

    const std::array<uint16_t, 16>& palette_array() const { return pal_; }
    uint64_t unsupported_resolution_frames = 0;

private:
    uint32_t base_ = 0;
    uint8_t sync_ = 0xFE;
    uint8_t res_ = 0;
    uint8_t mem_config_ = 0x0A;
    std::array<uint16_t, 16> pal_{};
};

} // namespace x2::st
