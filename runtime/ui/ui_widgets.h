// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The widget set. Each widget is a function that takes the Context and a Rect, registers itself in the focus
// graph, handles keyboard, pad and pointer input for this pass, draws, and returns what happened. The label
// (or id string) of a widget is its identity: keep it stable between frames.
#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ui_art.h"
#include "ui_core.h"

namespace x2::ui {

enum class Align { Left, Center, Right };

// ---- text and surfaces ----
// Single-line text, vertically centred in r, shortened with an ellipsis if it does not fit.
void draw_label(Context& ctx, Rect r, std::string_view s, Color col, Align a = Align::Left);
void draw_caption(Context& ctx, Rect r, std::string_view s, Color col, Align a = Align::Left);
// The 8x16 face at 2x with one pixel of letter spacing (32 pixels high).
void draw_heading(Context& ctx, Rect r, std::string_view s, Color col);
// Word-wrapped caption text; returns the number of lines drawn.
int draw_paragraph(Context& ctx, Rect r, std::string_view s, Color col, int line_h = 11);
void panel(Context& ctx, Rect r, bool raised = false);
void focus_ring(Context& ctx, Rect r, bool breathe = false);
void section_header(Context& ctx, Rect r, std::string_view s);

// ---- badges ----
enum class BadgeKind { Info, Ok, Warn, Err, Dim };
int badge_width(std::string_view s);
int draw_badge(Context& ctx, int x, int y, std::string_view s, BadgeKind kind);  // returns its width; 12 pixels high

// ---- controls ----
struct ButtonOpts {
  bool primary = false;
  bool enabled = true;
  bool default_focus = false;
};
bool button(Context& ctx, std::string_view label, Rect r, ButtonOpts o = {});
bool toggle(Context& ctx, std::string_view id, Rect r, bool& value, bool enabled = true);
// text is what the caller wants shown between the - and + boxes.
bool stepper(Context& ctx, std::string_view id, Rect r, int& value, int lo, int hi, int step, std::string_view text,
             bool enabled = true);
bool slider(Context& ctx, std::string_view id, Rect r, int& value, int lo, int hi, int step, std::string_view text = {},
            bool enabled = true);
bool choice(Context& ctx, std::string_view id, Rect r, int& index, std::span<const std::string_view> items, bool enabled = true);

struct ListOpts {
  bool wrap = false;               // Up on the first row goes to the last (menus)
  bool activate_on_click = false;
  bool default_focus = false;
};
struct ListResult {
  bool changed = false;    // the selection moved
  bool activated = false;  // Accept, or a click when activate_on_click
};
ListResult list(Context& ctx, std::string_view id, Rect r, std::span<const std::string> items, int& selected, ListOpts o = {});

enum class Severity { Info, Ok, Warn, Error };
struct Message {
  Severity severity = Severity::Info;
  std::string text;
};
// A read-only log view. With `follow` it stays at the bottom while new lines arrive unless the user scrolls up.
void message_list(Context& ctx, std::string_view id, Rect r, std::span<const Message> items, bool follow = true);
void progress(Context& ctx, Rect r, int permille, std::string_view text = {});

struct CardSpec {
  std::string_view title, subtitle, status, action;
  Icon emblem = Icon::Floppy;
  Color accent = 0;  // 0 = the theme's amber
  bool dashed = false;
  bool enabled = true;
  bool default_focus = false;
  uint32_t seed = 0;
};
bool card(Context& ctx, std::string_view id, Rect r, const CardSpec& spec);

// ---- structure ----
struct PageItem {
  std::string_view label;
  Icon icon;
};
// The page selector: a rail on the left (canvas at least 560 wide) or a tab strip on top. Handles PagePrev and
// PageNext, clicks, and focus-follows-selection with keyboard or pad. Returns the content area.
Rect page_chrome(Context& ctx, Rect area, std::span<const PageItem> pages, int& selected);

struct RowSpec {
  std::string_view label, help, hint;
  std::string_view badge;
  BadgeKind badge_kind = BadgeKind::Info;
  bool enabled = true;
  std::string_view reason;  // shown instead of the help text when the row is disabled
};
// A settings row: label, help, hotkey hint, badge. Returns the rect for the control; draw it with the widget of
// your choice using the same `control_id`.
Rect setting_row(Context& ctx, Stack& st, std::string_view control_id, const RowSpec& row, int control_w = 112);

struct Hint {
  Nav action;
  std::string_view label;
};
std::string_view nav_caption(Nav n, Device d, PadStyle p);
void hint_bar(Context& ctx, Rect r, std::span<const Hint> hints);
// "Equivalent command line" with a Copy button; true when Copy was pressed.
bool command_footer(Context& ctx, Rect r, std::string_view command);

enum class CaptureResult { None, Captured, Cleared, Cancelled };
// A key binding row: Accept starts "press a key"; the next RawKey is returned in key_out.
CaptureResult key_capture(Context& ctx, std::string_view id, Rect r, std::string_view current, int& key_out,
                          std::string_view prompt = "Press a key...");

}  // namespace x2::ui
