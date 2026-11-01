// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "live_apply.h"

#include <cstring>

namespace x2::ui {

std::string LiveResult::next_start_note() const {
  if (next_start.empty()) return {};
  std::string s = "Applies at the next start: ";
  for (size_t i = 0; i < next_start.size(); ++i) {
    const SettingRow* r = find_row(next_start[i]);
    s += i ? ", " : "";
    s += r ? r->label : next_start[i];
  }
  return s;
}

namespace {

bool is_picture_key(const std::string& k) {
  static const char* const keys[] = {"modern", "interp", "interp_scale", "smooth_far", "filter", "aspect", "integer", "curvature", "upgrade", "wide", "hd_panel"};
  for (const char* p : keys)
    if (k == p) return true;
  return false;
}

}  // namespace

LiveResult apply_live(const Settings& before, const Settings& after, GameKind game, const LiveHooks& h) {
  LiveResult res;
  bool picture_changed = false, autofire_changed = false, volumes_changed = false;
  auto done = [&](const SettingRow& r, bool ok) { (ok ? res.applied : res.next_start).push_back(r.key); };
  for (const SettingRow& r : schema()) {
    if (!applies_to(r, game)) continue;
    if (value_text(r, before) == value_text(r, after)) continue;
    const std::string k = r.key;
    if (k == "ui_text" || k == "ui_contrast" || k == "ui_motion") { done(r, true); continue; }
    if (k == "scale") { if (h.scale) h.scale(after.scale); done(r, bool(h.scale)); continue; }
    if (k == "fullscreen") { if (h.fullscreen) h.fullscreen(after.fullscreen); done(r, bool(h.fullscreen)); continue; }
    if (k == "volume") { if (h.volume) h.volume(after.volume); done(r, bool(h.volume)); continue; }
    if (is_picture_key(k)) {
      const bool ok = h.picture && h.picture_keys.count(k) != 0;
      picture_changed |= ok;
      done(r, ok);
      continue;
    }
    if (k == "autofire" || k == "autofire_rate") { autofire_changed |= bool(h.autofire); done(r, bool(h.autofire)); continue; }
    if (k == "god_mode") { if (h.god_mode) h.god_mode(after.god_mode); done(r, bool(h.god_mode)); continue; }
    if (k == "credits") { if (h.credits) h.credits(after.credits); done(r, bool(h.credits)); continue; }
    if (k == "cheat_scores") { if (h.cheat_scores) h.cheat_scores(after.cheat_scores); done(r, bool(h.cheat_scores)); continue; }
    if (k == "music") { if (h.music_source) h.music_source(after.music); done(r, bool(h.music_source)); continue; }
    if (k == "music_volume" || k == "sfx_volume" || k == "speech_volume") { volumes_changed |= bool(h.volumes); done(r, bool(h.volumes)); continue; }
    if (k == "protect_music") { if (h.protect_music) h.protect_music(after.protect_music); done(r, bool(h.protect_music)); continue; }
    if (k == "grab_keyboard") { if (h.grab) h.grab(after.grab_keyboard); done(r, bool(h.grab)); continue; }
    if (k == "cursor") { if (h.cursor) h.cursor(after.cursor); done(r, bool(h.cursor)); continue; }
    if (k == "audio_upgrade") { if (h.audio_upgrade) h.audio_upgrade(after.audio_upgrade); done(r, bool(h.audio_upgrade)); continue; }
    if (k == "input_debug") { if (h.input_debug) h.input_debug(after.input_debug); done(r, bool(h.input_debug)); continue; }
    done(r, false);  // everything else is read when the game starts
  }
  if (picture_changed) h.picture(after);
  if (autofire_changed) h.autofire(after.autofire, after.autofire_rate);
  if (volumes_changed) h.volumes(after.music_volume, after.sfx_volume, after.speech_volume);
  return res;
}

std::vector<std::string> merge_changed(const Settings& before, const Settings& after, Settings& file) {
  std::vector<std::string> keys;
  for (const SettingRow& r : schema()) {
    const std::string now = value_text(r, after);
    if (value_text(r, before) == now) continue;
    std::string err;
    if (set_value_text(r, file, now, err)) keys.push_back(r.key);
  }
  return keys;
}

}  // namespace x2::ui
