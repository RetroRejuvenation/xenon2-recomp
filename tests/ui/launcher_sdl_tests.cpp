// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The launcher window's real loop under SDL's dummy video driver: a quit event, a key press that plays a release, files dropped on the
// window (a fake importer, and the real importer on the user's own disk images when they are given), settings saved when the window
// closes, and the look following the settings. Exits 77 (Skipped) when SDL has no usable window here; the real-import part is skipped
// without the images.
//
//   x2_launcher_sdl_tests WORKDIR [ZIPS]      ZIPS: ';'-separated paths of the user's own Xenon 2 disk-image archives (optional)
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "import/importer.h"
#include "launcher.h"

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

fs::path make_data(const char* name, bool disk) {
  const fs::path d = g_work / name;
  fs::remove_all(d);
  fs::create_directories(d);
  if (disk) write_text(d / "assets" / "manifest.txt", "importer_version=3\nvariant_name=Test release\ncanonical=true\npatched=false\n");
  return d;
}

SDL_Event key_event(SDL_Keycode k) {
  SDL_Event e{};
  e.type = SDL_EVENT_KEY_DOWN;
  e.key.key = k;
  e.key.down = true;
  e.key.repeat = false;
  return e;
}

void drop_events(std::vector<SDL_Event>& out, const std::string& path) {
  SDL_Event b{};
  b.type = SDL_EVENT_DROP_BEGIN;
  out.push_back(b);
  SDL_Event f{};
  f.type = SDL_EVENT_DROP_FILE;
  f.drop.type = SDL_EVENT_DROP_FILE;
  f.drop.data = SDL_strdup(path.c_str());
  out.push_back(f);
  SDL_Event c{};
  c.type = SDL_EVENT_DROP_COMPLETE;
  out.push_back(c);
}

LauncherEnv env_for(const fs::path& dir, bool settings = true) {
  LauncherEnv env;
  env.data_dir = dir.string() + "/";
  if (settings) env.settings_path = (dir / "settings.cfg").string();
  env.version = "test";
  return env;
}

LaunchHooks hooks_with(std::function<bool(int, LauncherState&, std::vector<SDL_Event>&)> fn) {
  LaunchHooks h;
  int guard = 0;
  h.per_frame = [fn = std::move(fn), guard](int frame, LauncherState& st, std::vector<SDL_Event>& inject) mutable {
    SDL_Delay(1);
    if (++guard > 20000) {
      std::fprintf(stderr, "  the loop did not finish\n");
      ++g_failures;
      return true;
    }
    return fn(frame, st, inject);
  };
  return h;
}

void test_quit() {
  const fs::path d = make_data("quit", true);
  const LaunchHooks h = hooks_with([](int frame, LauncherState&, std::vector<SDL_Event>& inject) {
    if (frame == 3) {
      SDL_Event q{};
      q.type = SDL_EVENT_QUIT;
      inject.push_back(q);
    }
    return false;
  });
  const LaunchResult r = run_launcher(env_for(d), &h);
  CHECK(!r.start);
}

void test_play_with_a_key() {
  const fs::path d = make_data("play", true);
  const LaunchHooks h = hooks_with([](int frame, LauncherState& st, std::vector<SDL_Event>& inject) {
    if (frame == 4) {
      CHECK(!st.in_import && st.selected == GameKind::Disk);
      inject.push_back(key_event(SDLK_RETURN));
    }
    return false;
  });
  const LaunchResult r = run_launcher(env_for(d), &h);
  CHECK(r.start && r.game == "disk");
  CHECK(read_text(d / "settings.cfg").find("last_game = disk") != std::string::npos);
}

