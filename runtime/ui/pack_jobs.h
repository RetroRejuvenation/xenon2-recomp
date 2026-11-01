// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The two jobs of the asset packs page that take long enough to need a thread: checking a pack (the library behind
// `--check-asset-pack`) and dumping the game's graphics and sounds for pack authors (a headless run of this same program with
// `--dump-assets`). One job runs at a time; the page polls, never waits. See docs/re-notes/gui-asset-packs-page.md.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pack/pack_api.h"
#include "pack/pack_check.h"

namespace x2::ui {

// ---- the dump ----

struct DumpRequest {
  std::string exe;         // the x2recomp executable to run
  std::string game;        // "disk", "cdtv" or "st"
  std::string assets_dir;  // --assets, "" for the program's own default
  std::string data_dir;    // --data-dir, "" for the default
  std::string out_dir;     // the dump folder to create; it must not exist
  uint32_t frames = 6000;  // how long the headless run lasts (a frame is 1/50 s of game time)
  bool audio = true;       // also --dump-audio (the Atari ST release has no sampled sound, so it is never asked there)
};

struct DumpOutcome {
  bool ok = false;
  bool cancelled = false;
  std::string error;               // one plain sentence when !ok and not cancelled
  std::string out_dir;             // the finished dump folder when ok
  std::vector<std::string> lines;  // what the run printed that is worth showing (the "dump-assets:" summary and warnings)
};

using DumpProgressFn = std::function<void(int permille, const std::string& stage)>;
// Runs a dump. `cancel` is polled; a cancelled run leaves no folder behind. The default is run_dump_process.
using DumpRunner = std::function<DumpOutcome(const DumpRequest&, const DumpProgressFn&, const std::atomic<bool>& cancel)>;

// The arguments (without the program) of the headless run for `r`, writing into `into_dir`.
std::vector<std::string> dump_command_args(const DumpRequest& r, const std::string& into_dir);
// Starts r.exe with those arguments (into a folder next to r.out_dir, renamed on success), turns its frame reports into progress, and
// reads its exit status. Works without a console on every platform.
DumpOutcome run_dump_process(const DumpRequest& r, const DumpProgressFn& progress, const std::atomic<bool>& cancel);

// <dumps_root>/<game> if it does not exist, else <game>-2, <game>-3 ...: a dump never overwrites another one.
std::string next_dump_dir(const std::string& dumps_root, const std::string& game);
// The newest finished dump of `game` (the folder with the highest number that holds a manifest.json), or "".
std::string latest_dump_dir(const std::string& dumps_root, const std::string& game);

// ---- the job runner ----

struct CheckDone {
  std::string path;  // the pack that was checked
  bool against = false;
  pack::CheckResult result;
};

class PackJobs {
 public:
  PackJobs() = default;
  PackJobs(const PackJobs&) = delete;
  PackJobs& operator=(const PackJobs&) = delete;
  ~PackJobs();

  // True from start_* until the result has been taken with take_*. While true the host should keep redrawing about ten times a
  // second (a finished job has nothing to show until the page polls it).
  bool busy() const { return busy_.load(); }
  // Which job runs, its progress (0 to 1000) and what it is doing.
  bool checking() const { return busy() && kind_ == Kind::Check; }
  bool dumping() const { return busy() && kind_ == Kind::Dump; }
  int permille() const { return permille_.load(); }
  std::string stage() const;

  // False when a job is already running.
  bool start_check(const pack::CheckOptions& o);
  bool start_dump(const DumpRequest& r, DumpRunner runner = run_dump_process);
  // Asks a running dump to stop (its run is killed); a check cannot be interrupted and finishes.
  void cancel() { cancel_.store(true); }

  // Moves a finished result out, once. False when there is none.
  bool take_check(CheckDone& out);
  bool take_dump(DumpOutcome& out);

 private:
  enum class Kind { None, Check, Dump };
  void join();

  std::thread th_;
  std::atomic<bool> busy_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<int> permille_{0};
  std::atomic<Kind> kind_{Kind::None};
  mutable std::mutex mu_;
  std::string stage_;
  bool have_check_ = false, have_dump_ = false;
  CheckDone check_;
  DumpOutcome dump_;
};

}  // namespace x2::ui
