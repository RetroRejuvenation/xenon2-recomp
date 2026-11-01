// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "launcher_state.h"

#include <utility>

namespace x2::ui {

LauncherState::LauncherState(LauncherEnv e) : env(std::move(e)) {
  if (!env.importer) env.importer = run_import;
  if (!env.settings_path.empty()) {
    std::string text, err;
    if (!load_settings_file(env.settings_path, model.s, text, err) && !err.empty())
      log.push_back({Severity::Warn, "settings.cfg: " + err});
  }
  model.saved = model.s;
  library = scan_library(env.data_dir);
  GameKind g;
  if (parse_game_kind(env.initial_game, g) || parse_game_kind(model.s.last_game, g)) selected = g;
  else selected = library.first_present();
  if (!library.get(selected).present && library.any() && !parse_game_kind(env.initial_game, g)) selected = library.first_present();
  model.game = selected;
  in_import = env.first_run || !library.any();
  if (in_import && !env.first_run_reason.empty()) headline = "Nothing is imported yet (" + env.first_run_reason + ").";
}

void LauncherState::open_import() {
  if (worker_.busy()) return;
  in_import = true;
  patch_offer = false;
  patch_owned = false;
}

void LauncherState::close_import() {
  if (worker_.busy()) return;
  if (!library.any()) {
    quit = true;
    return;
  }
  in_import = false;
  dropped.clear();
  patch_offer = false;
  patch_owned = false;
}

void LauncherState::forget_dropped() {
  if (worker_.busy()) return;
  dropped.clear();
  log.clear();
  headline.clear();
  patch_offer = false;
  patch_owned = false;
}

void LauncherState::drop(const std::vector<std::string>& paths) {
  if (paths.empty()) return;
  if (worker_.busy()) {
    log.push_back({Severity::Warn, "An import is running. Give the files again when it has finished."});
    return;
  }
  in_import = true;
  for (const std::string& p : paths) dropped.push_back(p);
  start_import(false);
}

void LauncherState::import_with_patch() {
  if (worker_.busy() || dropped.empty() || !patch_owned) return;  // the ownership confirmation is part of the request
  start_import(true);
}

void LauncherState::start_import(bool patch) {
  ImportRequest req;
  req.paths = dropped;
  req.data_dir = env.data_dir;
  req.apply_protection_patch = patch;
  req.confirmed_ownership = patch && patch_owned;
  if (patch) patch_owned = false;  // each patch run needs its own confirmation
  log.clear();
  headline.clear();
  patch_offer = false;
  job = JobView{};
  job.running = true;
  auto fn = env.importer;
  worker_.start([this, req, fn](JobContext& ctx) { outcome_ = fn(req, ctx); });
}

void LauncherState::cancel_import() {
  if (worker_.running()) worker_.cancel();
}

void LauncherState::apply_outcome(const ImportOutcome& o) {
  headline = o.headline;
  switch (o.kind) {
    case ImportOutcome::Kind::Imported:
      headline_severity = Severity::Ok;
      rescan();
      selected = o.game;
      model.game = selected;
      dropped.clear();
      patch_offer = false;
      patch_owned = false;
      in_import = false;
      note = o.headline;
      break;
    case ImportOutcome::Kind::Failed:
      headline_severity = Severity::Error;
      dropped.clear();
      break;
    case ImportOutcome::Kind::NeedMore:
      headline_severity = Severity::Warn;  // the dropped files stay
      break;
    case ImportOutcome::Kind::PatchOffer:
      headline_severity = Severity::Warn;
      patch_offer = true;  // the dropped files stay
      break;
    case ImportOutcome::Kind::Cancelled:
      headline_severity = Severity::Info;
      dropped.clear();
      break;
  }
}

void LauncherState::poll() {
  std::vector<Message> fresh;
  job = worker_.poll(fresh);
  for (Message& m : fresh) log.push_back(std::move(m));
  if (worker_.collect()) {
    job.running = false;
    apply_outcome(outcome_);
  }
}

void LauncherState::play(GameKind g) {
  selected = g;
  model.game = g;
  model.s.last_game = game_flag_value(g);
  save_settings();
  start_game = g;
  start = true;
}

void LauncherState::save_settings() {
  if (env.settings_path.empty() || !model.dirty()) return;
  std::string why;
  if (save_settings_file(env.settings_path, model.s, why)) model.saved = model.s;
  else log.push_back({Severity::Warn, why});
}

}  // namespace x2::ui
