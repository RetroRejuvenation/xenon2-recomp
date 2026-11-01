// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "packs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <span>
#include <utility>

#include "pack/pack_discovery.h"

namespace x2::ui {

namespace fs = std::filesystem;

namespace {

pack::GameFamily family_of(GameKind g) {
  switch (g) {
    case GameKind::Cdtv: return pack::GameFamily::Cdtv;
    case GameKind::St: return pack::GameFamily::St;
    default: return pack::GameFamily::Disk;
  }
}

std::string join_list(const std::vector<std::string>& v) {
  std::string r;
  for (const std::string& s : v) {
    if (!r.empty()) r += ';';
    r += s;
  }
  return r;
}

std::vector<std::string> dedup(const std::vector<std::string>& v) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const std::string& s : v)
    if (seen.insert(s).second) out.push_back(s);
  return out;
}

std::string file_name(const std::string& path) {
  const fs::path p(path);
  std::string n = p.filename().string();
  if (n.empty()) n = p.parent_path().filename().string();
  return n.empty() ? path : n;
}

std::string plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

std::string trimmed(std::string s) {
  auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && sp(s.back())) s.pop_back();
  size_t i = 0;
  while (i < s.size() && sp(s[i])) ++i;
  s.erase(0, i);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) s = s.substr(1, s.size() - 2);
  return s;
}

bool same_path(const std::string& a, const std::string& b) {
  std::error_code ec;
  const fs::path pa = fs::weakly_canonical(fs::path(a), ec);
  const fs::path pb = fs::weakly_canonical(fs::path(b), ec);
  return pa.lexically_normal() == pb.lexically_normal();
}

std::string message_text(const pack::PackMessage& m) {
  std::string t;
  if (!m.section.empty()) {
    t = m.section;
    if (!m.key.empty()) t += " \"" + m.key + "\"";
    t += ": ";
  }
  return t + m.text;
}

}  // namespace

// ---- the model ----

void PacksModel::note(Severity sev, std::string text) {
  notices.clear();
  notices.push_back({sev, std::move(text)});
}

void PacksModel::rescan() {
  discovered_ = pack::discover_packs(packs_dir);
  by_path_.clear();
  by_path_ok_.clear();
  ++scans_;
  scanned_ = true;
}

void PacksModel::sync(const SettingsModel& m) {
  if (!scanned_) rescan();
  const std::string key = m.s.packs_enabled + "|" + m.s.asset_packs + "|" + game_flag_value(m.game) + "|" + build + "|" + std::to_string(scans_);
  if (key == sync_key_) return;
  sync_key_ = key;
  const pack::PackContext ctx{family_of(m.game), build};
  rows_.clear();
  std::set<std::string> enabled_ids;
  for (const std::string& id : dedup(pack::split_list(m.s.packs_enabled))) {
    enabled_ids.insert(id);
    PackRow r;
    r.enabled = true;
    r.id = id;
    r.name = id;
    const auto it = std::find_if(discovered_.begin(), discovered_.end(), [&](const pack::PackInfo& p) { return p.id == id; });
    if (it == discovered_.end()) {
      r.kind = PackRow::Kind::Missing;
    } else {
      r.kind = PackRow::Kind::Folder;
      r.info = *it;
      r.inspected = true;
      r.path = it->path;
      r.name = it->name.empty() ? id : it->name;
      r.applies = it->scope.applies(ctx);
    }
    rows_.push_back(std::move(r));
  }
  for (const std::string& path : pack::split_list(m.s.asset_packs)) {
    PackRow r;
    r.kind = PackRow::Kind::Path;
    r.path = path;
    r.enabled = true;
    if (!by_path_ok_.count(path)) {
      pack::PackInfo info;
      by_path_ok_[path] = pack::inspect_pack(path, info);
      by_path_[path] = std::move(info);
    }
    r.inspected = by_path_ok_[path];
    r.info = by_path_[path];
    r.name = r.inspected && !r.info.name.empty() ? r.info.name : file_name(path);
    r.applies = !r.inspected || r.info.scope.applies(ctx);
    rows_.push_back(std::move(r));
  }
  for (const pack::PackInfo& p : discovered_) {
    if (enabled_ids.count(p.id)) continue;
    PackRow r;
    r.kind = PackRow::Kind::Folder;
    r.id = p.id;
    r.path = p.path;
    r.name = p.name.empty() ? p.id : p.name;
    r.info = p;
    r.inspected = true;
    r.enabled = false;
    r.applies = p.scope.applies(ctx);
    rows_.push_back(std::move(r));
  }
  int order = 0;
  for (PackRow& r : rows_)
    if (r.enabled && r.kind != PackRow::Kind::Missing) r.order = ++order;
  if (!follow_.empty()) {
    for (size_t i = 0; i < rows_.size(); ++i) {
      if ((!rows_[i].id.empty() && rows_[i].id == follow_) || (!rows_[i].path.empty() && rows_[i].path == follow_)) {
        selected = static_cast<int>(i);
        break;
      }
    }
    follow_.clear();
  }
  if (!rows_.empty()) selected = std::clamp(selected, 0, static_cast<int>(rows_.size()) - 1);
  else selected = 0;
}

