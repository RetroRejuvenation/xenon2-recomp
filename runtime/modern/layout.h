// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Where one game build keeps the things the modernisation layer watches (docs/re-notes/cdtv-modern.md).
//
// The disk builds share one layout. The CDTV release's engine is the same code re-assembled with long absolute
// addressing, so every address moved: the layout below was mapped by instruction shape (tools/modern/map_cdtv.py) and
// confirmed by running the game (the per-layer blit counts and the object records match the disk build for the same
// scene). All code addresses are the PCs REG_PPC shows for the instruction that wrote BLTSIZE or a star pixel.
#pragma once
#include "pack/pack_api.h"

#include <array>
#include <cstdint>

namespace x2::modern {

// "Not mapped yet": the value of a layout field that an asset-pack workstream still has to measure or map
// (docs/re-notes/asset-packs-v2-design.md, section 10, "Measure items first"). Nothing may use a field that holds it;
// the workstream that fills it replaces the sentinel with the measured value and a test.
constexpr uint32_t kUnknownAddr = 0xFFFFFFFFu;
constexpr int kUnknown = -1;

// Game variables sampled at each swap (memory-map.md, timing.md): the ring row and scroll state of the far layer and
// the tiles, the speed, the frame counter and two more used by the object passes.
enum Var { kRing = 0, kScrollCD0, kMapRow, kFineScroll, kSpeed, kFrameCount, kVarCEA, kVarCE8, kVarCount };
constexpr int kStars = 48; // star table entries: {line offset.w, pixel mask.w, sub-line accumulator.w}

// The status panel strip of the window (spec 4.5): `lines` rows from window line `first_line`, split into cells of
// `cell_w` pixels, matched in runs of at most `max_span` cells.
struct PanelLayout {
    int first_line = kUnknown, lines = kUnknown, cell_w = kUnknown, max_span = kUnknown;
    bool known() const { return first_line != kUnknown; }
};

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
    // Asset packs v2 (spec 7). The level number word, sampled with the other variables once WS2 adds it to Var (it is not in
    // `var_addr` yet: the draw log prints every Var, and the existing logs must not change). The far layer's ring height in
    // lines and the scroll direction (+1: the ring row grows as the picture scrolls up, -1: the other way, 0: not measured).
    uint32_t level_var = kUnknownAddr;
    int far_ring_lines = kUnknown;
    int far_scroll_sign = 0;
    PanelLayout panel;

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

// The Atari ST draws with the CPU, so none of the capture addresses exist: every one holds kUnknownAddr and only the
// fields the screens and panel passes use are filled (the level word, which the ST keeps where the disk builds do).
extern const CaptureLayout kStLayout;

// Where a game's indexed frame lives (spec 3.4): the window rectangle inside the faithful frame, and the colour index range.
struct ScreenGeometry {
    const char* name;
    int x, y, w, h;   // the window inside the faithful frame
    int colours;      // indices 0..colours-1
};
extern const ScreenGeometry kAmigaScreen;  // 320 x 200 at kFrameTop of the 320 x 256 frame (modern/present.h), 32 indices
extern const ScreenGeometry kStScreen;     // the 320 x 200 picture, 16 indices

// What the audio side of packs hooks into, per game family (spec 5, 7): where effects and speech come from, which side
// each Paula channel plays on, and the music cues. Addresses are absolute PCs / RAM addresses of the running image.
struct AudioLayout {
    const char* name;
    uint32_t effect_lo, effect_hi;    // [lo, hi): the effect code, whose AUDxLC writes mark a sound effect (disk builds)
    uint32_t speech_lo, speech_hi;    // [lo, hi): the speech buffer a speech clip is read from (CDTV)
    std::array<uint8_t, 4> side;      // per AUD channel: 0 left, 1 right, 0xFF mono / not applicable
    uint32_t level_word;              // the game's level number word
    uint32_t music_init, music_stop;  // the tracker driver's entries (disk builds)
    uint32_t title_call;              // where the title calls music_init
    std::array<uint32_t, 4> level_calls; // where in-game starts call it
    uint32_t cd_play, cd_stop, cd_tick, cd_volume_full, cd_volume_mute; // the CDTV program's CD routines (traps)
    uint32_t cd_track_var;            // long: the CD track playing, -1 for none
    uint32_t music_shadow;            // the driver's shadow channel records (disk builds)
};
extern const AudioLayout kDiskAudio, kCdtvAudio, kStAudio;

// The tables of a game family. Build tags differ in addresses only where a build moves code; no pack table does yet, so
// every disk build (canonical, retail pressings, Power Pack, 1762, PDos, cracks) shares the disk entries.
const CaptureLayout& capture_layout_for(pack::GameFamily g);
const ScreenGeometry& screen_geometry_for(pack::GameFamily g);
const AudioLayout& audio_layout_for(pack::GameFamily g);

// Every disk build: canonical, the retail pressings, the Power Pack and the cracks share these addresses, and the 1762
// compile only moved code after $D000, where none of the drawing routines live.
extern const CaptureLayout kDiskLayout;
// The CDTV release (X2 linked at $400): draw and show pointers $408/$40C, the variable block at the disk addresses + 6.
extern const CaptureLayout kCdtvLayout;

} // namespace x2::modern
