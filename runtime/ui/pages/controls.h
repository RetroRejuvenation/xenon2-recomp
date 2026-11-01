// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Controls page: keyboard bindings for every action (input.cfg), the gamepad buttons for fire, the second button and the menu, and the
// rows of the settings schema that belong with them (autofire, keyboard grab, cursor). A binding is changed by pressing the key or button;
// a key that another action already uses, or that the game itself reads, is asked about first. The model holds the bindings being edited;
// the host saves and applies them when `changed` is set (the in-game menu does it after every pass).
#pragma once
#include <string>

#include "../../input/input.h"
#include "pages.h"

namespace x2::ui {

struct ControlsModel {
  input::Bindings b;         // being edited
  input::Bindings in_force;  // what the game has now (restore-defaults and dirty() compare with the platform defaults and this)
  std::string message;       // what the last change did, shown in the help line
  bool changed = false;      // the bindings changed in the last pass: the host saves and applies them, then clears this

  struct Pending {
    enum Kind { None, Key, Pad } kind = None;
    int action = -1;                    // Key: the action being bound; Pad: 0 fire, 1 button 2, 2 menu
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
    bool add = false;                   // add to the list instead of replacing it
    int other = -1;                     // the action or pad list it is taken from, -1 when only the game uses it
    std::string text;                   // what the confirmation says
  } pending;

  void open(const input::Bindings& live) {
    b = in_force = live;
    message.clear();
    changed = false;
    pending = {};
  }
  bool differs_from_defaults() const { return !(b == input::default_bindings()); }
};

// Human names for the page: the action's row label and its help line.
const char* action_label(int action);
const char* action_help(int action);
// A gamepad button as the pad's own labels write it ("A" or "Cross").
std::string pad_caption(SDL_GamepadButton b, PadStyle style);

// Whole page: heading, scrolling rows (gamepad, the schema's control rows, keyboard), help line. `sm.s` is edited by the schema rows.
// Returns true when a setting (not a binding) changed in this pass.
bool page_controls(Context& ctx, ControlsModel& cm, SettingsModel& sm, Rect body, Rect help, bool with_heading = true);
// The confirmation about a key or button that is taken; build it last, after everything else in the frame.
void controls_modal(Context& ctx, ControlsModel& cm);

}  // namespace x2::ui
