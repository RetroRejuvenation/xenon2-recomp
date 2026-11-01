// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "ui_core.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace x2::ui {

CanvasGeometry compute_geometry(int win_w, int win_h, bool large_text) {
  const int min_w = large_text ? 320 : 480;
  const int min_h = large_text ? 180 : 270;
  CanvasGeometry g;
  g.n = std::max(1, std::min(win_w / min_w, win_h / min_h));
  g.cw = std::max(1, win_w / g.n);
  g.ch = std::max(1, win_h / g.n);
  g.ox = (win_w - g.cw * g.n) / 2;
  g.oy = (win_h - g.ch * g.n) / 2;
  return g;
}

Context::Context() { id_stack_.push_back(0); }

Id Context::id(std::string_view label) const { return hash_id(id_stack_.back(), label); }
void Context::push_id(std::string_view label) { id_stack_.push_back(id(label)); }
void Context::pop_id() {
  if (id_stack_.size() > 1) id_stack_.pop_back();
}

bool Context::take_nav(Nav n) {
  if (!has_event_ || consumed_ || capturing() || ev_.type != Event::Type::NavKey || ev_.nav != n) return false;
  consumed_ = true;
  return true;
}

bool Context::peek_nav(Nav n) const {
  return has_event_ && !consumed_ && !capturing() && ev_.type == Event::Type::NavKey && ev_.nav == n;
}

bool Context::hovered(Rect r) const {
  return pointer_valid_ && !blocked() && r.contains(pointer_x_, pointer_y_) && canvas_->clip().contains(pointer_x_, pointer_y_);
}

bool Context::focusable(Id id, Rect r, FocusOpts o) {
  if (r.empty()) return focus_ == id;  // nothing to see or point at: not part of the focus graph
  FocusItem it;
  it.id = id;
  it.rect = r;
  it.region = o.region;
  it.selected = o.selected;
  it.in_modal = in_modal_;
  it.default_focus = o.default_focus;
  it.consumes_horizontal = o.consumes_horizontal;
  it.scope = scope_stack_.empty() ? 0 : scope_stack_.back();
  items_.push_back(it);
  if (!blocked() && has_event_ && ev_.type == Event::Type::PointerMove && focus_ != id && r.contains(ev_.x, ev_.y) &&
      canvas_->clip().contains(ev_.x, ev_.y)) {
    focus_ = id;
    changed_ = true;
  }
  return focus_ == id;
}

void Context::set_focus(Id id) {
  if (focus_ != id) {
    focus_ = id;
    changed_ = true;
    focus_moved_ = true;
  }
}

Region Context::focus_region() const {
  for (const FocusItem& it : items_)
    if (it.id == focus_) return it.region;
  return Region::Page;
}

bool Context::click(Id id, Rect r) {
  if (blocked() || !has_event_) return false;
  if (ev_.type == Event::Type::PointerDown && !consumed_ && r.contains(ev_.x, ev_.y) && canvas_->clip().contains(ev_.x, ev_.y)) {
    active_ = id;
    if (focus_ != id) {
      focus_ = id;
      changed_ = true;
    }
    consumed_ = true;
    return false;
  }
  if (ev_.type == Event::Type::PointerUp && active_ == id) {
    active_ = 0;
    changed_ = true;
    return r.contains(ev_.x, ev_.y);
  }
  return false;
}

namespace {
// Distance between two intervals on the cross axis; overlapping intervals only pay for the centre offset.
int cross_gap(int a0, int a1, int b0, int b1) {
  const int centre = std::abs((a0 + a1) / 2 - (b0 + b1) / 2) / 4;
  const int lo = std::max(a0, b0), hi = std::min(a1, b1);
  return (hi > lo ? 0 : lo - hi) + centre;
}
}  // namespace

int Context::neighbor(const std::vector<FocusItem>& items, int from, Nav dir, bool modal_only) {
  if (from < 0 || from >= static_cast<int>(items.size())) return -1;
  const Rect a = items[static_cast<size_t>(from)].rect;
  int best = -1;
  long long best_score = LLONG_MAX;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    if (i == from) continue;
    const FocusItem& it = items[static_cast<size_t>(i)];
    if (modal_only && !it.in_modal) continue;
    const Rect b = it.rect;
    bool ok = false;
    int primary = 0, cross = 0;
    switch (dir) {
      case Nav::Down:
        ok = b.cy() > a.cy();
        primary = std::max(0, b.y - a.bottom());
        cross = cross_gap(a.x, a.right(), b.x, b.right());
        break;
      case Nav::Up:
        ok = b.cy() < a.cy();
        primary = std::max(0, a.y - b.bottom());
        cross = cross_gap(a.x, a.right(), b.x, b.right());
        break;
      case Nav::Right:
        ok = b.cx() > a.cx();
        primary = std::max(0, b.x - a.right());
        cross = cross_gap(a.y, a.bottom(), b.y, b.bottom());
        break;
      case Nav::Left:
        ok = b.cx() < a.cx();
        primary = std::max(0, a.x - b.right());
        cross = cross_gap(a.y, a.bottom(), b.y, b.bottom());
        break;
      default:
        break;
    }
    if (!ok) continue;
    const long long score = static_cast<long long>(primary) * 2 + static_cast<long long>(cross) * 3;
    if (score < best_score) {
      best_score = score;
      best = i;
    }
  }
  return best;
}

