// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_manifest.h"

#include "pack/pack_paths.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace x2::pack {

// ---- versions and ids ---------------------------------------------------------------------------------------------------

namespace {

bool digits(const std::string& s, size_t& at, int& v) {
    const size_t b = at;
    long long x = 0;
    while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
        x = x * 10 + (s[at] - '0');
        if (x > 1000000000) return false;
        ++at;
    }
    v = int(x);
    return at > b;
}

bool name_char(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-'; }

} // namespace

bool parse_semver(const std::string& s, Semver& out) {
    size_t at = 0;
    Semver v;
    if (!digits(s, at, v.major) || at >= s.size() || s[at++] != '.') return false;
    if (!digits(s, at, v.minor) || at >= s.size() || s[at++] != '.') return false;
    if (!digits(s, at, v.patch)) return false;
    if (at < s.size()) {
        if (s[at] != '-' && s[at] != '+') return false;
        v.pre = s[at] == '-';
        ++at;
        if (at >= s.size()) return false;
        for (; at < s.size(); ++at)
            if (!name_char(s[at])) return false;
    }
    out = v;
    return true;
}

namespace {

// Operator and the x.y[.z] after it; `parts` is 2 or 3.
bool parse_constraint(const std::string& c, std::string& op, int v[3], int& parts) {
    size_t at = 0;
    op = ">=";
    if (c.compare(0, 2, ">=") == 0) at = 2;
    else if (c.compare(0, 1, ">") == 0) { op = ">"; at = 1; }
    else if (c.compare(0, 1, "=") == 0) { op = "="; at = 1; }
    v[2] = 0;
    if (!digits(c, at, v[0]) || at >= c.size() || c[at++] != '.' || !digits(c, at, v[1])) return false;
    parts = 2;
    if (at < c.size()) {
        if (c[at++] != '.' || !digits(c, at, v[2])) return false;
        parts = 3;
    }
    return at == c.size();
}

} // namespace

bool valid_constraint(const std::string& c) {
    std::string op;
    int v[3], parts;
    return parse_constraint(c, op, v, parts);
}

bool version_satisfies(const std::string& have, const std::string& constraint) {
    Semver h;
    std::string op;
    int v[3], parts = 0;
    if (!parse_semver(have, h) || !parse_constraint(constraint, op, v, parts)) return false;
    const int hv[3] = {h.major, h.minor, h.patch};
    if (op == "=") {
        if (hv[0] != v[0] || hv[1] != v[1]) return false;
        return parts == 2 || (hv[2] == v[2] && !h.pre);
    }
    int cmp = 0;  // have compared with the constraint
    for (int i = 0; i < 3 && !cmp; ++i) cmp = hv[i] < v[i] ? -1 : hv[i] > v[i] ? 1 : 0;
    if (!cmp && h.pre) cmp = -1;  // 1.0.0-rc1 comes before 1.0.0
    return op == ">=" ? cmp >= 0 : cmp > 0;
}

bool valid_pack_id(const std::string& id) {
    if (id.size() < 2 || id.size() > 64) return false;
    auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); };
    if (!alnum(id[0])) return false;
    for (char c : id)
        if (!alnum(c) && c != '.' && c != '_' && c != '-') return false;
    return true;
}

std::string slug_of(const std::string& name) {
    std::string r;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.' || c == '_';
        if (ok) r += c;
        else if (!r.empty() && r.back() != '-') r += '-';
    }
    while (!r.empty() && !((r.back() >= '0' && r.back() <= '9') || (r.back() >= 'a' && r.back() <= 'z'))) r.pop_back();
    if (r.size() > 64) r.resize(64);
    return valid_pack_id(r) ? r : "";
}

// ---- the parser -----------------------------------------------------------------------------------------------------------

namespace {

class Parser {
public:
    Parser(const std::string& label, const ParseOptions& opt, Manifest& m, PackReport& rep) : label_(label), opt_(opt), m_(m), rep_(rep) {}

    bool run(const Json& root, std::string& fatal);

private:
    // ---- messages
    void msg(Severity s, const std::string& section, const std::string& key, const std::string& text) { rep_.add(s, label_, section, key, text); }
    void skip(const std::string& section, const std::string& key, const std::string& text) {
        msg(Severity::Error, section, key, text);
        ++m_.skipped;
    }
    // Unknown and duplicate fields (validator mode). A dump writes descriptive fields into every entry, so the same field in many entries of a section is
    // reported once, at the first entry, with the count (flush_unknown, at the end).
    void unknown(const Json& o, std::initializer_list<const char*> known, const std::string& section, const std::string& key) {
        if (!opt_.validate) return;
        std::set<std::string> seen;
        for (const JsonMember& mem : o.o) {
            if (!seen.insert(mem.first).second) note_field("duplicate", mem.first, section, key);
            bool ok = false;
            for (const char* k : known) ok = ok || mem.first == k;
            if (!ok) note_field("unknown", mem.first, section, key);
        }
    }
    void note_field(const char* what, const std::string& field, const std::string& section, const std::string& key) {
        auto& e = fields_[{std::string(what), section, field}];
        if (e.count++ == 0) e.key = key;
    }
    void flush_unknown() {
        for (const auto& [id, e] : fields_) {
            const auto& [what, section, field] = id;
            const std::string text = what == "unknown" ? "unknown field \"" + field + "\" (ignored" : "duplicate field \"" + field + "\" (the last one is used";
            msg(Severity::Warning, section, e.key, text + (e.count > 1 ? "; also in " + std::to_string(e.count - 1) + " other entr" + (e.count == 2 ? "y" : "ies") + " of this section" : "") + ")");
        }
        fields_.clear();
    }
    bool room(const std::string& section) {
        if (++m_.entries <= opt_.limits.max_entries) return true;
        --m_.entries;
        if (!room_said_) {
            msg(Severity::Error, section, "", "more than " + std::to_string(opt_.limits.max_entries) + " entries: the rest are ignored");
            room_said_ = true;
        }
        return false;
    }

