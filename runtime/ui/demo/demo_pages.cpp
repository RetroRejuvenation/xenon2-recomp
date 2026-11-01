// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "demo_pages.h"

#include <algorithm>

namespace x2::ui::demo {

namespace {

const char* const kGameNames[] = {"Amiga disk", "Amiga CDTV", "Atari ST"};
constexpr std::string_view kFilters[] = {"nearest", "sharp", "scanlines", "crt", "edge"};
constexpr std::string_view kAspects[] = {"4:3", "square"};
constexpr std::string_view kSmooth[] = {"off", "60 fps", "120 fps"};
constexpr std::string_view kWide[] = {"off", "16:10", "16:9"};
constexpr std::string_view kAudioFilters[] = {"A500", "A1200", "none"};

std::string pct(int v) { return std::to_string(v) + "%"; }

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

// ---- Play ----
void page_play(Context& ctx, DemoState& st, Rect body) {
  const Theme& t = ctx.theme;
  Rect head = cut_top(body, 36);
  draw_heading(ctx, {head.x, head.y, head.w, 32}, "X2RECOMP", t.c.amber);
  const int tw = 8 * 17 + 14;
  draw_caption(ctx, {head.x + tw, head.y + 4, head.w - tw, 10}, "A native port. Bring your own copy.", t.c.dim);
  draw_caption(ctx, {head.x + tw, head.y + 17, head.w - tw, 10}, "v0.5.0 preview", t.c.dim);
  cut_top(body, 6);

  struct Mock {
    const char* title;
    const char* sub;
    const char* status;
    Icon icon;
    Color accent;
  };
  const Mock mocks[3] = {{"Amiga disk", "Retail release, two disks", "canonical", Icon::Floppy, t.c.amber},
                         {"Amiga CDTV", "CD-ROM release, CD music", "ready", Icon::Disc, t.c.cyan},
                         {"Atari ST", "Disk release, in development", "preview", Icon::Chip, t.c.ok}};
  const bool narrow = body.w < 420;
  const int gap = 8;
  const int cards_h = narrow ? 3 * 38 + 2 * gap : 140;
  Rect cards = cut_top(body, cards_h);
  for (int i = 0; i < 3; ++i) {
    Rect r = narrow ? Rect{cards.x, cards.y + i * (38 + gap), cards.w, 38}
                    : Rect{cards.x + i * ((cards.w - 2 * gap) / 3 + gap), cards.y, (cards.w - 2 * gap) / 3, cards.h};
    CardSpec c;
    c.title = mocks[i].title;
    c.subtitle = mocks[i].sub;
    c.status = mocks[i].status;
    c.action = i == static_cast<int>(st.game) ? "Play (last)" : "Play";
    c.emblem = mocks[i].icon;
    c.accent = mocks[i].accent;
    c.default_focus = i == 0;
    c.seed = static_cast<uint32_t>(i + 1);
    if (card(ctx, std::string("card") + std::to_string(i), r, c)) st.game = static_cast<Game>(i);
  }
  cut_top(body, 10);
  Rect link = cut_top(body, 20);
  if (button(ctx, "Import another game...", {link.x, link.y, 188, 20})) {
    st.log.push_back({Severity::Info, "Import requested (demo only)"});
  }
  cut_top(body, 4);
  Stack s(body.x, body.y, body.w, 0);
  const Rect r1 = s.row(14);
  draw_caption(ctx, r1, std::string("Display: ") + std::string(kFilters[st.display.filter]) + ", " +
                            std::string(kAspects[st.display.aspect]) + ", " + std::string(kSmooth[st.display.smooth]),
               t.c.dim);
  const Rect r2 = s.row(14);
  draw_caption(ctx, r2, std::string("Audio: asset pack ") + (st.pack_audio ? "on" : "off") + "     Packs: " +
                            std::to_string(static_cast<int>(std::count(st.pack_on.begin(), st.pack_on.end(), true))) + " enabled",
               t.c.dim);
  const Rect r3 = s.row(20);
  toggle(ctx, "Start the last game directly", {r3.x, r3.y, 56, 18}, st.start_directly);
  draw_label(ctx, {r3.x + 66, r3.y, r3.w - 66, r3.h}, "Start the last game directly next time", t.c.text);
}

// ---- Display (the proving page) ----
void page_display(Context& ctx, DemoState& st, Rect body, Rect help_rect) {
  DisplayModel& d = st.display;
  const Theme& t = ctx.theme;
  page_heading(ctx, body, "Display", std::string("for: ") + kGameNames[static_cast<int>(st.game)]);
  const bool is_st = st.game == Game::St;
  const char* st_reason = "The Atari ST release has no interpolated frames";
  ScrollView sv = ctx.scroll_begin("display", body);
  Stack s(body.x, sv.origin_y() + 4, body.w - 6, 0);
  auto row = [&](const char* label, const char* control, const char* help, const char* hint, bool enabled = true,
                 const char* reason = "", const char* badge = "") {
    RowSpec r;
    r.label = label;
    r.help = help;
    r.hint = hint;
    r.enabled = enabled;
    r.reason = reason;
    r.badge = badge;
    r.badge_kind = BadgeKind::Dim;
    return setting_row(ctx, s, control, r);
  };
  Rect c;
  c = row("Faithful switch", "faithful", "Original picture only. Overrides every other display option.", "", true, "", "");
  toggle(ctx, "faithful", c, d.faithful);
  c = row("Window size", "size", "Multiples of the Amiga picture.", "F5/F6");
  stepper(ctx, "size", c, d.size, 1, 8, 1, std::to_string(d.size) + "x");
  c = row("Fullscreen", "fullscreen", "Use the whole display.", "F11");
  toggle(ctx, "fullscreen", c, d.fullscreen);
  c = row("Filter", "filter", "nearest, sharp, scanlines, crt or edge.", "F2");
  choice(ctx, "filter", c, d.filter, kFilters);
  const bool crt = d.filter == 3;
  c = row("CRT curvature", "curvature", "Curves the picture like a tube.", "", crt, "Only with the crt filter");
  toggle(ctx, "curvature", c, d.curvature, crt);
  c = row("Aspect", "aspect", "PAL pixels are 16:15 wide, so 4:3 corrected is the faithful shape.", "F4");
  choice(ctx, "aspect", c, d.aspect, kAspects);
  c = row("Whole-pixel scale", "integer", "Only whole-number scale factors.", "");
  toggle(ctx, "integer", c, d.integer);
  c = row("Smooth frames", "smooth", "Interpolated frames at the display rate.", "F3", !is_st, st_reason, is_st ? "Not for ST" : "");
  choice(ctx, "smooth", c, d.smooth, kSmooth, !is_st);
  const bool smooth_on = d.smooth > 0 && !is_st;
  c = row("Smooth size", "interp_size", "Canvas scale for the smooth frames.", "", smooth_on, is_st ? st_reason : "Turn on smooth frames first");
  stepper(ctx, "interp_size", c, d.interp_size, 1, 8, 1, std::to_string(d.interp_size) + "x", smooth_on);
  c = row("Smooth far layer", "smooth_far", "Glide the far background between frames.", "", smooth_on, is_st ? st_reason : "Turn on smooth frames first");
  toggle(ctx, "smooth_far", c, d.smooth_far, smooth_on);
  c = row("Widescreen", "wide", "Extra picture beside the original.", "End", !is_st, "The Atari ST release has no widescreen", is_st ? "Not for ST" : "");
  choice(ctx, "wide", c, d.wide, kWide, !is_st);
  c = row("HD status panel", "hd_panel", "A sharper status panel.", "PgDn", !is_st, "The Atari ST release has no HD panel", is_st ? "Not for ST" : "");
  toggle(ctx, "hd_panel", c, d.hd_panel, !is_st);
  c = row("Asset packs", "packs", "Use replacement graphics from enabled packs.", "Home", true, "", "next start");
  toggle(ctx, "packs", c, d.packs);
  s.space(10);
  // A generated test card shows the filter and aspect choices without the game.
  const Rect pv_area = s.row(136);
  const int ph = 128;
  const int pw = d.aspect == 0 ? 160 : 128 * 5 / 4;
  const Rect pv{pv_area.x + 12, pv_area.y + 4, pw, ph};
  c = pv;
  ctx.canvas().frame(pv.inset(-2), t.c.line, 1);
  draw_test_card(ctx.canvas(), pv);
  if (d.filter == 2 || d.filter == 3) apply_scanlines(ctx.canvas(), pv, 90);
  draw_paragraph(ctx, {pv.right() + 12, pv.y, pv_area.right() - pv.right() - 12, 60},
                 "Test card, generated here. The game's own picture is never used by this page; in the in-game menu "
                 "your changes show on the running picture.",
                 t.c.dim);
  ctx.scroll_end(sv, s.used() + 8);
  draw_caption(ctx, help_rect, ctx.help(), t.c.dim);
}

// ---- Audio ----
void page_audio(Context& ctx, DemoState& st, Rect body, Rect help_rect) {
  page_heading(ctx, body, "Audio", std::string("for: ") + kGameNames[static_cast<int>(st.game)]);
  ScrollView sv = ctx.scroll_begin("audio", body);
  Stack s(body.x, sv.origin_y() + 4, body.w - 6, 0);
  auto row = [&](const char* label, const char* id, const char* help, bool en = true, const char* reason = "") {
    RowSpec r;
    r.label = label;
    r.help = help;
    r.enabled = en;
    r.reason = reason;
    return setting_row(ctx, s, id, r, 160);
  };
  Rect c = row("Sound", "sound", "Turns all sound on or off.");
  toggle(ctx, "sound", c, st.sound);
  c = row("Master volume", "master", "Overall volume.");
  slider(ctx, "master", c, st.master, 0, 100, 5, pct(st.master), st.sound);
  c = row("Sound effects", "sfx", "Volume of effects.");
  slider(ctx, "sfx", c, st.sfx, 0, 100, 5, pct(st.sfx), st.sound);
  c = row("Music", "music", "Volume of the music.");
  slider(ctx, "music", c, st.music, 0, 100, 5, pct(st.music), st.sound);
  c = row("Speech", "speech", "Volume of speech.", st.game == Game::Cdtv, "Only the CDTV release has speech");
  slider(ctx, "speech", c, st.speech, 0, 100, 5, pct(st.speech), st.sound && st.game == Game::Cdtv);
  c = row("Output filter", "audio_filter", "A500, A1200 or none.");
  choice(ctx, "audio_filter", c, st.audio_filter, kAudioFilters);
  c = row("Asset-pack audio", "pack_audio", "Use replacement sounds from enabled packs.");
  toggle(ctx, "pack_audio", c, st.pack_audio);
  s.space(6);
  const Rect b = s.row(20);
  if (button(ctx, "Play test tone", {b.x + 12, b.y, 132, 20})) st.log.push_back({Severity::Info, "Tone requested (demo only)"});
  ctx.scroll_end(sv, s.used() + 8);
  draw_caption(ctx, help_rect, ctx.help(), ctx.theme.c.dim);
}

// ---- Controls ----
void page_controls(Context& ctx, DemoState& st, Rect body, Rect help_rect) {
  page_heading(ctx, body, "Controls", "Keyboard");
  ScrollView sv = ctx.scroll_begin("controls", body);
  Stack s(body.x, sv.origin_y() + 4, body.w - 6, 0);
  static const char* const kActions[] = {"Move up", "Move down", "Move left", "Move right", "Fire", "Second button"};
  for (size_t i = 0; i < st.keys.size(); ++i) {
    RowSpec r;
    r.label = kActions[i];
    r.help = "Enter, then press a key. Esc cancels, Backspace clears.";
    const std::string id = "key" + std::to_string(i);
    const Rect c = setting_row(ctx, s, id, r, 140);
    int key = 0;
    const CaptureResult res = key_capture(ctx, id, c, st.keys[i].empty() ? "(none)" : st.keys[i], key);
    if (res == CaptureResult::Captured) st.keys[i] = "Key " + std::to_string(key);
    if (res == CaptureResult::Cleared) st.keys[i].clear();
  }
  s.space(6);
  section_header(ctx, s.row(12), "Gamepad");
  static constexpr std::string_view kFire[] = {"A / Cross / B", "Any face button"};
  RowSpec r;
  r.label = "Fire";
  r.help = "Which pad buttons fire.";
  const Rect c = setting_row(ctx, s, "pad_fire", r, 140);
  choice(ctx, "pad_fire", c, st.pad_fire, kFire);
  ctx.scroll_end(sv, s.used() + 8);
  draw_caption(ctx, help_rect, ctx.help(), ctx.theme.c.dim);
}

// ---- Asset packs ----
void page_packs(Context& ctx, DemoState& st, Rect body, Rect help_rect) {
  page_heading(ctx, body, "Asset packs", "Later packs override earlier ones");
  static const std::vector<std::string> names = {"[x] 1  Crisp Sprites 2x     disk, cdtv  graphics  ok",
                                                  "[x] 2  Remastered Music     disk        audio     ok",
                                                  "[ ] 3  Title Screen Remake  disk        screens   1 warning",
                                                  "[!]    Broken Pack          --          --        unusable"};
  std::vector<std::string> shown = names;
  for (size_t i = 0; i < st.pack_on.size(); ++i) shown[i][1] = st.pack_on[i] ? 'x' : ' ';
  ScrollView sv = ctx.scroll_begin("packs", body);
  Stack s(body.x + 4, sv.origin_y() + 4, body.w - 12, 8);
  const ListResult res = list(ctx, "packs", s.row(4 * 20 + 6), shown, st.pack_selected, {.activate_on_click = false, .default_focus = true});
  if (res.activated && st.pack_selected < static_cast<int>(st.pack_on.size())) {
    st.pack_on[static_cast<size_t>(st.pack_selected)] = !st.pack_on[static_cast<size_t>(st.pack_selected)];
  }
  const Rect brow = s.row(22);
  if (button(ctx, "Check pack", {brow.x, brow.y, 108, 22})) {
    st.pack_report = {{Severity::Info, "Checking pack..."},
                      {Severity::Ok, "Manifest is valid"},
                      {Severity::Warn, "1 screen has a different size from the original"},
                      {Severity::Info, "Entries 412, skipped 0"}};
  }
  button(ctx, "Move up", {brow.x + 116, brow.y, 80, 22});
  button(ctx, "Move down", {brow.x + 204, brow.y, 96, 22});
  message_list(ctx, "report", s.row(80), st.pack_report);
  ctx.scroll_end(sv, s.used() + 8);
  draw_caption(ctx, help_rect, "Changes apply at the next start.", ctx.theme.c.dim);
}

// ---- Advanced ----
void page_advanced(Context& ctx, DemoState& st, Rect body, Rect help_rect) {
  page_heading(ctx, body, "Advanced", "");
  ScrollView sv = ctx.scroll_begin("advanced", body);
  Stack s(body.x + 4, sv.origin_y() + 4, body.w - 12, 8);
  progress(ctx, s.row(22), st.progress_permille);
  const Rect b = s.row(22);
  if (button(ctx, "Advance", {b.x, b.y, 88, 22}, {.primary = true, .default_focus = true})) st.progress_permille = (st.progress_permille + 170) % 1100;
  if (button(ctx, "Reset settings...", {b.x + 96, b.y, 148, 22})) ctx.open_modal("reset");
  if (button(ctx, "Add log line", {b.x + 252, b.y, 116, 22})) {
    static const Severity sev[] = {Severity::Info, Severity::Ok, Severity::Warn, Severity::Error};
    st.log.push_back({sev[st.log.size() % 4], "Log line " + std::to_string(st.log.size() + 1) + ": something happened that needs a sentence to explain it"});
  }
  message_list(ctx, "log", s.row(110), st.log);
  ctx.scroll_end(sv, s.used() + 8);
  draw_caption(ctx, help_rect, "Logs, diagnostics and protection information live here.", ctx.theme.c.dim);
}

void draw_modals(Context& ctx, DemoState& st) {
  Rect inner;
  if (ctx.modal_begin("reset", "Reset settings?", 300, 108, &inner)) {
    draw_paragraph(ctx, {inner.x, inner.y, inner.w, 30}, "This restores every setting to its default. Your imported games and packs are not touched.", ctx.theme.c.text);
    const Rect br = {inner.x, inner.bottom() - 24, inner.w, 24};
    if (button(ctx, "Cancel", {br.x, br.y, 96, 24}, {.default_focus = true})) ctx.close_modal();
    if (button(ctx, "Reset", {br.right() - 96, br.y, 96, 24}, {.primary = true})) {
      st.display = DisplayModel{};
      ctx.close_modal();
    }
    ctx.modal_end();
  }
}

// ---- About ----
void page_about(Context& ctx, DemoState&, Rect body) {
  const Theme& t = ctx.theme;
  page_heading(ctx, body, "About", "");
  draw_label(ctx, cut_top(body, 20), "x2recomp 0.5.0 preview", t.c.text);
  draw_paragraph(ctx, cut_top(body, 24), "Free software under GPL-3.0-or-later.", t.c.dim);
  cut_top(body, 6);
  draw_paragraph(ctx, cut_top(body, 36),
                 "This project contains no game data. You need your own copy of the game. The game belongs to its rights holders.", t.c.text);
  cut_top(body, 6);
  section_header(ctx, cut_top(body, 12), "Third-party notices");
  draw_paragraph(ctx, cut_top(body, 44), "Musashi (MIT), SDL3 (zlib), dr_flac, stb_vorbis, puff, Unscii font (public domain).", t.c.dim);
}

}  // namespace

DemoState::DemoState() {
  log = {{Severity::Info, "x2recomp started"}, {Severity::Ok, "Settings loaded"}, {Severity::Warn, "No gamepad connected"}};
}

const std::vector<PageItem>& pages() {
  static const std::vector<PageItem> p = {{"Play", Icon::Play},       {"Display", Icon::Display}, {"Audio", Icon::Speaker},
                                          {"Controls", Icon::Controller}, {"Packs", Icon::Puzzle},    {"Advanced", Icon::Gear},
                                          {"About", Icon::Info}};
  return p;
}

std::string display_command(const DemoState& st) {
  const DisplayModel& d = st.display;
  static const char* const games[] = {"disk", "cdtv", "st"};
  std::string s = std::string("x2recomp --game ") + games[static_cast<int>(st.game)];
  if (d.faithful) return s + " --faithful";
  if (d.size != 3) s += " --scale " + std::to_string(d.size);
  if (d.fullscreen) s += " --fullscreen";
  if (d.filter != 3) s += " --filter " + std::string(kFilters[d.filter]);
  if (d.curvature && d.filter == 3) s += " --curvature";
  if (d.aspect != 0) s += " --no-aspect";
  if (!d.integer) s += " --no-integer";
  std::string modern;
  if (d.smooth > 0 && st.game != Game::St) {
    modern += "interp";
    if (d.interp_size != 4) modern += ",scale=" + std::to_string(d.interp_size);
  }
  if (d.wide > 0 && st.game != Game::St) modern += std::string(modern.empty() ? "" : ",") + "wide=" + std::string(kWide[d.wide]);
  if (!modern.empty()) s += " --modern " + modern;
  return s;
}

void build(Context& ctx, DemoState& st) {
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  if (st.page == 0 && t.decor && !ctx.motion.reduced) {
    draw_starfield(c, ctx.screen(), ctx.now_ms(), t);
    ctx.animate();
  }
  Rect content = page_chrome(ctx, ctx.screen(), pages(), st.page);
  static const Hint hints[] = {{Nav::Up, "Move"}, {Nav::Accept, "Select"}, {Nav::Back, "Back"}, {Nav::PageNext, "Next page"},
                               {Nav::Menu, "Menu"}};
  const Rect hint = cut_bottom(content, 12);
  cut_bottom(content, 4);
  const Rect footer = cut_bottom(content, 32);
  cut_bottom(content, 4);
  const Rect help = cut_bottom(content, 10);
  cut_bottom(content, 4);
  const Rect body = ctx.begin_page(hash_id(0, pages()[static_cast<size_t>(st.page)].label), content);
  switch (st.page) {
    case 0: page_play(ctx, st, body); break;
    case 1: page_display(ctx, st, body, help); break;
    case 2: page_audio(ctx, st, body, help); break;
    case 3: page_controls(ctx, st, body, help); break;
    case 4: page_packs(ctx, st, body, help); break;
    case 5: page_advanced(ctx, st, body, help); break;
    default: page_about(ctx, st, body); break;
  }
  ctx.end_page(content);
  if (command_footer(ctx, footer, display_command(st))) st.copied = display_command(st);
  hint_bar(ctx, hint, hints);
  draw_modals(ctx, st);
}

}  // namespace x2::ui::demo
