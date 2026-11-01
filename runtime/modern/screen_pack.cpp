// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/screen_pack.h"

#include "modern/assets.h"
#include "modern/json_lite.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace x2::modern {

namespace fs = std::filesystem;
using jl::Json;

namespace {

constexpr size_t kMaxManifest = 16u << 20;
constexpr size_t kMaxPng = 32u << 20;

bool read_file_bytes(const fs::path& p, size_t limit, std::vector<uint8_t>& out, std::string& why) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(p, ec);
    if (ec) { why = "cannot read"; return false; }
    if (size > limit) { why = "too large"; return false; }
    std::ifstream in(p, std::ios::binary);
    out.resize(size_t(size));
    if (!in || !in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size))) { why = "cannot read"; return false; }
    return true;
}

bool int_in(const Json* j, int lo, int hi, int& out) {
    if (!j || j->t != Json::Num || j->n != std::floor(j->n) || j->n < lo || j->n > hi) return false;
    out = int(j->n);
    return true;
}

bool parse_hex16(const std::string& s, uint64_t& out) {
    if (!pack::is_hash_hex(s)) return false;
    out = 0;
    for (char c : s) out = out << 4 | uint64_t(c <= '9' ? c - '0' : c - 'a' + 10);
    return true;
}

// "#rgb" (each digit x17) or "#rrggbb" -> 0x00RRGGBB.
bool parse_rgb(const std::string& s, uint32_t& out) {
    if ((s.size() != 4 && s.size() != 7) || s[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = char(s[i] | 0x20);
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | uint32_t(d);
    }
    if (s.size() == 4) v = (((v >> 8) & 15) * 17) << 16 | (((v >> 4) & 15) * 17) << 8 | ((v & 15) * 17);
    out = v;
    return true;
}

bool read_scope(const Json* o, pack::GameScope& sc) {
    sc = {};
    if (!o) return true;
    if (const Json* g = o->get("games")) {
        if (g->t != Json::Arr || g->a.empty()) return false;
        sc.games = 0;
        for (const Json& e : g->a) {
            pack::GameFamily f;
            if (e.t != Json::Str || !pack::parse_game(e.s, f)) return false;
            sc.games |= uint8_t(1u << unsigned(f));
        }
    }
    if (const Json* b = o->get("builds")) {
        if (b->t != Json::Arr || b->a.empty()) return false;
        for (const Json& e : b->a) {
            if (e.t != Json::Str) return false;
            sc.builds.push_back(e.s);
        }
    }
    return true;
}

bool read_anchor(const Json* j, pack::Anchor& a) {
    if (!j) return true;
    return j->t == Json::Arr && j->a.size() == 2 && int_in(&j->a[0], -4096, 4096, a.x) && int_in(&j->a[1], -4096, 4096, a.y);
}

} // namespace

const pack::Picture* ScreenPackLibrary::add_picture(pack::Picture p) {
    pictures_.push_back(std::move(p));
    return &pictures_.back();
}

const pack::ScreenEntry* ScreenPackLibrary::add_screen(pack::ScreenEntry e) {
    by_block_valid_ = false;
    for (const pack::ScreenEntry*& s : screens_)
        if (s->name == e.name) {
            entries_.push_back(std::move(e));
            s = &entries_.back();
            return s;
        }
    entries_.push_back(std::move(e));
    screens_.push_back(&entries_.back());
    return screens_.back();
}

const std::vector<const pack::ScreenEntry*>& ScreenPackLibrary::screens_for_block(uint64_t first_block_hash) const {
    if (!by_block_valid_) {
        by_block_.clear();
        for (const pack::ScreenEntry* e : screens_)
            if (!e->signature.blocks.empty()) by_block_[e->signature.blocks[0].hash].push_back(e);
        by_block_valid_ = true;
    }
    static const std::vector<const pack::ScreenEntry*> none;
    auto it = by_block_.find(first_block_hash);
    return it == by_block_.end() ? none : it->second;
}

