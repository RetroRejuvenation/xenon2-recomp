// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// --menu-script FRAME:KEY,KEY,...: a test aid for window runs. Once FRAME emulated frames have run, the keys are pressed one by one (one per
// loop iteration, down and up) as if typed, so a run can open the in-game menu, change things and close it again without anyone at the keyboard.
// The window loops call pump() once per iteration, with the number of frames run so far.
#pragma once
#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::ui {

class MenuScript {
 public:
  // False with a reason when a key name is unknown. An empty spec is valid and does nothing.
  bool parse(const std::string& spec, std::string& err);
  bool empty() const { return keys_.empty(); }
  // Pushes the next key as SDL events when `frames_run` has reached the start frame. Returns true when it pushed one.
  bool pump(uint32_t frames_run);
  bool done() const { return next_ >= keys_.size(); }

 private:
  uint32_t from_ = 0;
  std::vector<SDL_Scancode> keys_;
  size_t next_ = 0;
};

}  // namespace x2::ui
