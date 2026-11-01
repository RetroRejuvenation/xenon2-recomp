// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Demonstration pages for the widget kit (the gallery and the golden tests). They run on a mock model with fake
// data: no settings file, no importer, no game data. The real pages replace them in later workstreams; the Display
// page is the proving page for the kit (docs/re-notes/gui-widget-kit.md).
#pragma once
#include <string>
#include <vector>

#include "../ui_widgets.h"

namespace x2::ui::demo {

enum class Game { Disk = 0, Cdtv = 1, St = 2 };

struct DisplayModel {
  bool faithful = false;
  int size = 3;
  bool fullscreen = false;
  int filter = 3;  // nearest, sharp, scanlines, crt, edge
  bool curvature = false;
  int aspect = 0;  // 4:3 corrected, square pixels
  bool integer = true;
  int smooth = 2;  // off, 60, 120
  int interp_size = 4;
  bool smooth_far = true;
  int wide = 0;  // off, 16:10, 16:9
  bool hd_panel = false;
  bool packs = true;
};

struct DemoState {
  int page = 0;
  Game game = Game::Disk;
  DisplayModel display;
  // Audio
  bool sound = true;
  int master = 80, sfx = 100, music = 70, speech = 100;
  int audio_filter = 0;
  bool pack_audio = true;
  // Controls
  std::vector<std::string> keys = {"Up", "Down", "Left", "Right", "Space", "C"};
  int pad_fire = 0;
  // Packs
  int pack_selected = 0;
  std::vector<bool> pack_on = {true, true, false};
  std::vector<Message> pack_report;
  // Advanced
  int progress_permille = 640;
  std::vector<Message> log;
  int list_sel = 0;
  bool start_directly = false;
  bool launcher_first_run = false;
  std::string copied;  // the last text "copied" by the Copy button
  bool quit = false;
  DemoState();
};

const std::vector<PageItem>& pages();
// One whole frame: page selector, the current page, help line, command footer and hint bar.
void build(Context& ctx, DemoState& st);
// The equivalent command line the Display page shows for the model.
std::string display_command(const DemoState& st);

}  // namespace x2::ui::demo
