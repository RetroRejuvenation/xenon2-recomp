// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Scanline video model: Agnus bitplane and sprite DMA plus Denise pixel output, rendered into a PAL lores
// RGBA framebuffer. Platform independent (no SDL); the window front end only uploads front().
//
// The model observes the Machine (Machine::on_custom_write / on_line_end) and owns the display DMA state: the
// bitplane and sprite pointers it fetches through are its own copies of BPLxPT/SPRxPT, loaded by CPU or copper
// writes and advanced by the fetches, so the CPU-visible machine needs no changes. Register writes are applied
// at the colour clock they happened at, so CPU or copper palette changes take effect mid-line.
//
// Coordinates: horizontal positions are in lores pixels as used by DIWSTRT/STOP and SPRxPOS (one colour clock
// = 2 lores pixels = 4 hires pixels); vertical positions are beam lines. The framebuffer is the standard PAL
// overscan-free window: kFbWidth x kFbHeight pixels from (kFbLeft, kFbTop) = ($81, $2C). Hires is rendered at
// full resolution internally and each pair of hires pixels is averaged into one framebuffer pixel.
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace x2 {

// RGBA pixels are packed into uint32_t as R | G << 8 | B << 16 | A << 24 here, in the modernisation layer and
// in the window filters, and then read as R,G,B,A bytes: every supported host (x86-64, arm64) is little-endian.
static_assert(std::endian::native == std::endian::little, "x2recomp's RGBA framebuffers assume a little-endian host");

class Machine;

class Video {
public:
    static constexpr int kFbWidth = 320;
    static constexpr int kFbHeight = 256;
    static constexpr int kFbLeft = 0x81;
    static constexpr int kFbTop = 0x2C;
    static constexpr int kSpriteDmaFirstLine = 25; // first line with sprite DMA slots after vertical blank (PAL)

    // Hook into the machine (sets on_custom_write / on_line_end) and take the current register state.
    void attach(Machine& m);
    // Used directly by tests; attach() wires these to the machine callbacks.
    void custom_write(uint16_t off, uint16_t value, int line, uint16_t hpos);
    void line_end(const uint8_t* chip, uint32_t chip_mask, int line, bool last_line);

    // Last completed frame, RGBA8 (R,G,B,A bytes), kFbWidth*kFbHeight*4 bytes, row-major.
    const std::vector<uint8_t>& front() const { return front_; }
    uint32_t frames_completed() const { return frames_; }
    // 64-bit FNV-1a of front(), used by the golden-frame tests.
    uint64_t front_hash() const;

    // Expand a 12-bit Amiga colour to RGBA (each 4-bit gun x17).
    static uint32_t rgba_of(uint16_t c12);

private:
    struct Event {
        uint16_t off, value, hpos;
    };
    struct Sprite {
        uint32_t pt = 0;
        uint16_t pos = 0, ctl = 0, data = 0, datb = 0;
        bool armed = false;
        enum Dma : uint8_t { FetchCtl, WaitStart, Active, Done } dma = FetchCtl;
        int vstart() const { return (pos >> 8) | ((ctl & 4) << 6); }
        int vstop() const { return (ctl >> 8) | ((ctl & 2) << 7); }
        int hstart() const { return ((pos & 0xFF) << 1) | (ctl & 1); }
    };
    struct State {
        uint16_t diwstrt = 0, diwstop = 0, ddfstrt = 0, ddfstop = 0;
        uint16_t dmacon = 0, bplcon0 = 0, bplcon1 = 0, bplcon2 = 0, bpl1mod = 0, bpl2mod = 0;
        uint32_t bplpt[6] = {};
        uint16_t color[32] = {};
        Sprite spr[8];
    };

    void apply(const Event& e);
    void sprite_dma(const uint8_t* chip, uint32_t mask, int line);
    void render_line(int line, const std::vector<Event>& events);

    State s_;
    std::vector<Event> events_; // writes during the current line, in order
    std::vector<uint8_t> back_ = std::vector<uint8_t>(std::size_t(kFbWidth) * kFbHeight * 4, 0);
    std::vector<uint8_t> front_ = std::vector<uint8_t>(std::size_t(kFbWidth) * kFbHeight * 4, 0);
    uint32_t frames_ = 0;

    // Per-line bitplane data, one entry per hires pixel position (lores x * 2), bit p = plane p.
    static constexpr int kHiresSpan = 2 * 0x1C8;
    std::array<uint8_t, kHiresSpan> pf_{};
};

} // namespace x2
