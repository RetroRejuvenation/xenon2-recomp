// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The UI's bitmap fonts: Unscii 8x16 (body, headings at 2x) and 8x8 (captions). Public domain; see
// vendor/unscii/LICENSE-unscii.txt. Text is UTF-8; a code point the font lacks is drawn as a hollow box.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "ui_canvas.h"

namespace x2::ui {

class Font {
 public:
  static const Font& body();     // 8x16
  static const Font& caption();  // 8x8
  int cell_w() const { return 8; }
  int cell_h() const { return h_; }
  // Eight pixel wide rows, MSB leftmost; nullptr when the font has no glyph for `cp`.
  const uint8_t* glyph(uint32_t cp) const;
  bool has(uint32_t cp) const { return glyph(cp) != nullptr; }

 private:
  Font(int h, const void* table, size_t count) : h_(h), table_(table), count_(count) {}
  int h_;
  const void* table_;
  size_t count_;
};

// Reads one code point from the front of `s` and removes it. Malformed input yields U+FFFD, one byte at a time.
uint32_t next_codepoint(std::string_view& s);
int codepoint_count(std::string_view s);

// Width in pixels: every code point is one 8-pixel cell times `scale`, plus `spacing` after each.
int text_width(std::string_view s, int scale = 1, int spacing = 0);
// Draws with the top-left of the first cell at (x, y); returns the x after the last cell.
int draw_text(Canvas& c, const Font& f, int x, int y, std::string_view s, Color color, int scale = 1, int spacing = 0);

// Word wrap to `max_w` pixels (explicit '\n' honoured; a word longer than a line is split).
std::vector<std::string> wrap_text(std::string_view s, int max_w, int scale = 1);
// Shortens to `max_w` pixels with an ellipsis (U+2026) at the end, or in the middle.
std::string elide_text(std::string_view s, int max_w, int scale = 1, bool middle = false);

}  // namespace x2::ui