const PackRow* PacksModel::current() const {
  if (selected < 0 || selected >= static_cast<int>(rows_.size())) return nullptr;
  return &rows_[static_cast<size_t>(selected)];
}

bool PacksModel::toggle(Settings& s, int row) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
  const PackRow r = rows_[static_cast<size_t>(row)];
  if (r.kind == PackRow::Kind::Path) {
    note(Severity::Info, "A pack added by path stays on while it is in the list. Use Remove to take it off.");
    return false;
  }
  std::vector<std::string> ids = dedup(pack::split_list(s.packs_enabled));
  const auto it = std::find(ids.begin(), ids.end(), r.id);
  if (it != ids.end()) {
    ids.erase(it);
    s.packs_enabled = join_list(ids);
    follow_ = r.id;
    note(Severity::Info, r.kind == PackRow::Kind::Missing ? "Forgot " + r.id + ", which is not in the packs folder." : r.name + " is off.");
    return true;
  }
  if (!r.inspected || !r.info.usable) {
    note(Severity::Error, r.name + " cannot be used: " + (r.info.problem.empty() ? std::string("it has no usable manifest.") : r.info.problem));
    return false;
  }
  ids.push_back(r.id);
  s.packs_enabled = join_list(ids);
  follow_ = r.id;
  note(Severity::Ok, r.name + " is on, loaded after the packs above it.");
  return true;
}

bool PacksModel::move(Settings& s, int row, int delta) {
  if (row < 0 || row >= static_cast<int>(rows_.size()) || delta == 0) return false;
  const PackRow r = rows_[static_cast<size_t>(row)];
  const bool in_folder = r.kind == PackRow::Kind::Missing || (r.kind == PackRow::Kind::Folder && r.enabled);
  if (!in_folder && r.kind != PackRow::Kind::Path) {
    note(Severity::Info, "Only packs that are on have a place in the load order. Turn it on first.");
    return false;
  }
  std::vector<std::string> list = in_folder ? dedup(pack::split_list(s.packs_enabled)) : pack::split_list(s.asset_packs);
  const std::string& id = in_folder ? r.id : r.path;
  const auto it = std::find(list.begin(), list.end(), id);
  if (it == list.end()) return false;
  const int pos = static_cast<int>(it - list.begin());
  const int to = pos + (delta < 0 ? -1 : 1);
  if (to < 0 || to >= static_cast<int>(list.size())) return false;
  std::swap(list[static_cast<size_t>(pos)], list[static_cast<size_t>(to)]);
  (in_folder ? s.packs_enabled : s.asset_packs) = join_list(list);
  follow_ = id;
  return true;
}

bool PacksModel::remove(Settings& s, int row) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
  const PackRow r = rows_[static_cast<size_t>(row)];
  if (r.kind != PackRow::Kind::Path) {
    note(Severity::Info, "Packs in the packs folder are taken off by turning them off, or by deleting them from the folder.");
    return false;
  }
  std::vector<std::string> list = pack::split_list(s.asset_packs);
  const auto it = std::find(list.begin(), list.end(), r.path);
  if (it == list.end()) return false;
  list.erase(it);
  s.asset_packs = join_list(list);
  note(Severity::Info, "Took " + r.name + " off the list. The files are untouched.");
  return true;
}

