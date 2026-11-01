// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Unit tests of the launcher UI kit (runtime/ui): canvas, font, theme, geometry, input repeat, the focus and
// navigation model, every widget's state machine driven by synthetic events, modals, scrolling, key capture,
// page selector, reachability of every control on every demo page, and determinism. No SDL, no window, no game data.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "demo/demo_scenes.h"

using namespace x2::ui;
using namespace x2::ui::demo;

namespace {

int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                           \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(c)) {                                                            \
      ++g_fail;                                                            \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
    }                                                                      \
  } while (0)
#define CHECK_EQ(a, b)                                                                                                   \
  do {                                                                                                                   \
    ++g_checks;                                                                                                          \
    const auto va_ = (a);                                                                                                \
    const auto vb_ = (b);                                                                                                \
    if (!(va_ == vb_)) {                                                                                                 \
      ++g_fail;                                                                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)va_, (long long)vb_); \
    }                                                                                                                    \
  } while (0)

Event key(Nav n) { return Event::nav_key(n); }
Event move(int x, int y) { return Event::pointer(Event::Type::PointerMove, x, y); }
Event down(int x, int y) { return Event::pointer(Event::Type::PointerDown, x, y); }
Event up(int x, int y) { return Event::pointer(Event::Type::PointerUp, x, y); }

// A small context + canvas + builder rig.
struct Rig {
  Context ctx;
  Canvas canvas{320, 240};
  Context::Builder builder;
  Rig() { ctx.motion.reduced = true; }
  bool run(const EventList& e = {}, int dt = 0) { return ctx.run(canvas, e, dt, builder); }
  void send(Event e) { run({e}); }
  void send(Nav n) { run({key(n)}); }
  Id id(const char* label) const { return ctx.id(label); }
};

// ---- types, canvas ----
void test_types() {
  CHECK_EQ(mix(rgb(0, 0, 0), rgb(255, 255, 255), 128), rgb(128, 128, 128));
  CHECK_EQ(mix(rgb(10, 20, 30), rgb(200, 100, 50), 0), rgb(10, 20, 30));
  CHECK_EQ(mix(rgb(10, 20, 30), rgb(200, 100, 50), 255), rgb(200, 100, 50));
  CHECK_EQ(rgb_hex(0xFFB03B), rgb(0xFF, 0xB0, 0x3B));
  Rect r{10, 20, 100, 50};
  Rect a = cut_left(r, 30);
  CHECK(a == (Rect{10, 20, 30, 50}) && r == (Rect{40, 20, 70, 50}));
  Rect b = cut_right(r, 20);
  CHECK(b == (Rect{90, 20, 20, 50}) && r == (Rect{40, 20, 50, 50}));
  Rect t = cut_top(r, 10);
  CHECK(t == (Rect{40, 20, 50, 10}) && r == (Rect{40, 30, 50, 40}));
  Rect bt = cut_bottom(r, 100);  // more than is left: takes what there is
  CHECK(bt.h == 40 && r.h == 0);
  CHECK(intersect({0, 0, 10, 10}, {5, 5, 10, 10}) == (Rect{5, 5, 5, 5}));
  CHECK(intersect({0, 0, 10, 10}, {20, 20, 5, 5}).empty());
  CHECK(hash_id(0, "a") != 0 && hash_id(0, "a") != hash_id(0, "b") && hash_id(1, "a") != hash_id(0, "a"));
  Stack s(0, 100, 50, 4);
  s.row(20);
  s.row(20);
  CHECK_EQ(s.used(), 44);
}

int count_color(const Canvas& c, Color col) {
  int n = 0;
  for (int y = 0; y < c.height(); ++y)
    for (int x = 0; x < c.width(); ++x) n += c.get(x, y) == col;
  return n;
}

