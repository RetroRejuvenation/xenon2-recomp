// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Asset pack screens and status panel (spec sections 4.4 and 4.5; docs/re-notes/asset-packs-screens.md).
//
//   ScreenMatcher  which pack screen the current indexed frame is, by block signature.
//   fade_ratio     how far the game has faded the frame, from the palette colours behind the indices.
//   PackScreens    the presentation stage: matching with hysteresis, the screen picture composited at the canvas scale
//                  (fade, `dynamic` rectangles, widescreen sides), or, during play, the panel strip replaced cell run by cell run.
//
// Presentation only: nothing here touches machine state, and with no screens and no panel in the library nothing is drawn.
#pragma once
#include "modern/indexed.h"
#include "modern/layout.h"
#include "pack/pack_api.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace x2::modern {

struct ScreenMatch {
    const pack::ScreenEntry* entry = nullptr;
    int matched = 0;   // signature blocks that matched
};

// Counts the signature blocks of `e` that match the frame (blocks outside the frame never match). Stops early once the
// minimum can no longer be reached; the count is then below `e.signature.min`.
int count_signature_matches(const pack::ScreenEntry& e, const IndexedFrame& f);

class ScreenMatcher {
public:
    // The best screen of `screens` for the frame: the highest share of matched blocks among those that reach their minimum;
    // equal shares go to the later one in load order (a later pack overrides an earlier one). `prefer` (the screen matched
    // for the previous frame) is kept while at least `min - 2` of its blocks still match, so a block that changes briefly
    // does not flicker the picture.
    static ScreenMatch match(const std::vector<const pack::ScreenEntry*>& screens, const IndexedFrame& f,
                             const pack::ScreenEntry* prefer = nullptr);
};

// How far the game has faded a frame, per channel: either a multiplier (0..1) or an amount (0..255) taken off, whichever reproduces
// the current palette better. The Amiga game fades by taking a fixed step off every colour (a fade-in is the same steps backwards).
struct FadeRatio {
    float r = 1, g = 1, b = 1;       // multiplier
    int sub_r = 0, sub_g = 0, sub_b = 0;  // amount taken off, when `subtractive`
    bool subtractive = false;
    bool unity() const { return subtractive ? !(sub_r | sub_g | sub_b) : (r >= 1 && g >= 1 && b >= 1); }
};
// The game's fade of the frame against a reference palette (a screen's `ref_palette`): the colours the frame's lines give the indices
// it uses, against the reference colours of the same indices (outside the `dynamic` rectangles). Unity when the screen does not fade
// (`fade: none`, no reference palette) and for a frame within 2 % of the reference.
FadeRatio fade_ratio(const pack::ScreenEntry& e, const IndexedFrame& f);
// The same over the window lines [y0, y1) for any reference palette, leaving out the `skip_rects`.
FadeRatio fade_ratio_rows(const std::vector<uint32_t>& ref_palette, pack::FadeMode mode, const IndexedFrame& f, int y0, int y1,
                          const std::vector<pack::Rect>& skip_rects);

// What apply() drew.
struct DrawResult {
    bool screen = false, panel = false;
};

class PackScreens {
public:
    // The library to draw from (kept: it must outlive this object) and the build's panel strip. A null library or one without
    // screens and panel makes every call a no-op.
    void set_library(const pack::PackLibrary* lib, const PanelLayout& panel);
    bool has_screens() const { return !screens_.empty(); }
    bool has_panel() const { return panel_ && !panel_->empty() && layout_.known(); }
    bool active() const { return has_screens() || has_panel(); }
    // The largest picture scale of the screens and panel (1 when there are none): the canvas scale that shows them exactly.
    int pack_scale() const;

    // Draws onto a canvas that already shows the faithful window `S` times enlarged: the window's top-left canvas pixel is
    // canvas[oy * stride + ox], `side` Amiga pixels of surround lie on each side of it, and (cw, ch) is the canvas size.
    // `frame_serial` changes with every emulated frame (matching and the fade are done once per frame); `in_game` allows the
    // panel; `allow_screens` is false while the game is replayed from captured steps (a screen never matches then).
    DrawResult apply(const IndexedFrame& f, uint64_t frame_serial, bool in_game, bool allow_screens, uint32_t* canvas,
                     size_t stride, int cw, int ch, int S, int ox, int oy, int side);
    // The same on a canvas of its own (the window only, S x enlarged): returns false when nothing was drawn.
    bool render_window(const IndexedFrame& f, uint64_t frame_serial, bool in_game, int S, std::vector<uint32_t>& out);

    // The name of the screen the last apply() drew ("" none) and counters for the tests and the logs.
    const std::string& screen_name() const { return name_; }
    // The fade applied to the screen drawn last (1 when none or not fading).
    const FadeRatio& last_fade() const { return fade_; }
    bool last_drew_panel() const { return last_panel_; }
    const FadeRatio& last_panel_fade() const { return panel_fade_; }
    uint64_t frames_with_screen = 0, frames_with_panel = 0, matches_run = 0;

private:
    struct View {
        const uint32_t* px = nullptr;
        int w = 0, h = 0;
    };
    struct Placement {
        int col = 0, span = 0;
        const pack::PanelCell* cell = nullptr;
    };
    View scaled(const pack::Picture* p, int pic_scale, int S);
    void draw_picture(uint32_t* canvas, size_t stride, int cw, int ch, int x0, int y0, const View& v, const FadeRatio& fade);
    void draw_screen(const IndexedFrame& f, const pack::ScreenEntry& e, const FadeRatio& fade, uint32_t* canvas, size_t stride,
                     int cw, int ch, int S, int ox, int oy, int side);
    bool draw_panel(const IndexedFrame& f, uint32_t* canvas, size_t stride, int cw, int ch, int S, int ox, int oy);

    const pack::PackLibrary* lib_ = nullptr;
    std::vector<const pack::ScreenEntry*> screens_;
    const pack::PanelSet* panel_ = nullptr;
    PanelLayout layout_;
    std::map<std::tuple<const pack::Picture*, int, int>, std::vector<uint32_t>> cache_;
    std::map<std::tuple<const pack::Picture*, int, int>, std::pair<int, int>> cache_size_;
    uint64_t serial_ = ~0ull;
    const pack::ScreenEntry* current_ = nullptr;
    FadeRatio fade_;
    std::string name_;
    bool last_panel_ = false;
    FadeRatio panel_fade_;
    // The panel placements of the strip as last seen.
    uint64_t strip_hash_ = 0;
    bool strip_valid_ = false;
    std::vector<Placement> placements_;
    std::vector<uint32_t> saved_;
};

} // namespace x2::modern
