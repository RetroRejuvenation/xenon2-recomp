// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Fuzz target: manifest.json through the JSON reader and the manifest parser (runtime/pack/pack_json.*, pack_manifest.*).
// The first byte selects validator mode and the game scope; the rest is the manifest text.
#include "pack/pack_json.h"
#include "pack/pack_manifest.h"

#include <cstdint>
#include <cstdlib>
#include <string>

using namespace x2::pack;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > (1u << 20)) return 0;
    const uint8_t mode = size ? data[0] : 0;
    const std::string text(reinterpret_cast<const char*>(data) + (size ? 1 : 0), size ? size - 1 : 0);
    Json j;
    std::string why;
    if (!parse_json(text, j, why)) return 0;
    Manifest m;
    PackReport rep;
    ParseOptions po;
    po.validate = (mode & 1) != 0;
    po.limits.max_entries = 2000;
    std::string fatal;
    const bool ok = parse_manifest(j, "fuzz", po, m, rep, fatal);
    if (!ok && fatal.empty()) std::abort();  // a refusal always says why
    if (ok) {
        for (const GraphicDecl& g : m.graphics)
            if (g.keys.empty() || (!valid_graphic_key(g.keys[0]) && !valid_flash_key(g.keys[0]))) std::abort();
        for (const ClipDecl& c : m.clips)
            if (!valid_sample_key(c.key) || c.ref_period < 0 || c.ref_period > 65535) std::abort();
        if (m.entries > 2000 + 64) std::abort();
    }
    for (const PackMessage& msg : rep.messages)
        if (msg.format().empty()) std::abort();
    return 0;
}