void test_canvas() {
  const Color W = rgb(255, 255, 255), K = rgb(0, 0, 0);
  Canvas c(8, 8);
  c.clear(K);
  c.fill({-2, -2, 5, 5}, W);  // clipped to the canvas
  CHECK_EQ(count_color(c, W), 9);
  c.clear(K);
  c.push_clip({2, 2, 4, 4});
  c.push_clip({4, 4, 10, 10});
  c.fill({0, 0, 8, 8}, W);
  CHECK_EQ(count_color(c, W), 4);  // the intersection of both clips: 2x2
  c.pop_clip();
  c.fill({0, 0, 8, 8}, rgb(9, 9, 9));
  CHECK_EQ(count_color(c, rgb(9, 9, 9)), 16);  // the whole 4x4 clip, over the white 2x2
  c.pop_clip();
  c.clear(K);
  c.blend({0, 0, 2, 2}, W, 128);
  CHECK_EQ(c.get(0, 0), rgb(128, 128, 128));
  c.blend({4, 4, 2, 2}, W, 0);
  CHECK_EQ(c.get(4, 4), K);
  c.blend({4, 4, 2, 2}, W, 255);
  CHECK_EQ(c.get(4, 4), W);
  // chamfer: the corner pixel is cut, the 45 degree edge is connected
  Canvas p(12, 12);
  p.clear(K);
  p.fill_chamfer({0, 0, 10, 10}, 3, W);
  CHECK_EQ(p.get(0, 0), K);
  CHECK_EQ(p.get(2, 0), K);
  CHECK_EQ(p.get(3, 0), W);
  CHECK_EQ(p.get(0, 2), K);
  CHECK_EQ(p.get(1, 2), W);
  CHECK_EQ(p.get(0, 3), W);
  CHECK_EQ(p.get(9, 9), K);
  p.clear(K);
  p.frame_chamfer({0, 0, 10, 10}, 3, W);
  CHECK_EQ(p.get(3, 0), W);
  CHECK_EQ(p.get(2, 1), W);
  CHECK_EQ(p.get(1, 2), W);
  CHECK_EQ(p.get(0, 3), W);
  CHECK_EQ(p.get(5, 5), K);  // hollow
  CHECK_EQ(p.get(0, 5), W);
  // dither alternates
  p.clear(K);
  p.dither({0, 0, 4, 1}, W, K, 0);
  CHECK(p.get(0, 0) == W && p.get(1, 0) == K && p.get(2, 0) == W);
  // discs and rings
  p.clear(K);
  p.disc(6, 6, 3, W);
  CHECK_EQ(p.get(6, 6), W);
  CHECK(count_color(p, W) > 20 && count_color(p, W) < 50);
  for (int d = 0; d <= 3; ++d) CHECK_EQ(p.get(6 + d, 6), p.get(6 - d, 6));
  Canvas q(16, 16);
  q.clear(K);
  q.ring(8, 8, 6, 2, W);
  CHECK_EQ(q.get(8, 8), K);
  CHECK_EQ(q.get(14, 8), W);
  // hash: stable, sensitive, and a pinned value so a platform difference shows up in CI
  Canvas h(16, 16);
  h.clear(rgb(11, 15, 26));
  h.fill_chamfer({1, 1, 14, 14}, 3, rgb(20, 27, 45));
  h.frame_chamfer({1, 1, 14, 14}, 3, rgb(52, 65, 106));
  h.disc(8, 8, 3, rgb(255, 176, 59));
  draw_text(h, Font::caption(), 3, 4, "Ab", rgb(232, 236, 248));
  const uint64_t h1 = h.hash();
  Canvas h2 = h;
  CHECK_EQ(h1, h2.hash());
  h2.put(0, 0, W);
  CHECK(h2.hash() != h1);
  std::printf("pinned canvas hash %016llx\n", static_cast<unsigned long long>(h1));
  CHECK_EQ(h1, 0x22d9c99d61850b3full);
}

// ---- font ----
void test_font() {
  for (const Font* f : {&Font::body(), &Font::caption()}) {
    for (uint32_t cp = 0x20; cp < 0x7F; ++cp) CHECK(f->has(cp));
    for (uint32_t cp = 0xA0; cp < 0x100; ++cp) CHECK(f->has(cp));
    for (uint32_t cp = 0x100; cp < 0x180; ++cp) CHECK(f->has(cp));
    for (uint32_t cp : {0x2026u, 0x2022u, 0x2713u, 0x2190u, 0x2193u, 0x2500u, 0x2588u, 0x25B6u, 0x25C0u}) CHECK(f->has(cp));
    CHECK(!f->has(0x4E2D));
    CHECK(!f->has(0x1F600));
    const uint8_t* a = f->glyph('A');
    bool any = false;
    for (int i = 0; i < f->cell_h(); ++i) any = any || a[i];
    CHECK(any);
    const uint8_t* sp = f->glyph(' ');
    for (int i = 0; i < f->cell_h(); ++i) CHECK_EQ(sp[i], 0);
  }
  std::string_view s = "\xC3\xA9x";
  CHECK_EQ(next_codepoint(s), 0xE9u);
  CHECK_EQ(s.size(), 1u);
  s = "\xC3";
  CHECK_EQ(next_codepoint(s), 0xFFFDu);
  CHECK(s.empty());
  s = "\xC0\x80";  // overlong
  CHECK_EQ(next_codepoint(s), 0xFFFDu);
  CHECK_EQ(s.size(), 1u);
  s = "\xF0\x9F\x98\x80";
  CHECK_EQ(next_codepoint(s), 0x1F600u);
  CHECK(s.empty());
  s = "\xED\xA0\x80";  // a surrogate
  CHECK_EQ(next_codepoint(s), 0xFFFDu);
  s = "\xE2\x82";  // truncated three-byte sequence
  CHECK_EQ(next_codepoint(s), 0xFFFDu);
  CHECK_EQ(codepoint_count("h\xC3\xA9llo"), 5);
  CHECK_EQ(text_width("h\xC3\xA9llo"), 40);
  CHECK_EQ(text_width("ab", 2, 1), 2 * 17);
  const std::vector<std::string> w = wrap_text("the quick brown fox", 80);
  CHECK_EQ(w.size(), 2u);
  CHECK(w[0] == "the quick" && w[1] == "brown fox");
  const std::vector<std::string> long_word = wrap_text("abcdefghijklmnop", 64);  // 8 characters per line
  CHECK_EQ(long_word.size(), 2u);
  CHECK(long_word[0] == "abcdefgh" && long_word[1] == "ijklmnop");
  CHECK_EQ(wrap_text("a\nb", 80).size(), 2u);
  CHECK_EQ(wrap_text("", 80).size(), 1u);
  const std::string e = elide_text("hello world", 56);
  CHECK_EQ(codepoint_count(e), 7);
  CHECK(e.size() >= 3 && e.substr(e.size() - 3) == "\xE2\x80\xA6");
  CHECK(elide_text("abcdefghij", 40, 1, true) == std::string("ab\xE2\x80\xA6ij"));
  CHECK(elide_text("short", 100) == "short");
  CHECK(elide_text("anything", 0).empty());
  // a missing glyph is drawn as a visible box, and clipping keeps drawing inside
  Canvas c(20, 20);
  c.clear(rgb(0, 0, 0));
  draw_text(c, Font::body(), 0, 0, "\xE4\xB8\xAD", rgb(255, 255, 255));
  CHECK_EQ(c.get(1, 1), rgb(255, 255, 255));
  CHECK_EQ(c.get(0, 0), rgb(0, 0, 0));
  c.clear(rgb(0, 0, 0));
  c.push_clip({0, 0, 4, 4});
  draw_text(c, Font::body(), 0, 0, "WWWW", rgb(255, 255, 255));
  c.pop_clip();
  CHECK_EQ(c.get(10, 2), rgb(0, 0, 0));
}

