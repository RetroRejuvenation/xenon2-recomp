// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Asset packs, workstream 2a: screens and the status panel (modern/screens.h, screen_pack.h, screen_dump.h), on synthetic frames only.
// Matching (signatures, minimum, hysteresis, collisions), the fade fit, compositing at several canvas scales (dynamic rectangles,
// anchors, widescreen pictures), the panel's cell runs, the manifest loader and its errors, the dump finding screens and writing a pack
// that reproduces the run, and the ST shifter's index plane that the ST present hook draws from.
//
//   x2_screens_tests SCRATCH_DIR
#include "gfx/image_io.h"
#include "modern/indexed.h"
#include "modern/layout.h"
#include "modern/screen_dump.h"
#include "modern/screen_pack.h"
#include "modern/screens.h"
#include "import/sha1.h"
#include "pack/pack_api.h"
#include "pack/pack_json.h"
#include "pack/pack_stack.h"
#include "st/shifter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace x2;
using namespace x2::modern;

namespace {
namespace fs = std::filesystem;
int g_failures = 0;
int g_checks = 0;
fs::path g_scratch;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

constexpr int kW = 320, kH = 200, kColours = 32;

uint32_t to_rgba(uint32_t rgb) { return 0xFF000000u | (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF); }

// A synthetic indexed frame: indices, one palette for every line, and the colours they give (the frame the game "rendered").
struct Frame {
    std::vector<uint8_t> idx = std::vector<uint8_t>(size_t(kW) * kH, 0);
    std::vector<uint32_t> pal = std::vector<uint32_t>(size_t(kH) * kColours, 0);
    std::vector<uint32_t> rgba = std::vector<uint32_t>(size_t(kW) * kH, 0xFF000000u);
    Frame() { palette(default_palette()); }
    static std::vector<uint32_t> default_palette() {
        std::vector<uint32_t> p(kColours, 0);
        const uint32_t base[] = {0x000000, 0xEE0000, 0x00EE00, 0x0000EE, 0xEEEE00, 0xEE00EE, 0x00EEEE, 0xEEEEEE,
                                 0x884400, 0x448800, 0x004488, 0x888888, 0x440000, 0x004400, 0x000044, 0xCCAA88};
        for (size_t i = 0; i < 16; ++i) p[i] = base[i];
        for (size_t i = 16; i < p.size(); ++i) p[i] = base[i - 16] ^ 0x111111;
        return p;
    }
    void palette(const std::vector<uint32_t>& p) {
        for (int y = 0; y < kH; ++y) std::copy(p.begin(), p.end(), pal.begin() + long(y * kColours));
        render();
    }
    void render() {
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) rgba[size_t(y) * kW + size_t(x)] = to_rgba(pal[size_t(y) * kColours + idx[size_t(y) * kW + size_t(x)]]);
    }
    void rect(int x, int y, int w, int h, uint8_t i) {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx) idx[size_t(yy) * kW + size_t(xx)] = i;
        render();
    }
    // A pattern that differs from block to block, so that blocks have distinct hashes and are not flat.
    void pattern(int seed) {
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) idx[size_t(y) * kW + size_t(x)] = uint8_t(1 + ((x / 3 + y / 5) * 7 + seed * 13 + x / 16 * 5 + y / 16 * 11) % 14);
        render();
    }
    IndexedFrame view() const {
        IndexedFrame f;
        f.w = kW;
        f.h = kH;
        f.colours = kColours;
        f.index = idx.data();
        f.stride = kW;
        f.pal = pal.data();
        f.rgba = rgba.data();
        f.rgba_stride = kW;
        return f;
    }
};

pack::Picture picture_of(const Frame& f, int scale = 1) {
    pack::Picture p;
    p.w = kW * scale;
    p.h = kH * scale;
    p.px.resize(size_t(p.w) * size_t(p.h));
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) p.px[size_t(y) * size_t(p.w) + size_t(x)] = f.rgba[size_t(y / scale) * kW + size_t(x / scale)];
    return p;
}

pack::Picture solid(int w, int h, uint32_t rgba) {
    pack::Picture p;
    p.w = w;
    p.h = h;
    p.px.assign(size_t(w) * size_t(h), rgba);
    return p;
}

// A signature of the given 16 x 16 grid blocks of `f`; `min` of 0 asks for every block.
pack::ScreenEntry screen_of(const Frame& f, const char* name, std::initializer_list<std::pair<int, int>> blocks, int min = 0) {
    pack::ScreenEntry e;
    e.name = name;
    e.signature.block = 16;
    for (const auto& b : blocks) {
        e.signature.blocks.push_back({b.first * 16, b.second * 16, pack::block_hash_region(f.idx.data(), kW, b.first * 16, b.second * 16, 16, 16)});
    }
    e.signature.min = min ? min : int(e.signature.blocks.size());
    return e;
}

std::vector<uint32_t> render(PackScreens& ps, const Frame& f, uint64_t serial, bool in_game, int S, bool* drew = nullptr) {
    std::vector<uint32_t> out;
    const bool d = ps.render_window(f.view(), serial, in_game, S, out);
    if (drew) *drew = d;
    return out;
}

std::vector<uint32_t> faithful(const Frame& f, int S) {
    std::vector<uint32_t> out(size_t(kW) * kH * size_t(S) * size_t(S));
    for (int y = 0; y < kH * S; ++y)
        for (int x = 0; x < kW * S; ++x) out[size_t(y) * size_t(kW * S) + size_t(x)] = f.rgba[size_t(y / S) * kW + size_t(x / S)];
    return out;
}

constexpr PanelLayout kPanel{192, 8, 8, 6};

// ---------------------------------------------------------------------------------------------------------------------------------

