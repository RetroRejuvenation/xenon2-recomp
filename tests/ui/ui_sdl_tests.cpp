// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The SDL side of the UI kit: SDL events to UI events (keys, pointer mapping through the canvas geometry, wheel, pad
// buttons, d-pad and stick repeat, key capture) and the presenter (a canvas shown through a renderer must have the
// right colours at the right places). Runs under SDL's dummy video driver; Skipped (77) if SDL cannot start.
#include <cstdio>
#include <cstring>

#include "demo/demo_pages.h"
#include "ui_sdl.h"

using namespace x2::ui;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                        \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(c)) {                                                         \
      ++g_fail;                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    }                                                                   \
  } while (0)

SDL_Event key_event(SDL_Keycode k, bool repeat = false, SDL_Keymod mod = SDL_KMOD_NONE) {
  SDL_Event e;
  std::memset(&e, 0, sizeof e);
  e.type = SDL_EVENT_KEY_DOWN;
  e.key.key = k;
  e.key.mod = mod;
  e.key.repeat = repeat;
  return e;
}

SDL_Event pad_button(Uint8 b, bool down) {
  SDL_Event e;
  std::memset(&e, 0, sizeof e);
  e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
  e.gbutton.button = b;
  e.gbutton.which = 0;
  return e;
}

SDL_Event pad_axis(Uint8 axis, Sint16 v) {
  SDL_Event e;
  std::memset(&e, 0, sizeof e);
  e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
  e.gaxis.axis = axis;
  e.gaxis.value = v;
  return e;
}

Nav only_nav(const EventList& l) { return l.size() == 1 && l[0].type == Event::Type::NavKey ? l[0].nav : Nav::None; }

void test_keys() {
  SdlInput in;
  struct Case {
    SDL_Keycode k;
    SDL_Keymod mod;
    Nav want;
  } cases[] = {{SDLK_UP, SDL_KMOD_NONE, Nav::Up},         {SDLK_DOWN, SDL_KMOD_NONE, Nav::Down},
               {SDLK_LEFT, SDL_KMOD_NONE, Nav::Left},     {SDLK_RIGHT, SDL_KMOD_NONE, Nav::Right},
               {SDLK_RETURN, SDL_KMOD_NONE, Nav::Accept}, {SDLK_KP_ENTER, SDL_KMOD_NONE, Nav::Accept},
               {SDLK_SPACE, SDL_KMOD_NONE, Nav::Accept},  {SDLK_ESCAPE, SDL_KMOD_NONE, Nav::Back},
               {SDLK_TAB, SDL_KMOD_NONE, Nav::PageNext},  {SDLK_TAB, SDL_KMOD_LSHIFT, Nav::PagePrev},
               {SDLK_PAGEUP, SDL_KMOD_NONE, Nav::PagePrev}, {SDLK_PAGEDOWN, SDL_KMOD_NONE, Nav::PageNext},
               {SDLK_HOME, SDL_KMOD_NONE, Nav::First},    {SDLK_END, SDL_KMOD_NONE, Nav::Last},
               {SDLK_DELETE, SDL_KMOD_NONE, Nav::Default}, {SDLK_BACKSPACE, SDL_KMOD_NONE, Nav::Default},
               {SDLK_F1, SDL_KMOD_NONE, Nav::Menu},       {SDLK_I, SDL_KMOD_NONE, Nav::Details}};
  for (const Case& c : cases) {
    EventList out;
    in.handle(key_event(c.k, false, c.mod), out);
    CHECK(only_nav(out) == c.want);
    CHECK(out.empty() || out[0].device == Device::Keyboard);
  }
  EventList out;
  in.handle(key_event(SDLK_DOWN, true), out);  // OS key repeat: moves are accepted
  CHECK(only_nav(out) == Nav::Down);
  out.clear();
  in.handle(key_event(SDLK_RETURN, true), out);  // but not repeated presses of Enter
  CHECK(out.empty());
  out.clear();
  in.handle(key_event(SDLK_Z), out);  // unmapped
  CHECK(out.empty());
  // key capture: a press becomes a RawKey, whatever the key
  in.set_capture(true);
  in.handle(key_event(SDLK_ESCAPE), out);
  CHECK(out.size() == 1 && out[0].type == Event::Type::RawKey && out[0].key == static_cast<int>(SDLK_ESCAPE));
  out.clear();
  in.handle(key_event(SDLK_A, true), out);  // no repeats while capturing
  CHECK(out.empty());
  in.set_capture(false);
}

