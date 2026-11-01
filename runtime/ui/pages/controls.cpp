// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "controls.h"

#include <algorithm>

namespace x2::ui {

namespace {

using input::Action;

struct ActionInfo {
  const char* label;  // at most 18 characters, the width of the label column
  const char* help;
};

const ActionInfo kInfo[input::kActionCount] = {
    /* P2Up */ {"Move up", "Joystick port 2, the one the game reads: up."},
    /* P2Down */ {"Move down", "Joystick port 2: down."},
    /* P2Left */ {"Move left", "Joystick port 2: left."},
    /* P2Right */ {"Move right", "Joystick port 2: right."},
    /* P2Fire */ {"Fire", "Joystick port 2: fire. In two-player games the players take turns on this joystick."},
    /* P1Up */ {"Move up", "Joystick port 1, the second joystick: up."},
    /* P1Down */ {"Move down", "Joystick port 1: down."},
    /* P1Left */ {"Move left", "Joystick port 1: left."},
    /* P1Right */ {"Move right", "Joystick port 1: right."},
    /* P1Fire */ {"Fire", "Joystick port 1: fire."},
    /* HostPause */ {"Pause", "Pauses and resumes the game. P pauses the game itself too and is not bound here."},
    /* HostReset */ {"Reset the game", "Starts the game again from the title, as a reset button would."},
    /* HostQuit */ {"Quit", "Closes x2recomp. Ctrl+Q and Cmd+Q always work."},
    /* HostFullscreen */ {"Fullscreen", "Switches between a window and the whole display. Alt+Return always works."},
    /* HostVolumeDown */ {"Volume down", "Ten per cent quieter."},
    /* HostVolumeUp */ {"Volume up", "Ten per cent louder."},
    /* HostScaleDown */ {"Smaller window", "One step smaller (the window is a multiple of the original picture)."},
    /* HostScaleUp */ {"Larger window", "One step larger."},
    /* HostFaithful */ {"Faithful switch", "Original picture and sound only. Also the first row of the menu."},
    /* HostFilter */ {"Next filter", "Cycles nearest, sharp, scanlines, crt and edge."},
    /* HostInterp */ {"Smooth frames", "Interpolated frames at the display rate."},
    /* HostAspect */ {"4:3 picture", "The original 4:3 shape, or square pixels."},
    /* HostUpgrade */ {"Asset packs", "Uses or stops using the replacement graphics and sound of the loaded packs."},
    /* HostWide */ {"Next surround", "Steps through the widescreen surrounds."},
    /* HostPanel */ {"HD status panel", "A sharper status panel."},
    /* HostMusic */ {"Music source", "Cycles tracker music, CD tracks and both (the CDTV release: the CD on or off)."},
    /* HostGrab */ {"Keyboard grab", "Sends the system's own shortcuts to the window."},
    /* HostAutofire */ {"Autofire", "Pulses the fire line while fire is held, during a level only. An assist, not a cheat."},
    /* HostGodMode */ {"God mode", "CHEAT: the ship's energy never runs out. No high scores are saved."},
    /* HostAddCredit */ {"Add a credit", "CHEAT: one more credit during a game. No high scores are saved."},
    /* HostProtectMusic */ {"Protect the tune", "Not what the original does: keeps the tune's voices through the sound effects."},
    /* HostButton2 */ {"Second button", "The CDTV release's special weapon (the pad's second button). The disk and ST releases ignore it."},
    /* HostMenu */ {"Open the menu", "Opens and closes this menu. The gamepad's menu button does the same."},
};

struct Group {
  const char* title;
  std::vector<int> actions;
};

const std::vector<Group>& groups() {
  static const std::vector<Group> g = {
      {"Menu", {Action::HostMenu}},
      {"Joystick port 2 (the player)", {Action::P2Up, Action::P2Down, Action::P2Left, Action::P2Right, Action::P2Fire, Action::HostButton2}},
      {"Joystick port 1 (second player)", {Action::P1Up, Action::P1Down, Action::P1Left, Action::P1Right, Action::P1Fire}},
      {"Window and picture",
       {Action::HostFullscreen, Action::HostScaleDown, Action::HostScaleUp, Action::HostFaithful, Action::HostFilter, Action::HostInterp,
        Action::HostAspect, Action::HostUpgrade, Action::HostWide, Action::HostPanel}},
      {"Sound", {Action::HostVolumeDown, Action::HostVolumeUp, Action::HostMusic, Action::HostProtectMusic}},
      {"Session", {Action::HostPause, Action::HostReset, Action::HostQuit, Action::HostGrab}},
      {"Assists and cheats", {Action::HostAutofire, Action::HostGodMode, Action::HostAddCredit}},
  };
  return g;
}

constexpr size_t kMaxKeys = 6;

std::string keys_text(const std::vector<SDL_Scancode>& keys) {
  if (keys.empty()) return "(none)";
  std::string s;
  for (size_t i = 0; i < keys.size(); ++i) s += (i ? ", " : "") + input::key_label(keys[i]);
  return s;
}

std::vector<SDL_GamepadButton>& pad_list(input::Bindings& b, int which) {
  return which == 0 ? b.pad.fire : which == 1 ? b.pad.button2 : b.pad.menu;
}

const char* pad_list_name(int which) { return which == 0 ? "Fire" : which == 1 ? "Second button" : "Open the menu"; }

std::string pad_text(const std::vector<SDL_GamepadButton>& v, PadStyle style) {
  if (v.empty()) return "(none)";
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + pad_caption(v[i], style);
  return s;
}

// The menu must stay reachable: at least one key or one button opens it (the file can always be edited by hand, but nobody should have to).
bool menu_would_be_unreachable(const input::Bindings& b) { return b.keys[Action::HostMenu].empty() && b.pad.menu.empty(); }

void erase_key(std::vector<SDL_Scancode>& v, SDL_Scancode sc) { v.erase(std::remove(v.begin(), v.end(), sc), v.end()); }
void erase_button(std::vector<SDL_GamepadButton>& v, SDL_GamepadButton b) { v.erase(std::remove(v.begin(), v.end(), b), v.end()); }

void apply_key(ControlsModel& cm, int action, SDL_Scancode sc, bool add, int other) {
  input::Bindings next = cm.b;
  if (other >= 0) erase_key(next.keys[static_cast<size_t>(other)], sc);
  auto& list = next.keys[static_cast<size_t>(action)];
  if (!add) list.clear();
  if (std::find(list.begin(), list.end(), sc) == list.end() && list.size() < kMaxKeys) list.push_back(sc);
  if (menu_would_be_unreachable(next)) {
    cm.message = "The menu needs at least one key or gamepad button.";
    return;
  }
  cm.b = next;
  cm.changed = true;
  cm.message = input::key_label(sc) + " now does " + kInfo[action].label + (other >= 0 ? std::string(" (taken from ") + kInfo[other].label + ")" : "") + ".";
}

void apply_button(ControlsModel& cm, int which, SDL_GamepadButton btn, bool add, int other) {
  input::Bindings next = cm.b;
  if (other >= 0) erase_button(pad_list(next, other), btn);
  auto& list = pad_list(next, which);
  if (!add) list.clear();
  if (std::find(list.begin(), list.end(), btn) == list.end()) list.push_back(btn);
  if (menu_would_be_unreachable(next)) {
    cm.message = "The menu needs at least one key or gamepad button.";
    return;
  }
  cm.b = next;
  cm.changed = true;
  cm.message = std::string("Gamepad button ") + input::pad_button_name(btn) + " now does: " + pad_list_name(which) + ".";
}

void request_key(Context& ctx, ControlsModel& cm, int action, SDL_Scancode sc, bool add) {
  if (sc == SDL_SCANCODE_UNKNOWN) return;
  const int other = input::find_binding_conflict(cm.b, action, sc);
  const bool game = input::is_game_key(sc);
  if (other < 0 && !game) {
    apply_key(cm, action, sc, add, -1);
    return;
  }
  cm.pending = {};
  cm.pending.kind = ControlsModel::Pending::Key;
  cm.pending.action = action;
  cm.pending.key = sc;
  cm.pending.add = add;
  cm.pending.other = other;
  const std::string k = input::key_label(sc);
  std::string text;
  if (other >= 0) text = k + " already does \"" + kInfo[other].label + "\". It will do \"" + kInfo[action].label + "\" instead, and \"" + kInfo[other].label + "\" loses it.";
  if (game) text += std::string(text.empty() ? "" : " ") + "The game itself reads " + k + ", so binding it takes it away from the game.";
  cm.pending.text = text;
  ctx.open_modal("controls_confirm");
}

void request_button(Context& ctx, ControlsModel& cm, int which, SDL_GamepadButton btn, bool add) {
  int other = -1;
  for (int w = 0; w < 3; ++w) {
    if (w == which) continue;
    const auto& l = pad_list(cm.b, w);
    if (std::find(l.begin(), l.end(), btn) != l.end()) other = w;
  }
  if (other < 0) {
    apply_button(cm, which, btn, add, -1);
    return;
  }
  cm.pending = {};
  cm.pending.kind = ControlsModel::Pending::Pad;
  cm.pending.action = which;
  cm.pending.button = btn;
  cm.pending.add = add;
  cm.pending.other = other;
  cm.pending.text = std::string("That button already does \"") + pad_list_name(other) + "\". It will do \"" + pad_list_name(which) + "\" instead.";
  ctx.open_modal("controls_confirm");
}

void heading(Context& ctx, Rect& body, std::string_view title, std::string_view sub) {
  const bool small = ctx.screen().h < 320;
  const Rect h = cut_top(body, small ? 20 : 32);
  const int cw = small ? 9 : 17;
  if (small) draw_text(ctx.canvas(), Font::body(), h.x, h.y + 2, title, ctx.theme.c.text, 1, 1);
  else draw_heading(ctx, h, title, ctx.theme.c.text);
  if (!sub.empty()) {
    const int tw = static_cast<int>(title.size()) * cw + 12;
    draw_caption(ctx, {h.x + tw, h.y + (small ? 6 : 12), h.w - tw, 10}, sub, ctx.theme.c.dim);
  }
  cut_top(body, small ? 2 : 6);
}

constexpr int kControlW = 192;
constexpr int kAddW = 22;

// One binding row: the keys (Accept replaces them, Backspace while capturing clears them, Del restores the default) and a "+" that adds one.
struct KeyRowResult {
  bool captured = false, cleared = false, add = false, restore = false;
  SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
  SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
  bool is_pad_key = false;
};

KeyRowResult binding_row(Context& ctx, Stack& st, std::string_view id, std::string_view label, std::string_view help, std::string_view badge,
                         BadgeKind badge_kind, std::string_view text, std::string_view prompt) {
  RowSpec spec;
  spec.label = label;
  spec.help = help;
  spec.badge = badge;
  spec.badge_kind = badge_kind;
  const Rect c = setting_row(ctx, st, id, spec, kControlW);
  const Rect keys{c.x, c.y, c.w - kAddW - 2, c.h};
  const Rect add{c.right() - kAddW, c.y, kAddW, c.h};
  KeyRowResult out;
  int key = 0;
  const CaptureResult r = key_capture(ctx, id, keys, text, key, prompt);
  if (r == CaptureResult::Captured) {
    const Event& e = ctx.event();
    out.captured = true;
    if (e.key >= kPadKeyBase) {
      out.is_pad_key = true;
      out.button = static_cast<SDL_GamepadButton>(e.key - kPadKeyBase);
    } else {
      out.key = static_cast<SDL_Scancode>(e.scancode);
    }
  } else if (r == CaptureResult::Cleared) {
    out.cleared = true;
  }
  int add_key = 0;
  const std::string add_id = std::string(id) + "/add";
  const CaptureResult ar = key_capture(ctx, add_id, add, "+", add_key, prompt.size() > 12 ? "..." : prompt);
  if (ar == CaptureResult::Captured) {
    const Event& e = ctx.event();
    out.captured = out.add = true;
    if (e.key >= kPadKeyBase) {
      out.is_pad_key = true;
      out.button = static_cast<SDL_GamepadButton>(e.key - kPadKeyBase);
    } else {
      out.key = static_cast<SDL_Scancode>(e.scancode);
    }
  }
  if (ctx.is_focused(ctx.id(id)) && ctx.take_nav(Nav::Default)) out.restore = true;
  return out;
}

}  // namespace

const char* action_label(int action) { return action >= 0 && action < input::kActionCount ? kInfo[action].label : "?"; }
const char* action_help(int action) { return action >= 0 && action < input::kActionCount ? kInfo[action].help : ""; }

std::string pad_caption(SDL_GamepadButton b, PadStyle style) {
  const bool ps = style == PadStyle::PlayStation, nin = style == PadStyle::Nintendo;
  switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ps ? "Cross" : (nin ? "B" : "A");
    case SDL_GAMEPAD_BUTTON_EAST: return ps ? "Circle" : (nin ? "A" : "B");
    case SDL_GAMEPAD_BUTTON_WEST: return ps ? "Square" : (nin ? "Y" : "X");
    case SDL_GAMEPAD_BUTTON_NORTH: return ps ? "Triangle" : (nin ? "X" : "Y");
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : (nin ? "L" : "LB");
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : (nin ? "R" : "RB");
    case SDL_GAMEPAD_BUTTON_START: return ps ? "Options" : (nin ? "+" : "Start");
    case SDL_GAMEPAD_BUTTON_BACK: return ps ? "Share" : (nin ? "-" : "View");
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS";
    default: return input::pad_button_name(b);
  }
}