// ---- theme, geometry, input ----
void test_theme_geometry_input() {
  CHECK(contrast_ratio(rgb(255, 255, 255), rgb(0, 0, 0)) > 20.99);
  const Theme t = Theme::standard();
  const double tg = contrast_ratio(t.c.text, t.c.ground);
  CHECK(tg > 15.9 && tg < 16.6);
  for (const Theme& th : {Theme::standard(), Theme::high_contrast_theme()}) {
    for (const ContrastPair& p : contrast_pairs(th)) {
      const double r = contrast_ratio(p.fg, p.bg);
      if (r < p.minimum) std::fprintf(stderr, "contrast %s%s: %.2f < %.1f\n", th.high_contrast ? "(hc) " : "", p.name.c_str(), r, p.minimum);
      CHECK(r >= p.minimum);
    }
  }
  CHECK(ease_permille(0) == 0 && ease_permille(1000) == 1000 && ease_permille(500) == 500);
  CHECK(ease_permille(250) < 250 && ease_permille(750) > 750);

  CanvasGeometry g = compute_geometry(640, 360, false);
  CHECK(g.n == 1 && g.cw == 640 && g.ch == 360);
  g = compute_geometry(1280, 720, false);
  CHECK(g.n == 2 && g.cw == 640 && g.ch == 360);
  g = compute_geometry(1920, 1080, false);
  CHECK(g.n == 4 && g.cw == 480 && g.ch == 270);
  g = compute_geometry(1280, 720, true);
  CHECK(g.n == 4 && g.cw == 320 && g.ch == 180);
  g = compute_geometry(800, 600, false);
  CHECK(g.n == 1 && g.cw == 800 && g.ch == 600);
  g = compute_geometry(1001, 601, false);
  CHECK(g.n == 2 && g.cw == 500 && g.ch == 300 && g.ox == 0 && g.oy == 0);
  g = compute_geometry(1923, 1083, false);
  CHECK(g.n == 4 && g.cw == 480 && g.ch == 270 && g.ox == 1 && g.oy == 1);
  g = compute_geometry(300, 200, false);  // smaller than the minimum: n stays 1
  CHECK(g.n == 1 && g.cw == 300);

  DirRepeater rep;
  EventList out;
  int events = 0;
  for (int t_ms = 0; t_ms <= 1000; t_ms += 10) {
    out.clear();
    rep.update(Nav::Down, t_ms == 0 ? 0 : 10, out);
    events += static_cast<int>(out.size());
    if (t_ms == 0) CHECK_EQ(out.size(), 1u);
    if (t_ms > 0 && t_ms < 400) CHECK(out.empty());
  }
  CHECK_EQ(events, 9);
  out.clear();
  rep.update(Nav::None, 10, out);
  rep.update(Nav::Left, 0, out);  // a new direction fires at once
  CHECK_EQ(out.size(), 1u);
  CHECK(out[0].nav == Nav::Left && out[0].device == Device::Pad);
  CHECK(nav_caption(Nav::Accept, Device::Pad, PadStyle::PlayStation) == "Cross");
  CHECK(nav_caption(Nav::Accept, Device::Pad, PadStyle::Nintendo) == "B");
  CHECK(nav_caption(Nav::Accept, Device::Pad, PadStyle::Xbox) == "A");
  CHECK(nav_caption(Nav::Accept, Device::Keyboard, PadStyle::Xbox) == "Enter");
  CHECK(nav_caption(Nav::Menu, Device::Pad, PadStyle::PlayStation) == "Options");
}