bool PacksModel::add_path(Settings& s, const std::string& raw) {
  const std::string path = trimmed(raw);
  if (path.empty()) return false;
  if (path.find(';') != std::string::npos) {
    note(Severity::Error, "A path with a semicolon cannot be kept in settings.cfg. Rename the folder or file first.");
    return false;
  }
  pack::PackInfo info;
  if (!pack::inspect_pack(path, info)) {
    note(Severity::Error, "That is not an asset pack: " + file_name(path) + " has no manifest.json. Choose a pack folder or a .zip file.");
    return false;
  }
  if (!info.usable) {
    note(Severity::Error, file_name(path) + " cannot be used: " + (info.problem.empty() ? std::string("its manifest is not usable.") : info.problem));
    return false;
  }
  // A pack that sits in the packs folder is enabled by its id, like the ones the folder scan finds.
  if (!packs_dir.empty() && same_path(fs::path(path).parent_path().string(), packs_dir)) {
    rescan();
    const std::string id = info.id;
    std::vector<std::string> ids = dedup(pack::split_list(s.packs_enabled));
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) {
      note(Severity::Info, info.name + " is already on.");
      return false;
    }
    ids.push_back(id);
    s.packs_enabled = join_list(ids);
    follow_ = id;
    note(Severity::Ok, info.name + " is on, loaded after the packs above it.");
    return true;
  }
  std::vector<std::string> list = pack::split_list(s.asset_packs);
  for (const std::string& p : list) {
    if (same_path(p, path)) {
      note(Severity::Info, info.name + " is already in the list.");
      return false;
    }
  }
  list.push_back(path);
  s.asset_packs = join_list(list);
  follow_ = path;
  note(Severity::Ok, "Added " + (info.name.empty() ? file_name(path) : info.name) + ", loaded after the other packs.");
  return true;
}

// ---- jobs ----

std::string PacksModel::latest_dump(GameKind game) const {
  return dumps_dir.empty() ? std::string() : latest_dump_dir(dumps_dir, game_flag_value(game));
}

bool PacksModel::start_check(const SettingsModel& m, int row) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
  const PackRow& r = rows_[static_cast<size_t>(row)];
  if (r.path.empty()) {
    note(Severity::Error, "Nothing to check: " + r.id + " is not in the packs folder.");
    return false;
  }
  pack::CheckOptions o;
  o.path = r.path;
  o.game = family_of(m.game);
  o.build = build;
  o.strict = strict;
  if (against_dump) o.against = latest_dump(m.game);
  if (!jobs_.start_check(o)) return false;
  notices.clear();
  return true;
}

bool PacksModel::start_dump(const SettingsModel& m) {
  if (exe_path.empty() || dumps_dir.empty()) {
    note(Severity::Error, "This copy of the program cannot start a dump: it does not know where it is or where to write.");
    return false;
  }
  DumpRequest r;
  r.exe = exe_path;
  r.game = game_flag_value(m.game);
  r.assets_dir = assets_dir;
  r.data_dir = data_dir;
  r.out_dir = next_dump_dir(dumps_dir, r.game);
  r.frames = dump_frames;
  r.audio = m.game != GameKind::St;
  if (!jobs_.start_dump(r, dump_runner ? dump_runner : DumpRunner(run_dump_process))) return false;
  dump_log.clear();
  return true;
}

void PacksModel::apply_check(const CheckDone& d) {
  PackCheckView v;
  v.against = d.against;
  v.exit_code = d.result.exit_code;
  v.entries = d.result.entries;
  v.skipped = d.result.skipped;
  v.errors = static_cast<int>(d.result.report.count(pack::Severity::Error));
  v.warnings = static_cast<int>(d.result.report.count(pack::Severity::Warning));
  const std::string label = d.result.label.empty() ? file_name(d.path) : d.result.label;
  if (v.exit_code == 3) v.messages.push_back({Severity::Error, label + " could not be read."});
  else if (v.errors > 0) v.messages.push_back({Severity::Error, label + ": " + plural(static_cast<size_t>(v.errors), "error", "errors") + ", " + plural(static_cast<size_t>(v.warnings), "warning", "warnings") + "."});
  else if (v.warnings > 0) v.messages.push_back({Severity::Warn, label + ": " + plural(static_cast<size_t>(v.warnings), "warning", "warnings") + "."});
  else v.messages.push_back({Severity::Ok, label + " is valid."});
  if (v.entries > 0 || v.skipped > 0)
    v.messages.push_back({Severity::Info, "Entries " + std::to_string(v.entries) + ", skipped " + std::to_string(v.skipped) + "."});
  size_t shown = 0;
  for (const pack::PackMessage& m : d.result.report.messages) {
    if (m.section == "against" && m.severity == pack::Severity::Info && m.text.rfind("coverage", 0) == 0) v.coverage = m.text;
    if (shown >= 200) continue;
    ++shown;
    const Severity sev = m.severity == pack::Severity::Error ? Severity::Error : m.severity == pack::Severity::Warning ? Severity::Warn : Severity::Info;
    v.messages.push_back({sev, message_text(m)});
  }
  if (d.result.report.messages.size() > shown)
    v.messages.push_back({Severity::Info, std::to_string(d.result.report.messages.size() - shown) + " more messages are not shown. Run --check-asset-pack for all of them."});
  checks_[d.path] = std::move(v);
}

