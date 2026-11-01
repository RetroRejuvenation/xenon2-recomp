// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "ui_theme.h"

#include <cmath>

namespace x2::ui {

Theme Theme::standard() {
  Theme t;
  t.c = Palette{
      rgb_hex(0x0B0F1A),  // ground
      rgb_hex(0x141B2D),  // panel
      rgb_hex(0x1E2740),  // raised
      rgb_hex(0x34416A),  // line (decorative: meaning never rests on it)
      rgb_hex(0xE8ECF8),  // text
      rgb_hex(0x9AA6C8),  // dim
      rgb_hex(0xFFB03B),  // amber
      rgb_hex(0x4FD1E8),  // cyan
      rgb_hex(0x5BD98A),  // ok
      rgb_hex(0xFFD05B),  // warn
      rgb_hex(0xFF6B6B),  // err
      rgb_hex(0x0B0F1A),  // on_amber
      rgb_hex(0x05070D),  // scrim
  };
  return t;
}

Theme Theme::high_contrast_theme() {
  Theme t;
  t.c = Palette{
      rgb_hex(0x000000),  // ground
      rgb_hex(0x000000),  // panel (separated from the ground by a white border)
      rgb_hex(0x1C1C1C),  // raised
      rgb_hex(0xFFFFFF),  // line
      rgb_hex(0xFFFFFF),  // text
      rgb_hex(0xD0D0D0),  // dim
      rgb_hex(0xFFFF00),  // amber (focus, primary)
      rgb_hex(0x00FFFF),  // cyan
      rgb_hex(0x40FF40),  // ok
      rgb_hex(0xFFFF00),  // warn
      rgb_hex(0xFF7070),  // err
      rgb_hex(0x000000),  // on_amber
      rgb_hex(0x000000),  // scrim
  };
  t.high_contrast = true;
  t.decor = false;
  return t;
}

namespace {
double channel(unsigned v) {
  const double s = v / 255.0;
  return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}
double luminance(Color c) {
  return 0.2126 * channel(red_of(c)) + 0.7152 * channel(green_of(c)) + 0.0722 * channel(blue_of(c));
}
}  // namespace

double contrast_ratio(Color a, Color b) {
  double la = luminance(a), lb = luminance(b);
  if (la < lb) std::swap(la, lb);
  return (la + 0.05) / (lb + 0.05);
}

std::vector<ContrastPair> contrast_pairs(const Theme& t) {
  const Palette& c = t.c;
  std::vector<ContrastPair> v;
  struct Bg {
    const char* name;
    Color col;
  };
  const Bg bgs[] = {{"ground", c.ground}, {"panel", c.panel}, {"raised", c.raised}};
  for (const Bg& b : bgs) {
    v.push_back({std::string("text on ") + b.name, c.text, b.col, 4.5});
    v.push_back({std::string("dim on ") + b.name, c.dim, b.col, 4.5});
    v.push_back({std::string("cyan on ") + b.name, c.cyan, b.col, 4.5});
    v.push_back({std::string("ok on ") + b.name, c.ok, b.col, 4.5});
    v.push_back({std::string("warn on ") + b.name, c.warn, b.col, 4.5});
    v.push_back({std::string("err on ") + b.name, c.err, b.col, 4.5});
    v.push_back({std::string("amber on ") + b.name, c.amber, b.col, 4.5});  // selected text, links
    v.push_back({std::string("focus ring on ") + b.name, c.amber, b.col, 3.0});
  }
  v.push_back({"text on amber button", c.on_amber, c.amber, 4.5});
  v.push_back({"amber selection marker on raised", c.amber, c.raised, 3.0});
  return v;
}

int ease_permille(int t) {
  if (t <= 0) return 0;
  if (t >= 1000) return 1000;
  // 3t^2 - 2t^3 with t in permille.
  const long long tt = t;
  return static_cast<int>((3 * tt * tt * 1000 - 2 * tt * tt * tt) / 1000000);
}

}  // namespace x2::ui
