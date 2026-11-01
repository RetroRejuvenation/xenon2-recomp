// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "test_tone.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace x2::ui {

std::vector<int16_t> make_test_chirp(int rate, int ms, int volume_percent) {
  std::vector<int16_t> out;
  if (rate <= 0 || ms <= 0) return out;
  const int frames = static_cast<int>(static_cast<int64_t>(rate) * ms / 1000);
  const int fade = std::max(1, rate / 100);  // 10 ms
  const double amp = 16384.0 * std::clamp(volume_percent, 0, 100) / 100.0;
  const double pi = 3.14159265358979323846;
  out.resize(static_cast<size_t>(frames) * 2);
  double phase = 0;
  for (int i = 0; i < frames; ++i) {
    const double progress = static_cast<double>(i) / frames;
    const double hz = 440.0 + 440.0 * progress;
    phase += 2.0 * pi * hz / rate;
    if (phase > 2.0 * pi) phase -= 2.0 * pi;
    double env = 1.0;
    if (i < fade) env = static_cast<double>(i) / fade;
    else if (i >= frames - fade) env = static_cast<double>(frames - 1 - i) / fade;
    const int16_t v = static_cast<int16_t>(std::lround(std::sin(phase) * amp * env));
    out[static_cast<size_t>(i) * 2] = v;
    out[static_cast<size_t>(i) * 2 + 1] = v;
  }
  return out;
}

}  // namespace x2::ui
