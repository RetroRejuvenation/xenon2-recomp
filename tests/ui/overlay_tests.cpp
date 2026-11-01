// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The in-game menu (runtime/ui/overlay.*, pages/controls.*, live_apply.*): which settings take effect in a running game and which wait for the
// next start, navigation by synthetic SDL events, the Controls page (capture, conflicts, the gamepad, restore), and golden renders compared
// by hash at zero tolerance. No window, no game data: SDL is used for its event structures only.
//
//   x2_overlay_tests HASHES [OUTDIR]     run the tests, compare the renders; a mismatch writes the picture to OUTDIR
//   x2_overlay_tests --update HASHES     rewrite the hash file from the current output
//   x2_overlay_tests --dump DIR          write every scene as PNG (2x, over a generated test card) for review
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <filesystem>

#include "overlay.h"
#include "support/synth.h"
#include "ui_art.h"
#include "ui_png.h"

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

// ---- events ----
SDL_Event key_event(SDL_Scancode sc, bool down = true, SDL_Keymod mod = SDL_KMOD_NONE) {
  SDL_Event e{};
  e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  e.key.scancode = sc;
  e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
  e.key.down = down;
  e.key.mod = mod;
  return e;
}
SDL_Event pad_event(SDL_GamepadButton b) {
  SDL_Event e{};
  e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  e.gbutton.which = 77;
  e.gbutton.button = b;
  e.gbutton.down = true;
  return e;
}

// ---- live apply ----
// What a running game takes at once, and what waits for the next start. A setting that is in neither list fails the test below: someone
// has to decide which it is when a row is added to the schema.
const std::set<std::string>& live_keys() {
  static const std::set<std::string> k = {"scale", "fullscreen", "volume", "modern", "interp", "interp_scale", "smooth_far", "filter", "aspect",
                                          "integer", "curvature", "upgrade", "wide", "hd_panel", "autofire", "autofire_rate", "god_mode",
                                          "credits", "cheat_scores", "music", "music_volume", "sfx_volume", "speech_volume", "protect_music",
                                          "grab_keyboard", "cursor", "audio_upgrade", "input_debug", "ui_text", "ui_contrast", "ui_motion"};
  return k;
}
const std::set<std::string>& next_start_keys() {
  static const std::set<std::string> k = {"game", "launcher", "high_scores", "audio", "audio_filter", "stereo_separation", "audio_latency",
                                          "cd_crossfade", "cd_pause", "cdtv_tracks", "cdtv", "asset_packs", "packs_enabled", "last_game"};
  return k;
}

// A different value for the row (Text rows get another string).
Settings changed(const SettingRow& r, const Settings& base) {
  Settings s = base;
  std::string err;
  switch (r.type) {
    case SettingType::Bool: set_bool(r, s, !get_bool(r, base)); break;
    case SettingType::Int: set_int(r, s, get_int(r, base) == r.lo ? r.hi : r.lo); break;
    case SettingType::Float: set_float(r, s, get_float(r, base) < 0.5f ? r.fhi : r.flo); break;
    case SettingType::Choice: set_choice(r, s, (std::max(0, get_choice(r, base)) + 1) % r.nchoices); break;
    case SettingType::Text: {
      const std::string key = r.key;
      const std::string now = value_text(r, base);
      const std::string other = key == "cdtv_tracks" ? (now == "3,3,3,3,3,3" ? "2,3,5,2,3,5" : "3,3,3,3,3,3") : key == "last_game" ? (now == "cdtv" ? "disk" : "cdtv") : (now == "/a" ? "/b" : "/a");
      if (!set_value_text(r, s, other, err)) std::fprintf(stderr, "    cannot set '%s' to '%s': %s\n", r.key, other.c_str(), err.c_str());
      break;
    }
  }
  return s;
}

struct Calls {
  std::vector<std::string> names;
  bool has(const std::string& n) const { return std::find(names.begin(), names.end(), n) != names.end(); }
};

LiveHooks all_hooks(Calls& c) {
  LiveHooks h;
  h.scale = [&c](int) { c.names.push_back("scale"); };
  h.fullscreen = [&c](bool) { c.names.push_back("fullscreen"); };
  h.volume = [&c](int) { c.names.push_back("volume"); };
  h.picture = [&c](const Settings&) { c.names.push_back("picture"); };
  h.autofire = [&c](bool, int) { c.names.push_back("autofire"); };
  h.god_mode = [&c](bool) { c.names.push_back("god_mode"); };
  h.credits = [&c](int) { c.names.push_back("credits"); };
  h.cheat_scores = [&c](bool) { c.names.push_back("cheat_scores"); };
  h.music_source = [&c](const std::string&) { c.names.push_back("music"); };
  h.volumes = [&c](int, int, int) { c.names.push_back("volumes"); };
  h.protect_music = [&c](bool) { c.names.push_back("protect_music"); };
  h.grab = [&c](const std::string&) { c.names.push_back("grab"); };
  h.cursor = [&c](const std::string&) { c.names.push_back("cursor"); };
  h.audio_upgrade = [&c](bool) { c.names.push_back("audio_upgrade"); };
  h.input_debug = [&c](bool) { c.names.push_back("input_debug"); };
  return h;
}

