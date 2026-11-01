// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The launcher's model: the library of imported releases, the settings being edited, the import flow and what the user decided (play,
// quit). No SDL and no drawing: the frame (pages/launcher_pages.cpp) reads and changes it, the window host (launcher.cpp) feeds it
// drops and file-dialog results, and the tests drive it directly with a fake importer.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "import_job.h"
#include "library.h"
#include "pages/about.h"
#include "pages/advanced.h"
#include "pages/packs.h"
#include "pages/pages.h"
#include "worker.h"

namespace x2::ui {

struct LauncherEnv {
  std::string data_dir;        // the per-user data directory, with a trailing separator
  std::string settings_path;   // settings.cfg ("" keeps the settings in memory only: the tests)
  std::string version;         // "0.5.0"
  std::string args_note;       // the arguments this run was started with, shown when they override the stored settings (--gui)
  std::string exe_path;        // this program, for the Asset packs page's "Dump my assets" (a headless run of it)
  std::string assets_dir;      // --assets of this run for that dump ("" = the program's default)
  std::string initial_game;    // --game, "disk", "cdtv" or "st": the card to focus first ("" = the one played last)
  bool first_run = false;      // nothing imported: open on the import view
  std::string first_run_reason; // why there is nothing usable (shown on the import view)
  // The window host sets these (native dialogs, the clipboard, the file manager); nothing happens when one is empty.
  std::function<void()> choose_files, choose_folder, open_data_folder;
  std::function<void(const std::string&)> copy_text;
  std::function<std::string()> choose_pack;                     // the Asset packs page's "Add a pack..." (returns "" at once; the pick comes back as a drop)
  std::function<void(const std::string&)> open_folder;
  std::function<void(const std::string&)> open_url;
  std::function<void()> open_log, choose_image, export_settings, import_settings;  // the log file, the image check's file dialog, the settings file's dialogs
  std::function<void()> test_tone;                                                // the Audio page's "Play test tone"
  HostInfo host;                                                                  // SDL's version and drivers, for About and the diagnostics
  // Defaults to run_import; the tests give a fake that posts progress and waits.
  std::function<ImportOutcome(const ImportRequest&, JobContext&)> importer;
};

class LauncherState {
 public:
  explicit LauncherState(LauncherEnv env);

  LauncherEnv env;
  SettingsModel model;
  PacksModel packs;  // the Asset packs page; model.packs points at it
  AdvancedModel adv;  // the Advanced page
  AboutInfo about;    // the About page
  // The Controls page needs SDL's key and button names, so the window host owns it and draws it through these (empty: a placeholder shows).
  std::function<bool(Context&, SettingsModel&, Rect body, Rect help)> controls_page;
  std::function<void(Context&)> controls_modal;  // the confirmation about a taken key; built last in the frame
  Library library;
  int page = kPlayPage;
  GameKind selected = GameKind::Disk;  // the focused card; the other pages describe this release

  // The import view (what the first-run window was): the files given so far, the outcome in one sentence, the job's messages.
  bool in_import = false;
  std::vector<std::string> dropped;
  std::vector<Message> log;
  std::string headline;
  Severity headline_severity = Severity::Info;
  JobView job;
  bool patch_offer = false;     // the dropped disk 1 is a retail disk without its key track: the opt-in patch can be offered
  bool patch_owned = false;     // the "I own the original disc" box of the patch dialog (never remembered)
  std::string note;             // one line on the home page ("Imported: ...")

  // What the user decided.
  bool start = false;
  GameKind start_game = GameKind::Disk;
  bool quit = false;

  // ---- actions (from the frame, the window host and the tests) ----
  void open_import();
  void close_import();                         // back to the library; with nothing imported this quits instead
  void drop(const std::vector<std::string>& paths);  // adds the paths and imports (or says to wait)
  void forget_dropped();
  void import_with_patch();                    // only after the disclaimer and the ownership box: ImportRequest carries both flags
  void cancel_import();
  void play(GameKind g);
  void rescan() { library = scan_library(env.data_dir); }
  // The Advanced page's files: an image to check (also a drop on that page), the settings file written to or read from a chosen path, and the
  // reset the page confirmed. Each says what it did on the page and in the log.
  void check_images(const std::vector<std::string>& paths);
  void export_settings_to(const std::string& path);
  void import_settings_from(const std::string& path);
  void reset_settings();
  const char* back_label() const { return library.any() ? "Back" : "Quit"; }

  // Once per loop iteration, before the frame: moves job messages and the outcome into the model.
  void poll();
  bool busy() const { return worker_.busy(); }
  bool animating() const { return worker_.busy() || packs.busy() || adv.busy(); }  // the host keeps redrawing while true
  // Writes settings.cfg when the settings changed (merging into what is there); a failure becomes a warning in the log.
  void save_settings();
  bool settings_dirty() const { return model.dirty(); }

 private:
  void start_import(bool patch);
  void apply_outcome(const ImportOutcome& o);
  Worker worker_;
  ImportOutcome outcome_;
};

}  // namespace x2::ui
