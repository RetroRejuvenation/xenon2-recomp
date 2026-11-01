// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// load_packs (the load order of a run) and --list-asset-packs.
#include "pack/pack_discovery.h"
#include "pack/pack_stack.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

namespace x2::pack {

namespace fs = std::filesystem;

namespace {

std::string canonical_or(const std::string& p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(fs::path(p), ec);
    return ec ? p : c.string();
}

std::string games_text(const GameScope& s) {
    std::string r;
    for (GameFamily g : {GameFamily::Disk, GameFamily::Cdtv, GameFamily::St})
        if (s.games >> unsigned(g) & 1u) r += std::string(r.empty() ? "" : ",") + game_name(g);
    if (r.empty()) r = "none";
    if (!s.builds.empty()) {
        r += " (builds ";
        for (size_t i = 0; i < s.builds.size(); ++i) r += (i ? "," : "") + s.builds[i];
        r += ")";
    }
    return r;
}

std::string provides_text(const PackInfo& i) {
    std::string r;
    auto add = [&](bool on, const char* n) {
        if (on) r += std::string(r.empty() ? "" : ",") + n;
    };
    add(i.provides_graphics, "graphics");
    add(i.provides_screens, "screens");
    add(i.provides_audio, "audio");
    return r.empty() ? "-" : r;
}

} // namespace

std::shared_ptr<PackStack> load_pack_stack(const AssetPackSettings& s, const PackContext& c, PackReport& report) {
    struct Item {
        std::string path;
        bool discovered;
    };
    std::vector<Item> order;
    if (!s.packs_enabled.empty()) {
        const std::vector<PackInfo> found = discover_packs(s.packs_dir);
        for (const std::string& id : s.packs_enabled) {
            const auto it = std::find_if(found.begin(), found.end(), [&](const PackInfo& p) { return p.id == id; });
            if (it == found.end()) {
                report.add(Severity::Warning, "", "", "", "the enabled pack '" + id + "' was not found" + (s.packs_dir.empty() ? "" : " in " + s.packs_dir));
                continue;
            }
            order.push_back({it->path, true});
        }
    }
    for (const std::string& p : s.pack_paths) order.push_back({p, false});
    if (order.empty()) return nullptr;

    auto stack = std::make_shared<PackStack>(c);
    std::set<std::string> seen;
    for (const Item& it : order) {
        if (!seen.insert(canonical_or(it.path)).second) {
            report.add(Severity::Info, "", "", "", it.path + ": listed more than once; loaded once");
            continue;
        }
        stack->add(it.path, report, it.discovered);
    }
    return stack;
}

std::shared_ptr<const PackLibrary> load_packs(const AssetPackSettings& s, const PackContext& c, PackReport& report) { return load_pack_stack(s, c, report); }

int list_asset_packs_cli(const AssetPackSettings& s, bool json) {
    std::vector<PackInfo> found;
    if (s.packs_dir.empty()) {
        std::fprintf(stderr, "x2recomp: --list-asset-packs: no per-user data directory (use --data-dir DIR); no packs folder to scan\n");
    } else {
        found = discover_packs(s.packs_dir);
    }
    auto enabled = [&](const PackInfo& p) { return std::find(s.packs_enabled.begin(), s.packs_enabled.end(), p.id) != s.packs_enabled.end(); };
    if (json) {
        std::printf("[");
        for (size_t i = 0; i < found.size(); ++i) {
            const PackInfo& p = found[i];
            std::printf("%s\n  {\"id\": %s, \"name\": %s, \"version\": %s, \"author\": %s, \"license\": %s, \"description\": %s, \"url\": %s, \"path\": %s, "
                        "\"format_version\": %d, \"games\": [",
                        i ? "," : "", json_quote(p.id).c_str(), json_quote(p.name).c_str(), json_quote(p.pack_version).c_str(), json_quote(p.author).c_str(),
                        json_quote(p.license).c_str(), json_quote(p.description).c_str(), json_quote(p.url).c_str(), json_quote(p.path).c_str(), p.format_version);
            bool first = true;
            for (GameFamily g : {GameFamily::Disk, GameFamily::Cdtv, GameFamily::St})
                if (p.scope.games >> unsigned(g) & 1u) { std::printf("%s\"%s\"", first ? "" : ", ", game_name(g)); first = false; }
            std::printf("], \"builds\": [");
            for (size_t b = 0; b < p.scope.builds.size(); ++b) std::printf("%s%s", b ? ", " : "", json_quote(p.scope.builds[b]).c_str());
            std::printf("], \"provides\": [");
            first = true;
            auto prov = [&](bool on, const char* n) {
                if (on) { std::printf("%s\"%s\"", first ? "" : ", ", n); first = false; }
            };
            prov(p.provides_graphics, "graphics");
            prov(p.provides_screens, "screens");
            prov(p.provides_audio, "audio");
            std::printf("], \"enabled\": %s, \"usable\": %s, \"problem\": %s}", enabled(p) ? "true" : "false", p.usable ? "true" : "false", json_quote(p.problem).c_str());
        }
        std::printf("%s]\n", found.empty() ? "" : "\n");
        return 0;
    }
    if (found.empty()) {
        std::printf("No asset packs found%s%s.\n", s.packs_dir.empty() ? "" : " in ", s.packs_dir.c_str());
        return 0;
    }
    std::printf("Asset packs in %s:\n", s.packs_dir.c_str());
    for (const PackInfo& p : found) {
        std::printf("  %-24s %-8s %s%s\n", p.id.c_str(), p.pack_version.empty() ? "-" : p.pack_version.c_str(), p.name.c_str(), enabled(p) ? "   [enabled]" : "");
        if (!p.usable) {
            std::printf("      problem: %s\n", p.problem.c_str());
            continue;
        }
        std::printf("      games: %s; provides: %s; licence: %s%s%s\n", games_text(p.scope).c_str(), provides_text(p).c_str(), p.license.empty() ? "(none given)" : p.license.c_str(),
                    p.author.empty() ? "" : "; by ", p.author.c_str());
    }
    std::printf("Enable a pack with packs_enabled = id1;id2 in settings.cfg.\n");
    return 0;
}

} // namespace x2::pack
