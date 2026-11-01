// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "ui_input.h"

namespace x2::ui {

void DirRepeater::update(Nav held, int dt_ms, EventList& out, Device device) {
  if (held == Nav::None) {
    held_ = Nav::None;
    timer_ = 0;
    return;
  }
  if (held != held_) {
    held_ = held;
    timer_ = kDelayMs;
    out.push_back(Event::nav_key(held, device));
    return;
  }
  timer_ -= dt_ms;
  while (timer_ <= 0) {
    out.push_back(Event::nav_key(held, device));
    timer_ += kRepeatMs;
  }
}

}  // namespace x2::ui
