// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "pages.h"

namespace x2::ui {

bool SettingsModel::dirty() const { return format_settings(s) != format_settings(saved); }

const CommandLine& SettingsModel::command(bool with_game) {
  const std::string key = format_settings(s);
  if (!cache_valid_ || key != cache_key_ || with_game != cache_with_game_ || game != cache_game_) {
    cache_ = command_line(s, game, with_game);
    cache_key_ = key;
    cache_with_game_ = with_game;
    cache_game_ = game;
    cache_valid_ = true;
  }
  return cache_;
}

const std::vector<PageItem>& page_items() {
  static const std::vector<PageItem> p = {{"Play", Icon::Play},       {"Display", Icon::Display}, {"Audio", Icon::Speaker},
                                          {"Controls", Icon::Controller}, {"Packs", Icon::Puzzle},    {"Advanced", Icon::Gear},
                                          {"About", Icon::Info}};
  return p;
}

namespace {

std::string unit_text(int v, const SettingRow& r) { return std::to_string(v) + r.unit; }

void page_heading(Context& ctx, Rect& body, std::string_view title, std::string_view sub) {
  const bool small = ctx.screen().h < 320;  // short windows: a one-line heading
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

std::string sub_for(const SettingsModel& m) { return std::string("for: ") + game_name(m.game); }

// One row: the label column, the right control for the type, the badge and the hotkey. True when the value changed.
bool one_row(Context& ctx, SettingsModel& m, Stack& st, const SettingRow& r) {
  const std::string reason = disabled_reason(r, m.s, m.game);
  const bool enabled = reason.empty();
  RowSpec spec;
  spec.label = r.label;
  spec.help = r.help;
  spec.enabled = enabled;
  spec.reason = reason;
  std::string hint = m.hotkey && r.action[0] ? m.hotkey(r.action) : std::string();
  spec.hint = hint;
  std::string badge;
  spec.badge_kind = BadgeKind::Dim;
  if (!applies_to(r, m.game)) {
    badge = std::string("Not for ") + game_short_name(m.game);
  } else if (is_cheat_active(r, m.s)) {
    badge = "cheat";
    spec.badge_kind = BadgeKind::Warn;
  } else if (r.effect == Effect::Restart) {
    badge = "next start";
  } else if (r.games == kCdtv) {
    badge = "CDTV";
  } else if (r.games == kDisk) {
    badge = "disk";
  }
  spec.badge = badge;
  const bool slider_like = r.type == SettingType::Float || (r.type == SettingType::Int && (r.unit[0] == '%' || r.hi - r.lo > 12));
  const int cw = slider_like ? 160 : 120;
  const Rect c = setting_row(ctx, st, r.key, spec, cw);
  bool changed = false;
  switch (r.type) {
    case SettingType::Bool: {
      bool v = get_bool(r, m.s);
      if (toggle(ctx, r.key, c, v, enabled)) { set_bool(r, m.s, v); changed = true; }
      break;
    }
    case SettingType::Int: {
      int v = get_int(r, m.s);
      const bool ch = slider_like ? slider(ctx, r.key, c, v, r.lo, r.hi, r.step, unit_text(v, r), enabled)
                                  : stepper(ctx, r.key, c, v, r.lo, r.hi, r.step, unit_text(v, r), enabled);
      if (ch) { set_int(r, m.s, v); changed = true; }
      break;
    }
    case SettingType::Float: {
      int pct = static_cast<int>(std::lround(get_float(r, m.s) * 100.0f));
      const int step = std::max(1, static_cast<int>(std::lround(r.fstep * 100.0f)));
      if (slider(ctx, r.key, c, pct, static_cast<int>(std::lround(r.flo * 100.0f)), static_cast<int>(std::lround(r.fhi * 100.0f)), step,
                 std::to_string(pct) + "%", enabled)) {
        set_float(r, m.s, static_cast<float>(pct) / 100.0f);
        changed = true;
      }
      break;
    }
    case SettingType::Choice: {
      std::vector<std::string_view> items;
      for (int i = 0; i < r.nchoices; ++i) items.push_back(r.choices[i].label);
      std::string custom;  // a value settings.cfg holds that is none of the choices (a surround in pixels)
      int idx = get_choice(r, m.s);
      if (idx < 0) {
        custom = value_text(r, m.s) + " px";
        items.push_back(custom);
        idx = r.nchoices;
      }
      if (choice(ctx, r.key, c, idx, items, enabled) && idx < r.nchoices) { set_choice(r, m.s, idx); changed = true; }
      break;
    }
    case SettingType::Text:
      draw_label(ctx, c, value_text(r, m.s).empty() ? std::string("(none)") : value_text(r, m.s), ctx.theme.c.dim);
      break;
  }
  return changed;
}

}  // namespace

bool settings_rows(Context& ctx, SettingsModel& m, PageBit page, Stack& st, const std::function<bool(const SettingRow&)>& keep) {
  bool changed = false;
  const char* group = nullptr;
  for (const SettingRow* r : rows_for_page(page)) {
    if (r->expert && !m.show_expert) continue;
    if (keep && !keep(*r)) continue;
    if (!group || std::strcmp(group, r->group) != 0) {
      group = r->group;
      if (group[0]) {
        st.space(4);
        section_header(ctx, st.row(12), group);
        st.space(2);
      }
    }
    changed |= one_row(ctx, m, st, *r);
  }
  return changed;
}

namespace {

bool has_expert_rows(PageBit page) {
  for (const SettingRow* r : rows_for_page(page))
    if (r->expert) return true;
  return false;
}

// The "Expert" disclosure row at the foot of a page that has expert rows.
void expert_row(Context& ctx, SettingsModel& m, Stack& st, PageBit page) {
  if (!has_expert_rows(page)) return;
  st.space(4);
  RowSpec spec;
  spec.label = "Expert options";
  spec.help = "Shows the settings that most people never need.";
  const Rect c = setting_row(ctx, st, "expert", spec, 120);
  toggle(ctx, "expert", c, m.show_expert);
}

}  // namespace

bool page_play(Context& ctx, SettingsModel& m, Rect body, Rect help) {
  page_heading(ctx, body, "Play", "Start, scores, assists and cheats");
  ScrollView sv = ctx.scroll_begin("play", body);
  Stack st(body.x, sv.origin_y() + 4, body.w - 6, 0);
  const bool changed = settings_rows(ctx, m, kPagePlay, st);
  ctx.scroll_end(sv, st.used() + 8);
  draw_caption(ctx, help, ctx.help(), ctx.theme.c.dim);
  return changed;
}

bool page_audio(Context& ctx, SettingsModel& m, Rect body, Rect help) {
  page_heading(ctx, body, "Audio", sub_for(m));
  ScrollView sv = ctx.scroll_begin("audio", body);
  Stack st(body.x, sv.origin_y() + 4, body.w - 6, 0);
  const bool changed = settings_rows(ctx, m, kPageAudio, st);
  if (m.test_tone) {
    st.space(6);
    const Rect b = st.row(20);
    // In the control column, so Up and Down step through it with the rows (spatial navigation compares columns).
    const int ctl_x = b.x + 12 + std::min(148, b.w * 36 / 100);
    if (button(ctx, "Play test tone", {ctl_x, b.y, 120, 20})) m.test_tone();
  }
  expert_row(ctx, m, st, kPageAudio);
  ctx.scroll_end(sv, st.used() + 8);
  draw_caption(ctx, help, ctx.help(), ctx.theme.c.dim);
  return changed;
}

bool page_display(Context& ctx, SettingsModel& m, Rect body, Rect help) {
  const Theme& t = ctx.theme;
  page_heading(ctx, body, "Display", sub_for(m));
  ScrollView sv = ctx.scroll_begin("display", body);
  Stack st(body.x, sv.origin_y() + 4, body.w - 6, 0);
  const bool changed = settings_rows(ctx, m, kPageDisplay, st);
  st.space(10);
  // A generated test card shows the filter and the aspect without the game: the launcher has no running picture.
  const Rect area = st.row(136);
  const int ph = 128;
  const int pw = m.s.aspect ? 160 : 128 * 5 / 4;
  const Rect pv{area.x + 12, area.y + 4, pw, ph};
  ctx.canvas().frame(pv.inset(-2), t.c.line, 1);
  draw_test_card(ctx.canvas(), pv);
  if (m.s.video_filter == VideoFilter::Scanlines || m.s.video_filter == VideoFilter::Crt) apply_scanlines(ctx.canvas(), pv, 90);
  draw_paragraph(ctx, {pv.right() + 12, pv.y, area.right() - pv.right() - 12, 60},
                 "Test card, generated here. The game's own picture is never used by this page; in the in-game menu your changes show "
                 "on the running picture.",
                 t.c.dim);
  ctx.scroll_end(sv, st.used() + 8);
  draw_caption(ctx, help, ctx.help(), t.c.dim);
  return changed;
}

bool settings_footer(Context& ctx, SettingsModel& m, Rect r) {
  const std::string text = m.command(true).text();
  if (command_footer(ctx, r, text)) {
    m.copied = text;
    return true;
  }
  return false;
}

namespace {

void placeholder(Context& ctx, Rect body, std::string_view title, std::string_view text) {
  page_heading(ctx, body, title, "");
  draw_paragraph(ctx, cut_top(body, 44), text, ctx.theme.c.dim);
}

}  // namespace

bool settings_frame(Context& ctx, SettingsModel& m, int& page) {
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  if (page == kPlayPage && t.decor && !ctx.motion.reduced) {
    draw_starfield(c, ctx.screen(), ctx.now_ms(), t);
    ctx.animate();
  }
  Rect content = page_chrome(ctx, ctx.screen(), page_items(), page);
  static const Hint hints[] = {{Nav::Up, "Move"}, {Nav::Accept, "Select"}, {Nav::Back, "Back"}, {Nav::PageNext, "Next page"}, {Nav::Menu, "Menu"}};
  const Rect hint = cut_bottom(content, 12);
  cut_bottom(content, 4);
  const Rect footer = cut_bottom(content, 32);
  cut_bottom(content, 4);
  const Rect help = cut_bottom(content, 10);
  cut_bottom(content, 4);
  const Rect body = ctx.begin_page(hash_id(0, page_items()[static_cast<size_t>(page)].label), content);
  bool changed = false;
  switch (page) {
    case kPlayPage: changed = page_play(ctx, m, body, help); break;
    case kDisplayPage: changed = page_display(ctx, m, body, help); break;
    case kAudioPage: changed = page_audio(ctx, m, body, help); break;
    case kControlsPage: placeholder(ctx, body, "Controls", "Keyboard and gamepad bindings are built by a later part of the launcher."); break;
    case kPacksPage: placeholder(ctx, body, "Asset packs", "The pack list is built by a later part of the launcher."); break;
    case kAdvancedPage: placeholder(ctx, body, "Advanced", "Diagnostics and logs are built by a later part of the launcher."); break;
    default: placeholder(ctx, body, "About", "x2recomp: free software, GPL-3.0-or-later. It contains no game data."); break;
  }
  ctx.end_page(content);
  settings_footer(ctx, m, footer);
  hint_bar(ctx, hint, hints);
  return changed;
}

}  // namespace x2::ui
