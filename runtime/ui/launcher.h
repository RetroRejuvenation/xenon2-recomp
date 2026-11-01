// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The launcher window: SDL3 host of the pages in pages/launcher_pages.cpp. It opens its own window (the game opens its own when it
// starts), runs until the user plays a release or quits, and returns which release to start. Needs SDL3; headless builds have none.
#pragma once
#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

#include "launcher_state.h"

namespace x2::ui {

struct LaunchResult {
  bool start = false;
  std::string game;  // "disk", "cdtv" or "st" when start is true
};

// The tests drive the real window loop through this (the SDL dummy video driver): `per_frame` is called once per loop iteration, may
// push events (key presses, drops) and read the state, and returns true to stop the loop. Without hooks the loop waits for events.
struct LaunchHooks {
  std::function<bool(int frame, LauncherState& st, std::vector<SDL_Event>& inject)> per_frame;
  int window_w = 960, window_h = 540;
};

LaunchResult run_launcher(LauncherEnv env, const LaunchHooks* hooks = nullptr);

}  // namespace x2::ui