bool ScreenPackLibrary::load(const std::string& dir, pack::PackReport& report) {
    const std::string pname = fs::path(dir).filename().string();
    auto err = [&](const std::string& section, const std::string& key, const std::string& text) {
        report.add(pack::Severity::Error, pname, section, key, text);
    };
    auto warn = [&](const std::string& section, const std::string& key, const std::string& text) {
        report.add(pack::Severity::Warning, pname, section, key, text);
    };
    std::vector<uint8_t> raw;
    std::string why;
    if (!read_file_bytes(fs::path(dir) / "manifest.json", kMaxManifest, raw, why)) {
        err("manifest", "", "manifest.json: " + why);
        return false;
    }
    Json m;
    {
        const std::string text(raw.begin(), raw.end());
        jl::JsonParser parser(text);
        if (!parser.parse(m, why) || m.t != Json::Obj) {
            err("manifest", "", "manifest.json: " + (why.empty() ? std::string("not an object") : why));
            return false;
        }
    }
    const Json* fmt = m.get("format");
    int version = 0;
    if (!fmt || fmt->t != Json::Str || fmt->s != "x2-asset-pack" || !int_in(m.get("version"), 1, 2, version)) {
        err("manifest", "", "not an x2-asset-pack manifest of version 1 or 2");
        return false;
    }
    pack::GameScope pack_scope;
    if (!read_scope(&m, pack_scope)) {
        err("manifest", "", "bad games/builds");
        return false;
    }
    if (!pack_scope.applies(ctx_)) {
        report.add(pack::Severity::Info, pname, "manifest", "", "does not apply to this game or build; skipped");
        return false;
    }
    int pack_scale = 1;
    if (const Json* s = m.get("scale"))
        if (!int_in(s, 1, 8, pack_scale)) { err("manifest", "", "bad scale"); return false; }

    const size_t first_info = infos_.size();
    pack::PackInfo info;
    info.path = dir;
    info.format_version = version;
    info.scope = pack_scope;
    if (const Json* n = m.get("name"); n && n->t == Json::Str) info.name = n->s;
    if (const Json* n = m.get("id"); n && n->t == Json::Str) info.id = n->s;

    auto load_png = [&](const std::string& section, const std::string& key, const Json* rel, const pack::Picture*& out) -> bool {
        if (!rel || rel->t != Json::Str || !safe_pack_path(rel->s)) {
            err(section, key, "missing or unsafe \"png\" path");
            return false;
        }
        std::vector<uint8_t> file;
        std::string w;
        if (!read_file_bytes(fs::path(dir) / fs::path(rel->s), kMaxPng, file, w)) {
            err(section, key, rel->s + ": " + w);
            return false;
        }
        Image img;
        if (!decode_png(file, img, w)) {
            err(section, key, rel->s + ": " + w);
            return false;
        }
        pack::Picture p;
        p.w = img.w;
        p.h = img.h;
        p.px = std::move(img.px);
        out = add_picture(std::move(p));
        info.decoded_bytes += uint64_t(out->px.size()) * 4;
        return true;
    };

    size_t added = 0, skipped = 0;
    if (const Json* screens = m.get("screens")) {
        if (version < 2) err("screens", "", "needs manifest version 2");
        else if (screens->t != Json::Obj) err("screens", "", "not an object");
        else
            for (const auto& member : screens->o) {
                const std::string& name = member.first;
                const Json& j = member.second;
                auto bad = [&](const std::string& text) {
                    err("screens", name, text);
                    ++skipped;
                };
                if (j.t != Json::Obj) { bad("not an object"); continue; }
                pack::GameScope es;
                if (!read_scope(&j, es)) { bad("bad games/builds"); continue; }
                if (!pack::GameScope::intersect(pack_scope, es).applies(ctx_)) continue;
                pack::ScreenEntry e;
                e.name = name;
                const Json* sig = j.get("signature");
                if (!sig || sig->t != Json::Obj) { bad("missing \"signature\""); continue; }
                e.signature.block = 16;
                if (const Json* b = sig->get("block"); b && (!int_in(b, 8, 32, e.signature.block) || (e.signature.block & (e.signature.block - 1)))) {
                    bad("signature.block must be 8, 16 or 32");
                    continue;
                }
                const Json* blocks = sig->get("blocks");
                if (!blocks || blocks->t != Json::Arr || blocks->a.empty() || blocks->a.size() > 64) { bad("signature.blocks needs 1 to 64 entries"); continue; }
                bool ok = true;
                for (const Json& b : blocks->a) {
                    pack::SignatureBlock sb;
                    const Json* h = b.get("hash");
                    if (b.t != Json::Obj || !int_in(b.get("x"), 0, 4095, sb.x) || !int_in(b.get("y"), 0, 4095, sb.y) || !h ||
                        h->t != Json::Str || !parse_hex16(h->s, sb.hash)) { ok = false; break; }
                    e.signature.blocks.push_back(sb);
                }
                if (!ok) { bad("bad signature block"); continue; }
                if (!int_in(sig->get("min"), 1, 64, e.signature.min) || e.signature.min > int(e.signature.blocks.size())) {
                    bad("signature.min must be 1 to the number of blocks");
                    continue;
                }
                e.scale = pack_scale;
                if (const Json* s = j.get("scale"); s && !int_in(s, 1, 8, e.scale)) { bad("bad scale"); continue; }
                if (!read_anchor(j.get("anchor"), e.anchor)) { bad("bad anchor"); continue; }
                if (const Json* rp = j.get("ref_palette")) {
                    if (rp->t != Json::Arr || rp->a.size() < 2 || rp->a.size() > 32) { bad("ref_palette needs 2 to 32 colours"); continue; }
                    for (const Json& c : rp->a) {
                        uint32_t v = 0;
                        if (c.t != Json::Str || !parse_rgb(c.s, v)) { ok = false; break; }
                        e.ref_palette.push_back(v);
                    }
                    if (!ok) { bad("bad ref_palette colour"); continue; }
                }
                if (const Json* f = j.get("fade")) {
                    if (f->t != Json::Str || (f->s != "scale" && f->s != "none")) { bad("fade must be \"scale\" or \"none\""); continue; }
                    e.fade = f->s == "none" ? pack::FadeMode::None : pack::FadeMode::Scale;
                }
                if (const Json* b = j.get("bg_index"); b && !int_in(b, 0, 31, e.bg_index)) { bad("bad bg_index"); continue; }
                if (const Json* d = j.get("dynamic")) {
                    if (d->t != Json::Arr || d->a.size() > 32) { bad("dynamic needs at most 32 rectangles"); continue; }
                    for (const Json& r : d->a) {
                        pack::Rect rc;
                        if (r.t != Json::Obj || !int_in(r.get("x"), 0, 4095, rc.x) || !int_in(r.get("y"), 0, 4095, rc.y) ||
                            !int_in(r.get("w"), 1, 4096, rc.w) || !int_in(r.get("h"), 1, 4096, rc.h)) { ok = false; break; }
                        e.dynamic.push_back(rc);
                    }
                    if (!ok) { bad("bad dynamic rectangle"); continue; }
                }
                if (!load_png("screens", name, j.get("png"), e.pic)) { ++skipped; continue; }
                if (const Json* w = j.get("wide_png")) {
                    if (!load_png("screens", name, w, e.wide_pic)) { ++skipped; continue; }
                    if (const Json* ws = j.get("wide_scale"); ws && !int_in(ws, 1, 8, e.wide_scale)) { bad("bad wide_scale"); continue; }
                }
                if (e.pic->w < 1 || e.pic->h < 1) { bad("empty picture"); continue; }
                if (e.pic->w != 320 * e.scale || e.pic->h != 200 * e.scale)
                    warn("screens", name, "picture is " + std::to_string(e.pic->w) + "x" + std::to_string(e.pic->h) + ", the window at scale " +
                                              std::to_string(e.scale) + " is " + std::to_string(320 * e.scale) + "x" + std::to_string(200 * e.scale));
                add_screen(std::move(e));
                ++added;
            }
    }
    if (const Json* pj = m.get("panel")) {
        if (version < 2) err("panel", "", "needs manifest version 2");
        else if (pj->t != Json::Obj) err("panel", "", "not an object");
        else {
            pack::GameScope ps;
            if (!read_scope(pj, ps)) err("panel", "", "bad games/builds");
            else if (pack::GameScope::intersect(pack_scope, ps).applies(ctx_)) {
                pack::PanelSet set = panel_;
                set.scale = pack_scale;
                bool ok = true;
                if (const Json* s = pj->get("scale"); s && !int_in(s, 1, 8, set.scale)) { err("panel", "", "bad scale"); ok = false; }
                if (const Json* c = pj->get("cell_w"); ok && c && (!int_in(c, 8, 16, set.cell_w) || (set.cell_w != 8 && set.cell_w != 16))) { err("panel", "", "cell_w must be 8 or 16"); ok = false; }
                if (const Json* c = pj->get("max_span"); ok && c && !int_in(c, 1, 8, set.max_span)) { err("panel", "", "max_span must be 1 to 8"); ok = false; }
                if (ok)
                    if (const Json* rp = pj->get("ref_palette")) {
                        if (rp->t != Json::Arr || rp->a.size() < 2 || rp->a.size() > 32) { err("panel", "", "ref_palette needs 2 to 32 colours"); ok = false; }
                        else
                            for (const Json& c : rp->a) {
                                uint32_t v = 0;
                                if (c.t != Json::Str || !parse_rgb(c.s, v)) { err("panel", "", "bad ref_palette colour"); ok = false; break; }
                                set.ref_palette.push_back(v);
                            }
                    }
                if (ok)
                    if (const Json* f = pj->get("fade")) {
                        if (f->t != Json::Str || (f->s != "scale" && f->s != "none")) { err("panel", "", "fade must be \"scale\" or \"none\""); ok = false; }
                        else set.fade = f->s == "none" ? pack::FadeMode::None : pack::FadeMode::Scale;
                    }
                if (ok)
                    if (const Json* b = pj->get("background")) {
                        if (b->t != Json::Obj || !load_png("panel", "background", b->get("png"), set.background)) { ++skipped; set.background = panel_.background; }
                    }
                if (ok)
                    if (const Json* cells = pj->get("cells")) {
                        if (cells->t != Json::Obj) err("panel", "cells", "not an object");
                        else
                            for (const auto& member : cells->o) {
                                const std::string& key = member.first;
                                const Json& j = member.second;
                                if (!pack::valid_panel_key(key)) { err("panel", key, "not a panel key (<16 hex>-<1..8>)"); ++skipped; continue; }
                                if (j.t != Json::Obj) { err("panel", key, "not an object"); ++skipped; continue; }
                                pack::GameScope cs;
                                if (!read_scope(&j, cs)) { err("panel", key, "bad games/builds"); ++skipped; continue; }
                                if (!pack::GameScope::intersect(pack_scope, cs).applies(ctx_)) continue;
                                pack::PanelCell cell;
                                cell.scale = set.scale;
                                if (const Json* s = j.get("scale"); s && !int_in(s, 1, 8, cell.scale)) { err("panel", key, "bad scale"); ++skipped; continue; }
                                if (!read_anchor(j.get("anchor"), cell.anchor)) { err("panel", key, "bad anchor"); ++skipped; continue; }
                                if (!load_png("panel", key, j.get("png"), cell.pic)) { ++skipped; continue; }
                                set.cells[key] = cell;
                                ++added;
                            }
                    }
                if (ok) panel_ = std::move(set);
            }
        }
    }
    info.entries = added;
    info.skipped_entries = skipped;
    info.provides_screens = added > 0;
    info.usable = true;
    info.enabled = true;
    infos_.push_back(info);
    (void)first_info;
    return true;
}

} // namespace x2::modern
