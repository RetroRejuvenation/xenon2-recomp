// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Where one game build keeps the things the modernisation layer watches (docs/re-notes/cdtv-modern.md).
//
// The disk builds share one layout. The CDTV release's engine is the same code re-assembled with long absolute
// addressing, so every address moved: the layout below was mapped by instruction shape (tools/modern/map_cdtv.py) and
// confirmed by running the game (the per-layer blit counts and the object records match the disk build for the same
// scene). All code addresses are the PCs REG_PPC shows for the instruction that wrote BLTSIZE or a star pixel.
#pragma once
#include <array>
#include <cstdint>

namespace x2::modern {

// Game variables sampled at each swap (memory-map.md, timing.md): the ring row and scroll state of the far layer and
// the tiles, the speed, the frame counter and two more used by the object passes.
enum Var { kRing = 0, kScrollCD0, kMapRow, kFineScroll, kSpeed, kFrameCount, kVarCEA, kVarCE8, kVarCount };
constexpr int kStars = 48; // star table entries: {line offset.w, pixel mask.w, sub-line accumulator.w}

struct CaptureLayout {
    const char* name;
    uint32_t draw_ptr, show_ptr;                 // the two screen buffer pointers the frame swap exchanges
    std::array<uint32_t, 5> list_heads;          // object list head pointers (next link at +$12)
    uint32_t object_draw_return;                 // return address of run_object_list_draw's `jsr (a1)`
    std::array<uint32_t, kVarCount> var_addr;    // indexed by Var
    uint32_t star_table;                         // 48 x 3 words
    uint32_t far_copy_a, far_copy_b, far_clear;  // blit_objects_main's far layer: two ring copies and the clear of planes 2-3
    uint32_t tile_lo, tile_hi;                   // draw_background_a/b: [lo, hi)
    uint32_t star_lo, star_hi;                   // object_pass_a's four star groups: [lo, hi)
    std::array<uint32_t, 3> star_split;          // where the second, third and fourth group start (colours 5, 6 and 7)
    // The star routine's captured writes are replayed as the game did them at an unshifted position, without the per-pixel
    // "only on empty background" rule (which the game applies word-wide itself). The CDTV release's level 4 draws star table
    // entries with multi-pixel masks that the per-pixel rule refuses; the disk builds keep the rule (their recorded
    // interpolated-picture goldens depend on it).
    bool star_writes_exact;

    bool far_copy_pc(uint32_t pc) const { return pc == far_copy_a || pc == far_copy_b; }
    bool far_pc(uint32_t pc) const { return far_copy_pc(pc) || pc == far_clear; }
    bool tile_pc(uint32_t pc) const { return pc >= tile_lo && pc < tile_hi; }
    bool star_pc(uint32_t pc) const { return pc >= star_lo && pc < star_hi; }
    // The planes a star write may set: the four star groups use colours 4, 5, 6 and 7.
    uint8_t star_planes(uint32_t pc) const {
        if (pc < star_split[0]) return 0x4;
        if (pc < star_split[1]) return 0x5;
        if (pc < star_split[2]) return 0x6;
        return 0x7;
    }
};

// Every disk build: canonical, the retail pressings, the Power Pack and the cracks share these addresses, and the 1762
// compile only moved code after $D000, where none of the drawing routines live.
extern const CaptureLayout kDiskLayout;
// The CDTV release (X2 linked at $400): draw and show pointers $408/$40C, the variable block at the disk addresses + 6.
extern const CaptureLayout kCdtvLayout;

} // namespace x2::modern