ScrollView Context::scroll_begin(std::string_view label, Rect view) {
  ScrollView sv;
  sv.id = id(label);
  sv.view = view;
  WState& st = state(sv.id);
  if (has_event_ && !consumed_ && ev_.type == Event::Type::Scroll && !blocked() && view.contains(ev_.x, ev_.y) &&
      canvas_->clip().contains(ev_.x, ev_.y)) {
    st.scroll += ev_.dy * theme.m.row_h;
    consumed_ = true;
  }
  st.scroll = std::clamp(st.scroll, 0, std::max(0, st.content_h - view.h));
  sv.offset = st.scroll;
  sv.first_item = items_.size();
  scope_stack_.push_back(sv.id);
  canvas_->push_clip(view);
  return sv;
}

void Context::scroll_end(ScrollView& sv, int content_h) {
  canvas_->pop_clip();
  if (!scope_stack_.empty()) scope_stack_.pop_back();
  WState& st = state(sv.id);
  const int max_scroll = std::max(0, content_h - sv.view.h);
  if (content_h != st.content_h) changed_ = true;
  st.content_h = content_h;
  if (focus_moved_now_) {
    const int margin = std::min(8, sv.view.h / 4);
    for (size_t i = sv.first_item; i < items_.size(); ++i) {
      if (items_[i].id != focus_) continue;
      const Rect r = items_[i].rect;
      if (r.y < sv.view.y + margin)
        st.scroll -= (sv.view.y + margin) - r.y;
      else if (r.bottom() > sv.view.bottom() - margin)
        st.scroll += r.bottom() - (sv.view.bottom() - margin);
      break;
    }
  }
  st.scroll = std::clamp(st.scroll, 0, max_scroll);
  if (st.scroll != sv.offset) changed_ = true;
  if (max_scroll > 0) {
    // A thin scrollbar on the right edge: a track and a thumb sized by the visible fraction.
    const Palette& c = theme.c;
    const Rect track{sv.view.right() - 3, sv.view.y, 3, sv.view.h};
    canvas_->fill(track, c.raised);
    const int thumb_h = std::max(8, sv.view.h * sv.view.h / content_h);
    const int thumb_y = sv.view.y + (sv.view.h - thumb_h) * st.scroll / max_scroll;
    canvas_->fill({track.x, thumb_y, 3, thumb_h}, c.dim);
  }
}

bool Context::modal_begin(std::string_view label, std::string_view title, int w, int h, Rect* inner) {
  const Id mid = id(label);
  if (modal_id_ != mid) return false;
  in_modal_ = true;
  if (take_nav(Nav::Back)) {
    modal_id_ = 0;
    in_modal_ = false;
    changed_ = true;
    return false;
  }
  const Palette& c = theme.c;
  Canvas& cv = *canvas_;
  cv.blend(screen(), c.scrim, 190);
  w = std::min(w, cv.width() - 16);
  h = std::min(h, cv.height() - 16);
  const Rect box{(cv.width() - w) / 2, (cv.height() - h) / 2, w, h};
  cv.fill_chamfer(box, theme.m.chamfer, c.panel);
  cv.frame_chamfer(box, theme.m.chamfer, c.amber);
  Rect body = box.inset(theme.m.card_pad);
  draw_text(cv, Font::body(), body.x, body.y, title, c.amber);
  cut_top(body, 24);
  if (inner) *inner = body;
  return true;
}

void Context::modal_end() { in_modal_ = false; }

Rect Context::begin_page(Id page, Rect area) {
  if (page != page_id_) {
    page_id_ = page;
    page_started_ms_ = now_ms_;
  }
  page_slide_ = page_fade_ = 0;
  const int total = motion.page_ms();
  const int elapsed = now_ms_ - page_started_ms_;
  if (total > 0 && elapsed < total) {
    const int e = ease_permille(elapsed * 1000 / total);
    page_slide_ = 8 * (1000 - e) / 1000;
    page_fade_ = 255 * (1000 - e) / 1000;
    animating_pass_ = true;
  }
  return area.moved(page_slide_, 0);
}

void Context::end_page(Rect area) {
  if (page_fade_ > 0) canvas_->blend(area, theme.c.ground, page_fade_);
}