void test_pointer() {
  SdlInput in;
  CanvasGeometry g;
  g.cw = 100;
  g.ch = 60;
  g.n = 2;
  g.ox = 10;
  g.oy = 4;
  in.set_geometry(g);
  EventList out;
  SDL_Event e;
  std::memset(&e, 0, sizeof e);
  e.type = SDL_EVENT_MOUSE_MOTION;
  e.motion.x = 31.0f;
  e.motion.y = 12.0f;
  in.handle(e, out);
  CHECK(out.size() == 1 && out[0].type == Event::Type::PointerMove && out[0].x == 10 && out[0].y == 4);
  out.clear();
  e.motion.x = 5.0f;  // left of the canvas: negative coordinates, not a crash
  in.handle(e, out);
  CHECK(out.size() == 1 && out[0].x < 0);
  out.clear();
  std::memset(&e, 0, sizeof e);
  e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  e.button.button = SDL_BUTTON_LEFT;
  e.button.x = 50.0f;
  e.button.y = 40.0f;
  in.handle(e, out);
  e.type = SDL_EVENT_MOUSE_BUTTON_UP;
  in.handle(e, out);
  CHECK(out.size() == 2 && out[0].type == Event::Type::PointerDown && out[1].type == Event::Type::PointerUp);
  CHECK(out[0].x == 20 && out[0].y == 18);
  out.clear();
  e.button.button = SDL_BUTTON_RIGHT;
  in.handle(e, out);
  CHECK(out.empty());
  std::memset(&e, 0, sizeof e);
  e.type = SDL_EVENT_MOUSE_WHEEL;
  e.wheel.integer_y = 2;  // away from the user: scroll up
  e.wheel.y = 2.0f;
  e.wheel.mouse_x = 30.0f;
  e.wheel.mouse_y = 20.0f;
  in.handle(e, out);
  CHECK(out.size() == 1 && out[0].type == Event::Type::Scroll && out[0].dy == -2 && out[0].x == 10 && out[0].y == 8);
  out.clear();
  e.wheel.integer_y = -1;
  e.wheel.y = -1.0f;
  in.handle(e, out);
  CHECK(out.size() == 1 && out[0].dy == 1);
  out.clear();
  e.wheel.integer_y = 0;
  e.wheel.y = 0.3f;  // a small smooth-scroll step still moves one line
  in.handle(e, out);
  CHECK(out.size() == 1 && out[0].dy == -1);
}

void test_pad() {
  SdlInput in;
  struct Case {
    Uint8 b;
    Nav want;
  } cases[] = {{SDL_GAMEPAD_BUTTON_SOUTH, Nav::Accept},  {SDL_GAMEPAD_BUTTON_EAST, Nav::Back},
               {SDL_GAMEPAD_BUTTON_NORTH, Nav::Details}, {SDL_GAMEPAD_BUTTON_WEST, Nav::Default},
               {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Nav::PagePrev}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Nav::PageNext},
               {SDL_GAMEPAD_BUTTON_START, Nav::Menu},    {SDL_GAMEPAD_BUTTON_BACK, Nav::ToggleFooter}};
  for (const Case& c : cases) {
    EventList out;
    in.handle(pad_button(c.b, true), out);
    CHECK(only_nav(out) == c.want && out[0].device == Device::Pad);
    out.clear();
    in.handle(pad_button(c.b, false), out);
    CHECK(out.empty());
  }
  // a held d-pad direction: once at once, then after 400 ms every 80 ms; releasing stops it
  EventList out;
  in.handle(pad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true), out);
  CHECK(only_nav(out) == Nav::Down);
  out.clear();
  in.tick(399, out);
  CHECK(out.empty());
  in.tick(1, out);
  CHECK(only_nav(out) == Nav::Down);
  out.clear();
  in.tick(160, out);
  CHECK(out.size() == 2);
  out.clear();
  in.handle(pad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false), out);
  in.tick(1000, out);
  CHECK(out.empty());
  // left stick: dead zone and hysteresis, dominant axis wins
  in.handle(pad_axis(SDL_GAMEPAD_AXIS_LEFTX, 8000), out);
  CHECK(out.empty());
  in.handle(pad_axis(SDL_GAMEPAD_AXIS_LEFTX, 20000), out);
  CHECK(only_nav(out) == Nav::Right);
  out.clear();
  in.handle(pad_axis(SDL_GAMEPAD_AXIS_LEFTX, 13000), out);  // still held: above the release threshold
  in.tick(10, out);
  CHECK(out.empty());
  in.handle(pad_axis(SDL_GAMEPAD_AXIS_LEFTY, -30000), out);  // now mostly up
  CHECK(only_nav(out) == Nav::Up);
  out.clear();
  in.handle(pad_axis(SDL_GAMEPAD_AXIS_LEFTX, 0), out);  // centre both axes: no new direction appears on the way
  in.handle(pad_axis(SDL_GAMEPAD_AXIS_LEFTY, 0), out);
  in.tick(1000, out);
  CHECK(out.empty());
  CHECK(pad_style_from_type(SDL_GAMEPAD_TYPE_PS5) == PadStyle::PlayStation);
  CHECK(pad_style_from_type(SDL_GAMEPAD_TYPE_XBOXONE) == PadStyle::Xbox);
  CHECK(pad_style_from_type(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) == PadStyle::Nintendo);
  CHECK(pad_style_from_type(SDL_GAMEPAD_TYPE_STANDARD) == PadStyle::Generic);
}