bool contains(const std::vector<std::string>& v, const std::string& k) { return std::find(v.begin(), v.end(), k) != v.end(); }

void test_every_row_is_decided() {
  for (const SettingRow& r : schema()) {
    const bool live = live_keys().count(r.key) != 0, later = next_start_keys().count(r.key) != 0;
    CHECK(live != later);  // in exactly one list
    if (live == later) std::fprintf(stderr, "    setting '%s' is in %s list\n", r.key, live ? "both" : "neither");
  }
}

void test_apply_live_rules() {
  for (GameKind game : {GameKind::Disk, GameKind::Cdtv}) {
    for (const SettingRow& r : schema()) {
      if (!applies_to(r, game)) continue;
      const Settings before;
      const Settings after = changed(r, before);
      CHECK(value_text(r, before) != value_text(r, after));
      if (value_text(r, before) == value_text(r, after)) std::fprintf(stderr, "    no different value for '%s'\n", r.key);
      Calls calls;
      const LiveResult res = apply_live(before, after, game, all_hooks(calls));
      const bool live = live_keys().count(r.key) != 0;
      CHECK(contains(live ? res.applied : res.next_start, r.key));
      CHECK(!contains(live ? res.next_start : res.applied, r.key));
      CHECK(res.applied.size() + res.next_start.size() == 1);
      if (live && std::string(r.key) != "ui_text" && std::string(r.key) != "ui_contrast" && std::string(r.key) != "ui_motion") CHECK(!calls.names.empty());
      if (!live) CHECK(calls.names.empty());  // nothing was called for a setting that waits
    }
  }
  // A row the release does not honour is left alone: interpolation on the Atari ST release is neither applied nor reported.
  {
    Settings before, after = before;
    after.interp = true;
    Calls calls;
    const LiveResult res = apply_live(before, after, GameKind::St, all_hooks(calls));
    CHECK(!res.any_change() && calls.names.empty());
  }
  // No hook, no live change: everything the host cannot do is reported for the next start, picture rows included.
  {
    Settings before, after = before;
    after.scale = 5;
    after.video_filter = VideoFilter::Crt;
    after.volume = 40;
    const LiveResult res = apply_live(before, after, GameKind::Disk, LiveHooks{});
    CHECK(res.applied.empty() && res.next_start.size() == 3);
    CHECK(res.next_start_note().find("Window size") != std::string::npos);
    CHECK(res.next_start_note().find("Master volume") != std::string::npos);
  }
  // The picture hook runs once for any number of picture rows, with the whole new Settings.
  {
    Settings before, after = before;
    after.video_filter = VideoFilter::Crt;
    after.aspect = !before.aspect;
    after.curvature = true;
    Calls calls;
    int seen_filter = -1;
    LiveHooks h = all_hooks(calls);
    h.picture = [&](const Settings& s) { calls.names.push_back("picture"); seen_filter = int(s.video_filter); };
    const LiveResult res = apply_live(before, after, GameKind::Disk, h);
    CHECK(res.applied.size() == 3);
    CHECK(std::count(calls.names.begin(), calls.names.end(), "picture") == 1);
    CHECK(seen_filter == int(VideoFilter::Crt));
  }
  // The Atari ST release takes the window, volume, filter, aspect, whole pixels, curvature, autofire and god mode, and nothing else.
  {
    Calls calls;
    LiveHooks h;
    h.scale = [&](int) { calls.names.push_back("scale"); };
    h.fullscreen = [&](bool) { calls.names.push_back("fullscreen"); };
    h.volume = [&](int) { calls.names.push_back("volume"); };
    h.picture = [&](const Settings&) { calls.names.push_back("picture"); };
    h.picture_keys = {"filter", "aspect", "integer", "curvature"};
    h.autofire = [&](bool, int) { calls.names.push_back("autofire"); };
    h.god_mode = [&](bool) { calls.names.push_back("god_mode"); };
    const std::set<std::string> st_live = {"scale", "fullscreen", "volume", "filter", "aspect", "integer", "curvature", "autofire", "autofire_rate",
                                           "god_mode", "ui_text", "ui_contrast", "ui_motion"};
    for (const SettingRow& r : schema()) {
      if (!applies_to(r, GameKind::St)) continue;
      const Settings before;
      const LiveResult res = apply_live(before, changed(r, before), GameKind::St, h);
      CHECK(contains(st_live.count(r.key) ? res.applied : res.next_start, r.key));
      if (!contains(st_live.count(r.key) ? res.applied : res.next_start, r.key)) std::fprintf(stderr, "    ST: '%s'\n", r.key);
    }
  }
  // merge_changed copies only what the menu changed, so a command-line override that was never saved stays out of the file.
  {
    Settings file, before, after = before;
    file.scale = 2;           // the file
    before.scale = 6;         // the run: --scale 6 over it
    after = before;
    after.video_filter = VideoFilter::Scanlines;
    after.volume = 55;
    const auto keys = merge_changed(before, after, file);
    CHECK(keys.size() == 2);
    CHECK(file.scale == 2 && file.video_filter == VideoFilter::Scanlines && file.volume == 55);
  }
}

