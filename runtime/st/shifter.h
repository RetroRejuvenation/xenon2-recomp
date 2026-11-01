// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The ST's video chips as the CPU sees them ($FF8200-$FF82FF, the GLUE/MMU/Shifter): the video base address, the video
// address counter (it advances as the screen is fetched, so the games can wait on it for the raster), the sync mode,
// the sixteen 9-bit palette entries and the resolution; and a decoder from screen memory to an RGBA picture.
//
// Timing model (S3, docs/re-notes/atari-st-video.md). The counter is reloaded from the base register once per frame, at
// line 310 cycle 56 (the real GLUE does it three lines before the end of the frame), so a base written after that point
// shows one frame later; the display is lines 63-262 and the counter advances one 16-bit word every 4 cycles between line
// cycles 56 and 376. The picture is decoded one line at a time: each display line reads its 160 bytes from RAM at the line
// end, and every palette write is logged with its cycle so a mid-line change shows from the right pixel.
//
// Registers sit on odd byte addresses ($FF8201 base high, $FF8203 base mid, $FF8205/07/09 counter high/mid/low, $FF820A
// sync, $FF8240-$FF825F palette words, $FF8260 resolution); an even byte read of a byte register is open bus ($FF). The
// palette is word-wide with three bits per gun (bits 11-15 and the low bit of each gun read back as zero). Low
// resolution (320x200, four interleaved bitplanes) is rendered; medium (640x200) and high (640x400) resolution are
// accepted but drawn as a blank picture with a count (nothing in the Xenon 2 ST build uses them).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace x2::st {

constexpr int kLineCycles = 512;       // CPU cycles per scan line (PAL 50 Hz)
constexpr int kFrameLines = 313;
constexpr int kFirstDisplayLine = 63;  // the first of the 200 display lines at 50 Hz
constexpr int kDisplayLines = 200;
constexpr int kDisplayStartCycle = 56; // the line cycle the low-resolution display starts at (after the left border)
constexpr int kDisplayEndCycle = kDisplayStartCycle + 320; // and the cycle it ends at (one pixel per cycle)
constexpr int kRestartLine = 310;      // the counter is reloaded from the base register here (50 Hz) ...
constexpr int kRestartCycle = 56;      // ... at this line cycle

constexpr int kFbWidth = 320;
constexpr int kFbHeight = 200;

// One palette register write: when (cycles since the frame's first line), which entry and the new 9-bit colour.
struct PaletteWrite {
    uint32_t at;
    uint8_t index;
    uint16_t value;
};

class Shifter {
public:
    Shifter() { reset(); }
    void reset();

    // CPU access: `off` is the address - $FF8200. `line`/`cycle` are the beam position (the counter depends on it).
    uint8_t read8(unsigned off, int line, int cycle) const;
    // `at` is the cycle count since the first line of the frame, for the palette log (the default logs nothing).
    void write8(unsigned off, uint8_t v, uint32_t at = kNoLog);
    uint16_t read16(unsigned off, int line, int cycle) const;
    void write16(unsigned off, uint16_t v, uint32_t at = kNoLog);
    static constexpr uint32_t kNoLog = 0xFFFFFFFFu;

    uint32_t base() const { return base_; }
    uint8_t sync_mode() const { return sync_; }
    uint8_t resolution() const { return res_ & 3; }
    uint16_t palette(int i) const { return pal_[i & 15]; }
    // The video address counter at a beam position: the latched base plus the bytes fetched so far in the display window
    // (one word per 4 cycles inside line cycles 56-376, 160 bytes per display line, held outside the display).
    uint32_t counter(int line, int cycle) const;
    // The counter is reloaded from the base register: the machine calls this at line 310 cycle 56 of every frame.
    void restart_counter() { counter_base_ = base_; }
    uint32_t counter_base() const { return counter_base_; }

    // Per-frame palette log: begin_frame() snapshots the palette the beam starts the frame with and clears the log; the
    // writes since then are in `palette_writes()` and `render_line` applies them as the beam passes.
    void begin_frame();
    const std::vector<PaletteWrite>& palette_writes() const { return writes_; }
    // Decodes display line `y` (0-199) of the picture into rgba[y*320..]: screen memory at `row_addr` (160 bytes), the beam
    // palette at the start of the line and the logged writes inside it; `line_start` is the frame cycle the line begins at.
    // Medium and high resolution (`resolution` != 0) give a black line. Call for consecutive lines of a frame.
    // With `index_row` (320 bytes) and `palette_row` (16 words) the line's colour indices and the palette it starts with
    // (0x00RRGGBB) are written too: the indexed frame asset packs match screens on (modern/indexed.h).
    void render_line(const uint8_t* ram, uint32_t ram_size, int y, uint32_t row_addr, uint32_t line_start, uint8_t resolution,
                     std::vector<uint8_t>& rgba, uint8_t* index_row = nullptr, uint32_t* palette_row = nullptr);
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
    void apply_writes_before(uint32_t t);
    uint32_t base_ = 0;
    uint32_t counter_base_ = 0;
    uint8_t sync_ = 0xFE;
    uint8_t res_ = 0;
    uint8_t mem_config_ = 0x0A;
    std::array<uint16_t, 16> pal_{};
    // The beam's view of the palette (what the display shows now) and the write log it is catching up with.
    std::array<uint16_t, 16> beam_pal_{};
    std::vector<PaletteWrite> writes_;
    size_t next_write_ = 0;
};

} // namespace x2::st
