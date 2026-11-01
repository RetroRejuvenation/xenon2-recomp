// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Fixed scenes of the demo pages for the golden tests, the gallery's --shot option and (later) the documentation
// screenshots: each is a page, a model state and a few scripted input events, rendered into a canvas.
#pragma once
#include <vector>

#include "demo_pages.h"
#include "../ui_png.h"

namespace x2::ui::demo {

struct SceneSpec {
  const char* name;
  int page;
  void (*setup)(DemoState&);   // may be null
  EventList (*events)();       // may be null
  bool full_motion;            // otherwise reduced motion (no starfield, no stripes, no slides)
};

const std::vector<SceneSpec>& scenes();
const SceneSpec* find_scene(const char* name);
// Renders a scene at a canvas size and theme. Deterministic: integer-only drawing, fixed clock.
Canvas render_scene(const SceneSpec& s, int w, int h, bool high_contrast);

}  // namespace x2::ui::demo
