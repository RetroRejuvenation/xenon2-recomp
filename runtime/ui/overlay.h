// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The in-game menu: F1 (or the gamepad's menu button) pauses the game and opens a panel with the Faithful switch and the same Display,
// Audio, Controls, Asset packs and Assists pages the launcher has. Changes take effect at once where the running release can take them (the
// same code paths as the hotkeys, through LiveHooks) and are written to settings.cfg and input.cfg. The window loops of the Amiga and
// Atari ST releases hold one Overlay, feed it the SDL events while it is active and draw it over the picture; neither learns anything about
// widgets. docs/re-notes/gui-ingame-menu.md.
#pragma once
#include <SDL3/SDL.h>

#include <functional>
#include <string>

#include "../input/input.h"
#include "live_apply.h"
#include "pages/controls.h"
#include "ui_sdl.h"

namespace x2::ui {

struct OverlayHooks {
  // The settings the running game is using now (called when the menu opens): the saved file with the command line and the hotkeys' changes.
  std::function<Settings()> current;
  // Applies the change to the running game and keeps it in settings.cfg; returns what took effect and what waits for the next start.
  std::function<LiveResult(const Settings& before, const Settings& after)> apply;
  // Puts the bindings in force and writes input.cfg. Returns false with a reason when the file could not be written.
  std::function<bool(const input::Bindings&, std::string& why)> save_bindings;
  std::function<void()> reset_game;
  std::function<void()> quit;
  std::function<void()> test_tone;  // optional
  // The menu opened (true) or closed (false): the host freezes the game exactly as for the P pause (audio stopped, the pointer shown) and
  // thaws it again.
  std::function<void(bool open)> set_open;
};

class Overlay {
 public:
  enum class Screen { Top, Display, Audio, Controls, Packs, Assists };

  Overlay(GameKind game, OverlayHooks hooks);

  bool active() const { return active_; }
  // `out_w` x `out_h` is the output size in pixels (the window); `live` the bindings in force.
  void open(int out_w, int out_h, const input::Bindings& live);
  void close();
  void toggle_menu(int out_w, int out_h, const input::Bindings& live) {
    if (active_) close();
    else open(out_w, out_h, live);
  }

  // One SDL event, while active. Keyboard, mouse and gamepad events drive the menu; the keys bound to `menu` and `quit` always do their job.
  void handle(const SDL_Event& e);
  // Once per loop iteration while active: the held d-pad or stick repeats, and animations advance. `dt_ms` is the time since the last call.
  void tick(int dt_ms);
  // Draws the menu over the picture the renderer holds. Call after the picture is drawn and before the frame is presented.
  void draw(SDL_Renderer* r, int out_w, int out_h);
  // The menu as pixels, for tests and screenshots: the canvas for a window of this size. Does not need a renderer.
  const Canvas& render(int out_w, int out_h);

  Screen screen() const { return screen_; }
  void show(Screen s);  // tests
  SettingsModel& settings() { return sm_; }
  ControlsModel& controls() { return cm_; }
  const std::string& status() const { return status_; }
  bool animating() const { return animating_; }
  int canvas_scale() const { return geo_.n; }

 private:
  void layout(int out_w, int out_h);
  void run(const EventList& events, int dt_ms);
  void build(Context& ctx);
  void top_screen(Context& ctx, Rect area, Rect help);
  void settings_screen(Context& ctx, Rect area, Rect help, const char* title, PageBit page, bool tone, bool expert);
  bool confirm_modal(Context& ctx, const char* id, const char* title, const char* text, const char* yes);
  void after_pass();

  GameKind game_;
  OverlayHooks hooks_;
  bool active_ = false, animating_ = false;
  Screen screen_ = Screen::Top;
  Context ctx_;
  Canvas canvas_;
  CanvasGeometry geo_;
  SdlInput input_;
  SdlPresenter presenter_;
  SDL_Renderer* renderer_ = nullptr;
  int out_w_ = 0, out_h_ = 0;
  SettingsModel sm_;
  ControlsModel cm_;
  Settings applied_;  // what the host has been told
  std::string status_;
};

}  // namespace x2::ui
