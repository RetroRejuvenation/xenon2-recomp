// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "launcher_state.h"

#include <fstream>
#include <sstream>
#include <utility>

#include "log_buffer.h"

namespace x2::ui {

LauncherState::LauncherState(LauncherEnv e) : env(std::move(e)) {
  if (!env.importer) env.importer = run_import;
  if (!env.settings_path.empty()) {
    std::string text, err;
    if (!load_settings_file(env.settings_path, model.s, text, err) && !err.empty())
      log.push_back({Severity::Warn, "settings.cfg: " + err}), log_warn("settings.cfg: " + err);
  }
  model.saved = model.s;
  model.packs = &packs;
  packs.packs_dir = env.data_dir + "packs";
  packs.dumps_dir = env.data_dir + "dumps";
  packs.exe_path = env.exe_path;
  packs.assets_dir = env.assets_dir;
  packs.data_dir = env.data_dir;
  packs.choose_pack = env.choose_pack;
  packs.open_folder = env.open_folder;
  model.test_tone = env.test_tone;
  adv.version = about.version = env.version;
  adv.host = about.host = env.host;
  adv.copy_text = env.copy_text;
  adv.choose_image = env.choose_image;
  adv.open_data_folder = about.open_data_folder = env.open_data_folder;
  adv.open_log = env.open_log;
  adv.export_settings = env.export_settings;
  adv.import_settings = env.import_settings;
  about.open_url = env.open_url;
  adv.reimport = [this] { open_import(); };
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
  if (!in_import && page == kPacksPage) {  // on the Asset packs page a drop is a pack, not a disk image
    for (const std::string& p : paths) {
      if (model.packs && packs.add_path(model.s, p)) save_settings();
    }
    return;
  }
  if (!in_import && page == kAdvancedPage) {  // on the Advanced page a drop is an image to check, not an import
    check_images(paths);
    return;
  }
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
  if (!o.headline.empty()) {
    const std::string line = "import: " + o.headline;
    if (o.kind == ImportOutcome::Kind::Failed) log_error(line);
    else if (o.kind == ImportOutcome::Kind::Imported) log_info(line);
    else log_warn(line);
  }
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
  for (Message& m : fresh) {
    if (m.severity != Severity::Info) LogBuffer::instance().add(m.severity, "import: " + m.text);
    log.push_back(std::move(m));
  }
  adv.poll();
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
  else {
    log.push_back({Severity::Warn, why});
    log_warn(why);
  }
}

void LauncherState::check_images(const std::vector<std::string>& paths) {
  if (paths.empty()) return;
  page = kAdvancedPage;
  if (adv.check_image(paths)) {
    adv.note = "Checking " + std::to_string(paths.size()) + (paths.size() == 1 ? " file." : " files.");
    adv.note_severity = Severity::Info;
    log_info("image check started for " + std::to_string(paths.size()) + (paths.size() == 1 ? " file" : " files"));
  } else {
    adv.note = "A check is already running.";
  }
}

void LauncherState::export_settings_to(const std::string& path) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (f) f << format_settings(model.s);
  if (f && f.good()) {
    adv.note = "Exported the settings.";
    adv.note_severity = Severity::Ok;
    log_info("settings exported");
  } else {
    adv.note = "Could not write that file.";
    adv.note_severity = Severity::Warn;
    log_warn("settings export failed");
  }
}

void LauncherState::import_settings_from(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    adv.note = "Could not read that file.";
    adv.note_severity = Severity::Warn;
    log_warn("settings import: cannot read the file");
    return;
  }
  std::stringstream text;
  text << f.rdbuf();
  Settings imported;
  std::string err;
  const bool ok = parse_settings(text.str(), imported, err);
  imported.last_game = model.s.last_game;  // which release was played last is this installation's, not the file's
  model.s = imported;
  adv.note = ok ? "Imported the settings." : "Imported the settings; some lines were not understood (see the log).";
  adv.note_severity = ok ? Severity::Ok : Severity::Warn;
  if (!ok) log_warn("settings import: " + err);
  else log_info("settings imported");
  save_settings();
}

void LauncherState::reset_settings() {
  const std::string last = model.s.last_game;
  model.s = Settings();
  model.s.last_game = last;
  adv.note = "All settings are back to their defaults.";
  adv.note_severity = Severity::Ok;
  log_info("settings reset to the defaults");
  save_settings();
}

}  // namespace x2::ui
