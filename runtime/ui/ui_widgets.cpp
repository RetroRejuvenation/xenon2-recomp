// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "ui_widgets.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace x2::ui {

namespace {

const Font& body_font() { return Font::body(); }
const Font& cap_font() { return Font::caption(); }

int aligned_x(Rect r, int w, Align a) {
  switch (a) {
    case Align::Center:
      return r.x + (r.w - w) / 2;
    case Align::Right:
      return r.right() - w;
    default:
      return r.x;
  }
}

// A small right-pointing marker, 5 pixels wide and 9 high, centred on cy.
void marker(Canvas& c, int x, int cy, Color col) {
  for (int k = -4; k <= 4; ++k) c.fill({x, cy + k, 5 - std::abs(k), 1}, col);
}

// Disabled controls fade towards the panel; they are exempt from the contrast rules, and the reason is in the help line.
Color dis(const Theme& t) { return mix(t.c.panel, t.c.dim, 140); }

Color fill_for(const Theme& t, bool enabled, bool primary) {
  if (!enabled) return t.c.panel;
  return primary ? t.c.amber : t.c.raised;
}

}  // namespace

void draw_label(Context& ctx, Rect r, std::string_view s, Color col, Align a) {
  const std::string e = elide_text(s, r.w, 1);
  const int w = text_width(e);
  draw_text(ctx.canvas(), body_font(), aligned_x(r, w, a), r.y + (r.h - 16) / 2, e, col);
}

void draw_caption(Context& ctx, Rect r, std::string_view s, Color col, Align a) {
  const std::string e = elide_text(s, r.w, 1);
  const int w = text_width(e);
  draw_text(ctx.canvas(), cap_font(), aligned_x(r, w, a), r.y + (r.h - 8) / 2, e, col);
}

void draw_heading(Context& ctx, Rect r, std::string_view s, Color col) {
  const int adv = 17;  // 2x cell plus one pixel of letter spacing
  const int per = std::max(1, r.w / adv);
  std::string e = codepoint_count(s) > per ? elide_text(s, per * 8, 1) : std::string(s);
  draw_text(ctx.canvas(), body_font(), r.x, r.y + (r.h - 32) / 2, e, col, 2, 1);
}

int draw_paragraph(Context& ctx, Rect r, std::string_view s, Color col, int line_h) {
  const std::vector<std::string> lines = wrap_text(s, r.w, 1);
  int n = 0;
  for (const std::string& l : lines) {
    if ((n + 1) * line_h > r.h + (line_h - 8)) break;
    draw_text(ctx.canvas(), cap_font(), r.x, r.y + n * line_h, l, col);
    ++n;
  }
  return n;
}

void panel(Context& ctx, Rect r, bool raised) {
  Canvas& c = ctx.canvas();
  const Theme& t = ctx.theme;
  c.fill_chamfer(r, t.m.chamfer, raised ? t.c.raised : t.c.panel);
  c.frame_chamfer(r, t.m.chamfer, t.c.line);
}

void focus_ring(Context& ctx, Rect r, bool breathe) {
  Canvas& c = ctx.canvas();
  int extra = 0;
  if (breathe && !ctx.motion.reduced) {
    extra = (ctx.now_ms() % Motion::kBreatheMs) < Motion::kBreatheMs / 2 ? 0 : 1;
    ctx.animate();
  }
  c.frame(r.inset(-1 - extra), ctx.theme.c.ground, 1);   // the gap keeps the ring visible on any background
  c.frame(r.inset(-3 - extra), ctx.theme.c.amber, 2);
}

void section_header(Context& ctx, Rect r, std::string_view s) {
  const int w = text_width(s);
  draw_text(ctx.canvas(), cap_font(), r.x, r.y + (r.h - 8) / 2, s, ctx.theme.c.dim);
  if (r.w > w + 8) ctx.canvas().hline(r.x + w + 6, r.y + r.h / 2, r.w - w - 6, ctx.theme.c.line);
}

int badge_width(std::string_view s) { return text_width(s) + 8; }

