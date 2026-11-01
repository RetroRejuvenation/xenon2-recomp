// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Fuzz target: a ZIP asset pack through the lazy reader (runtime/pack/pack_source.*): central directory, names, limits, inflate and CRC.
#include "pack/pack_source.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace x2::pack;
namespace fs = std::filesystem;

namespace {
// One scratch file per process (parallel fuzz jobs and ctest runs share the temp directory).
std::string temp_zip_path() {
    std::random_device rd;
    char name[48];
    std::snprintf(name, sizeof name, "x2_fuzz_zip_%08x%08x.zip", unsigned(rd()), unsigned(rd()));
    return (fs::temp_directory_path() / name).string();
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > (1u << 20)) return 0;
    static const std::string path = temp_zip_path();
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        if (size) std::fwrite(data, 1, size, f);
        std::fclose(f);
    } else {
        return 0;
    }
    PackLimits lim;
    lim.max_member = 8u << 20;  // keep one iteration cheap: a huge declared size is refused by the limits tests, not here
    std::string why;
    auto src = open_pack_source(path, lim, why);
    if (!src) return 0;
    for (const std::string& n : src->names()) {
        std::vector<uint8_t> out;
        src->read(n, 1u << 20, out, why);
        if (auto st = src->open(n, 1u << 20, why)) {
            uint8_t b[16];
            st->read(0, b, sizeof b);
            st->read(st->size(), b, sizeof b);
            st->read(st->size() / 2, b, sizeof b);
        }
        uint64_t sz = 0;
        src->file_size(n, sz);
        src->exists(n);
    }
    src->exists("manifest.json");
    src->exists("../x");
    return 0;
}