void test_matcher() {
    Frame a, b, c;
    a.pattern(1);
    b.pattern(2);
    c = a;
    c.rect(0, 0, 16, 16, 7);   // block (0,0) of `c` differs
    ScreenPackLibrary lib;
    const auto* sa = lib.add_screen(screen_of(a, "a", {{0, 0}, {3, 2}, {10, 5}, {19, 11}, {7, 7}}));
    const auto* sb = lib.add_screen(screen_of(b, "b", {{0, 0}, {3, 2}, {10, 5}, {19, 11}, {7, 7}}));
    CHECK(count_signature_matches(*sa, a.view()) == 5);
    CHECK(count_signature_matches(*sa, b.view()) == 0);
    CHECK(count_signature_matches(*sa, c.view(), 4) == 4 && count_signature_matches(*sa, c.view()) < 5);
    CHECK(ScreenMatcher::match(lib.screens(), a.view()).entry == sa);
    CHECK(ScreenMatcher::match(lib.screens(), b.view()).entry == sb);
    CHECK(ScreenMatcher::match(lib.screens(), c.view()).entry == nullptr);   // asks for every block
    Frame blank;
    CHECK(ScreenMatcher::match(lib.screens(), blank.view()).entry == nullptr);

    // a minimum below the block count tolerates misses; hysteresis keeps the screen through two more
    pack::ScreenEntry loose = screen_of(a, "loose", {{0, 0}, {3, 2}, {10, 5}, {19, 11}, {7, 7}, {5, 3}, {12, 9}, {1, 10}}, 6);
    ScreenPackLibrary lib2;
    const auto* sl = lib2.add_screen(loose);
    Frame d = a;
    d.rect(0, 0, 16, 16, 7);
    d.rect(48, 32, 16, 16, 7);
    CHECK(count_signature_matches(*sl, d.view()) == 6);
    CHECK(ScreenMatcher::match(lib2.screens(), d.view()).entry == sl);
    Frame e = d;
    e.rect(160, 80, 16, 16, 7);   // a third block gone: 5 of 8
    CHECK(ScreenMatcher::match(lib2.screens(), e.view()).entry == nullptr);
    CHECK(ScreenMatcher::match(lib2.screens(), e.view(), sl).entry == sl);   // kept while the previous screen still has min - 2 = 4
    Frame g = e;
    g.rect(192, 144, 16, 16, 7);
    g.rect(80, 48, 16, 16, 7);
    CHECK(ScreenMatcher::match(lib2.screens(), g.view(), sl).entry == nullptr);   // 3 of 8: let go
    // a signature that asks for all of its blocks gets no leeway from the hysteresis
    CHECK(ScreenMatcher::match(lib.screens(), c.view(), sa).entry == nullptr);

    // two screens match: the better share wins, equal shares go to the later one
    ScreenPackLibrary lib3;
    const auto* s1 = lib3.add_screen(screen_of(a, "s1", {{0, 0}, {3, 2}, {10, 5}, {19, 11}}, 3));
    const auto* s2 = lib3.add_screen(screen_of(a, "s2", {{0, 0}, {3, 2}, {10, 5}, {5, 3}, {12, 9}, {7, 7}}, 3));
    CHECK(ScreenMatcher::match(lib3.screens(), a.view()).entry == s2);   // both match fully: the later
    Frame h = a;
    h.rect(19 * 16, 11 * 16, 16, 16, 7);
    CHECK(ScreenMatcher::match(lib3.screens(), h.view()).entry == s2);   // s1 3/4, s2 6/6
    Frame k = a;
    k.rect(5 * 16, 3 * 16, 16, 16, 7);
    CHECK(ScreenMatcher::match(lib3.screens(), k.view()).entry == s1);   // s1 4/4, s2 5/6
    CHECK(lib3.screens_for_block(pack::block_hash_region(a.idx.data(), kW, 0, 0, 16, 16)).size() == 2);
    CHECK(lib3.screens_for_block(1).empty());

    // blocks outside the frame never match
    pack::ScreenEntry out = screen_of(a, "out", {{0, 0}});
    out.signature.blocks.push_back({312, 192, 0});
    out.signature.min = 2;
    CHECK(count_signature_matches(out, a.view()) == 1);
}

void test_fade() {
    Frame f;
    f.pattern(3);
    pack::ScreenEntry e = screen_of(f, "e", {{0, 0}});
    e.ref_palette = Frame::default_palette();
    CHECK(fade_ratio(e, f.view()).unity());
    // a multiplicative fade
    Frame half = f;
    std::vector<uint32_t> p = Frame::default_palette();
    for (auto& c : p) c = ((c >> 16 & 255) / 2) << 16 | ((c >> 8 & 255) / 2) << 8 | (c & 255) / 2;
    half.palette(p);
    FadeRatio r = fade_ratio(e, half.view());
    CHECK(!r.unity() && !r.subtractive && r.r > 0.45f && r.r < 0.55f && r.max_dev <= 1);
    // the game's own fade: a fixed step taken off every colour (the Amiga's, in steps of 0x22)
    for (int step = 1; step <= 6; ++step) {
        std::vector<uint32_t> q = Frame::default_palette();
        for (auto& c : q) {
            auto sub = [&](uint32_t v) { return v > uint32_t(step * 0x22) ? v - uint32_t(step * 0x22) : 0u; };
            c = sub(c >> 16 & 255) << 16 | sub(c >> 8 & 255) << 8 | sub(c & 255);
        }
        Frame fd = f;
        fd.palette(q);
        const FadeRatio s = fade_ratio(e, fd.view());
        CHECK(s.subtractive && s.sub_r == step * 0x22 && s.sub_g == step * 0x22 && s.sub_b == step * 0x22 && s.explained());
    }
    // `fade: none` and no reference palette do not fade; a palette that is not a fade is not explained
    pack::ScreenEntry none = e;
    none.fade = pack::FadeMode::None;
    CHECK(fade_ratio(none, half.view()).unity());
    pack::ScreenEntry noref = e;
    noref.ref_palette.clear();
    CHECK(fade_ratio(noref, half.view()).unity());
    Frame odd = f;
    std::vector<uint32_t> o = Frame::default_palette();
    std::swap(o[1], o[2]);
    std::swap(o[3], o[4]);
    odd.palette(o);
    CHECK(!fade_ratio(e, odd.view()).explained());
    // dynamic rectangles are left out of the fit: a changing area does not read as a fade
    Frame live = f;
    live.rect(16, 16, 32, 32, 7);
    pack::ScreenEntry dyn = e;
    dyn.dynamic.push_back({16, 16, 32, 32});
    CHECK(fade_ratio(dyn, live.view()).unity());
}

