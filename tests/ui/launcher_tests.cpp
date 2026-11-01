// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The launcher without a window: the library read from manifests, the worker, the import job's failure paths, the model driven through a
// fake importer (progress, cancel, a second disk, the protection-patch safeguards, drops while busy), the frame driven by key presses
// (cards, import flow, patch dialog, reachability without a mouse) and golden renders compared by hash at zero tolerance. No SDL, no
// game data: the manifests and files here are made by the test.
//
//   x2_launcher_tests HASHES [OUTDIR [WORKDIR]]   run the tests, compare the renders (a mismatch writes the picture to OUTDIR)
//   x2_launcher_tests --update HASHES             rewrite the hash file from the current output
//   x2_launcher_tests --dump DIR                  write every scene as PNG (2x) for review
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "diagnostics.h"
#include "log_buffer.h"
#include "pages/advanced.h"
#include "pages/launcher_pages.h"
#include "support/synth.h"
#include "test_tone.h"
#include "ui_png.h"

using namespace x2;
using namespace x2::ui;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

fs::path g_work;

void write_text(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Waits (up to a few seconds) for `cond`.
bool wait_for(const std::function<bool()>& cond) {
  for (int i = 0; i < 3000; ++i) {
    if (cond()) return true;
    sleep_ms(1);
  }
  return cond();
}

// A data directory with manifests like the importers write them.
fs::path make_data(const char* name, bool disk, bool cdtv, bool st, bool cd_audio = true, bool patched = false) {
  const fs::path d = g_work / name;
  fs::remove_all(d);
  fs::create_directories(d);
  if (disk)
    write_text(d / "assets" / "manifest.txt",
               std::string("# x2recomp imported assets\nimporter_version=3\nsource=/somewhere/a.adf\nvariant=retail\nvariant_name=Xenon 2 Megablast, retail release\n"
                           "canonical=") + (patched ? "false" : "true") + "\npatched=" + (patched ? "true" : "false") + "\n" +
                   (patched ? "copylock=absent\n" : "copylock=present\ncopylock.txt_sha1=a1b2c3d4e5f60718293a4b5c6d7e8f9012345678\n"));
  if (cdtv) {
    write_text(d / "assets-cdtv" / "manifest.txt", "importer_version=3\nbuild=cdtv\nvariant_name=CDTV disc, EN FR DE IT\ncanonical=false\npatched=false\n");
    if (cd_audio) write_text(d / "cdtv" / "manifest.txt", "x2recomp-cdtv 1\n");
  }
  if (st) write_text(d / "assets-st" / "manifest.txt", "importer_version=3\nbuild=atari-st-kixx\nvariant_name=Kixx, UK\ncanonical=false\npatched=false\n");
  return d;
}

std::string with_sep(const fs::path& p) { return p.string() + "/"; }

// ---- library ----
void test_library() {
  const fs::path d = make_data("lib_all", true, true, true);
  Library l = scan_library(with_sep(d));
  CHECK(l.count() == 3 && l.any());
  CHECK(l.get(GameKind::Disk).present && l.get(GameKind::Disk).variant == "Xenon 2 Megablast, retail release");
  CHECK(l.get(GameKind::Disk).canonical && !l.get(GameKind::Disk).patched && l.get(GameKind::Disk).status == "canonical");
  CHECK(l.get(GameKind::Cdtv).present && l.get(GameKind::Cdtv).has_cd_audio && l.get(GameKind::Cdtv).status == "imported");
  CHECK(l.get(GameKind::St).present && l.get(GameKind::St).status == "preview" && l.get(GameKind::St).variant == "Kixx, UK");
  CHECK(l.first_present() == GameKind::Disk);
  // The trailing separator is optional.
  CHECK(scan_library(d.string()).count() == 3);
  CHECK(assets_dir_for(d.string(), GameKind::Cdtv) == (d / "assets-cdtv").string());
  CHECK(cd_audio_dir(d.string()) == (d / "cdtv").string());

  Library none = scan_library(with_sep(make_data("lib_none", false, false, false)));
  CHECK(none.count() == 0 && !none.any() && none.first_present() == GameKind::Disk);
  Library empty = scan_library("");
  CHECK(empty.count() == 0);

  Library cd_only = scan_library(with_sep(make_data("lib_cdtv", false, true, false, false)));
  CHECK(cd_only.count() == 1 && cd_only.first_present() == GameKind::Cdtv && !cd_only.get(GameKind::Cdtv).has_cd_audio);
  CHECK(scan_library(with_sep(make_data("lib_st", false, false, true))).first_present() == GameKind::St);
  Library patched = scan_library(with_sep(make_data("lib_patched", true, false, false, true, true)));
  CHECK(patched.get(GameKind::Disk).patched && patched.get(GameKind::Disk).status == "patched" && !patched.get(GameKind::Disk).canonical);

  GameKind g;
  CHECK(parse_game_kind("disk", g) && g == GameKind::Disk && parse_game_kind("cdtv", g) && g == GameKind::Cdtv && parse_game_kind("st", g) && g == GameKind::St);
  CHECK(!parse_game_kind("auto", g) && !parse_game_kind("", g) && !parse_game_kind("ST", g));
}

// ---- worker ----
void test_worker() {
  Worker w;
  std::vector<Message> msgs;
  CHECK(!w.busy() && !w.running() && !w.collect());
  std::atomic<bool> go{false};
  CHECK(w.start([&](JobContext& j) {
    j.progress(100, "one");
    j.message(Severity::Info, "first");
    j.progress(50, "lower is ignored");
    while (!go.load() && !j.cancelled()) sleep_ms(1);
    j.progress(700, "two");
    j.message(Severity::Warn, "second");
  }));
  CHECK(w.busy() && w.running());
  CHECK(!w.start([](JobContext&) {}));  // one job at a time
  CHECK(wait_for([&] { return w.poll(msgs).permille == 100; }));
  JobView v = w.poll(msgs);
  CHECK(v.running && v.permille == 100 && v.stage == "lower is ignored");  // the stage text follows, the number never falls
  CHECK(msgs.size() == 1 && msgs[0].text == "first" && msgs[0].severity == Severity::Info);
  go = true;
  CHECK(wait_for([&] {
    w.poll(msgs);
    return w.collect();
  }));
  CHECK(msgs.size() == 2 && msgs[1].text == "second" && msgs[1].severity == Severity::Warn);
  CHECK(!w.busy() && !w.collect());
  v = w.poll(msgs);
  CHECK(!v.running);

  // Cancel reaches the job; a second job starts clean.
  std::atomic<bool> saw_cancel{false};
  CHECK(w.start([&](JobContext& j) {
    while (!j.cancelled()) sleep_ms(1);
    saw_cancel = true;
  }));
  w.cancel();
  CHECK(wait_for([&] { return w.collect(); }) && saw_cancel);
  msgs.clear();
  CHECK(w.start([](JobContext& j) { j.message(Severity::Ok, "again"); }));
  CHECK(wait_for([&] {
    w.poll(msgs);
    return w.collect();
  }));
  CHECK(msgs.size() == 1 && msgs[0].text == "again");
  v = w.poll(msgs);
  CHECK(v.permille == 0);  // progress restarts

  // A job that throws becomes a message, not a crash.
  CHECK(w.start([](JobContext&) { throw 7; }));
  msgs.clear();
  CHECK(wait_for([&] {
    w.poll(msgs);
    return w.collect();
  }));
  CHECK(!msgs.empty() && msgs.back().severity == Severity::Error);

  // The destructor cancels and waits for a running job.
  std::atomic<bool> done{false};
  {
    Worker w2;
    w2.start([&](JobContext& j) {
      while (!j.cancelled()) sleep_ms(1);
      done = true;
    });
  }
  CHECK(done);
}

// ---- the import job's own failure paths (no game data: junk files) ----
void test_import_job() {
  const fs::path d = g_work / "job";
  fs::remove_all(d);
  fs::create_directories(d / "in");
  write_text(d / "in" / "junk.adf", std::string(5000, 'x'));
  write_text(d / "in" / "notes.txt", "hello");
  Worker w;
  auto run = [&](ImportRequest req, std::vector<Message>* out = nullptr, bool cancel_first = false) {
    ImportOutcome o;
    std::atomic<bool> go{!cancel_first};
    w.start([&](JobContext& j) {
      while (!go.load() && !j.cancelled()) sleep_ms(1);
      o = run_import(req, j);
    });
    if (cancel_first) w.cancel();
    std::vector<Message> msgs;
    wait_for([&] {
      w.poll(msgs);
      return w.collect();
    });
    if (out) *out = msgs;
    return o;
  };
  ImportRequest req;
  req.data_dir = with_sep(d / "data");
  req.paths = {(d / "in" / "junk.adf").string()};
  std::vector<Message> msgs;
  ImportOutcome o = run(req, &msgs);
  CHECK(o.kind == ImportOutcome::Kind::Failed && !o.headline.empty());
  CHECK(!msgs.empty() && !fs::exists(d / "data" / "assets"));  // nothing is written for a failed import
  CHECK(detect_family(req.paths) == GameKind::Disk);

  req.paths = {(d / "in" / "does-not-exist.adf").string()};
  o = run(req, &msgs);
  CHECK(o.kind == ImportOutcome::Kind::Failed && o.headline.find("no such file") != std::string::npos);

  req.paths.clear();
  CHECK(run(req).kind == ImportOutcome::Kind::Failed);
  req.paths = {(d / "in").string()};
  req.data_dir.clear();
  o = run(req);
  CHECK(o.kind == ImportOutcome::Kind::Failed && o.headline.find("data folder") != std::string::npos);

  // Cancelled before the importer starts: nothing is changed and the outcome says so.
  req.data_dir = with_sep(d / "data");
  req.paths = {(d / "in" / "junk.adf").string(), (d / "in" / "notes.txt").string()};
  o = run(req, &msgs, true);
  CHECK(o.kind == ImportOutcome::Kind::Cancelled && o.headline.find("Nothing was changed") != std::string::npos);
  CHECK(!fs::exists(d / "data" / "assets"));

  // The patch is never applied without the confirmation, even if the request asks: junk fails either way, but the flags are what the
  // importer is given (checked through the model below); here the junk stays a plain failure.
  req.apply_protection_patch = true;
  req.confirmed_ownership = false;
  CHECK(run(req).kind != ImportOutcome::Kind::Imported);
}

// ---- the model, with a fake importer ----
struct Fake {
  enum Mode { Succeed, Block, Fail, NeedMore, PatchOffer } mode = Succeed;
  std::atomic<bool> release{false};
  std::mutex mu;
  std::vector<ImportRequest> requests;
  std::string data_dir;
  GameKind game = GameKind::Disk;

  ImportOutcome operator()(const ImportRequest& req, JobContext& j) {
    {
      std::lock_guard<std::mutex> lock(mu);
      requests.push_back(req);
    }
    j.progress(200, "Reading the images");
    j.message(Severity::Info, "disc 1: a.adf");
    ImportOutcome o;
    o.game = game;
    if (mode == Block) {
      while (!release.load() && !j.cancelled()) sleep_ms(1);
      if (j.cancelled()) {
        o.kind = ImportOutcome::Kind::Cancelled;
        o.headline = "Import cancelled. Nothing was changed.";
        return o;
      }
    }
    j.progress(600, "Decoding");
    switch (mode) {
      case Fail:
        j.message(Severity::Error, "disc 1 is damaged");
        o.kind = ImportOutcome::Kind::Failed;
        o.headline = "disc 1 is damaged";
        break;
      case NeedMore:
        o.kind = ImportOutcome::Kind::NeedMore;
        o.headline = "Only one disk so far: disk 2 is still missing. Drop it as well.";
        break;
      case PatchOffer:
        o.kind = ImportOutcome::Kind::PatchOffer;
        o.headline = "This is a retail disk 1 without its key track.";
        break;
      default:
        write_text(fs::path(data_dir) / (game == GameKind::Disk ? "assets" : game == GameKind::Cdtv ? "assets-cdtv" : "assets-st") / "manifest.txt",
                   "importer_version=3\nvariant_name=Fake release\ncanonical=true\npatched=false\n");
        j.progress(1000, "Done");
        o.kind = ImportOutcome::Kind::Imported;
        o.variant_name = "Fake release";
        o.headline = "Imported: Fake release";
    }
    return o;
  }
  size_t count() {
    std::lock_guard<std::mutex> lock(mu);
    return requests.size();
  }
};

struct Harness {
  Fake fake;
  fs::path dir;
  std::unique_ptr<LauncherState> st;
  Harness(const char* name, bool disk, bool cdtv, bool stt, bool first_run = false, bool settings = false) {
    dir = make_data(name, disk, cdtv, stt);
    fake.data_dir = with_sep(dir);
    LauncherEnv env;
    env.data_dir = with_sep(dir);
    if (settings) env.settings_path = (dir / "settings.cfg").string();
    env.version = "0.5.0";
    env.first_run = first_run;
    env.importer = [this](const ImportRequest& r, JobContext& j) { return fake(r, j); };
    st = std::make_unique<LauncherState>(std::move(env));
  }
  bool settle() {
    return wait_for([&] {
      st->poll();
      return !st->busy();
    });
  }
};

void test_state_flow() {
  // First run: nothing imported opens the import view; a successful import shows the library with the new card.
  {
    Harness h("flow_first", false, false, false, true);
    CHECK(h.st->in_import && !h.st->library.any() && h.st->back_label() == std::string("Quit"));
    h.st->drop({"/x/a.adf", "/x/b.adf"});
    CHECK(h.st->busy() || h.st->job.running);
    CHECK(h.settle());
    CHECK(!h.st->in_import && h.st->library.get(GameKind::Disk).present && h.st->selected == GameKind::Disk);
    CHECK(h.st->headline == "Imported: Fake release" && h.st->headline_severity == Severity::Ok && h.st->note == h.st->headline);
    CHECK(h.st->dropped.empty());
    CHECK(h.fake.count() == 1 && h.fake.requests[0].paths.size() == 2 && h.fake.requests[0].data_dir == with_sep(h.dir));
    CHECK(!h.fake.requests[0].apply_protection_patch && !h.fake.requests[0].confirmed_ownership);
    CHECK(h.st->log.size() == 1 && h.st->log[0].text == "disc 1: a.adf");
    CHECK(h.st->back_label() == std::string("Back"));
  }
  // Another family imported later: the card and the selection follow.
  {
    Harness h("flow_second", true, false, false);
    CHECK(!h.st->in_import && h.st->selected == GameKind::Disk);
    h.fake.game = GameKind::Cdtv;
    h.st->open_import();
    CHECK(h.st->in_import);
    h.st->drop({"/x/disc.cue"});
    CHECK(h.settle());
    CHECK(h.st->library.count() == 2 && h.st->selected == GameKind::Cdtv && h.st->model.game == GameKind::Cdtv);
  }
  // A failed import keeps the view, clears the dropped files and says why.
  {
    Harness h("flow_fail", true, false, false);
    h.fake.mode = Fake::Fail;
    h.st->drop({"/x/a.adf"});
    CHECK(h.settle());
    CHECK(h.st->in_import && h.st->headline == "disc 1 is damaged" && h.st->headline_severity == Severity::Error && h.st->dropped.empty());
    CHECK(h.st->log.size() == 2 && h.st->log[1].severity == Severity::Error);
    h.st->close_import();
    CHECK(!h.st->in_import && !h.st->quit);
  }
  // One disk of two: the files stay, and the second drop carries both.
  {
    Harness h("flow_more", false, false, false, true);
    h.fake.mode = Fake::Fail;
    h.fake.mode = Fake::NeedMore;
    h.st->drop({"/x/disk1.adf"});
    CHECK(h.settle());
    CHECK(h.st->in_import && h.st->dropped.size() == 1 && h.st->headline_severity == Severity::Warn && !h.st->patch_offer);
    h.fake.mode = Fake::Succeed;
    h.st->drop({"/x/disk2.adf"});
    CHECK(h.settle());
    CHECK(h.fake.count() == 2 && h.fake.requests[1].paths == (std::vector<std::string>{"/x/disk1.adf", "/x/disk2.adf"}));
    CHECK(h.st->library.any() && h.st->dropped.empty());
    // "Start over" forgets what was given.
    Harness h2("flow_more2", false, false, false, true);
    h2.fake.mode = Fake::NeedMore;
    h2.st->drop({"/x/disk1.adf"});
    CHECK(h2.settle() && h2.st->dropped.size() == 1);
    h2.st->forget_dropped();
    CHECK(h2.st->dropped.empty() && h2.st->headline.empty() && h2.st->log.empty());
  }
  // Cancel: the importer stops, nothing is changed, the previous import is untouched.
  {
    Harness h("flow_cancel", true, false, false);
    const std::string before = read_text(h.dir / "assets" / "manifest.txt");
    h.fake.mode = Fake::Block;
    h.st->drop({"/x/a.adf"});
    CHECK(wait_for([&] {
      h.st->poll();
      return h.st->job.running && h.st->job.permille == 200 && h.st->job.stage == "Reading the images";
    }));
    CHECK(h.st->busy());
    h.st->close_import();  // not while it runs
    CHECK(h.st->in_import);
    h.st->cancel_import();
    CHECK(h.settle());
    CHECK(h.st->in_import && h.st->headline == "Import cancelled. Nothing was changed." && h.st->headline_severity == Severity::Info);
    CHECK(h.st->dropped.empty() && read_text(h.dir / "assets" / "manifest.txt") == before);
  }
  // A drop while an import runs is refused with a message, not queued.
  {
    Harness h("flow_busy", true, false, false);
    h.fake.mode = Fake::Block;
    h.st->drop({"/x/a.adf"});
    CHECK(wait_for([&] {
      h.st->poll();
      return h.st->job.running && h.fake.count() == 1;
    }));
    h.st->drop({"/x/b.adf"});
    h.st->forget_dropped();  // ignored while running
    CHECK(h.fake.count() == 1 && h.st->dropped.size() == 1);
    CHECK(!h.st->log.empty() && h.st->log.back().severity == Severity::Warn);
    h.st->open_import();
    h.fake.release = true;
    CHECK(h.settle());
  }
  // Quit from the import view of a first run; Back otherwise.
  {
    Harness h("flow_quit", false, false, false, true);
    h.st->close_import();
    CHECK(h.st->quit);
  }
}

void test_patch_safeguards() {
  Harness h("patch", false, false, false, true);
  h.fake.mode = Fake::PatchOffer;
  h.st->drop({"/x/retail1.adf", "/x/retail2.adf"});
  CHECK(h.settle());
  CHECK(h.st->patch_offer && h.st->in_import && h.st->dropped.size() == 2 && h.st->headline_severity == Severity::Warn);
  CHECK(h.fake.count() == 1 && !h.fake.requests[0].apply_protection_patch);
  // Without the ownership box nothing happens.
  h.st->import_with_patch();
  CHECK(h.fake.count() == 1);
  // With it: both flags travel together, and the box does not stay ticked for the next run.
  h.st->patch_owned = true;
  h.fake.mode = Fake::Succeed;
  h.st->import_with_patch();
  CHECK(h.settle());
  CHECK(h.fake.count() == 2 && h.fake.requests[1].apply_protection_patch && h.fake.requests[1].confirmed_ownership);
  CHECK(!h.st->patch_owned && !h.st->patch_offer && h.st->library.any());
  // A plain retry after that never carries the patch.
  h.st->open_import();
  h.st->drop({"/x/other.ipf"});
  CHECK(h.settle());
  CHECK(h.fake.count() == 3 && !h.fake.requests[2].apply_protection_patch && !h.fake.requests[2].confirmed_ownership);
  // Nothing dropped: nothing to patch.
  Harness none("patch_none", false, false, false, true);
  none.st->patch_owned = true;
  none.st->import_with_patch();
  CHECK(none.fake.count() == 0);
}

void test_play_and_settings() {
  Harness h("play", true, true, false, false, true);
  CHECK(!h.st->in_import && h.st->selected == GameKind::Disk);
  h.st->model.s.scale = 5;
  CHECK(h.st->settings_dirty());
  h.st->play(GameKind::Cdtv);
  CHECK(h.st->start && h.st->start_game == GameKind::Cdtv && h.st->model.s.last_game == "cdtv");
  const std::string file = read_text(h.dir / "settings.cfg");
  CHECK(file.find("last_game = cdtv") != std::string::npos && file.find("scale = 5") != std::string::npos);
  CHECK(!h.st->settings_dirty());
  // Reopened: the release played last is focused first, an explicit --game wins over it.
  {
    LauncherEnv env;
    env.data_dir = with_sep(h.dir);
    env.settings_path = (h.dir / "settings.cfg").string();
    LauncherState again(env);
    CHECK(again.selected == GameKind::Cdtv && again.model.s.scale == 5);
    env.initial_game = "disk";
    LauncherState explicit_game(env);
    CHECK(explicit_game.selected == GameKind::Disk);
    // A release that is no longer imported does not stay selected.
    fs::remove_all(h.dir / "assets-cdtv");
    LauncherEnv genv;
    genv.data_dir = with_sep(h.dir);
    genv.settings_path = (h.dir / "settings.cfg").string();
    LauncherState gone(genv);
    CHECK(gone.selected == GameKind::Disk);
  }
  // Without a settings path nothing is written.
  Harness mem("play_mem", true, false, false);
  mem.st->model.s.scale = 2;
  mem.st->save_settings();
  CHECK(mem.st->settings_dirty() && !fs::exists(mem.dir / "settings.cfg"));
  // An unwritable settings file becomes a warning, not a crash.
  LauncherEnv bad;
  bad.data_dir = with_sep(h.dir);
  bad.settings_path = (h.dir / "no-such-dir" / "deeper" / "settings.cfg").string();
  LauncherState b(bad);
  b.model.s.scale = 7;
  b.save_settings();
  CHECK(b.model.dirty() || !b.log.empty() || fs::exists(bad.settings_path));
}

// ---- the frame, driven by keys ----
struct Frame {
  Context ctx;
  Canvas canvas;
  Harness& h;
  Frame(Harness& harness, int w = 640, int hh = 360) : canvas(w, hh), h(harness) {
    ctx.motion.reduced = true;
    run({});
    run({});
  }
  void run(const EventList& ev) {
    auto builder = [&](Context& c) { launcher_frame(c, *h.st); };
    ctx.run(canvas, ev, 0, builder);
  }
  void key(Nav n) { run({Event::nav_key(n)}); }
  void focus(const char* id) {
    ctx.set_focus(ctx.id(id));
    run({});
    run({});
  }
  bool focused(const char* id) const { return ctx.focused() == ctx.id(id); }
  // Every registered control is reachable from the focused one with Up, Down, Left and Right.
  bool all_reachable(bool modal_only = false) const {
    const std::vector<FocusItem>& items = ctx.items();
    std::vector<int> pool;
    for (size_t i = 0; i < items.size(); ++i)
      if (!modal_only || items[i].in_modal) pool.push_back(static_cast<int>(i));
    if (pool.empty()) return false;
    int start = -1;
    for (int i : pool)
      if (items[static_cast<size_t>(i)].id == ctx.focused()) start = i;
    if (start < 0) return false;
    std::set<int> seen{start};
    std::vector<int> todo{start};
    while (!todo.empty()) {
      const int cur = todo.back();
      todo.pop_back();
      for (Nav d : {Nav::Up, Nav::Down, Nav::Left, Nav::Right}) {
        const int n = Context::neighbor(items, cur, d, modal_only);
        if (n >= 0 && seen.insert(n).second) todo.push_back(n);
      }
    }
    if (seen.size() != pool.size())
      for (int i : pool)
        if (!seen.count(i)) std::fprintf(stderr, "  not reached: %d,%d %dx%d\n", items[static_cast<size_t>(i)].rect.x, items[static_cast<size_t>(i)].rect.y,
                                         items[static_cast<size_t>(i)].rect.w, items[static_cast<size_t>(i)].rect.h);
    return seen.size() == pool.size();
  }
};

void test_cards_and_navigation() {
  Harness h("nav", true, true, false);
  Frame f(h);
  CHECK(f.focused("card_disk"));  // the selected card has the focus first
  h.st->model.s.last_game = "disk";
  // Right moves to the next card and the release the pages describe follows.
  f.key(Nav::Right);
  CHECK(f.focused("card_cdtv") && h.st->selected == GameKind::Cdtv && h.st->model.game == GameKind::Cdtv);
  f.key(Nav::Right);
  CHECK(f.focused("card_st") && h.st->selected == GameKind::St);
  // A card that is not imported opens the import view instead of playing.
  f.key(Nav::Accept);
  CHECK(h.st->in_import && !h.st->start);
  CHECK(h.st->library.count() == 2);
  // Back leaves it again (the library is not empty).
  f.key(Nav::Back);
  CHECK(!h.st->in_import);
  f.focus("card_cdtv");
  f.key(Nav::Accept);
  CHECK(h.st->start && h.st->start_game == GameKind::Cdtv && h.st->model.s.last_game == "cdtv");

  // The summary lines jump to their pages; the "start directly" box writes launcher = never.
  Harness h2("nav2", true, false, false);
  Frame g(h2);
  g.focus("start_last_directly");
  g.key(Nav::Accept);
  CHECK(h2.st->model.s.launcher == "never");
  g.key(Nav::Accept);
  CHECK(h2.st->model.s.launcher == "auto");
  CHECK(h2.st->page == kPlayPage);
  bool found_link = false;
  for (const FocusItem& it : g.ctx.items())
    if (it.rect.h == 18 && it.rect.w >= 150 && it.rect.w <= 340 && it.region == Region::Page) {
      g.ctx.set_focus(it.id);
      g.run({});
      g.key(Nav::Accept);
      found_link = true;
      break;
    }
  CHECK(found_link && (h2.st->page == kDisplayPage || h2.st->page == kAudioPage));

  // The Quit button.
  Harness h3("nav3", true, false, false);
  Frame q(h3);
  bool quit_found = false;
  for (const FocusItem& it : q.ctx.items())
    if (it.rect.w == 72 && it.rect.h == 22) {
      q.ctx.set_focus(it.id);
      q.run({});
      q.key(Nav::Accept);
      quit_found = true;
    }
  CHECK(quit_found && h3.st->quit);

  // The other pages of the window are the settings pages: a setting changed on one is in the model, with its command line.
  Harness h4("nav4", true, false, false);
  Frame d(h4);
  d.key(Nav::Back);  // to the page selector
  d.key(Nav::Down);  // Display
  CHECK(h4.st->page == kDisplayPage);
  d.focus("scale");
  d.key(Nav::Right);
  CHECK(h4.st->model.s.scale == 4 && h4.st->model.command().text() == "x2recomp --game disk --scale 4");
}

void test_packs_page() {
  Harness h("packs", true, false, false, false, true);
  const fs::path pack = h.dir / "packs" / "demo";
  write_text(pack / "manifest.json",
             "{\"format\": \"x2-asset-pack\", \"version\": 2, \"id\": \"demo\", \"name\": \"Demo\", \"pack_version\": \"1.0.0\", \"author\": \"T\", "
             "\"license\": \"CC0-1.0\", \"games\": [\"disk\"], \"scale\": 2, \"graphics\": {\"0123456789abcdef-16x16\": {\"png\": \"a.png\"}}}");
  synth::put_flat((pack / "a.png").string(), 32, 32);
  CHECK(h.st->model.packs == &h.st->packs && h.st->packs.packs_dir == with_sep(h.dir) + "packs");
  h.st->page = kPacksPage;
  Frame f(h);
  CHECK(f.ctx.focused() != 0 && f.all_reachable());
  // A drop on the Asset packs page is a pack, not a disk image: nothing is imported.
  h.st->drop({pack.string()});
  CHECK(h.fake.count() == 0 && !h.st->in_import);
  CHECK(h.st->model.s.packs_enabled == "demo");
  CHECK(read_text(h.dir / "settings.cfg").find("packs_enabled = demo") != std::string::npos);
  // On the home page the same drop starts an import.
  h.st->page = kPlayPage;
  h.st->drop({"/x/a.adf"});
  CHECK(h.settle() && h.fake.count() == 1);
}

void test_import_view() {
  // The import view of a first run: drop zone, the two chooser buttons, Quit, the legal text; every control reachable.
  for (const int w : {640, 480}) {
    Harness h("view", false, false, false, true);
    int chosen = 0;
    h.st->env.choose_files = [&] { ++chosen; };
    h.st->env.choose_folder = [&] { chosen += 10; };
    Frame f(h, w, w == 640 ? 360 : 270);
    CHECK(h.st->in_import);
    CHECK(f.all_reachable());
    // The first control is "Choose files...".
    f.key(Nav::Accept);
    CHECK(chosen == 1);
    f.key(Nav::Right);
    f.key(Nav::Accept);
    CHECK(chosen == 11);
    // Back with nothing imported quits.
    f.key(Nav::Back);
    CHECK(h.st->quit);
  }
  // While an import runs the buttons that start another are inert and Cancel works from the keyboard.
  Harness h("view_run", false, false, false, true);
  int chosen = 0;
  h.st->env.choose_files = [&] { ++chosen; };
  h.fake.mode = Fake::Block;
  Frame f(h);
  h.st->drop({"/x/a.adf"});
  CHECK(wait_for([&] {
    h.st->poll();
    return h.st->job.running && h.st->job.permille == 200;
  }));
  f.run({});
  f.run({});
  f.key(Nav::Accept);  // "Choose files..." is disabled now
  CHECK(chosen == 0);
  f.key(Nav::Back);  // and Back does not leave while it runs
  CHECK(h.st->in_import && !h.st->quit);
  bool cancel_found = false;
  for (const FocusItem& it : f.ctx.items())
    if (it.rect.w == 88 && it.rect.h == 20) {
      f.ctx.set_focus(it.id);
      f.run({});
      f.key(Nav::Accept);
      cancel_found = true;
    }
  CHECK(cancel_found);
  CHECK(h.settle());
  CHECK(h.st->headline == "Import cancelled. Nothing was changed.");
  f.run({});
  f.run({});
}

void test_patch_dialog() {
  Harness h("dialog", false, false, false, true);
  h.fake.mode = Fake::PatchOffer;
  Frame f(h);
  h.st->drop({"/x/retail1.adf"});
  CHECK(h.settle());
  f.run({});
  f.run({});
  CHECK(h.st->patch_offer);
  // The offer is a button, never the default focus.
  Rect btn{};
  Id btn_id = 0;
  for (const FocusItem& it : f.ctx.items())
    if (it.rect.w == 272 && it.rect.h == 22) {
      btn = it.rect;
      btn_id = it.id;
    }
  CHECK(btn.w > 0 && f.ctx.focused() != btn_id);
  f.ctx.set_focus(btn_id);
  f.run({});
  f.key(Nav::Accept);
  CHECK(f.ctx.modal_open());
  CHECK(!h.st->patch_owned);
  CHECK(f.all_reachable(true));
  // Cancel is focused first; Apply is inert until the box is ticked.
  f.focus("Cancel");
  CHECK(f.focused("Cancel"));
  Id apply_id = 0, own_id = f.ctx.id("patch_owned");
  for (const FocusItem& it : f.ctx.items())
    if (it.in_modal && it.rect.w == 148) apply_id = it.id;
  CHECK(apply_id != 0);
  f.ctx.set_focus(apply_id);
  f.run({});
  f.key(Nav::Accept);
  CHECK(h.fake.count() == 1 && f.ctx.modal_open());  // no run: the box is not ticked
  f.ctx.set_focus(own_id);
  f.run({});
  f.key(Nav::Accept);
  CHECK(h.st->patch_owned);
  f.ctx.set_focus(apply_id);
  f.run({});
  h.fake.mode = Fake::Succeed;
  f.key(Nav::Accept);
  CHECK(!f.ctx.modal_open());
  CHECK(h.settle());
  CHECK(h.fake.count() == 2 && h.fake.requests[1].apply_protection_patch && h.fake.requests[1].confirmed_ownership);
  // Back closes the dialog without applying anything, and the box does not carry over.
  Harness h2("dialog2", false, false, false, true);
  h2.fake.mode = Fake::PatchOffer;
  Frame g(h2);
  h2.st->drop({"/x/retail1.adf"});
  CHECK(h2.settle());
  g.run({});
  g.run({});
  Id b2 = 0;
  for (const FocusItem& it : g.ctx.items())
    if (it.rect.w == 272 && it.rect.h == 22) b2 = it.id;
  g.ctx.set_focus(b2);
  g.run({});
  g.key(Nav::Accept);
  CHECK(g.ctx.modal_open());
  g.focus("patch_owned");
  g.key(Nav::Accept);
  CHECK(h2.st->patch_owned);
  g.key(Nav::Back);
  CHECK(!g.ctx.modal_open() && h2.fake.count() == 1);
  g.ctx.set_focus(b2);
  g.run({});
  g.key(Nav::Accept);
  CHECK(g.ctx.modal_open() && !h2.st->patch_owned);  // reopened: unticked again
}

void test_reachability() {
  for (const int w : {640, 480}) {
    Harness h("reach", true, true, true);
    Frame f(h, w, w == 640 ? 360 : 270);
    CHECK(f.all_reachable());
    for (const int page : {kDisplayPage, kAudioPage, kAboutPage}) {
      h.st->page = page;
      f.run({});
      f.run({});
      CHECK(f.ctx.focused() != 0);
    }
  }
}


// ---- the log ----
void test_log_buffer() {
  LogBuffer& log = LogBuffer::instance();
  log.close_file();
  log.clear();
  int64_t now = 1'800'000'000;  // 2027-01-15 08:00:00 UTC
  log.set_clock([&] { return now; });
  const uint64_t g0 = log.generation();
  log.add(Severity::Info, "first");
  log.add(Severity::Warn, "two\nlines\r\n\n");
  CHECK(log.generation() == g0 + 3);
  CHECK(log.lines().size() == 3 && log.lines()[1].text == "two" && log.lines()[2].text == "lines" && log.lines()[1].severity == Severity::Warn);
  log.add(Severity::Info, "");
  CHECK(log.generation() == g0 + 3 && log.lines().size() == 3);  // empty text is ignored
  // The ring keeps the newest 500.
  log.clear();
  for (int i = 0; i < 700; ++i) log.add(Severity::Info, "line " + std::to_string(i));
  CHECK(log.lines().size() == LogBuffer::kMaxLines && log.lines().front().text == "line 200" && log.lines().back().text == "line 699");
  // The file: time stamp, severity word, one line each.
  const fs::path file = g_work / "log" / "x2recomp.log";
  fs::remove_all(file.parent_path());
  fs::create_directories(file.parent_path());
  CHECK(log.open_file(file.string()) && log.file_path() == file.string());
  log.add(Severity::Error, "boom");
  log.add(Severity::Ok, "fine");
  const std::string text = read_text(file);
  CHECK(text == "2027-01-15 08:00:00 error: boom\n2027-01-15 08:00:00 ok: fine\n");
  // An existing file over the limit is moved aside when the file is opened, and the file rotates while it grows.
  log.close_file();
  write_text(file, std::string(LogBuffer::kMaxFileBytes + 10, 'x'));
  CHECK(log.open_file(file.string()));
  CHECK(fs::exists(file.string() + ".1") && fs::file_size(file.string() + ".1") == LogBuffer::kMaxFileBytes + 10);
  CHECK(read_text(file).empty());
  const std::string big(1000, 'y');
  for (int i = 0; i < 300; ++i) log.add(Severity::Info, big);  // about 300 KB
  CHECK(fs::exists(file.string() + ".1") && fs::file_size(file) < LogBuffer::kMaxFileBytes + 2000);
  // An unwritable path leaves the buffer working in memory.
  log.close_file();
  CHECK(!log.open_file((g_work / "no" / "such" / "dir" / "x.log").string()) && log.file_path().empty());
  log.add(Severity::Info, "still works");
  CHECK(log.lines().back().text == "still works");
  log.set_clock({});
  log.clear();
}

// ---- diagnostics and About ----
void test_diagnostics() {
  const fs::path d = make_data("diag", true, true, true);
  const Library lib = scan_library(with_sep(d));
  CHECK(lib.get(GameKind::Disk).protection == "key-track receipt" && lib.get(GameKind::Disk).importer_version == "3");
  CHECK(lib.get(GameKind::Disk).receipt_sha1 == "a1b2c3d4e5f6" && lib.get(GameKind::Disk).source_name == "a.adf");
  CHECK(lib.get(GameKind::Cdtv).protection == "none to answer" && lib.get(GameKind::Cdtv).source_name.empty());
  const Library patched = scan_library(with_sep(make_data("diag_patched", true, false, false, true, true)));
  CHECK(patched.get(GameKind::Disk).protection == "opt-in patch applied" && patched.get(GameKind::Disk).receipt_sha1.empty());

  Settings s;
  s.scale = 5;
  s.asset_packs = "/Users/someone/packs/mine.zip;/other/p";
  s.packs_enabled = "crisp;music";
  s.cdtv = "/Volumes/stuff/disc.cue";
  HostInfo host;
  host.platform = "Linux x86_64";
  host.sdl_version = "3.4.18";
  host.video_driver = "x11";
  host.audio_driver = "pipewire";
  host.display = "144 Hz";
  host.build_id = "gcc 12.2, release";
  const std::string text = diagnostics_text("0.5.0", host, lib, s, GameKind::Cdtv);
  CHECK(text.find("x2recomp 0.5.0\n") == 0);
  CHECK(text.find("platform: Linux x86_64") != std::string::npos && text.find("SDL: 3.4.18, video x11, audio pipewire, display 144 Hz") != std::string::npos);
  CHECK(text.find("selected release: Amiga CDTV") != std::string::npos);
  CHECK(text.find("disk: Xenon 2 Megablast, retail release; importer 3; protection: key-track receipt (a1b2c3d4e5f6); source a.adf") != std::string::npos);
  CHECK(text.find("cdtv: CDTV disc, EN FR DE IT; importer 3; protection: none to answer; CD music imported") != std::string::npos);
  CHECK(text.find("packs enabled: crisp;music") != std::string::npos && text.find("packs added by path: yes") != std::string::npos);
  CHECK(text.find("  scale = 5\n") != std::string::npos && text.find("  asset_packs = (set)\n") != std::string::npos && text.find("  cdtv = (set)\n") != std::string::npos);
  // No folder of the user's and no image path is copied.
  CHECK(text.find("/Users") == std::string::npos && text.find("/somewhere") == std::string::npos && text.find("/Volumes") == std::string::npos &&
        text.find(d.string()) == std::string::npos && text.find("/other") == std::string::npos);
  // Defaults say "none"; an empty library says "nothing".
  const std::string plain = diagnostics_text("", HostInfo{}, scan_library(""), Settings{}, GameKind::Disk);
  CHECK(plain.find("x2recomp (development build)") == 0 && plain.find("  nothing\n") != std::string::npos && plain.find("packs enabled: none") != std::string::npos);
  CHECK(plain.find("settings that differ from the defaults:\n  none\n") != std::string::npos && plain.find("SDL: none") != std::string::npos);

  CHECK(folder_name("/a/b/x2recomp/") == "x2recomp" && folder_name("C:\\Users\\me\\data\\") == "data" && folder_name("solo") == "solo" && folder_name("").empty());
  CHECK(third_party().size() >= 7);
  for (const char* name : {"Musashi", "puff", "dr_flac", "stb_vorbis", "Unscii", "SDL 3"}) {
    bool found = false;
    for (const ThirdParty& p : third_party()) found |= std::string(p.name) == name;
    CHECK(found);
  }
}

// ---- the test tone ----
void test_test_tone() {
  const std::vector<int16_t> a = make_test_chirp(48000, 450, 100);
  CHECK(a.size() == 48000u * 450 / 1000 * 2);
  CHECK(a == make_test_chirp(48000, 450, 100));  // deterministic
  CHECK(a[0] == 0 && a[1] == 0 && a[a.size() - 1] == 0 && a[a.size() - 2] == 0);  // fades in and out
  int peak = 0, peak_half = 0, differing = 0;
  for (int16_t v : a) peak = std::max(peak, std::abs(static_cast<int>(v)));
  for (int16_t v : make_test_chirp(48000, 450, 50)) peak_half = std::max(peak_half, std::abs(static_cast<int>(v)));
  CHECK(peak > 15000 && peak <= 16384 && peak_half > 7000 && peak_half <= 8192);
  for (size_t i = 0; i < a.size(); i += 2) differing += a[i] != a[i + 1];
  CHECK(differing == 0);  // the same on both channels
  for (int16_t v : make_test_chirp(48000, 450, 0)) CHECK(v == 0);
  CHECK(make_test_chirp(0, 450, 50).empty() && make_test_chirp(48000, 0, 50).empty());
  // The sweep rises: more zero crossings in the last third than in the first.
  auto crossings = [&](size_t from, size_t to) {
    int n = 0;
    for (size_t i = from + 2; i < to; i += 2) n += (a[i - 2] < 0) != (a[i] < 0);
    return n;
  };
  CHECK(crossings(a.size() * 2 / 3 & ~size_t(1), a.size()) > crossings(0, a.size() / 3 & ~size_t(1)));
}

// ---- the Advanced page's model: the image check, the settings file ----
void test_advanced_model() {
  Harness h("adv_model", true, false, false, false, true);
  AdvancedModel& adv = h.st->adv;
  CHECK(adv.version == "0.5.0" && !adv.busy() && adv.report.empty());
  // An image that does not exist is reported, not crashed on, and the report says so last.
  CHECK(adv.check_image({(h.dir / "no-such-image.adf").string()}));
  CHECK(!adv.check_image({"/x/second.adf"}));  // one check at a time
  CHECK(wait_for([&] {
    h.st->poll();
    return !adv.busy();
  }));
  CHECK(!adv.report.empty() && adv.report.back().severity == Severity::Error && adv.report.back().text == "Some input could not be read.");
  CHECK(!adv.check_image({}));

  // Export writes the settings file text; import reads it back (defaults first), keeps which release was played last, and says so.
  h.st->model.s.scale = 6;
  h.st->model.s.last_game = "disk";
  const fs::path out = h.dir / "exported.cfg";
  h.st->export_settings_to(out.string());
  CHECK(read_text(out) == format_settings(h.st->model.s) && adv.note == "Exported the settings.");
  h.st->export_settings_to((h.dir / "no" / "dir" / "x.cfg").string());
  CHECK(adv.note == "Could not write that file." && adv.note_severity == Severity::Warn);
  write_text(h.dir / "in.cfg", "scale = 2\nfilter = scanlines\n");
  h.st->model.s.last_game = "cdtv";
  h.st->import_settings_from((h.dir / "in.cfg").string());
  CHECK(h.st->model.s.scale == 2 && h.st->model.s.last_game == "cdtv" && adv.note == "Imported the settings." && !h.st->settings_dirty());
  CHECK(read_text(h.dir / "settings.cfg").find("scale = 2") != std::string::npos);
  write_text(h.dir / "bad.cfg", "scale = 4\nnot_a_key = 1\n");
  h.st->import_settings_from((h.dir / "bad.cfg").string());
  CHECK(h.st->model.s.scale == 4 && adv.note_severity == Severity::Warn);
  h.st->import_settings_from((h.dir / "missing.cfg").string());
  CHECK(adv.note == "Could not read that file." && h.st->model.s.scale == 4);
  // Reset: every setting back to the default except which release was played last; saved.
  h.st->model.s.fullscreen = true;
  h.st->reset_settings();
  CHECK(h.st->model.s.scale == Settings{}.scale && !h.st->model.s.fullscreen && h.st->model.s.last_game == "cdtv");
  CHECK(read_text(h.dir / "settings.cfg").find("scale = 4") == std::string::npos);
}

void test_advanced_page() {
  Harness h("adv_page", true, true, false, false, true);
  std::string copied;
  int opened_folder = 0, picked_image = 0;
  h.st->adv.copy_text = [&](const std::string& s) { copied = s; };
  h.st->adv.open_data_folder = [&] { ++opened_folder; };
  h.st->adv.choose_image = [&] { ++picked_image; };
  h.st->adv.export_settings = [] {};
  h.st->adv.import_settings = [] {};
  h.st->adv.host.platform = "Test OS";
  h.st->page = kAdvancedPage;
  Frame f(h);
  f.focus("Copy diagnostics");
  f.key(Nav::Accept);
  CHECK(copied.find("x2recomp 0.5.0") == 0 && copied.find("platform: Test OS") != std::string::npos && copied.find("disk: ") != std::string::npos);
  CHECK(h.st->adv.note.rfind("Copied the diagnostics", 0) == 0);
  f.focus("Open the data folder");
  f.key(Nav::Accept);
  CHECK(opened_folder == 1);
  f.focus("Check an image...");
  f.key(Nav::Accept);
  CHECK(picked_image == 1);
  // "Import again..." goes to the import flow, where the patch's confirmation lives; the patch itself is never applied here.
  f.focus("Import again...");
  f.key(Nav::Accept);
  CHECK(h.st->in_import && !h.st->patch_owned);
  h.st->close_import();
  f.run({});
  // The disclaimer opens in full and closes again.
  f.focus("Read the patch disclaimer");
  f.key(Nav::Accept);
  CHECK(f.ctx.modal_open());
  f.key(Nav::Back);
  CHECK(!f.ctx.modal_open());
  // Reset: Cancel keeps the settings, the confirmation resets them.
  h.st->model.s.scale = 7;
  f.focus("Reset all settings...");
  f.key(Nav::Accept);
  CHECK(f.ctx.modal_open());
  f.key(Nav::Accept);  // Cancel has the focus first
  CHECK(!f.ctx.modal_open() && h.st->model.s.scale == 7);
  f.focus("Reset all settings...");
  f.key(Nav::Accept);
  CHECK(f.ctx.modal_open());
  f.key(Nav::Left);  // from Cancel to the confirmation
  f.key(Nav::Accept);
  CHECK(!f.ctx.modal_open() && h.st->model.s.scale == Settings{}.scale);
  // A drop on this page is an image to check, not an import.
  CHECK(h.st->adv.report.empty());
  h.st->drop({(h.dir / "dropped.adf").string()});
  CHECK(!h.st->in_import && h.st->page == kAdvancedPage);
  CHECK(wait_for([&] {
    h.st->poll();
    return !h.st->adv.busy();
  }));
  CHECK(!h.st->adv.report.empty());
  // The input log switch is a schema row on this page, and the command footer follows it.
  f.focus("input_debug");
  f.key(Nav::Accept);
  CHECK(h.st->model.s.input_debug);
}

// ---- every page can be walked with a pad's buttons alone, in every size and theme; motion honours the setting ----
void test_pad_walk() {
  struct Size {
    int w, h;
    const char* tag;
  };
  const Size sizes[] = {{640, 360, "640x360"}, {480, 270, "480x270"}, {320, 180, "320x180 (large text)"}};
  for (const Size& sz : sizes)
    for (const bool hc : {false, true}) {
      Harness h("walk", true, true, true);
      h.st->adv.copy_text = [](const std::string&) {};
      h.st->adv.open_data_folder = [] {};
      h.st->adv.choose_image = [] {};
      h.st->adv.export_settings = [] {};
      h.st->adv.import_settings = [] {};
      Frame f(h, sz.w, sz.h);
      f.ctx.theme = hc ? Theme::high_contrast_theme() : Theme::standard();
      f.run({});
      // The page selector: PageNext (a shoulder button) steps through all seven pages and comes back to the first.
      int seen_pages = 0;
      for (int i = 0; i < kPageCount; ++i) {
        CHECK(h.st->page == i);
        f.run({});
        f.run({});
        CHECK(f.ctx.focused() != 0);
        // On every page every control is reachable from the focused one with the d-pad.
        if (!f.all_reachable()) std::fprintf(stderr, "  not reachable on page %d at %s%s\n", i, sz.tag, hc ? " hc" : "");
        CHECK(f.all_reachable());
        // Down for as long as the focus moves: it never loses the focus and never leaves the page for the page selector.
        for (int step = 0; step < 200; ++step) {
          const Id before = f.ctx.focused();
          f.key(Nav::Down);
          CHECK(f.ctx.focused() != 0);
          if (f.ctx.focused() == before) break;
        }
        CHECK(f.ctx.focus_region() == Region::Page);
        ++seen_pages;
        f.key(Nav::PageNext);
        f.run({});
      }
      CHECK(seen_pages == kPageCount && h.st->page == kPlayPage);
    }
}

void test_reduced_motion() {
  for (int page = 0; page < kPageCount; ++page) {
    Harness h("motion", true, true, true);
    h.st->page = page;
    Context ctx;
    ctx.motion.reduced = true;
    Canvas canvas(640, 360);
    auto builder = [&](Context& c) { launcher_frame(c, *h.st); };
    ctx.run(canvas, {}, 0, builder);
    ctx.run(canvas, {}, 0, builder);
    const uint64_t a = canvas.hash();
    const bool animating = ctx.run(canvas, {}, 700, builder);
    CHECK(!animating);              // nothing asks for another frame
    CHECK(canvas.hash() == a);      // and nothing moves with time
  }
  // With motion on, the home page's starfield keeps asking for frames.
  Harness h("motion_on", true, false, false);
  Context ctx;
  ctx.motion.reduced = false;
  Canvas canvas(640, 360);
  auto builder = [&](Context& c) { launcher_frame(c, *h.st); };
  ctx.run(canvas, {}, 0, builder);
  CHECK(ctx.run(canvas, {}, 100, builder));
}

// ---- golden scenes ----
struct Scene {
  const char* name;
  void (*prepare)(Harness&);
  EventList (*events)();
  bool settle_job;
};

void p_home_all(Harness&) {}
void p_home_one(Harness&) {}
void p_import_empty(Harness&) {}
void p_import_running(Harness& h) {
  h.fake.mode = Fake::Block;
  h.st->drop({"/a/disk1.adf", "/a/disk2.adf"});
  wait_for([&] {
    h.st->poll();
    return h.st->job.running && h.st->job.permille == 200;
  });
}
void p_import_error(Harness& h) {
  h.fake.mode = Fake::Fail;
  h.st->drop({"/a/disk1.adf"});
  h.settle();
}
void p_import_more(Harness& h) {
  h.fake.mode = Fake::NeedMore;
  h.st->drop({"/a/disk1.adf"});
  h.settle();
}
void p_patch_offer(Harness& h) {
  h.fake.mode = Fake::PatchOffer;
  h.st->drop({"/a/retail1.adf"});
  h.settle();
}
void p_about(Harness& h) {
  h.st->page = kAboutPage;
  h.st->about.host = {"Linux x86_64", "3.4.18", "wayland", "pipewire", "60 Hz", "gcc 12.2, release"};
  h.st->about.open_url = [](const std::string&) {};
  h.st->about.open_data_folder = [] {};
}
void adv_common(Harness& h) {
  h.st->page = kAdvancedPage;
  AdvancedModel& a = h.st->adv;
  a.copy_text = [](const std::string&) {};
  a.choose_image = [] {};
  a.open_data_folder = [] {};
  a.open_log = [] {};
  a.export_settings = [] {};
  a.import_settings = [] {};
  a.host = {"Linux x86_64", "3.4.18", "wayland", "pipewire", "60 Hz", "gcc 12.2, release"};
  LogBuffer& log = LogBuffer::instance();
  log.close_file();
  log.clear();
  log.add(Severity::Info, "x2recomp 0.5.0 started");
  log.add(Severity::Warn, "settings.cfg: line 4: unknown key 'flavour'");
  log.add(Severity::Info, "import: Imported: Xenon 2 Megablast, retail release");
}
void p_advanced(Harness& h) { adv_common(h); }
void p_advanced_report(Harness& h) {
  adv_common(h);
  h.st->adv.report_title = "Image report";
  h.st->adv.report = {{Severity::Info, "a.adf: ADF: 160 standard tracks, 0 raw, 0 empty of 160"},
                      {Severity::Info, "  key track (cylinder 0, head 1): ordinary Amiga sectors"},
                      {Severity::Warn, "  game image at $70C00: no retail entry or stub"},
                      {Severity::Ok, "Done. No game bytes are shown or copied."}};
  h.st->adv.note = "Checking 1 file.";
}
void p_advanced_reset(Harness& h) { adv_common(h); }
void p_advanced_disclaimer(Harness& h) { adv_common(h); }
void p_args(Harness& h) { h.st->env.args_note = "x2recomp --scale 4 --filter crt"; }
void p_never(Harness& h) { h.st->model.s.launcher = "never"; }
void p_display(Harness& h) {
  h.st->page = kDisplayPage;
  h.st->selected = GameKind::Cdtv;
  h.st->model.game = GameKind::Cdtv;
}

EventList ev_modal() {
  return {};
}

const std::vector<Scene>& scenes() {
  static const std::vector<Scene> v = {
      {"home_all", p_home_all, nullptr, false},       {"home_one", p_home_one, nullptr, false},   {"import_empty", p_import_empty, nullptr, false},
      {"import_running", p_import_running, nullptr, false}, {"import_error", p_import_error, nullptr, false},
      {"import_more", p_import_more, nullptr, false}, {"patch_offer", p_patch_offer, nullptr, false}, {"about", p_about, nullptr, false},
      {"home_args", p_args, nullptr, false},          {"home_never", p_never, nullptr, false},   {"display_cdtv", p_display, nullptr, false},
      {"patch_dialog", p_patch_offer, ev_modal, false}, {"advanced", p_advanced, nullptr, false},
      {"advanced_report", p_advanced_report, nullptr, false}, {"advanced_reset", p_advanced_reset, nullptr, false},
      {"advanced_disclaimer", p_advanced_disclaimer, nullptr, false},
  };
  return v;
}

struct Variant {
  int w, h;
  bool hc;
  const char* tag;
};
const Variant kVariants[] = {{640, 360, false, "640x360"}, {480, 270, false, "480x270"}, {640, 360, true, "640x360hc"}, {480, 270, true, "480x270hc"}};

Canvas render(const Scene& s, const Variant& v) {
  const bool one = std::string(s.name) == "home_one";
  const bool empty = std::string(s.name).rfind("import_", 0) == 0 || std::string(s.name) == "patch_offer" || std::string(s.name) == "patch_dialog";
  Harness h((std::string("gold_") + s.name).c_str(), !empty, !empty && !one, !empty && !one, empty);
  h.st->env.version = "0.5.0";
  s.prepare(h);
  Context ctx;
  ctx.theme = v.hc ? Theme::high_contrast_theme() : Theme::standard();
  ctx.motion.reduced = true;
  Canvas canvas(v.w, v.h);
  auto builder = [&](Context& c) { launcher_frame(c, *h.st); };
  ctx.run(canvas, {}, 0, builder);
  if (std::string(s.name) == "advanced_reset" || std::string(s.name) == "advanced_disclaimer") {
    ctx.set_focus(ctx.id(std::string(s.name) == "advanced_reset" ? "Reset all settings..." : "Read the patch disclaimer"));
    ctx.run(canvas, {}, 0, builder);
    ctx.run(canvas, {Event::nav_key(Nav::Accept)}, 0, builder);
  }
  if (std::string(s.name) == "patch_dialog") {
    // Open the dialog the way a user does: focus the offer button and press Accept.
    for (const FocusItem& it : ctx.items())
      if (it.rect.w == 272 && it.rect.h == 22) ctx.set_focus(it.id);
    ctx.run(canvas, {}, 0, builder);
    ctx.run(canvas, {Event::nav_key(Nav::Accept)}, 0, builder);
  }
  ctx.run(canvas, {}, 0, builder);
  if (std::string(s.name) == "import_running") {
    h.fake.release = true;
    h.settle();
  }
  return canvas;
}

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

}  // namespace

