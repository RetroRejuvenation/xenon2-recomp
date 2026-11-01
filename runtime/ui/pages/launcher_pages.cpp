// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "launcher_pages.h"

#include <algorithm>
#include <string>
#include <vector>

#include "import/variants.h"

namespace x2::ui {

const char* family_noun(GameKind g) {
  switch (g) {
    case GameKind::Disk: return "Amiga disk images";
    case GameKind::Cdtv: return "a CDTV disc image";
    case GameKind::St: return "Atari ST disk images";
  }
  return "";
}

namespace {

constexpr Hint kHints[] = {{Nav::Up, "Move"}, {Nav::Accept, "Select"}, {Nav::Back, "Back"}, {Nav::PageNext, "Next page"}};

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

Color accent_for(const Theme& t, GameKind g) { return g == GameKind::Disk ? t.c.amber : g == GameKind::Cdtv ? t.c.cyan : t.c.ok; }
Icon icon_for(GameKind g) { return g == GameKind::Disk ? Icon::Floppy : g == GameKind::Cdtv ? Icon::Disc : Icon::Chip; }

Color severity_color(const Theme& t, Severity s) {
  switch (s) {
    case Severity::Ok: return t.c.ok;
    case Severity::Warn: return t.c.warn;
    case Severity::Error: return t.c.err;
    default: return t.c.text;
  }
}

// What the display and audio settings come to, in a few words, for the links under the cards.
std::string display_summary(const Settings& s) {
  std::string t = s.faithful ? "faithful" : "";
  if (!s.faithful) {
    const char* const filters[] = {"nearest", "sharp", "scanlines", "crt", "edge"};
    t = filters[static_cast<int>(s.video_filter)];
    t += s.aspect ? ", 4:3" : ", square pixels";
    if (s.interp) t += ", smooth frames";
  }
  return "Display: " + t + (s.fullscreen ? ", fullscreen" : "");
}
std::string audio_summary(const Settings& s) {
  std::string t = s.audio ? "on, volume " + std::to_string(s.volume) + "%" : "off";
  return "Audio: " + t;
}

// ---- the home page ----
void page_library(Context& ctx, LauncherState& st, Rect body, Rect help) {
  const Theme& t = ctx.theme;
  const Rect head = cut_top(body, 36);
  draw_heading(ctx, {head.x, head.y, head.w, 32}, "X2RECOMP", t.c.amber);
  const int tw = 8 * 17 + 14;
  draw_caption(ctx, {head.x + tw, head.y + 4, head.w - tw, 10}, "A native port. Bring your own copy.", t.c.dim);
  if (!st.env.version.empty()) draw_caption(ctx, {head.x + tw, head.y + 17, head.w - tw, 10}, "version " + st.env.version, t.c.dim);
  cut_top(body, 6);

  ScrollView sv = ctx.scroll_begin("library", body);
  const int w = body.w - 6;
  int y = sv.origin_y() + 2;
  const int gap = 8;
  const int card_h = 124;  // the canvas is at least 480 wide, so three cards always fit side by side
  const GameKind games[3] = {GameKind::Disk, GameKind::Cdtv, GameKind::St};
  for (int i = 0; i < 3; ++i) {
    const GameKind g = games[i];
    const LibraryEntry& e = st.library.get(g);
    const Rect r{body.x + i * ((w - 2 * gap) / 3 + gap), y, (w - 2 * gap) / 3, card_h};
    CardSpec c;
    c.title = game_name(g);
    c.emblem = icon_for(g);
    c.accent = accent_for(t, g);
    c.seed = static_cast<uint32_t>(i + 1);
    c.default_focus = g == st.selected;
    std::string sub, status;
    if (e.present) {
      sub = e.variant.empty() ? "Imported" : e.variant;
      status = g == GameKind::Cdtv ? std::string(e.has_cd_audio ? "CD music: yes" : "no CD music") : e.status;
      c.action = g == st.selected && st.model.s.last_game == game_flag_value(g) ? "Play (last)" : "Play";
    } else {
      sub = "Not imported";
      c.action = "Import...";
      c.dashed = true;
    }
    c.subtitle = sub;
    c.status = status;
    const std::string id = std::string("card_") + game_flag_value(g);
    if (card(ctx, id, r, c)) {
      st.selected = g;
      st.model.game = g;
      if (e.present) st.play(g);
      else st.open_import();
    }
    if (ctx.is_focused(ctx.id(id)) && st.selected != g) {
      st.selected = g;
      st.model.game = g;
    }
  }
  y += card_h + 8;

  Stack row(body.x, y, w, 0);
  const Rect b = row.row(22);
  if (button(ctx, "Import another game...", {b.x, b.y, 188, 22})) st.open_import();
  if (button(ctx, "Quit", {b.right() - 72, b.y, 72, 22})) st.quit = true;
  row.space(4);
  const Rect l1 = row.row(18);
  if (button(ctx, display_summary(st.model.s), {l1.x, l1.y, std::min(l1.w, 340), 18})) st.page = kDisplayPage;
  row.space(2);
  const Rect l2 = row.row(18);
  if (button(ctx, audio_summary(st.model.s), {l2.x, l2.y, std::min(l2.w, 340), 18})) st.page = kAudioPage;
  row.space(4);
  const Rect tg = row.row(20);
  bool direct = st.model.s.launcher == "never";
  if (toggle(ctx, "start_last_directly", {tg.x, tg.y, 56, 18}, direct)) st.model.s.launcher = direct ? "never" : "auto";
  draw_label(ctx, {tg.x + 66, tg.y, tg.w - 66, tg.h}, "Start the last game directly next time", t.c.text);
  if (st.model.s.launcher == "never")
    draw_caption(ctx, {tg.x + 66, tg.bottom() - 2, tg.w - 66, 10}, "Run x2recomp --gui to see this launcher again.", t.c.dim);
  row.space(12);
  if (!st.note.empty()) {
    draw_caption(ctx, row.row(12), st.note, t.c.ok);
    row.space(2);
  }
  if (!st.env.args_note.empty()) {
    draw_paragraph(ctx, row.row(24), "Started with " + st.env.args_note + ". Those arguments override the settings for this run.", t.c.cyan);
    row.space(4);
  }
  Stack rows(body.x, y + row.used() + 8, w, 0);
  settings_rows(ctx, st.model, kPagePlay, rows);
  ctx.scroll_end(sv, (rows.y - sv.origin_y()) + 8);
  draw_caption(ctx, help, ctx.help(), t.c.dim);
}

// ---- About ----
void page_about(Context& ctx, LauncherState& st, Rect body) {
  const Theme& t = ctx.theme;
  page_heading(ctx, body, "About", "");
  ScrollView sv = ctx.scroll_begin("about", body);
  Stack s(body.x, sv.origin_y() + 2, body.w - 6, 6);
  draw_label(ctx, s.row(20), "x2recomp " + (st.env.version.empty() ? std::string("(development build)") : st.env.version), t.c.text);
  draw_paragraph(ctx, s.row(24), "Free software under the GPL, version 3 or later. The source and the documentation are on the project page.", t.c.dim);
  draw_paragraph(ctx, s.row(36),
                 "This project contains no game data. You need your own copy of the game. The game belongs to its rights holders; this "
                 "program only runs the copy you give it.",
                 t.c.text);
  section_header(ctx, s.row(12), "Third-party software");
  draw_paragraph(ctx, s.row(46),
                 "Musashi (MIT), SDL3 (zlib), puff (zlib), dr_flac (public domain or MIT-0), stb_vorbis (MIT or public domain) and the "
                 "Unscii fonts (public domain). The full notices ship with every release (THIRD-PARTY-NOTICES.md).",
                 t.c.dim);
  if (st.env.open_data_folder) {
    const Rect b = s.row(22);
    if (button(ctx, "Open the data folder", {b.x, b.y, 168, 22})) st.env.open_data_folder();
  }
  ctx.scroll_end(sv, s.used() + 8);
}

void placeholder(Context& ctx, Rect body, std::string_view title, std::string_view text) {
  page_heading(ctx, body, title, "");
  draw_paragraph(ctx, cut_top(body, 44), text, ctx.theme.c.dim);
}

// ---- the import flow ----
void draw_patch_modal(Context& ctx, LauncherState& st) {
  const Theme& t = ctx.theme;
  Rect inner;
  if (!ctx.modal_begin("patch", "Protection patch (opt-in)", 456, 260, &inner)) return;
  static const std::vector<Message> text = [] {
    std::vector<Message> v;
    for (const std::string& line : import::protection_patch_disclaimer()) v.push_back({Severity::Info, line});
    return v;
  }();
  const Rect br = cut_bottom(inner, 24);
  cut_bottom(inner, 4);
  const Rect own = cut_bottom(inner, 20);
  cut_bottom(inner, 4);
  message_list(ctx, "patch_text", inner, text, false);
  toggle(ctx, "patch_owned", {own.x, own.y, 56, 18}, st.patch_owned);
  draw_label(ctx, {own.x + 66, own.y, own.w - 66, own.h}, "I own the original disc", t.c.text);
  if (button(ctx, "Cancel", {br.x, br.y, 96, 24}, {.default_focus = true})) {
    st.patch_owned = false;
    ctx.close_modal();
  }
  if (button(ctx, "Apply the patch", {br.right() - 148, br.y, 148, 24}, {.primary = true, .enabled = st.patch_owned})) {
    ctx.close_modal();
    st.import_with_patch();
  }
  ctx.modal_end();
}

void draw_import(Context& ctx, LauncherState& st) {
  const Theme& t = ctx.theme;
  if (!ctx.modal_open() && ctx.peek_nav(Nav::Back)) {
    ctx.take_nav(Nav::Back);
    if (!st.job.running && !st.busy()) st.close_import();
  }
  Rect r = ctx.screen().inset(16, 10);
  const bool small = r.h < 250;
  const bool running = st.job.running || st.busy();

  const Rect head = cut_top(r, small ? 20 : 32);
  if (small) draw_text(ctx.canvas(), Font::body(), head.x, head.y + 2, "Import your game", t.c.text, 1, 1);
  else draw_heading(ctx, head, "Import your game", t.c.text);
  cut_top(r, small ? 2 : 4);

  const Rect zone = cut_top(r, small ? 38 : 54);
  panel(ctx, zone, true);
  draw_label(ctx, {zone.x, zone.y + (small ? 5 : 9), zone.w, 16}, "Drop your disk images, disc image or folder here", t.c.text, Align::Center);
  draw_caption(ctx, {zone.x, zone.y + (small ? 24 : 31), zone.w, 10}, "adf adz dms ipf scp raw hfe lha zip cue iso st msa stx", t.c.dim, Align::Center);
  cut_top(r, 6);

  // While an import runs the focus belongs to Cancel (the other buttons are inert).
  WState& run_state = ctx.state(ctx.id("import_running"));
  if (running && run_state.aux == 0) ctx.set_focus(ctx.id("Cancel"));
  run_state.aux = running ? 1 : 0;
  const Rect br = cut_top(r, 22);
  if (button(ctx, "Choose files...", {br.x, br.y, 124, 22}, {.primary = !running, .enabled = !running && static_cast<bool>(st.env.choose_files), .default_focus = !running}))
    st.env.choose_files();
  if (button(ctx, "Choose a folder...", {br.x + 132, br.y, 148, 22}, {.enabled = !running && static_cast<bool>(st.env.choose_folder)}))
    st.env.choose_folder();
  // The buttons sit side by side so Left and Right step through them (a wide control below would otherwise win the focus search).
  if (button(ctx, st.back_label(), {br.x + 288, br.y, 80, 22}, {.enabled = !running})) st.close_import();
  if (!st.dropped.empty() && !running && button(ctx, "Start over", {br.x + 376, br.y, 96, 22})) st.forget_dropped();
  cut_top(r, 6);

  const Rect legal = cut_top(r, 33);
  draw_paragraph(ctx, legal,
                 "This project ships no game data. Your files are read on this computer and copied into your own data folder. "
                 "Nothing is uploaded or shared.",
                 t.c.dim);
  cut_top(r, 2);
  if (!st.dropped.empty()) {
    const std::string given = std::to_string(st.dropped.size()) + (st.dropped.size() == 1 ? " file" : " files") + " given: " +
                              family_noun(detect_family(st.dropped));
    draw_caption(ctx, cut_top(r, 12), given, t.c.cyan);
  }
  if (!st.headline.empty()) draw_label(ctx, cut_top(r, 18), st.headline, severity_color(t, st.headline_severity));
  if (running) {
    const Rect pr = cut_top(r, 20);
    const std::string stage = st.job.stage.empty() ? "Working" : st.job.stage;
    progress(ctx, {pr.x, pr.y + 1, pr.w - 96, 18}, st.job.permille, stage);
    if (button(ctx, "Cancel", {pr.right() - 88, pr.y, 88, 20}, {.default_focus = true})) st.cancel_import();
    cut_top(r, 4);
  }
  if (st.patch_offer && !running) {
    const Rect pb = cut_top(r, 22);
    if (button(ctx, "Use the opt-in protection patch...", {pb.x, pb.y, 272, 22})) {
      st.patch_owned = false;
      ctx.open_modal("patch");
    }
    cut_top(r, 4);
  }
  const Rect hint = cut_bottom(r, 12);
  cut_bottom(r, 4);
  const Rect footer = cut_bottom(r, 32);
  cut_bottom(r, 4);
  if (r.h >= 36) message_list(ctx, "import_log", r, st.log, true);
  static const std::string kImportCommand = "x2recomp --import PATH [PATH2]";
  if (command_footer(ctx, footer, kImportCommand)) {
    st.model.copied = kImportCommand;
    if (st.env.copy_text) st.env.copy_text(kImportCommand);
  }
  static const Hint hints[] = {{Nav::Up, "Move"}, {Nav::Accept, "Select"}, {Nav::Back, "Back"}};
  hint_bar(ctx, hint, hints);
  if (running) ctx.animate();
  draw_patch_modal(ctx, st);
}

}  // namespace

void launcher_frame(Context& ctx, LauncherState& st) {
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  if (st.in_import) {
    draw_import(ctx, st);
    return;
  }
  if (st.page == kPlayPage && t.decor && !ctx.motion.reduced) {
    draw_starfield(c, ctx.screen(), ctx.now_ms(), t);
    ctx.animate();
  }
  Rect content = page_chrome(ctx, ctx.screen(), page_items(), st.page);
  const Rect hint = cut_bottom(content, 12);
  cut_bottom(content, 4);
  const Rect footer = cut_bottom(content, 32);
  cut_bottom(content, 4);
  const Rect help = cut_bottom(content, 10);
  cut_bottom(content, 4);
  const Rect body = ctx.begin_page(hash_id(0, page_items()[static_cast<size_t>(st.page)].label), content);
  switch (st.page) {
    case kPlayPage: page_library(ctx, st, body, help); break;
    case kDisplayPage: page_display(ctx, st.model, body, help); break;
    case kAudioPage: page_audio(ctx, st.model, body, help); break;
    case kControlsPage: placeholder(ctx, body, "Controls", "Keyboard and gamepad bindings are built by a later part of the launcher."); break;
    case kPacksPage: placeholder(ctx, body, "Asset packs", "The pack list is built by a later part of the launcher."); break;
    case kAdvancedPage: placeholder(ctx, body, "Advanced", "Diagnostics and logs are built by a later part of the launcher."); break;
    default: page_about(ctx, st, body); break;
  }
  ctx.end_page(content);
  if (settings_footer(ctx, st.model, footer) && st.env.copy_text) st.env.copy_text(st.model.copied);
  hint_bar(ctx, hint, kHints);
}

}  // namespace x2::ui