void test_compose() {
    Frame orig, art;
    orig.pattern(1);
    art.pattern(2);   // the replacement picture is made of other things
    for (int S : {1, 2, 3, 4, 8}) {
        ScreenPackLibrary lib;
        pack::ScreenEntry e = screen_of(orig, "title", {{0, 0}, {5, 5}, {12, 8}});
        e.pic = lib.add_picture(picture_of(art, 1));
        e.scale = 1;
        lib.add_screen(e);
        PackScreens ps;
        ps.set_library(&lib, kPanel);
        CHECK(ps.active() && ps.has_screens() && !ps.has_panel() && ps.pack_scale() == 1);
        bool drew = false;
        const auto out = render(ps, orig, 1, false, S, &drew);
        CHECK(drew && ps.screen_name() == "title");
        CHECK(out == faithful(art, S));   // integer enlargement replicates the pack's pixels
        // another frame: nothing matches, the original shows
        Frame other;
        other.pattern(9);
        const auto out2 = render(ps, other, 2, false, S, &drew);
        CHECK(!drew && out2 == faithful(other, S) && ps.screen_name().empty());
    }
    // a picture at scale 4 shown at scale 2 is the area average; at scale 4 it is the picture itself
    {
        ScreenPackLibrary lib;
        pack::ScreenEntry e = screen_of(orig, "hd", {{0, 0}, {5, 5}});
        pack::Picture big = solid(kW * 4, kH * 4, 0xFF204060u);
        e.pic = lib.add_picture(std::move(big));
        e.scale = 4;
        lib.add_screen(e);
        PackScreens ps;
        ps.set_library(&lib, kPanel);
        CHECK(ps.pack_scale() == 4);
        const auto o4 = render(ps, orig, 1, false, 4);
        CHECK(o4.size() == size_t(kW * 4) * size_t(kH * 4) && o4[0] == 0xFF204060u && o4.back() == 0xFF204060u);
        const auto o2 = render(ps, orig, 2, false, 2);
        CHECK(o2.size() == size_t(kW * 2) * size_t(kH * 2) && o2[12345] == 0xFF204060u);
        const auto o3 = render(ps, orig, 3, false, 3);
        CHECK(o3.size() == size_t(kW * 3) * size_t(kH * 3) && o3[777] == 0xFF204060u);
    }
    // the fade follows the game's: the picture darkens by the same step
    {
        ScreenPackLibrary lib;
        pack::ScreenEntry e = screen_of(orig, "fade", {{0, 0}, {5, 5}});
        e.pic = lib.add_picture(solid(kW, kH, 0xFFE0E0E0u));
        e.ref_palette = Frame::default_palette();
        lib.add_screen(e);
        PackScreens ps;
        ps.set_library(&lib, kPanel);
        Frame dim = orig;
        std::vector<uint32_t> q = Frame::default_palette();
        for (auto& c : q) {
            auto sub = [](uint32_t v) { return v > 0x44u ? v - 0x44u : 0u; };
            c = sub(c >> 16 & 255) << 16 | sub(c >> 8 & 255) << 8 | sub(c & 255);
        }
        dim.palette(q);
        const auto out = render(ps, dim, 1, false, 1);
        CHECK(out[0] == 0xFF9C9C9Cu);   // 0xE0 - 0x44
        // without `fade` the picture stays at full brightness
        pack::ScreenEntry n = screen_of(orig, "nofade", {{0, 0}, {5, 5}});
        n.pic = e.pic;
        n.ref_palette = e.ref_palette;
        n.fade = pack::FadeMode::None;
        ScreenPackLibrary lib2;
        lib2.add_screen(n);
        PackScreens ps2;
        ps2.set_library(&lib2, kPanel);
        CHECK(render(ps2, dim, 1, false, 1)[0] == 0xFFE0E0E0u);
    }
    // dynamic rectangles: what the game draws there (index != bg_index) stays, the rest is the picture
    {
        Frame live = orig;
        live.rect(32, 48, 64, 16, 0);     // the live area is background ...
        live.rect(40, 52, 8, 4, 3);       // ... with text in index 3
        ScreenPackLibrary lib;
        pack::ScreenEntry e = screen_of(live, "dyn", {{0, 0}, {10, 0}, {15, 6}});
        e.pic = lib.add_picture(solid(kW, kH, 0xFF112233u));
        e.dynamic.push_back({32, 48, 64, 16});
        e.bg_index = 0;
        lib.add_screen(e);
        PackScreens ps;
        ps.set_library(&lib, kPanel);
        for (int S : {1, 3}) {
            const auto out = render(ps, live, 1, false, S);
            auto at = [&](int x, int y) { return out[size_t(y) * size_t(kW * S) + size_t(x)]; };
            CHECK(at(0, 0) == 0xFF112233u);                                   // artwork
            CHECK(at(34 * S, 50 * S) == 0xFF112233u);                         // background inside the rectangle: artwork shows through
            CHECK(at(41 * S, 53 * S) == live.rgba[size_t(53) * kW + 41]);     // the game's text
            CHECK(at((40 * S) + S - 1, (52 * S) + S - 1) == live.rgba[size_t(52) * kW + 40]);
            CHECK(at(96 * S, 48 * S) == 0xFF112233u);                         // just outside
        }
    }
    // an anchor lets the picture overhang the window; a wide picture fills the surround
    {
        ScreenPackLibrary lib;
        pack::ScreenEntry e = screen_of(orig, "wide", {{0, 0}, {5, 5}});
        pack::Picture p = solid(kW + 20, kH, 0xFF00FF00u);
        e.pic = lib.add_picture(std::move(p));
        e.anchor = {10, 0};
        e.wide_pic = lib.add_picture(solid(kW + 80, kH, 0xFFFF0000u));
        lib.add_screen(e);
        PackScreens ps;
        ps.set_library(&lib, kPanel);
        const int S = 2, side = 16, cw = (kW + 2 * side) * S, ch = (kH + 56) * S;
        std::vector<uint32_t> canvas(size_t(cw) * size_t(ch), 0xFF000000u);
        // the faithful window in the middle, as the presenter lays it out
        for (int y = 0; y < kH * S; ++y)
            for (int x = 0; x < kW * S; ++x) canvas[size_t(y + 14 * S) * size_t(cw) + size_t(x + side * S)] = orig.rgba[size_t(y / S) * kW + size_t(x / S)];
        const DrawResult r = ps.apply(orig.view(), 1, false, true, canvas.data(), size_t(cw), cw, ch, S, side * S, 14 * S, side);
        CHECK(r.screen && !r.panel);
        auto at = [&](int x, int y) { return canvas[size_t(y) * size_t(cw) + size_t(x)]; };
        CHECK(at(side * S, 14 * S) == 0xFF00FF00u);                  // the picture, its anchor pixel on the window's corner
        CHECK(at(side * S - 3, 14 * S + 5) == 0xFF00FF00u);          // overhanging to the left (10 picture pixels = 20 canvas pixels)
        CHECK(at(side * S - 20, 14 * S + 5) == 0xFF00FF00u);
        CHECK(at(2, 14 * S + 5) == 0xFFFF0000u);                      // the wide picture in the surround beyond it
        CHECK(at(2, 2) == 0xFF000000u);                               // rows outside the window stay as they were
    }
}

