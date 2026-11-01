// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "overlay.h"

#include <algorithm>
#include <cstring>

namespace x2::ui {

namespace {

// The picture is dimmed, not hidden: the pixels outside the panel are black at about half strength, so a change of filter or size can
// be judged on the running game behind the menu.
constexpr Color kScrim = 0x78000000u;

struct ScreenInfo {
  Overlay::Screen screen;
  const char* title;
};
constexpr ScreenInfo kScreens[] = {
    {Overlay::Screen::Display, "Display"},   {Overlay::Screen::Audio, "Audio"},
    {Overlay::Screen::Controls, "Controls"}, {Overlay::Screen::Packs, "Asset packs"},
    {Overlay::Screen::Assists, "Assists and cheats"},
};

const char* title_of(Overlay::Screen s) {
  if (s == Overlay::Screen::Top) return "Paused";
  for (const ScreenInfo& i : kScreens)
    if (i.screen == s) return i.title;
  return "";
}

bool in_assists(const SettingRow& r) {
  return std::strcmp(r.group, "Scores") == 0 || std::strcmp(r.group, "Assists") == 0 || std::strcmp(r.group, "Cheats") == 0;
}

}  // namespace

Overlay::Overlay(GameKind game, OverlayHooks hooks) : game_(game), hooks_(std::move(hooks)) {}

void Overlay::open(int out_w, int out_h, const input::Bindings& live) {
  ctx_ = Context();
  input_ = SdlInput();
  sm_ = SettingsModel();
  sm_.game = game_;
  sm_.s = hooks_.current ? hooks_.current() : Settings();
  sm_.saved = sm_.s;
  applied_ = sm_.s;
  sm_.test_tone = hooks_.test_tone;
  sm_.hotkey = [this](const std::string& action) -> std::string {
    for (int a = 0; a < input::kActionCount; ++a)
      if (action == input::action_name(a)) {
        const auto& keys = cm_.in_force.keys[static_cast<size_t>(a)];
        return keys.empty() ? std::string() : input::key_label(keys.front());
      }
    return {};
  };
  cm_.open(live);
  screen_ = Screen::Top;
  status_.clear();
  active_ = true;
  out_w_ = out_w;
  out_h_ = out_h;
  if (hooks_.set_open) hooks_.set_open(true);
  run({}, 0);
}

void Overlay::close() {
  if (!active_) return;
  active_ = false;
  if (hooks_.set_open) hooks_.set_open(false);
}

void Overlay::show(Screen s) {
  screen_ = s;
  run({}, 0);
}

void Overlay::layout(int out_w, int out_h) {
  out_w_ = out_w;
  out_h_ = out_h;
  ctx_.theme = sm_.s.ui_contrast == "high" ? Theme::high_contrast_theme() : Theme::standard();
  ctx_.motion.reduced = sm_.s.ui_motion == "reduced";
  ctx_.large_text = sm_.s.ui_text == "large";
  ctx_.pad_style = input_.pad_style();
  geo_ = compute_geometry(out_w, out_h, ctx_.large_text);
  if (canvas_.width() != geo_.cw || canvas_.height() != geo_.ch) canvas_.resize(geo_.cw, geo_.ch);
  input_.set_geometry(geo_);
}

void Overlay::run(const EventList& events, int dt_ms) {
  layout(out_w_, out_h_);
  animating_ = ctx_.run(canvas_, events, dt_ms, [this](Context& c) { build(c); });
  after_pass();
}

void Overlay::after_pass() {
  if (!active_) return;
  if (format_settings(sm_.s) != format_settings(applied_)) {
    const Settings before = applied_;
    applied_ = sm_.s;
    if (hooks_.apply) {
      const LiveResult r = hooks_.apply(before, sm_.s);
      const std::string note = r.next_start_note();
      status_ = note;
      if (screen_ == Screen::Controls) cm_.message = note;
    }
  }
  if (cm_.changed) {
    cm_.changed = false;
    std::string why;
    if (hooks_.save_bindings && !hooks_.save_bindings(cm_.b, why)) {
      cm_.message = "Not saved: " + why;
    }
    cm_.in_force = cm_.b;
  }
}

void Overlay::handle(const SDL_Event& e0) {
  if (!active_) return;
  SDL_Event e = e0;
  if (renderer_ && (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                    e.type == SDL_EVENT_MOUSE_WHEEL))
    SDL_ConvertEventToRenderCoordinates(renderer_, &e);
  if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !ctx_.capturing()) {
    const SDL_Scancode sc = e.key.scancode;
    const auto bound = [&](int action) {
      const auto& k = cm_.in_force.keys[static_cast<size_t>(action)];
      return std::find(k.begin(), k.end(), sc) != k.end();
    };
    if (sc == SDL_SCANCODE_Q && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
      if (hooks_.quit) hooks_.quit();
      return;
    }
    if (bound(input::HostQuit)) {
      if (hooks_.quit) hooks_.quit();
      return;
    }
    if (bound(input::HostMenu)) {
      close();
      return;
    }
#ifdef __APPLE__
    if ((e.key.mod & SDL_KMOD_GUI) && input::host_command_for_gui(sc) == input::HostCommand::Menu) {  // Cmd+1, as F1
      close();
      return;
    }
#endif
    // A combination with Ctrl, Alt or Cmd belongs to the system or to a hotkey of the game, not to the menu (Alt+Return must not press Enter).
    if (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) return;
  }
  if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && !ctx_.capturing()) {
    const auto& m = cm_.in_force.pad.menu;
    if (std::find(m.begin(), m.end(), static_cast<SDL_GamepadButton>(e.gbutton.button)) != m.end()) {
      close();
      return;
    }
  }
  if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || e.type == SDL_EVENT_WINDOW_RESIZED) {
    int w = 0, h = 0;
    if (renderer_ && SDL_GetRenderOutputSize(renderer_, &w, &h)) { out_w_ = w; out_h_ = h; }
    run({}, 0);
    return;
  }
  if (e.type == SDL_EVENT_KEY_DOWN && e.key.key != SDLK_UNKNOWN) status_.clear();
  EventList evs;
  input_.set_capture(ctx_.capturing());
  input_.handle(e, evs);
  if (evs.empty()) return;
  run(evs, 0);
}

