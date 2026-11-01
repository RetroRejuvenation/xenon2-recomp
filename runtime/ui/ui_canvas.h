// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A CPU pixel canvas: the UI is drawn here at its logical size and then scaled up by whole numbers when it is
// presented (ui_sdl.h). Integer-only, so the same calls give the same pixels on every platform: the golden
// tests compare canvas hashes at zero tolerance and need no window or renderer.
#pragma once
#include <cstdint>
#include <vector>

#include "ui_types.h"

namespace x2::ui {

class Canvas {
 public:
  Canvas() = default;
  Canvas(int w, int h) { resize(w, h); }
  void resize(int w, int h);
  int width() const { return w_; }
  int height() const { return h_; }
  Rect bounds() const { return {0, 0, w_, h_}; }
  const uint32_t* data() const { return px_.data(); }
  uint32_t* data() { return px_.data(); }

  void clear(Color c);
  // Clipping: every drawing call is limited to the intersection of all pushed rectangles and the canvas.
  void push_clip(Rect r);
  void pop_clip();
  Rect clip() const { return clip_; }

  void put(int x, int y, Color c);
  Color get(int x, int y) const;
  void fill(Rect r, Color c);
  void blend(Rect r, Color c, int alpha);  // alpha 0..255
  void hline(int x, int y, int w, Color c) { fill({x, y, w, 1}, c); }
  void vline(int x, int y, int h, Color c) { fill({x, y, 1, h}, c); }
  void frame(Rect r, Color c, int thickness = 1);
  // Panels with 45 degree cut corners (no rounded corners, no shadows).
  void fill_chamfer(Rect r, int cut, Color c);
  void frame_chamfer(Rect r, int cut, Color c);
  // Checkerboard of two colours; `phase` flips which one starts.
  void dither(Rect r, Color a, Color b, int phase);
  // A filled circle / ring on the pixel grid (radius in pixels).
  void disc(int cx, int cy, int r, Color c);
  void ring(int cx, int cy, int r, int thickness, Color c);
  // A filled triangle pointing up (apex on top), base width w at the bottom of r.
  void triangle_up(Rect r, Color c);

  // FNV-1a over width, height and the R,G,B,A bytes of every pixel (endian independent).
  uint64_t hash() const;

 private:
  int w_ = 0, h_ = 0;
  std::vector<uint32_t> px_;
  std::vector<Rect> clips_;
  Rect clip_{};
};

}  // namespace x2::ui