// ---- navigation and widgets on a small page ----
struct Mini {
  int one = 0, two = 0;
  bool tog = false, tog_off = false;
  int step = 5, sl = 50, ch = 0, ls = 0;
  bool show_one = true;
  bool modal = false;
  int modal_ok = 0;
  std::vector<std::string> rows = {"alpha", "beta", "gamma"};
  Rect slider_rect;
  void build(Context& c) {
    static constexpr std::string_view kItems[] = {"a", "b", "c"};
    Stack s(10, 10, 300, 4);
    if (show_one && button(c, "One", s.row(20), {.default_focus = true})) ++one;
    if (button(c, "Two", s.row(20))) ++two;
    toggle(c, "tog", s.row(20), tog);
    toggle(c, "tog_off", s.row(20), tog_off, false);
    stepper(c, "step", s.row(20), step, 0, 10, 1, "x");
    slider_rect = s.row(20);
    slider(c, "sl", slider_rect, sl, 0, 100, 10);
    choice(c, "ch", s.row(20), ch, kItems);
    list(c, "ls", s.row(66), rows, ls, {});
    if (button(c, "Open", s.row(20))) c.open_modal("m");
    Rect inner;
    if (c.modal_begin("m", "Title", 200, 100, &inner)) {
      if (button(c, "Cancel", {inner.x, inner.y, 80, 20}, {.default_focus = true})) c.close_modal();
      if (button(c, "Ok", {inner.x + 90, inner.y, 60, 20})) ++modal_ok;
      c.modal_end();
    }
  }
};