bool page_controls(Context& ctx, ControlsModel& cm, SettingsModel& sm, Rect body, Rect help, bool with_heading) {
  const Theme& t = ctx.theme;
  if (ctx.event().type == Event::Type::NavKey && (ctx.event().nav == Nav::Up || ctx.event().nav == Nav::Down)) cm.message.clear();
  if (!ctx.modal_open() && cm.pending.kind != ControlsModel::Pending::None) cm.pending = {};
  if (with_heading) heading(ctx, body, "Controls", "Press a key or button to bind it");
  ScrollView sv = ctx.scroll_begin("controls", body);
  Stack st(body.x, sv.origin_y() + 4, body.w - 6, 0);

  // ---- gamepad ----
  st.space(4);
  section_header(ctx, st.row(12), "Gamepad");
  st.space(2);
  const PadStyle style = ctx.pad_style;
  static const char* const pad_help[3] = {
      "The buttons that fire. A button pressed here is a fire button on both ports' pads.",
      "The pad's second button: the CDTV release's special weapon. The disk and ST releases ignore it.",
      "Opens and closes the menu."};
  static const char* const pad_ids[3] = {"pad_fire", "pad_button2", "pad_menu"};
  for (int w = 0; w < 3; ++w) {
    const auto& list = pad_list(cm.b, w);
    const bool unbound = list.empty();
    const KeyRowResult r = binding_row(ctx, st, pad_ids[w], pad_list_name(w), pad_help[w], unbound ? "unbound" : "", BadgeKind::Dim,
                                       pad_text(list, style), "Press a button...");
    if (r.captured) {
      if (r.is_pad_key) request_button(ctx, cm, w, r.button, r.add);
      else cm.message = "That was a keyboard key. Press a button on the gamepad.";
    } else if (r.cleared) {
      input::Bindings next = cm.b;
      pad_list(next, w).clear();
      if (menu_would_be_unreachable(next)) cm.message = "The menu needs at least one key or gamepad button.";
      else { cm.b = next; cm.changed = true; cm.message = std::string(pad_list_name(w)) + " has no gamepad button now."; }
    } else if (r.restore) {
      input::Bindings def = input::default_bindings();
      pad_list(cm.b, w) = pad_list(def, w);
      cm.changed = true;
      cm.message = std::string(pad_list_name(w)) + " restored to the default.";
    }
  }
  {
    const Rect note = st.row(24);
    draw_paragraph(ctx, {note.x + 12, note.y + 2, note.w - 16, 22},
                   "The d-pad and left stick always steer. Button names follow the pad you connect (A, B, X, Y until then).", t.c.dim, 10);
  }

  // ---- the schema's rows for this page (autofire, keyboard grab, cursor) ----
  bool changed = settings_rows(ctx, sm, kPageControls, st);

  // ---- keyboard ----
  st.space(4);
  section_header(ctx, st.row(12), "Key bindings (Enter changes one, + adds a key, Del restores)");
  for (const Group& g : groups()) {
    st.space(4);
    section_header(ctx, st.row(12), g.title);
    st.space(2);
    for (int a : g.actions) {
      const auto& keys = cm.b.keys[static_cast<size_t>(a)];
      const bool default_keys = keys == input::default_bindings().keys[static_cast<size_t>(a)];
      std::string_view badge;
      BadgeKind bk = BadgeKind::Dim;
      if (keys.empty()) badge = "unbound";
      else if (!default_keys) { badge = "custom"; bk = BadgeKind::Info; }
      if (a == Action::HostButton2 && keys.empty()) badge = "unbound";
      const std::string id = std::string("key_") + input::action_name(a);
      const KeyRowResult r = binding_row(ctx, st, id, kInfo[a].label, kInfo[a].help, badge, bk, keys_text(keys), "Press a key...");
      if (r.captured) {
        if (r.is_pad_key) cm.message = "That was a gamepad button. Press a key on the keyboard.";
        else request_key(ctx, cm, a, r.key, r.add);
      } else if (r.cleared) {
        input::Bindings next = cm.b;
        next.keys[static_cast<size_t>(a)].clear();
        if (menu_would_be_unreachable(next)) cm.message = "The menu needs at least one key or gamepad button.";
        else { cm.b = next; cm.changed = true; cm.message = std::string(kInfo[a].label) + " has no key now."; }
      } else if (r.restore) {
        input::Bindings next = cm.b;
        const auto def = input::default_bindings().keys[static_cast<size_t>(a)];
        // A default key another action holds is taken from it only if that action is also being restored; here it simply stays with its owner.
        std::vector<SDL_Scancode> keep;
        for (SDL_Scancode sc : def)
          if (input::find_binding_conflict(next, a, sc) < 0) keep.push_back(sc);
        next.keys[static_cast<size_t>(a)] = keep;
        cm.b = next;
        cm.changed = true;
        cm.message = std::string(kInfo[a].label) + (keep.size() == def.size() ? " restored to the default." : " restored (a default key is used by another action and was left there).");
      }
    }
  }
  st.space(8);
  {
    const Rect r = st.row(20);
    const int ctl_x = r.x + 12 + std::min(148, r.w * 36 / 100);
    if (button(ctx, "Restore all defaults", {ctl_x, r.y, kControlW, 20})) {
      cm.b = input::default_bindings();
      cm.changed = true;
      cm.message = "Every key and gamepad button restored to the defaults.";
    }
  }
  ctx.scroll_end(sv, st.used() + 8);
  const std::string_view line = cm.message.empty() ? ctx.help() : std::string_view(cm.message);
  draw_caption(ctx, help, line, t.c.dim);
  return changed;
}