void PacksModel::apply_dump(const DumpOutcome& o) {
  dump_log.clear();
  if (o.cancelled) {
    dump_log.push_back({Severity::Info, "The dump was cancelled. Nothing was kept."});
    return;
  }
  if (!o.ok) {
    dump_log.push_back({Severity::Error, o.error.empty() ? std::string("The dump failed.") : o.error});
    return;
  }
  dump_log.push_back({Severity::Ok, "Dump saved in " + file_name(o.out_dir) + ". It holds your own copy's graphics and sounds: use it to make a pack, never share it."});
  for (const std::string& l : o.lines) dump_log.push_back({l.find("warning") != std::string::npos ? Severity::Warn : Severity::Info, l});
}

void PacksModel::poll() {
  CheckDone c;
  if (jobs_.take_check(c)) apply_check(c);
  DumpOutcome d;
  if (jobs_.take_dump(d)) apply_dump(d);
}

const PackCheckView* PacksModel::check_view(const std::string& path) const {
  const auto it = checks_.find(path);
  return it == checks_.end() ? nullptr : &it->second;
}

std::string PacksModel::status_of(const PackRow& r, GameKind game) const {
  if (r.kind == PackRow::Kind::Missing) return "not found";
  if (!r.inspected || !r.info.usable) return "unusable";
  if (!r.applies) return std::string("not for ") + game_short_name(game);
  if (const PackCheckView* v = check_view(r.path)) {
    if (v->exit_code == 3 || v->errors > 0) return "errors";
    if (v->warnings > 0) return plural(static_cast<size_t>(v->warnings), "warning", "warnings");
    return "ok";
  }
  return "unchecked";
}

// ---- the page ----

