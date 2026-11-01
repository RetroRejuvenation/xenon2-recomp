// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "launcher.h"

#include <cstdio>
#include <memory>
#include <mutex>

#include "pages/launcher_pages.h"
#include "ui_sdl.h"

namespace x2::ui {

namespace {

// File dialogs call back from another thread on some platforms: the picked paths wait here for the UI thread.
struct Picked {
  std::mutex mu;
  std::vector<std::string> paths;
};

void SDLCALL on_picked(void* userdata, const char* const* list, int) {
  auto* p = static_cast<Picked*>(userdata);
  if (list) {
    std::lock_guard<std::mutex> lock(p->mu);
    for (; *list; ++list) p->paths.emplace_back(*list);
  }
  SDL_Event wake{};
  wake.type = SDL_EVENT_USER;
  SDL_PushEvent(&wake);  // thread-safe: wakes the loop
}

std::string file_url(const std::string& path) {
  std::string u = "file://";
  if (path.empty() || path[0] != '/') u += '/';
  for (char ch : path) u += ch == '\\' ? '/' : ch;
  return u;
}

}  // namespace

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

  Picked picked;
  env.choose_files = [&] { SDL_ShowOpenFileDialog(on_picked, &picked, win, nullptr, 0, nullptr, true); };
  env.choose_folder = [&] { SDL_ShowOpenFolderDialog(on_picked, &picked, win, nullptr, false); };
  env.copy_text = [](const std::string& s) { SDL_SetClipboardText(s.c_str()); };
  const std::string data_dir = env.data_dir;
  env.open_data_folder = [data_dir] { SDL_OpenURL(file_url(data_dir).c_str()); };
  env.open_folder = [](const std::string& folder) { SDL_OpenURL(file_url(folder).c_str()); };
  env.choose_pack = [&]() -> std::string {  // the dialog is asynchronous here: the pick arrives as a drop on the page
    SDL_ShowOpenFileDialog(on_picked, &picked, win, nullptr, 0, nullptr, false);
    return std::string();
  };

  auto state = std::make_unique<LauncherState>(std::move(env));
  LauncherState& st = *state;
  SdlInput input;
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
        input.handle(e, evs);
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
      std::vector<std::string> chosen;
      {
        std::lock_guard<std::mutex> lock(picked.mu);
        chosen.swap(picked.paths);
      }
      if (!chosen.empty()) {
        redraw = true;
        st.drop(chosen);
      }
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
    ctx.pad_style = input.pad_style();
    input.set_geometry(geo);
    if (canvas.width() != geo.cw || canvas.height() != geo.ch) {
      canvas.resize(geo.cw, geo.ch);
      redraw = true;
    }
    const Uint64 now = SDL_GetTicks();
    const int dt = static_cast<int>(now - last_tick);
    last_tick = now;
    input.tick(dt, evs);
    if (!redraw && evs.empty()) continue;
    for (const Event& ev : evs) {
      ctx.run(canvas, {ev}, 0, builder);
      input.set_capture(ctx.capturing());
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
  state.reset();
  presenter.reset();  // its texture belongs to the renderer
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return result;
}

}  // namespace x2::ui
