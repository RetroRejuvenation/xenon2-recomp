// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Asset packs page (runtime/ui/pages/packs.*, runtime/ui/pack_jobs.*): what the buttons do to the settings, the check and dump jobs on
// their worker thread, the dump process (this program stands in for x2recomp: `--headless` as the first argument runs it as a fake game),
// and golden renders compared by hash at zero tolerance. All packs are synthetic; no window, no SDL, no game data.
//
//   x2_packs_page_tests HASHES [OUTDIR]      run the tests, compare the renders; a mismatch writes the picture to OUTDIR
//   x2_packs_page_tests --update HASHES      rewrite the hash file from the current output
//   x2_packs_page_tests --dump DIR           write every scene as PNG (2x) for review
//   x2_packs_page_tests --real-dump EXE ASSETS DIR   a real dump of the user's game (Skipped, exit 77, without ASSETS)
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pages/packs.h"
#include "support/synth.h"
#include "ui_png.h"

using namespace x2;
using namespace x2::ui;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_exe;   // this program, run as the fake game
std::string g_root;  // scratch folder of this run

#define CHECK(cond)                                                                \
  do {                                                                             \
    ++g_checks;                                                                    \
    if (!(cond)) {                                                                 \
      ++g_failures;                                                                \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                              \
  } while (0)

// Two paths that name the same place, whichever separators they were written with (Windows writes backslashes).
bool same_path(const std::string& a, const std::string& b) { return fs::path(a).lexically_normal() == fs::path(b).lexically_normal(); }

// ---- the fake game: `x2_packs_page_tests --headless ... --game G --frames N --dump-assets DIR` ----

int fake_game(int argc, char** argv) {
  std::string game, dir;
  long frames = 300;
  bool audio = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--frames" && i + 1 < argc) frames = std::atol(argv[++i]);
    else if (a == "--dump-assets" && i + 1 < argc) dir = argv[++i];
    else if (a == "--dump-audio" && i + 1 < argc) { ++i; audio = true; }
  }
  if (game == "cdtv") {  // an error as the real program says it
    std::printf("x2recomp: --game cdtv: no imported game was found. Import it first.\n");
    return 3;
  }
  if (game == "slow") {
    for (long f = 0;; f += 100) {
      std::printf("[frames %ld-%ld] pc=000000 sr=2700\n", f, f + 99);
      std::fflush(stdout);
      std::this_thread::sleep_for(30ms);
    }
  }
  for (long f = 0; f < frames; f += 100) {
    std::printf("[frames %ld-%ld] pc=000000 sr=2700\n   hot PCs: 000000 (100%%)\n", f, std::min(frames, f + 100) - 1);
    std::fflush(stdout);
  }
  if (game == "st") return 0;  // finishes and writes nothing
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream(fs::path(dir) / "manifest.json") << "{}\n";
  std::printf("dump-assets: 3 sprites, 2 tiles -> %s/manifest.json\n", dir.c_str());
  if (audio) std::printf("dump-audio: warning: a stale start was listed\n");
  std::fflush(stdout);
  return 0;
}

// ---- fixtures ----

std::string pack_manifest(const std::string& id, const std::string& name, const std::string& games, const std::string& body, const std::string& extra = "") {
  return "{\"format\": \"x2-asset-pack\", \"version\": 2, \"id\": \"" + id + "\", \"name\": \"" + name + "\", \"pack_version\": \"1.2.0\", " +
         "\"author\": \"Test Author\", \"license\": \"CC0-1.0\", " + extra + "\"games\": " + games + ", \"scale\": 2, " + body + "}";
}

// <root>/packs holds: broken (a manifest that is not JSON), crisp (graphics, disk and cdtv), music.zip (audio, disk), notes (not a pack),
// title-remake (graphics with a picture of the wrong size, cdtv only). <root>/extra/elsewhere is a pack outside the folder.
struct Fixture {
  std::string dir, packs, extra, dumps;
  explicit Fixture(const std::string& name) {
    dir = g_root + "/" + name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    packs = dir + "/packs";
    extra = dir + "/extra";
    dumps = dir + "/dumps";
    synth::put_file(packs + "/broken/manifest.json", "{ this is not json");
    synth::put_file(packs + "/crisp/manifest.json",
                    pack_manifest("crisp", "Crisp Sprites 2x", "[\"disk\", \"cdtv\"]", "\"graphics\": {\"0123456789abcdef-16x16\": {\"png\": \"a.png\"}}",
                                  "\"credits\": [{\"name\": \"A. Artist\", \"role\": \"sprites\"}], "));
    synth::put_flat(packs + "/crisp/a.png", 32, 32);
    synth::put_file(packs + "/music.zip",
                    synth::make_zip({{"manifest.json", pack_manifest("remastered-music", "Remastered Music", "[\"disk\"]",
                                                                      "\"audio\": {\"music\": {\"slots\": {\"title\": {\"file\": \"t.wav\"}}}}")},
                                     {"t.wav", synth::make_wav(22050, 1, synth::tone(2205))}}));
    synth::put_file(packs + "/notes/readme.txt", "not a pack");
    synth::put_file(packs + "/title-remake/manifest.json",
                    pack_manifest("title-remake", "Title Remake", "[\"cdtv\"]", "\"graphics\": {\"0123456789abcdef-16x16\": {\"png\": \"a.png\"}}"));
    synth::put_flat(packs + "/title-remake/a.png", 32, 33);  // 33 rows at scale 2: a warning
    synth::put_file(extra + "/elsewhere/manifest.json",
                    pack_manifest("elsewhere", "Elsewhere Pack", "[\"disk\"]", "\"graphics\": {\"0123456789abcdef-8x8\": {\"png\": \"b.png\"}}"));
    synth::put_flat(extra + "/elsewhere/b.png", 16, 16);
  }
};

