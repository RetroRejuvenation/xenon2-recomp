// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "advanced.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "../log_buffer.h"
#include "import/importer.h"
#include "import/variants.h"
#include "page_common.h"

namespace x2::ui {

bool AdvancedModel::check_image(const std::vector<std::string>& paths) {
  if (paths.empty() || worker_.busy()) return false;
  report.clear();
  report_title = "Image report";
  job_ = JobView{};
  job_.running = true;
  const bool started = worker_.start([paths](JobContext& j) {
    j.progress(100, "Reading the images");
    bool unreadable = false;
    const std::vector<std::string> lines = import::diagnose_report(paths, &unreadable);
    for (const std::string& l : lines) j.message(Severity::Info, l);
    j.message(unreadable ? Severity::Error : Severity::Ok, unreadable ? "Some input could not be read." : "Done. No game bytes are shown or copied.");
    j.progress(1000, "Done");
  });
  if (!started) job_.running = false;
  return started;
}

void AdvancedModel::poll() {
  std::vector<Message> fresh;
  job_ = worker_.poll(fresh);
  for (Message& m : fresh) {
    if (m.severity == Severity::Error || m.severity == Severity::Warn) LogBuffer::instance().add(m.severity, "image check: " + m.text);
    report.push_back(std::move(m));
  }
  if (worker_.collect()) job_.running = false;
}

std::string AdvancedModel::diagnostics(const Library& lib, const Settings& s, GameKind game) const {
  return diagnostics_text(version, host, lib, s, game);
}

const std::vector<CliTool>& command_line_tools() {
  static const std::vector<CliTool> tools = {
      {"Check an image from a shell", "x2recomp --diagnose-image FILE"},
      {"Check an asset pack", "x2recomp --check-asset-pack PACK"},
      {"Dump graphics and sound for pack authors", "x2recomp --headless --dump-assets DIR"},
      {"Record the sound of a run", "x2recomp --headless --frames 3000 --audio-out run.wav"},
      {"Save a frame as a picture", "x2recomp --headless --frames 600 --screenshot-at 500:frame.png"},
      {"Run a scripted input replay", "x2recomp --headless --input-script FILE --hash-every 50"},
      {"Accurate or fast timing", "x2recomp --timing accurate"},
      {"Chip memory size (recompiled builds)", "x2recomp --chip-ram 512"},
      {"List every option", "x2recomp --help"},
  };
  return tools;
}

namespace {

constexpr int kRowH = 22;

bool confirm_modal(Context& ctx, const char* id, const char* title, const char* text, const char* yes) {
  Rect inner;
  if (!ctx.modal_begin(id, title, 340, 120, &inner)) return false;
  draw_paragraph(ctx, cut_top(inner, 48), text, ctx.theme.c.text, 11);
  Rect buttons = cut_bottom(inner, 22);
  const Rect ok = cut_left(buttons, 132);
  cut_left(buttons, 8);
  const Rect cancel = cut_left(buttons, 96);
  bool confirmed = false;
  ButtonOpts o;
  o.primary = true;
  ctx.push_id(id);
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

void disclaimer_modal(Context& ctx) {
  Rect inner;
  if (!ctx.modal_begin("disclaimer", "Protection patch (opt-in)", 456, 260, &inner)) return;
  static const std::vector<Message> text = [] {
    std::vector<Message> v;
    for (const std::string& line : import::protection_patch_disclaimer()) v.push_back({Severity::Info, line});
    return v;
  }();
  const Rect br = cut_bottom(inner, 24);
  cut_bottom(inner, 4);
  message_list(ctx, "disclaimer_text", inner, text, false);
  if (button(ctx, "Close", {br.x, br.y, 96, 24}, {.default_focus = true})) ctx.close_modal();
  ctx.modal_end();
}

// Buttons side by side, wrapping onto the next row when the next one does not fit, so none is ever lost on a narrow canvas. Left and Right
// step through a row, Up and Down between rows. Returns the index pressed, or -1.
struct Btn {
  const char* label;
  bool enabled = true;
  bool primary = false;
};
int button_row(Context& ctx, Stack& st, std::initializer_list<Btn> btns) {
  Rect r = st.row(kRowH);
  int x = r.x, hit = -1, i = 0;
  for (const Btn& b : btns) {
    const int w = std::min(button_w(b.label), r.w);
    if (x + w > r.right() && x > r.x) {
      r = st.row(kRowH);
      x = r.x;
    }
    if (button(ctx, b.label, {x, r.y, w, kRowH}, {.primary = b.primary, .enabled = b.enabled})) hit = i;
    x += w + 8;
    ++i;
  }
  return hit;
}

}  // namespace

bool page_advanced(Context& ctx, SettingsModel& m, AdvancedModel& adv, const Library& lib, Rect body, Rect help) {
  const Theme& t = ctx.theme;
  page_title(ctx, body, "Advanced", "Diagnostics, logs and tools");
  ScrollView sv = ctx.scroll_begin("advanced", body);
  Stack st(body.x, sv.origin_y() + 2, body.w - 6, 4);
  bool changed = false;

  // ---- imported games ----
  section_header(ctx, st.row(12), "Imported games");
  bool any = false;
  for (GameKind g : {GameKind::Disk, GameKind::Cdtv, GameKind::St}) {
    const LibraryEntry& e = lib.get(g);
    if (!e.present) continue;
    any = true;
    draw_label(ctx, st.row(16), std::string(game_name(g)) + ": " + (e.variant.empty() ? std::string("imported") : e.variant), t.c.text);
    std::string detail = "importer " + (e.importer_version.empty() ? std::string("?") : e.importer_version) + ", ";
    detail += e.protection == "none to answer" ? std::string("no protection to answer") : e.protection;
    if (!e.receipt_sha1.empty()) detail += " " + e.receipt_sha1;
    if (!e.source_name.empty()) detail += ", " + e.source_name;
    draw_caption(ctx, st.row(10), detail, t.c.dim);
  }
  if (!any) draw_caption(ctx, st.row(10), "Nothing is imported yet.", t.c.dim);
  paragraph(ctx, st,
            "The opt-in protection patch can only be applied while importing, and only after you confirm that you own the original disc. "
            "It is never the default: the key-track receipt is.",
            t.c.dim);
  const int b1 = button_row(ctx, st, {{"Import again...", static_cast<bool>(adv.reimport)}, {"Read the patch disclaimer"}});
  if (b1 == 0) adv.reimport();
  if (b1 == 1) ctx.open_modal("disclaimer");
  st.space(6);

  // ---- diagnostics ----
  section_header(ctx, st.row(12), "Diagnostics");
  const bool busy = adv.busy();
  const int b2 = button_row(ctx, st, {{"Copy diagnostics", static_cast<bool>(adv.copy_text)}, {"Check an image...", !busy && static_cast<bool>(adv.choose_image)}});
  if (b2 == 0) {
    adv.copy_text(adv.diagnostics(lib, m.s, m.game));
    adv.note = "Copied the diagnostics: no game data and no folder names beyond the data folder's own.";
    adv.note_severity = Severity::Ok;
  }
  if (b2 == 1) adv.choose_image();
  const int b3 = button_row(ctx, st, {{"Open the data folder", static_cast<bool>(adv.open_data_folder)},
                                       {"Open the log file", static_cast<bool>(adv.open_log) && !LogBuffer::instance().file_path().empty()}});
  if (b3 == 0) adv.open_data_folder();
  if (b3 == 1) adv.open_log();
  if (!adv.note.empty()) draw_caption(ctx, st.row(10), adv.note, adv.note_severity == Severity::Ok ? t.c.ok : t.c.dim);
  changed |= settings_rows(ctx, m, kPageAdvanced, st);
  if (busy || !adv.report.empty()) {
    st.space(4);
    if (busy) {
      const Rect pr = st.row(20);
      progress(ctx, {pr.x, pr.y + 1, pr.w - 96, 18}, adv.job().permille, adv.job().stage.empty() ? "Working" : adv.job().stage);
      if (button(ctx, "Cancel", {pr.right() - 88, pr.y, 88, 20})) adv.cancel();
      ctx.animate();
    }
    const int rh = std::min(120, std::max(60, body.h / 3));
    message_list(ctx, "image_report", st.row(rh), adv.report, true);
  }
  st.space(6);

  // ---- the log ----
  section_header(ctx, st.row(12), "Log");
  const uint64_t gen = LogBuffer::instance().generation();
  if (gen != adv.log_seen || adv.log_view.empty()) {
    adv.log_view = LogBuffer::instance().lines();
    adv.log_seen = gen;
  }
  if (adv.log_view.empty()) adv.log_view.push_back({Severity::Info, "Nothing logged yet."});
  const int lh = std::min(110, std::max(56, body.h / 3));
  message_list(ctx, "log_view", st.row(lh), adv.log_view, true);
  draw_caption(ctx, st.row(10), "The last 500 lines. The file x2recomp.log in the data folder keeps more (and x2recomp.log.1 before it).", t.c.dim);
  st.space(6);

  // ---- the settings file ----
  section_header(ctx, st.row(12), "Settings file");
  const int b4 = button_row(ctx, st, {{"Export settings...", static_cast<bool>(adv.export_settings)},
                                       {"Import settings...", static_cast<bool>(adv.import_settings)},
                                       {"Reset all settings..."}});
  if (b4 == 0) adv.export_settings();
  if (b4 == 1) adv.import_settings();
  if (b4 == 2) ctx.open_modal("reset_all");
  draw_caption(ctx, st.row(10), "The same plain text file as settings.cfg. Key bindings and imported games are not touched.", t.c.dim);
  st.space(6);

  // ---- the command line ----
  section_header(ctx, st.row(12), "Command line only");
  paragraph(ctx, st,
            "Replays, traces, hashes, the dump and the developer options have no buttons: they are for scripts and testing. "
            "The page for each is in the documentation; these are the common ones.",
            t.c.dim);
  for (const CliTool& c : command_line_tools()) {
    draw_caption(ctx, st.row(10), c.what, t.c.dim);
    draw_label(ctx, st.row(16), c.command, t.c.cyan);
  }
  const int b5 = button_row(ctx, st, {{"Copy the list", static_cast<bool>(adv.copy_text)}});
  if (b5 == 0) {
    std::string all;
    for (const CliTool& c : command_line_tools()) all += std::string("# ") + c.what + "\n" + c.command + "\n";
    adv.copy_text(all);
    adv.note = "Copied the command list.";
    adv.note_severity = Severity::Ok;
  }

  ctx.scroll_end(sv, st.used() + 8);
  draw_caption(ctx, help, ctx.help(), t.c.dim);
  return changed;
}

void advanced_modals(Context& ctx, AdvancedModel& adv) {
  disclaimer_modal(ctx);
  if (confirm_modal(ctx, "reset_all", "Reset all settings?",
                    "Every option goes back to its default. Key bindings (input.cfg) and imported games are not touched.", "Reset settings"))
    adv.reset_requested = true;
}

}  // namespace x2::ui
