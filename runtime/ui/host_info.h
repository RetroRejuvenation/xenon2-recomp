// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// What only a window host can say about the machine, as plain text for About and the diagnostics. No dependencies.
#pragma once
#include <string>

namespace x2::ui {

// What only the window host can say (SDL's version and drivers); the pages and the tests fill it with plain text.
struct HostInfo {
  std::string platform;       // "macOS arm64"
  std::string sdl_version;    // "3.4.18"
  std::string video_driver;   // "cocoa"
  std::string audio_driver;   // "coreaudio"
  std::string display;        // "60 Hz" or ""
  std::string build_id;       // the compiler and build type, "" when unknown
};

}  // namespace x2::ui
