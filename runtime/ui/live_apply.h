// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Applying a settings change to a running game. The in-game menu edits a Settings; this turns "before" and "after" into the calls the hotkeys
// already make (the window loop supplies them as hooks), and reports the settings that only the next start can take. It knows no window and
// no machine, so the rule for each setting is tested without either. docs/re-notes/gui-ingame-menu.md.
#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "settings_schema.h"

namespace x2::ui {

// A hook that is empty means the running release cannot take that setting now: the change is reported as "next start".
struct LiveHooks {
  std::function<void(int)> scale;
  std::function<void(bool)> fullscreen;
  std::function<void(int)> volume;
  // Faithful switch, smooth frames and size, far layer, filter, aspect, whole pixels, curvature, asset packs, surround, HD panel.
  // `picture_keys` names the ones this release takes; the hook receives the whole Settings.
  std::function<void(const Settings&)> picture;
  std::set<std::string> picture_keys{"modern", "interp", "interp_scale", "smooth_far", "filter", "aspect", "integer", "curvature", "upgrade", "wide", "hd_panel"};
  std::function<void(bool on, int rate)> autofire;
  std::function<void(bool)> god_mode;
  std::function<void(int)> credits;
  std::function<void(bool)> cheat_scores;
  std::function<void(const std::string&)> music_source;
  std::function<void(int music, int sfx, int speech)> volumes;
  std::function<void(bool)> protect_music;
  std::function<void(const std::string&)> grab;
  std::function<void(const std::string&)> cursor;
  std::function<void(bool)> audio_upgrade;
  std::function<void(bool)> input_debug;
};

struct LiveResult {
  std::vector<std::string> applied;     // keys applied now
  std::vector<std::string> next_start;  // keys that changed but only the next start takes (labels are in the schema)
  bool any_change() const { return !applied.empty() || !next_start.empty(); }
  // "Applies at the next start: Output buffer, Output filter", or "" when everything took effect.
  std::string next_start_note() const;
};

// Compares the two settings row by row (schema order) and calls the hooks. Rows this release does not honour are left alone.
// The menu settings (ui_text, ui_contrast, ui_motion) are the menu's own and count as applied.
LiveResult apply_live(const Settings& before, const Settings& after, GameKind game, const LiveHooks& hooks);

// Copies into `file` the rows that differ between before and after (so a command-line override that was never saved stays out of the
// file, as with the hotkeys). Returns the keys copied.
std::vector<std::string> merge_changed(const Settings& before, const Settings& after, Settings& file);

}  // namespace x2::ui