// ---- the overlay ----
struct Rec {
  int opened = 0, closed = 0, resets = 0, quits = 0;
  std::vector<std::pair<Settings, Settings>> applies;
  std::vector<input::Bindings> saved;
  bool save_ok = true;
  Settings current;
  std::set<std::string> live;  // what the fake game takes at once
  std::string packs_dir;       // the Asset packs page's folder (empty: none)
};

OverlayHooks make_hooks(Rec& rec) {
  OverlayHooks h;
  h.current = [&rec] { return rec.current; };
  h.apply = [&rec](const Settings& b, const Settings& a) {
    rec.applies.emplace_back(b, a);
    LiveHooks lh;
    if (rec.live.count("scale")) lh.scale = [](int) {};
    if (rec.live.count("volume")) lh.volume = [](int) {};
    if (rec.live.count("god_mode")) lh.god_mode = [](bool) {};
    if (rec.live.count("picture")) lh.picture = [](const Settings&) {};
    return apply_live(b, a, GameKind::Disk, lh);
  };
  h.save_bindings = [&rec](const input::Bindings& b, std::string& why) {
    rec.saved.push_back(b);
    if (!rec.save_ok) why = "disk full";
    return rec.save_ok;
  };
  h.packs = [&rec] {
    OverlayHooks::PacksConfig c;
    c.packs_dir = rec.packs_dir;
    return c;
  };
  h.reset_game = [&rec] { ++rec.resets; };
  h.quit = [&rec] { ++rec.quits; };
  h.set_open = [&rec](bool open) { ++(open ? rec.opened : rec.closed); };
  return h;
}

struct Menu {
  Rec rec;
  Overlay ov;
  Menu(GameKind game = GameKind::Disk) : ov(game, make_hooks(rec)) {
    rec.live = {"scale", "volume", "picture", "god_mode"};
  }
  void open(int w = 1280, int h = 720) { ov.open(w, h, input::default_bindings()); }
  void key(SDL_Scancode sc, SDL_Keymod mod = SDL_KMOD_NONE) {
    ov.handle(key_event(sc, true, mod));
    ov.handle(key_event(sc, false, mod));
  }
  void down(int n = 1) {
    for (int i = 0; i < n; ++i) key(SDL_SCANCODE_DOWN);
  }
  void enter() { key(SDL_SCANCODE_RETURN); }
  void esc() { key(SDL_SCANCODE_ESCAPE); }
};

void test_open_close() {
  Menu m;
  m.open();
  CHECK(m.ov.active() && m.rec.opened == 1 && m.rec.closed == 0);
  CHECK(m.ov.screen() == Overlay::Screen::Top);
  m.enter();  // Resume has the focus
  CHECK(!m.ov.active() && m.rec.closed == 1);
  m.key(SDL_SCANCODE_DOWN);  // a closed menu ignores events
  CHECK(!m.ov.active());

  m.open();
  m.key(SDL_SCANCODE_F1);  // the key bound to `menu` closes it
  CHECK(!m.ov.active() && m.rec.closed == 2);
  m.open();
  m.esc();  // Back at the top level resumes
  CHECK(!m.ov.active() && m.rec.closed == 3);
  m.open();
  m.ov.handle(pad_event(SDL_GAMEPAD_BUTTON_START));  // and so does the gamepad's menu button
  CHECK(!m.ov.active() && m.rec.closed == 4);
  m.open();
  m.ov.handle(pad_event(SDL_GAMEPAD_BUTTON_EAST));  // B is Back
  CHECK(!m.ov.active() && m.rec.closed == 5);
  CHECK(m.rec.opened == 5 && m.rec.quits == 0 && m.rec.resets == 0);
  CHECK(m.rec.applies.empty() && m.rec.saved.empty());  // opening and closing changes nothing
}

void test_modified_keys_are_not_the_menus() {
  Menu m;
  m.open();
  m.key(SDL_SCANCODE_RETURN, SDL_KMOD_LALT);  // Alt+Return is the window's fullscreen, not Enter on Resume
  CHECK(m.ov.active());
  m.key(SDL_SCANCODE_DOWN, SDL_KMOD_LCTRL);
  m.key(SDL_SCANCODE_RETURN);  // still on Resume
  CHECK(!m.ov.active());
#ifdef __APPLE__
  Menu c;
  c.open();
  c.key(SDL_SCANCODE_1, SDL_KMOD_LGUI);  // Cmd+1 is F1
  CHECK(!c.ov.active());
#endif
}