void Overlay::tick(int dt_ms) {
  if (!active_) return;
  EventList evs;
  input_.tick(dt_ms, evs);
  if (evs.empty() && !animating_) return;
  run(evs, dt_ms);
}

void Overlay::draw(SDL_Renderer* r, int out_w, int out_h) {
  if (!active_) return;
  renderer_ = r;
  if (out_w != out_w_ || out_h != out_h_) {
    out_w_ = out_w;
    out_h_ = out_h;
    run({}, 0);
  }
  presenter_.draw_over(r, canvas_, geo_);
}

const Canvas& Overlay::render(int out_w, int out_h) {
  if (out_w != out_w_ || out_h != out_h_) {
    out_w_ = out_w;
    out_h_ = out_h;
    run({}, 0);
  }
  return canvas_;
}

bool Overlay::confirm_modal(Context& ctx, const char* id, const char* title, const char* text, const char* yes) {
  Rect inner;
  if (!ctx.modal_begin(id, title, 340, 112, &inner)) return false;
  draw_paragraph(ctx, cut_top(inner, 40), text, ctx.theme.c.text, 11);
  Rect buttons = cut_bottom(inner, 22);
  const Rect ok = cut_left(buttons, 120);
  cut_left(buttons, 8);
  const Rect cancel = cut_left(buttons, 96);
  bool confirmed = false;
  ButtonOpts o;
  o.primary = true;
  ctx.push_id(id);  // the buttons must not share an identity with the rows behind the modal ("Quit" is both)
  struct Pop {
    Context& c;
    ~Pop() { c.pop_id(); }
  } pop{ctx};
  if (button(ctx, yes, ok, o)) {
    confirmed = true;
    ctx.close_modal();
  } else if (button(ctx, "Cancel", cancel, [] { ButtonOpts c; c.default_focus = true; return c; }())) {
    ctx.close_modal();
  }
  ctx.modal_end();
  return confirmed;
}

