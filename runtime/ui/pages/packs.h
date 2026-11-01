// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Asset packs page: the packs found in the data folder's packs/ folder and the ones added by path, enabled or not, in load order;
// what each pack says about itself; the checker of `--check-asset-pack` on a worker thread; and "Dump my assets" for pack authors. It edits
// `SettingsModel::s` (packs_enabled, asset_packs, upgrade, audio_upgrade) and leaves saving to the host. docs/re-notes/gui-asset-packs-page.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../pack_jobs.h"
#include "pages.h"
#include "pack/pack_api.h"

namespace x2::ui {

// One line of the pack list.
struct PackRow {
  enum class Kind {
    Folder,   // found in the packs folder; loaded when enabled (its id is in packs_enabled)
    Path,     // added by path (asset_packs): loaded after the enabled ones for as long as it is listed
    Missing,  // an id in packs_enabled that is not in the packs folder any more
  };
  Kind kind = Kind::Folder;
  std::string id;    // Folder, Missing
  std::string path;  // Folder, Path
  std::string name;  // what the list shows
  pack::PackInfo info;
  bool inspected = false;  // info holds what the manifest says (false: the path is not a pack, or does not exist)
  bool enabled = false;
  bool applies = true;     // the pack's games/builds include the release being described
  int order = 0;           // 1-based position in the load order, 0 when it is not loaded
};

// What the last check of one pack found.
struct PackCheckView {
  bool against = false;  // it was compared with a dump
  int exit_code = 0;
  int errors = 0, warnings = 0;
  size_t entries = 0, skipped = 0;
  std::string coverage;  // "coverage of the dump: ..." when compared
  std::vector<Message> messages;
};

class PacksModel {
 public:
  // ---- set by the host ----
  std::string packs_dir;   // <data dir>/packs
  std::string dumps_dir;   // <data dir>/dumps
  std::string exe_path;    // this program, for the dump (a headless run of it)
  std::string assets_dir;  // --assets for the dump ("" = the program's default)
  std::string data_dir;    // --data-dir for the dump ("" = the default)
  std::string build;       // the importer's build tag of the release being described ("" = canonical)
  // The native file dialog: returns a pack folder or .zip, or "" when cancelled. Without it the page offers drag-and-drop only.
  std::function<std::string()> choose_pack;
  std::function<void(const std::string& folder)> open_folder;  // opens a folder in the system's file manager
  DumpRunner dump_runner;                                       // tests replace it; empty means run_dump_process
  uint32_t dump_frames = 6000;

  // ---- what the page edits and shows ----
  int selected = 0;
  bool strict = false;        // the check counts warnings as failures (--strict)
  bool against_dump = false;  // the check also compares with the newest dump (--against)
  std::vector<Message> notices;   // add, remove and enable results, shown under the buttons
  std::vector<Message> dump_log;  // the last dump's outcome

  // Reads the packs folder and the manifests of the packs added by path. The page does it when it is first shown and on "Rescan".
  void rescan();
  bool scanned() const { return scanned_; }
  // Rebuilds the rows from the settings when they, the release or the last scan changed. Cheap otherwise.
  void sync(const SettingsModel& m);
  const std::vector<PackRow>& rows() const { return rows_; }
  const PackRow* current() const;

  // ---- changing the settings (each returns true when `s` changed; `notices` says what happened) ----
  bool toggle(Settings& s, int row);             // enable or disable a pack of the packs folder; forget a missing id
  bool move(Settings& s, int row, int delta);    // -1 earlier, +1 later, inside its group (packs folder, then added paths)
  bool remove(Settings& s, int row);             // take an added path off the list
  bool add_path(Settings& s, const std::string& path);  // a dropped or chosen pack: enabled if it is in the packs folder, else added

  // ---- jobs ----
  PackJobs& jobs() { return jobs_; }
  bool busy() const { return jobs_.busy(); }
  bool start_check(const SettingsModel& m, int row);
  bool start_dump(const SettingsModel& m);
  void cancel() { jobs_.cancel(); }
  // Collects finished jobs. The page calls it every pass.
  void poll();
  // The text of the status column and the newest dump folder.
  std::string status_of(const PackRow& r, GameKind game) const;
  const PackCheckView* check_view(const std::string& path) const;
  std::string latest_dump(GameKind game) const;

 private:
  void apply_check(const CheckDone& d);
  void apply_dump(const DumpOutcome& o);
  void note(Severity sev, std::string text);

  std::vector<pack::PackInfo> discovered_;
  std::map<std::string, pack::PackInfo> by_path_;  // manifests of the packs added by path
  std::map<std::string, bool> by_path_ok_;
  std::vector<PackRow> rows_;
  std::string sync_key_;
  std::string follow_;  // the id or path of the pack the selection stays on after the next rebuild
  unsigned scans_ = 0;
  bool scanned_ = false;
  std::map<std::string, PackCheckView> checks_;
  PackJobs jobs_;
};

// The page. True when a setting changed in this pass (the host saves it). The host keeps redrawing while `pm.busy()`.
bool page_packs(Context& ctx, SettingsModel& m, PacksModel& pm, Rect body, Rect help);

}  // namespace x2::ui