int draw_badge(Context& ctx, int x, int y, std::string_view s, BadgeKind kind) {
  const Palette& p = ctx.theme.c;
  Color col = p.cyan;
  switch (kind) {
    case BadgeKind::Ok: col = p.ok; break;
    case BadgeKind::Warn: col = p.warn; break;
    case BadgeKind::Err: col = p.err; break;
    case BadgeKind::Dim: col = p.dim; break;
    default: break;
  }
  const int w = badge_width(s);
  Canvas& c = ctx.canvas();
  const Rect r{x, y, w, 12};
  c.fill_chamfer(r, 2, p.panel);
  c.frame_chamfer(r, 2, mix(p.panel, col, 110));
  draw_text(c, cap_font(), x + 4, y + 2, s, col);
  return w;
}

bool button(Context& ctx, std::string_view label, Rect r, ButtonOpts o) {
  const Id id = ctx.id(label);
  FocusOpts fo;
  fo.default_focus = o.default_focus;
  const bool focused = ctx.focusable(id, r, fo);
  const bool clicked = o.enabled && ctx.click(id, r);
  const bool accept = o.enabled && focused && ctx.take_nav(Nav::Accept);
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  const bool pressed = ctx.dragging(id) && ctx.hovered(r);
  c.fill_chamfer(r, t.m.chamfer, fill_for(t, o.enabled, o.primary));
  if (!o.primary || !o.enabled) c.frame_chamfer(r, t.m.chamfer, o.enabled ? t.c.line : mix(t.c.panel, t.c.line, 128));
  const Color tc = !o.enabled ? dis(t) : (o.primary ? t.c.on_amber : t.c.text);
  Rect tr = r.inset(4, 0);
  if (pressed) tr = tr.moved(0, 1);
  draw_label(ctx, tr, label, tc, Align::Center);
  if (focused) focus_ring(ctx, r);
  return clicked || accept;
}

bool toggle(Context& ctx, std::string_view id_str, Rect r, bool& value, bool enabled) {
  const Id id = ctx.id(id_str);
  FocusOpts fo;
  fo.consumes_horizontal = true;
  const bool focused = ctx.focusable(id, r, fo);
  bool changed = false;
  const bool clicked = enabled && ctx.click(id, r);
  if (enabled && (clicked || (focused && ctx.take_nav(Nav::Accept)))) {
    value = !value;
    changed = true;
  }
  if (focused && ctx.take_nav(Nav::Left) && enabled && value) {
    value = false;
    changed = true;
  }
  if (focused && ctx.take_nav(Nav::Right) && enabled && !value) {
    value = true;
    changed = true;
  }
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  if (value && enabled) {
    c.fill_chamfer(r, t.m.chamfer, t.c.amber);
    draw_label(ctx, r, "ON", t.c.on_amber, Align::Center);
  } else {
    c.fill_chamfer(r, t.m.chamfer, enabled ? t.c.raised : t.c.panel);
    c.frame_chamfer(r, t.m.chamfer, t.c.line);
    draw_label(ctx, r, value ? "ON" : "OFF", enabled ? t.c.dim : dis(t), Align::Center);
  }
  if (focused) focus_ring(ctx, r);
  return changed;
}