void test_navigation_and_widgets() {
  Mini m;
  Rig r;
  r.builder = [&](Context& c) { m.build(c); };
  r.run();
  CHECK_EQ(r.ctx.focused(), r.id("One"));  // a default focus exists from the first frame
  r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("Two"));
  r.send(Nav::Up);
  CHECK_EQ(r.ctx.focused(), r.id("One"));
  r.send(Nav::Up);  // nothing above: stays
  CHECK_EQ(r.ctx.focused(), r.id("One"));
  r.send(Nav::Accept);
  CHECK_EQ(m.one, 1);  // exactly once, although the frame is rebuilt to settle
  r.run();
  r.run({}, 100);
  CHECK_EQ(m.one, 1);
  r.send(Nav::Down);
  r.send(Nav::Accept);
  CHECK_EQ(m.two, 1);

  // toggle: Accept flips, Left/Right set, and the horizontal keys never move the focus off a value widget
  r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("tog"));
  r.send(Nav::Accept);
  CHECK(m.tog);
  r.send(Nav::Left);
  CHECK(!m.tog);
  r.send(Nav::Left);
  CHECK(!m.tog && r.ctx.focused() == r.id("tog"));
  r.send(Nav::Right);
  CHECK(m.tog);
  // a disabled toggle is reachable (its reason is shown) but inert
  r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("tog_off"));
  r.send(Nav::Accept);
  r.send(Nav::Right);
  CHECK(!m.tog_off);
  // stepper: Right steps and clamps; focus stays
  r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("step"));
  for (int i = 0; i < 8; ++i) r.send(Nav::Right);
  CHECK_EQ(m.step, 10);
  r.send(Nav::Left);
  CHECK_EQ(m.step, 9);
  CHECK_EQ(r.ctx.focused(), r.id("step"));
  // slider by keys and by pointer drag
  r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("sl"));
  r.send(Nav::Right);
  CHECK_EQ(m.sl, 60);
  const Rect sr = m.slider_rect;
  const int track_x = sr.x + 2, track_w = sr.w - 4;
  r.run({down(track_x + (track_w - 1) / 2, sr.cy())});
  CHECK_EQ(m.sl, 50);
  r.run({move(track_x + track_w + 100, sr.cy())});  // dragging beyond the end clamps
  CHECK_EQ(m.sl, 100);
  r.run({move(track_x - 50, sr.cy())});
  CHECK_EQ(m.sl, 0);
  r.run({up(track_x, sr.cy())});
  r.run({move(track_x + track_w / 2, sr.cy())});  // after release a move no longer drags
  CHECK_EQ(m.sl, 0);
  // choice wraps both ways
  r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("ch"));
  r.send(Nav::Left);
  CHECK_EQ(m.ch, 2);
  r.send(Nav::Right);
  CHECK_EQ(m.ch, 0);
  r.send(Nav::Accept);
  CHECK_EQ(m.ch, 1);
  // list: Up/Down move the selection inside; the edges let the focus out
  r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("ls"));
  r.send(Nav::Down);
  r.send(Nav::Down);
  CHECK_EQ(m.ls, 2);
  CHECK_EQ(r.ctx.focused(), r.id("ls"));
  r.send(Nav::Down);  // last row: leaves the list
  CHECK_EQ(r.ctx.focused(), r.id("Open"));
  r.send(Nav::Up);
  CHECK_EQ(r.ctx.focused(), r.id("ls"));
  r.send(Nav::Up);
  r.send(Nav::Up);
  CHECK_EQ(m.ls, 0);
  r.send(Nav::Up);  // first row: leaves upward
  CHECK_EQ(r.ctx.focused(), r.id("ch"));

  // pointer: hover focuses, a click needs press and release on the same widget
  const Rect two{10, 10 + 24, 300, 20};
  r.run({move(two.cx(), two.cy())});
  CHECK_EQ(r.ctx.focused(), r.id("Two"));
  r.run({down(two.cx(), two.cy())});
  r.run({up(two.cx(), two.cy())});
  CHECK_EQ(m.two, 2);
  const Rect one{10, 10, 300, 20};
  r.run({down(one.cx(), one.cy()), move(two.cx(), two.cy()), up(two.cx(), two.cy())});
  CHECK_EQ(m.one, 1);
  CHECK_EQ(m.two, 2);

  // a focused widget that disappears: a new focus is chosen, there is never none
  r.run({move(one.cx(), one.cy())});
  CHECK_EQ(r.ctx.focused(), r.id("One"));
  m.show_one = false;
  r.run();
  CHECK(r.ctx.focused() != 0);
  CHECK_EQ(r.ctx.focused(), r.id("Two"));
  m.show_one = true;

  // modal: focus is trapped, Back closes, the page is inert behind it
  r.ctx.set_focus(r.id("Open"));
  r.run();
  r.send(Nav::Accept);
  CHECK(r.ctx.modal_open());
  CHECK_EQ(r.ctx.focused(), r.id("Cancel"));
  r.send(Nav::Up);
  r.send(Nav::Down);
  CHECK(r.ctx.focused() == r.id("Cancel") || r.ctx.focused() == r.id("Ok"));
  r.send(Nav::Right);
  CHECK_EQ(r.ctx.focused(), r.id("Ok"));
  r.send(Nav::Accept);
  CHECK_EQ(m.modal_ok, 1);
  const int before = m.two;
  r.run({move(two.cx(), two.cy())});  // pointer over the page behind the modal
  r.run({down(two.cx(), two.cy())});
  r.run({up(two.cx(), two.cy())});
  CHECK_EQ(m.two, before);
  r.send(Nav::Back);
  CHECK(!r.ctx.modal_open());
  CHECK(r.ctx.focused() != 0);
  r.ctx.set_focus(r.id("Open"));
  r.run();
  r.send(Nav::Accept);
  r.send(Nav::Right);  // to Ok
  r.send(Nav::Left);   // back to Cancel
  r.send(Nav::Accept);
  CHECK(!r.ctx.modal_open());
}

// ---- scrolling ----
void test_scroll() {
  int pressed = -1;
  Rig r;
  const Rect view{0, 0, 200, 100};
  r.builder = [&](Context& c) {
    ScrollView sv = c.scroll_begin("tall", view);
    Stack s(4, sv.origin_y() + 4, 180, 2);
    for (int i = 0; i < 30; ++i)
      if (button(c, "row" + std::to_string(i), s.row(20), {.default_focus = i == 0})) pressed = i;
    c.scroll_end(sv, s.used() + 8);
  };
  r.run();
  CHECK_EQ(r.ctx.focused(), r.id("row0"));
  r.send(Nav::Last);
  CHECK_EQ(r.ctx.focused(), r.id("row29"));
  // the last row is inside the view after the focus moved
  Rect last{};
  for (const FocusItem& it : r.ctx.items())
    if (it.id == r.id("row29")) last = it.rect;
  CHECK(last.y >= view.y && last.bottom() <= view.bottom());
  r.send(Nav::First);
  CHECK_EQ(r.ctx.focused(), r.id("row0"));
  Rect first{};
  for (const FocusItem& it : r.ctx.items())
    if (it.id == r.id("row0")) first = it.rect;
  CHECK(first.y >= view.y);
  for (int i = 0; i < 9; ++i) r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("row9"));
  for (const FocusItem& it : r.ctx.items())
    if (it.id == r.id("row9")) last = it.rect;
  CHECK(last.y >= view.y && last.bottom() <= view.bottom());
  // the wheel scrolls and clamps; focus is not dragged along
  const Id f = r.ctx.focused();
  r.run({Event::scroll(3, 50, 50)});
  r.run({Event::scroll(1000, 50, 50)});
  r.run({Event::scroll(-1000, 50, 50)});
  CHECK_EQ(r.ctx.focused(), f);
  Rect r0{};
  for (const FocusItem& it : r.ctx.items())
    if (it.id == r.id("row0")) r0 = it.rect;
  CHECK_EQ(r0.y, 4);  // back at the top
  // clicking a partly visible row works; a click outside the view does not reach hidden rows
  r.run({Event::scroll(100, 50, 50)});
  r.run({down(50, 95)});
  r.run({up(50, 95)});
  CHECK(pressed >= 0);
  const int p = pressed;
  pressed = -1;
  r.run({down(50, 150)});  // below the view: nothing
  r.run({up(50, 150)});
  CHECK_EQ(pressed, -1);
  (void)p;
}

