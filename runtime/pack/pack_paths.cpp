// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_paths.h"

#include <cctype>

namespace x2::pack {

namespace {

bool reserved_name(const std::string& comp) {
    // The device name is the part before the first dot ("nul.txt" is NUL too).
    std::string stem = comp.substr(0, comp.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) c = char(std::toupper(uint8_t(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    return stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '1' && stem[3] <= '9';
}

} // namespace

std::string path_problem(const std::string& p) {
    if (p.empty()) return "empty path";
    if (p.size() > 240) return "longer than 240 bytes";
    if (p[0] == '/') return "absolute path";
    for (char c : p) {
        if (uint8_t(c) < 0x20 || c == 0x7F) return "control character";
        if (c == '\\') return "backslash";
        if (c == ':') return "':' (drive letter)";
    }
    if (p.back() == '/') return "ends with '/'";
    size_t at = 0;
    while (at <= p.size()) {
        size_t end = p.find('/', at);
        if (end == std::string::npos) end = p.size();
        const std::string comp = p.substr(at, end - at);
        if (comp.empty()) return "empty component";
        if (comp == "." || comp == "..") return "'" + comp + "' component";
        if (comp.back() == ' ' || comp.back() == '.') return "component ends with a space or a dot";
        if (reserved_name(comp)) return "reserved Windows device name";
        at = end + 1;
    }
    return "";
}

bool safe_pack_path(const std::string& p) { return path_problem(p).empty(); }

std::string fold_path(const std::string& p) {
    std::string r = p;
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return r;
}

std::string normalise_member_name(const std::string& name, bool& changed) {
    std::string r = name;
    changed = false;
    for (char& c : r)
        if (c == '\\') { c = '/'; changed = true; }
    return r;
}

} // namespace x2::pack
