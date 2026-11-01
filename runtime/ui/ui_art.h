// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Procedural art: icons, library-card emblems, the launcher starfield and a test card. All of it is drawn from
// rectangles and circles in code. There is no bitmap, no game art and no logo in the UI.
#pragma once
#include "ui_canvas.h"
#include "ui_theme.h"

namespace x2::ui {

enum class Icon {
  Floppy, Disc, Chip, Gear, Speaker, Controller, Puzzle, Warning, Check, Cross, Play, Display, Info,
};

// Draws a 16x16-grid icon scaled by the largest whole number that fits `r`, centred in it. `bg` is the colour
// of the cut-outs (the surface the icon sits on).
void draw_icon(Canvas& c, Icon icon, Rect r, Color fg, Color bg);

// A library-card picture: a seeded pattern in the accent colour with the icon on top.
void draw_emblem(Canvas& c, Rect r, Icon icon, Color accent, uint32_t seed, const Theme& t);

// Sparse parallax stars (three layers), updated at 12 frames per second. Deterministic for a given t_ms.
void draw_starfield(Canvas& c, Rect area, int t_ms, const Theme& t);

// A 320x256 test card (colour bars, grey ramp, checkerboard, ruler, circle) scaled into `r`.
void draw_test_card(Canvas& c, Rect r);
// Darkens every second row inside `r` (the scanline filter in the Display preview).
void apply_scanlines(Canvas& c, Rect r, int alpha);

uint32_t hash32(uint32_t x);

}  // namespace x2::ui