// An SDL event stream drives a UI frame: a keyboard Down then Accept presses the second button, a pad press
// switches the hints to pad names.
void test_end_to_end() {
  using namespace x2::ui::demo;
  SdlInput in;
  Context ctx;
  ctx.motion.reduced = true;
  DemoState st;
  st.page = 0;
  Canvas canvas(640, 360);
  CanvasGeometry g = compute_geometry(1280, 720, false);
  CHECK(g.cw == 640 && g.ch == 360);
  in.set_geometry(g);
  auto builder = [&](Context& c) { build(c, st); };
  ctx.run(canvas, {}, 0, builder);
  EventList evs;
  in.handle(key_event(SDLK_RIGHT), evs);
  in.handle(key_event(SDLK_RETURN), evs);
  for (const Event& e : evs) ctx.run(canvas, {e}, 0, builder);
  CHECK(st.game == Game::Cdtv);  // Right moved to the second card, Accept chose it
  evs.clear();
  in.handle(pad_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true), evs);
  for (const Event& e : evs) ctx.run(canvas, {e}, 0, builder);
  CHECK(ctx.device() == Device::Pad);
}

void test_presenter() {
  SDL_Window* win = SDL_CreateWindow("ui test", 64, 48, SDL_WINDOW_HIDDEN);
  if (!win) {
    std::printf("no window under this video driver (%s): presenter test skipped\n", SDL_GetError());
    return;
  }
  SDL_Renderer* ren = SDL_CreateRenderer(win, "software");
  if (!ren) {
    std::printf("no software renderer (%s): presenter test skipped\n", SDL_GetError());
    SDL_DestroyWindow(win);
    return;
  }
  Canvas c(8, 6);
  c.clear(rgb(10, 20, 30));
  c.put(0, 0, rgb(255, 0, 0));
  c.put(7, 5, rgb(0, 255, 0));
  CanvasGeometry g;
  g.cw = 8;
  g.ch = 6;
  g.n = 4;
  g.ox = 8;
  g.oy = 2;
  SdlPresenter p;
  CHECK(p.draw(ren, c, g, rgb(1, 2, 3)));
  SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr);
  CHECK(s != nullptr);
  if (s) {
    SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    CHECK(rgba != nullptr);
    if (rgba) {
      auto px = [&](int x, int y) {
        const Uint8* p8 = static_cast<const Uint8*>(rgba->pixels) + y * rgba->pitch + x * 4;
        return rgb(p8[0], p8[1], p8[2]);
      };
      CHECK(px(0, 0) == rgb(1, 2, 3));                   // letterbox
      CHECK(px(8, 2) == rgb(255, 0, 0));                 // canvas pixel (0,0), top-left of the 4x4 block
      CHECK(px(11, 5) == rgb(255, 0, 0));                // its far corner: nearest-neighbour, no blending
      CHECK(px(12, 2) == rgb(10, 20, 30));               // the next canvas pixel
      CHECK(px(8 + 7 * 4 + 3, 2 + 5 * 4 + 3) == rgb(0, 255, 0));
      SDL_DestroySurface(rgba);
    }
    SDL_DestroySurface(s);
  }
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
}
}  // namespace

int main() {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::printf("SDL_Init failed (%s): skipped\n", SDL_GetError());
    return 77;
  }
  test_keys();
  test_pointer();
  test_pad();
  test_end_to_end();
  test_presenter();
  SDL_Quit();
  std::printf("%d checks, %d failures\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