void test_drop_with_a_fake_importer() {
  const fs::path d = make_data("drop", false);
  LauncherEnv env = env_for(d);
  env.first_run = true;
  std::vector<std::string> seen;
  env.importer = [&seen, d](const ImportRequest& req, JobContext& j) {
    seen = req.paths;
    j.progress(500, "Fake");
    write_text(d / "assets" / "manifest.txt", "importer_version=3\nvariant_name=Dropped release\ncanonical=true\npatched=false\n");
    ImportOutcome o;
    o.kind = ImportOutcome::Kind::Imported;
    o.game = GameKind::Disk;
    o.variant_name = "Dropped release";
    o.headline = "Imported: Dropped release";
    return o;
  };
  bool dropped = false;
  const LaunchHooks h = hooks_with([&](int frame, LauncherState& st, std::vector<SDL_Event>& inject) {
    if (frame == 3) {
      CHECK(st.in_import && !st.library.any());
      drop_events(inject, "/some/where/Disk 1.adf");
      dropped = true;
    }
    if (dropped && st.library.any() && !st.busy()) {
      CHECK(!st.in_import && st.selected == GameKind::Disk && st.library.get(GameKind::Disk).variant == "Dropped release");
      return true;
    }
    return false;
  });
  const LaunchResult r = run_launcher(std::move(env), &h);
  CHECK(!r.start && dropped);
  CHECK(seen.size() == 1 && seen[0] == "/some/where/Disk 1.adf");
}

void test_settings_saved_and_look() {
  const fs::path d = make_data("settings", true);
  write_text(d / "settings.cfg", "# my notes\nscale = 3\nmystery = 1\n");
  const LaunchHooks h = hooks_with([](int frame, LauncherState& st, std::vector<SDL_Event>& inject) {
    if (frame == 2) {
      st.model.s.scale = 6;
      st.model.s.ui_contrast = "high";
      st.model.s.ui_text = "large";
      st.model.s.ui_motion = "reduced";
    }
    if (frame == 8) {
      SDL_Event q{};
      q.type = SDL_EVENT_QUIT;
      inject.push_back(q);
    }
    return false;
  });
  const LaunchResult r = run_launcher(env_for(d), &h);
  CHECK(!r.start);
  const std::string file = read_text(d / "settings.cfg");
  CHECK(file.find("# my notes") != std::string::npos && file.find("mystery = 1") != std::string::npos);  // what was there is kept
  CHECK(file.find("scale = 6") != std::string::npos && file.find("ui_contrast = high") != std::string::npos);
}

SDL_Event pad_event(SDL_GamepadButton b) {
  SDL_Event e{};
  e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  e.gbutton.which = 77;
  e.gbutton.button = b;
  e.gbutton.down = true;
  return e;
}

