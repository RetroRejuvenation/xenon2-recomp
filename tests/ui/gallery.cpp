// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A window to try the launcher UI kit by hand, and a way to write its frames as PNG without a window.
//
//   x2ui_gallery                          interactive: arrows/Enter/Esc/Tab/PageUp/PageDown, mouse, wheel, gamepad
//                                         F3 theme (high contrast), F4 reduced motion, F5 large text, F11 fullscreen, Ctrl+Q quit
//   x2ui_gallery --list                   the scene names
//   x2ui_gallery --shot FILE [--scene NAME] [--size WxH] [--hc] [--zoom N]   write one scene as PNG (no window)
//
// Developer tool: it is not installed and not part of a release. It shows mock data only.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "demo/demo_scenes.h"
#include "ui_sdl.h"

using namespace x2::ui;
using namespace x2::ui::demo;

namespace {
int shot(int argc, char** argv) {
  std::string file, scene = "play";
  int w = 640, h = 360, zoom = 2;
  bool hc = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) file = argv[++i];
    else if (a == "--scene" && i + 1 < argc) scene = argv[++i];
    else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
    else if (a == "--zoom" && i + 1 < argc) zoom = std::atoi(argv[++i]);
    else if (a == "--hc") hc = true;
  }
  const SceneSpec* s = find_scene(scene.c_str());
  if (!s) {
    std::fprintf(stderr, "no scene '%s' (try --list)\n", scene.c_str());
    return 2;
  }
  const Canvas c = render_scene(*s, w, h, hc);
  if (!write_png(c, file, zoom)) {
    std::fprintf(stderr, "cannot write %s\n", file.c_str());
    return 1;
  }
  std::printf("%s: %dx%d, hash %016llx\n", file.c_str(), w, h, static_cast<unsigned long long>(c.hash()));
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--list")) {
      for (const SceneSpec& s : scenes()) std::printf("%s\n", s.name);
      return 0;
    }
    if (!std::strcmp(argv[i], "--shot")) return shot(argc, argv);
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* win = nullptr;
  SDL_Renderer* ren = nullptr;
  if (!SDL_CreateWindowAndRenderer("x2recomp UI gallery", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) {
    std::fprintf(stderr, "window: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(ren, 1);

  SdlInput input;
  SdlPresenter presenter;
  Context ctx;
  DemoState st;
  Canvas canvas;
  bool large = false, hc = false, running = true, animating = true, fullscreen = false;
  Uint64 last = SDL_GetTicks();
  auto builder = [&](Context& c) { build(c, st); };

  while (running && !st.quit) {
    EventList evs;
    bool redraw = animating;
    SDL_Event e;
    bool have = animating ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, 250);
    while (have) {
      redraw = true;
      if (e.type == SDL_EVENT_QUIT) running = false;
      else if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
      else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_Q && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) running = false;
      else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F3) {
        hc = !hc;
        ctx.theme = hc ? Theme::high_contrast_theme() : Theme::standard();
      } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F4) ctx.motion.reduced = !ctx.motion.reduced;
      else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F5) large = !large;
      else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F11) {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(win, fullscreen);
      } else {
        SDL_ConvertEventToRenderCoordinates(ren, &e);
        input.handle(e, evs);
      }
      have = SDL_PollEvent(&e);
    }
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    const CanvasGeometry geo = compute_geometry(ow, oh, large);
    ctx.large_text = large;
    ctx.pad_style = input.pad_style();
    input.set_geometry(geo);
    if (canvas.width() != geo.cw || canvas.height() != geo.ch) canvas.resize(geo.cw, geo.ch);
    const Uint64 now = SDL_GetTicks();
    const int dt = static_cast<int>(now - last);
    last = now;
    input.tick(dt, evs);
    if (!redraw && evs.empty()) continue;
    for (const Event& ev : evs) {
      ctx.run(canvas, {ev}, 0, builder);
      input.set_capture(ctx.capturing());
    }
    animating = ctx.run(canvas, {}, dt, builder);
    presenter.draw(ren, canvas, geo, ctx.theme.c.ground);
    SDL_RenderPresent(ren);
  }
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