// ---- message list: follow the bottom unless the user scrolled away ----
void test_message_list() {
  std::vector<Message> msgs;
  Rig r;
  const Rect box{0, 0, 200, 60};
  r.builder = [&](Context& c) { message_list(c, "log", box, msgs); };
  for (int i = 0; i < 20; ++i) msgs.push_back({Severity::Info, "line " + std::to_string(i)});
  r.run();
  r.run();
  const WState& ss = r.ctx.state(r.ctx.id("log/scroll"));
  const int max_scroll = std::max(0, ss.content_h - (box.h - 6));
  CHECK(max_scroll > 0);
  CHECK_EQ(ss.scroll, max_scroll);  // following
  r.send(Nav::Up);                  // the list is the only focusable: Up scrolls it
  CHECK(r.ctx.state(r.ctx.id("log/scroll")).scroll < max_scroll);
  const int mid = r.ctx.state(r.ctx.id("log/scroll")).scroll;
  msgs.push_back({Severity::Warn, "new"});
  r.run();
  r.run();
  CHECK(r.ctx.state(r.ctx.id("log/scroll")).scroll == mid);  // not dragged to the bottom
  for (int i = 0; i < 20; ++i) r.send(Nav::Down);
  msgs.push_back({Severity::Error, "newer"});
  r.run();
  r.run();
  const WState& s2 = r.ctx.state(r.ctx.id("log/scroll"));
  CHECK_EQ(s2.scroll, std::max(0, s2.content_h - (box.h - 6)));  // following again
}

// ---- key capture ----
void test_key_capture() {
  int key = 0;
  CaptureResult last = CaptureResult::None;
  Rig r;
  r.builder = [&](Context& c) {
    const CaptureResult res = key_capture(c, "bind", {10, 10, 120, 20}, "Up", key);
    if (res != CaptureResult::None) last = res;
    button(c, "Other", {10, 40, 80, 20});
  };
  r.run();
  r.send(Nav::Accept);
  CHECK(r.ctx.capturing());
  r.send(Nav::Down);  // navigation is off while capturing
  CHECK_EQ(r.ctx.focused(), r.id("bind"));
  r.send(Event::raw_key(65));
  CHECK(last == CaptureResult::Captured && key == 65 && !r.ctx.capturing());
  r.send(Nav::Accept);
  r.send(Event::raw_key(r.ctx.key_cancel));
  CHECK(last == CaptureResult::Cancelled && !r.ctx.capturing());
  r.send(Nav::Accept);
  r.send(Event::raw_key(r.ctx.key_clear));
  CHECK(last == CaptureResult::Cleared);
  r.send(Nav::Accept);
  r.run({down(200, 200)});  // a click elsewhere cancels
  CHECK(last == CaptureResult::Cancelled && !r.ctx.capturing());
  // Accept works again afterwards, and Down navigates again
  r.send(Nav::Down);
  CHECK_EQ(r.ctx.focused(), r.id("Other"));
}

