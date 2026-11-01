// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "ui_art.h"

#include <algorithm>

namespace x2::ui {

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

void draw_icon(Canvas& c, Icon icon, Rect r, Color fg, Color bg) {
  const int s = std::max(1, std::min(r.w, r.h) / 16);
  const int ox = r.x + (r.w - 16 * s) / 2, oy = r.y + (r.h - 16 * s) / 2;
  auto R = [&](int x, int y, int w, int h, Color col) { c.fill({ox + x * s, oy + y * s, w * s, h * s}, col); };
  auto D = [&](int x, int y, int rad, Color col) { c.disc(ox + x * s + s / 2, oy + y * s + s / 2, rad * s, col); };
  switch (icon) {
    case Icon::Floppy:
      R(1, 1, 14, 14, fg);
      R(12, 1, 3, 3, bg);
      R(4, 1, 8, 5, bg);
      R(9, 2, 2, 3, fg);
      R(3, 9, 10, 6, bg);
      R(5, 11, 6, 1, fg);
      break;
    case Icon::Disc:
      D(8, 8, 7, fg);
      D(8, 8, 2, bg);
      R(3, 5, 2, 1, bg);
      break;
    case Icon::Chip:
      R(4, 4, 8, 8, fg);
      R(6, 6, 4, 4, bg);
      for (int i = 0; i < 3; ++i) {
        R(1, 5 + i * 2, 3, 1, fg);
        R(12, 5 + i * 2, 3, 1, fg);
        R(5 + i * 2, 1, 1, 3, fg);
        R(5 + i * 2, 12, 1, 3, fg);
      }
      break;
    case Icon::Gear:
      R(7, 1, 2, 3, fg);
      R(7, 12, 2, 3, fg);
      R(1, 7, 3, 2, fg);
      R(12, 7, 3, 2, fg);
      R(3, 3, 2, 2, fg);
      R(11, 3, 2, 2, fg);
      R(3, 11, 2, 2, fg);
      R(11, 11, 2, 2, fg);
      D(8, 8, 5, fg);
      D(8, 8, 2, bg);
      break;
    case Icon::Speaker:
      R(2, 6, 4, 4, fg);
      R(6, 4, 2, 8, fg);
      R(8, 2, 2, 12, fg);
      R(11, 6, 1, 4, fg);
      R(13, 4, 1, 8, fg);
      break;
    case Icon::Controller:
      R(1, 5, 14, 7, fg);
      R(2, 12, 3, 2, fg);
      R(11, 12, 3, 2, fg);
      R(3, 8, 3, 1, bg);
      R(4, 7, 1, 3, bg);
      R(10, 7, 2, 2, bg);
      R(12, 9, 2, 2, bg);
      break;
    case Icon::Puzzle:
      R(2, 5, 10, 9, fg);
      D(7, 4, 2, fg);
      D(12, 9, 2, fg);
      R(5, 8, 3, 3, bg);
      break;
    case Icon::Warning:
      c.triangle_up({ox + 1 * s, oy + 2 * s, 14 * s, 12 * s}, fg);
      R(7, 6, 2, 4, bg);
      R(7, 11, 2, 2, bg);
      break;
    case Icon::Check:
      R(2, 8, 2, 2, fg);
      R(4, 10, 2, 2, fg);
      R(6, 8, 2, 2, fg);
      R(8, 6, 2, 2, fg);
      R(10, 4, 2, 2, fg);
      R(12, 2, 2, 2, fg);
      break;
    case Icon::Cross:
      for (int i = 0; i < 10; ++i) {
        R(3 + i, 3 + i, 2, 2, fg);
        R(12 - i, 3 + i, 2, 2, fg);
      }
      break;
    case Icon::Play:
      for (int i = 0; i < 14; ++i) R(4, 1 + i, std::min(i, 13 - i) + 1, 1, fg);
      break;
    case Icon::Display:
      R(1, 2, 14, 9, fg);
      R(3, 4, 10, 5, bg);
      R(6, 12, 4, 1, fg);
      R(4, 13, 8, 1, fg);
      break;
    case Icon::Info:
      D(8, 8, 7, fg);
      R(7, 4, 2, 2, bg);
      R(7, 7, 2, 5, bg);
      break;
  }
}

void draw_emblem(Canvas& c, Rect r, Icon icon, Color accent, uint32_t seed, const Theme& t) {
  c.fill(r, t.c.raised);
  if (t.decor) {
    for (int y = 0; y < r.h; y += 4)
      for (int x = 0; x < r.w; x += 4) {
        const uint32_t h = hash32(seed * 2654435761u + static_cast<uint32_t>(x / 4) * 7919u + static_cast<uint32_t>(y / 4) * 104729u);
        const int a = (h & 3u) == 0 ? 56 : ((h & 3u) == 1 ? 28 : 0);
        if (a) c.blend({r.x + x, r.y + y, 4, 4}, accent, a);
      }
  }
  const int side = std::min(r.w, r.h) * 3 / 5;
  const Rect ir{r.cx() - side / 2, r.cy() - side / 2, side, side};
  draw_icon(c, icon, ir, accent, t.c.raised);
}

void draw_starfield(Canvas& c, Rect area, int t_ms, const Theme& t) {
  if (!t.decor || area.empty()) return;
  const int tq = t_ms - t_ms % 83;  // 12 updates per second
  for (uint32_t i = 0; i < 56; ++i) {
    const int layer = static_cast<int>(i % 3);
    const int x0 = static_cast<int>(hash32(i * 2654435761u + 1u) % static_cast<uint32_t>(area.w));
    const int y = static_cast<int>(hash32(i * 40503u + 7u) % static_cast<uint32_t>(area.h));
    const long long speed = 4 * (layer + 1);  // pixels per second
    long long pos = (static_cast<long long>(x0) * 1000 - static_cast<long long>(tq) * speed) % (static_cast<long long>(area.w) * 1000);
    if (pos < 0) pos += static_cast<long long>(area.w) * 1000;
    const int x = static_cast<int>(pos / 1000);
    const Color col = mix(t.c.ground, t.c.dim, 36 + layer * 36);
    const int sz = layer == 2 ? 2 : 1;
    c.fill({area.x + x, area.y + y, sz, sz}, col);
  }
}

void draw_test_card(Canvas& c, Rect r) {
  if (r.empty()) return;
  auto X = [&](int v) { return r.x + v * r.w / 320; };
  auto Y = [&](int v) { return r.y + v * r.h / 256; };
  auto box = [&](int x, int y, int w, int h, Color col) { c.fill({X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y)}, col); };
  box(0, 0, 320, 256, rgb(32, 32, 32));
  static const Color kBars[8] = {rgb(235, 235, 235), rgb(235, 235, 0), rgb(0, 235, 235), rgb(0, 235, 0),
                                 rgb(235, 0, 235),   rgb(235, 0, 0),   rgb(0, 0, 235),   rgb(16, 16, 16)};
  for (int i = 0; i < 8; ++i) box(i * 40, 0, 40, 112, kBars[i]);
  for (int i = 0; i < 8; ++i) box(i * 40, 112, 40, 32, rgb(i * 255 / 7, i * 255 / 7, i * 255 / 7));
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 20; ++x)
      if ((x + y) & 1) box(x * 16, 144 + y * 16, 16, 16, rgb(220, 220, 220));
  for (int i = 0; i <= 40; ++i) box(i * 8, 200, 1, i % 4 == 0 ? 16 : 8, rgb(235, 235, 235));
  box(0, 224, 320, 1, rgb(235, 235, 235));
  const int cx = X(160), cy = Y(128), rad = std::max(2, 90 * std::min(r.w, r.h * 5 / 4) / 320);
  c.ring(cx, cy, rad, std::max(1, rad / 40), rgb(250, 250, 250));
  c.fill({cx - rad, cy, 2 * rad, 1}, rgb(250, 250, 250));
  c.fill({cx, cy - rad, 1, 2 * rad}, rgb(250, 250, 250));
}

void apply_scanlines(Canvas& c, Rect r, int alpha) {
  for (int y = r.y + 1; y < r.bottom(); y += 2) c.blend({r.x, y, r.w, 1}, rgb(0, 0, 0), alpha);
}

}  // namespace x2::ui