void test_panel() {
    Frame f;
    f.rect(0, 0, kW, 192, 0);
    // the strip: a pattern of glyphs at the first four cells, blank elsewhere but a bar in cells 10-12
    auto glyph = [&](int cell, uint8_t i) {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) f.idx[size_t(192 + y) * kW + size_t(cell * 8 + x)] = uint8_t((x + y) % 3 ? i : 0);
    };
    glyph(0, 1);
    glyph(1, 2);
    glyph(2, 1);
    glyph(3, 3);
    for (int c = 10; c <= 12; ++c) glyph(c, 4);
    f.render();
    auto cell_hash = [&](int c, int span) { return pack::block_hash_region(f.idx.data(), kW, c * 8, 192, span * 8, 8); };
    ScreenPackLibrary lib;
    pack::PanelSet& ps = lib.panel_set();
    ps.scale = 1;
    auto add = [&](uint64_t h, int span, uint32_t colour, pack::Anchor anchor = {}) {
        pack::PanelCell cell;
        cell.pic = lib.add_picture(solid(8 * span, 8, colour));
        cell.scale = 1;
        cell.anchor = anchor;
        ps.cells[pack::panel_key(h, span)] = cell;
    };
    add(cell_hash(0, 1), 1, 0xFF0000FFu);                 // glyph 1 at cell 0 and 2 (cell 0 and 2 have the same content)
    add(cell_hash(3, 1), 1, 0xFF00FF00u);
    add(cell_hash(10, 3), 3, 0xFFFF0000u);                // the bar as one run of three
    add(cell_hash(10, 1), 1, 0xFFFFFF00u);                // (and a cell of it: the longer run wins)
    PackScreens stage;
    stage.set_library(&lib, kPanel);
    CHECK(stage.has_panel() && !stage.has_screens());
    bool drew = false;
    auto out = render(stage, f, 1, true, 1, &drew);
    CHECK(drew);
    auto at = [&](int x, int y) { return out[size_t(y) * kW + size_t(x)]; };
    CHECK(at(0, 192) == 0xFF0000FFu && at(7, 199) == 0xFF0000FFu);       // cell 0
    CHECK(at(8, 192) == f.rgba[size_t(192) * kW + 8]);                    // cell 1: no replacement, the original
    CHECK(at(16, 195) == 0xFF0000FFu);                                    // cell 2: same content as cell 0
    CHECK(at(24, 192) == 0xFF00FF00u);                                    // cell 3
    CHECK(at(80, 192) == 0xFFFF0000u && at(103, 199) == 0xFFFF0000u);     // the run of three
    CHECK(at(0, 100) == f.rgba[size_t(100) * kW]);                        // the playfield is not touched
    // not in the game, or a blank strip: nothing is drawn
    CHECK(render(stage, f, 2, false, 1, &drew) == faithful(f, 1) && !drew);
    Frame blank;
    CHECK(render(stage, blank, 3, true, 1, &drew) == faithful(blank, 1) && !drew);
    // the strip changes: its runs are found again
    f.idx[size_t(192) * kW + 1] ^= 0x05;
    f.render();
    out = render(stage, f, 4, true, 1, &drew);
    CHECK(drew && at(0, 192) == f.rgba[size_t(192) * kW] && at(16, 195) == 0xFF0000FFu);   // cell 0 changed, cell 2 did not
    // enlarged: the replacement is replicated, an anchor overhangs the playfield
    ScreenPackLibrary lib2;
    lib2.panel_set().scale = 1;
    {
        pack::PanelCell cell;
        cell.pic = lib2.add_picture(solid(8, 12, 0xFF123456u));
        cell.anchor = {0, 4};      // four picture rows above the strip
        lib2.panel_set().cells[pack::panel_key(cell_hash(3, 1), 1)] = cell;
    }
    PackScreens stage2;
    stage2.set_library(&lib2, kPanel);
    out = render(stage2, f, 1, true, 3, &drew);
    auto at3 = [&](int x, int y) { return out[size_t(y) * size_t(kW * 3) + size_t(x)]; };
    CHECK(drew && at3(24 * 3, 192 * 3) == 0xFF123456u && at3(24 * 3 + 23, 199 * 3 + 2) == 0xFF123456u);
    CHECK(at3(24 * 3, 192 * 3 - 12) == 0xFF123456u);                      // overhang: 4 picture rows = 12 canvas rows up
    CHECK(at3(24 * 3, 192 * 3 - 13) == f.rgba[size_t(190) * kW + 24]);
    // a background is drawn first; cells without a replacement keep the original over it
    ScreenPackLibrary lib3;
    lib3.panel_set().scale = 1;
    lib3.panel_set().background = lib3.add_picture(solid(kW, 8, 0xFF303030u));
    {
        pack::PanelCell cell;
        cell.pic = lib3.add_picture(solid(8, 8, 0xFF0000FFu));
        lib3.panel_set().cells[pack::panel_key(cell_hash(3, 1), 1)] = cell;
        // a blank cell of the original replaced by a transparent picture lets the background show through
        const std::vector<uint8_t> zeros(64, 0);
        pack::PanelCell blank_cell;
        blank_cell.pic = lib3.add_picture(solid(8, 8, 0x00000000u));
        lib3.panel_set().cells[pack::panel_key(pack::block_hash(8, 8, zeros.data()), 1)] = blank_cell;
    }
    PackScreens stage3;
    stage3.set_library(&lib3, kPanel);
    out = render(stage3, f, 1, true, 1, &drew);
    CHECK(drew && at(24, 192) == 0xFF0000FFu);
    CHECK(at(150, 193) == 0xFF303030u);                                   // blank part of the strip: the background
    CHECK(at(8, 192) == f.rgba[size_t(192) * kW + 8] || f.idx[size_t(192) * kW + 8] == 0);   // cell 1 is its own original
    // the panel fades with the game
    ScreenPackLibrary lib4;
    lib4.panel_set().scale = 1;
    lib4.panel_set().ref_palette = Frame::default_palette();
    {
        pack::PanelCell cell;
        cell.pic = lib4.add_picture(solid(8, 8, 0xFFE0E0E0u));
        lib4.panel_set().cells[pack::panel_key(cell_hash(3, 1), 1)] = cell;
    }
    PackScreens stage4;
    stage4.set_library(&lib4, kPanel);
    Frame dim = f;
    std::vector<uint32_t> q = Frame::default_palette();
    for (auto& c : q) {
        auto sub = [](uint32_t v) { return v > 0x22u ? v - 0x22u : 0u; };
        c = sub(c >> 16 & 255) << 16 | sub(c >> 8 & 255) << 8 | sub(c & 255);
    }
    dim.palette(q);
    out = render(stage4, dim, 1, true, 1, &drew);
    CHECK(drew && at(24, 192) == 0xFFBEBEBEu);                            // 0xE0 - 0x22
    out = render(stage4, f, 2, true, 1, &drew);
    CHECK(drew && at(24, 192) == 0xFFE0E0E0u);
}

// ---------------------------------------------------------------------------------------------------------------------------------

void write_png_file(const fs::path& p, const pack::Picture& pic) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::vector<uint8_t> bytes(pic.px.size() * 4);
    std::memcpy(bytes.data(), pic.px.data(), bytes.size());
    x2::write_png(p.string(), bytes, pic.w, pic.h, true);
}