void Overlay::top_screen(Context& ctx, Rect area, Rect help) {
  const Theme& t = ctx.theme;
  // A scrolling list: on the smallest canvas the rows do not all fit, and the focus brings the others into view.
  ScrollView sv = ctx.scroll_begin("top", area);
  Stack st(area.x, sv.origin_y() + 2, area.w - 6, 4);
  const int h = 22;
  struct Item {
    const char* label;
    Screen target;
    const char* help;
  };
  static const Item items[] = {
      {"Display", Screen::Display, "Window size, filter, smooth frames, widescreen and the look of this menu."},
      {"Audio", Screen::Audio, "Volume, the output filter and the music."},
      {"Controls", Screen::Controls, "Keyboard and gamepad buttons."},
      {"Asset packs", Screen::Packs, "Replacement graphics and sound."},
      {"Assists and cheats", Screen::Assists, "Autofire, high scores, god mode and credits."},
  };
  ButtonOpts resume;
  resume.primary = true;
  resume.default_focus = true;
  if (button(ctx, "Resume", st.row(h), resume)) close();
  if (ctx.is_focused(ctx.id("Resume"))) ctx.set_help("Back to the game, exactly where it stopped.");

  // The Faithful switch: the one row of the menu that is a setting rather than a page.
  {
    RowSpec spec;
    spec.label = "Faithful";
    spec.help = "The original look and sound only. Turns off every picture option and every asset pack. The same as the Faithful switch.";
    const std::string hint = sm_.hotkey ? sm_.hotkey("faithful") : std::string();
    spec.hint = hint;
    st.space(2);
    const Rect c = setting_row(ctx, st, "faithful_top", spec, 120);
    const SettingRow* row = find_row("modern");
    bool v = row ? get_bool(*row, sm_.s) : false;
    if (row && toggle(ctx, "faithful_top", c, v)) set_bool(*row, sm_.s, v);
    st.space(2);
  }
  for (const Item& it : items) {
    if (button(ctx, it.label, st.row(h))) {
      screen_ = it.target;
      ctx.set_focus(0);
    }
    if (ctx.is_focused(ctx.id(it.label))) ctx.set_help(it.help);
  }
  st.space(6);
  if (button(ctx, "Reset the game", st.row(h))) ctx.open_modal("confirm_reset");
  if (ctx.is_focused(ctx.id("Reset the game"))) ctx.set_help("Starts the game again from the title, as the reset hotkey does.");
  if (button(ctx, "Quit", st.row(h))) ctx.open_modal("confirm_quit");
  if (ctx.is_focused(ctx.id("Quit"))) ctx.set_help("Closes x2recomp.");
  ctx.scroll_end(sv, st.used() + 4);
  draw_caption(ctx, help, status_.empty() ? ctx.help() : std::string_view(status_), status_.empty() ? t.c.dim : t.c.warn);
}

void Overlay::settings_screen(Context& ctx, Rect area, Rect help, const char* title, PageBit page, bool tone, bool expert) {
  const Theme& t = ctx.theme;
  ScrollView sv = ctx.scroll_begin(title, area);
  Stack st(area.x, sv.origin_y() + 2, area.w - 6, 0);
  std::function<bool(const SettingRow&)> keep;
  if (screen_ == Screen::Assists) keep = in_assists;
  settings_rows(ctx, sm_, page, st, keep);
  if (tone && sm_.test_tone) {
    st.space(6);
    const Rect b = st.row(20);
    const int ctl_x = b.x + 12 + std::min(148, b.w * 36 / 100);
    if (button(ctx, "Play test tone", {ctl_x, b.y, 120, 20})) sm_.test_tone();
  }
  if (expert) {
    bool any = false;
    for (const SettingRow* r : rows_for_page(page)) any |= r->expert && applies_to(*r, game_);
    if (any) {
      st.space(4);
      RowSpec spec;
      spec.label = "Expert options";
      spec.help = "Shows the settings that most people never need.";
      const Rect c = setting_row(ctx, st, "expert", spec, 120);
      toggle(ctx, "expert", c, sm_.show_expert);
    }
  }
  if (page == kPagePacks) {
    st.space(6);
    const Rect note = st.row(34);
    draw_paragraph(ctx, {note.x + 12, note.y, note.w - 16, 34},
                   "The pack folders are listed in settings.cfg (asset_packs, packs_enabled). The switch above applies at once; a change of folders takes effect at the next start.",
                   t.c.dim, 10);
  }
  ctx.scroll_end(sv, st.used() + 8);
  draw_caption(ctx, help, status_.empty() ? ctx.help() : std::string_view(status_), status_.empty() ? t.c.dim : t.c.warn);
}

