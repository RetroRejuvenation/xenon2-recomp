// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Small pieces the page files share: the page heading and the width of a button.
#pragma once
#include <string_view>

#include "../ui_widgets.h"

namespace x2::ui {

// The heading of a page, one line on a short canvas; takes it off the top of `body`.
inline void page_title(Context& ctx, Rect& body, std::string_view title, std::string_view sub) {
  const bool small = ctx.screen().h < 320;
  const Rect h = cut_top(body, small ? 20 : 32);
  const int cw = small ? 9 : 17;
  if (small) draw_text(ctx.canvas(), Font::body(), h.x, h.y + 2, title, ctx.theme.c.text, 1, 1);
  else draw_heading(ctx, h, title, ctx.theme.c.text);
  if (!sub.empty()) {
    const int tw = static_cast<int>(title.size()) * cw + 12;
    draw_caption(ctx, {h.x + tw, h.y + (small ? 6 : 12), h.w - tw, 10}, sub, ctx.theme.c.dim);
  }
  cut_top(body, small ? 2 : 6);
}

// A word-wrapped paragraph in a stack: the row is as tall as the wrapped text, so nothing is cut however narrow the canvas is.
inline void paragraph(Context& ctx, Stack& st, std::string_view text, Color col, int line_h = 11) {
  const int lines = static_cast<int>(wrap_text(text, st.w, 1).size());
  draw_paragraph(ctx, st.row(lines * line_h + 1), text, col, line_h);
}

// A button wide enough for its label.
inline int button_w(std::string_view label) { return text_width(label) + 20; }

}  // namespace x2::ui