bool stepper(Context& ctx, std::string_view id_str, Rect r, int& value, int lo, int hi, int step, std::string_view text,
             bool enabled) {
  const Id id = ctx.id(id_str);
  FocusOpts fo;
  fo.consumes_horizontal = true;
  const bool focused = ctx.focusable(id, r, fo);
  const int old = value;
  const int bw = r.h;  // square - and + boxes
  if (enabled) {
    if (ctx.click(id, r)) {
      if (ctx.event().x < r.x + bw) value -= step;
      else if (ctx.event().x >= r.right() - bw) value += step;
    }
    if (focused && ctx.take_nav(Nav::Left)) value -= step;
    if (focused && ctx.take_nav(Nav::Right)) value += step;
    value = std::clamp(value, lo, hi);
  }
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  c.fill_chamfer(r, t.m.chamfer, enabled ? t.c.raised : t.c.panel);
  c.frame_chamfer(r, t.m.chamfer, t.c.line);
  const Color off = enabled ? t.c.dim : dis(t);
  const Color mc = (enabled && value > lo) ? t.c.text : off;
  const Color pc = (enabled && value < hi) ? t.c.text : off;
  draw_label(ctx, {r.x, r.y, bw, r.h}, "-", mc, Align::Center);
  draw_label(ctx, {r.right() - bw, r.y, bw, r.h}, "+", pc, Align::Center);
  draw_label(ctx, {r.x + bw, r.y, r.w - 2 * bw, r.h}, text, enabled ? t.c.text : off, Align::Center);
  if (focused) focus_ring(ctx, r);
  return value != old;
}

bool slider(Context& ctx, std::string_view id_str, Rect r, int& value, int lo, int hi, int step, std::string_view text,
            bool enabled) {
  const Id id = ctx.id(id_str);
  FocusOpts fo;
  fo.consumes_horizontal = true;
  const bool focused = ctx.focusable(id, r, fo);
  const int old = value;
  const int text_w = text.empty() ? 0 : text_width(text) + 8;
  const Rect track{r.x + 2, r.y + r.h / 2 - 2, r.w - text_w - 4, 4};
  if (enabled) {
    ctx.click(id, r);
    if (focused && ctx.take_nav(Nav::Left)) value -= step;
    if (focused && ctx.take_nav(Nav::Right)) value += step;
    if (ctx.dragging(id) && (ctx.event().type == Event::Type::PointerDown || ctx.event().type == Event::Type::PointerMove) &&
        track.w > 1) {
      const int px = std::clamp(ctx.pointer_x() - track.x, 0, track.w - 1);
      const long long span = hi - lo;
      long long v = lo + (static_cast<long long>(px) * span + (track.w - 1) / 2) / (track.w - 1);
      if (step > 1) v = lo + ((v - lo + step / 2) / step) * step;
      value = static_cast<int>(v);
    }
    value = std::clamp(value, lo, hi);
  }
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  c.fill(track, t.c.raised);
  c.frame(track.inset(-1), t.c.line, 1);
  const int fillw = hi > lo ? (track.w * (value - lo)) / (hi - lo) : 0;
  if (fillw > 0) c.fill({track.x, track.y, fillw, track.h}, enabled ? t.c.amber : dis(t));
  const int hx = track.x + std::clamp(fillw, 0, track.w) - 2;
  c.fill_chamfer({hx, r.y + 2, 5, r.h - 4}, 1, enabled ? t.c.text : dis(t));
  if (!text.empty()) draw_label(ctx, {r.right() - text_w + 4, r.y, text_w - 4, r.h}, text, enabled ? t.c.text : dis(t), Align::Right);
  if (focused) focus_ring(ctx, r);
  return value != old;
}

bool choice(Context& ctx, std::string_view id_str, Rect r, int& index, std::span<const std::string_view> items, bool enabled) {
  const Id id = ctx.id(id_str);
  FocusOpts fo;
  fo.consumes_horizontal = true;
  const bool focused = ctx.focusable(id, r, fo);
  const int n = static_cast<int>(items.size());
  const int old = index;
  if (n > 0) {
    index = std::clamp(index, 0, n - 1);
    if (enabled) {
      if (ctx.click(id, r)) {
        const int dir = ctx.event().x < r.cx() ? -1 : 1;
        index = (index + dir + n) % n;
      }
      if (focused && ctx.take_nav(Nav::Accept)) index = (index + 1) % n;
      if (focused && ctx.take_nav(Nav::Left)) index = (index + n - 1) % n;
      if (focused && ctx.take_nav(Nav::Right)) index = (index + 1) % n;
    }
  }
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  c.fill_chamfer(r, t.m.chamfer, enabled ? t.c.raised : t.c.panel);
  c.frame_chamfer(r, t.m.chamfer, t.c.line);
  const Color tc = enabled ? t.c.text : dis(t);
  const Color ac = (focused && enabled) ? t.c.amber : (enabled ? t.c.dim : dis(t));
  if (n > 0) draw_label(ctx, r.inset(14, 0), items[static_cast<size_t>(index)], tc, Align::Center);
  if (n > 1) {
    draw_text(c, body_font(), r.x + 3, r.y + (r.h - 16) / 2, "\xE2\x97\x80", ac);        // left-pointing triangle
    draw_text(c, body_font(), r.right() - 11, r.y + (r.h - 16) / 2, "\xE2\x96\xB6", ac);  // right-pointing triangle
  }
  if (focused) focus_ring(ctx, r);
  return index != old;
}