    // ---- field helpers: false = present but invalid (the caller reports it)
    static bool opt_int(const Json& o, const char* name, long long lo, long long hi, int& out, bool* present = nullptr) {
        const Json* j = o.get(name);
        if (present) *present = j != nullptr;
        return !j || j->as_int(lo, hi, out);
    }
    static bool opt_num(const Json& o, const char* name, double lo, double hi, double& out) {
        const Json* j = o.get(name);
        if (!j) return true;
        if (!j->is_num() || j->n < lo || j->n > hi) return false;
        out = j->n;
        return true;
    }
    static bool opt_bool(const Json& o, const char* name, bool& out) {
        const Json* j = o.get(name);
        if (!j) return true;
        if (j->t != Json::Bool) return false;
        out = j->b;
        return true;
    }
    static bool opt_anchor(const Json& o, const char* name, Anchor& out) {
        const Json* a = o.get(name);
        if (!a) return true;
        return a->is_arr() && a->a.size() == 2 && a->a[0].as_int(-4096, 4096, out.x) && a->a[1].as_int(-4096, 4096, out.y);
    }
    // A relative path string (syntax only here: existence is the loader's business). `why` set on failure.
    static bool path_field(const Json* j, std::string& out, std::string& why) {
        if (!j || !j->is_str()) { why = "must be a string"; return false; }
        if (j->s.empty()) { why = "is empty"; return false; }
        if (j->s.size() > 240) { why = "is longer than 240 bytes"; return false; }
        out = j->s;
        why.clear();
        return true;
    }
    bool parse_scale(const Json& o, int& scale, const std::string& section, const std::string& key) {
        if (!o.get("scale")) return true;
        if (opt_int(o, "scale", 1, 8, scale)) return true;
        skip(section, key, "\"scale\" must be 1-8");
        return false;
    }
    // games / builds of an entry. false = invalid (reported unless `quiet`).
    static bool parse_scope(const Json& o, GameScope& sc, std::string& why) {
        sc = GameScope();
        if (const Json* g = o.get("games")) {
            if (!g->is_arr() || g->a.empty()) { why = "\"games\" must be a non-empty list of disk, cdtv, st"; return false; }
            uint8_t bits = 0;
            for (const Json& e : g->a) {
                GameFamily f;
                if (!e.is_str() || !parse_game(e.s, f)) { why = "\"games\" must be a non-empty list of disk, cdtv, st"; return false; }
                if (bits >> unsigned(f) & 1u) { why = "\"games\" lists '" + e.s + "' twice"; return false; }
                bits = uint8_t(bits | (1u << unsigned(f)));
            }
            sc.games = bits;
        }
        if (const Json* b = o.get("builds")) {
            if (!b->is_arr() || b->a.empty()) { why = "\"builds\" must be a non-empty list of build tags"; return false; }
            for (const Json& e : b->a) {
                bool ok = e.is_str() && !e.s.empty() && e.s.size() <= 48 && ((e.s[0] >= 'a' && e.s[0] <= 'z') || (e.s[0] >= '0' && e.s[0] <= '9'));
                for (char c : ok ? e.s : std::string())
                    ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
                if (!ok) { why = "\"builds\" must be a non-empty list of build tags (lowercase letters, digits and '-')"; return false; }
                if (std::find(sc.builds.begin(), sc.builds.end(), e.s) != sc.builds.end()) { why = "\"builds\" lists '" + e.s + "' twice"; return false; }
                sc.builds.push_back(e.s);
            }
        }
        return true;
    }
    bool scope_of(const Json& o, DeclBase& d) {
        std::string why;
        if (parse_scope(o, d.scope, why)) return true;
        skip(d.section, d.key, why);
        return false;
    }
    static bool colour12(const std::string& s, uint16_t& v) {
        if (s.size() != 4 || s[0] != '#') return false;
        v = 0;
        for (size_t i = 1; i < 4; ++i) {
            const char c = char(s[i] | 0x20);
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return false;
            v = uint16_t(v << 4 | d);
        }
        return true;
    }
    static bool colour24(const std::string& s, uint32_t& abgr) {
        if (s.size() != 7 || s[0] != '#') return false;
        uint32_t v = 0;
        for (size_t i = 1; i < 7; ++i) {
            const char c = char(s[i] | 0x20);
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return false;
            v = v << 4 | uint32_t(d);
        }
        abgr = 0xFF000000u | (v >> 16) | (v & 0xFF00) | ((v & 0xFF) << 16);
        return true;
    }
    // "#rgb" (each digit x17) or "#rrggbb" -> 0x00RRGGBB (screen and panel `ref_palette`).
    static bool rgb_colour(const std::string& s, uint32_t& out) {
        if ((s.size() != 4 && s.size() != 7) || s[0] != '#') return false;
        uint32_t v = 0;
        for (size_t i = 1; i < s.size(); ++i) {
            const char c = char(s[i] | 0x20);
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return false;
            v = v << 4 | uint32_t(d);
        }
        out = s.size() == 4 ? (((v >> 8) & 15) * 17) << 16 | (((v >> 4) & 15) * 17) << 8 | ((v & 15) * 17) : v;
        return true;
    }
    static bool ref_palette_of(const Json& rp, std::vector<uint32_t>& out) {
        if (!rp.is_arr() || rp.a.size() < 2 || rp.a.size() > 32) return false;
        for (const Json& c : rp.a) {
            uint32_t v = 0;
            if (!c.is_str() || !rgb_colour(c.s, v)) return false;
            out.push_back(v);
        }
        return true;
    }
    static uint32_t rgb_of_12(uint16_t c) { return uint32_t((c >> 8 & 15) * 17) << 16 | uint32_t((c >> 4 & 15) * 17) << 8 | uint32_t((c & 15) * 17); }

    // ---- sections
    void metadata(const Json& root);
    void graphics(const Json& g);
    void far(const Json& f);
    void palettes(const Json& p);
    void backgrounds(const Json& b);
    void atlases(const Json& a);
    void groups(const Json& g);
    void stars(const Json& s);
    void screens(const Json& s);
    void panel(const Json& p);
    void audio(const Json& a);
    void clips(const Json& c, bool speech);
    void music(const Json& m);

    bool graphic_keys(const Json& e, const std::string& key, const std::string& section, std::vector<std::string>& keys);

