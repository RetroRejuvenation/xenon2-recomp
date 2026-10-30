// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "retail_protection.h"

#include "boot.h"
#include "machine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace x2 {

namespace {
constexpr uint32_t kImageBase = kGameBase; // $400

bool read_all(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
} // namespace

std::string format_copylock_datum(uint32_t seed) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0x%06X\n",
                  seed & 0x7FFFFF);
    return buf;
}

bool parse_copylock_datum(const std::string& text, CopylockDatum& out, std::string& why) {
    out = CopylockDatum{};
    std::istringstream in(text);
    std::string line;
    bool version = false, signature = false, syncs = false, seed_seen = false;
    uint32_t seed = 0;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string key, value;
        ls >> key >> value;
        if (key == "x2recomp-copylock") {
            if (value != "1") { why = "unsupported receipt version " + value; return false; }
            version = true;
        } else if (key == "signature") {
            if (value != "rob-northen-comp") { why = "the key track signature was not verified"; return false; }
            signature = true;
        } else if (key == "syncs") {
            if (value != "ok") { why = "the key track's sync order was not verified"; return false; }
            syncs = true;
        } else if (key == "seed") {
            char* end = nullptr;
            unsigned long v = std::strtoul(value.c_str(), &end, 0);
            if (value.empty() || !end || *end) { why = "bad seed \"" + value + "\""; return false; }
            if (v == 0 || v > 0x7FFFFF) { why = "the seed must be a non-zero 23-bit value"; return false; }
            seed = uint32_t(v);
            seed_seen = true;
        } else {
            why = "unknown line \"" + key + "\"";
            return false;
        }
    }
    if (!version) { why = "missing version line"; return false; }
    if (!signature) { why = "missing signature line"; return false; }
    if (!syncs) { why = "missing syncs line"; return false; }
    if (!seed_seen) { why = "missing seed line"; return false; }
    out.present = true;
    out.seed = seed;
    return true;
}

bool load_copylock_datum(const std::string& dir, CopylockDatum& out, std::string& why) {
    out = CopylockDatum{};
    why.clear();
    std::string text;
    if (!read_all(dir + "/" + kCopylockDatumFile, text)) return true; // absent
    if (!parse_copylock_datum(text, out, why)) {
        why = dir + "/" + kCopylockDatumFile + ": " + why;
        return false;
    }
    return true;
}

bool is_retail_copylock_image(const uint8_t* image, size_t size) {
    const size_t at = kRetailCopylockEntry - kImageBase;
    if (!image || size < at + kCopylockStubLength) return false;
    const uint8_t* p = image + at;
    static const uint8_t kPrologue[] = {0x2F, 0x0E, 0x4D, 0xFA, 0, 0, 0x48, 0xD6, 0xFF, 0xFF, 0x4D, 0xEE, 0x00, 0x40};
    for (size_t i = 0; i < sizeof kPrologue; ++i) {
        if (i == 4 || i == 5) continue; // the lea displacement
        if (p[i] != kPrologue[i]) return false;
    }
    return true;
}

RetailProtection install_retail_protection(Machine& m, const GameAssets& assets, const CopylockDatum& key) {
    if (!is_retail_copylock_image(assets.game_image.data(), assets.game_image.size())) return RetailProtection::NotRetail;
    if (!key.present) return RetailProtection::MissingKey;
    // A trap rather than a byte patch: the image stays as it is on the disc, and the translated executor, which
    // verifies each instruction's bytes, sees no modified code (a trap that moves the PC behaves identically in
    // both executors, runtime/recomp/x2r_runtime.h).
    m.add_trap(kRetailCopylockEntry, [](Machine& mm) { mm.set_pc(kRetailCopylockExit); });
    return RetailProtection::Installed;
}

} // namespace x2