ListResult list(Context& ctx, std::string_view id_str, Rect r, std::span<const std::string> items, int& selected, ListOpts o) {
  ListResult res;
  const Id id = ctx.id(id_str);
  FocusOpts fo;
  fo.default_focus = o.default_focus;
  const bool focused = ctx.focusable(id, r, fo);
  WState& st = ctx.state(id);
  const int n = static_cast<int>(items.size());
  const int rh = ctx.theme.m.row_h;
  const Rect inner = r.inset(2);
  const int vis = std::max(1, inner.h / rh);
  if (n > 0) selected = std::clamp(selected, 0, n - 1);

  if (ctx.event().type == Event::Type::Scroll && ctx.event_unconsumed() && ctx.hovered(r)) {
    st.scroll += ctx.event().dy;
    ctx.consume();
  }
  if (focused && n > 0) {
    if (ctx.peek_nav(Nav::Up) && (selected > 0 || (o.wrap && n > 1))) {
      ctx.consume();
      selected = selected > 0 ? selected - 1 : n - 1;
      res.changed = true;
    } else if (ctx.peek_nav(Nav::Down) && (selected < n - 1 || (o.wrap && n > 1))) {
      ctx.consume();
      selected = selected < n - 1 ? selected + 1 : 0;
      res.changed = true;
    } else if (ctx.take_nav(Nav::Accept)) {
      res.activated = true;
    }
  }
  if (ctx.click(id, r) && n > 0) {
    const int row = st.scroll + (ctx.event().y - inner.y) / rh;
    if (row >= 0 && row < n) {
      res.changed = res.changed || row != selected;
      selected = row;
      if (o.activate_on_click) res.activated = true;
    }
  }
  if (res.changed) {  // keep the selection in view
    if (selected < st.scroll) st.scroll = selected;
    if (selected >= st.scroll + vis) st.scroll = selected - vis + 1;
  }
  st.scroll = std::clamp(st.scroll, 0, std::max(0, n - vis));

  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  panel(ctx, r);
  c.push_clip(inner);
  for (int i = st.scroll; i < n && i < st.scroll + vis; ++i) {
    const Rect row{inner.x, inner.y + (i - st.scroll) * rh, inner.w - (n > vis ? 4 : 0), rh};
    const bool sel = i == selected;
    if (sel) {
      c.fill(row, t.c.raised);
      marker(c, row.x + 3, row.cy(), t.c.amber);
    }
    draw_label(ctx, {row.x + 12, row.y, row.w - 14, row.h}, items[static_cast<size_t>(i)], sel ? t.c.amber : t.c.text);
  }
  if (n > vis) {
    const int thumb_h = std::max(8, inner.h * vis / n);
    const int thumb_y = inner.y + (inner.h - thumb_h) * st.scroll / (n - vis);
    c.fill({inner.right() - 3, inner.y, 3, inner.h}, t.c.raised);
    c.fill({inner.right() - 3, thumb_y, 3, thumb_h}, t.c.dim);
  }
  c.pop_clip();
  if (focused) focus_ring(ctx, r);
  return res;
}

