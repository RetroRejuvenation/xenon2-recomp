// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The settings pages (Play, Display, Audio, and the rows of Controls, Packs and Advanced for the workstreams that build those pages):
// every row comes from the schema (settings_schema.h), so the labels, ranges, badges, greyed reasons and the equivalent command line
// cannot disagree with settings.cfg or with the command line. A page edits `SettingsModel::s`; the host decides when to save it.
#pragma once
#include <functional>
#include <string>

#include "../settings_schema.h"
#include "../ui_widgets.h"

namespace x2::ui {

class PacksModel;  // pages/packs.h

struct SettingsModel {
  Settings s;                       // being edited
  Settings saved;                   // as it is in settings.cfg now
  GameKind game = GameKind::Disk;   // the release the pages describe (greys out what it does not use)
  bool show_expert = false;         // the "Expert" disclosure
  // The key name for an input.cfg action ("filter" -> "F2"), or "". The pages show it beside the row; without one nothing is shown.
  std::function<std::string(const std::string& action)> hotkey;
  std::function<void()> test_tone;  // plays a short tone (the Audio page offers the button when set)
  std::string copied;               // the last command line the Copy button was pressed for
  PacksModel* packs = nullptr;      // the Asset packs page's state (pages/packs.h); without it that page is a placeholder

  bool dirty() const;
  // The command that starts the release with these settings on a clean file. Cached until the settings change.
  const CommandLine& command(bool with_game = true);

 private:
  std::string cache_key_;
  bool cache_with_game_ = true;
  GameKind cache_game_ = GameKind::Disk;
  CommandLine cache_;
  bool cache_valid_ = false;
};

enum PageIndex { kPlayPage = 0, kDisplayPage, kAudioPage, kControlsPage, kPacksPage, kAdvancedPage, kAboutPage, kPageCount };
const std::vector<PageItem>& page_items();

// The rows of `page` in groups, one stack. Returns true when a value changed in this pass (the model is already updated).
bool settings_rows(Context& ctx, SettingsModel& m, PageBit page, Stack& st);

// Whole pages: heading, scrolling rows, help line. `help` is the one-line area under the page.
bool page_play(Context& ctx, SettingsModel& m, Rect body, Rect help);
bool page_display(Context& ctx, SettingsModel& m, Rect body, Rect help);
bool page_audio(Context& ctx, SettingsModel& m, Rect body, Rect help);

// The "Equivalent command line" footer. True when Copy was pressed (m.copied is set).
bool settings_footer(Context& ctx, SettingsModel& m, Rect r);

// A whole frame: the page selector, the current page, help line, the command footer and the hint bar. Pages that later workstreams
// build (Controls, Packs, Advanced, About) show a short placeholder. Returns true when a setting changed in this pass.
bool settings_frame(Context& ctx, SettingsModel& m, int& page);

}  // namespace x2::ui
