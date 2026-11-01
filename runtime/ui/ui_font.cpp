// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "ui_font.h"

#include <algorithm>
#include <cstdint>

namespace x2::ui {

namespace {
struct GlyphRow8 {
  uint32_t cp;
  uint8_t rows[8];
};
struct GlyphRow16 {
  uint32_t cp;
  uint8_t rows[16];
};
#include "font_data.inc"

template <typename G>
const uint8_t* find_glyph(const void* table, size_t count, uint32_t cp) {
  const G* g = static_cast<const G*>(table);
  const G* e = g + count;
  const G* it = std::lower_bound(g, e, cp, [](const G& a, uint32_t v) { return a.cp < v; });
  return (it != e && it->cp == cp) ? it->rows : nullptr;
}
}  // namespace

const Font& Font::body() {
  static const Font f(16, kGlyphs16, sizeof(kGlyphs16) / sizeof(kGlyphs16[0]));
  return f;
}

const Font& Font::caption() {
  static const Font f(8, kGlyphs8, sizeof(kGlyphs8) / sizeof(kGlyphs8[0]));
  return f;
}

const uint8_t* Font::glyph(uint32_t cp) const {
  return h_ == 16 ? find_glyph<GlyphRow16>(table_, count_, cp) : find_glyph<GlyphRow8>(table_, count_, cp);
}

uint32_t next_codepoint(std::string_view& s) {
  if (s.empty()) return 0;
  const auto b0 = static_cast<uint8_t>(s[0]);
  auto take = [&](size_t n, uint32_t first, uint32_t min) -> uint32_t {
    if (s.size() < n) {
      s.remove_prefix(1);
      return 0xFFFD;
    }
    uint32_t cp = first;
    for (size_t i = 1; i < n; ++i) {
      const auto b = static_cast<uint8_t>(s[i]);
      if ((b & 0xC0) != 0x80) {
        s.remove_prefix(1);
        return 0xFFFD;
      }
      cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      s.remove_prefix(1);
      return 0xFFFD;
    }
    s.remove_prefix(n);
    return cp;
  };
  if (b0 < 0x80) {
    s.remove_prefix(1);
    return b0;
  }
  if ((b0 & 0xE0) == 0xC0) return take(2, b0 & 0x1F, 0x80);
  if ((b0 & 0xF0) == 0xE0) return take(3, b0 & 0x0F, 0x800);
  if ((b0 & 0xF8) == 0xF0) return take(4, b0 & 0x07, 0x10000);
  s.remove_prefix(1);
  return 0xFFFD;
}

int codepoint_count(std::string_view s) {
  int n = 0;
  while (!s.empty()) {
    next_codepoint(s);
    ++n;
  }
  return n;
}

int text_width(std::string_view s, int scale, int spacing) {
  const int n = codepoint_count(s);
  return n * (8 * scale + spacing);
}

namespace {
void draw_cell(Canvas& c, const Font& f, uint32_t cp, int x, int y, Color color, int scale) {
  const uint8_t* g = f.glyph(cp);
  const int h = f.cell_h();
  if (!g) {  // visible fallback: a hollow box
    c.frame({x + scale, y + scale, 6 * scale, (h - 2) * scale}, color, scale);
    return;
  }
  for (int row = 0; row < h; ++row) {
    const uint8_t bits = g[row];
    if (!bits) continue;
    for (int col = 0; col < 8;) {
      if (!(bits & (0x80u >> col))) {
        ++col;
        continue;
      }
      int run = 1;
      while (col + run < 8 && (bits & (0x80u >> (col + run)))) ++run;  // horizontal runs: fewer fills
      c.fill({x + col * scale, y + row * scale, run * scale, scale}, color);
      col += run;
    }
  }
}
}  // namespace

int draw_text(Canvas& c, const Font& f, int x, int y, std::string_view s, Color color, int scale, int spacing) {
  const Rect clip = c.clip();
  const int adv = 8 * scale + spacing;
  while (!s.empty()) {
    const uint32_t cp = next_codepoint(s);
    if (cp == '\n') continue;
    if (x + adv > clip.x && x < clip.right() && y + f.cell_h() * scale > clip.y && y < clip.bottom())
      draw_cell(c, f, cp, x, y, color, scale);
    x += adv;
  }
  return x;
}

std::vector<std::string> wrap_text(std::string_view s, int max_w, int scale) {
  std::vector<std::string> lines;
  const int per = std::max(1, max_w / (8 * scale));  // characters per line
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t nl = s.find('\n', pos);
    std::string_view para = s.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    std::string line;
    int line_n = 0;
    auto flush = [&] {
      lines.push_back(line);
      line.clear();
      line_n = 0;
    };
    size_t i = 0;
    while (i < para.size()) {
      while (i < para.size() && para[i] == ' ') ++i;
      size_t j = i;
      while (j < para.size() && para[j] != ' ') ++j;
      std::string_view word = para.substr(i, j - i);
      i = j;
      if (word.empty()) break;
      int wn = codepoint_count(word);
      while (wn > per) {  // split an over-long word
        if (line_n) flush();
        std::string_view rest = word;
        std::string part;
        for (int k = 0; k < per; ++k) {
          std::string_view before = rest;
          next_codepoint(rest);
          part.append(before.substr(0, before.size() - rest.size()));
        }
        line = part;
        line_n = per;
        flush();
        word = rest;
        wn = codepoint_count(word);
      }
      if (line_n && line_n + 1 + wn > per) flush();
      if (line_n) {
        line.push_back(' ');
        ++line_n;
      }
      line.append(word);
      line_n += wn;
    }
    lines.push_back(line);
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return lines;
}

std::string elide_text(std::string_view s, int max_w, int scale, bool middle) {
  const int per = max_w / (8 * scale);
  const int n = codepoint_count(s);
  if (n <= per) return std::string(s);
  if (per <= 0) return {};
  static const char kEllipsis[] = "\xE2\x80\xA6";
  if (per == 1) return kEllipsis;
  auto take_front = [&](std::string_view v, int k) {
    std::string_view rest = v;
    for (int i = 0; i < k && !rest.empty(); ++i) next_codepoint(rest);
    return std::string(v.substr(0, v.size() - rest.size()));
  };
  if (!middle) return take_front(s, per - 1) + kEllipsis;
  const int head = (per - 1 + 1) / 2, tail = per - 1 - head;
  std::string_view rest = s;
  for (int i = 0; i < n - tail; ++i) next_codepoint(rest);
  return take_front(s, head) + kEllipsis + std::string(rest);
}

}  // namespace x2::ui
