// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Basic value types of the launcher UI kit. Everything here is integer: layout and drawing never use floating
// point, so a frame is bit-identical on every platform and compiler.
#pragma once
#include <cstdint>
#include <string_view>

namespace x2::ui {

// 0xAABBGGRR: the bytes in memory are R, G, B, A (SDL_PIXELFORMAT_ABGR8888 on little-endian hosts).
using Color = uint32_t;

constexpr Color rgb(unsigned r, unsigned g, unsigned b) {
  return 0xFF000000u | ((b & 255u) << 16) | ((g & 255u) << 8) | (r & 255u);
}
constexpr Color rgb_hex(uint32_t rrggbb) { return rgb((rrggbb >> 16) & 255u, (rrggbb >> 8) & 255u, rrggbb & 255u); }
constexpr unsigned red_of(Color c) { return c & 255u; }
constexpr unsigned green_of(Color c) { return (c >> 8) & 255u; }
constexpr unsigned blue_of(Color c) { return (c >> 16) & 255u; }

// c over d with `alpha` in 0..255 (255 = c). Exact integer rounding.
constexpr Color mix(Color d, Color c, int alpha) {
  const unsigned a = static_cast<unsigned>(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));
  auto ch = [&](unsigned dc, unsigned cc) { return (cc * a + dc * (255u - a) + 127u) / 255u; };
  return rgb(ch(red_of(d), red_of(c)), ch(green_of(d), green_of(c)), ch(blue_of(d), blue_of(c)));
}

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  constexpr int right() const { return x + w; }
  constexpr int bottom() const { return y + h; }
  constexpr int cx() const { return x + w / 2; }
  constexpr int cy() const { return y + h / 2; }
  constexpr bool empty() const { return w <= 0 || h <= 0; }
  constexpr bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  constexpr Rect inset(int d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
  constexpr Rect inset(int dx, int dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
  constexpr Rect moved(int dx, int dy) const { return {x + dx, y + dy, w, h}; }
  constexpr bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

constexpr Rect intersect(Rect a, Rect b) {
  const int x0 = a.x > b.x ? a.x : b.x;
  const int y0 = a.y > b.y ? a.y : b.y;
  const int x1 = a.right() < b.right() ? a.right() : b.right();
  const int y1 = a.bottom() < b.bottom() ? a.bottom() : b.bottom();
  return x1 > x0 && y1 > y0 ? Rect{x0, y0, x1 - x0, y1 - y0} : Rect{x0, y0, 0, 0};
}

// Rect cutting: each takes a slice off one side of `r` and returns it.
constexpr Rect cut_left(Rect& r, int w) {
  if (w > r.w) w = r.w < 0 ? 0 : r.w;
  const Rect s{r.x, r.y, w, r.h};
  r.x += w;
  r.w -= w;
  return s;
}
constexpr Rect cut_right(Rect& r, int w) {
  if (w > r.w) w = r.w < 0 ? 0 : r.w;
  const Rect s{r.x + r.w - w, r.y, w, r.h};
  r.w -= w;
  return s;
}
constexpr Rect cut_top(Rect& r, int h) {
  if (h > r.h) h = r.h < 0 ? 0 : r.h;
  const Rect s{r.x, r.y, r.w, h};
  r.y += h;
  r.h -= h;
  return s;
}
constexpr Rect cut_bottom(Rect& r, int h) {
  if (h > r.h) h = r.h < 0 ? 0 : r.h;
  const Rect s{r.x, r.y + r.h - h, r.w, h};
  r.h -= h;
  return s;
}

// Widget identity: FNV-1a over the label, chained with the parent id.
using Id = uint32_t;
constexpr Id hash_id(Id parent, std::string_view s) {
  uint32_t h = 2166136261u ^ parent;
  h *= 16777619u;
  for (const char ch : s) {
    h ^= static_cast<uint8_t>(ch);
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;  // 0 means "no widget"
}

}  // namespace x2::ui
