// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_check.h"

#include "import/sha1.h"
#include "pack/pack_discovery.h"
#include "pack/pack_paths.h"
#include "pack/pack_stack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace x2::pack {

namespace fs = std::filesystem;

namespace {

std::string seconds(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

std::string pct(size_t a, size_t b) { return b ? std::to_string(a * 100 / b) + "%" : "-"; }

class Checker {
public:
    Checker(const CheckOptions& o, CheckResult& res, PackStack& stack, PackStack::Record& rec, const Manifest& m, const std::string& label)
        : o_(o), res_(res), stack_(stack), rec_(rec), m_(m), label_(label) {}

    void run() {
        graphics();
        far();
        atlases();
        panel();
        screens();
        audio();
        groups();
        references();
        memory();
        if (!o_.against.empty()) against();
        scoping();
    }

private:
    void add(Severity s, const std::string& section, const std::string& key, const std::string& text) { res_.report.add(s, label_, section, key, text); }
    bool dims(const std::string& path, int& w, int& h) const {
        auto it = rec_.pictures.find(path);
        if (it == rec_.pictures.end() || !it->second) return false;
        w = it->second->w;
        h = it->second->h;
        return true;
    }
    void expect_size(const DeclBase& d, const std::string& key, const std::string& path, int ew, int eh, const char* what) {
        int w = 0, h = 0;
        if (!dims(path, w, h) || (w == ew && h == eh)) return;
        add(Severity::Warning, d.section, key,
            "'" + path + "' is " + std::to_string(w) + "x" + std::to_string(h) + ", " + what + " is expected to be " + std::to_string(ew) + "x" + std::to_string(eh) +
                " (a picture that overhangs through its anchor is fine, otherwise check the scale)");
    }

    void graphics() {
        std::map<std::string, std::string> defined;  // key -> where
        auto define = [&](const std::string& key, const std::string& where) {
            auto [it, fresh] = defined.emplace(key, where);
            if (!fresh) add(Severity::Warning, "graphics", key, "is defined twice (" + it->second + " and " + where + "); the later one wins");
        };
        for (const GraphicDecl& d : m_.graphics)
            for (const std::string& k : d.keys) {
                const bool flash = k.compare(0, 2, "f:") == 0;
                if (!flash) define(k, "graphics");
                uint64_t h = 0;
                int w = 0, hh = 0;
                if (!parse_graphic_key(flash ? k.substr(2) : k, h, w, hh)) continue;
                expect_size(d, d.key, d.png, w * d.scale, hh * d.scale, "a graphic of this key at this scale");
                if (!d.flash_png.empty()) expect_size(d, d.key, d.flash_png, w * d.scale, hh * d.scale, "the flash picture of this key at this scale");
            }
        for (size_t i = 0; i < m_.atlases.size(); ++i) {
            const AtlasDecl& d = m_.atlases[i];
            for (const AtlasFrame& f : d.frames) define(f.key, d.section);
            for (const std::string& k : d.grid_keys)
                if (!k.empty()) define(k, d.section);
        }
        graphic_keys_ = defined;
    }

    void far() {
        for (const FarDecl& d : m_.far) {
            int w = 0, h = 0;
            if (dims(d.png, w, h) && h % d.scale != 0) add(Severity::Warning, d.section, "", "'" + d.png + "' is " + std::to_string(h) + " pixels tall, not a multiple of its scale " + std::to_string(d.scale));
        }
    }

    void atlases() {
        for (const AtlasDecl& d : m_.atlases) {
            auto check = [&](const std::string& key, int rw, int rh) {
                uint64_t h = 0;
                int w = 0, hh = 0;
                if (!parse_graphic_key(key, h, w, hh)) return;
                if (rw != w * d.scale || rh != hh * d.scale)
                    add(Severity::Warning, d.section, key, "the frame is " + std::to_string(rw) + "x" + std::to_string(rh) + ", a graphic of this key at scale " + std::to_string(d.scale) + " is expected to be " +
                                                              std::to_string(w * d.scale) + "x" + std::to_string(hh * d.scale));
            };
            for (const AtlasFrame& f : d.frames) check(f.key, f.rect[2], f.rect[3]);
            if (d.has_grid)
                for (const std::string& k : d.grid_keys)
                    if (!k.empty()) check(k, d.cell_w, d.cell_h);
        }
    }

    void panel() {
        if (!m_.panel) return;
        const PanelDecl& p = *m_.panel;
        if (!p.background_png.empty()) expect_size(p, "", p.background_png, 320 * p.scale, 8 * p.scale, "the panel background");
        for (const PanelCellDecl& c : p.cells) {
            const size_t dash = c.key.find('-');
            if (dash == std::string::npos) continue;
            const int span = std::atoi(c.key.c_str() + dash + 1);
            expect_size(c, c.key, c.png, p.cell_w * span * c.scale, 8 * c.scale, "a panel run of this many cells");
        }
    }

    void screens() {
        for (const ScreenDecl& d : m_.screens) {
            expect_size(d, d.key, d.png, 320 * d.scale, 200 * d.scale, "a screen picture");
            for (const Rect& r : d.dynamic)
                if (r.x + r.w > 320 || r.y + r.h > 200)
                    add(Severity::Warning, d.section, d.key, "a \"dynamic\" rectangle reaches outside the 320x200 window");
            if (d.ref_palette.empty() && d.fade == FadeMode::Scale)
                add(Severity::Info, d.section, d.key, "has no \"ref_palette\": the picture stays at full brightness while the game fades");
            if (d.signature.blocks.size() < 6) add(Severity::Warning, d.section, d.key, "the signature has fewer than 6 blocks: it may match other screens");
        }
    }

    void audio() {
        const std::string zip_hint = "a ZIP member stored without compression streams from the archive; this one is deflated and is inflated into memory";
        auto peak_check = [&](const DeclBase& d, const std::string& key, const std::string& file) {
            auto it = rec_.audio.find(file);
            if (it == rec_.audio.end()) return;
            if (it->second.peak > 1.0f) {
                char db[16];
                std::snprintf(db, sizeof db, "%.1f", 20.0 * std::log10(double(it->second.peak)));
                add(Severity::Warning, d.section, key, "'" + file + "' clips: its peak is " + db + " dBFS above full scale");
            }
        };
        for (const ClipDecl& d : m_.clips) peak_check(d, d.key, d.file);
        for (const MusicSlotDecl& d : m_.slots) {
            for (const std::string* f : {&d.slot.file.path, &d.slot.intro.path}) {
                if (f->empty()) continue;
                peak_check(d, d.key, *f);
                if (rec_.source && rec_.source->is_zip() && !rec_.source->streamable(*f)) add(Severity::Warning, d.section, d.key, "'" + *f + "': " + zip_hint);
            }
            const auto it = rec_.audio.find(d.slot.file.path);
            if (it != rec_.audio.end() && d.slot.loop.kind == LoopKind::Points && it->second.frames > 0 && it->second.rate > 0) {
                const double dur = double(it->second.frames) / it->second.rate;
                if (d.slot.loop.start_s >= dur) add(Severity::Error, d.section, d.key, "the loop starts at " + seconds(d.slot.loop.start_s) + " s, after the end of the file (" + seconds(dur) + " s)");
                else if (d.slot.loop.end_s > dur + 1e-6) add(Severity::Error, d.section, d.key, "the loop ends at " + seconds(d.slot.loop.end_s) + " s, after the end of the file (" + seconds(dur) + " s)");
            }
        }
        if (!audio_codec() && m_.has_audio_section)
            add(Severity::Info, "audio", "", "this build has no audio decoder: sound files were checked for existence only");
    }

    void groups() {
        for (const auto& [name, keys] : m_.groups)
            for (const std::string& k : keys)
                if (!graphic_keys_.count(k)) add(Severity::Warning, "groups", name, "lists the key '" + k + "', which no graphics entry or atlas defines");
    }

    void references() {
        std::set<std::string> used;
        auto use = [&](const std::string& p) {
            if (!p.empty()) used.insert(p);
        };
        for (const GraphicDecl& d : m_.graphics) { use(d.png); use(d.flash_png); }
        for (const FarDecl& d : m_.far) use(d.png);
        for (const BackgroundDecl& d : m_.backgrounds) use(d.png);
        for (const AtlasDecl& d : m_.atlases) use(d.png);
        for (const ScreenDecl& d : m_.screens) { use(d.png); use(d.wide_png); }
        if (m_.panel) {
            use(m_.panel->background_png);
            for (const PanelCellDecl& c : m_.panel->cells) use(c.png);
        }
        for (const ClipDecl& d : m_.clips) use(d.file);
        for (const MusicSlotDecl& d : m_.slots) { use(d.slot.file.path); use(d.slot.intro.path); }
        use(m_.preview);
        if (!m_.preview.empty()) {
            uint64_t size = 0;
            if (!rec_.source->file_size(m_.preview, size)) add(Severity::Error, "preview", "", "missing file '" + m_.preview + "'");
            else if (size > (1u << 20)) add(Severity::Warning, "preview", "", "'" + m_.preview + "' is larger than 1 MB");
        }
        std::vector<std::string> extra;
        for (const std::string& n : rec_.source->names()) {
            if (used.count(n) || n == "manifest.json") continue;
            std::string low = n;
            for (char& c : low) c = char(std::tolower(uint8_t(c)));
            const size_t slash = low.find_last_of('/');
            const std::string base = slash == std::string::npos ? low : low.substr(slash + 1);
            if (base.rfind("readme", 0) == 0 || base.rfind("licen", 0) == 0 || base.rfind("notice", 0) == 0 || base.rfind("credits", 0) == 0 || base.rfind("changelog", 0) == 0 ||
                base.compare(0, 1, ".") == 0 || (low.size() > 3 && (low.compare(low.size() - 3, 3, ".md") == 0 || low.compare(low.size() - 4, 4, ".txt") == 0)))
                continue;
            extra.push_back(n);
        }
        if (!extra.empty()) {
            std::string list;
            for (size_t i = 0; i < extra.size() && i < 3; ++i) list += (i ? ", " : "") + extra[i];
            add(Severity::Info, "files", "", std::to_string(extra.size()) + " file" + (extra.size() == 1 ? " is" : "s are") + " not used by manifest.json (" + list + (extra.size() > 3 ? ", ..." : "") + ")");
        }
        for (const Requirement& q : m_.requires_packs)
            add(Severity::Info, "requires", q.id, "needs the pack '" + q.id + "'" + (q.version.empty() ? "" : " " + q.version) + " to be loaded before this one");
        if (!m_.requires_x2recomp.empty()) add(Severity::Info, "requires", "x2recomp", "needs x2recomp " + m_.requires_x2recomp);
    }

    void memory() {
        const PackLimits& l = stack_.options().limits;
        const size_t pb = stack_.picture_bytes(), ab = stack_.audio_bytes();
        add(Severity::Info, "memory", "", "decoded pictures " + std::to_string(pb >> 20) + " MB of " + std::to_string(l.max_total >> 20) + " MB" +
                                              (stack_.options().decode_audio ? ", decoded sound " + std::to_string(ab >> 20) + " MB of " + std::to_string(l.max_audio_total >> 20) + " MB" : ""));
        if (pb > l.max_total / 10 * 8) add(Severity::Warning, "memory", "", "the pictures use more than 80% of the memory limit for all packs");
        if (ab > l.max_audio_total / 10 * 8) add(Severity::Warning, "memory", "", "the sound uses more than 80% of the memory limit for decoded sound");
    }

    void scoping() {
        if (!o_.game && o_.build.empty()) return;
        PackContext ctx;
        ctx.game = o_.game ? *o_.game : GameFamily::Disk;
        ctx.build = o_.build == "canonical" ? "" : o_.build;
        if (!m_.scope.applies(ctx)) {
            add(Severity::Warning, "games", "", std::string("the pack's games/builds do not include ") + game_name(ctx.game) + " build " + ctx.build_name() + ": nothing of it would be used there");
            return;
        }
        size_t total = 0, in = 0;
        auto count = [&](const GameScope& s) {
            ++total;
            if (GameScope::intersect(m_.scope, s).applies(ctx)) ++in;
        };
        for (const GraphicDecl& d : m_.graphics) count(d.scope);
        for (const FarDecl& d : m_.far) count(d.scope);
        for (const PaletteDecl& d : m_.palettes) count(d.scope);
        for (const BackgroundDecl& d : m_.backgrounds) count(d.scope);
        for (const AtlasDecl& d : m_.atlases) count(d.scope);
        for (const ScreenDecl& d : m_.screens) count(d.scope);
        for (const ClipDecl& d : m_.clips) count(d.scope);
        for (const MusicSlotDecl& d : m_.slots) count(d.scope);
        add(Severity::Info, "scope", "", std::to_string(in) + " of " + std::to_string(total) + " entries apply to " + game_name(ctx.game) + ", build " + ctx.build_name());
    }

    // ---- --against DUMPDIR
    struct DumpKeys {
        std::set<std::string> graphics, panel, sfx, speech, screens;  // screens: "x,y,hash" of the first block
        std::set<uint64_t> far;
        std::map<std::string, std::string> files;                     // "kind/key" -> png/file path inside the dump
        bool v2_screens = false;
    };
    void against() {
        std::ifstream in(fs::path(o_.against) / "manifest.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        Json root;
        std::string why, fatal;
        if (!in || !parse_json(text, root, why)) { add(Severity::Error, "against", "", "cannot read " + o_.against + "/manifest.json" + (why.empty() ? "" : ": " + why)); return; }
        Manifest dm;
        PackReport scratch;
        ParseOptions po;
        if (!parse_manifest(root, "dump", po, dm, scratch, fatal)) { add(Severity::Error, "against", "", fatal); return; }
        DumpKeys d;
        for (const GraphicDecl& g : dm.graphics)
            for (const std::string& k : g.keys) {
                d.graphics.insert(k);
                d.files["g/" + k] = g.png;
            }
        for (const AtlasDecl& a : dm.atlases) {
            for (const AtlasFrame& f : a.frames) d.graphics.insert(f.key);
            for (const std::string& k : a.grid_keys)
                if (!k.empty()) d.graphics.insert(k);
        }
        for (const FarDecl& f : dm.far)
            for (size_t i = 0; i < f.lines.size(); ++i)
                if (f.used[i]) d.far.insert(f.lines[i]);
        if (dm.panel)
            for (const PanelCellDecl& c : dm.panel->cells) {
                d.panel.insert(c.key);
                d.files["p/" + c.key] = c.png;
            }
        for (const ClipDecl& c : dm.clips) {
            (c.speech ? d.speech : d.sfx).insert(c.key);
            d.files[std::string(c.speech ? "h/" : "s/") + c.key] = c.file;
        }
        auto sig_of = [](const ScreenDecl& s) { return std::to_string(s.signature.blocks[0].x) + "," + std::to_string(s.signature.blocks[0].y) + "," + hash_hex(s.signature.blocks[0].hash); };
        for (const ScreenDecl& s : dm.screens) {
            d.screens.insert(sig_of(s));
            d.files["c/" + sig_of(s)] = s.png;
        }

        // coverage and typos
        size_t g_hit = 0, f_hit = 0, p_hit = 0, s_hit = 0, h_hit = 0, c_hit = 0;
        size_t typos = 0;
        auto typo = [&](const std::string& section, const std::string& key) {
            if (++typos <= 50) add(Severity::Warning, section, key, "this key is not in the dump: a typo, or a graphic or sound of another build");
        };
        std::set<std::string> pack_graphics;
        for (const GraphicDecl& g : m_.graphics)
            for (const std::string& k : g.keys) pack_graphics.insert(k);
        for (const AtlasDecl& a : m_.atlases) {
            for (const AtlasFrame& f : a.frames) pack_graphics.insert(f.key);
            for (const std::string& k : a.grid_keys)
                if (!k.empty()) pack_graphics.insert(k);
        }
        for (const std::string& k : pack_graphics) {
            if (k.compare(0, 2, "f:") == 0) continue;
            if (d.graphics.count(k)) ++g_hit;
            else if (!d.graphics.empty()) typo("graphics", k);
        }
        std::set<uint64_t> pack_far;
        for (const FarDecl& f : m_.far)
            for (size_t i = 0; i < f.lines.size(); ++i)
                if (f.used[i]) pack_far.insert(f.lines[i]);
        for (uint64_t h : pack_far) {
            if (d.far.count(h)) ++f_hit;
            else if (!d.far.empty()) typo("far", hash_hex(h));
        }
        if (m_.panel)
            for (const PanelCellDecl& c : m_.panel->cells)
                for (const std::string& k : c.keys) {
                    if (d.panel.count(k)) ++p_hit;
                    else if (!d.panel.empty()) typo("panel", k);
                }
        for (const ClipDecl& c : m_.clips)
            for (const std::string& k : c.keys) {
                auto& set = c.speech ? d.speech : d.sfx;
                if (set.count(k)) ++(c.speech ? h_hit : s_hit);
                else if (!set.empty()) typo(c.section, k);
            }
        for (const ScreenDecl& s : m_.screens) {
            if (d.screens.count(sig_of(s))) ++c_hit;
            else if (!d.screens.empty()) add(Severity::Warning, "screens", s.key, "its signature does not match any screen in the dump (the dump may not have reached it, or it is from another build)");
        }
        if (typos > 50) add(Severity::Warning, "", "", std::to_string(typos - 50) + " more keys are not in the dump");
        add(Severity::Info, "against", "",
            "coverage of the dump: graphics " + std::to_string(g_hit) + "/" + std::to_string(d.graphics.size()) + " (" + pct(g_hit, d.graphics.size()) + "), far lines " + std::to_string(f_hit) + "/" +
                std::to_string(d.far.size()) + " (" + pct(f_hit, d.far.size()) + "), screens " + std::to_string(c_hit) + "/" + std::to_string(d.screens.size()) + ", panel cells " + std::to_string(p_hit) +
                "/" + std::to_string(d.panel.size()) + ", sound effects " + std::to_string(s_hit) + "/" + std::to_string(d.sfx.size()) + ", speech " + std::to_string(h_hit) + "/" + std::to_string(d.speech.size()));

        // unchanged copies: a pack file byte-identical to the dump's original
        auto file_sha = [&](const fs::path& p, std::string& out) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return false;
            std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
            out = x2::import::sha1_hex(b.data(), b.size());
            return true;
        };
        std::set<std::string> done;
        size_t copies = 0;
        auto compare = [&](const DeclBase& decl, const std::string& dump_file, const std::string& pack_file) {
            if (dump_file.empty() || pack_file.empty() || !safe_pack_path(dump_file) || !done.insert(pack_file + "|" + dump_file).second) return;
            std::string a;
            std::vector<uint8_t> bytes;
            std::string e;
            if (!file_sha(fs::path(o_.against) / fs::path(dump_file), a)) return;
            if (!rec_.source->read(pack_file, 512u << 20, bytes, e)) return;
            if (x2::import::sha1_hex(bytes.data(), bytes.size()) == a) {
                ++copies;
                add(Severity::Warning, decl.section, decl.key, "'" + pack_file + "' is byte-identical to the dump's original: a dump holds the game's own material and must not be redistributed");
            }
        };
        for (const GraphicDecl& g : m_.graphics) {
            auto it = d.files.find("g/" + g.key);
            if (it != d.files.end()) compare(g, it->second, g.png);
        }
        if (m_.panel)
            for (const PanelCellDecl& c : m_.panel->cells) {
                auto it = d.files.find("p/" + c.key);
                if (it != d.files.end()) compare(c, it->second, c.png);
            }
        for (const ClipDecl& c : m_.clips) {
            auto it = d.files.find(std::string(c.speech ? "h/" : "s/") + c.key);
            if (it != d.files.end()) compare(c, it->second, c.file);
        }
        for (const ScreenDecl& s : m_.screens) {
            auto it = d.files.find("c/" + sig_of(s));
            if (it != d.files.end()) compare(s, it->second, s.png);
        }
        (void)copies;
    }

    const CheckOptions& o_;
    CheckResult& res_;
    PackStack& stack_;
    PackStack::Record& rec_;
    const Manifest& m_;
    std::string label_;
    std::map<std::string, std::string> graphic_keys_;
};

} // namespace

CheckResult check_asset_pack(const CheckOptions& o) {
    CheckResult res;
    std::error_code ec;
    if (!fs::exists(fs::path(o.path), ec)) {
        res.report.add(Severity::Error, "", "", "", o.path + ": no such asset pack (a directory or a .zip file)");
        res.exit_code = 3;
        return res;
    }
    PackContext ctx;
    ctx.game = o.game ? *o.game : GameFamily::Disk;
    ctx.build = o.build == "canonical" ? "" : o.build;
    StackOptions so;
    so.validate = true;
    so.decode_audio = o.full;
    PackStack stack(ctx, so);
    const bool ok = stack.add(o.path, res.report);
    res.label = stack.packs().empty() ? o.path : stack.packs().back().name;
    if (!stack.packs().empty()) {
        res.entries = stack.packs().back().entries;
        res.skipped = stack.packs().back().skipped_entries;
    }
    if (ok && !stack.records().empty() && stack.records().back()->manifest) {
        Checker(o, res, stack, *stack.records().back(), *stack.records().back()->manifest, res.label).run();
    }
    if (o.strict)
        for (PackMessage& m : res.report.messages)
            if (m.severity == Severity::Warning && m.section == "license") m.severity = Severity::Error;
    const size_t errors = res.report.count(Severity::Error), warnings = res.report.count(Severity::Warning);
    // A path that cannot be read at all (not a directory, not a readable ZIP) is exit code 3; a readable pack that cannot be used is 2.
    bool unreadable = false;
    if (!ok && !stack.packs().empty()) {
        const std::string& p = stack.packs().back().problem;
        unreadable = p.find("no such asset pack") != std::string::npos || p.rfind("not a usable ZIP file", 0) == 0 || p == "cannot open the directory";
    }
    res.exit_code = unreadable ? 3 : !ok || errors ? 2 : (o.strict && warnings) ? 1 : 0;
    return res;
}

std::string format_check(const CheckResult& r, bool json) {
    const size_t errors = r.report.count(Severity::Error), warnings = r.report.count(Severity::Warning);
    auto sev = [](Severity s) { return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "info"; };
    std::string out;
    if (json) {
        out = "[";
        for (size_t i = 0; i < r.report.messages.size(); ++i) {
            const PackMessage& m = r.report.messages[i];
            out += std::string(i ? ",\n " : "\n ") + "{\"severity\": \"" + sev(m.severity) + "\", \"pack\": " + json_quote(m.pack) + ", \"section\": " + json_quote(m.section) + ", \"key\": " +
                   json_quote(m.key) + ", \"text\": " + json_quote(m.text) + ", \"message\": " + json_quote(m.format()) + "}";
        }
        out += std::string(r.report.messages.empty() ? "" : ",\n ") + "{\"severity\": \"summary\", \"errors\": " + std::to_string(errors) + ", \"warnings\": " + std::to_string(warnings) +
               ", \"entries\": " + std::to_string(r.entries) + ", \"skipped\": " + std::to_string(r.skipped) + ", \"exit_code\": " + std::to_string(r.exit_code) + "}\n]\n";
        return out;
    }
    for (const PackMessage& m : r.report.messages) out += std::string(sev(m.severity)) + ": " + m.format() + "\n";
    out += "check: " + (r.label.empty() ? std::string("pack") : r.label) + ": " + std::to_string(errors) + " error" + (errors == 1 ? "" : "s") + ", " + std::to_string(warnings) + " warning" +
           (warnings == 1 ? "" : "s") + ", " + std::to_string(r.entries) + " entries used, " + std::to_string(r.skipped) + " skipped: " +
           (r.exit_code == 0 ? "valid" : r.exit_code == 1 ? "valid with warnings (--strict)" : r.exit_code == 2 ? "not valid" : "cannot be read") + "\n";
    return out;
}

int check_asset_pack_cli(const CheckOptions& o) {
    const CheckResult r = check_asset_pack(o);
    const std::string text = format_check(r, o.json);
    std::fputs(text.c_str(), r.exit_code == 3 && !o.json ? stderr : stdout);
    return r.exit_code;
}

} // namespace x2::pack