// ---- page selector ----
void test_page_chrome() {
  for (const int w : {640, 480}) {
    int page = 0;
    Rig r;
    r.canvas.resize(w, w == 640 ? 360 : 270);
    int content_hits = 0;
    r.builder = [&](Context& c) {
      static const PageItem items[] = {{"One", Icon::Play}, {"Two", Icon::Gear}, {"Three", Icon::Info}};
      const Rect content = page_chrome(c, c.screen(), items, page);
      if (button(c, "page" + std::to_string(page), {content.x, content.y, 80, 20}, {.default_focus = true})) ++content_hits;
    };
    r.run();
    CHECK_EQ(page, 0);
    r.send(Nav::PageNext);
    CHECK_EQ(page, 1);
    r.send(Nav::PageNext);
    r.send(Nav::PageNext);
    CHECK_EQ(page, 0);  // wraps
    r.send(Nav::PagePrev);
    CHECK_EQ(page, 2);
    // Back from a page control focuses the rail (or tab strip); Down/Right then selects pages with a keyboard
    r.send(Nav::Back);
    CHECK(r.ctx.focus_region() == Region::Rail);
    CHECK_EQ(r.ctx.focused(), r.id("page/Three"));
    r.send(Nav::Up);
    CHECK_EQ(page, 2 - (w == 640 ? 1 : 0)) /* rail: Up selects page 1; tabs: Up has nowhere to go */;
    if (w == 480) r.send(Nav::Left);
    CHECK_EQ(page, 1);  // focus follows selection
    r.send(Nav::Accept);  // into the page
    CHECK(r.ctx.focus_region() == Region::Page);
    r.send(Nav::Accept);
    CHECK_EQ(content_hits, 1);
    // pointer hover over another page item does not switch the page; a click does
    Rect other{};
    for (const FocusItem& it : r.ctx.items())
      if (it.id == r.id("page/One")) other = it.rect;
    r.run({move(other.cx(), other.cy())});
    CHECK_EQ(page, 1);
    r.run({down(other.cx(), other.cy())});
    r.run({up(other.cx(), other.cy())});
    CHECK_EQ(page, 0);
    // Back on the rail is not consumed by the kit (the host decides: close the menu)
    r.ctx.set_focus(r.id("page/One"));
    r.run();
    bool unconsumed = false;
    Rig r2;
    (void)r2;
    r.builder = [&](Context& c) {
      static const PageItem items[] = {{"One", Icon::Play}, {"Two", Icon::Gear}, {"Three", Icon::Info}};
      const Rect content = page_chrome(c, c.screen(), items, page);
      button(c, "page", {content.x, content.y, 80, 20});
      if (c.focus_region() == Region::Rail && c.peek_nav(Nav::Back)) unconsumed = true;
    };
    r.ctx.set_focus(r.id("page/One"));
    r.run();
    r.send(Nav::Back);
    CHECK(unconsumed);
  }
}

// ---- timing and animation ----
void test_animation_flags() {
  int permille = 500;
  Rig r;
  r.builder = [&](Context& c) { progress(c, {10, 10, 200, 20}, permille); };
  r.ctx.motion.reduced = true;
  CHECK(!r.run());  // reduced motion: nothing animates
  r.ctx.motion.reduced = false;
  CHECK(r.run({}, 16));  // a moving stripe
  const uint64_t h0 = r.canvas.hash();
  r.run({}, 100);
  CHECK(r.canvas.hash() != h0);  // the stripe moved
  r.ctx.motion.reduced = true;
  r.run({}, 160);
  const uint64_t h1 = r.canvas.hash();
  r.run({}, 160);
  CHECK_EQ(r.canvas.hash(), h1);  // static
  // an idle page with no animated widget does not ask for more frames
  Rig idle;
  idle.ctx.motion.reduced = false;
  idle.builder = [&](Context& c) {
    Mini m;
    (void)m;
    button(c, "x", {10, 10, 80, 20});
  };
  CHECK(!idle.run({}, 16));
}

// ---- every control of every demo page can be reached by Up/Down/Left/Right/Back/Accept ----
void test_reachability() {
  int pages_checked = 0;
  for (const int w : {640, 480}) {
    for (const bool st : {false, true}) {
      for (int page = 0; page < static_cast<int>(pages().size()); ++page) {
        Context ctx;
        ctx.motion.reduced = true;
        DemoState state;
        state.page = page;
        if (st) state.game = Game::St;
        Canvas canvas(w, w == 640 ? 360 : 270);
        auto builder = [&](Context& c) { build(c, state); };
        ctx.run(canvas, {}, 0, builder);
        ctx.run(canvas, {}, 0, builder);
        const std::vector<FocusItem> items = ctx.items();
        CHECK(!items.empty());
        int start = -1;
        for (size_t i = 0; i < items.size(); ++i)
          if (items[i].id == ctx.focused()) start = static_cast<int>(i);
        CHECK(start >= 0);
        if (start < 0) continue;
        int rail_target = -1, page_entry = -1;
        for (size_t i = 0; i < items.size(); ++i) {
          if (items[i].region == Region::Rail && (rail_target < 0 || items[i].selected)) rail_target = static_cast<int>(i);
          if (items[i].region == Region::Page && items[i].default_focus && page_entry < 0) page_entry = static_cast<int>(i);
        }
        for (size_t i = 0; i < items.size() && page_entry < 0; ++i)
          if (items[i].region == Region::Page) page_entry = static_cast<int>(i);
        std::set<int> seen{start};
        std::vector<int> todo{start};
        while (!todo.empty()) {
          const int cur = todo.back();
          todo.pop_back();
          auto visit = [&](int to) {
            if (to >= 0 && seen.insert(to).second) todo.push_back(to);
          };
          for (const Nav d : {Nav::Up, Nav::Down, Nav::Left, Nav::Right}) {
            if ((d == Nav::Left || d == Nav::Right) && items[static_cast<size_t>(cur)].consumes_horizontal) continue;
            visit(Context::neighbor(items, cur, d, false));
          }
          if (items[static_cast<size_t>(cur)].region == Region::Page) visit(rail_target);  // Back
          else visit(page_entry);                                                          // Accept
        }
        for (size_t i = 0; i < items.size(); ++i)
          if (!seen.count(static_cast<int>(i))) std::fprintf(stderr, "  unreachable item %zu at %d,%d %dx%d\n", i, items[i].rect.x, items[i].rect.y, items[i].rect.w, items[i].rect.h);
        if (seen.size() != items.size())
          std::fprintf(stderr, "unreachable controls on page %d (%dx, st=%d): %zu of %zu reached\n", page, w, st, seen.size(), items.size());
        CHECK_EQ(seen.size(), items.size());
        ++pages_checked;
      }
    }
  }
  CHECK_EQ(pages_checked, 2 * 2 * static_cast<int>(pages().size()));
}