void message_list(Context& ctx, std::string_view id_str, Rect r, std::span<const Message> items, bool follow) {
  const Id id = ctx.id(id_str);
  const bool focused = ctx.focusable(id, r);
  WState& st = ctx.state(id);  // aux: 0 = following the bottom, 1 = the user scrolled away
  const Id sid = ctx.id(std::string(id_str) + "/scroll");
  WState& ss = ctx.state(sid);
  const Rect view = r.inset(3);
  const int pitch = 11;
  const int max_prev = std::max(0, ss.content_h - view.h);
  if (follow && st.aux == 0) ss.scroll = max_prev;
  if (focused) {
    const int step = pitch * 3;
    if (ctx.peek_nav(Nav::Down) && ss.scroll < max_prev) {
      ctx.consume();
      ss.scroll = std::min(max_prev, ss.scroll + step);
    } else if (ctx.peek_nav(Nav::Up) && ss.scroll > 0) {
      ctx.consume();
      ss.scroll = std::max(0, ss.scroll - step);
    }
  }
  const Theme& t = ctx.theme;
  panel(ctx, r);
  ScrollView sv = ctx.scroll_begin(std::string(id_str) + "/scroll", view);
  st.aux = sv.offset >= max_prev ? 0 : 1;

  Canvas& c = ctx.canvas();
  int line = 0;
  for (const Message& m : items) {
    Color col = t.c.text;
    const char* mark = "\xE2\x80\xA2";  // bullet
    switch (m.severity) {
      case Severity::Ok: col = t.c.ok; mark = "\xE2\x9C\x93"; break;
      case Severity::Warn: col = t.c.warn; mark = "!"; break;
      case Severity::Error: col = t.c.err; mark = "\xC3\x97"; break;
      default: col = t.c.dim; break;
    }
    const std::vector<std::string> lines = wrap_text(m.text, view.w - 8 - 12, 1);
    for (size_t k = 0; k < lines.size(); ++k, ++line) {
      const int y = sv.origin_y() + 2 + line * pitch;
      if (y + 8 < view.y || y > view.bottom()) continue;
      if (k == 0) draw_text(c, cap_font(), view.x + 2, y, mark, col);
      draw_text(c, cap_font(), view.x + 14, y, lines[k], m.severity == Severity::Info ? t.c.text : col);
    }
  }
  ctx.scroll_end(sv, line * pitch + 4);
  if (focused) focus_ring(ctx, r);
}

void progress(Context& ctx, Rect r, int permille, std::string_view text) {
  permille = std::clamp(permille, 0, 1000);
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  c.fill_chamfer(r, 2, t.c.raised);
  c.frame_chamfer(r, 2, t.c.line);
  const Rect in = r.inset(2);
  const int fw = in.w * permille / 1000;
  const Rect fr{in.x, in.y, fw, in.h};
  if (fw > 0) {
    if (t.decor && !ctx.motion.reduced) {
      const int phase = (ctx.now_ms() / 80) & 1;
      c.dither(fr, t.c.amber, mix(t.c.amber, t.c.raised, 70), phase);
      ctx.animate();
    } else {
      c.fill(fr, t.c.amber);
    }
  }
  std::string label(text);
  if (label.empty()) label = std::to_string(permille / 10) + "%";
  const int tw = text_width(label);
  const int tx = r.cx() - tw / 2, ty = r.y + (r.h - 16) / 2;
  draw_text(c, body_font(), tx, ty, label, t.c.text);
  if (fw > 0) {
    c.push_clip(fr);
    draw_text(c, body_font(), tx, ty, label, t.c.on_amber);
    c.pop_clip();
  }
}