void Context::pass(const Event& ev, bool has, int dt_ms, const Builder& b) {
  ev_ = ev;
  has_event_ = has;
  consumed_ = false;
  changed_ = false;
  now_ms_ += dt_ms;
  items_.clear();
  id_stack_.assign(1, 0);
  scope_stack_.clear();
  in_modal_ = false;
  animating_pass_ = false;
  help_.clear();
  focus_moved_now_ = focus_moved_;
  focus_moved_ = false;
  if (has) {
    device_ = ev.device;
    switch (ev.type) {
      case Event::Type::PointerMove:
        pointer_valid_ = true;
        pointer_x_ = ev.x;
        pointer_y_ = ev.y;
        break;
      case Event::Type::PointerDown:
        pointer_valid_ = true;
        pointer_down_ = true;
        pointer_x_ = ev.x;
        pointer_y_ = ev.y;
        break;
      case Event::Type::PointerUp:
        pointer_valid_ = true;
        pointer_down_ = false;
        pointer_x_ = ev.x;
        pointer_y_ = ev.y;
        break;
      case Event::Type::Scroll:
        pointer_valid_ = true;
        pointer_x_ = ev.x;
        pointer_y_ = ev.y;
        break;
      default:
        break;
    }
  }
  canvas_->clear(theme.c.ground);
  b(*this);
  end_pass();
  animating_ = animating_pass_;
}

void Context::end_pass() {
  if (has_event_ && ev_.type == Event::Type::PointerUp) active_ = 0;

  auto index_of = [&](Id id) {
    for (size_t i = 0; i < items_.size(); ++i)
      if (items_[i].id == id) return static_cast<int>(i);
    return -1;
  };

  if (has_event_ && !consumed_ && ev_.type == Event::Type::NavKey && !capturing()) {
    const Nav n = ev_.nav;
    if (n == Nav::Up || n == Nav::Down || n == Nav::Left || n == Nav::Right) {
      const int from = index_of(focus_);
      const bool horizontal = n == Nav::Left || n == Nav::Right;
      if (from >= 0 && !(horizontal && items_[static_cast<size_t>(from)].consumes_horizontal)) {
        const int t = neighbor(items_, from, n, modal_id_ != 0);
        if (t >= 0) {
          focus_ = items_[static_cast<size_t>(t)].id;
          changed_ = focus_moved_ = true;
          consumed_ = true;
        }
      }
    } else if (n == Nav::Back && modal_id_ == 0) {
      const int cur = index_of(focus_);
      if (cur >= 0 && items_[static_cast<size_t>(cur)].region != Region::Rail) {
        int target = -1;
        for (size_t i = 0; i < items_.size(); ++i) {
          if (items_[i].region != Region::Rail) continue;
          if (target < 0 || items_[i].selected) target = static_cast<int>(i);
          if (items_[i].selected) break;
        }
        if (target >= 0) {
          focus_ = items_[static_cast<size_t>(target)].id;
          changed_ = focus_moved_ = true;
          consumed_ = true;
        }
      }
    } else if (n == Nav::First || n == Nav::Last) {
      int pick = -1;
      const int fi = index_of(focus_);
      const Id scope = fi >= 0 ? items_[static_cast<size_t>(fi)].scope : 0;
      for (size_t i = 0; i < items_.size(); ++i) {
        if (!eligible(items_[i]) || items_[i].region != Region::Page || items_[i].scope != scope) continue;
        if (n == Nav::First) {
          pick = static_cast<int>(i);
          break;
        }
        pick = static_cast<int>(i);
      }
      if (pick >= 0 && items_[static_cast<size_t>(pick)].id != focus_) {
        focus_ = items_[static_cast<size_t>(pick)].id;
        changed_ = focus_moved_ = true;
        consumed_ = true;
      }
    }
  }

  // There is always a focused control: if the old one is gone (page change, modal), pick a default.
  const int cur = index_of(focus_);
  if (cur < 0 || !eligible(items_[static_cast<size_t>(cur)])) {
    int pick = -1;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (!eligible(items_[i])) continue;
      if (items_[i].default_focus) {
        pick = static_cast<int>(i);
        break;
      }
      if (pick < 0 && items_[i].region == Region::Page) pick = static_cast<int>(i);
    }
    if (pick < 0)
      for (size_t i = 0; i < items_.size(); ++i)
        if (eligible(items_[i])) {
          pick = static_cast<int>(i);
          break;
        }
    if (pick >= 0) {
      focus_ = items_[static_cast<size_t>(pick)].id;
      changed_ = focus_moved_ = true;
    }
  }
}

bool Context::run(Canvas& canvas, const EventList& events, int dt_ms, const Builder& builder) {
  canvas_ = &canvas;
  if (events.empty()) {
    pass(Event{}, false, dt_ms, builder);
  } else {
    for (size_t i = 0; i < events.size(); ++i) pass(events[i], true, i == 0 ? dt_ms : 0, builder);
  }
  for (int k = 0; k < 4 && changed_; ++k) pass(Event{}, false, 0, builder);
  return animating_;
}

}  // namespace x2::ui