void test_pages_and_back() {
  Menu m;
  m.open();
  m.down(2);  // Resume, Faithful, Display
  m.enter();
  CHECK(m.ov.screen() == Overlay::Screen::Display);
  m.esc();
  CHECK(m.ov.screen() == Overlay::Screen::Top && m.ov.active());
  m.down(4);  // Faithful, Display, Audio, Controls (the focus starts on Resume again)
  m.enter();
  CHECK(m.ov.screen() == Overlay::Screen::Controls);
  // PageDown cycles the pages; PageUp goes back.
  m.key(SDL_SCANCODE_PAGEDOWN);
  CHECK(m.ov.screen() == Overlay::Screen::Packs);
  m.key(SDL_SCANCODE_PAGEDOWN);
  CHECK(m.ov.screen() == Overlay::Screen::Assists);
  m.key(SDL_SCANCODE_PAGEDOWN);
  CHECK(m.ov.screen() == Overlay::Screen::Display);
  m.key(SDL_SCANCODE_PAGEUP);
  CHECK(m.ov.screen() == Overlay::Screen::Assists);
  m.esc();
  CHECK(m.ov.screen() == Overlay::Screen::Top);
  // The menu key closes from a page too.
  m.down(2);
  m.enter();
  m.key(SDL_SCANCODE_F1);
  CHECK(!m.ov.active());
}

void test_faithful_row() {
  Menu m;
  m.open();
  m.down();  // the Faithful row
  m.enter();
  CHECK(m.rec.applies.size() == 1);
  CHECK(!m.rec.applies[0].first.faithful && m.rec.applies[0].second.faithful);
  CHECK(m.ov.settings().s.faithful);
  m.enter();  // and back
  CHECK(m.rec.applies.size() == 2 && !m.rec.applies[1].second.faithful);
  CHECK(m.ov.active());
}

void test_reset_and_quit() {
  Menu a;
  a.open();
  a.down(7);  // Reset the game
  a.enter();  // asks first, with Cancel focused
  CHECK(a.rec.resets == 0 && a.ov.active());
  a.enter();  // Cancel
  CHECK(a.rec.resets == 0 && a.ov.active());

  Menu b;
  b.open();
  b.down(7);
  b.enter();
  b.key(SDL_SCANCODE_LEFT);  // from Cancel to Reset
  b.enter();
  CHECK(b.rec.resets == 1 && !b.ov.active() && b.rec.closed == 1);

  Menu q;
  q.open();
  q.down(8);  // Quit
  q.enter();
  q.key(SDL_SCANCODE_LEFT);
  q.enter();
  CHECK(q.rec.quits == 1);
  // Back out of the question instead.
  Menu c;
  c.open();
  c.down(8);
  c.enter();
  c.esc();
  CHECK(c.rec.quits == 0 && c.ov.active() && c.ov.screen() == Overlay::Screen::Top);

  Menu k;
  k.open();
  k.key(SDL_SCANCODE_F10);  // quit stays on F10 and Ctrl+Q, in the menu too
  CHECK(k.rec.quits == 1);
  k.key(SDL_SCANCODE_Q, SDL_KMOD_LCTRL);
  CHECK(k.rec.quits == 2);
  k.key(SDL_SCANCODE_Q, SDL_KMOD_LGUI);
  CHECK(k.rec.quits == 3);
}

void test_settings_apply_and_status() {
  Menu m;
  m.open();
  m.ov.show(Overlay::Screen::Display);
  m.key(SDL_SCANCODE_DOWN);  // from the Faithful switch to Window size
  m.key(SDL_SCANCODE_RIGHT);
  CHECK(m.rec.applies.size() == 1);
  CHECK(m.rec.applies[0].second.scale == m.rec.applies[0].first.scale + 1);
  CHECK(m.ov.status().empty());  // the game took it
  // A setting the fake game cannot take now is reported, and the report goes when the user moves on.
  m.ov.show(Overlay::Screen::Audio);
  m.ov.settings().s.audio_latency = 120;
  m.ov.show(Overlay::Screen::Audio);
  CHECK(m.ov.status().find("next start") != std::string::npos);
  CHECK(m.ov.status().find("Output buffer") != std::string::npos);
  m.key(SDL_SCANCODE_DOWN);
  CHECK(m.ov.status().empty());
  // The settings the menu opened with are what the game says it is using.
  Menu fresh;
  fresh.rec.current.volume = 35;
  fresh.rec.current.video_filter = VideoFilter::Edge;
  fresh.open();
  CHECK(fresh.ov.settings().s.volume == 35 && fresh.ov.settings().s.video_filter == VideoFilter::Edge);
}

void test_look_follows_settings() {
  Menu m;
  m.open();
  const Canvas& normal = m.ov.render(1280, 720);
  CHECK(normal.width() == 640 && normal.height() == 360 && m.ov.canvas_scale() == 2);
  m.ov.settings().s.ui_text = "large";
  m.ov.show(Overlay::Screen::Top);
  const Canvas& large = m.ov.render(1280, 720);
  CHECK(large.width() == 320 && large.height() == 180 && m.ov.canvas_scale() == 4);
  // The window follows: a smaller window gives a smaller canvas, never below 480x270 (320x180 with large text).
  m.ov.settings().s.ui_text = "normal";
  m.ov.show(Overlay::Screen::Top);
  const Canvas& small = m.ov.render(960, 540);
  CHECK(small.width() == 480 && small.height() == 270);
  // The panel is opaque and the rest see-through: the picture shows at the left.
  const Canvas& c = m.ov.render(1280, 720);
  const uint32_t left = c.data()[100 * size_t(c.width()) + 10], panel = c.data()[100 * size_t(c.width()) + c.width() - 20];
  CHECK((left >> 24) < 255 && (left >> 24) > 0);
  CHECK((panel >> 24) == 255);
}

