// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "demo_scenes.h"

#include <cstring>

namespace x2::ui::demo {

namespace {

EventList nav_times(Nav n, int times) {
  EventList e;
  for (int i = 0; i < times; ++i) e.push_back(Event::nav_key(n));
  return e;
}

EventList ev_display_end() { return {Event::nav_key(Nav::Last)}; }
EventList ev_display_st() { return nav_times(Nav::Down, 7); }
EventList ev_controls_capture() {
  EventList e = nav_times(Nav::Down, 2);
  e.push_back(Event::nav_key(Nav::Accept));
  return e;
}
EventList ev_packs_buttons() {
  EventList e = nav_times(Nav::Down, 1);
  e.push_back(Event::nav_key(Nav::Accept));
  return e;
}
EventList ev_advanced_modal() {
  return {Event::nav_key(Nav::Right), Event::nav_key(Nav::Accept)};
}

void setup_st(DemoState& s) { s.game = Game::St; }
void setup_packs(DemoState& s) {
  s.pack_report = {{Severity::Info, "Checking pack..."}, {Severity::Ok, "Manifest is valid"},
                   {Severity::Warn, "1 screen has a different size from the original"}, {Severity::Info, "Entries 412, skipped 0"}};
}
void setup_log(DemoState& s) {
  s.log.push_back({Severity::Error, "Disk 1 was read but it has no copy-protection key track. Import an IPF or flux dump instead."});
  s.log.push_back({Severity::Ok, "Imported 1 file"});
}

}  // namespace

const std::vector<SceneSpec>& scenes() {
  static const std::vector<SceneSpec> v = {
      {"play", 0, nullptr, nullptr, false},
      {"display", 1, nullptr, nullptr, false},
      {"display_end", 1, nullptr, ev_display_end, false},
      {"display_st", 1, setup_st, ev_display_st, false},
      {"audio", 2, nullptr, nullptr, false},
      {"controls_capture", 3, nullptr, ev_controls_capture, false},
      {"packs", 4, setup_packs, ev_packs_buttons, false},
      {"advanced", 5, setup_log, nullptr, false},
      {"advanced_modal", 5, nullptr, ev_advanced_modal, false},
      {"about", 6, nullptr, nullptr, false},
      {"play_motion", 0, nullptr, nullptr, true},
      {"advanced_motion", 5, nullptr, nullptr, true},
  };
  return v;
}

const SceneSpec* find_scene(const char* name) {
  for (const SceneSpec& s : scenes())
    if (std::strcmp(s.name, name) == 0) return &s;
  return nullptr;
}

Canvas render_scene(const SceneSpec& spec, int w, int h, bool high_contrast) {
  Context ctx;
  ctx.theme = high_contrast ? Theme::high_contrast_theme() : Theme::standard();
  ctx.motion.reduced = !spec.full_motion;
  DemoState st;
  st.page = spec.page;
  if (spec.setup) spec.setup(st);
  Canvas canvas(w, h);
  auto builder = [&](Context& c) { build(c, st); };
  ctx.run(canvas, {}, 0, builder);
  if (spec.events) ctx.run(canvas, spec.events(), 0, builder);
  ctx.run(canvas, {}, spec.full_motion ? 2000 : 0, builder);
  return canvas;
}

}  // namespace x2::ui::demo
