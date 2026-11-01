// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "launcher.h"

#include <cstdio>
#include <memory>
#include <string>

#include "host_actions.h"
#include "input/input.h"
#include "log_buffer.h"
#include "pages/controls.h"
#include "pages/launcher_pages.h"
#include "ui_sdl.h"

namespace x2::ui {

LaunchResult run_launcher(LauncherEnv env, const LaunchHooks* hooks) {
  LaunchResult result;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "x2recomp: SDL_Init failed: %s (use --no-gui)\n", SDL_GetError());
    return result;
  }
  SDL_Window* win = nullptr;
  SDL_Renderer* ren = nullptr;
  const int ww = hooks ? hooks->window_w : 960, wh = hooks ? hooks->window_h : 540;
  if (!SDL_CreateWindowAndRenderer("x2recomp", ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) {
    std::fprintf(stderr, "x2recomp: cannot open the launcher window: %s (use --no-gui)\n", SDL_GetError());
    SDL_Quit();
    return result;
  }
  SDL_SetRenderVSync(ren, 1);

  SDL_InitSubSystem(SDL_INIT_AUDIO);  // for the driver's name in About and for the test tone; a machine without sound still gets a launcher
  FilePicker picked, images, exports, imports;  // import files and packs (they arrive as drops), images to check, the settings file's two dialogs
  TestTone tone;
  env.choose_files = [&] { picked.show_files(win, true); };
  env.choose_folder = [&] { picked.show_folder(win); };
  env.copy_text = [](const std::string& s) { SDL_SetClipboardText(s.c_str()); };
  const std::string data_dir = env.data_dir;
  env.open_data_folder = [data_dir] { SDL_OpenURL(file_url(data_dir).c_str()); };
  env.open_folder = [](const std::string& folder) { SDL_OpenURL(file_url(folder).c_str()); };
  env.open_url = [](const std::string& url) { SDL_OpenURL(url.c_str()); };
  env.open_log = [] {
    const std::string log = LogBuffer::instance().file_path();
    if (!log.empty()) SDL_OpenURL(file_url(log).c_str());
  };
  env.choose_pack = [&]() -> std::string {  // the dialog is asynchronous here: the pick arrives as a drop on the page
    picked.show_files(win, false);
    return std::string();
  };
  env.choose_image = [&] { images.show_files(win, true); };
  env.export_settings = [&] { exports.show_save(win, "settings.cfg"); };
  env.import_settings = [&] { imports.show_files(win, false); };
  env.host = make_host_info(win);
  LauncherState* state_ptr = nullptr;
  env.test_tone = [&] {
    std::string why;
    const int volume = state_ptr ? state_ptr->model.s.volume : 100;
    if (!tone.play(volume, &why)) log_warn("test tone: " + why);
  };

  auto state = std::make_unique<LauncherState>(std::move(env));
  LauncherState& st = *state;
  state_ptr = &st;
  ControlsModel controls;
  controls.open(input::preview_bindings(data_dir).bindings);
  st.controls_page = [&](Context& c, SettingsModel& m, Rect body, Rect help) {
    const bool changed = page_controls(c, controls, m, body, help);
    if (controls.changed) {  // a binding changed: input.cfg becomes the user's
      controls.changed = false;
      std::string why;
      if (input::save_bindings_file(data_dir, controls.b, why)) log_info("key bindings saved");
      else {
        controls.message = "Not saved: " + why;
        log_warn("key bindings: " + why);
      }
      controls.in_force = controls.b;
    }
    return changed;
  };
  st.controls_modal = [&](Context& c) { controls_modal(c, controls); };
  st.model.hotkey = [&](const std::string& action) -> std::string {
    for (int a = 0; a < input::kActionCount; ++a)
      if (action == input::action_name(a)) {
        const auto& keys = controls.in_force.keys[static_cast<size_t>(a)];
        return keys.empty() ? std::string() : input::key_label(keys.front());
      }
    return {};
  };
  SdlInput ui_in;
  auto presenter = std::make_unique<SdlPresenter>();
  Context ctx;
  Canvas canvas;
  std::vector<std::string> drops;
  bool drop_open = false, fullscreen = false, animating = true;
  Uint64 last_drop = 0, last_tick = SDL_GetTicks(), last_save = last_tick;
  int frame_no = 0;
  auto builder = [&](Context& c) { launcher_frame(c, st); };

  while (!st.quit && !st.start) {
    EventList evs;
    bool redraw = animating || st.animating();
    std::vector<SDL_Event> events;
    if (hooks && hooks->per_frame) {
      std::vector<SDL_Event> inject;
      if (hooks->per_frame(frame_no, st, inject)) break;
      for (SDL_Event copy : inject) SDL_PushEvent(&copy);
    }
    ++frame_no;
    SDL_Event e;
    bool have = (animating || st.animating() || hooks) ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, 250);
    while (have) {
      redraw = true;
      if (e.type == SDL_EVENT_QUIT) st.quit = true;
      else if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
      else if (e.type == SDL_EVENT_DROP_BEGIN) drop_open = true;
      else if (e.type == SDL_EVENT_DROP_FILE) {
        if (e.drop.data) drops.emplace_back(e.drop.data);
        last_drop = SDL_GetTicks();
      } else if (e.type == SDL_EVENT_DROP_COMPLETE) {
        drop_open = false;
        if (!drops.empty()) st.drop(drops);
        drops.clear();
        last_drop = 0;
      } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_Q && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
        st.quit = true;
      } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                 (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(win, fullscreen);
      } else if (e.type != SDL_EVENT_USER) {
        SDL_ConvertEventToRenderCoordinates(ren, &e);
        ui_in.handle(e, evs);
      }
      have = SDL_PollEvent(&e);
    }
    // Platforms that send no DROP_COMPLETE: take the files 300 ms after the last one.
    if (!drop_open && last_drop && SDL_GetTicks() - last_drop > 300 && !drops.empty()) {
      st.drop(drops);
      drops.clear();
      last_drop = 0;
    }
    {
      const std::vector<std::string> chosen = picked.take();
      if (!chosen.empty()) {
        redraw = true;
        st.drop(chosen);
      }
      const std::vector<std::string> to_check = images.take();
      if (!to_check.empty()) {
        redraw = true;
        st.check_images(to_check);
      }
      const std::vector<std::string> to_export = exports.take();
      if (!to_export.empty()) {
        redraw = true;
        st.export_settings_to(to_export.front());
      }
      const std::vector<std::string> to_import = imports.take();
      if (!to_import.empty()) {
        redraw = true;
        st.import_settings_from(to_import.front());
      }
      tone.service();
    }
    st.poll();
    if (st.animating()) redraw = true;

    // The look follows the settings, so the toggles on the pages act at once.
    const Settings& s = st.model.s;
    const bool large = s.ui_text == "large";
    const bool hc = s.ui_contrast == "high";
    if (hc != ctx.theme.high_contrast) {
      ctx.theme = hc ? Theme::high_contrast_theme() : Theme::standard();
      redraw = true;
    }
    ctx.motion.reduced = s.ui_motion == "reduced";
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    const CanvasGeometry geo = compute_geometry(ow, oh, large);
    ctx.large_text = large;
    ctx.pad_style = ui_in.pad_style();
    ui_in.set_geometry(geo);
    if (canvas.width() != geo.cw || canvas.height() != geo.ch) {
      canvas.resize(geo.cw, geo.ch);
      redraw = true;
    }
    const Uint64 now = SDL_GetTicks();
    const int dt = static_cast<int>(now - last_tick);
    last_tick = now;
    ui_in.tick(dt, evs);
    if (!redraw && evs.empty()) continue;
    for (const Event& ev : evs) {
      ctx.run(canvas, {ev}, 0, builder);
      ui_in.set_capture(ctx.capturing());
    }
    animating = ctx.run(canvas, {}, dt, builder);
    presenter->draw(ren, canvas, geo, ctx.theme.c.ground);
    SDL_RenderPresent(ren);
    if (st.settings_dirty() && now - last_save > 400) {
      st.save_settings();
      last_save = now;
    }
  }
  st.save_settings();
  if (st.start) {
    result.start = true;
    result.game = game_flag_value(st.start_game);
  }
  tone.stop();
  state.reset();
  presenter.reset();  // its texture belongs to the renderer
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return result;
}

}  // namespace x2::ui