// The Asset packs screen is the launcher's page (pages/packs.*) hosted in the menu: its list, a pack dropped on the window, and the
// "applies at the next start" note for the lists only the next start reads.
void test_packs_screen() {
  namespace fs = std::filesystem;
  const std::string root = (fs::temp_directory_path() / "x2_overlay_packs_test").string();
  std::error_code ec;
  fs::remove_all(root, ec);
  synth::put_file(root + "/packs/crisp/manifest.json",
                  "{\"format\": \"x2-asset-pack\", \"version\": 2, \"id\": \"crisp\", \"name\": \"Crisp\", \"pack_version\": \"1\", "
                  "\"author\": \"T\", \"license\": \"CC0-1.0\", \"games\": [\"disk\"], \"scale\": 2, "
                  "\"graphics\": {\"0123456789abcdef-16x16\": {\"png\": \"a.png\"}}}");
  synth::put_flat(root + "/packs/crisp/a.png", 32, 32);
  synth::put_file(root + "/elsewhere/manifest.json",
                  "{\"format\": \"x2-asset-pack\", \"version\": 2, \"id\": \"elsewhere\", \"name\": \"Elsewhere\", \"pack_version\": \"1\", "
                  "\"author\": \"T\", \"license\": \"CC0-1.0\", \"games\": [\"disk\"], \"scale\": 2, "
                  "\"graphics\": {\"0123456789abcdef-16x16\": {\"png\": \"b.png\"}}}");
  synth::put_flat(root + "/elsewhere/b.png", 32, 32);
  Menu m;
  m.rec.packs_dir = root + "/packs";
  m.open();
  m.ov.show(Overlay::Screen::Packs);
  PacksModel* pm = m.ov.settings().packs;
  CHECK(pm != nullptr && pm->packs_dir == root + "/packs");
  pm->sync(m.ov.settings());
  CHECK(pm->rows().size() == 1 && pm->rows()[0].id == "crisp" && !pm->rows()[0].enabled);
  m.enter();  // Enter on the list turns the pack on: the list of packs is read at the next start
  CHECK(m.ov.settings().s.packs_enabled == "crisp");
  CHECK(!m.rec.applies.empty() && m.ov.status().find("Enabled packs") != std::string::npos);
  // A pack folder dropped on the window is added to asset_packs.
  const std::string dropped = root + "/elsewhere";
  SDL_Event drop{};
  drop.type = SDL_EVENT_DROP_FILE;
  drop.drop.data = dropped.c_str();
  m.ov.handle(drop);
  CHECK(m.ov.settings().s.asset_packs == dropped);
  // The drop does nothing on another screen.
  m.ov.show(Overlay::Screen::Display);
  const std::string before = m.ov.settings().s.asset_packs;
  m.ov.handle(drop);
  CHECK(m.ov.settings().s.asset_packs == before);
  // The menu closes with its jobs; a new one starts with a fresh page.
  m.ov.close();
  m.ov.open(1280, 720, input::default_bindings());
  CHECK(m.ov.settings().packs != nullptr && m.ov.settings().packs->rows().empty());
  fs::remove_all(root, ec);
}

// ---- the Controls page ----
struct CFrame {
  Context ctx;
  SettingsModel sm;
  ControlsModel cm;
  Canvas canvas{640, 360};
  CFrame() {
    ctx.motion.reduced = true;
    cm.open(input::default_bindings());
    run({});
    run({});
  }
  void run(const EventList& ev) {
    auto builder = [&](Context& c) {
      c.canvas().clear(c.theme.c.ground);
      page_controls(c, cm, sm, {10, 6, 620, 330}, {10, 344, 620, 10}, true);
      controls_modal(c, cm);
    };
    ctx.run(canvas, ev, 0, builder);
  }
  void focus(const std::string& id) {
    ctx.set_focus(ctx.id(id));
    run({});
    run({});
    CHECK(ctx.focused() == ctx.id(id));
  }
  void nav(Nav n) { run({Event::nav_key(n)}); }
  void capture(const std::string& id, SDL_Scancode sc) {
    focus(id);
    nav(Nav::Accept);
    CHECK(ctx.capturing());
    run({Event::raw_key(int(SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false)), int(sc))});
  }
  void capture_pad(const std::string& id, SDL_GamepadButton b) {
    focus(id);
    nav(Nav::Accept);
    CHECK(ctx.capturing());
    run({Event::raw_key(kPadKeyBase + int(b), 0, Device::Pad)});
  }
  const input::Bindings& b() const { return cm.b; }
};
using input::Action;

