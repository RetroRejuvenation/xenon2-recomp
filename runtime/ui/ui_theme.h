// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Palette, metrics and motion settings. Two themes: the default dark navy with amber and cyan, and High contrast
// (white on black, yellow focus). docs/re-notes/gui-widget-kit.md has the rationale and the contrast table.
#pragma once
#include <string>
#include <vector>

#include "ui_types.h"

namespace x2::ui {

struct Palette {
  Color ground, panel, raised, line;
  Color text, dim;
  Color amber, cyan, ok, warn, err;
  Color on_amber;  // text drawn on an amber (accent) fill
  Color scrim;     // colour blended over the page behind a modal
};

struct Metrics {
  int grid = 4;
  int row_h = 20;
  int card_pad = 12;
  int margin = 16;
  int chamfer = 3;
  int min_target = 20;
};

struct Theme {
  Palette c;
  Metrics m;
  bool high_contrast = false;
  bool decor = true;  // dithered stripes, starfield: off in High contrast
  static Theme standard();
  static Theme high_contrast_theme();
};

// WCAG 2.x contrast ratio of two colours (floating point, used by tests and tooling only, never by drawing).
double contrast_ratio(Color a, Color b);

struct ContrastPair {
  std::string name;
  Color fg, bg;
  double minimum;  // 4.5 for text, 3.0 for focus and selection indicators
};
// Every foreground/background pair the widgets actually draw with this theme.
std::vector<ContrastPair> contrast_pairs(const Theme& t);

// Motion: all values are integer milliseconds or permille.
struct Motion {
  bool reduced = false;
  static constexpr int kPageMs = 90;      // page slide + fade
  static constexpr int kFocusMs = 60;     // focus ring ease
  static constexpr int kBreatheMs = 1200; // focus ring breathing period on cards
  int page_ms() const { return reduced ? 0 : kPageMs; }
};

// Smooth-step in permille (0..1000 in, 0..1000 out), integer.
int ease_permille(int t_permille);

}  // namespace x2::ui
