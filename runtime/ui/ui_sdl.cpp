// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "ui_sdl.h"

#include <cmath>
#include <cstdlib>

namespace x2::ui {

PadStyle pad_style_from_type(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
      return PadStyle::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return PadStyle::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return PadStyle::Nintendo;
    default:
      return PadStyle::Generic;
  }
}

namespace {
int to_canvas(float v, int offset, int n) { return static_cast<int>(std::floor((v - static_cast<float>(offset)) / static_cast<float>(n))); }

Nav key_to_nav(SDL_Keycode k, SDL_Keymod mod, bool& repeatable) {
  repeatable = false;
  switch (k) {
    case SDLK_UP: repeatable = true; return Nav::Up;
    case SDLK_DOWN: repeatable = true; return Nav::Down;
    case SDLK_LEFT: repeatable = true; return Nav::Left;
    case SDLK_RIGHT: repeatable = true; return Nav::Right;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE: return Nav::Accept;
    case SDLK_ESCAPE: return Nav::Back;
    case SDLK_TAB: return (mod & SDL_KMOD_SHIFT) ? Nav::PagePrev : Nav::PageNext;
    case SDLK_PAGEUP: return Nav::PagePrev;
    case SDLK_PAGEDOWN: return Nav::PageNext;
    case SDLK_HOME: return Nav::First;
    case SDLK_END: return Nav::Last;
    case SDLK_DELETE:
    case SDLK_BACKSPACE: return Nav::Default;
    case SDLK_F1: return Nav::Menu;
    case SDLK_F2: return Nav::ToggleFooter;
    case SDLK_I: return Nav::Details;
    default: return Nav::None;
  }
}

Nav button_to_nav(Uint8 b, bool& is_dpad) {
  is_dpad = false;
  switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: is_dpad = true; return Nav::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: is_dpad = true; return Nav::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: is_dpad = true; return Nav::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: is_dpad = true; return Nav::Right;
    case SDL_GAMEPAD_BUTTON_SOUTH: return Nav::Accept;
    case SDL_GAMEPAD_BUTTON_EAST: return Nav::Back;
    case SDL_GAMEPAD_BUTTON_NORTH: return Nav::Details;
    case SDL_GAMEPAD_BUTTON_WEST: return Nav::Default;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Nav::PagePrev;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Nav::PageNext;
    case SDL_GAMEPAD_BUTTON_START: return Nav::Menu;
    case SDL_GAMEPAD_BUTTON_BACK: return Nav::ToggleFooter;
    default: return Nav::None;
  }
}
}  // namespace

void SdlInput::update_held(EventList& out, int dt_ms) {
  repeat_.update(dpad_ != Nav::None ? dpad_ : stick_, dt_ms, out, Device::Pad);
}

void SdlInput::tick(int dt_ms, EventList& out) { update_held(out, dt_ms); }

void SdlInput::handle(const SDL_Event& e, EventList& out) {
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
      if (capture_) {
        if (!e.key.repeat) out.push_back(Event::raw_key(static_cast<int>(e.key.key), static_cast<int>(e.key.scancode)));
        return;
      }
      bool repeatable = false;
      const Nav n = key_to_nav(e.key.key, e.key.mod, repeatable);
      if (n != Nav::None && (!e.key.repeat || repeatable)) out.push_back(Event::nav_key(n, Device::Keyboard));
      return;
    }
    case SDL_EVENT_MOUSE_MOTION:
      out.push_back(Event::pointer(Event::Type::PointerMove, to_canvas(e.motion.x, geo_.ox, geo_.n), to_canvas(e.motion.y, geo_.oy, geo_.n)));
      return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (e.button.button != SDL_BUTTON_LEFT) return;
      out.push_back(Event::pointer(e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? Event::Type::PointerDown : Event::Type::PointerUp,
                                   to_canvas(e.button.x, geo_.ox, geo_.n), to_canvas(e.button.y, geo_.oy, geo_.n)));
      return;
    case SDL_EVENT_MOUSE_WHEEL: {
      int lines = -static_cast<int>(e.wheel.integer_y);
      if (lines == 0 && e.wheel.y != 0.0f) lines = e.wheel.y > 0 ? -1 : 1;
      if (lines != 0)
        out.push_back(Event::scroll(lines, to_canvas(e.wheel.mouse_x, geo_.ox, geo_.n), to_canvas(e.wheel.mouse_y, geo_.oy, geo_.n)));
      return;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      pad_style_ = pad_style_from_type(SDL_GetGamepadTypeForID(e.gbutton.which));
      if (capture_) {  // a gamepad button is the answer to "press a button"
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) out.push_back(Event::raw_key(kPadKeyBase + static_cast<int>(e.gbutton.button), 0, Device::Pad));
        return;
      }
      bool dpad = false;
      const Nav n = button_to_nav(e.gbutton.button, dpad);
      const bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      if (n == Nav::None) return;
      if (dpad) {
        dpad_ = down ? n : (dpad_ == n ? Nav::None : dpad_);
        update_held(out, 0);
      } else if (down) {
        out.push_back(Event::nav_key(n, Device::Pad));
      }
      return;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) return;
      (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? ax_ : ay_) = e.gaxis.value;
      const int threshold = stick_ == Nav::None ? 16000 : 12000;  // hysteresis
      Nav n = Nav::None;
      if (std::abs(ax_) >= threshold || std::abs(ay_) >= threshold)
        n = std::abs(ax_) > std::abs(ay_) ? (ax_ < 0 ? Nav::Left : Nav::Right) : (ay_ < 0 ? Nav::Up : Nav::Down);
      stick_ = n;
      update_held(out, 0);
      return;
    }
    default:
      return;
  }
}

SdlPresenter::~SdlPresenter() {
  if (tex_) SDL_DestroyTexture(tex_);
}

bool SdlPresenter::upload(SDL_Renderer* r, const Canvas& c) {
  if (!r || c.width() <= 0 || c.height() <= 0) return false;
  if (!tex_ || renderer_ != r || tw_ != c.width() || th_ != c.height()) {
    if (tex_) SDL_DestroyTexture(tex_);
    tex_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, c.width(), c.height());
    renderer_ = r;
    tw_ = c.width();
    th_ = c.height();
    if (!tex_) return false;
    SDL_SetTextureScaleMode(tex_, SDL_SCALEMODE_NEAREST);
  }
  return SDL_UpdateTexture(tex_, nullptr, c.data(), c.width() * 4);
}

bool SdlPresenter::draw(SDL_Renderer* r, const Canvas& c, const CanvasGeometry& g, Color letterbox) {
  if (!upload(r, c)) return false;
  SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(r, static_cast<Uint8>(red_of(letterbox)), static_cast<Uint8>(green_of(letterbox)),
                         static_cast<Uint8>(blue_of(letterbox)), 255);
  SDL_RenderClear(r);
  const SDL_FRect dst{static_cast<float>(g.ox), static_cast<float>(g.oy), static_cast<float>(c.width() * g.n),
                      static_cast<float>(c.height() * g.n)};
  return SDL_RenderTexture(r, tex_, nullptr, &dst);
}

bool SdlPresenter::draw_over(SDL_Renderer* r, const Canvas& c, const CanvasGeometry& g) {
  if (!upload(r, c)) return false;
  SDL_SetTextureBlendMode(tex_, SDL_BLENDMODE_BLEND);
  const SDL_FRect dst{static_cast<float>(g.ox), static_cast<float>(g.oy), static_cast<float>(c.width() * g.n),
                      static_cast<float>(c.height() * g.n)};
  return SDL_RenderTexture(r, tex_, nullptr, &dst);
}

}  // namespace x2::ui