void test_controls_capture() {
  CFrame f;
  // A free key replaces the binding.
  f.capture("key_fullscreen", SDL_SCANCODE_Q);
  CHECK(f.b().keys[Action::HostFullscreen] == std::vector<SDL_Scancode>{SDL_SCANCODE_Q});
  CHECK(f.cm.changed && !f.ctx.capturing() && f.cm.message.find("Q") != std::string::npos);
  f.cm.changed = false;
  // "+" adds one.
  f.capture("key_fullscreen/add", SDL_SCANCODE_E);
  CHECK((f.b().keys[Action::HostFullscreen] == std::vector<SDL_Scancode>{SDL_SCANCODE_Q, SDL_SCANCODE_E}));
  // Backspace while capturing clears the binding.
  f.focus("key_fullscreen");
  f.nav(Nav::Accept);
  f.run({Event::raw_key(8, int(SDL_SCANCODE_BACKSPACE))});
  CHECK(f.b().keys[Action::HostFullscreen].empty() && f.cm.changed);
  // Escape cancels and changes nothing.
  f.cm.changed = false;
  f.focus("key_pause");
  f.nav(Nav::Accept);
  f.run({Event::raw_key(27, int(SDL_SCANCODE_ESCAPE))});
  CHECK(!f.cm.changed && !f.ctx.capturing() && f.b().keys[Action::HostPause] == input::default_bindings().keys[Action::HostPause]);
  // Del restores a row to its default.
  f.focus("key_fullscreen");
  f.nav(Nav::Default);
  CHECK(f.b().keys[Action::HostFullscreen] == input::default_bindings().keys[Action::HostFullscreen]);
}

void test_controls_conflicts() {
  CFrame f;
  // F2 is the filter key: asked first, nothing changes until the answer.
  f.capture("key_fullscreen", SDL_SCANCODE_F2);
  CHECK(f.cm.pending.kind == ControlsModel::Pending::Key && f.cm.pending.other == Action::HostFilter);
  CHECK(f.ctx.modal_open());
  CHECK(f.b().keys[Action::HostFullscreen] == input::default_bindings().keys[Action::HostFullscreen]);
  f.nav(Nav::Back);  // cancels
  CHECK(!f.ctx.modal_open() && f.cm.pending.kind == ControlsModel::Pending::None);
  f.run({});
  CHECK(!f.cm.changed && f.b().keys[Action::HostFilter] == input::default_bindings().keys[Action::HostFilter]);
  // Again, and confirm: the key moves, and the other action goes without.
  f.capture("key_fullscreen", SDL_SCANCODE_F2);
  f.nav(Nav::Accept);  // "Move it here" is the default
  CHECK(f.b().keys[Action::HostFullscreen] == std::vector<SDL_Scancode>{SDL_SCANCODE_F2});
  CHECK(f.b().keys[Action::HostFilter].empty() && f.cm.changed);
  CHECK(f.cm.message.find("taken from") != std::string::npos);
  // A key the game itself reads is asked about too, even when no other action has it.
  CFrame g;
  g.capture("key_fullscreen", SDL_SCANCODE_P);
  CHECK(g.cm.pending.kind == ControlsModel::Pending::Key && g.cm.pending.other == -1);
  CHECK(g.cm.pending.text.find("game itself") != std::string::npos);
  g.nav(Nav::Accept);
  CHECK(g.b().keys[Action::HostFullscreen] == std::vector<SDL_Scancode>{SDL_SCANCODE_P});
  // The two joysticks are different actions: Fire of port 1 on a port 2 key asks.
  CFrame h;
  h.capture("key_p1_fire", h.b().keys[Action::P2Fire].front());
  CHECK(h.cm.pending.other == Action::P2Fire);
}

void test_controls_gamepad() {
  CFrame f;
  f.capture_pad("pad_fire", SDL_GAMEPAD_BUTTON_NORTH);
  CHECK(f.b().pad.fire == std::vector<SDL_GamepadButton>{SDL_GAMEPAD_BUTTON_NORTH} && f.cm.changed);
  f.cm.changed = false;
  f.capture_pad("pad_fire/add", SDL_GAMEPAD_BUTTON_WEST);
  CHECK((f.b().pad.fire == std::vector<SDL_GamepadButton>{SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST}));
  // A button that another list has asks, and then moves.
  f.capture_pad("pad_fire", SDL_GAMEPAD_BUTTON_START);
  CHECK(f.cm.pending.kind == ControlsModel::Pending::Pad && f.cm.pending.other == 2);
  f.nav(Nav::Accept);
  CHECK(f.b().pad.fire == std::vector<SDL_GamepadButton>{SDL_GAMEPAD_BUTTON_START} && f.b().pad.menu.empty());
  // The menu must stay reachable: F1 still opens it, so the gamepad can go, but not both.
  CHECK(!f.b().keys[Action::HostMenu].empty());
  f.focus("key_menu");
  f.nav(Nav::Accept);
  f.run({Event::raw_key(8, int(SDL_SCANCODE_BACKSPACE))});
  CHECK(!f.b().keys[Action::HostMenu].empty());  // refused: nothing else opens it now
  CHECK(f.cm.message.find("menu needs") != std::string::npos);
  // A keyboard key during a gamepad capture is explained, not taken.
  CFrame g;
  g.cm.changed = false;
  g.capture("pad_button2", SDL_SCANCODE_Q);
  CHECK(!g.cm.changed && g.cm.message.find("keyboard key") != std::string::npos);
  CHECK(g.b().pad.button2 == input::default_bindings().pad.button2);
  // ... and a gamepad button during a keyboard capture.
  CFrame h;
  h.capture_pad("key_fullscreen", SDL_GAMEPAD_BUTTON_SOUTH);
  CHECK(!h.cm.changed && h.cm.message.find("gamepad button") != std::string::npos);
  // Button names follow the pad.
  CHECK(pad_caption(SDL_GAMEPAD_BUTTON_SOUTH, PadStyle::Xbox) == "A");
  CHECK(pad_caption(SDL_GAMEPAD_BUTTON_SOUTH, PadStyle::PlayStation) == "Cross");
  CHECK(pad_caption(SDL_GAMEPAD_BUTTON_SOUTH, PadStyle::Nintendo) == "B");
  CHECK(pad_caption(SDL_GAMEPAD_BUTTON_START, PadStyle::PlayStation) == "Options");
}