void Overlay::build(Context& ctx) {
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  const Rect all = ctx.screen();
  c.fill(all, kScrim);
  const int pw = all.w >= 600 ? 400 : all.w;
  const Rect panel_r{all.w - pw, 0, pw, all.h};
  c.fill(panel_r, t.c.ground);
  c.vline(panel_r.x, 0, panel_r.h, t.c.line);
  Rect area = panel_r.inset(10, 6);

  const Rect head = cut_top(area, 20);
  draw_text(c, Font::body(), head.x, head.y + 2, screen_ == Screen::Top ? "PAUSED" : title_of(screen_), t.c.amber);
  {
    const std::string sub = std::string(game_name(game_));
    const int sw = text_width(sub);
    draw_text(c, Font::caption(), head.right() - sw, head.y + 8, sub, t.c.dim);
  }
  cut_top(area, 4);

  static const Hint hints[] = {{Nav::Up, "Move"}, {Nav::Accept, "Select"}, {Nav::Back, "Back"}, {Nav::Menu, "Resume"}};
  const Rect hint = cut_bottom(area, 12);
  cut_bottom(area, 4);
  const bool controls = screen_ == Screen::Controls;
  const Rect footer = cut_bottom(area, controls ? 26 : 32);
  cut_bottom(area, 4);
  const Rect help = cut_bottom(area, 10);
  cut_bottom(area, 4);

  switch (screen_) {
    case Screen::Top: top_screen(ctx, area, help); break;
    case Screen::Display: settings_screen(ctx, area, help, "display", kPageDisplay, false, false); break;
    case Screen::Audio: settings_screen(ctx, area, help, "audio", kPageAudio, true, true); break;
    case Screen::Packs: settings_screen(ctx, area, help, "packs", kPagePacks, false, false); break;
    case Screen::Assists: settings_screen(ctx, area, help, "assists", kPagePlay, false, false); break;
    case Screen::Controls: page_controls(ctx, cm_, sm_, area, help, false); break;
  }

  if (controls) {
    panel(ctx, footer, false);
    draw_caption(ctx, {footer.x + 8, footer.y + 4, footer.w - 16, 10}, "Saved in input.cfg when you change a binding.", t.c.dim);
    draw_caption(ctx, {footer.x + 8, footer.y + 14, footer.w - 16, 10}, "x2recomp --list-keys prints the bindings.", t.c.cyan);
  } else if (settings_footer(ctx, sm_, footer)) {
    SDL_SetClipboardText(sm_.copied.c_str());
    status_ = "Copied the command line.";
  }
  hint_bar(ctx, hint, hints);

  if (confirm_modal(ctx, "confirm_reset", "Reset the game?", "The game starts again from the title. Anything not saved is lost.", "Reset")) {
    close();
    if (hooks_.reset_game) hooks_.reset_game();
  }
  if (confirm_modal(ctx, "confirm_quit", "Quit?", "Closes x2recomp.", "Quit")) {
    if (hooks_.quit) hooks_.quit();
  }
  controls_modal(ctx, cm_);

  if (active_ && !ctx.modal_open()) {
    if (ctx.take_nav(Nav::Menu)) {
      close();
    } else if (screen_ != Screen::Top && (ctx.peek_nav(Nav::PageNext) || ctx.peek_nav(Nav::PagePrev))) {
      const int step = ctx.peek_nav(Nav::PageNext) ? 1 : -1;
      ctx.take_nav(step > 0 ? Nav::PageNext : Nav::PagePrev);
      int idx = 0;
      constexpr int n = static_cast<int>(sizeof kScreens / sizeof kScreens[0]);
      for (int i = 0; i < n; ++i)
        if (kScreens[i].screen == screen_) idx = i;
      screen_ = kScreens[(idx + step + n) % n].screen;
      ctx.set_focus(0);
    } else if (ctx.take_nav(Nav::Back)) {
      if (screen_ == Screen::Top) close();
      else {
        screen_ = Screen::Top;
        ctx.set_focus(0);
      }
    }
  }
}

}  // namespace x2::ui
