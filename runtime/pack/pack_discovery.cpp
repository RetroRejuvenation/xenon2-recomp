// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_discovery.h"

#include "pack/pack_manifest.h"
#include "pack/pack_stack.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace x2::pack {

namespace fs = std::filesystem;

std::string json_quote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
            if (uint8_t(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", unsigned(uint8_t(c)));
                r += b;
            } else r += c;
        }
    }
    return r + "\"";
}

namespace {

std::string credits_line(const std::vector<Credit>& credits) {
    std::string s;
    for (const Credit& c : credits) {
        if (c.name.empty()) continue;
        if (!s.empty()) s += "; ";
        s += c.name;
        if (!c.role.empty()) s += " (" + c.role + ")";
    }
    return s;
}

// Reads the manifest of the pack at `p`. False: not a pack at all (no manifest.json: some other folder or archive).
bool inspect_path(const fs::path& p, const PackLimits& limits, PackInfo& info) {
    info.path = p.string();
    const std::string stem = p.stem().string();
    info.id = slug_of(stem);
    info.name = stem;
    std::string why;
    auto src = open_pack_source(info.path, limits, why);
    if (!src) {
        if (why == "no manifest.json in the ZIP file") return false;  // some other archive
        info.problem = why;
        return true;
    }
    std::vector<uint8_t> bytes;
    if (!src->exists("manifest.json")) return false;                     // some other folder
    if (!src->read("manifest.json", limits.max_file, bytes, why)) { info.problem = why; return true; }
    Json root;
    if (!parse_json(std::string(bytes.begin(), bytes.end()), root, why, limits.max_json_nodes)) { info.problem = "manifest.json: " + why; return true; }
    Manifest m;
    PackReport scratch;
    std::string fatal;
    ParseOptions po;
    po.limits = limits;
    if (!parse_manifest(root, stem, po, m, scratch, fatal)) { info.problem = fatal; return true; }
    if (!m.name.empty()) info.name = m.name;
    if (!m.id.empty()) info.id = m.id;
    else if (!slug_of(m.name).empty()) info.id = slug_of(m.name);
    info.pack_version = m.pack_version;
    info.author = m.author;
    info.license = m.license;
    info.description = m.description;
    info.url = m.url;
    info.credits = credits_line(m.credits);
    info.format_version = m.version;
    info.scope = m.scope;
    info.provides_graphics = m.has_graphics_section;
    info.provides_screens = m.has_screens_section;
    info.provides_audio = m.has_audio_section;
    info.entries = m.entries;
    info.skipped_entries = m.skipped;
    info.usable = true;
    return true;
}

} // namespace

bool inspect_pack(const std::string& path, PackInfo& out) {
    PackInfo info;
    std::error_code ec;
    if (path.empty() || !fs::exists(fs::path(path), ec) || !inspect_path(fs::path(path), PackLimits(), info)) return false;
    out = std::move(info);
    return true;
}

std::vector<PackInfo> discover_packs(const std::string& dir) {
    std::vector<PackInfo> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(fs::path(dir), ec)) return out;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const fs::path p = it->path();
        std::string ext = p.extension().string();
        for (char& c : ext) c = char(std::tolower(uint8_t(c)));
        if (it->is_directory(e2) || (it->is_regular_file(e2) && ext == ".zip")) children.push_back(p);
    }
    std::sort(children.begin(), children.end());
    if (children.size() > 1000) children.resize(1000);
    const PackLimits limits;
    for (const fs::path& p : children) {
        PackInfo info;
        if (!inspect_path(p, limits, info)) continue;
        info.discovered = true;
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace x2::pack