void test_controls_restore_and_reach() {
  CFrame f;
  f.capture("key_pause", SDL_SCANCODE_Q);
  f.capture_pad("pad_menu", SDL_GAMEPAD_BUTTON_BACK);
  CHECK(!(f.b() == input::default_bindings()));
  f.focus("Restore all defaults");
  f.nav(Nav::Accept);
  CHECK(f.b() == input::default_bindings());
  // Every control is reachable with the arrow keys alone: walking down from the top reaches the last one.
  CFrame g;
  std::set<Id> seen;
  for (int i = 0; i < 400; ++i) {
    seen.insert(g.ctx.focused());
    g.nav(Nav::Down);
  }
  CHECK(seen.count(g.ctx.id("pad_fire")) && seen.count(g.ctx.id("pad_menu")) && seen.count(g.ctx.id("key_menu")) &&
        seen.count(g.ctx.id("key_p2_fire/add")) == 0);  // the "+" is beside its row, reached with Right
  CHECK(seen.count(g.ctx.id("key_add_credit")) && seen.count(g.ctx.id("Restore all defaults")));
  // Every action has a label and a help line that fit the page.
  for (int a = 0; a < input::kActionCount; ++a) {
    CHECK(std::strlen(action_label(a)) > 0 && std::strlen(action_label(a)) <= 18);
    CHECK(std::strlen(action_help(a)) > 0);
  }
}

void test_overlay_saves_bindings() {
  Menu m;
  m.open();
  m.ov.show(Overlay::Screen::Controls);
  m.ov.controls().b.keys[Action::HostFullscreen] = {SDL_SCANCODE_Q};
  m.ov.controls().changed = true;
  m.ov.show(Overlay::Screen::Controls);
  CHECK(m.rec.saved.size() == 1 && m.rec.saved[0].keys[Action::HostFullscreen] == std::vector<SDL_Scancode>{SDL_SCANCODE_Q});
  CHECK(!m.ov.controls().changed);
  // The key now bound to `menu` is the one that closes it, even when it is not F1.
  Menu n;
  input::Bindings b = input::default_bindings();
  b.keys[Action::HostMenu] = {SDL_SCANCODE_F3};
  n.ov.open(1280, 720, b);
  n.key(SDL_SCANCODE_F1);  // F1 is no longer the menu key (the Nav::Menu alias of the kit still closes it)
  n.ov.open(1280, 720, b);
  n.key(SDL_SCANCODE_F3);
  CHECK(!n.ov.active());
  // A failed write is said, and the bindings stay in force.
  Menu bad;
  bad.rec.save_ok = false;
  bad.open();
  bad.ov.show(Overlay::Screen::Controls);
  bad.ov.controls().b.keys[Action::HostFullscreen] = {SDL_SCANCODE_Q};
  bad.ov.controls().changed = true;
  bad.ov.show(Overlay::Screen::Controls);
  CHECK(bad.ov.controls().message.find("Not saved") != std::string::npos);
  CHECK(bad.ov.controls().in_force.keys[Action::HostFullscreen] == std::vector<SDL_Scancode>{SDL_SCANCODE_Q});
}

// ---- golden scenes ----
struct SceneSpec {
  const char* name;
  GameKind game;
  Overlay::Screen screen;
  void (*setup)(Settings&);
  int downs;
};
void setup_look(Settings& s) {
  s.scale = 4;
  s.video_filter = VideoFilter::Crt;
  s.interp = true;
  s.interp_scale = 4;
}
void setup_cheats(Settings& s) {
  s.god_mode = true;
  s.autofire = true;
  s.credits = 5;
}
void setup_faithful(Settings& s) {
  setup_look(s);
  s.faithful = true;
}
void setup_packs(Settings& s) {
  s.upgrade = true;
  s.asset_packs = "/packs/crisp.zip";
}

