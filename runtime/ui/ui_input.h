// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The kit's own input vocabulary. ui_sdl.h turns SDL events into these; tests build them directly.
#pragma once
#include <vector>

namespace x2::ui {

enum class Nav {
  None,
  Up,
  Down,
  Left,
  Right,
  Accept,        // Enter, Space, pad South
  Back,          // Esc, pad East
  PagePrev,      // PageUp, Shift+Tab, left shoulder
  PageNext,      // PageDown, Tab, right shoulder
  Menu,          // F1, pad Start
  Details,       // pad North, I: show the full help or command
  Default,       // Delete/Backspace, pad West: reset the focused row to its default
  First,         // Home
  Last,          // End
  ToggleFooter,  // pad Back/Select
};

enum class Device { Keyboard, Pad, Pointer };
enum class PadStyle { Generic, Xbox, PlayStation, Nintendo };

struct Event {
  enum class Type { NavKey, PointerMove, PointerDown, PointerUp, Scroll, RawKey } type = Type::NavKey;
  Nav nav = Nav::None;
  int x = 0, y = 0;  // pointer position in canvas pixels
  int dy = 0;        // Scroll: lines, positive = down
  int key = 0;       // RawKey: an opaque key code (SDL keycode) for key capture
  Device device = Device::Keyboard;

  static Event nav_key(Nav n, Device d = Device::Keyboard) {
    Event e;
    e.type = Type::NavKey;
    e.nav = n;
    e.device = d;
    return e;
  }
  static Event pointer(Type t, int x, int y) {
    Event e;
    e.type = t;
    e.x = x;
    e.y = y;
    e.device = Device::Pointer;
    return e;
  }
  static Event scroll(int lines, int x, int y) {
    Event e;
    e.type = Type::Scroll;
    e.dy = lines;
    e.x = x;
    e.y = y;
    e.device = Device::Pointer;
    return e;
  }
  static Event raw_key(int key) {
    Event e;
    e.type = Type::RawKey;
    e.key = key;
    return e;
  }
};
using EventList = std::vector<Event>;

// Turns a held direction (d-pad or stick) into events: one at once, then repeats after 400 ms every 80 ms.
class DirRepeater {
 public:
  static constexpr int kDelayMs = 400;
  static constexpr int kRepeatMs = 80;
  void update(Nav held, int dt_ms, EventList& out, Device device = Device::Pad);

 private:
  Nav held_ = Nav::None;
  int timer_ = 0;
};

}  // namespace x2::ui