bool card(Context& ctx, std::string_view id_str, Rect r, const CardSpec& spec) {
  const Id id = ctx.id(id_str);
  FocusOpts fo;
  fo.default_focus = spec.default_focus;
  const bool focused = ctx.focusable(id, r, fo);
  const bool clicked = spec.enabled && ctx.click(id, r);
  const bool accept = spec.enabled && focused && ctx.take_nav(Nav::Accept);
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  const Color accent = spec.accent ? spec.accent : t.c.amber;
  const Color bg = focused ? t.c.raised : t.c.panel;
  c.fill_chamfer(r, t.m.chamfer, bg);
  if (spec.dashed) {
    for (int x = r.x + 4; x + 4 <= r.right() - 3; x += 7) {
      c.fill({x, r.y, 4, 1}, t.c.line);
      c.fill({x, r.bottom() - 1, 4, 1}, t.c.line);
    }
    for (int y = r.y + 4; y + 4 <= r.bottom() - 3; y += 7) {
      c.fill({r.x, y, 1, 4}, t.c.line);
      c.fill({r.right() - 1, y, 1, 4}, t.c.line);
    }
  } else {
    c.frame_chamfer(r, t.m.chamfer, t.c.line);
  }
  Rect in = r.inset(8);
  const int action_h = spec.action.empty() ? 0 : 20;
  Rect act = cut_bottom(in, action_h);
  if (action_h) cut_bottom(in, 6);
  const int emblem_h = std::max(24, in.h * 32 / 100);
  const Rect emb = cut_top(in, emblem_h);
  if (spec.dashed) {
    const Color pc = t.c.dim;
    c.fill({emb.cx() - 1, emb.cy() - 8, 3, 17}, pc);
    c.fill({emb.cx() - 8, emb.cy() - 1, 17, 3}, pc);
  } else {
    draw_emblem(c, emb, spec.emblem, accent, spec.seed ? spec.seed : id, t);
  }
  cut_top(in, 6);
  draw_label(ctx, cut_top(in, 16), spec.title, spec.enabled ? t.c.text : t.c.dim);
  cut_top(in, 2);
  if (!spec.subtitle.empty()) {
    const int lines = draw_paragraph(ctx, {in.x, in.y, in.w, 22}, spec.subtitle, t.c.dim);
    cut_top(in, lines * 11 + 2);
  }
  if (!spec.status.empty()) draw_badge(ctx, in.x, in.y, spec.status, BadgeKind::Info);
  if (action_h) {
    const bool hot = focused && spec.enabled;
    c.fill_chamfer(act, 2, hot ? accent : t.c.raised);
    if (!hot) c.frame_chamfer(act, 2, t.c.line);
    draw_label(ctx, act, spec.action, hot ? t.c.on_amber : (spec.enabled ? t.c.text : t.c.dim), Align::Center);
  }
  if (focused) focus_ring(ctx, r, true);
  return clicked || accept;
}

