// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
//
// manifest.txt, the key=value file the importers write next to the imported game files, read one way everywhere. The
// readers used to disagree (last value wins here, first value there), so a file with a repeated key could be read as two
// different builds by two parts of the program. Now every reader uses parse_manifest_text(): `source` may repeat (one line
// per input), any other repeated key marks the file as damaged and the readers refuse it, which makes the next start import
// again from the recorded sources. Values are written through manifest_value() so a name taken from a disk image can never
// start a new line.
#pragma once

#include "text_safety.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace x2 {

struct ManifestText {
    std::map<std::string, std::string> kv; // every key except `source`: its value (the first one, if the key is repeated)
    std::vector<std::string> sources;      // every `source=` value, in order
    std::string repeated;                  // the first key other than `source` that appears twice; empty when the file is consistent
    bool consistent() const { return repeated.empty(); }
};

inline ManifestText parse_manifest_text(std::string_view text) {
    ManifestText m;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key(line.substr(0, eq));
        std::string value(line.substr(eq + 1));
        if (key == "source") {
            m.sources.push_back(std::move(value));
        } else if (m.kv.count(key) != 0) {
            if (m.repeated.empty()) m.repeated = key;
        } else {
            m.kv.emplace(std::move(key), std::move(value));
        }
        if (eol >= text.size()) break;
    }
    return m;
}

// A value for a manifest line: no line break, control character or malformed UTF-8 (they are escaped, see text_safety.h).
inline std::string manifest_value(std::string_view v) { return sanitize_text(v); }

// A network or device path: a UNC share (\\host\share, //host/share, \\?\UNC\...) would make Windows open a connection to
// the named host just by looking at it, so such a path is never touched on a start-up re-import.
inline bool is_network_path(std::string_view p) {
    auto sep = [](char c) { return c == '\\' || c == '/'; };
    return p.size() >= 2 && sep(p[0]) && sep(p[1]);
}

} // namespace x2