void write_text(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string sig_json(const Frame& f, int count) {
    std::string j = "{\"block\": 16, \"min\": " + std::to_string(count) + ", \"blocks\": [";
    for (int i = 0; i < count; ++i) {
        const int bx = i * 3 % 20, by = i * 5 % 12;
        j += std::string(i ? ", " : "") + "{\"x\": " + std::to_string(bx * 16) + ", \"y\": " + std::to_string(by * 16) + ", \"hash\": \"" +
             pack::hash_hex(pack::block_hash_region(f.idx.data(), kW, bx * 16, by * 16, 16, 16)) + "\"}";
    }
    return j + "]}";
}

// Packs are loaded by the general loader (pack/pack_stack.h): a directory pack or a ZIP.
std::shared_ptr<pack::PackStack> load_pack(const pack::PackContext& ctx, const std::string& path, pack::PackReport& report, bool* ok = nullptr) {
    auto s = std::make_shared<pack::PackStack>(ctx);
    const bool good = s->add(path, report);
    if (ok) *ok = good;
    return s;
}

// A stored ZIP of every file under `dir` (names relative to it, '/' separators), written to `zip`.
void zip_directory(const fs::path& dir, const fs::path& zip) {
    std::string out, cd;
    auto u16 = [](std::string& o, uint32_t v) { o += char(v & 255); o += char(v >> 8 & 255); };
    auto u32 = [&](std::string& o, uint32_t v) { u16(o, v & 0xFFFF); u16(o, v >> 16); };
    uint16_t count = 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        const std::string name = fs::relative(f, dir).generic_string();
        std::ifstream in(f, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const uint32_t crc = x2::import::crc32(reinterpret_cast<const uint8_t*>(data.data()), data.size()), off = uint32_t(out.size());
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, crc); u32(out, uint32_t(data.size())); u32(out, uint32_t(data.size())); u16(out, uint16_t(name.size())); u16(out, 0);
        out += name + data;
        u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0);
        u32(cd, crc); u32(cd, uint32_t(data.size())); u32(cd, uint32_t(data.size())); u16(cd, uint16_t(name.size()));
        u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, off);
        cd += name;
        ++count;
    }
    const uint32_t cd_off = uint32_t(out.size());
    out += cd;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, count); u16(out, count);
    u32(out, uint32_t(cd.size())); u32(out, cd_off); u16(out, 0);
    fs::create_directories(zip.parent_path());
    std::ofstream(zip, std::ios::binary).write(out.data(), std::streamsize(out.size()));
}