// ---- determinism and the demo scenes ----
void test_scenes() {
  std::set<uint64_t> hashes;
  for (const SceneSpec& s : scenes()) {
    for (const bool hc : {false, true}) {
      const Canvas a = render_scene(s, 640, 360, hc);
      const Canvas b = render_scene(s, 640, 360, hc);
      CHECK_EQ(a.hash(), b.hash());
      hashes.insert(a.hash());
      const Canvas n = render_scene(s, 480, 270, hc);
      CHECK_EQ(n.width(), 480);
      hashes.insert(n.hash());
      // a drawn scene is not blank: more than one colour
      CHECK(count_color(a, a.get(0, 0)) < a.width() * a.height());
    }
  }
  CHECK_EQ(hashes.size(), scenes().size() * 4);  // every scene, size and theme differs
  // the play scene with and without motion differs (starfield), and the starfield moves with time
  Canvas c1(640, 360), c2(640, 360);
  Theme t = Theme::standard();
  draw_starfield(c1, c1.bounds(), 0, t);
  draw_starfield(c2, c2.bounds(), 5000, t);
  CHECK(c1.hash() != c2.hash());
  Canvas c3(640, 360), c4(640, 360);
  draw_starfield(c3, c3.bounds(), 100, t);
  draw_starfield(c4, c4.bounds(), 100, t);
  CHECK_EQ(c3.hash(), c4.hash());
  Canvas none(640, 360);
  draw_starfield(none, none.bounds(), 100, Theme::high_contrast_theme());
  CHECK_EQ(none.hash(), Canvas(640, 360).hash());  // no stars in High contrast
  const std::vector<uint8_t> png = encode_png(c1);
  CHECK(png.size() > 100 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
}


// ---- cost of a frame ----
// A whole page, settled, at the canvas a 1280x720 window uses. The gate is generous (CI runners are noisy, sanitizer builds slow);
// the real figure is printed. Everything is integer drawing into a 640x360 canvas, so a Raspberry Pi 3 has the same work to do.
void test_frame_cost() {
  Context ctx;
  ctx.motion.reduced = true;
  DemoState st;
  st.page = 1;
  Canvas canvas(640, 360);
  auto builder = [&](Context& c) { build(c, st); };
  ctx.run(canvas, {}, 0, builder);
  const int frames = 30;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    st.display.size = 1 + i % 8;  // a changing page, not a cached one
    ctx.run(canvas, {key(i % 2 ? Nav::Down : Nav::Up)}, 16, builder);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
  std::printf("display page frame (event + settle passes): %.2f ms\n", ms);
#ifdef X2_SANITIZED_BUILD
  CHECK(ms < 150.0);
#else
  CHECK(ms < 40.0);
#endif
}

// ---- the demo pages' model ----
void test_demo_model() {
  DemoState st;
  CHECK(display_command(st) == "x2recomp --game disk --modern interp");
  st.display.faithful = true;
  CHECK(display_command(st) == "x2recomp --game disk --faithful");
  st.display = DisplayModel{};
  st.game = Game::St;
  CHECK(display_command(st) == "x2recomp --game st");
  st.game = Game::Disk;
  st.display.size = 4;
  st.display.filter = 1;
  st.display.smooth = 0;
  CHECK(display_command(st) == "x2recomp --game disk --scale 4 --filter sharp");
}

}  // namespace

int main() {
  test_types();
  test_canvas();
  test_font();
  test_theme_geometry_input();
  test_navigation_and_widgets();
  test_scroll();
  test_message_list();
  test_key_capture();
  test_page_chrome();
  test_animation_flags();
  test_reachability();
  test_scenes();
  test_frame_cost();
  test_demo_model();
  std::printf("%d checks, %d failures\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