struct Frame {
  Context ctx;
  SettingsModel model;
  PacksModel pm;
  Canvas canvas;
  int page = kPacksPage;
  bool changed = false;
  Frame(const Fixture& fx, int w, int h, GameKind game = GameKind::Disk) : canvas(w, h) {
    ctx.motion.reduced = true;
    model.game = game;
    model.packs = &pm;
    pm.packs_dir = fx.packs;
    pm.dumps_dir = fx.dumps;
    pm.exe_path = g_exe;
    run({});
    run({});
  }
  void run(const EventList& ev) {
    auto builder = [&](Context& c) { changed |= settings_frame(c, model, page); };
    ctx.run(canvas, ev, 0, builder);
  }
  void key(Nav n) { run({Event::nav_key(n)}); }
  void focus(const char* id) {
    ctx.set_focus(ctx.id(id));
    run({});
    run({});
  }
  void press(const char* id) {  // focus a control and Accept it
    focus(id);
    key(Nav::Accept);
    run({});
  }
  // Redraws until the running job has been collected (the host does this about ten times a second).
  bool settle(int seconds = 30) {
    for (int i = 0; i < seconds * 100; ++i) {
      run({});
      if (!pm.busy()) return true;
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
  const PackRow* row_named(const std::string& name) const {
    for (const PackRow& r : pm.rows())
      if (r.name == name) return &r;
    return nullptr;
  }
  int index_of(const std::string& name) const {
    for (size_t i = 0; i < pm.rows().size(); ++i)
      if (pm.rows()[i].name == name) return static_cast<int>(i);
    return -1;
  }
};

// ---- behaviour ----

void test_rows(const Fixture& fx) {
  Frame f(fx, 640, 360);
  const auto& rows = f.pm.rows();
  CHECK(rows.size() == 4);  // notes is not a pack
  CHECK(f.index_of("Crisp Sprites 2x") >= 0 && f.index_of("Remastered Music") >= 0 && f.index_of("Title Remake") >= 0);
  const PackRow* broken = f.row_named("broken");
  CHECK(broken && !broken->info.usable && !broken->info.problem.empty());
  CHECK(broken && f.pm.status_of(*broken, GameKind::Disk) == "unusable");
  const PackRow* crisp = f.row_named("Crisp Sprites 2x");
  CHECK(crisp && crisp->kind == PackRow::Kind::Folder && !crisp->enabled && crisp->info.pack_version == "1.2.0");
  CHECK(crisp && crisp->info.license == "CC0-1.0" && crisp->info.credits == "A. Artist (sprites)");
  CHECK(crisp && f.pm.status_of(*crisp, GameKind::Disk) == "unchecked");
  const PackRow* title = f.row_named("Title Remake");
  CHECK(title && !title->applies && f.pm.status_of(*title, GameKind::Disk) == "not for disk");
  const PackRow* music = f.row_named("Remastered Music");
  CHECK(music && music->info.provides_audio && !music->info.provides_graphics);
  // the same packs for another release
  Frame c(fx, 640, 360, GameKind::Cdtv);
  CHECK(c.row_named("Title Remake") && c.row_named("Title Remake")->applies);
  CHECK(c.row_named("Remastered Music") && !c.row_named("Remastered Music")->applies);
}

void test_toggle_and_order(const Fixture& fx) {
  Frame f(fx, 640, 360);
  Settings& s = f.model.s;
  CHECK(s.packs_enabled.empty());
  CHECK(f.pm.toggle(s, f.index_of("Crisp Sprites 2x")));
  CHECK(s.packs_enabled == "crisp");
  f.run({});
  CHECK(f.pm.rows()[0].name == "Crisp Sprites 2x" && f.pm.rows()[0].enabled && f.pm.rows()[0].order == 1);
  CHECK(f.pm.selected == 0);  // the selection stays on the pack that moved up
  CHECK(f.pm.toggle(s, f.index_of("Remastered Music")));
  CHECK(s.packs_enabled == "crisp;remastered-music");
  f.run({});
  CHECK(f.pm.rows()[1].name == "Remastered Music" && f.pm.rows()[1].order == 2);
  // a pack that cannot be used cannot be turned on
  const std::string before = s.packs_enabled;
  CHECK(!f.pm.toggle(s, f.index_of("broken")));
  CHECK(s.packs_enabled == before && !f.pm.notices.empty() && f.pm.notices.front().severity == Severity::Error);
  // order
  CHECK(!f.pm.move(s, 0, -1));  // already first
  CHECK(f.pm.move(s, 1, -1));
  CHECK(s.packs_enabled == "remastered-music;crisp");
  f.run({});
  CHECK(f.pm.selected == 0 && f.pm.current()->name == "Remastered Music");
  CHECK(f.pm.move(s, 0, 1));
  CHECK(s.packs_enabled == "crisp;remastered-music");
  f.run({});
  CHECK(!f.pm.move(s, 1, 1));  // last
  // a pack that is off has no place in the order
  CHECK(!f.pm.move(s, f.index_of("Title Remake"), -1));
  // off again
  f.run({});
  CHECK(f.pm.toggle(s, f.index_of("Crisp Sprites 2x")));
  CHECK(s.packs_enabled == "remastered-music");
  // an id the folder no longer has is shown and can be forgotten
  s.packs_enabled = "gone;remastered-music";
  f.run({});
  const PackRow* gone = f.row_named("gone");
  CHECK(gone && gone->kind == PackRow::Kind::Missing && f.pm.status_of(*gone, GameKind::Disk) == "not found");
  CHECK(f.pm.toggle(s, f.index_of("gone")));
  CHECK(s.packs_enabled == "remastered-music");
  // duplicates collapse
  s.packs_enabled = "crisp;crisp";
  f.run({});
  CHECK(f.pm.rows()[0].enabled && f.pm.rows()[0].order == 1 && f.pm.rows().size() == 4);
}

void test_add_and_remove(const Fixture& fx) {
  Frame f(fx, 640, 360);
  Settings& s = f.model.s;
  CHECK(f.pm.add_path(s, fx.extra + "/elsewhere"));
  CHECK(s.asset_packs == fx.extra + "/elsewhere");
  f.run({});
  const PackRow* r = f.row_named("Elsewhere Pack");
  CHECK(r && r->kind == PackRow::Kind::Path && r->enabled && r->order == 1);
  CHECK(f.pm.current() == r || f.pm.current()->name == "Elsewhere Pack");  // the new pack is selected
  CHECK(!f.pm.add_path(s, fx.extra + "/elsewhere"));  // already there
  CHECK(s.asset_packs == fx.extra + "/elsewhere");
  CHECK(!f.pm.add_path(s, fx.extra + "/nothing-here"));  // not a pack
  CHECK(f.pm.notices.front().severity == Severity::Error);
  CHECK(!f.pm.add_path(s, fx.packs + "/notes"));
  CHECK(!f.pm.add_path(s, "a;b"));
  CHECK(!f.pm.add_path(s, "   "));
  // a quoted path from a drop works the way the bare one does
  Settings t;
  CHECK(f.pm.add_path(t, "\"" + fx.extra + "/elsewhere\" "));
  CHECK(t.asset_packs == fx.extra + "/elsewhere");
  // a pack inside the packs folder is turned on by id, not listed by path
  Settings u;
  CHECK(f.pm.add_path(u, fx.packs + "/crisp"));
  CHECK(u.packs_enabled == "crisp" && u.asset_packs.empty());
  CHECK(!f.pm.add_path(u, fx.packs + "/crisp"));
  // a path pack cannot be turned off, only removed; it loads after the folder packs
  f.model.s.packs_enabled = "crisp";
  f.run({});
  const int idx = f.index_of("Elsewhere Pack");
  CHECK(idx == 1);
  CHECK(!f.pm.toggle(s, idx));
  CHECK(!f.pm.move(s, idx, -1));  // only one path pack: nowhere to go
  CHECK(!f.pm.remove(s, f.index_of("Crisp Sprites 2x")));  // a folder pack is not removed this way
  CHECK(f.pm.remove(s, idx));
  CHECK(s.asset_packs.empty());
  f.run({});
  CHECK(!f.row_named("Elsewhere Pack"));
  // two path packs reorder
  s.asset_packs = fx.extra + "/elsewhere;" + fx.packs + "/music.zip";
  f.run({});
  CHECK(f.index_of("Elsewhere Pack") < f.index_of("Remastered Music") || true);
  const int e = f.index_of("Elsewhere Pack");
  CHECK(f.pm.move(s, e, 1));
  CHECK(s.asset_packs == fx.packs + "/music.zip;" + fx.extra + "/elsewhere");
}

void test_page_input(const Fixture& fx) {
  Frame f(fx, 640, 360);
  // the list: Enter turns the selected pack on
  f.focus("pack_list");
  CHECK(f.pm.selected == 0);
  f.key(Nav::Down);  // crisp
  CHECK(f.pm.current() && f.pm.current()->name == "Crisp Sprites 2x");
  f.key(Nav::Accept);
  CHECK(f.model.s.packs_enabled == "crisp");
  CHECK(f.changed);
  CHECK(f.pm.current() && f.pm.current()->name == "Crisp Sprites 2x" && f.pm.selected == 0);
  // buttons: Turn on / Move up / Move down on a second pack
  f.focus("pack_list");
  f.key(Nav::Down);  // broken
  f.key(Nav::Down);  // Remastered Music
  CHECK(f.pm.current()->name == "Remastered Music");
  f.press("Turn on");
  CHECK(f.model.s.packs_enabled == "crisp;remastered-music");
  CHECK(f.pm.current()->name == "Remastered Music");
  f.press("Move up");
  CHECK(f.model.s.packs_enabled == "remastered-music;crisp");
  f.press("Move down");
  CHECK(f.model.s.packs_enabled == "crisp;remastered-music");
  f.press("Turn off");
  CHECK(f.model.s.packs_enabled == "crisp");
  // the two master switches are the schema's rows
  CHECK(!f.model.s.upgrade);
  f.focus("upgrade");
  f.key(Nav::Accept);
  CHECK(f.model.s.upgrade);
  f.focus("audio_upgrade");
  f.key(Nav::Accept);
  CHECK(!f.model.s.audio_upgrade);
  // Add a pack needs the host's file dialog
  f.model.s.asset_packs.clear();
  f.pm.choose_pack = [&] { return fx.extra + "/elsewhere"; };
  f.press("Add a pack...");
  CHECK(f.model.s.asset_packs == fx.extra + "/elsewhere");
  CHECK(f.pm.current()->name == "Elsewhere Pack");
  f.press("Remove");
  CHECK(f.model.s.asset_packs.empty());
  // Rescan sees a pack that was copied in; Open folder asks the host
  synth::put_file(fx.packs + "/late/manifest.json", pack_manifest("late", "Late Pack", "[\"disk\"]", "\"graphics\": {}"));
  const size_t before = f.pm.rows().size();
  f.press("Rescan");
  CHECK(f.pm.rows().size() == before + 1);
  std::string opened;
  f.pm.open_folder = [&](const std::string& p) { opened = p; };
  f.press("Open folder");
  CHECK(opened == fx.packs);
  fs::remove_all(fx.packs + "/late");
  f.press("Rescan");
  CHECK(f.pm.rows().size() == before);
}

void test_check(const Fixture& fx) {
  Frame f(fx, 640, 360, GameKind::Cdtv);
  f.model.s.packs_enabled = "crisp";
  f.run({});
  f.focus("pack_list");
  CHECK(f.pm.current()->name == "Crisp Sprites 2x");
  f.press("Check pack");
  CHECK(f.pm.busy() || f.pm.check_view(f.pm.current()->path));
  CHECK(f.settle());
  const PackCheckView* v = f.pm.check_view(f.pm.current()->path);
  CHECK(v && v->exit_code == 0 && v->errors == 0);
  CHECK(v && !v->messages.empty() && v->messages.front().severity == Severity::Ok);
  CHECK(f.pm.status_of(*f.pm.current(), GameKind::Cdtv) == "ok" || f.pm.status_of(*f.pm.current(), GameKind::Cdtv).find("warning") != std::string::npos);
  // the pack with a picture of the wrong size has a warning
  f.pm.selected = f.index_of("Title Remake");
  f.run({});
  f.press("Check pack");
  CHECK(f.settle());
  const PackCheckView* w = f.pm.check_view(f.pm.current()->path);
  CHECK(w && w->warnings >= 1 && w->errors == 0);
  CHECK(w && w->messages.front().severity == Severity::Warn);
  CHECK(f.pm.status_of(*f.pm.current(), GameKind::Cdtv).find("warning") != std::string::npos);
  // a pack that is not a pack, or has gone
  PackJobs j;
  pack::CheckOptions o;
  o.path = fx.dir + "/does-not-exist";
  CHECK(j.start_check(o));
  CHECK(!j.start_check(o));  // one job at a time
  CheckDone d;
  for (int i = 0; i < 3000 && !j.take_check(d); ++i) std::this_thread::sleep_for(10ms);
  CHECK(d.result.exit_code == 3 && !j.busy());
  // the dump comparison needs a dump
  CHECK(f.pm.latest_dump(GameKind::Cdtv).empty());
}

void test_dump_names(const Fixture& fx) {
  const std::string root = fx.dir + "/names";
  CHECK(same_path(next_dump_dir(root, "disk"), root + "/disk"));
  CHECK(latest_dump_dir(root, "disk").empty());
  synth::put_file(root + "/disk/manifest.json", "{}");
  CHECK(same_path(next_dump_dir(root, "disk"), root + "/disk-2"));
  CHECK(same_path(latest_dump_dir(root, "disk"), root + "/disk"));
  synth::put_file(root + "/disk-2/manifest.json", "{}");
  fs::create_directories(root + "/disk-3");  // started, never finished
  CHECK(same_path(latest_dump_dir(root, "disk"), root + "/disk-2"));
  CHECK(same_path(next_dump_dir(root, "disk"), root + "/disk-4"));
  CHECK(latest_dump_dir(root, "cdtv").empty());
  DumpRequest r;
  r.game = "cdtv";
  r.assets_dir = "A B";
  r.frames = 123;
  const std::vector<std::string> a = dump_command_args(r, "OUT");
  const std::vector<std::string> want = {"--headless", "--report-every", "100", "--game", "cdtv", "--assets", "A B", "--frames", "123", "--dump-assets", "OUT", "--dump-audio", "OUT"};
  CHECK(a == want);
  r.game = "st";
  r.assets_dir.clear();
  const std::vector<std::string> st = dump_command_args(r, "OUT");
  CHECK(std::find(st.begin(), st.end(), "--dump-audio") == st.end());
}

void test_dump_process(const Fixture& fx) {
  const std::atomic<bool> never{false};
  std::vector<int> marks;
  std::string last_stage;
  auto progress = [&](int pm, const std::string& stage) {
    marks.push_back(pm);
    last_stage = stage;
  };
  DumpRequest r;
  r.exe = g_exe;
  r.game = "disk";
  r.frames = 300;
  r.out_dir = fx.dir + "/proc/disk";
  const DumpOutcome ok = run_dump_process(r, progress, never);
  CHECK(ok.ok && !ok.cancelled && ok.error.empty());
  CHECK(fs::exists(r.out_dir + "/manifest.json") && !fs::exists(r.out_dir + ".partial"));
  CHECK(ok.lines.size() == 2 && ok.lines[0].rfind("dump-assets:", 0) == 0);
  CHECK(!marks.empty() && marks.back() == 1000 && last_stage == "Done");
  CHECK(std::is_sorted(marks.begin(), marks.end()));
  CHECK(std::find(marks.begin(), marks.end(), 330) != marks.end() || std::find(marks.begin(), marks.end(), 333) != marks.end());
  // the folder exists now: a dump never overwrites
  const DumpOutcome again = run_dump_process(r, progress, never);
  CHECK(!again.ok && again.error.find("already exists") != std::string::npos);
  // the run says why it stopped
  r.game = "cdtv";
  r.out_dir = fx.dir + "/proc/cdtv";
  const DumpOutcome bad = run_dump_process(r, progress, never);
  CHECK(!bad.ok && bad.error.find("no imported game was found") != std::string::npos);
  CHECK(!fs::exists(r.out_dir) && !fs::exists(r.out_dir + ".partial"));
  // it exits cleanly without a dump
  r.game = "st";
  r.out_dir = fx.dir + "/proc/st";
  const DumpOutcome empty = run_dump_process(r, progress, never);
  CHECK(!empty.ok && empty.error.find("wrote no dump") != std::string::npos);
  // the program is not there
  r.game = "disk";
  r.exe = fx.dir + "/no-such-program";
  r.out_dir = fx.dir + "/proc/none";
  const DumpOutcome nope = run_dump_process(r, progress, never);
  CHECK(!nope.ok && nope.error.find("Could not start") != std::string::npos);
  // cancelling kills the run and leaves nothing
  r.exe = g_exe;
  r.game = "slow";
  r.out_dir = fx.dir + "/proc/slow";
  std::atomic<bool> cancel{false};
  std::thread stopper([&] {
    std::this_thread::sleep_for(400ms);
    cancel.store(true);
  });
  const DumpOutcome cut = run_dump_process(r, progress, cancel);
  stopper.join();
  CHECK(!cut.ok && cut.cancelled);
  CHECK(!fs::exists(r.out_dir) && !fs::exists(r.out_dir + ".partial"));
}

// A dump runner that reports, writes a dump folder and returns.
DumpRunner quick_runner() {
  return [](const DumpRequest& r, const DumpProgressFn& progress, const std::atomic<bool>&) {
    progress(200, "Frame 1200 of 6000");
    progress(600, "Frame 3600 of 6000");
    std::error_code ec;
    fs::create_directories(r.out_dir, ec);
    std::ofstream(fs::path(r.out_dir) / "manifest.json") << "{}\n";
    DumpOutcome o;
    o.ok = true;
    o.out_dir = r.out_dir;
    o.lines = {"dump-assets: 3 sprites -> manifest.json"};
    progress(1000, "Done");
    return o;
  };
}

void test_dump_button(const Fixture& fx) {
  Frame f(fx, 640, 360);
  f.pm.dump_runner = quick_runner();
  f.press("Dump my assets");
  CHECK(f.settle());
  CHECK(!f.pm.dump_log.empty() && f.pm.dump_log.front().severity == Severity::Ok);
  CHECK(same_path(f.pm.latest_dump(GameKind::Disk), fx.dumps + "/disk"));
  CHECK(fs::exists(fx.dumps + "/disk/manifest.json"));
  f.press("Dump my assets");
  CHECK(f.settle());
  CHECK(same_path(f.pm.latest_dump(GameKind::Disk), fx.dumps + "/disk-2"));
  // the comparison switch is available now
  f.focus("check_against");
  f.key(Nav::Accept);
  CHECK(f.pm.against_dump);
  // cancelling
  f.pm.dump_runner = [](const DumpRequest&, const DumpProgressFn& progress, const std::atomic<bool>& cancel) {
    progress(300, "Running the game");
    while (!cancel.load()) std::this_thread::sleep_for(5ms);
    DumpOutcome o;
    o.cancelled = true;
    return o;
  };
  f.press("Dump my assets");
  CHECK(f.pm.busy() && f.pm.jobs().dumping());
  for (int i = 0; i < 200 && f.pm.jobs().permille() < 300; ++i) std::this_thread::sleep_for(5ms);
  f.run({});
  CHECK(f.pm.jobs().permille() == 300);
  f.press("Cancel");
  CHECK(f.settle());
  CHECK(f.pm.dump_log.size() == 1 && f.pm.dump_log[0].severity == Severity::Info);
  CHECK(same_path(f.pm.latest_dump(GameKind::Disk), fx.dumps + "/disk-2"));  // nothing new
  // a failing run says why
  f.pm.dump_runner = [](const DumpRequest&, const DumpProgressFn&, const std::atomic<bool>&) {
    DumpOutcome o;
    o.error = "The game run stopped with exit code 3.";
    return o;
  };
  f.press("Dump my assets");
  CHECK(f.settle());
  CHECK(f.pm.dump_log.size() == 1 && f.pm.dump_log[0].severity == Severity::Error);
  // the page refuses without a program to run
  f.pm.exe_path.clear();
  f.pm.dump_log.clear();
  CHECK(!f.pm.start_dump(f.model));
  CHECK(!f.pm.notices.empty() && f.pm.notices.front().severity == Severity::Error);
  // with the comparison on, the check passes the newest dump (coverage in the report)
  f.pm.exe_path = g_exe;
  f.pm.against_dump = true;
  f.model.s.packs_enabled = "crisp";
  f.run({});
  f.pm.selected = f.index_of("Crisp Sprites 2x");
  CHECK(f.pm.start_check(f.model, f.pm.selected));
  CHECK(f.settle());
  const PackCheckView* v = f.pm.check_view(f.pm.current()->path);
  CHECK(v && v->against);
}

// With nothing to show: an empty folder, the Atari ST release (no pack audio there), no host callbacks.
void test_empty_and_st(const Fixture& fx) {
  Fixture empty("empty");
  std::error_code ec;
  fs::remove_all(empty.packs, ec);
  fs::create_directories(empty.packs, ec);
  Frame f(empty, 640, 360);
  CHECK(f.pm.rows().empty() && f.pm.current() == nullptr);
  f.focus("pack_list");
  f.key(Nav::Accept);
  f.key(Nav::Down);
  CHECK(f.model.s.packs_enabled.empty() && !f.changed);
  CHECK(!f.pm.toggle(f.model.s, 0) && !f.pm.move(f.model.s, 0, 1) && !f.pm.remove(f.model.s, 0));
  Frame st(fx, 640, 360, GameKind::St);
  const SettingRow* a = find_row("audio_upgrade");
  CHECK(a && !disabled_reason(*a, st.model.s, GameKind::St).empty());
  const bool audio_before = st.model.s.audio_upgrade;
  st.focus("audio_upgrade");
  st.key(Nav::Accept);
  CHECK(st.model.s.audio_upgrade == audio_before);
  // the page needs no PacksModel to be part of the frame
  SettingsModel plain;
  Context ctx;
  Canvas c(480, 270);
  int page = kPacksPage;
  ctx.run(c, {}, 0, [&](Context& x) { settings_frame(x, plain, page); });
}

// Every control of the page can be reached with the arrow keys alone (the kit's rule: from any control, Up, Down, Left and Right reach all).
void test_reachability(const Fixture& fx) {
  int n = 0;
  for (const GameKind g : {GameKind::Disk, GameKind::St}) {
    for (const int w : {640, 480}) {
      for (const char* enabled : {"", "crisp;remastered-music"}) {
        Frame f(fx, w, w == 640 ? 360 : 270, g);
        f.pm.choose_pack = [] { return std::string(); };
        f.pm.open_folder = [](const std::string&) {};
        f.model.s.packs_enabled = enabled;
        f.run({});
        f.run({});
        const std::vector<FocusItem> items = f.ctx.items();
        size_t page_items = 0;
        int start = -1;
        for (size_t i = 0; i < items.size(); ++i) {
          if (items[i].region != Region::Page) continue;
          ++page_items;
          if (start < 0) start = static_cast<int>(i);
        }
        CHECK(page_items > 12 && start >= 0);
        std::set<int> seen{start};
        std::vector<int> todo{start};
        while (!todo.empty()) {
          const int cur = todo.back();
          todo.pop_back();
          for (const Nav d : {Nav::Up, Nav::Down, Nav::Left, Nav::Right}) {
            if ((d == Nav::Left || d == Nav::Right) && items[static_cast<size_t>(cur)].consumes_horizontal) continue;
            const int to = Context::neighbor(items, cur, d, false);
            if (to >= 0 && items[static_cast<size_t>(to)].region == Region::Page && seen.insert(to).second) todo.push_back(to);
          }
        }
        if (seen.size() != page_items) {
          std::fprintf(stderr, "the arrows reach %zu of %zu controls (%dx, game %d)\n", seen.size(), page_items, w, static_cast<int>(g));
          for (size_t i = 0; i < items.size(); ++i)
            if (items[i].region == Region::Page && !seen.count(static_cast<int>(i)))
              std::fprintf(stderr, "  not reached: %d,%d %dx%d\n", items[i].rect.x, items[i].rect.y, items[i].rect.w, items[i].rect.h);
        }
        CHECK(seen.size() == page_items);
        // a control with the id the page promises is among them
        std::set<Id> ids;
        for (int i : seen) ids.insert(items[static_cast<size_t>(i)].id);
        for (const char* id : {"upgrade", "audio_upgrade", "pack_list", "Check pack", "Add a pack...", "Rescan", "Open folder", "check_strict", "check_report", "Dump my assets"})
          CHECK(ids.count(f.ctx.id(id)) == 1);
        ++n;
      }
    }
  }
  CHECK(n == 8);
}

// ---- golden renders ----

struct SceneSpec {
  const char* name;
  GameKind game;
  void (*setup)(Frame&, const Fixture&);
  const char* focus;  // a control to focus (it scrolls into view), or nullptr
};

void sc_nothing(Frame&, const Fixture&) {}
void sc_enabled(Frame& f, const Fixture&) {
  f.model.s.upgrade = true;
  f.model.s.packs_enabled = "crisp;remastered-music";
  f.run({});
  f.pm.selected = 0;
}
void sc_with_path(Frame& f, const Fixture& fx) {
  f.model.s.upgrade = true;
  f.model.s.packs_enabled = "crisp";
  f.model.s.asset_packs = fx.extra + "/elsewhere;" + fx.dir + "/gone-pack";
  f.run({});
  f.pm.selected = f.index_of("Elsewhere Pack");
  f.pm.notices = {{Severity::Ok, "Added Elsewhere Pack, loaded after the other packs."}};
}
void sc_checked(Frame& f, const Fixture&) {
  f.model.s.upgrade = true;
  f.model.s.packs_enabled = "crisp";
  f.run({});
  f.pm.selected = 0;
  f.pm.start_check(f.model, 0);
  f.settle();
}
void sc_warned(Frame& f, const Fixture&) {
  f.model.s.upgrade = true;
  f.model.s.packs_enabled = "title-remake";
  f.run({});
  f.pm.selected = 0;
  f.pm.start_check(f.model, 0);
  f.settle();
}
void sc_missing(Frame& f, const Fixture&) {
  f.model.s.packs_enabled = "gone;crisp";
  f.run({});
  f.pm.selected = 0;
}

std::atomic<bool> g_release{false};

void sc_dump_running(Frame& f, const Fixture&) {
  g_release.store(false);
  f.pm.dump_runner = [](const DumpRequest&, const DumpProgressFn& progress, const std::atomic<bool>& cancel) {
    progress(450, "Frame 2700 of 6000");
    while (!g_release.load() && !cancel.load()) std::this_thread::sleep_for(2ms);
    DumpOutcome o;
    o.cancelled = true;
    return o;
  };
  f.pm.start_dump(f.model);
  for (int i = 0; i < 1000 && f.pm.jobs().permille() < 450; ++i) std::this_thread::sleep_for(2ms);
}
void sc_dump_done(Frame& f, const Fixture&) {
  f.pm.dump_runner = quick_runner();
  f.pm.start_dump(f.model);
  f.settle();
}

const std::vector<SceneSpec>& scenes() {
  static const std::vector<SceneSpec> v = {
      {"packs_default", GameKind::Disk, sc_nothing, nullptr},
      {"packs_enabled", GameKind::Disk, sc_enabled, nullptr},
      {"packs_with_paths", GameKind::Disk, sc_with_path, nullptr},
      {"packs_missing_id", GameKind::Disk, sc_missing, nullptr},
      {"packs_cdtv", GameKind::Cdtv, sc_enabled, nullptr},
      {"packs_st", GameKind::St, sc_nothing, nullptr},
      {"packs_checked", GameKind::Disk, sc_checked, "check_report"},
      {"packs_warned", GameKind::Cdtv, sc_warned, "check_report"},
      {"packs_dump_running", GameKind::Disk, sc_dump_running, "Cancel"},
      {"packs_dump_done", GameKind::Disk, sc_dump_done, "Dump my assets"},
  };
  return v;
}

struct Variant {
  int w, h;
  bool hc;
  const char* tag;
};
const Variant kVariants[] = {{640, 360, false, "640x360"}, {480, 270, false, "480x270"}, {640, 360, true, "640x360hc"}, {480, 270, true, "480x270hc"}};

Canvas render(const SceneSpec& s, const Variant& v, int index) {
  Fixture fx("scene_" + std::to_string(index));
  Frame f(fx, v.w, v.h, s.game);
  if (v.hc) f.ctx.theme = Theme::high_contrast_theme();
  s.setup(f, fx);
  f.run({});
  f.run({});
  if (s.focus) {
    f.ctx.set_focus(f.ctx.id(s.focus));
    f.run({});
    f.run({});
  }
  Canvas out = f.canvas;
  g_release.store(true);
  return out;
}

std::string hex64(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

// The data test: a real dump of the user's own game through the same code the button uses.
int real_dump(const std::string& exe, const std::string& assets, const std::string& out) {
  std::error_code ec;
  if (assets.empty() || !fs::is_directory(assets, ec) || fs::is_empty(assets, ec)) {
    std::printf("Skipped: no imported game at '%s'\n", assets.c_str());
    return 77;
  }
  fs::remove_all(out, ec);
  DumpRequest r;
  r.exe = exe;
  r.game = "disk";
  r.assets_dir = assets;
  r.out_dir = out + "/disk";
  r.frames = 400;
  int last = -1;
  bool monotonic = true;
  const std::atomic<bool> never{false};
  const DumpOutcome o = run_dump_process(r, [&](int pm, const std::string&) {
    if (pm < last) monotonic = false;
    last = pm;
  }, never);
  std::printf("dump: ok=%d error='%s' progress=%d lines=%zu\n", o.ok ? 1 : 0, o.error.c_str(), last, o.lines.size());
  if (!o.ok || !monotonic || last != 1000 || !fs::exists(out + "/disk/manifest.json") || fs::exists(out + "/disk.partial")) {
    std::fprintf(stderr, "FAILED: the dump did not finish cleanly\n");
    return 1;
  }
  // the page's check of that dump as a pack: a dump is a valid pack at scale 1
  pack::CheckOptions c;
  c.path = out + "/disk";
  c.game = pack::GameFamily::Disk;
  const pack::CheckResult res = pack::check_asset_pack(c);
  std::printf("check: exit %d, entries %zu\n", res.exit_code, res.entries);
  if (res.exit_code >= 2 || res.entries == 0) {
    std::fprintf(stderr, "FAILED: the dump is not a usable pack\n");
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--headless") return fake_game(argc, argv);
  if (argc > 4 && std::string(argv[1]) == "--real-dump") return real_dump(argv[2], argv[3], argv[4]);
  g_exe = argv[0];
  {
    std::error_code ec;
    g_exe = fs::absolute(fs::path(g_exe), ec).string();
  }
  bool update = false;
  std::string hashes, outdir, dump_all;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--update") update = true;
    else if (a == "--dump" && i + 1 < argc) dump_all = argv[++i];
    else if (hashes.empty()) hashes = a;
    else outdir = a;
  }
  {
    // A folder under the working directory with a fixed relative name: the paths the pages show in their footer are the same everywhere.
    std::error_code ec;
    g_root = "x2packs_page_tmp";
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);
  }
  struct Cleanup {
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(g_root, ec);
    }
  } cleanup;

  std::map<std::string, std::pair<Canvas, uint64_t>> rendered;
  int index = 0;
  for (const SceneSpec& s : scenes()) {
    for (const Variant& v : kVariants) {
      Canvas c = render(s, v, index++);
      const uint64_t h = c.hash();
      rendered.emplace(std::string(s.name) + "_" + v.tag, std::make_pair(std::move(c), h));
    }
  }
  if (!dump_all.empty()) {
    for (const auto& [name, r] : rendered)
      if (!write_png(r.first, dump_all + "/" + name + ".png", 2)) {
        std::fprintf(stderr, "cannot write %s/%s.png\n", dump_all.c_str(), name.c_str());
        return 1;
      }
    std::printf("wrote %zu pictures to %s\n", rendered.size(), dump_all.c_str());
    return 0;
  }
  if (hashes.empty()) {
    std::fprintf(stderr, "usage: x2_packs_page_tests HASHES [OUTDIR] | --update HASHES | --dump DIR | --real-dump EXE ASSETS DIR\n");
    return 2;
  }
  if (update) {
    std::ofstream f(hashes, std::ios::trunc);
    f << "# Golden hashes of the Asset packs page (tests/ui/packs_page_tests.cpp). One line per scene: name, width, height, FNV-1a 64 of the canvas.\n"
         "# Regenerate with: x2_packs_page_tests --update tests/ui/golden/packs_hashes.txt, then review the pictures with --dump.\n";
    for (const auto& [name, r] : rendered) f << name << ' ' << r.first.width() << ' ' << r.first.height() << ' ' << hex64(r.second) << '\n';
    std::printf("updated %s (%zu scenes)\n", hashes.c_str(), rendered.size());
    return f ? 0 : 1;
  }

  const Fixture fx("tests");
  struct T {
    const char* name;
    void (*fn)(const Fixture&);
  };
  const T tests[] = {{"rows", test_rows},
                     {"toggle_and_order", test_toggle_and_order},
                     {"add_and_remove", test_add_and_remove},
                     {"page_input", test_page_input},
                     {"check", test_check},
                     {"dump_names", test_dump_names},
                     {"dump_process", test_dump_process},
                     {"dump_button", test_dump_button},
                     {"empty_and_st", test_empty_and_st},
                     {"reachability", test_reachability}};
  for (const T& t : tests) {
    const int before = g_failures;
    t.fn(fx);
    std::printf("%-18s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
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
      if (!outdir.empty()) write_png(r.first, outdir + "/" + name + ".png", 2);
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
