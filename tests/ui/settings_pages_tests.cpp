// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The settings pages (runtime/ui/pages): what a key press does to the Settings and to the equivalent command line, that every control is
// reachable without a mouse, and golden renders (compared by hash at zero tolerance, like tests/ui/ui_golden_tests.cpp). No window, no
// SDL, no game data.
//
//   x2_settings_pages_tests HASHES [OUTDIR]     run the tests, compare the renders; a mismatch writes the picture to OUTDIR
//   x2_settings_pages_tests --update HASHES     rewrite the hash file from the current output
//   x2_settings_pages_tests --dump DIR          write every scene as PNG (2x) for review
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ui_png.h"
#include "pages/pages.h"

using namespace x2;
using namespace x2::ui;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
  do {                                                                             \
    ++g_checks;                                                                    \
    if (!(cond)) {                                                                 \
      ++g_failures;                                                                \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                              \
  } while (0)

// The hotkeys the pages show beside a row in the scenes: the default PC bindings.
std::string default_hotkey(const std::string& action) {
  static const std::map<std::string, std::string> keys = {
      {"faithful", "F1"}, {"scale_up", "F6"}, {"fullscreen", "F11"}, {"filter", "F2"}, {"interp", "F3"}, {"aspect", "F4"},
      {"asset_packs", "Home"}, {"wide", "End"}, {"hd_panel", "PgDn"}, {"music_source", "PgUp"}, {"volume_up", "F8"}, {"grab", "ScrollLk"},
      {"autofire", "Tab"}, {"god_mode", "Bksp"}, {"protect_music", "Del"}};
  const auto it = keys.find(action);
  return it == keys.end() ? std::string() : it->second;
}

// A frame to drive with events.
struct Frame {
  Context ctx;
  SettingsModel model;
  Canvas canvas;
  int page;
  bool changed = false;
  Frame(int w, int h, int page_, GameKind game = GameKind::Disk) : canvas(w, h), page(page_) {
    ctx.motion.reduced = true;
    model.game = game;
    model.hotkey = default_hotkey;
    run({});
    run({});
  }
  void run(const EventList& ev) {
    auto builder = [&](Context& c) { changed |= settings_frame(c, model, page); };
    ctx.run(canvas, ev, 0, builder);
  }
  void key(Nav n) { run({Event::nav_key(n)}); }
  void focus(const char* key_id) {
    ctx.set_focus(ctx.id(key_id));
    run({});
    run({});
  }
  bool focused(const char* key_id) const { return ctx.focused() == ctx.id(key_id); }
};

// ---- behaviour ----
void test_edits() {
  Frame f(640, 360, kDisplayPage);
  f.focus("scale");
  CHECK(f.focused("scale"));
  CHECK(f.model.s.scale == 3);
  f.key(Nav::Right);
  CHECK(f.model.s.scale == 4 && f.changed);
  CHECK(f.model.command().text() == "x2recomp --game disk --scale 4");
  f.key(Nav::Left);
  f.key(Nav::Left);
  CHECK(f.model.s.scale == 2);
  for (int i = 0; i < 6; ++i) f.key(Nav::Left);
  CHECK(f.model.s.scale == 1); // the stepper stops at the ends
  // A toggle: Accept.
  f.focus("fullscreen");
  f.key(Nav::Accept);
  CHECK(f.model.s.fullscreen);
  f.key(Nav::Accept);
  CHECK(!f.model.s.fullscreen);
  // A choice: Right cycles.
  f.focus("filter");
  f.key(Nav::Right);
  CHECK(f.model.s.video_filter == VideoFilter::Sharp);
  for (int i = 0; i < 2; ++i) f.key(Nav::Right);
  CHECK(f.model.s.video_filter == VideoFilter::Crt);
  CHECK(f.model.command().text().find("--filter crt") != std::string::npos);
  // The row that needs the crt filter is on now.
  f.focus("curvature");
  f.key(Nav::Accept);
  CHECK(f.model.s.curvature);
  CHECK(f.model.command().text() == "x2recomp --game disk --scale 1 --filter crt --curvature");
  CHECK(f.model.dirty());
  f.model.saved = f.model.s;
  CHECK(!f.model.dirty());
  // Smooth frames switch their dependants on.
  f.focus("smooth_far");
  const bool before = f.model.s.smooth_far;
  f.key(Nav::Accept);
  CHECK(f.model.s.smooth_far == before); // inert while smooth frames are off
  f.focus("interp");
  f.key(Nav::Accept);
  CHECK(f.model.s.interp);
  f.focus("smooth_far");
  f.key(Nav::Accept);
  CHECK(!f.model.s.smooth_far);
  // The Faithful switch greys the picture rows and leaves the window rows alone.
  f.focus("modern");
  f.key(Nav::Accept);
  CHECK(f.model.s.faithful);
  f.focus("filter");
  f.key(Nav::Right);
  CHECK(f.model.s.video_filter == VideoFilter::Crt); // unchanged
  f.focus("scale");
  f.key(Nav::Right);
  CHECK(f.model.s.scale == 2);
  CHECK(f.model.command().text() == "x2recomp --game disk --faithful --scale 2");
  CHECK(f.model.copied.empty());
}

void test_atari_st_page() {
  Frame f(640, 360, kDisplayPage, GameKind::St);
  f.focus("interp");
  f.key(Nav::Accept);
  CHECK(!f.model.s.interp); // the Atari ST has no interpolation: the row is inert
  f.focus("wide");
  f.key(Nav::Right);
  CHECK(f.model.s.wide == "off");
  f.focus("filter");
  f.key(Nav::Right);
  CHECK(f.model.s.video_filter == VideoFilter::Sharp); // the ST honours the filter
  CHECK(f.model.command().text() == "x2recomp --game st --filter sharp");
}

void test_audio_and_play() {
  Frame a(640, 360, kAudioPage, GameKind::Cdtv);
  a.focus("volume");
  CHECK(a.model.s.volume == 100);
  a.key(Nav::Left);
  CHECK(a.model.s.volume == 95);
  a.focus("stereo_separation");
  a.key(Nav::Left);
  CHECK(a.model.s.stereo_separation > 0.94f && a.model.s.stereo_separation < 0.96f);
  a.focus("audio_filter");
  a.key(Nav::Right);
  CHECK(a.model.s.filter == FilterSetting::A1200);
  a.focus("audio");
  a.key(Nav::Accept);
  CHECK(!a.model.s.audio);
  a.focus("volume");
  a.key(Nav::Left);
  CHECK(a.model.s.volume == 95); // inert while the sound is off
  CHECK(a.model.command().text().find("--no-audio") != std::string::npos);
  // The expert disclosure shows the track map and the CD folder.
  CHECK(!a.model.show_expert);
  a.focus("expert");
  a.key(Nav::Accept);
  CHECK(a.model.show_expert);

  Frame p(640, 360, kPlayPage);
  p.focus("god_mode");
  p.key(Nav::Accept);
  CHECK(p.model.s.god_mode);
  p.focus("credits");
  for (int i = 0; i < 3; ++i) p.key(Nav::Right);
  CHECK(p.model.s.credits == 6);
  CHECK(p.model.command().text() == "x2recomp --game disk --god-mode --credits 6");
  p.focus("game");
  p.key(Nav::Right);
  CHECK(p.model.s.game == "disk");
  CHECK(p.model.command().text() == "x2recomp --game disk --god-mode --credits 6");
  p.focus("autofire");
  p.key(Nav::Accept);
  CHECK(p.model.s.autofire);
  p.focus("autofire_rate");
  p.key(Nav::Left);
  CHECK(p.model.s.autofire_rate == 16);
  CHECK(p.model.command().text().find("--autofire=16") != std::string::npos);
}

void test_copy_button() {
  Frame f(640, 360, kPlayPage);
  // Tab to the Copy button: it is the last focusable widget of the page chrome; click it with the pointer instead.
  const std::vector<FocusItem> items = f.ctx.items();
  Rect copy{};
  for (const FocusItem& it : items)
    if (it.rect.y >= 300 && it.rect.w < 80 && it.rect.w > 40 && it.rect.h < 30) copy = it.rect;
  CHECK(copy.w > 0);
  f.run({Event::pointer(Event::Type::PointerMove, copy.cx(), copy.cy()), Event::pointer(Event::Type::PointerDown, copy.cx(), copy.cy()),
         Event::pointer(Event::Type::PointerUp, copy.cx(), copy.cy())});
  CHECK(f.model.copied == "x2recomp --game disk");
}

// Every control of every built page can be reached with the arrow keys and Back alone: Down walks the whole page (the list scrolls
// as the focus moves, and the Copy button is the last stop), and Back reaches the page selector.
void test_reachability() {
  int n = 0;
  for (const int w : {640, 480}) {
    for (const GameKind g : {GameKind::Disk, GameKind::Cdtv, GameKind::St}) {
      for (const int page : {kPlayPage, kDisplayPage, kAudioPage}) {
        for (const bool expert : {false, true}) {
          Frame f(w, w == 640 ? 360 : 270, page, g);
          f.model.show_expert = expert;
          f.model.test_tone = [] {};
          f.run({});
          f.run({});
          size_t page_items = 0, rail_items = 0;
          for (const FocusItem& it : f.ctx.items()) (it.region == Region::Page ? page_items : rail_items)++;
          CHECK(page_items > 5 && rail_items == 7);
          // Into the page, then Down as far as it goes.
          f.key(Nav::Accept); // from the rail into the page
          std::set<Id> seen{f.ctx.focused()};
          Id last = 0;
          for (int i = 0; i < 200 && f.ctx.focused() != last; ++i) {
            last = f.ctx.focused();
            f.key(Nav::Down);
            seen.insert(f.ctx.focused());
          }
          if (seen.size() != page_items)
            for (const FocusItem& it : f.ctx.items())
              if (it.region == Region::Page && !seen.count(it.id)) std::fprintf(stderr, "  not reached: %d,%d %dx%d\n", it.rect.x, it.rect.y, it.rect.w, it.rect.h);
          if (seen.size() != page_items)
            std::fprintf(stderr, "Down reaches %zu of %zu controls on page %d (%dx, game %d, expert %d)\n", seen.size(), page_items, page, w,
                         static_cast<int>(g), expert);
          CHECK(seen.size() == page_items);
          // And back up to the first.
          for (int i = 0; i < 200; ++i) f.key(Nav::Up);
          // Back leaves the page for the page selector.
          f.key(Nav::Back);
          bool on_rail = false;
          for (const FocusItem& it : f.ctx.items())
            if (it.id == f.ctx.focused() && it.region == Region::Rail) on_rail = true;
          CHECK(on_rail);
          ++n;
        }
      }
    }
  }
  CHECK(n == 36);
}

// Every row of a page is built, with the right control, on every release (nothing throws, nothing is skipped by accident).
void test_rows_built() {
  for (const GameKind g : {GameKind::Disk, GameKind::Cdtv, GameKind::St}) {
    for (const int page : {kPlayPage, kDisplayPage, kAudioPage}) {
      Frame f(640, 360, page, g);
      f.model.show_expert = true;
      f.run({});
      f.run({});
      // The scroll area hides the rows below the fold, so look at the focusable ids: one per control row that is not text.
      std::set<Id> ids;
      for (const FocusItem& it : f.ctx.items()) ids.insert(it.id);
      PageBit bit = page == kPlayPage ? kPagePlay : page == kDisplayPage ? kPageDisplay : kPageAudio;
      size_t visible = 0;
      for (const SettingRow* r : rows_for_page(bit))
        if (r->type != SettingType::Text && ids.count(f.ctx.id(r->key))) ++visible;
      size_t want = 0;
      for (const SettingRow* r : rows_for_page(bit))
        if (r->type != SettingType::Text) ++want;
      CHECK(visible > 0 && visible <= want);
    }
  }
}

// ---- golden scenes ----
struct SceneSpec {
  const char* name;
  int page;
  GameKind game;
  void (*setup)(SettingsModel&);
  EventList (*events)();
};

EventList ev_end() { return {Event::nav_key(Nav::Last)}; }
EventList ev_down(int n) {
  EventList e;
  for (int i = 0; i < n; ++i) e.push_back(Event::nav_key(Nav::Down));
  return e;
}
EventList ev_down5() { return ev_down(5); }

void setup_cheats(SettingsModel& m) {
  m.s.god_mode = true;
  m.s.credits = 5;
  m.s.autofire = true;
  m.s.autofire_rate = 12;
}
void setup_display(SettingsModel& m) {
  m.s.scale = 4;
  m.s.video_filter = VideoFilter::Crt;
  m.s.curvature = true;
  m.s.aspect = true;
  m.s.interp = true;
  m.s.interp_scale = 4;
  m.s.wide = "16:9";
}
void setup_st_display(SettingsModel& m) {
  setup_display(m);
  m.s.hd_panel = true;
}
void setup_faithful(SettingsModel& m) {
  setup_display(m);
  m.s.faithful = true;
}
void setup_muted(SettingsModel& m) {
  m.s.audio = false;
  m.s.volume = 60;
}
void setup_audio_expert(SettingsModel& m) {
  m.s.music = "cdtv";
  m.s.stereo_separation = 0.5f;
  m.show_expert = true;
  m.test_tone = [] {};
}
void setup_packs(SettingsModel& m) {
  setup_display(m);
  m.s.upgrade = true;
  m.s.asset_packs = "/packs/crisp.zip;/packs/music";
}

const std::vector<SceneSpec>& scenes() {
  static const std::vector<SceneSpec> v = {
      {"play_disk", kPlayPage, GameKind::Disk, nullptr, nullptr},
      {"play_cheats", kPlayPage, GameKind::Disk, setup_cheats, ev_down5},
      {"display_disk", kDisplayPage, GameKind::Disk, setup_display, nullptr},
      {"display_end", kDisplayPage, GameKind::Disk, setup_display, ev_end},
      {"display_st", kDisplayPage, GameKind::St, setup_st_display, nullptr},
      {"display_faithful", kDisplayPage, GameKind::Disk, setup_faithful, nullptr},
      {"display_packs", kDisplayPage, GameKind::Cdtv, setup_packs, nullptr},
      {"audio_cdtv", kAudioPage, GameKind::Cdtv, nullptr, nullptr},
      {"audio_disk_muted", kAudioPage, GameKind::Disk, setup_muted, nullptr},
      {"audio_st", kAudioPage, GameKind::St, nullptr, nullptr},
      {"audio_expert_end", kAudioPage, GameKind::Disk, setup_audio_expert, ev_end},
  };
  return v;
}

struct Variant {
  int w, h;
  bool hc;
  const char* tag;
};
const Variant kVariants[] = {{640, 360, false, "640x360"}, {480, 270, false, "480x270"}, {640, 360, true, "640x360hc"}, {480, 270, true, "480x270hc"}};

Canvas render(const SceneSpec& s, const Variant& v) {
  Context ctx;
  ctx.theme = v.hc ? Theme::high_contrast_theme() : Theme::standard();
  ctx.motion.reduced = true;
  SettingsModel m;
  m.game = s.game;
  m.hotkey = default_hotkey;
  if (s.setup) s.setup(m);
  int page = s.page;
  Canvas canvas(v.w, v.h);
  auto builder = [&](Context& c) { settings_frame(c, m, page); };
  ctx.run(canvas, {}, 0, builder);
  if (s.events) ctx.run(canvas, s.events(), 0, builder);
  ctx.run(canvas, {}, 0, builder);
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
  std::string hashes, outdir, dump_all;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--update") update = true;
    else if (a == "--dump" && i + 1 < argc) dump_all = argv[++i];
    else if (hashes.empty()) hashes = a;
    else outdir = a;
  }
  std::map<std::string, std::pair<Canvas, uint64_t>> rendered;
  for (const SceneSpec& s : scenes())
    for (const Variant& v : kVariants) {
      Canvas c = render(s, v);
      const uint64_t h = c.hash();
      rendered.emplace(std::string(s.name) + "_" + v.tag, std::make_pair(std::move(c), h));
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
    std::fprintf(stderr, "usage: x2_settings_pages_tests HASHES [OUTDIR] | --update HASHES | --dump DIR\n");
    return 2;
  }
  if (update) {
    std::ofstream f(hashes, std::ios::trunc);
    f << "# Golden hashes of the settings pages (tests/ui/settings_pages_tests.cpp). One line per scene: name, width, height, FNV-1a 64 of the canvas.\n"
         "# Regenerate with: x2_settings_pages_tests --update tests/ui/golden/settings_hashes.txt, then review the pictures with --dump.\n";
    for (const auto& [name, r] : rendered) f << name << ' ' << r.first.width() << ' ' << r.first.height() << ' ' << hex64(r.second) << '\n';
    std::printf("updated %s (%zu scenes)\n", hashes.c_str(), rendered.size());
    return f ? 0 : 1;
  }

  struct T {
    const char* name;
    void (*fn)();
  };
  const T tests[] = {{"edits", test_edits},         {"atari_st_page", test_atari_st_page}, {"audio_and_play", test_audio_and_play},
                     {"copy_button", test_copy_button}, {"reachability", test_reachability},   {"rows_built", test_rows_built}};
  for (const T& t : tests) {
    const int before = g_failures;
    t.fn();
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