void controls_modal(Context& ctx, ControlsModel& cm) {
  if (cm.pending.kind == ControlsModel::Pending::None) return;
  Rect inner;
  const char* title = cm.pending.kind == ControlsModel::Pending::Key ? "Key in use" : "Button in use";
  if (!ctx.modal_begin("controls_confirm", title, 380, 128, &inner)) {
    if (!ctx.modal_open()) cm.pending = {};
    return;
  }
  const Rect text = cut_top(inner, 56);
  draw_paragraph(ctx, text, cm.pending.text, ctx.theme.c.text, 11);
  const Rect row = cut_bottom(inner, 22);
  Rect buttons = row;
  const Rect move = cut_left(buttons, 128);
  cut_left(buttons, 8);
  const Rect cancel = cut_left(buttons, 96);
  ButtonOpts primary;
  primary.primary = true;
  primary.default_focus = true;
  if (button(ctx, "Move it here", move, primary)) {
    if (cm.pending.kind == ControlsModel::Pending::Key) apply_key(cm, cm.pending.action, cm.pending.key, cm.pending.add, cm.pending.other);
    else apply_button(cm, cm.pending.action, cm.pending.button, cm.pending.add, cm.pending.other);
    cm.pending = {};
    ctx.close_modal();
  } else if (button(ctx, "Cancel", cancel)) {
    cm.pending = {};
    ctx.close_modal();
  }
  ctx.modal_end();
}

}  // namespace x2::ui
