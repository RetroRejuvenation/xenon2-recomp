// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Audio page's test tone: a short generated chirp, so the volume can be set before a game starts. Pure samples, no SDL and nothing from
// the game; the window hosts play them (host_actions.h).
#pragma once
#include <cstdint>
#include <vector>

namespace x2::ui {

// Interleaved stereo 16-bit samples: a sine sweeping from 440 Hz to 880 Hz over `ms`, with 10 ms fades at both ends, at `volume_percent`
// of a comfortable level (half of full scale at 100). Deterministic.
std::vector<int16_t> make_test_chirp(int rate, int ms, int volume_percent);

}  // namespace x2::ui
