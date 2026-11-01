// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// `--dump-assets` for screens and the status panel (spec 4.4, 4.5; docs/re-notes/asset-packs-screens.md).
//
// Fed one indexed frame per emulated frame, it finds the screens the game shows (runs of frames whose non-flat 16 x 16 blocks stay
// put), writes each as a PNG with its block signature, reference palette and `dynamic` rectangles, and writes every distinct
// 8 x 8 panel cell seen during play as a PNG keyed by its content. The result is a valid version 2 pack at scale 1: loaded back,
// every screen it matched shows the original picture.
//
// Differences from the spec's sketch, chosen so the dump reproduces the original exactly: every frame is looked at, not every
// fifth, and a block belongs to the signature only if it never changed during the screen's life (the spec allowed 90 %); a block
// that changed at any time becomes part of a `dynamic` rectangle.
#pragma once
#include "modern/indexed.h"
#include "modern/layout.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace x2::modern {

class ScreenDumper {
public:
    static constexpr int kBlock = 16;
    static constexpr int kMaxSignature = 40;        // the first choice; up to 64 with the blocks added to tell the screen from other frames
    static constexpr int kMinStable = 6;        // fewer stable blocks: not matchable
    static constexpr int kMinFrames = 12;       // a screen shown for fewer frames is a transition, not a screen
    static constexpr int kMinFramesInGame = 45; // ... and when the game was in play throughout: a level that stands still is not a screen
    static constexpr int kMaxDynamic = 32;
    static constexpr size_t kMaxScreens = 512;
    static constexpr size_t kMaxFrameGrids = 60000;

    explicit ScreenDumper(const PanelLayout& panel) : panel_(panel) {}

    // One frame of the run. `in_game` (the game has ships left) allows the panel; `gameplay` marks frames known to be play (a captured
    // step exists): they are never screens, and are kept (a sample) to check the signatures against.
    void frame(const IndexedFrame& f, uint32_t frame_no, bool in_game, bool gameplay);

    // Closes the open screen, writes the PNGs to dir/screens and dir/panel, and returns the manifest members ("screens": {...},
    // "panel": {...}) without braces around them; empty when there is nothing to write.
    bool finish(const std::string& dir, std::string& members, std::string& why);

    size_t screens() const { return screens_.size(); }
    size_t panel_cells() const { return cells_.size(); }
    size_t not_matchable() const { return not_matchable_; }
    size_t transitions() const { return transitions_; }
    // Collisions between signatures and other screens or sampled gameplay frames, and other findings, one line each.
    const std::vector<std::string>& warnings() const { return warnings_; }
    // How many different palettes the panel strip was drawn with during the run (fades, shops, levels). The cell pictures are painted
    // with the one it had in most frames, which is also the panel's `ref_palette`.
    size_t strip_palettes() const { return strip_pals_.size(); }

private:
    static constexpr int kMaxCells = 4096;
    struct Life {
        bool open = false, all_in_game = true;
        uint32_t first_frame = 0, frames = 0;
        std::vector<uint64_t> first_hash;
        std::vector<uint8_t> changed, flat;      // per grid block
        bool bottom_changed = false;
        uint64_t bottom_first = 0;
        // the brightest frame of the life gives the picture's content; the palette is the one most frames had
        double brightness = -1;
        uint32_t best_frame = 0;
        std::vector<uint8_t> index;
        std::vector<uint32_t> pal;
        struct PalCount {
            uint32_t frames = 0;
            std::vector<uint32_t> pal;
        };
        std::map<uint64_t, PalCount> pals;
    };
    struct Shot {
        std::string name;
        int block = kBlock, min = 0;
        std::vector<std::array<int, 2>> pos;
        std::vector<uint64_t> hash;
        uint32_t first_frame = 0, frames = 0, uses = 0, best_frame = 0, pal_frames = 0;
        double brightness = -1;
        std::vector<uint8_t> changed, flat;
        std::vector<uint64_t> first_hash;         // the grid of the first frame of the life
        bool bottom_changed = false;
        std::vector<uint8_t> index;
        std::vector<uint32_t> pal;
        std::vector<uint64_t> grid;               // block hashes of the representative frame (collision checks)
        bool drop = false;
    };
    struct Cell {
        std::vector<uint8_t> index;               // cell_w x lines colour indices
        uint32_t uses = 0, first_frame = 0;
    };
    struct StripPalette {
        uint32_t frames = 0;
        std::vector<uint32_t> lines;              // the strip's lines x colours, 0x00RRGGBB
    };

    void grid_of(const IndexedFrame& f, std::vector<uint64_t>& hashes, std::vector<uint8_t>& flat) const;
    void close_life();
    void record_frame(const std::vector<uint64_t>& hashes, bool gameplay);
    void collect_panel(const IndexedFrame& f, uint32_t frame_no);

    PanelLayout panel_;
    int gw_ = 0, gh_ = 0, w_ = 0, h_ = 0, colours_ = 0;
    Life life_;
    std::vector<uint64_t> prev_hash_, cur_hash_;
    std::vector<uint8_t> prev_flat_, cur_flat_;
    bool have_prev_ = false;
    std::map<std::string, Shot> screens_;
    std::map<uint64_t, Cell> cells_;
    std::map<std::vector<uint32_t>, StripPalette> strip_pals_;
    // The block grid of every frame shown outside play (and of a sample of the play frames), to check the finished signatures against.
    std::vector<std::vector<uint64_t>> frame_grids_;
    std::vector<uint8_t> frame_gameplay_;
    uint64_t gameplay_seen_ = 0;
    size_t not_matchable_ = 0, transitions_ = 0;
    std::vector<std::string> warnings_;
};

} // namespace x2::modern
