// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Advanced page of the launcher: what each import recorded (variant, importer version, protection method, receipt), the opt-in
// protection patch explained (it can only be applied while importing), diagnostics (copy a bug report, check an image, open the data folder
// and the log), the log itself, the settings file (export, import, reset) and the tools that only the command line has. No SDL: the window
// host supplies the dialogs and the clipboard through the hooks. docs/re-notes/gui-advanced-integration.md.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../diagnostics.h"
#include "../worker.h"
#include "pages.h"

namespace x2::ui {

class AdvancedModel {
 public:
  // ---- set by the host (a missing hook greys the button out) ----
  std::string version;
  HostInfo host;
  std::function<void(const std::string&)> copy_text;
  std::function<void()> choose_image;       // the file dialog of "Check an image..."; the pick comes back through check_image()
  std::function<void()> open_data_folder;
  std::function<void()> open_log;           // shows x2recomp.log
  std::function<void()> export_settings;    // a save dialog; the pick comes back through LauncherState::export_settings_to()
  std::function<void()> import_settings;    // an open dialog; the pick comes back through LauncherState::import_settings_from()
  std::function<void()> reimport;           // the launcher's import flow

  // ---- what the page shows ----
  std::vector<Message> report;  // the last image check
  std::string report_title;
  std::string note;             // what the last button did ("Copied the diagnostics.")
  Severity note_severity = Severity::Info;
  bool reset_requested = false;  // set by the page when the reset was confirmed; the host applies it (LauncherState::reset_settings)

  // Checks the images on a worker thread (`x2recomp --diagnose-image` as a report). False while a check is running.
  bool check_image(const std::vector<std::string>& paths);
  // Once per loop iteration: collects the worker's messages and result.
  void poll();
  bool busy() const { return worker_.busy(); }
  const JobView& job() const { return job_; }
  void cancel() { worker_.cancel(); }

  // The text of "Copy diagnostics" for these releases and settings.
  std::string diagnostics(const Library& lib, const Settings& s, GameKind game) const;

  uint64_t log_seen = 0;           // the log generation the page last drew
  std::vector<Message> log_view;   // the lines it drew

 private:
  Worker worker_;
  JobView job_;
};

// The page. True when a setting changed in this pass (the input log switch): the host saves it.
bool page_advanced(Context& ctx, SettingsModel& m, AdvancedModel& adv, const Library& lib, Rect body, Rect help);

// The page's dialogs (the patch disclaimer, the reset confirmation). Build them last in the frame, after the footer, so they are drawn on top.
// Sets `adv.reset_requested` when the reset was confirmed.
void advanced_modals(Context& ctx, AdvancedModel& adv);

// The commands that only the command line has, as the page lists them (also used by the docs test).
struct CliTool {
  const char* what;
  const char* command;
};
const std::vector<CliTool>& command_line_tools();

}  // namespace x2::ui