int main(int argc, char** argv) {
  bool update = false;
  std::string hashes, outdir, dump_all, work;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--update") update = true;
    else if (a == "--dump" && i + 1 < argc) dump_all = argv[++i];
    else if (hashes.empty()) hashes = a;
    else if (outdir.empty()) outdir = a;
    else work = a;
  }
  g_work = work.empty() ? fs::temp_directory_path() / "x2_launcher_tests" : fs::path(work);
  std::error_code ec;
  fs::remove_all(g_work, ec);
  fs::create_directories(g_work, ec);

  std::map<std::string, std::pair<Canvas, uint64_t>> rendered;
  for (const Scene& s : scenes())
    for (const Variant& v : kVariants) {
      Canvas c = render(s, v);
      const uint64_t h = c.hash();
      rendered.emplace(std::string(s.name) + "_" + v.tag, std::make_pair(std::move(c), h));
    }
  if (!dump_all.empty()) {
    fs::create_directories(dump_all, ec);
    for (const auto& [name, r] : rendered)
      if (!write_png(r.first, dump_all + "/" + name + ".png", 2)) {
        std::fprintf(stderr, "cannot write %s/%s.png\n", dump_all.c_str(), name.c_str());
        return 1;
      }
    std::printf("wrote %zu pictures to %s\n", rendered.size(), dump_all.c_str());
    return 0;
  }
  if (hashes.empty()) {
    std::fprintf(stderr, "usage: x2_launcher_tests HASHES [OUTDIR [WORKDIR]] | --update HASHES | --dump DIR\n");
    return 2;
  }
  if (update) {
    std::ofstream f(hashes, std::ios::trunc);
    f << "# Golden hashes of the launcher (tests/ui/launcher_tests.cpp). One line per scene: name, width, height, FNV-1a 64 of the canvas.\n"
         "# Regenerate with: x2_launcher_tests --update tests/ui/golden/launcher_hashes.txt, then review the pictures with --dump.\n";
    for (const auto& [name, r] : rendered) f << name << ' ' << r.first.width() << ' ' << r.first.height() << ' ' << hex64(r.second) << '\n';
    std::printf("updated %s (%zu scenes)\n", hashes.c_str(), rendered.size());
    return f ? 0 : 1;
  }

  struct T {
    const char* name;
    void (*fn)();
  };
  const T tests[] = {{"library", test_library},
                     {"worker", test_worker},
                     {"import_job", test_import_job},
                     {"state_flow", test_state_flow},
                     {"patch_safeguards", test_patch_safeguards},
                     {"play_and_settings", test_play_and_settings},
                     {"cards_and_navigation", test_cards_and_navigation},
                     {"import_view", test_import_view},
                     {"patch_dialog", test_patch_dialog},
                     {"packs_page", test_packs_page},
                     {"reachability", test_reachability},
                     {"log_buffer", test_log_buffer},
                     {"diagnostics", test_diagnostics},
                     {"test_tone", test_test_tone},
                     {"advanced_model", test_advanced_model},
                     {"advanced_page", test_advanced_page},
                     {"pad_walk", test_pad_walk},
                     {"reduced_motion", test_reduced_motion}};
  for (const T& t : tests) {
    const int before = g_failures;
    t.fn();
    std::printf("%-22s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
  }
  std::printf("%d checks, %d failures\n", g_checks, g_failures);

  std::map<std::string, std::string> expected;
  {
    std::ifstream f(hashes);
    if (!f) {
      std::fprintf(stderr, "cannot read %s\n", hashes.c_str());
      return 1;
    }
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ls(line);
      std::string name, w, h, hash;
      ls >> name >> w >> h >> hash;
      expected[name] = hash;
    }
  }
  int bad = 0;
  for (const auto& [name, r] : rendered) {
    const auto it = expected.find(name);
    const std::string got = hex64(r.second);
    if (it == expected.end() || it->second != got) {
      ++bad;
      std::fprintf(stderr, "MISMATCH %s: expected %s, got %s\n", name.c_str(), it == expected.end() ? "(none)" : it->second.c_str(), got.c_str());
      if (!outdir.empty()) {
        fs::create_directories(outdir, ec);
        write_png(r.first, outdir + "/" + name + ".png", 2);
      }
    }
  }
  for (const auto& [name, hash] : expected)
    if (!rendered.count(name)) {
      ++bad;
      std::fprintf(stderr, "STALE golden %s\n", name.c_str());
    }
  if (bad) std::fprintf(stderr, "%d of %zu golden scenes differ (update with --update after reviewing the pictures)\n", bad, rendered.size());
  else std::printf("all %zu golden scenes match\n", rendered.size());
  return (g_failures || bad) ? 1 : 0;
}