void test_loader() {
    const fs::path dir = g_scratch / "pack_ok";
    fs::remove_all(dir);
    Frame f;
    f.pattern(4);
    write_png_file(dir / "screens/one.png", picture_of(f, 1));
    write_png_file(dir / "screens/two.png", solid(kW * 2, kH * 2, 0xFF336699u));
    write_png_file(dir / "screens/wide.png", solid(400, kH, 0xFF999999u));
    write_png_file(dir / "panel/a.png", solid(16, 16, 0xFF778899u));
    write_png_file(dir / "panel/bg.png", solid(kW, 8, 0xFF101010u));
    const std::string key1 = pack::panel_key(0x0123456789abcdefull, 1), key2 = pack::panel_key(0xfedcba9876543210ull, 3);
    write_text(dir / "manifest.json",
               "{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": \"t\", \"scale\": 1,\n"
               "\"screens\": {\n"
               " \"one\": {\"signature\": " + sig_json(f, 6) + ", \"png\": \"screens/one.png\", \"ref_palette\": [\"#000\", \"#fff\", \"#112233\"], \"fade\": \"scale\",\n"
               "          \"dynamic\": [{\"x\": 1, \"y\": 2, \"w\": 3, \"h\": 4}], \"bg_index\": 2, \"wide_png\": \"screens/wide.png\", \"wide_scale\": 1},\n"
               " \"two\": {\"signature\": " + sig_json(f, 3) + ", \"png\": \"screens/two.png\", \"scale\": 2, \"anchor\": [4, -3], \"fade\": \"none\"},\n"
               " \"games_only_st\": {\"signature\": " + sig_json(f, 3) + ", \"png\": \"screens/two.png\", \"scale\": 2, \"games\": [\"st\"]}},\n"
               "\"panel\": {\"scale\": 2, \"cell_w\": 8, \"max_span\": 4, \"ref_palette\": [\"#000000\", \"#eeeeee\"], \"fade\": \"scale\",\n"
               "           \"background\": {\"png\": \"panel/bg.png\"},\n"
               "           \"cells\": {\"" + key1 + "\": {\"png\": \"panel/a.png\", \"anchor\": [0, 2]}, \"" + key2 + "\": {\"png\": \"panel/a.png\", \"scale\": 1}}}\n}");
    pack::PackContext ctx;
    ctx.game = pack::GameFamily::Disk;
    pack::PackReport report;
    bool loaded = false;
    auto lib_stack = load_pack(ctx, dir.string(), report, &loaded);
    const pack::PackStack& lib = *lib_stack;
    CHECK(loaded);
    CHECK(report.count(pack::Severity::Error) == 0);
    CHECK(lib.screens().size() == 2);                   // the ST-only screen is out of scope here
    const pack::ScreenEntry* one = lib.screens()[0];
    const pack::ScreenEntry* two = lib.screens()[1];
    CHECK(one->name == "one" && one->signature.blocks.size() == 6 && one->signature.min == 6 && one->signature.block == 16);
    CHECK(one->scale == 1 && one->pic && one->pic->w == kW && one->ref_palette.size() == 3 && one->ref_palette[1] == 0xFFFFFFu &&
          one->ref_palette[2] == 0x112233u);
    CHECK(one->dynamic.size() == 1 && one->dynamic[0].w == 3 && one->bg_index == 2 && one->wide_pic && one->wide_pic->w == 400);
    CHECK(two->scale == 2 && two->anchor.x == 4 && two->anchor.y == -3 && two->fade == pack::FadeMode::None && two->pic->w == kW * 2);
    CHECK(lib.panel() && lib.panel()->scale == 2 && lib.panel()->max_span == 4 && lib.panel()->cells.size() == 2);
    CHECK(lib.panel()->find(key1) && lib.panel()->find(key1)->scale == 2 && lib.panel()->find(key1)->anchor.y == 2);
    CHECK(lib.panel()->find(key2) && lib.panel()->find(key2)->scale == 1);
    CHECK(lib.panel()->background && lib.panel()->ref_palette.size() == 2 && lib.panel()->ref_palette[1] == 0xEEEEEEu);
    CHECK(lib.has_graphics() && !lib.has_audio() && !lib.empty());
    pack::PackReport r2;
    CHECK(load_pack(pack::PackContext{pack::GameFamily::St, "atari-st-kixx"}, dir.string(), r2)->screens().size() == 3);
    // the same pack as a ZIP (the manifest at the root, then inside one folder) loads to the same screens and panel
    zip_directory(dir, g_scratch / "pack_ok.zip");
    pack::PackReport rz;
    auto zip_stack = load_pack(ctx, (g_scratch / "pack_ok.zip").string(), rz, &loaded);
    CHECK(loaded && rz.count(pack::Severity::Error) == 0 && zip_stack->screens().size() == 2 && zip_stack->panel() && zip_stack->panel()->cells.size() == 2);
    CHECK(zip_stack->screens()[0]->pic->px == lib.screens()[0]->pic->px && zip_stack->panel()->background->px == lib.panel()->background->px);

    // what goes wrong is reported and skipped, the rest still loads
    const fs::path bad = g_scratch / "pack_bad";
    fs::remove_all(bad);
    write_png_file(bad / "ok.png", solid(kW, kH, 0xFF000000u));
    write_text(bad / "notpng.png", "this is not a PNG");
    write_text(bad / "manifest.json",
               "{\"format\": \"x2-asset-pack\", \"version\": 2, \"screens\": {\n"
               " \"good\": {\"signature\": " + sig_json(f, 4) + ", \"png\": \"ok.png\"},\n"
               " \"nosig\": {\"png\": \"ok.png\"},\n"
               " \"badblock\": {\"signature\": {\"block\": 12, \"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"0000000000000000\"}]}, \"png\": \"ok.png\"},\n"
               " \"minbig\": {\"signature\": {\"block\": 16, \"min\": 3, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"0000000000000000\"}]}, \"png\": \"ok.png\"},\n"
               " \"badhash\": {\"signature\": {\"block\": 16, \"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"XYZ\"}]}, \"png\": \"ok.png\"},\n"
               " \"escape\": {\"signature\": " + sig_json(f, 4) + ", \"png\": \"../ok.png\"},\n"
               " \"missing\": {\"signature\": " + sig_json(f, 4) + ", \"png\": \"nope.png\"},\n"
               " \"garbage\": {\"signature\": " + sig_json(f, 4) + ", \"png\": \"notpng.png\"},\n"
               " \"badfade\": {\"signature\": " + sig_json(f, 4) + ", \"png\": \"ok.png\", \"fade\": \"zoom\"},\n"
               " \"badscale\": {\"signature\": " + sig_json(f, 4) + ", \"png\": \"ok.png\", \"scale\": 9},\n"
               " \"toomanyrects\": {\"signature\": " + sig_json(f, 4) + ", \"png\": \"ok.png\", \"dynamic\": [");
    {
        std::string m;
        std::ifstream in(bad / "manifest.json");
        m.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        for (int i = 0; i < 33; ++i) m += std::string(i ? ", " : "") + "{\"x\": 0, \"y\": 0, \"w\": 1, \"h\": 1}";
        m += "]},\n \"last\": {\"signature\": " + sig_json(f, 4) + ", \"png\": \"ok.png\"}},\n \"panel\": {\"cells\": {\"nonsense\": {\"png\": \"ok.png\"}, \"" +
             key1 + "\": {\"png\": \"missing.png\"}}}}";
        write_text(bad / "manifest.json", m);
    }
    pack::PackReport rb;
    bool bad_ok = false;
    auto lb_stack = load_pack(pack::PackContext{}, bad.string(), rb, &bad_ok);
    const pack::PackStack& lb = *lb_stack;
    CHECK(bad_ok);
    CHECK(lb.screens().size() == 2 && lb.screens()[0]->name == "good" && lb.screens()[1]->name == "last");
    CHECK(rb.count(pack::Severity::Error) >= 10);
    bool saw_missing = false, saw_escape = false, saw_key = false;
    for (const auto& msg : rb.messages) {
        const std::string text = msg.format();
        saw_missing = saw_missing || (text.find("missing") != std::string::npos && text.find("nope.png") != std::string::npos);
        saw_escape = saw_escape || (msg.key == "escape");
        saw_key = saw_key || (msg.section == "panel" && msg.key == "nonsense");
    }
    CHECK(saw_missing && saw_escape && saw_key);
    CHECK(!lb.panel());   // its only valid-looking cell failed too

    // a version 1 manifest has no screens; a different format is refused; an unreadable manifest is an error
    const fs::path v1 = g_scratch / "pack_v1";
    write_text(v1 / "manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 1, \"screens\": {}}");
    pack::PackReport r1;
    bool ok1 = false;
    auto l1 = load_pack({}, v1.string(), r1, &ok1);
    CHECK(ok1 && l1->screens().empty() && r1.count(pack::Severity::Error) == 1);
    write_text(v1 / "manifest.json", "{\"format\": \"something-else\", \"version\": 2}");
    pack::PackReport rr2;
    bool ok2 = true;
    load_pack({}, v1.string(), rr2, &ok2);
    CHECK(!ok2 && rr2.count(pack::Severity::Error) == 1);
    write_text(v1 / "manifest.json", "{\"format\": ");
    pack::PackReport rr3;
    bool ok3 = true;
    load_pack({}, v1.string(), rr3, &ok3);
    CHECK(!ok3);
    pack::PackReport rr4;
    bool ok4 = true;
    load_pack({}, (g_scratch / "no_such_pack").string(), rr4, &ok4);
    CHECK(!ok4);

    // a later screen of the same name replaces the earlier one
    ScreenPackLibrary rep;
    pack::ScreenEntry a = screen_of(f, "same", {{0, 0}});
    rep.add_screen(a);
    a.scale = 2;
    rep.add_screen(a);
    CHECK(rep.screens().size() == 1 && rep.screens()[0]->scale == 2);

    // the JSON reader of the loader: the shapes the manifests rely on
    pack::Json j;
    std::string why;
    CHECK(pack::parse_json("{\"a\": [1, 2.5, \"x\\u00e9\"], \"b\": {\"c\": true, \"c\": false}, \"n\": null}", j, why) && j.t == pack::Json::Obj && j.get("a")->a.size() == 3 &&
          j.get("a")->a[1].n == 2.5 && j.get("b")->get("c")->b == false);
    CHECK(!pack::parse_json("{\"a\": [1, }", j, why) && !why.empty());
}

// ---------------------------------------------------------------------------------------------------------------------------------

// A "game": screen A (a pattern with a blinking cursor block and a changing score), a cut, screen B, play, and screen A again.
struct Run {
    std::vector<Frame> frames;
    std::vector<uint8_t> play;
};

