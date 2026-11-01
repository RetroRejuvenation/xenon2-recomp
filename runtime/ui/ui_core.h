// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The immediate-mode context: per-pass input, widget identity and retained state, focus graph and navigation,
// scroll areas, modals and page transitions. Widgets (ui_widgets.h) are plain functions that take a Context.
//
// Model: run() executes the builder once per input event (so one Accept or one Down arrow is seen by exactly one
// pass), then re-runs it until nothing changes (a moved focus or scroll offset shows up in the final frame).
// Every pass redraws the whole frame into the canvas; the host only calls run() when an event arrives or
// animating() says something is moving, so an idle UI costs nothing.
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ui_canvas.h"
#include "ui_font.h"
#include "ui_input.h"
#include "ui_theme.h"
#include "ui_types.h"

namespace x2::ui {

// The logical canvas for a window: the largest whole number n for which the canvas stays at least
// 480x270 (320x180 with Large text), centred in the window.
struct CanvasGeometry {
  int cw = 0, ch = 0;  // canvas size in logical pixels
  int n = 1;           // device pixels per logical pixel
  int ox = 0, oy = 0;  // window offset of the canvas
};
CanvasGeometry compute_geometry(int win_w, int win_h, bool large_text);

enum class Region { Page, Rail };

struct FocusOpts {
  bool default_focus = false;  // preferred first focus on a page
  Region region = Region::Page;
  bool selected = false;       // the current rail entry (target of Back)
  bool consumes_horizontal = false;  // value widgets use Left/Right themselves
};

struct FocusItem {
  Id id = 0;
  Rect rect;
  Region region = Region::Page;
  bool selected = false;
  bool in_modal = false;
  bool default_focus = false;
  bool consumes_horizontal = false;
  Id scope = 0;  // the innermost scroll area it was built in (0: none); Home/End stay within a scope
};

// Retained per-widget state, keyed by widget id.
struct WState {
  int scroll = 0;
  int content_h = 0;
  int aux = 0;
  int aux2 = 0;
};

struct ScrollView {
  Id id = 0;
  Rect view;
  int offset = 0;
  size_t first_item = 0;
  int origin_y() const { return view.y - offset; }
};

// Vertical stacking of rows inside a column.
struct Stack {
  int x = 0, w = 0, y0 = 0, y = 0, gap = 0;
  Stack() = default;
  Stack(int x_, int y_, int w_, int gap_ = 0) : x(x_), w(w_), y0(y_), y(y_), gap(gap_) {}
  Rect row(int h) {
    const Rect r{x, y, w, h};
    y += h + gap;
    return r;
  }
  void space(int h) { y += h; }
  int used() const { return y > y0 ? y - y0 - gap : 0; }
};

class Context {
 public:
  Context();
  using Builder = std::function<void(Context&)>;

  Theme theme = Theme::standard();
  Motion motion;
  PadStyle pad_style = PadStyle::Generic;  // set by the host from the connected pad

  // Runs the builder for `events` (each in its own pass) and settles. Returns animating().
  bool run(Canvas& canvas, const EventList& events, int dt_ms, const Builder& builder);
  bool animating() const { return animating_; }

  // ---- inside the builder ----
  Canvas& canvas() { return *canvas_; }
  Rect screen() const { return canvas_->bounds(); }
  int now_ms() const { return now_ms_; }
  Device device() const { return device_; }
  bool large_text = false;  // informational: the host chose the canvas size with this
  int key_cancel = 27;      // RawKey codes that cancel / clear a key capture (SDLK_ESCAPE, SDLK_BACKSPACE)
  int key_clear = 8;

  Id id(std::string_view label) const;
  void push_id(std::string_view label);
  void pop_id();
  WState& state(Id id) { return states_[id]; }