Rect page_chrome(Context& ctx, Rect area, std::span<const PageItem> pages, int& selected) {
  const int n = static_cast<int>(pages.size());
  if (n == 0) return area.inset(ctx.theme.m.margin);
  selected = std::clamp(selected, 0, n - 1);
  if (!ctx.blocked()) {
    if (ctx.take_nav(Nav::PagePrev)) selected = (selected + n - 1) % n;
    else if (ctx.take_nav(Nav::PageNext)) selected = (selected + 1) % n;
  }
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  const bool rail = ctx.screen().w >= 560;
  auto item_logic = [&](int i, Rect it) {
    const bool sel = i == selected;
    FocusOpts fo;
    fo.region = Region::Rail;
    fo.selected = sel;
    const Id id = ctx.id("page/" + std::string(pages[static_cast<size_t>(i)].label));
    bool focused = ctx.focusable(id, it, fo);
    if (ctx.click(id, it)) selected = i;
    if (focused && ctx.take_nav(Nav::Accept)) ctx.set_focus(0);  // into the page
    if (focused && !sel && ctx.device() != Device::Pointer) selected = i;  // focus follows selection
    return focused;
  };
  if (rail) {
    Rect rr = cut_left(area, 116);
    c.fill(rr, t.c.panel);
    c.vline(rr.right() - 1, rr.y, rr.h, t.c.line);
    int y = rr.y + 12;
    for (int i = 0; i < n; ++i, y += 26) {
      const Rect it{rr.x + 6, y, rr.w - 13, 24};
      const bool focused = item_logic(i, it);
      const bool sel = i == selected;
      if (sel) {
        c.fill(it, t.c.raised);
        c.fill({it.x, it.y, 3, it.h}, t.c.amber);
      }
      const Color col = sel ? t.c.amber : t.c.dim;
      draw_icon(c, pages[static_cast<size_t>(i)].icon, {it.x + 6, it.y + 4, 16, 16}, col, sel ? t.c.raised : t.c.panel);
      draw_label(ctx, {it.x + 26, it.y, it.w - 28, it.h}, pages[static_cast<size_t>(i)].label, col);
      if (focused) focus_ring(ctx, it);
    }
  } else {
    Rect strip = cut_top(area, 34);
    c.fill(strip, t.c.panel);
    c.hline(strip.x, strip.bottom() - 1, strip.w, t.c.line);
    for (int i = 0; i < n; ++i) {
      const int x0 = strip.x + strip.w * i / n, x1 = strip.x + strip.w * (i + 1) / n;
      const Rect it{x0 + 1, strip.y + 1, x1 - x0 - 2, strip.h - 3};
      const bool focused = item_logic(i, it);
      const bool sel = i == selected;
      if (sel) {
        c.fill(it, t.c.raised);
        c.fill({it.x, it.bottom() - 2, it.w, 2}, t.c.amber);
      }
      const Color col = sel ? t.c.amber : t.c.dim;
      draw_icon(c, pages[static_cast<size_t>(i)].icon, {it.cx() - 8, it.y + 3, 16, 16}, col, sel ? t.c.raised : t.c.panel);
      draw_caption(ctx, {it.x, it.y + 21, it.w, 10}, pages[static_cast<size_t>(i)].label, col, Align::Center);
      if (focused) focus_ring(ctx, it);
    }
  }
  return area.inset(t.m.margin);
}

Rect setting_row(Context& ctx, Stack& st, std::string_view control_id, const RowSpec& row, int control_w) {
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  const Rect r = st.row(t.m.row_h);
  const Id cid = ctx.id(control_id);
  const bool lifted = ctx.is_focused(cid);
  if (lifted) {
    c.fill_chamfer(r, 2, t.c.raised);
    marker(c, r.x + 3, r.cy(), t.c.amber);
    ctx.set_help(row.enabled || row.reason.empty() ? row.help : row.reason);
  }
  const int label_w = std::min(148, r.w * 36 / 100);
  const Color lc = row.enabled ? t.c.text : dis(t);
  draw_label(ctx, {r.x + 12, r.y, label_w - 4, r.h}, row.label, lc);
  const Rect ctl{r.x + 12 + label_w, r.y + 1, control_w, r.h - 2};
  int right = r.right() - 4;
  if (!row.hint.empty()) {
    const int hw = text_width(row.hint);
    draw_text(c, cap_font(), right - hw, r.y + (r.h - 8) / 2, row.hint, t.c.dim);
    right -= hw + 8;
  }
  if (!row.badge.empty()) {
    const int bw = badge_width(row.badge);
    if (right - bw >= ctl.right() + 6) draw_badge(ctx, right - bw, r.y + (r.h - 12) / 2, row.badge, row.badge_kind);
  }
  return ctl;
}

std::string_view nav_caption(Nav n, Device d, PadStyle p) {
  if (d == Device::Pad) {
    const bool ps = p == PadStyle::PlayStation, nin = p == PadStyle::Nintendo;
    switch (n) {
      case Nav::Accept: return ps ? "Cross" : (nin ? "B" : "A");
      case Nav::Back: return ps ? "Circle" : (nin ? "A" : "B");
      case Nav::Details: return ps ? "Triangle" : (nin ? "X" : "Y");
      case Nav::Default: return ps ? "Square" : (nin ? "Y" : "X");
      case Nav::PagePrev: return ps ? "L1" : (nin ? "L" : "LB");
      case Nav::PageNext: return ps ? "R1" : (nin ? "R" : "RB");
      case Nav::Menu: return ps ? "Options" : (nin ? "+" : "Start");
      case Nav::ToggleFooter: return ps ? "Share" : (nin ? "-" : "View");
      case Nav::Up: case Nav::Down: case Nav::Left: case Nav::Right: return "D-pad";
      default: return "";
    }
  }
  switch (n) {
    case Nav::Accept: return "Enter";
    case Nav::Back: return "Esc";
    case Nav::Details: return "I";
    case Nav::Default: return "Del";
    case Nav::PagePrev: return "PgUp";
    case Nav::PageNext: return "PgDn";
    case Nav::Menu: return "F1";
    case Nav::ToggleFooter: return "F2";
    case Nav::Up: case Nav::Down: case Nav::Left: case Nav::Right: return "Arrows";
    case Nav::First: return "Home";
    case Nav::Last: return "End";
    default: return "";
  }
}