namespace {

std::string scope_text(const pack::PackInfo& p) {
  if (p.scope.games == pack::GameScope::kAllGames && p.scope.builds.empty()) return "all";
  std::string s;
  for (int g = 0; g < 3; ++g) {
    if (!(p.scope.games & (1u << g))) continue;
    if (!s.empty()) s += ",";
    s += pack::game_name(static_cast<pack::GameFamily>(g));
  }
  return s.empty() ? std::string("none") : s;
}

std::string provides_text(const pack::PackInfo& p) {
  std::string s;
  auto add = [&](const char* w) {
    if (!s.empty()) s += " ";
    s += w;
  };
  if (p.provides_graphics) add("gfx");
  if (p.provides_screens) add("scr");
  if (p.provides_audio) add("snd");
  return s.empty() ? std::string("-") : s;
}

std::string padded(std::string s, size_t w) {
  if (s.size() > w) s = w > 3 ? s.substr(0, w - 3) + "..." : s.substr(0, w);
  s.resize(w, ' ');
  return s;
}

// One list line. Columns are dropped, last first, until the name has room: mark, order, name, status always; then version, games, what it provides.
std::string row_text(const PacksModel& pm, const PackRow& r, GameKind game, size_t chars) {
  const std::string status = pm.status_of(r, game);
  const bool bad = r.kind == PackRow::Kind::Missing || !r.inspected || !r.info.usable;
  const char* mark = r.kind == PackRow::Kind::Missing ? "[?]" : bad ? "[!]" : r.enabled ? "[x]" : "[ ]";
  const std::string idx = r.order > 0 ? std::to_string(r.order) : std::string(" ");
  const std::string ver = r.inspected && !r.info.pack_version.empty() ? "v" + r.info.pack_version : std::string("-");
  const std::string scope = r.inspected && r.info.usable ? scope_text(r.info) : std::string("-");
  const std::string prov = r.inspected && r.info.usable ? provides_text(r.info) : std::string("-");
  const size_t status_w = 11;
  size_t room = chars > 7 + 1 + status_w ? chars - (7 + 1 + status_w) : 0;  // what is left for the name and the optional columns
  const size_t kMinName = 14;
  bool with_ver = false, with_games = false, with_prov = false;
  if (room >= kMinName + 1 + 7) {
    with_ver = true;
    room -= 8;
  }
  if (room >= kMinName + 1 + 10) {
    with_games = true;
    room -= 11;
  }
  if (room >= kMinName + 1 + 7) {
    with_prov = true;
    room -= 8;
  }
  std::string t = std::string(mark) + " " + padded(idx, 2) + " " + padded(r.name, std::max<size_t>(room, 8));
  if (with_ver) t += " " + padded(ver, 7);
  if (with_games) t += " " + padded(scope, 10);
  if (with_prov) t += " " + padded(prov, 7);
  return t + " " + status;
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

// A button that also supplies its help line while focused.
bool action(Context& ctx, std::string_view label, Rect r, std::string_view help, bool enabled = true, bool primary = false, bool default_focus = false) {
  ButtonOpts o;
  o.enabled = enabled;
  o.primary = primary;
  o.default_focus = default_focus;
  const bool pressed = button(ctx, label, r, o);
  if (ctx.is_focused(ctx.id(label))) ctx.set_help(help);
  return pressed && enabled;
}

// A toggle row of a schema setting (Asset packs, Asset-pack audio).
bool schema_toggle(Context& ctx, SettingsModel& m, Stack& st, const char* key) {
  const SettingRow* r = find_row(key);
  if (!r) return false;
  const std::string reason = disabled_reason(*r, m.s, m.game);
  RowSpec spec;
  spec.label = r->label;
  spec.help = r->help;
  spec.enabled = reason.empty();
  spec.reason = reason;
  spec.badge = !applies_to(*r, m.game) ? "" : (r->effect == Effect::Restart ? "next start" : "");
  spec.badge_kind = BadgeKind::Dim;
  const Rect c = setting_row(ctx, st, r->key, spec, 120);
  bool v = get_bool(*r, m.s);
  if (toggle(ctx, r->key, c, v, spec.enabled)) {
    set_bool(*r, m.s, v);
    return true;
  }
  return false;
}

// A plain on/off row of the page's own (not a setting).
bool local_toggle(Context& ctx, Stack& st, const char* id, const char* label, const char* help, bool& value, bool enabled, const char* reason) {
  RowSpec spec;
  spec.label = label;
  spec.help = help;
  spec.enabled = enabled;
  spec.reason = reason;
  const Rect c = setting_row(ctx, st, id, spec, 120);
  return toggle(ctx, id, c, value, enabled);
}

void details(Context& ctx, Rect r, const PacksModel& pm, GameKind game) {
  const Theme& t = ctx.theme;
  panel(ctx, r);
  const Rect in = r.inset(4);
  const PackRow* row = pm.current();
  if (!row) {
    draw_paragraph(ctx, in, "No packs yet. Copy a pack folder or .zip into the packs folder and choose Rescan, or add one with Add a pack.", t.c.dim);
    return;
  }
  int y = in.y;
  const auto line = [&](std::string_view s, Color col) {
    draw_caption(ctx, {in.x, y, in.w, 10}, s, col);
    y += 11;
  };
  std::string head = row->name;
  if (row->inspected && !row->info.pack_version.empty()) head += "  v" + row->info.pack_version;
  if (row->inspected && !row->info.author.empty()) head += "  by " + row->info.author;
  draw_label(ctx, {in.x, y, in.w, 16}, head, t.c.text);
  y += 17;
  if (row->kind == PackRow::Kind::Missing) {
    line("The packs folder has no pack with this id. Turn it off to forget it.", t.c.warn);
    return;
  }
  if (!row->inspected || !row->info.usable) {
    line(!row->inspected ? "Not an asset pack: no manifest.json was found there." : "Cannot be used: " + row->info.problem, t.c.err);
    return;
  }
  const pack::PackInfo& p = row->info;
  line("Licence " + (p.license.empty() ? std::string("not stated") : p.license) + "   Games " + scope_text(p) +
           (p.scope.builds.empty() ? std::string() : "   Builds " + std::to_string(p.scope.builds.size())),
       t.c.dim);
  if (!p.credits.empty()) line("Credits " + p.credits, t.c.dim);
  else if (!p.description.empty()) line(p.description, t.c.dim);
  const PackCheckView* v = pm.check_view(row->path);
  std::string counts = "Entries " + std::to_string(v ? v->entries : p.entries) + ", skipped " + std::to_string(v ? v->skipped : p.skipped_entries);
  if (v && !v->coverage.empty()) counts = v->coverage;
  line(counts, t.c.dim);
  if (!row->applies) line(std::string("Its games and builds do not include ") + game_short_name(game) + ": nothing of it is used there.", t.c.warn);
}

}  // namespace

bool page_packs(Context& ctx, SettingsModel& m, PacksModel& pm, Rect body, Rect help) {
  const Theme& t = ctx.theme;
  pm.sync(m);
  pm.poll();
  bool changed = false;
  heading(ctx, body, "Asset packs", std::string("for: ") + game_flag_value(m.game));
  ScrollView sv = ctx.scroll_begin("packs", body);
  Stack st(body.x, sv.origin_y() + 4, body.w - 6, 0);

  changed |= schema_toggle(ctx, m, st, "upgrade");
  changed |= schema_toggle(ctx, m, st, "audio_upgrade");
  st.space(6);
  section_header(ctx, st.row(12), "Packs: later ones override earlier ones");
  st.space(2);

  // the list
  const Rect area = st.row(0);
  const int rows_n = static_cast<int>(pm.rows().size());
  const int vis = std::clamp(rows_n, 3, 6);
  const Rect lr{area.x + 4, st.y, area.w - 8, vis * t.m.row_h + 6};
  st.space(lr.h);
  std::vector<std::string> items;
  const size_t chars = static_cast<size_t>(std::max(20, (lr.w - 22) / 8));
  for (const PackRow& r : pm.rows()) items.push_back(row_text(pm, r, m.game, chars));
  if (items.empty()) items.push_back("(no packs)");
  int sel = pm.selected;
  ListOpts lo;
  lo.default_focus = true;
  const ListResult res = list(ctx, "pack_list", lr, items, sel, lo);
  if (rows_n > 0) pm.selected = sel;
  if (ctx.is_focused(ctx.id("pack_list"))) ctx.set_help("Enter turns it on or off. Top packs load first.");
  if (res.activated && rows_n > 0) changed |= pm.toggle(m.s, pm.selected);
  st.space(4);

  // what the selected pack says
  const Rect dr = st.row(70);
  details(ctx, {dr.x + 4, dr.y, dr.w - 8, dr.h}, pm, m.game);
  st.space(6);

  // buttons
  const bool idle = !pm.busy();
  const PackRow* cur = pm.current();
  const bool have = cur != nullptr;
  const bool is_path = have && cur->kind == PackRow::Kind::Path;
  const bool loaded = have && cur->enabled && cur->kind != PackRow::Kind::Missing;
  const Rect b1 = st.row(22);
  int x = b1.x + 4;
  const char* toggle_label = !have ? "Turn on" : cur->kind == PackRow::Kind::Missing ? "Forget" : cur->enabled ? "Turn off" : "Turn on";
  if (action(ctx, toggle_label, {x, b1.y, 92, 22}, "Turns the selected pack on or off. Changes apply at the next start.", have && !is_path)) changed |= pm.toggle(m.s, pm.selected);
  x += 100;
  if (action(ctx, "Move up", {x, b1.y, 76, 22}, "Loads the pack earlier, so packs below it override it.", loaded)) changed |= pm.move(m.s, pm.selected, -1);
  x += 84;
  if (action(ctx, "Move down", {x, b1.y, 92, 22}, "Loads the pack later, so it overrides the packs above it.", loaded)) changed |= pm.move(m.s, pm.selected, 1);
  x += 100;
  if (action(ctx, "Check pack", {x, b1.y, 96, 22}, "Runs the pack checker (--check-asset-pack) on the selected pack and shows what it finds.", idle && have && !(cur->kind == PackRow::Kind::Missing))) pm.start_check(m, pm.selected);
  st.space(4);
  const Rect b2 = st.row(22);
  x = b2.x + 4;
  if (action(ctx, "Add a pack...", {x, b2.y, 116, 22}, "Adds a pack folder or .zip from anywhere. You can also drop one on this window.", static_cast<bool>(pm.choose_pack))) {
    const std::string p = pm.choose_pack();
    if (!p.empty()) changed |= pm.add_path(m.s, p);
  }
  x += 124;
  if (action(ctx, "Remove", {x, b2.y, 76, 22}, "Takes a pack you added by path off the list. The files are not deleted.", is_path)) changed |= pm.remove(m.s, pm.selected);
  x += 84;
  if (action(ctx, "Rescan", {x, b2.y, 76, 22}, "Looks at the packs folder again, for packs you copied in.", idle)) {
    pm.rescan();
    pm.sync(m);
  }
  x += 84;
  if (action(ctx, "Open folder", {x, b2.y, 108, 22}, "Opens the packs folder in your file manager.", static_cast<bool>(pm.open_folder) && !pm.packs_dir.empty())) pm.open_folder(pm.packs_dir);
  st.space(4);
  if (!pm.notices.empty()) {
    const Rect nr = st.row(12);
    const Message& n = pm.notices.front();
    const Color col = n.severity == Severity::Error ? t.c.err : n.severity == Severity::Ok ? t.c.ok : t.c.dim;
    draw_caption(ctx, {nr.x + 4, nr.y, nr.w - 8, 10}, n.text, col);
  }
  st.space(6);

  // the check
  section_header(ctx, st.row(12), "Check");
  st.space(2);
  {
    const bool have_dump = !pm.latest_dump(m.game).empty();
    local_toggle(ctx, st, "check_strict", "Strict check", "Counts warnings as failures, like --strict.", pm.strict, true, "");
    if (!have_dump) pm.against_dump = false;
    local_toggle(ctx, st, "check_against", "Compare to dump", "Also lists keys that match nothing in your newest dump, and how much of it the pack covers (--against).", pm.against_dump,
                 have_dump, "Make a dump first (below).");
  }
  st.space(4);
  if (pm.jobs().checking()) {
    const Rect pr = st.row(12);
    draw_caption(ctx, {pr.x + 4, pr.y, pr.w - 8, 10}, "Checking the pack...", t.c.cyan);
    ctx.animate();
    st.space(2);
  }
  const PackCheckView* view = cur ? pm.check_view(cur->path) : nullptr;
  static const std::vector<Message> kNoReport = {{Severity::Info, "Choose Check pack to check the selected pack."}};
  message_list(ctx, "check_report", st.row(80), view ? std::span<const Message>(view->messages) : std::span<const Message>(kNoReport), false);
  st.space(8);

  // the dump
  section_header(ctx, st.row(12), "Dump my assets (for pack authors)");
  st.space(2);
  const Rect pr = st.row(0);
  const int ph = draw_paragraph(ctx, {pr.x + 4, st.y, pr.w - 8, 66},
                                "Plays the title and demo screens without a window and saves every picture and sound it sees, as a reference for making a pack. "
                                "The dump is your own copy's material: keep it to yourself and never put it in a pack you share.",
                                t.c.dim);
  st.space(ph * 11 + 4);
  const Rect db = st.row(22);
  if (pm.jobs().dumping()) {
    progress(ctx, {db.x + 4, db.y, db.w - 100, 22}, pm.jobs().permille(), pm.jobs().stage());
    if (action(ctx, "Cancel", {db.right() - 88, db.y, 84, 22}, "Stops the dump and removes what it wrote.")) pm.cancel();
    ctx.animate();
  } else {
    const bool can = idle && !pm.exe_path.empty() && !pm.dumps_dir.empty();
    if (action(ctx, "Dump my assets", {db.x + 4, db.y, 136, 22},
               "Runs a headless run of this program with --dump-assets. It takes about a minute; you can keep using this window.", can, false, false)) pm.start_dump(m);
    const std::string latest = pm.latest_dump(m.game);
    if (!latest.empty()) draw_caption(ctx, {db.x + 148, db.y + 6, db.w - 156, 10}, "Newest dump: " + file_name(latest), t.c.dim);
  }
  st.space(4);
  if (!pm.dump_log.empty()) message_list(ctx, "dump_log", st.row(48), pm.dump_log, false);
  ctx.scroll_end(sv, st.used() + 8);
  draw_caption(ctx, help, ctx.help(), t.c.dim);
  return changed;
}

}  // namespace x2::ui