  // Input of this pass.
  const Event& event() const { return ev_; }
  bool take_nav(Nav n);               // true, and consumed, if this pass's event is `n`
  bool peek_nav(Nav n) const;         // true if this pass's unconsumed event is `n`
  // The help line of the focused row (set by setting_row, drawn by the page).
  void set_help(std::string_view s) { help_.assign(s); }
  std::string_view help() const { return help_; }
  bool event_unconsumed() const { return !consumed_; }
  void consume() { consumed_ = true; }
  bool pointer_valid() const { return pointer_valid_; }
  int pointer_x() const { return pointer_x_; }
  int pointer_y() const { return pointer_y_; }
  bool pointer_down() const { return pointer_down_; }
  bool hovered(Rect r) const;  // pointer inside r, inside the clip, and r is not behind a modal
  bool blocked() const { return modal_id_ != 0 && !in_modal_; }

  // Focus.
  bool focusable(Id id, Rect r, FocusOpts opts = {});  // registers the item; true if it has the focus
  bool is_focused(Id id) const { return focus_ == id; }
  Id focused() const { return focus_; }
  void set_focus(Id id);
  Region focus_region() const;
  const std::vector<FocusItem>& items() const { return items_; }
  // Pure neighbour search over a registered item list (used by the tests for reachability).
  static int neighbor(const std::vector<FocusItem>& items, int from, Nav dir, bool modal_only);

  // Pointer clicks: press on the widget makes it active and focused; release over it is a click.
  bool click(Id id, Rect r);
  bool active(Id id) const { return active_ == id; }
  bool dragging(Id id) const { return active_ == id && pointer_down_; }

  // Scrolling areas. Build the content from view.y - offset downwards, pass its height to scroll_end.
  ScrollView scroll_begin(std::string_view label, Rect view);
  void scroll_end(ScrollView& sv, int content_h);

  // Modals: open_modal/close_modal from a handler, modal_begin/modal_end in the builder (last).
  void open_modal(std::string_view label) { modal_id_ = id(label); }
  void close_modal() { modal_id_ = 0; }
  bool modal_open() const { return modal_id_ != 0; }
  bool modal_begin(std::string_view label, std::string_view title, int w, int h, Rect* inner);
  void modal_end();

  // Pages: slide and fade on change; returns `area` shifted for the slide. Call end_page after drawing the page.
  Rect begin_page(Id page, Rect area);
  void end_page(Rect area);

  // Key capture (Controls page): while on, the next RawKey event is delivered to the widget that asked.
  void begin_capture(Id who) { capture_ = who; }
  void end_capture() { capture_ = 0; }
  bool capturing() const { return capture_ != 0; }
  Id capture_owner() const { return capture_; }

  // Something this pass draws depends on time (stripes, breathing ring, starfield).
  void animate() { if (!motion.reduced) animating_pass_ = true; }

 private:
  void pass(const Event& ev, bool has_event, int dt_ms, const Builder& b);
  void end_pass();
  bool eligible(const FocusItem& it) const { return modal_id_ == 0 || it.in_modal; }

  Canvas* canvas_ = nullptr;
  std::unordered_map<Id, WState> states_;
  std::vector<Id> id_stack_;
  std::vector<Id> scope_stack_;
  std::vector<FocusItem> items_;

  Event ev_;
  bool has_event_ = false;
  bool consumed_ = false;
  Device device_ = Device::Keyboard;
  int now_ms_ = 0;

  bool pointer_valid_ = false, pointer_down_ = false;
  int pointer_x_ = 0, pointer_y_ = 0;

  std::string help_;
  Id focus_ = 0;
  Id active_ = 0;
  Id modal_id_ = 0;
  bool in_modal_ = false;
  Id capture_ = 0;
  bool changed_ = false;      // something changed during this pass: run another
  bool focus_moved_ = false;  // focus moved at the end of the previous pass: scroll areas bring it into view
  bool focus_moved_now_ = false;

  Id page_id_ = 0;
  int page_started_ms_ = 0;
  int page_slide_ = 0, page_fade_ = 0;  // this pass
  bool animating_ = false, animating_pass_ = false;
};

}  // namespace x2::ui