// Every page of the launcher in the real loop, walked with a gamepad's shoulder buttons only: the host fills the Controls page's hooks and
// the platform, SDL and driver texts, and each page draws without a pointer or keyboard.
void test_pad_walk_in_the_real_loop() {
  const fs::path d = make_data("padwalk", true);
  std::vector<int> pages;
  bool hooks_ok = false, host_ok = false;
  const LaunchHooks h = hooks_with([&](int frame, LauncherState& st, std::vector<SDL_Event>& inject) {
    if (frame == 2) {
      hooks_ok = static_cast<bool>(st.controls_page) && static_cast<bool>(st.controls_modal);
      host_ok = !st.adv.host.platform.empty() && !st.adv.host.sdl_version.empty() && !st.about.host.build_id.empty() &&
                static_cast<bool>(st.adv.copy_text) && static_cast<bool>(st.adv.choose_image) && static_cast<bool>(st.adv.export_settings) &&
                static_cast<bool>(st.about.open_url) && static_cast<bool>(st.model.test_tone);
    }
    if (frame >= 3 && frame % 4 == 3) {
      pages.push_back(st.page);
      if (pages.size() > 8) {
        SDL_Event q{};
        q.type = SDL_EVENT_QUIT;
        inject.push_back(q);
      } else {
        inject.push_back(pad_event(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
      }
    }
    return false;
  });
  const LaunchResult r = run_launcher(env_for(d), &h);
  CHECK(!r.start);
  CHECK(hooks_ok && host_ok);
  // Play, Display, Audio, Controls, Packs, Advanced, About, then round to Play again.
  const std::vector<int> want = {0, 1, 2, 3, 4, 5, 6, 0, 1};
  CHECK(pages == want);
}

// A file dropped on the Advanced page is checked, not imported, in the real loop; an unreadable one ends in an error line.
void test_image_check_in_the_real_loop() {
  const fs::path d = make_data("imagecheck", true);
  bool started = false;
  const LaunchHooks h = hooks_with([&](int frame, LauncherState& st, std::vector<SDL_Event>& inject) {
    if (frame == 2) st.page = kAdvancedPage;
    if (frame == 4) drop_events(inject, (d / "not-an-image.adf").string());
    if (frame > 4 && !started && (st.adv.busy() || !st.adv.report.empty())) started = true;
    if (frame > 6 && started && !st.adv.busy() && !st.adv.report.empty()) {
      CHECK(!st.in_import && st.page == kAdvancedPage);
      CHECK(st.adv.report.back().severity == Severity::Error);
      SDL_Event q{};
      q.type = SDL_EVENT_QUIT;
      inject.push_back(q);
    }
    return false;
  });
  run_launcher(env_for(d), &h);
  CHECK(started);
}

void test_first_run_quit_with_the_keyboard() {
  const fs::path d = make_data("first_quit", false);
  LauncherEnv env = env_for(d);
  env.first_run = true;
  const LaunchHooks h = hooks_with([](int frame, LauncherState& st, std::vector<SDL_Event>& inject) {
    if (frame == 3) {
      CHECK(st.in_import);
      inject.push_back(key_event(SDLK_ESCAPE));  // Back with nothing imported quits
    }
    return false;
  });
  const LaunchResult r = run_launcher(std::move(env), &h);
  CHECK(!r.start);
}

// The user's own disk images dropped on the real window and imported by the real importer, the way a first run does it.
int test_real_import(const std::string& zips) {
  std::string zip;
  std::stringstream list(zips);
  for (std::string z; std::getline(list, z, ';');) {
    std::error_code ec;
    if (!z.empty() && fs::exists(z, ec)) {
      zip = z;
      break;
    }
  }
  if (zip.empty()) {
    std::printf("real import: skipped (none of the disk-image archives is present)\n");
    return 0;
  }
  const fs::path d = make_data("real", false);
  LauncherEnv env = env_for(d, false);
  env.first_run = true;
  bool dropped = false;
  const LaunchHooks h = hooks_with([&](int frame, LauncherState& st, std::vector<SDL_Event>& inject) {
    if (frame == 3) {
      drop_events(inject, zip);
      dropped = true;
    }
    if (dropped && frame > 6 && !st.busy()) {
      CHECK(st.library.get(GameKind::Disk).present);
      return true;
    }
    if (frame > 12000) return true;
    return false;
  });
  run_launcher(std::move(env), &h);
  std::string why;
  CHECK(x2::import::verify_assets((d / "assets").string(), why));
  CHECK(fs::exists(d / "assets" / "game_0400.bin") && fs::exists(d / "assets" / "disc2.bin") && fs::exists(d / "assets" / "manifest.txt"));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  g_work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_launcher_sdl_tests";
  std::error_code ec;
  fs::remove_all(g_work, ec);
  fs::create_directories(g_work, ec);
  // Without a usable window (no dummy driver in this SDL build) there is nothing to test here.
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::printf("SKIPPED: SDL video is not available: %s\n", SDL_GetError());
    return 77;
  }
  SDL_Quit();
  struct T {
    const char* name;
    void (*fn)();
  };
  const T tests[] = {{"quit", test_quit},
                     {"play_with_a_key", test_play_with_a_key},
                     {"drop_with_a_fake_importer", test_drop_with_a_fake_importer},
                     {"settings_saved_and_look", test_settings_saved_and_look},
                     {"first_run_quit", test_first_run_quit_with_the_keyboard},
                     {"pad_walk_real_loop", test_pad_walk_in_the_real_loop},
                     {"image_check_real_loop", test_image_check_in_the_real_loop}};
  for (const T& t : tests) {
    const int before = g_failures;
    t.fn();
    std::printf("%-28s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
  }
  test_real_import(argc > 2 ? argv[2] : "");
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