    const std::string& label_;
    const ParseOptions& opt_;
    Manifest& m_;
    PackReport& rep_;
    bool room_said_ = false;
    struct FieldNote {
        size_t count = 0;
        std::string key;
    };
    std::map<std::tuple<std::string, std::string, std::string>, FieldNote> fields_;
};

void Parser::metadata(const Json& root) {
    auto text = [&](const char* name, size_t max, std::string& out) {
        const Json* j = root.get(name);
        if (!j) return;
        if (!j->is_str() || j->s.size() > max) { msg(Severity::Error, name, "", "must be a string of at most " + std::to_string(max) + " bytes (ignored)"); return; }
        out = j->s;
    };
    text("name", 80, m_.name);
    if (root.get("name") && m_.name.empty() && root.get("name")->is_str()) msg(Severity::Error, "name", "", "must not be empty (ignored)");
    text("description", 2000, m_.description);
    text("author", 200, m_.author);
    text("url", 400, m_.url);
    text("license", 200, m_.license);
    if (const Json* j = root.get("id")) {
        if (j->is_str() && valid_pack_id(j->s)) m_.id = j->s;
        else msg(Severity::Error, "id", "", "must match ^[a-z0-9][a-z0-9._-]{1,63}$ (the id is taken from the name instead)");
    }
    if (const Json* j = root.get("pack_version")) {
        Semver v;
        if (j->is_str() && parse_semver(j->s, v)) m_.pack_version = j->s;
        else msg(Severity::Error, "pack_version", "", "must be a version like 1.2.3 or 1.2.3-beta (ignored)");
    }
    if (const Json* j = root.get("preview")) {
        std::string why;
        if (path_field(j, m_.preview, why) && safe_pack_path(m_.preview)) {}
        else {
            msg(Severity::Error, "preview", "", why.empty() ? "unsafe path '" + m_.preview + "' (relative paths inside the pack only)" : "" + why);
            m_.preview.clear();
        }
    }
    if (const Json* j = root.get("credits")) {
        if (!j->is_arr() || j->a.size() > 200) msg(Severity::Error, "credits", "", "must be a list of at most 200 {name, role, url}");
        else
            for (size_t i = 0; i < j->a.size(); ++i) {
                const Json& c = j->a[i];
                const Json* n = c.is_obj() ? c.get("name") : nullptr;
                if (!n || !n->is_str()) { msg(Severity::Error, "credits[" + std::to_string(i) + "]", "", "needs a \"name\""); continue; }
                Credit cr;
                cr.name = n->s.substr(0, 200);
                if (const Json* r = c.get("role"); r && r->is_str()) cr.role = r->s.substr(0, 200);
                if (const Json* u = c.get("url"); u && u->is_str()) cr.url = u->s.substr(0, 400);
                m_.credits.push_back(std::move(cr));
                unknown(c, {"name", "role", "url"}, "credits[" + std::to_string(i) + "]", "");
            }
    }
    if (const Json* j = root.get("provides")) {
        if (!j->is_arr()) msg(Severity::Error, "provides", "", "must be a list of graphics, screens, audio");
        else
            for (const Json& e : j->a) {
                if (e.is_str() && (e.s == "graphics" || e.s == "screens" || e.s == "audio")) m_.provides.push_back(e.s);
                else msg(Severity::Error, "provides", "", "must be a list of graphics, screens, audio");
            }
    }
    if (const Json* r = root.get("requires")) {
        if (!r->is_obj()) msg(Severity::Error, "requires", "", "must be an object");
        else {
            if (const Json* x = r->get("x2recomp")) {
                if (x->is_str() && valid_constraint(x->s)) m_.requires_x2recomp = x->s;
                else msg(Severity::Error, "requires", "x2recomp", "must be a version like \">=0.5\" (ignored)");
            }
            if (const Json* ps = r->get("packs")) {
                if (!ps->is_arr() || ps->a.size() > 16) msg(Severity::Error, "requires", "packs", "must be a list of at most 16 {id, version}");
                else
                    for (const Json& p : ps->a) {
                        const Json* id = p.is_obj() ? p.get("id") : nullptr;
                        if (!id || !id->is_str() || !valid_pack_id(id->s)) { msg(Severity::Error, "requires", "packs", "each item needs a valid \"id\""); continue; }
                        Requirement q;
                        q.id = id->s;
                        if (const Json* v = p.get("version")) {
                            if (v->is_str() && valid_constraint(v->s)) q.version = v->s;
                            else { msg(Severity::Error, "requires", id->s, "\"version\" must be a version like \">=1.0\" (ignored)"); }
                        }
                        m_.requires_packs.push_back(std::move(q));
                    }
            }
            unknown(*r, {"x2recomp", "packs"}, "requires", "");
        }
    }
}

bool Parser::graphic_keys(const Json& e, const std::string& key, const std::string& section, std::vector<std::string>& keys) {
    keys.push_back(key);
    if (const Json* k = e.get("keys")) {
        if (!k->is_arr() || k->a.size() > 64) { skip(section, key, "\"keys\" must be a list of at most 64 graphic keys"); return false; }
        const bool flash = valid_flash_key(key);
        for (const Json& a : k->a) {
            if (!a.is_str() || !(flash ? valid_flash_key(a.s) : valid_graphic_key(a.s))) {
                skip(section, key, "\"keys\": '" + (a.is_str() ? a.s : std::string("?")) + "' is not a graphic key \"<16 hex digits>-<width>x<height>\"" + (flash ? " with the f: prefix" : ""));
                return false;
            }
            keys.push_back(a.s);
        }
    }
    return true;
}

void Parser::graphics(const Json& g) {
    if (!g.is_obj()) { skip("graphics", "", "must be an object"); return; }
    m_.has_graphics_section = true;
    for (const JsonMember& mem : g.o) {
        if (!room("graphics")) break;
        const std::string& key = mem.first;
        const Json& e = mem.second;
        if (!valid_graphic_key(key) && !valid_flash_key(key)) {
            --m_.entries;
            skip("graphics", key, "the key must be \"<16 hex digits>-<width>x<height>\" as written by --dump-assets");
            continue;
        }
        if (!e.is_obj()) { --m_.entries; skip("graphics", key, "must be an object"); continue; }
        GraphicDecl d;
        d.section = "graphics";
        d.key = key;
        d.scale = m_.scale;
        auto bad = [&]() { --m_.entries; };
        if (!parse_scale(e, d.scale, d.section, d.key)) { bad(); continue; }
        if (!opt_anchor(e, "anchor", d.anchor)) { bad(); skip(d.section, d.key, "\"anchor\" must be [x, y] in picture pixels"); continue; }
        if (!scope_of(e, d)) { bad(); continue; }
        if (!graphic_keys(e, key, d.section, d.keys)) { bad(); continue; }
        std::string why;
        if (!e.get("png")) { bad(); skip(d.section, d.key, "\"png\" missing"); continue; }
        if (!path_field(e.get("png"), d.png, why)) { bad(); skip(d.section, d.key, "\"png\" " + why); continue; }
        if (const Json* f = e.get("flash_png")) {
            if (!path_field(f, d.flash_png, why)) { bad(); skip(d.section, d.key, "\"flash_png\" " + why); continue; }
        }
        unknown(e, {"png", "scale", "anchor", "flash_png", "keys", "games", "builds"}, d.section, d.key);
        m_.graphics.push_back(std::move(d));
    }
}

void Parser::far(const Json& f) {
    if (!f.is_arr()) { skip("far", "", "must be an array"); return; }
    m_.has_graphics_section = true;
    for (size_t i = 0; i < f.a.size(); ++i) {
        const Json& e = f.a[i];
        FarDecl d;
        d.section = "far[" + std::to_string(i) + "]";
        d.scale = m_.scale;
        const Json* lines = e.is_obj() ? e.get("lines") : nullptr;
        if (!lines || !lines->is_arr()) { skip(d.section, "", "needs \"png\" and \"lines\""); continue; }
        if (!parse_scale(e, d.scale, d.section, "")) continue;
        if (!scope_of(e, d)) continue;
        std::string why;
        if (!e.get("png")) { skip(d.section, "", "\"png\" missing"); continue; }
        if (!path_field(e.get("png"), d.png, why)) { skip(d.section, "", "\"png\" " + why); continue; }
        d.lines.assign(lines->a.size(), 0);
        d.used.assign(lines->a.size(), false);
        for (size_t r = 0; r < lines->a.size(); ++r) {
            const Json& l = lines->a[r];
            if (l.is_str() && (l.s.empty() || l.s == "-")) continue; // a row left unused
            if (!l.is_str() || !is_hash_hex(l.s)) { msg(Severity::Error, d.section, "", "line " + std::to_string(r) + ": not a 16-digit hex key"); ++m_.skipped; continue; }
            if (!room("far")) break;
            d.lines[r] = std::strtoull(l.s.c_str(), nullptr, 16);
            d.used[r] = true;
        }
        unknown(e, {"png", "scale", "lines", "games", "builds"}, d.section, "");
        m_.far.push_back(std::move(d));
    }
}

void Parser::palettes(const Json& p) {
    if (!p.is_arr()) { skip("palettes", "", "must be an array"); return; }
    m_.has_graphics_section = true;
    static const char* const kLayerNames[] = {"other", "far", "tiles", "stars", "objects"};
    for (size_t i = 0; i < p.a.size(); ++i) {
        const Json& e = p.a[i];
        PaletteDecl d;
        d.section = "palettes[" + std::to_string(i) + "]";
        const Json* match = e.is_obj() ? e.get("match") : nullptr;
        const Json* colors = e.is_obj() ? e.get("colors") : nullptr;
        bool ok = match && colors && match->is_arr() && colors->is_arr() && match->a.size() == 16 && colors->a.size() == 16;
        for (int k = 0; ok && k < 16; ++k) {
            uint16_t v = 0;
            uint32_t c = 0;
            ok = match->a[size_t(k)].is_str() && colour12(match->a[size_t(k)].s, v) && colors->a[size_t(k)].is_str() && colour24(colors->a[size_t(k)].s, c);
            d.po.match[k] = v;
            d.po.rgba[k] = c;
        }
        if (!ok) { skip(d.section, "", "needs \"match\" (16 \"#rgb\" Amiga colours) and \"colors\" (16 \"#rrggbb\")"); continue; }
        if (const Json* l = e.get("layers")) {
            uint8_t bits = 0;
            bool good = l->is_arr();
            for (const Json& n : good ? l->a : std::vector<Json>{}) {
                int bit = -1;
                for (int k = 0; k < 5; ++k)
                    if (n.is_str() && n.s == kLayerNames[k]) bit = k;
                if (bit < 0) { good = false; break; }
                bits = uint8_t(bits | (1 << bit));
            }
            if (!good) { skip(d.section, "", "\"layers\": a list of other, far, tiles, stars, objects"); continue; }
            d.po.layers = bits;
        }
        if (!scope_of(e, d)) continue;
        if (!room("palettes")) break;
        unknown(e, {"match", "colors", "layers", "games", "builds"}, d.section, "");
        m_.palettes.push_back(std::move(d));
    }
}

void Parser::backgrounds(const Json& b) {
    if (!b.is_arr()) { skip("backgrounds", "", "must be an array"); return; }
    m_.has_graphics_section = true;
    for (size_t i = 0; i < b.a.size(); ++i) {
        const Json& e = b.a[i];
        BackgroundDecl d;
        d.section = "backgrounds[" + std::to_string(i) + "]";
        d.scale = m_.scale;
        if (!e.is_obj()) { skip(d.section, "", "must be an object"); continue; }
        if (!e.get("level") || !e.get("level")->as_int(1, 5, d.level)) { skip(d.section, "", "\"level\" must be 1-5"); continue; }
        d.key = "level" + std::to_string(d.level);
        if (!parse_scale(e, d.scale, d.section, "")) continue;
        if (!opt_int(e, "ring_lines", 1, 4096, d.ring_lines)) { skip(d.section, "", "\"ring_lines\" must be 1-4096"); continue; }
        if (!scope_of(e, d)) continue;
        std::string why;
        if (!e.get("png")) { skip(d.section, "", "\"png\" missing"); continue; }
        if (!path_field(e.get("png"), d.png, why)) { skip(d.section, "", "\"png\" " + why); continue; }
        if (!room("backgrounds")) break;
        unknown(e, {"level", "png", "scale", "ring_lines", "games", "builds"}, d.section, "");
        m_.backgrounds.push_back(std::move(d));
    }
}

void Parser::atlases(const Json& a) {
    if (!a.is_arr()) { skip("atlases", "", "must be an array"); return; }
    m_.has_graphics_section = true;
    for (size_t i = 0; i < a.a.size(); ++i) {
        const Json& e = a.a[i];
        AtlasDecl d;
        d.section = "atlases[" + std::to_string(i) + "]";
        d.scale = m_.scale;
        if (!e.is_obj()) { skip(d.section, "", "must be an object"); continue; }
        if (!parse_scale(e, d.scale, d.section, "")) continue;
        if (!scope_of(e, d)) continue;
        std::string why;
        if (!e.get("png")) { skip(d.section, "", "\"png\" missing"); continue; }
        if (!path_field(e.get("png"), d.png, why)) { skip(d.section, "", "\"png\" " + why); continue; }
        const Json* frames = e.get("frames");
        const Json* grid = e.get("grid");
        if (!frames && !grid) { skip(d.section, "", "needs \"frames\" or \"grid\""); continue; }
        bool good = true;
        if (frames) {
            if (!frames->is_arr()) { skip(d.section, "", "\"frames\" must be a list"); continue; }
            for (size_t k = 0; k < frames->a.size() && good; ++k) {
                const Json& f = frames->a[k];
                AtlasFrame fr;
                const Json* key = f.is_obj() ? f.get("key") : nullptr;
                const Json* rect = f.is_obj() ? f.get("rect") : nullptr;
                auto rect4 = [](const Json* r, int out[4]) {
                    if (!r || !r->is_arr() || r->a.size() != 4) return false;
                    for (int q = 0; q < 4; ++q)
                        if (!r->a[size_t(q)].as_int(0, 1 << 20, out[q])) return false;
                    return true;
                };
                if (!key || !key->is_str() || !valid_graphic_key(key->s)) { skip(d.section, "", "frame " + std::to_string(k) + ": \"key\" must be a graphic key \"<16 hex digits>-<width>x<height>\""); continue; }
                fr.key = key->s;
                if (!rect4(rect, fr.rect) || fr.rect[2] < 1 || fr.rect[3] < 1) { skip(d.section, fr.key, "\"rect\" must be [x, y, w, h] in atlas pixels"); continue; }
                if (!opt_anchor(f, "anchor", fr.anchor)) { skip(d.section, fr.key, "\"anchor\" must be [x, y] in picture pixels"); continue; }
                if (f.get("flash_rect")) {
                    if (!rect4(f.get("flash_rect"), fr.flash_rect) || fr.flash_rect[2] < 1 || fr.flash_rect[3] < 1) { skip(d.section, fr.key, "\"flash_rect\" must be [x, y, w, h] in atlas pixels"); continue; }
                    fr.has_flash = true;
                }
                if (!room("atlases")) { good = false; break; }
                unknown(f, {"key", "rect", "anchor", "flash_rect"}, d.section, fr.key);
                d.frames.push_back(std::move(fr));
            }
        }
        if (grid && good) {
            const Json* cell = grid->is_obj() ? grid->get("cell") : nullptr;
            const Json* keys = grid->is_obj() ? grid->get("keys") : nullptr;
            if (!cell || !cell->is_arr() || cell->a.size() != 2 || !cell->a[0].as_int(1, 1 << 20, d.cell_w) || !cell->a[1].as_int(1, 1 << 20, d.cell_h)) {
                skip(d.section, "", "grid: \"cell\" must be [width, height] in atlas pixels");
            } else if (!keys || !keys->is_arr()) {
                skip(d.section, "", "grid: \"keys\" must be a list of graphic keys or null");
            } else if (!opt_int(*grid, "columns", 1, 1 << 20, d.columns)) {
                skip(d.section, "", "grid: \"columns\" must be a positive integer");
            } else {
                d.has_grid = true;
                for (size_t k = 0; k < keys->a.size(); ++k) {
                    const Json& kk = keys->a[k];
                    if (kk.t == Json::Null) { d.grid_keys.push_back(""); continue; }
                    if (!kk.is_str() || !valid_graphic_key(kk.s)) {
                        skip(d.section, "", "grid: key " + std::to_string(k) + " must be a graphic key or null");
                        d.grid_keys.push_back("");
                        continue;
                    }
                    if (!room("atlases")) { good = false; break; }
                    d.grid_keys.push_back(kk.s);
                }
                unknown(*grid, {"cell", "columns", "keys"}, d.section + ".grid", "");
            }
        }
        unknown(e, {"png", "scale", "frames", "grid", "games", "builds"}, d.section, "");
        if (d.frames.empty() && !d.has_grid) continue;
        m_.atlases.push_back(std::move(d));
        if (!good) break;
    }
}

void Parser::groups(const Json& g) {
    if (!g.is_obj()) { skip("groups", "", "must be an object of key lists"); return; }
    for (const JsonMember& mem : g.o) {
        if (!mem.second.is_arr()) { skip("groups", mem.first, "must be a list of keys"); continue; }
        std::vector<std::string> keys;
        for (const Json& k : mem.second.a)
            if (k.is_str()) keys.push_back(k.s);
            else msg(Severity::Error, "groups", mem.first, "must be a list of keys");
        m_.groups.emplace_back(mem.first, std::move(keys));
    }
}

void Parser::stars(const Json& s) {
    if (!s.is_obj()) { skip("stars", "", "must be an object"); return; }
    m_.has_graphics_section = true;
    StarsDecl d;
    d.section = "stars";
    if (const Json* sh = s.get("shape")) {
        if (sh->is_str() && sh->s == "pixel") d.shape = StarsEntry::Shape::Pixel;
        else if (sh->is_str() && sh->s == "round") d.shape = StarsEntry::Shape::Round;
        else { skip("stars", "", "\"shape\" must be pixel or round"); return; }
    }
    if (!opt_num(s, "size", 0.25, 8.0, d.size)) { skip("stars", "", "\"size\" must be 0.25-8 (Amiga pixels)"); return; }
    if (!opt_num(s, "softness", 0.0, 1.0, d.softness)) { skip("stars", "", "\"softness\" must be 0-1"); return; }
    if (!scope_of(s, d)) return;
    if (!room("stars")) return;
    unknown(s, {"shape", "size", "softness", "games", "builds"}, "stars", "");
    m_.stars = std::move(d);
}

void Parser::screens(const Json& s) {
    if (!s.is_obj()) { skip("screens", "", "must be an object"); return; }
    m_.has_screens_section = true;
    size_t count = 0;
    for (const JsonMember& mem : s.o) {
        const std::string& name = mem.first;
        const Json& e = mem.second;
        if (++count > opt_.limits.max_screens) {
            skip("screens", name, "more than " + std::to_string(opt_.limits.max_screens) + " screens in one pack: the rest are ignored");
            break;
        }
        bool name_ok = !name.empty() && name.size() <= 48 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'));
        for (char c : name_ok ? name : std::string())
            name_ok = name_ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
        if (!name_ok) { skip("screens", name, "the name must be lowercase letters, digits, '_' and '-' (at most 48)"); continue; }
        if (!e.is_obj()) { skip("screens", name, "must be an object"); continue; }
        ScreenDecl d;
        d.section = "screens";
        d.key = name;
        d.scale = m_.scale;
        const Json* sig = e.get("signature");
        const Json* blocks = sig && sig->is_obj() ? sig->get("blocks") : nullptr;
        if (!sig || !sig->is_obj() || !blocks || !blocks->is_arr() || blocks->a.empty() || blocks->a.size() > 64) {
            skip(d.section, name, "\"signature\" needs \"blocks\" (1-64 entries of {x, y, hash}) and \"min\"");
            continue;
        }
        bool good = true;
        d.signature.block = 16;
        if (const Json* b = sig->get("block")) {
            int bv = 0;
            if (!b->as_int(8, 32, bv) || (bv != 8 && bv != 16 && bv != 32)) { skip(d.section, name, "signature \"block\" must be 8, 16 or 32"); continue; }
            d.signature.block = bv;
        }
        const Json* mn = sig->get("min");
        if (!mn || !mn->as_int(1, 64, d.signature.min)) { skip(d.section, name, "signature \"min\" must be 1-64"); continue; }
        for (size_t k = 0; k < blocks->a.size() && good; ++k) {
            const Json& b = blocks->a[k];
            SignatureBlock sb;
            const Json* h = b.is_obj() ? b.get("hash") : nullptr;
            if (!b.is_obj() || !b.get("x") || !b.get("y") || !b.get("x")->as_int(0, 4095, sb.x) || !b.get("y")->as_int(0, 4095, sb.y) || !h || !h->is_str() || !is_hash_hex(h->s)) {
                skip(d.section, name, "signature block " + std::to_string(k) + " needs \"x\", \"y\" (0-4095) and a 16-digit hex \"hash\"");
                good = false;
                break;
            }
            sb.hash = std::strtoull(h->s.c_str(), nullptr, 16);
            d.signature.blocks.push_back(sb);
            unknown(b, {"x", "y", "hash"}, "screens.signature.blocks", name);
        }
        if (!good) continue;
        if (d.signature.min > int(d.signature.blocks.size())) { skip(d.section, name, "signature \"min\" is larger than the number of blocks: the screen could never match"); continue; }
        unknown(*sig, {"block", "min", "blocks"}, "screens.signature", name);
        if (!parse_scale(e, d.scale, d.section, name)) continue;
        if (!opt_anchor(e, "anchor", d.anchor)) { skip(d.section, name, "\"anchor\" must be [x, y] in picture pixels"); continue; }
        if (const Json* rp = e.get("ref_palette")) {
            if (!ref_palette_of(*rp, d.ref_palette)) { skip(d.section, name, "\"ref_palette\" must be 2-32 colours (\"#rgb\" or \"#rrggbb\")"); continue; }
        }
        if (const Json* f = e.get("fade")) {
            if (f->is_str() && f->s == "scale") d.fade = FadeMode::Scale;
            else if (f->is_str() && f->s == "none") d.fade = FadeMode::None;
            else { skip(d.section, name, "\"fade\" must be scale or none"); continue; }
        }
        if (!opt_int(e, "bg_index", 0, 31, d.bg_index)) { skip(d.section, name, "\"bg_index\" must be 0-31"); continue; }
        if (const Json* dyn = e.get("dynamic")) {
            bool dok = dyn->is_arr() && dyn->a.size() <= 32;
            for (size_t k = 0; dok && k < dyn->a.size(); ++k) {
                const Json& r = dyn->a[k];
                Rect rc;
                dok = r.is_obj() && r.get("x") && r.get("y") && r.get("w") && r.get("h") && r.get("x")->as_int(0, 1 << 20, rc.x) &&
                      r.get("y")->as_int(0, 1 << 20, rc.y) && r.get("w")->as_int(1, 1 << 20, rc.w) && r.get("h")->as_int(1, 1 << 20, rc.h);
                if (dok) {
                    d.dynamic.push_back(rc);
                    unknown(r, {"x", "y", "w", "h"}, "screens.dynamic", name);
                }
            }
            if (!dok) { skip(d.section, name, "\"dynamic\" must be at most 32 rectangles {x, y, w, h} (w and h at least 1)"); continue; }
        }
        std::string why;
        if (!e.get("png")) { skip(d.section, name, "\"png\" missing"); continue; }
        if (!path_field(e.get("png"), d.png, why)) { skip(d.section, name, "\"png\" " + why); continue; }
        if (const Json* w = e.get("wide_png")) {
            if (!path_field(w, d.wide_png, why)) { skip(d.section, name, "\"wide_png\" " + why); continue; }
        }
        if (e.get("wide_scale") && !opt_int(e, "wide_scale", 1, 8, d.wide_scale)) { skip(d.section, name, "\"wide_scale\" must be 1-8"); continue; }
        if (!scope_of(e, d)) continue;
        if (!room("screens")) break;
        unknown(e, {"signature", "png", "scale", "anchor", "ref_palette", "fade", "bg_index", "dynamic", "wide_png", "wide_scale", "games", "builds"}, d.section, name);
        m_.screens.push_back(std::move(d));
    }
}

void Parser::panel(const Json& p) {
    if (!p.is_obj()) { skip("panel", "", "must be an object"); return; }
    m_.has_graphics_section = true;
    PanelDecl d;
    d.section = "panel";
    d.scale = m_.scale;
    if (!parse_scale(p, d.scale, "panel", "")) return;
    if (const Json* c = p.get("cell_w")) {
        if (!c->as_int(8, 16, d.cell_w) || (d.cell_w != 8 && d.cell_w != 16)) { skip("panel", "", "\"cell_w\" must be 8 or 16"); return; }
    }
    if (!opt_int(p, "max_span", 1, 8, d.max_span)) { skip("panel", "", "\"max_span\" must be 1-8"); return; }
    if (const Json* rp = p.get("ref_palette")) {
        if (!ref_palette_of(*rp, d.ref_palette)) { skip("panel", "", "\"ref_palette\" must be 2-32 colours (\"#rgb\" or \"#rrggbb\")"); return; }
    }
    if (const Json* f = p.get("fade")) {
        if (f->is_str() && f->s == "scale") d.fade = FadeMode::Scale;
        else if (f->is_str() && f->s == "none") d.fade = FadeMode::None;
        else { skip("panel", "", "\"fade\" must be scale or none"); return; }
        d.has_fade = true;
    }
    if (!scope_of(p, d)) return;
    if (const Json* b = p.get("background")) {
        std::string why;
        if (!b->is_obj() || !path_field(b->get("png"), d.background_png, why)) {
            skip("panel", "", "\"background\" needs a \"png\"" + (why.empty() ? std::string() : ": " + why));
            d.background_png.clear();
        } else if (!opt_anchor(*b, "anchor", d.background_anchor)) {
            skip("panel", "", "\"background\": \"anchor\" must be [x, y] in picture pixels");
            d.background_png.clear();
        }
        unknown(*b, {"png", "anchor"}, "panel.background", "");
    }
    if (const Json* cells = p.get("cells")) {
        if (!cells->is_obj()) { skip("panel", "", "\"cells\" must be an object"); }
        else
            for (const JsonMember& mem : cells->o) {
                PanelCellDecl c;
                c.section = "panel";
                c.key = mem.first;
                c.scale = d.scale;
                const Json& e = mem.second;
                if (!valid_panel_key(mem.first)) { skip("panel", mem.first, "the key must be \"<16 hex digits>-<cells 1-8>\""); continue; }
                if (!e.is_obj()) { skip("panel", mem.first, "must be an object"); continue; }
                if (!parse_scale(e, c.scale, "panel", mem.first)) continue;
                if (!opt_anchor(e, "anchor", c.anchor)) { skip("panel", mem.first, "\"anchor\" must be [x, y] in picture pixels"); continue; }
                if (!scope_of(e, c)) continue;
                std::string why;
                if (!e.get("png")) { skip("panel", mem.first, "\"png\" missing"); continue; }
                if (!path_field(e.get("png"), c.png, why)) { skip("panel", mem.first, "\"png\" " + why); continue; }
                c.keys.push_back(mem.first);
                if (const Json* k = e.get("keys")) {
                    bool kok = k->is_arr() && k->a.size() <= 64;
                    for (size_t q = 0; kok && q < k->a.size(); ++q) {
                        kok = k->a[q].is_str() && valid_panel_key(k->a[q].s);
                        if (kok) c.keys.push_back(k->a[q].s);
                    }
                    if (!kok) { skip("panel", mem.first, "\"keys\" must be a list of at most 64 panel keys"); continue; }
                }
                if (!room("panel")) break;
                unknown(e, {"png", "scale", "anchor", "keys", "games", "builds"}, "panel", mem.first);
                d.cells.push_back(std::move(c));
            }
    }
    unknown(p, {"scale", "cell_w", "max_span", "ref_palette", "fade", "background", "cells", "games", "builds"}, "panel", "");
    m_.panel = std::move(d);
}

void Parser::clips(const Json& c, bool speech) {
    const std::string section = speech ? "audio.speech" : "audio.sfx";
    if (!c.is_obj()) { skip(section, "", "must be an object"); return; }
    m_.has_audio_section = true;
    for (const JsonMember& mem : c.o) {
        const std::string& key = mem.first;
        const Json& e = mem.second;
        if (!valid_sample_key(key)) { skip(section, key, "the key must be \"<16 hex digits>-<sample bytes>\" as written by --dump-audio"); continue; }
        if (!e.is_obj()) { skip(section, key, "must be an object"); continue; }
        ClipDecl d;
        d.section = section;
        d.key = key;
        d.speech = speech;
        std::string why;
        if (!e.get("file")) { skip(section, key, "\"file\" missing"); continue; }
        if (!path_field(e.get("file"), d.file, why)) { skip(section, key, "\"file\" " + why); continue; }
        if (e.get("ref_period") && !opt_int(e, "ref_period", 124, 65535, d.ref_period)) { skip(section, key, "\"ref_period\" must be 124-65535"); continue; }
        double g = 0;
        if (!opt_num(e, "gain_db", -60, 24, g)) { skip(section, key, "\"gain_db\" must be -60 to 24"); continue; }
        d.gain_db = float(g);
        if (const Json* p = e.get("pan")) {
            if (p->is_str() && p->s == "channel") d.pan = PanMode::Channel;
            else if (p->is_num() && p->n >= -1 && p->n <= 1) { d.pan = PanMode::Position; d.pan_pos = float(p->n); }
            else { skip(section, key, "\"pan\" must be \"channel\" or a number from -1 (left) to 1 (right)"); continue; }
        }
        if (const Json* p = e.get("pitch")) {
            if (p->is_str() && p->s == "follow") d.pitch = PitchMode::Follow;
            else if (p->is_str() && p->s == "fixed") d.pitch = PitchMode::Fixed;
            else { skip(section, key, "\"pitch\" must be follow or fixed"); continue; }
        }
        if (const Json* p = e.get("cut")) {
            if (p->is_str() && p->s == "channel") d.cut = CutMode::Channel;
            else if (p->is_str() && p->s == "never") d.cut = CutMode::Never;
            else { skip(section, key, "\"cut\" must be channel or never"); continue; }
        }
        if (const Json* o = e.get("original")) {
            bool ook = false;
            if (o->is_str()) {
                if (o->s == "mute") { d.original = OriginalMode::Mute; ook = true; }
                else if (o->s == "mix") { d.original = OriginalMode::Mix; ook = true; }
                else if (o->s.rfind("mix:", 0) == 0) {
                    // mix:-12 or mix:-6.5 (up to two digits, one optional decimal)
                    const std::string v = o->s.substr(4);
                    size_t at = 0;
                    if (at < v.size() && v[at] == '-') ++at;
                    const size_t d0 = at;
                    while (at < v.size() && v[at] >= '0' && v[at] <= '9') ++at;
                    bool fine = at > d0 && at - d0 <= 2;
                    if (fine && at < v.size() && v[at] == '.') {
                        ++at;
                        fine = at < v.size() && v[at] >= '0' && v[at] <= '9';
                        if (fine) ++at;
                    }
                    if (fine && at == v.size()) {
                        d.original = OriginalMode::Mix;
                        d.original_mix_db = std::strtof(v.c_str(), nullptr);
                        ook = true;
                    }
                }
            }
            if (!ook) { skip(section, key, "\"original\" must be \"mute\", \"mix\" or \"mix:<dB>\" (for example \"mix:-12\")"); continue; }
        }
        if (!opt_int(e, "fade_out_ms", 0, 2000, d.fade_out_ms)) { skip(section, key, "\"fade_out_ms\" must be 0-2000"); continue; }
        if (!opt_bool(e, "loop", d.loop)) { skip(section, key, "\"loop\" must be true or false"); continue; }
        if (const Json* cf = e.get("cd0_file")) {
            bool cok = cf->is_str() && !cf->s.empty() && cf->s.size() <= 32;
            for (char ch : cok ? cf->s : std::string()) cok = cok && ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-');
            if (!cok) { skip(section, key, "\"cd0_file\" must be an upper-case disc file name such as SPEECH63"); continue; }
            d.cd0_file = cf->s;
            if (!speech) msg(Severity::Warning, section, key, "\"cd0_file\" only applies to speech (ignored)");
        }
        if (!scope_of(e, d)) continue;
        d.keys.push_back(key);
        if (const Json* k = e.get("keys")) {
            bool kok = k->is_arr() && k->a.size() <= 64;
            for (size_t q = 0; kok && q < k->a.size(); ++q) {
                kok = k->a[q].is_str() && valid_sample_key(k->a[q].s);
                if (kok) d.keys.push_back(k->a[q].s);
            }
            if (!kok) { skip(section, key, "\"keys\" must be a list of at most 64 sample keys"); continue; }
        }
        if (!room(section)) break;
        unknown(e, {"file", "ref_period", "gain_db", "pan", "pitch", "loop", "cut", "fade_out_ms", "original", "cd0_file", "keys", "games", "builds"}, section, key);
        m_.clips.push_back(std::move(d));
    }
}

void Parser::music(const Json& mu) {
    if (!mu.is_obj()) { skip("audio.music", "", "must be an object"); return; }
    m_.has_audio_section = true;
    auto base_fields = [&](const Json& o, std::optional<float>& gain, std::optional<int>& fi, std::optional<int>& fo, std::optional<int>& cf,
                           std::optional<Retrigger>& rt, std::optional<LoopMode>& lm, const std::string& section, const std::string& key) {
        if (o.get("gain_db")) {
            double g = 0;
            if (!opt_num(o, "gain_db", -60, 24, g)) { skip(section, key, "\"gain_db\" must be -60 to 24"); return false; }
            gain = float(g);
        }
        auto ms = [&](const char* name, long long hi, std::optional<int>& out) {
            if (!o.get(name)) return true;
            int v = 0;
            if (!opt_int(o, name, 0, hi, v)) { skip(section, key, std::string("\"") + name + "\" must be 0-" + std::to_string(hi)); return false; }
            out = v;
            return true;
        };
        if (!ms("fade_in_ms", 10000, fi) || !ms("fade_out_ms", 10000, fo) || !ms("crossfade_ms", 5000, cf)) return false;
        if (const Json* r = o.get("retrigger")) {
            if (r->is_str() && r->s == "restart") rt = Retrigger::Restart;
            else if (r->is_str() && r->s == "continue") rt = Retrigger::Continue;
            else { skip(section, key, "\"retrigger\" must be restart or continue"); return false; }
        }
        if (const Json* l = o.get("loop_mode")) {
            if (l->is_str() && l->s == "pack") lm = LoopMode::Pack;
            else if (l->is_str() && l->s == "game") lm = LoopMode::Game;
            else { skip(section, key, "\"loop_mode\" must be pack or game"); return false; }
        }
        return true;
    };
    if (const Json* d = mu.get("defaults")) {
        if (!d->is_obj()) skip("audio.music", "defaults", "must be an object");
        else {
            MusicDefaultsDecl dd;
            if (base_fields(*d, dd.gain_db, dd.fade_in_ms, dd.fade_out_ms, dd.crossfade_ms, dd.retrigger, dd.loop_mode, "audio.music", "defaults")) m_.music_defaults = dd;
            unknown(*d, {"gain_db", "fade_in_ms", "fade_out_ms", "crossfade_ms", "retrigger", "loop_mode"}, "audio.music", "defaults");
        }
    }
    if (const Json* sl = mu.get("slots")) {
        if (!sl->is_obj()) { skip("audio.music", "slots", "must be an object"); return; }
        for (const JsonMember& mem : sl->o) {
            const std::string& name = mem.first;
            const Json& e = mem.second;
            bool name_ok = name == "title" || name == "level" || (name.size() == 6 && name.compare(0, 5, "level") == 0 && name[5] >= '1' && name[5] <= '5');
            if (!name_ok && name.compare(0, 2, "cd") == 0) {
                const std::string n = name.substr(2);
                name_ok = n.size() >= 1 && n.size() <= 2 && n[0] != '0' && n.find_first_not_of("0123456789") == std::string::npos && std::atoi(n.c_str()) >= 2 && std::atoi(n.c_str()) <= 13;
            }
            if (!name_ok) { skip("audio.music.slots", name, "the slot must be title, level, level1-level5 or cd2-cd13"); continue; }
            if (!e.is_obj()) { skip("audio.music.slots", name, "must be an object"); continue; }
            MusicSlotDecl d;
            d.section = "audio.music.slots";
            d.key = name;
            d.slot.slot = name;
            std::string why;
            const Json* file = e.get("file");
            bool mute = false;
            if (!opt_bool(e, "mute", mute)) { skip(d.section, name, "\"mute\" must be true or false"); continue; }
            d.slot.mute = mute;
            if (file && file->t == Json::Null) d.file_null = true;
            else if (file) {
                if (!path_field(file, d.slot.file.path, why)) { skip(d.section, name, "\"file\" " + why); continue; }
            }
            if (d.file_null && !mute) { skip(d.section, name, "\"file\": null needs \"mute\": true"); continue; }
            if (!file && !e.get("mute")) { skip(d.section, name, "needs a \"file\" or \"mute\": true"); continue; }
            if (const Json* in = e.get("intro")) {
                if (!path_field(in, d.slot.intro.path, why)) { skip(d.section, name, "\"intro\" " + why); continue; }
            }
            if (const Json* lp = e.get("loop")) {
                if (lp->t == Json::Bool) d.slot.loop.kind = lp->b ? LoopKind::Whole : LoopKind::Off;
                else if (lp->is_obj()) {
                    double st = 0, en = -1;
                    if (!lp->get("start_s") || !opt_num(*lp, "start_s", 0, 1e9, st) || !lp->get("start_s")->is_num() ||
                        (lp->get("end_s") && (!opt_num(*lp, "end_s", 1e-9, 1e9, en) || en <= st))) {
                        skip(d.section, name, "\"loop\" {start_s, end_s}: start_s is seconds from 0, end_s is after start_s");
                        continue;
                    }
                    d.slot.loop.kind = LoopKind::Points;
                    d.slot.loop.start_s = st;
                    d.slot.loop.end_s = lp->get("end_s") ? en : -1.0;
                    unknown(*lp, {"start_s", "end_s"}, d.section + ".loop", name);
                } else { skip(d.section, name, "\"loop\" must be true, false or {start_s, end_s}"); continue; }
            }
            std::optional<float> g;
            std::optional<int> fi, fo, cf;
            std::optional<Retrigger> rt;
            std::optional<LoopMode> lm;
            if (!base_fields(e, g, fi, fo, cf, rt, lm, d.section, name)) continue;
            d.gain_db = g;
            d.slot.crossfade_ms = cf;
            d.slot.fade_in_ms = fi;
            d.slot.fade_out_ms = fo;
            d.slot.retrigger = rt;
            d.slot.loop_mode = lm;
            if (!scope_of(e, d)) continue;
            if (!room("audio.music.slots")) break;
            unknown(e, {"file", "intro", "loop", "mute", "gain_db", "fade_in_ms", "fade_out_ms", "crossfade_ms", "retrigger", "loop_mode", "games", "builds"}, d.section, name);
            m_.slots.push_back(std::move(d));
        }
    }
    unknown(mu, {"defaults", "slots"}, "audio.music", "");
}

void Parser::audio(const Json& a) {
    if (!a.is_obj()) { skip("audio", "", "must be an object"); return; }
    m_.has_audio_section = true;
    if (const Json* s = a.get("sfx")) clips(*s, false);
    if (const Json* s = a.get("speech")) clips(*s, true);
    if (const Json* mu = a.get("music")) music(*mu);
    unknown(a, {"sfx", "speech", "music"}, "audio", "");
}

bool Parser::run(const Json& root, std::string& fatal) {
    if (!root.is_obj()) { fatal = "manifest.json: the document must be a JSON object"; return false; }
    const Json* fmt = root.get("format");
    if (!fmt || !fmt->is_str() || fmt->s != "x2-asset-pack") { fatal = "manifest.json: \"format\" must be \"x2-asset-pack\""; return false; }
    const Json* ver = root.get("version");
    if (!ver || !ver->is_num() || ver->n != std::floor(ver->n) || ver->n < 1) {
        fatal = "manifest.json: unsupported \"version\" (this runtime reads versions 1 to " + std::to_string(kMaxManifestVersion) + ")";
        return false;
    }
    if (ver->n > kMaxManifestVersion) {
        char shown[400];  // the number as written: a version far outside any integer type must not be cast
        std::snprintf(shown, sizeof shown, "%.0f", ver->n);
        fatal = "manifest.json: \"version\" " + std::string(shown) + " needs a newer x2recomp (this one reads versions 1 to " + std::to_string(kMaxManifestVersion) + ")";
        return false;
    }
    m_.version = int(ver->n);
    if (root.get("scale") && !opt_int(root, "scale", 1, 8, m_.scale)) { fatal = "manifest.json: \"scale\" must be 1-8"; return false; }
    std::string why;
    if (!parse_scope(root, m_.scope, why)) { fatal = "manifest.json: " + why; return false; }
    metadata(root);

    auto v2_only = [&](const char* name, const std::function<void(const Json&)>& f) {
        const Json* s = root.get(name);
        if (!s) return;
        if (m_.version < 2) { skip(name, "", "needs \"version\": 2 in manifest.json (the section is ignored)"); return; }
        f(*s);
    };
    if (const Json* g = root.get("graphics")) graphics(*g);
    if (const Json* f = root.get("far")) far(*f);
    if (const Json* p = root.get("palettes")) palettes(*p);
    v2_only("backgrounds", [&](const Json& s) { backgrounds(s); });
    v2_only("atlases", [&](const Json& s) { atlases(s); });
    v2_only("groups", [&](const Json& s) { groups(s); });
    v2_only("stars", [&](const Json& s) { stars(s); });
    v2_only("screens", [&](const Json& s) { screens(s); });
    v2_only("panel", [&](const Json& s) { panel(s); });
    v2_only("audio", [&](const Json& s) { audio(s); });
    if (m_.version < 2 && root.get("requires")) { msg(Severity::Error, "requires", "", "needs \"version\": 2 in manifest.json (ignored)"); m_.requires_x2recomp.clear(); m_.requires_packs.clear(); }

    if (opt_.validate) {
        if (m_.license.empty()) msg(Severity::Warning, "license", "", "missing: say under which terms the pack may be used and shared (an error with --strict)");
        unknown(root, {"format", "version", "name", "id", "pack_version", "description", "author", "url", "license", "credits", "preview", "provides",
                       "games", "builds", "requires", "scale", "graphics", "far", "palettes", "backgrounds", "atlases", "groups", "stars", "screens", "panel", "audio"},
                "", "");
        flush_unknown();
    }
    return true;
}

} // namespace

bool parse_manifest(const Json& root, const std::string& label, const ParseOptions& opt, Manifest& out, PackReport& report, std::string& fatal) {
    out = Manifest();
    return Parser(label, opt, out, report).run(root, fatal);
}

} // namespace x2::pack