const std::vector<SceneSpec>& scenes() {
  static const std::vector<SceneSpec> v = {
      {"top_disk", GameKind::Disk, Overlay::Screen::Top, nullptr, 0},
      {"top_faithful", GameKind::Disk, Overlay::Screen::Top, setup_faithful, 1},
      {"top_quit", GameKind::Cdtv, Overlay::Screen::Top, nullptr, 8},
      {"display_disk", GameKind::Disk, Overlay::Screen::Display, setup_look, 0},
      {"display_st", GameKind::St, Overlay::Screen::Display, setup_look, 3},
      {"audio_cdtv", GameKind::Cdtv, Overlay::Screen::Audio, nullptr, 0},
      {"controls_disk", GameKind::Disk, Overlay::Screen::Controls, nullptr, 0},
      {"controls_keys", GameKind::Disk, Overlay::Screen::Controls, nullptr, 9},
      {"controls_end", GameKind::Cdtv, Overlay::Screen::Controls, nullptr, 60},
      {"packs_disk", GameKind::Disk, Overlay::Screen::Packs, setup_packs, 0},
      {"assists_cheats", GameKind::Disk, Overlay::Screen::Assists, setup_cheats, 0},
  };
  return v;
}
struct Variant {
  int w, h;  // window pixels
  bool hc;
  const char* tag;
};
const Variant kVariants[] = {{1280, 720, false, "1280x720"}, {960, 540, false, "960x540"}, {1280, 720, true, "1280x720hc"}, {960, 540, true, "960x540hc"}};

Canvas render_scene(const SceneSpec& s, const Variant& v) {
  Rec rec;
  OverlayHooks h = make_hooks(rec);
  Settings cur;
  if (s.setup) s.setup(cur);
  cur.ui_contrast = v.hc ? "high" : "normal";
  cur.ui_motion = "reduced";
  h.current = [cur] { return cur; };
  Overlay ov(s.game, h);
  ov.open(v.w, v.h, input::default_bindings());
  ov.show(s.screen);
  for (int i = 0; i < s.downs; ++i) {
    ov.handle(key_event(SDL_SCANCODE_DOWN, true));
    ov.handle(key_event(SDL_SCANCODE_DOWN, false));
  }
  return ov.render(v.w, v.h);
}

// A review picture: the menu over a generated test card standing in for the paused game.
Canvas over_test_card(const Canvas& menu) {
  Canvas out(menu.width(), menu.height());
  out.clear(rgb(0, 0, 0));
  draw_test_card(out, {0, 0, menu.width(), menu.height()});
  for (int y = 0; y < menu.height(); ++y)
    for (int x = 0; x < menu.width(); ++x) {
      const uint32_t p = menu.data()[size_t(y) * size_t(menu.width()) + size_t(x)];
      const int a = int(p >> 24);
      out.put(x, y, mix(out.get(x, y), rgb(red_of(p), green_of(p), blue_of(p)), a));
    }
  return out;
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
      Canvas c = render_scene(s, v);
      const uint64_t h = c.hash();
      rendered.emplace(std::string(s.name) + "_" + v.tag, std::make_pair(std::move(c), h));
    }
  if (!dump_all.empty()) {
    for (const auto& [name, r] : rendered)
      if (!write_png(over_test_card(r.first), dump_all + "/" + name + ".png", 2)) {
        std::fprintf(stderr, "cannot write %s/%s.png\n", dump_all.c_str(), name.c_str());
        return 1;
      }
    std::printf("wrote %zu pictures to %s\n", rendered.size(), dump_all.c_str());
    return 0;
  }
  if (hashes.empty()) {
    std::fprintf(stderr, "usage: x2_overlay_tests HASHES [OUTDIR] | --update HASHES | --dump DIR\n");
    return 2;
  }
  if (update) {
    std::ofstream f(hashes, std::ios::trunc);
    f << "# Golden hashes of the in-game menu (tests/ui/overlay_tests.cpp). One line per scene: name, width, height, FNV-1a 64 of the canvas.\n"
         "# Regenerate with: x2_overlay_tests --update tests/ui/golden/overlay_hashes.txt, then review the pictures with --dump.\n";
    for (const auto& [name, r] : rendered) f << name << ' ' << r.first.width() << ' ' << r.first.height() << ' ' << hex64(r.second) << '\n';
    std::printf("updated %s (%zu scenes)\n", hashes.c_str(), rendered.size());
    return f ? 0 : 1;
  }

  struct T {
    const char* name;
    void (*fn)();
  };
  const T tests[] = {{"every_row_decided", test_every_row_is_decided},
                     {"apply_live", test_apply_live_rules},
                     {"open_close", test_open_close},
                     {"modified_keys", test_modified_keys_are_not_the_menus},
                     {"pages_and_back", test_pages_and_back},
                     {"faithful_row", test_faithful_row},
                     {"reset_and_quit", test_reset_and_quit},
                     {"apply_and_status", test_settings_apply_and_status},
                     {"look", test_look_follows_settings},
                     {"controls_capture", test_controls_capture},
                     {"controls_conflicts", test_controls_conflicts},
                     {"controls_gamepad", test_controls_gamepad},
                     {"controls_restore", test_controls_restore_and_reach},
                     {"saves_bindings", test_overlay_saves_bindings},
                     {"packs_screen", test_packs_screen}};
  for (const T& t : tests) {
    const int before = g_failures;
    t.fn();
    std::printf("%-20s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
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
      if (!outdir.empty()) write_png(over_test_card(r.first), outdir + "/" + name + ".png", 2);
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