void hint_bar(Context& ctx, Rect r, std::span<const Hint> hints) {
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  const Device dev = ctx.device() == Device::Pad ? Device::Pad : Device::Keyboard;
  int x = r.x;
  const int y = r.y + (r.h - 11) / 2;
  for (const Hint& h : hints) {
    const std::string_view cap = nav_caption(h.action, dev, ctx.pad_style);
    if (cap.empty()) continue;
    const int kw = text_width(cap) + 6;
    if (x + kw + 4 + text_width(h.label) > r.right()) break;
    const Rect key{x, y, kw, 11};
    c.fill_chamfer(key, 2, t.c.raised);
    c.frame_chamfer(key, 2, t.c.line);
    draw_text(c, cap_font(), x + 3, y + 2, cap, t.c.text);
    x += kw + 4;
    x = draw_text(c, cap_font(), x, y + 2, h.label, t.c.dim) + 12;
  }
}

bool command_footer(Context& ctx, Rect r, std::string_view command) {
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  panel(ctx, r, false);
  draw_caption(ctx, {r.x + 8, r.y + 3, r.w - 16, 10}, "Equivalent command line", t.c.dim);
  const Rect btn{r.right() - 56, r.y + 4, 52, r.h - 8};
  const Rect cmd{r.x + 8, r.y + 13, r.w - 8 - 60, 16};
  const std::string e = elide_text(command, cmd.w, 1, true);
  draw_text(c, body_font(), cmd.x, cmd.y, e, t.c.cyan);
  return button(ctx, "Copy", btn);
}

CaptureResult key_capture(Context& ctx, std::string_view id_str, Rect r, std::string_view current, int& key_out) {
  const Id id = ctx.id(id_str);
  FocusOpts fo;
  const bool focused = ctx.focusable(id, r, fo);
  CaptureResult res = CaptureResult::None;
  const bool capturing = ctx.capture_owner() == id;
  if (capturing) {
    const Event& e = ctx.event();
    if (e.type == Event::Type::RawKey && ctx.event_unconsumed()) {
      ctx.consume();
      if (e.key == ctx.key_cancel) res = CaptureResult::Cancelled;
      else if (e.key == ctx.key_clear) res = CaptureResult::Cleared;
      else {
        key_out = e.key;
        res = CaptureResult::Captured;
      }
      ctx.end_capture();
    } else if (e.type == Event::Type::PointerDown && !r.contains(e.x, e.y)) {
      res = CaptureResult::Cancelled;
      ctx.end_capture();
    }
  } else if (!ctx.capturing()) {
    if (ctx.click(id, r) || (focused && ctx.take_nav(Nav::Accept))) ctx.begin_capture(id);
  }
  const Theme& t = ctx.theme;
  Canvas& c = ctx.canvas();
  const bool now = ctx.capture_owner() == id;
  c.fill_chamfer(r, t.m.chamfer, now ? t.c.panel : t.c.raised);
  c.frame_chamfer(r, t.m.chamfer, now ? t.c.amber : t.c.line);
  draw_label(ctx, r.inset(4, 0), now ? "Press a key..." : current, now ? t.c.amber : t.c.text, Align::Center);
  if (focused) focus_ring(ctx, r);
  return res;
}

}  // namespace x2::ui
