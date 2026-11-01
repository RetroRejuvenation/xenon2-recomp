// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "ui_canvas.h"

#include <algorithm>

namespace x2::ui {

void Canvas::resize(int w, int h) {
  w_ = w < 0 ? 0 : w;
  h_ = h < 0 ? 0 : h;
  px_.assign(static_cast<size_t>(w_) * static_cast<size_t>(h_), 0xFF000000u);
  clips_.clear();
  clip_ = bounds();
}

void Canvas::clear(Color c) { std::fill(px_.begin(), px_.end(), c); }

void Canvas::push_clip(Rect r) {
  clips_.push_back(clip_);
  clip_ = intersect(clip_, r);
}

void Canvas::pop_clip() {
  if (clips_.empty()) return;
  clip_ = clips_.back();
  clips_.pop_back();
}

void Canvas::put(int x, int y, Color c) {
  if (!clip_.contains(x, y)) return;
  px_[static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)] = c;
}

Color Canvas::get(int x, int y) const {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return 0;
  return px_[static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)];
}

void Canvas::fill(Rect r, Color c) {
  const Rect v = intersect(r, clip_);
  if (v.empty()) return;
  for (int y = v.y; y < v.bottom(); ++y) {
    uint32_t* row = &px_[static_cast<size_t>(y) * static_cast<size_t>(w_)];
    std::fill(row + v.x, row + v.right(), c);
  }
}

void Canvas::blend(Rect r, Color c, int alpha) {
  const Rect v = intersect(r, clip_);
  if (v.empty() || alpha <= 0) return;
  if (alpha >= 255) {
    fill(v, c);
    return;
  }
  for (int y = v.y; y < v.bottom(); ++y) {
    uint32_t* row = &px_[static_cast<size_t>(y) * static_cast<size_t>(w_)];
    for (int x = v.x; x < v.right(); ++x) row[x] = mix(row[x], c, alpha);
  }
}

void Canvas::frame(Rect r, Color c, int t) {
  if (r.empty() || t <= 0) return;
  if (t * 2 >= r.w || t * 2 >= r.h) {
    fill(r, c);
    return;
  }
  fill({r.x, r.y, r.w, t}, c);
  fill({r.x, r.bottom() - t, r.w, t}, c);
  fill({r.x, r.y + t, t, r.h - 2 * t}, c);
  fill({r.right() - t, r.y + t, t, r.h - 2 * t}, c);
}

namespace {
// How far row `i` of an h-row chamfered panel is cut in from each side.
int chamfer_inset(int i, int h, int cut) {
  const int top = cut - i;
  const int bottom = cut - (h - 1 - i);
  const int s = top > bottom ? top : bottom;
  return s > 0 ? s : 0;
}
}  // namespace

void Canvas::fill_chamfer(Rect r, int cut, Color c) {
  if (r.empty()) return;
  if (cut * 2 > r.h || cut * 2 > r.w) cut = std::min(r.w, r.h) / 2;
  for (int i = 0; i < r.h; ++i) {
    const int s = chamfer_inset(i, r.h, cut);
    fill({r.x + s, r.y + i, r.w - 2 * s, 1}, c);
  }
}

void Canvas::frame_chamfer(Rect r, int cut, Color c) {
  if (r.empty()) return;
  if (cut * 2 > r.h || cut * 2 > r.w) cut = std::min(r.w, r.h) / 2;
  for (int i = 0; i < r.h; ++i) {
    const int s = chamfer_inset(i, r.h, cut);
    if (i == 0 || i == r.h - 1) {
      fill({r.x + s, r.y + i, r.w - 2 * s, 1}, c);
    } else {
      put(r.x + s, r.y + i, c);
      put(r.right() - 1 - s, r.y + i, c);
    }
  }
}

void Canvas::dither(Rect r, Color a, Color b, int phase) {
  const Rect v = intersect(r, clip_);
  for (int y = v.y; y < v.bottom(); ++y)
    for (int x = v.x; x < v.right(); ++x) put(x, y, ((x + y + phase) & 1) ? b : a);
}

void Canvas::disc(int cx, int cy, int r, Color c) {
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) ++dx;  // +r: rounder small circles
    fill({cx - dx, cy + dy, 2 * dx + 1, 1}, c);
  }
}

void Canvas::ring(int cx, int cy, int r, int thickness, Color c) {
  const int ri = r - thickness;
  for (int dy = -r; dy <= r; ++dy) {
    int dxo = 0;
    while ((dxo + 1) * (dxo + 1) + dy * dy <= r * r + r) ++dxo;
    if (dy > -ri && dy < ri && ri > 0) {
      int dxi = 0;
      while ((dxi + 1) * (dxi + 1) + dy * dy <= ri * ri + ri) ++dxi;
      fill({cx - dxo, cy + dy, dxo - dxi, 1}, c);
      fill({cx + dxi + 1, cy + dy, dxo - dxi, 1}, c);
    } else {
      fill({cx - dxo, cy + dy, 2 * dxo + 1, 1}, c);
    }
  }
}

void Canvas::triangle_up(Rect r, Color c) {
  if (r.empty()) return;
  for (int i = 0; i < r.h; ++i) {
    const int half = (r.w * (i + 1)) / (2 * r.h);  // integer growth from the apex
    const int w = std::max(1, half * 2 + (r.w & 1));
    fill({r.cx() - w / 2, r.y + i, w, 1}, c);
  }
}

uint64_t Canvas::hash() const {
  uint64_t h = 1469598103934665603ull;
  auto eat = [&](unsigned byte) {
    h ^= byte;
    h *= 1099511628211ull;
  };
  for (const int v : {w_, h_})
    for (int s = 0; s < 32; s += 8) eat((static_cast<unsigned>(v) >> s) & 255u);
  for (const uint32_t p : px_) {
    eat(red_of(p));
    eat(green_of(p));
    eat(blue_of(p));
    eat((p >> 24) & 255u);
  }
  return h;
}

}  // namespace x2::ui