Run make_run() {
    Run r;
    auto add = [&](const Frame& f, bool play) {
        r.frames.push_back(f);
        r.play.push_back(play);
    };
    Frame a;
    a.pattern(1);
    Frame b;
    b.pattern(2);
    for (int i = 0; i < 40; ++i) {
        Frame f = a;
        if (i % 6 < 3) f.rect(160, 96, 16, 16, 9);          // blinking
        f.rect(272, 16, 16, 16, uint8_t(1 + i % 5));        // a changing number
        add(f, false);
    }
    for (int i = 0; i < 3; ++i) add(Frame(), false);          // a black frame or two between screens
    for (int i = 0; i < 30; ++i) add(b, false);
    for (int i = 0; i < 20; ++i) {                            // play: scrolls, nothing stays
        Frame f;
        f.pattern(10 + i);
        add(f, true);
    }
    // the same screen A again, with the score showing something it never showed before
    for (int i = 0; i < 30; ++i) {
        Frame f = a;
        f.rect(272, 16, 16, 16, 12);
        add(f, false);
    }
    // fade-out of B: the indices stay, the palette dims
    for (int s = 0; s < 6; ++s) {
        Frame f = b;
        std::vector<uint32_t> q = Frame::default_palette();
        for (auto& c : q) {
            auto sub = [&](uint32_t v) { return v > uint32_t(s * 0x22) ? v - uint32_t(s * 0x22) : 0u; };
            c = sub(c >> 16 & 255) << 16 | sub(c >> 8 & 255) << 8 | sub(c & 255);
        }
        f.palette(q);
        for (int k = 0; k < 3; ++k) add(f, false);
    }
    return r;
}

void test_dump() {
    const Run run = make_run();
    ScreenDumper d(kPanel);
    for (size_t i = 0; i < run.frames.size(); ++i) d.frame(run.frames[i].view(), uint32_t(i), false, run.play[i] != 0);
    const fs::path dir = g_scratch / "dump";
    fs::remove_all(dir);
    std::string members, why;
    CHECK(d.finish(dir.string(), members, why));
    CHECK(d.screens() == 2);                           // A (once, its two showings are one screen) and B; play and the black frames are not
    CHECK(d.panel_cells() == 0);
    CHECK(members.find("\"screens\"") != std::string::npos && members.find("\"panel\"") == std::string::npos);
    write_text(dir / "manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": \"dump\", \"scale\": 1,\n" + members + "}\n");

    pack::PackReport report;
    bool dump_ok = false;
    auto lib_stack = load_pack({}, dir.string(), report, &dump_ok);
    const pack::PackStack& lib = *lib_stack;
    CHECK(dump_ok && report.count(pack::Severity::Error) == 0);
    CHECK(lib.screens().size() == 2);
    const pack::ScreenEntry* sa = nullptr;
    for (const auto* s : lib.screens())
        if (s->dynamic.size() > 1) sa = s;
    CHECK(sa != nullptr);   // screen A has the cursor and the score as changing parts
    // the dump as a pack gives every frame of the run back, exactly, at any canvas scale: loaded from the directory and from a ZIP of it
    zip_directory(dir, g_scratch / "dump.zip");
    pack::PackReport report_zip;
    bool zip_ok = false;
    auto zip_stack = load_pack({}, (g_scratch / "dump.zip").string(), report_zip, &zip_ok);
    CHECK(zip_ok && report_zip.count(pack::Severity::Error) == 0 && zip_stack->screens().size() == 2);
    for (const pack::PackStack* source : std::initializer_list<const pack::PackStack*>{&lib, zip_stack.get()}) {
        PackScreens ps;
        ps.set_library(source, kPanel);
        for (int S : {1, 2, 4}) {
            int drawn = 0, wrong = 0;
            for (size_t i = 0; i < run.frames.size(); ++i) {
                bool drew = false;
                const auto out = render(ps, run.frames[i], uint64_t(S) * 1000 + i, false, S, &drew);
                drawn += drew;
                wrong += out != faithful(run.frames[i], S);
            }
            CHECK(wrong == 0);
            CHECK(drawn >= 100);   // A twice and B, including the fade
        }
    }
    PackScreens ps;
    ps.set_library(&lib, kPanel);
    // play is not a screen
    bool drew = true;
    render(ps, run.frames[75], 9999, false, 1, &drew);
    CHECK(run.play[75] && !drew);
    // the signature is exact: a frame that differs in a stable block is not the screen
    Frame changed = run.frames[0];
    changed.rect(32, 32, 16, 16, 1);
    changed.rect(64, 128, 16, 16, 1);
    changed.rect(200, 150, 16, 16, 1);
    changed.rect(288, 176, 16, 16, 1);
    render(ps, changed, 10000, false, 1, &drew);
    CHECK(!drew);

    // a screen whose signature the other one satisfies is told apart: the same top, one with a blank bottom, one with a picture
    Frame x, y, lower;
    x.pattern(3);
    lower.pattern(8);
    y = x;
    for (int row = 96; row < kH; ++row)
        for (int col = 0; col < kW; ++col) {
            x.idx[size_t(row) * kW + size_t(col)] = 0;
            y.idx[size_t(row) * kW + size_t(col)] = lower.idx[size_t(row) * kW + size_t(col)];
        }
    x.render();
    y.render();
    ScreenDumper d2(kPanel);
    for (int i = 0; i < 20; ++i) d2.frame(x.view(), uint32_t(i), false, false);
    for (int i = 0; i < 20; ++i) d2.frame(y.view(), uint32_t(20 + i), false, false);
    fs::remove_all(g_scratch / "dump2");
    CHECK(d2.finish((g_scratch / "dump2").string(), members, why));
    CHECK(d2.screens() == 2);
    write_text(g_scratch / "dump2/manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"scale\": 1,\n" + members + "}\n");
    pack::PackReport r2;
    auto lib2_stack = load_pack({}, (g_scratch / "dump2").string(), r2);
    const pack::PackStack& lib2 = *lib2_stack;
    CHECK(lib2.screens().size() == 2);
    PackScreens p2;
    p2.set_library(&lib2, kPanel);
    CHECK(render(p2, x, 1, false, 1, &drew) == faithful(x, 1) && drew);
    CHECK(render(p2, y, 2, false, 1, &drew) == faithful(y, 1) && drew);
    CHECK(render(p2, x, 3, false, 1, &drew) == faithful(x, 1) && drew);
    bool grown = false;
    for (const auto* s : lib2.screens()) grown = grown || s->signature.blocks.size() > ScreenDumper::kMaxSignature;   // the one with the plain bottom got blocks added
    CHECK(grown);

    // play that looks like a screen for a moment is not taken for one
    Frame stay;
    stay.pattern(5);
    ScreenDumper d3(kPanel);
    for (int i = 0; i < 30; ++i) d3.frame(stay.view(), uint32_t(i), false, true);   // 30 frames of "play" the same
    CHECK(d3.screens() == 0);
}

