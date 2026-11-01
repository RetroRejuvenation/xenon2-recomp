// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The SDL3 side of the UI kit: turns SDL events into the kit's events and shows the canvas in a window.
// Nothing here knows about the emulator.
#pragma once
#include <SDL3/SDL.h>

#include "ui_core.h"

namespace x2::ui {

PadStyle pad_style_from_type(SDL_GamepadType t);

// Event translation. Pointer coordinates in the SDL events must already be in renderer output pixels (call
// SDL_ConvertEventToRenderCoordinates first); they are mapped to canvas pixels with the current geometry.
// Feed the context one SDL event at a time (run() per event) and mirror ctx.capturing() into set_capture()
// between events: during a key capture a key press becomes a RawKey instead of a navigation event.
class SdlInput {
 public:
  void set_geometry(const CanvasGeometry& g) { geo_ = g; }
  void set_capture(bool on) { capture_ = on; }
  PadStyle pad_style() const { return pad_style_; }
  void handle(const SDL_Event& e, EventList& out);
  // Call once per loop iteration with the elapsed milliseconds: repeats a held d-pad direction or stick.
  void tick(int dt_ms, EventList& out);

 private:
  void update_held(EventList& out, int dt_ms);
  CanvasGeometry geo_{};
  bool capture_ = false;
  PadStyle pad_style_ = PadStyle::Generic;
  Nav dpad_ = Nav::None, stick_ = Nav::None;
  int ax_ = 0, ay_ = 0;  // left stick axes
  DirRepeater repeat_;
};

// Shows a canvas in an SDL renderer: a streaming texture scaled by whole numbers with nearest-neighbour
// filtering, centred, with `letterbox` around it. Does not call SDL_RenderPresent.
class SdlPresenter {
 public:
  SdlPresenter() = default;
  SdlPresenter(const SdlPresenter&) = delete;
  SdlPresenter& operator=(const SdlPresenter&) = delete;
  ~SdlPresenter();
  bool draw(SDL_Renderer* r, const Canvas& c, const CanvasGeometry& g, Color letterbox);

 private:
  SDL_Texture* tex_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  int tw_ = 0, th_ = 0;
};

}  // namespace x2::ui