void test_dump_panel() {
    // the strip during play: glyph cells keyed by content; a fade-in first, the full palette most of the time
    ScreenDumper d(kPanel);
    std::vector<Frame> frames;
    for (int i = 0; i < 40; ++i) {
        Frame f;
        f.rect(0, 0, kW, 192, 0);
        for (int c = 0; c < 40; ++c)
            for (int yy = 0; yy < 8; ++yy)
                for (int xx = 0; xx < 8; ++xx) f.idx[size_t(192 + yy) * kW + size_t(c * 8 + xx)] = uint8_t(c % 6 == 0 ? 0 : 1 + (c % 4) * ((xx + yy + (c < 8 ? i / 8 : 0)) % 2));
        if (i < 6) {
            std::vector<uint32_t> q = Frame::default_palette();
            for (auto& col : q) {
                auto sub = [&](uint32_t v) { return v > uint32_t((6 - i) * 0x22) ? v - uint32_t((6 - i) * 0x22) : 0u; };
                col = sub(col >> 16 & 255) << 16 | sub(col >> 8 & 255) << 8 | sub(col & 255);
            }
            f.palette(q);
        } else {
            f.render();
        }
        frames.push_back(f);
        d.frame(f.view(), uint32_t(i), true, true);
    }
    CHECK(d.screens() == 0 && d.panel_cells() > 4 && d.strip_palettes() == 7);
    const fs::path dir = g_scratch / "dump_panel";
    fs::remove_all(dir);
    std::string members, why;
    CHECK(d.finish(dir.string(), members, why));
    write_text(dir / "manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"scale\": 1,\n" + members + "}\n");
    pack::PackReport report;
    bool panel_ok = false;
    auto lib_stack = load_pack({}, dir.string(), report, &panel_ok);
    const pack::PackStack& lib = *lib_stack;
    CHECK(panel_ok && report.count(pack::Severity::Error) == 0);
    CHECK(lib.panel() && lib.panel()->cells.size() == d.panel_cells() && lib.panel()->ref_palette.size() == kColours);
    zip_directory(dir, g_scratch / "dump_panel.zip");
    pack::PackReport report_zip;
    bool zip_ok = false;
    auto zip_stack = load_pack({}, (g_scratch / "dump_panel.zip").string(), report_zip, &zip_ok);
    CHECK(zip_ok && report_zip.count(pack::Severity::Error) == 0 && zip_stack->panel() && zip_stack->panel()->cells.size() == d.panel_cells());
    for (const pack::PackStack* source : std::initializer_list<const pack::PackStack*>{&lib, zip_stack.get()}) {
        PackScreens ps;
        ps.set_library(source, kPanel);
        int wrong = 0;
        for (size_t i = 0; i < frames.size(); ++i) {
            bool drew = false;
            const auto out = render(ps, frames[i], i + 1, true, 2, &drew);
            wrong += !drew || out != faithful(frames[i], 2);
        }
        CHECK(wrong == 0);   // including the six frames of the fade-in
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------

// The Atari ST present hook draws from the shifter's index plane: the indices, and the palette each line starts with, including
// a palette write in the middle of the frame.
void test_st_index_plane() {
    std::vector<uint8_t> ram(0x10000, 0);
    // 16 pixels at line 0: plane words so that pixel x has index x
    uint8_t* row = ram.data() + 0x8000;
    const uint16_t w0 = 0x5555, w1 = 0x3333, w2 = 0x0F0F, w3 = 0x00FF;   // pixel x of each group of 16 has index x
    for (int g = 0; g < 20; ++g) {
        const uint16_t w[4] = {w0, w1, w2, w3};
        for (int p = 0; p < 4; ++p) {
            row[g * 8 + p * 2] = uint8_t(w[p] >> 8);
            row[g * 8 + p * 2 + 1] = uint8_t(w[p]);
        }
    }
    st::Shifter sh;
    sh.write16(0x40, 0x0000);
    sh.write16(0x42, 0x0700);
    sh.write16(0x44, 0x0070);
    sh.begin_frame();
    std::vector<uint8_t> rgba, plane(kW, 0xEE);
    std::vector<uint32_t> pal(16, 0xDEADBEEF);
    sh.render_line(ram.data(), uint32_t(ram.size()), 0, 0x8000, 0, 0, rgba, plane.data(), pal.data());
    int bad = 0;
    for (int x = 0; x < 16; ++x) bad += plane[size_t(x)] != x;
    CHECK(bad == 0);
    CHECK(pal[0] == 0x000000 && pal[1] == 0xFF0000 && pal[2] == 0x00FF00);   // 0x0RGB with three bits a gun, as 0x00RRGGBB
    // the same line without the planes gives the same picture
    std::vector<uint8_t> rgba2;
    st::Shifter sh2;
    sh2.write16(0x40, 0x0000);
    sh2.write16(0x42, 0x0700);
    sh2.write16(0x44, 0x0070);
    sh2.begin_frame();
    sh2.render_line(ram.data(), uint32_t(ram.size()), 0, 0x8000, 0, 0, rgba2);
    CHECK(rgba == rgba2);
    // a blank line outside RAM gives index 0
    std::fill(plane.begin(), plane.end(), uint8_t(0xEE));
    sh.render_line(ram.data(), uint32_t(ram.size()), 1, 0x20000, 512, 0, rgba, plane.data(), pal.data());
    CHECK(std::all_of(plane.begin(), plane.end(), [](uint8_t v) { return v == 0; }));
    // a frame on the shifter's plane feeds the stage exactly as the ST runner does it (no surround, nothing above the window)
    Frame f;
    f.pattern(6);
    ScreenPackLibrary lib;
    pack::ScreenEntry e = screen_of(f, "st", {{0, 0}, {4, 4}, {8, 8}});
    e.pic = lib.add_picture(solid(kW, kH, 0xFFABCDEFu));
    lib.add_screen(e);
    PackScreens ps;
    ps.set_library(&lib, PanelLayout{});
    CHECK(ps.active());
    std::vector<uint32_t> out;
    CHECK(ps.render_window(f.view(), 1, false, 3, out) && out[0] == 0xFFABCDEFu);
}

} // namespace

int main(int argc, char** argv) {
    g_scratch = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_screens_tests";
    std::error_code ec;
    fs::create_directories(g_scratch, ec);
    struct T {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"matcher", test_matcher},
        {"fade", test_fade},
        {"compose", test_compose},
        {"panel", test_panel},
        {"loader", test_loader},
        {"dump", test_dump},
        {"dump_panel", test_dump_panel},
        {"st_index_plane", test_st_index_plane},
    };
    for (auto& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-16s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
