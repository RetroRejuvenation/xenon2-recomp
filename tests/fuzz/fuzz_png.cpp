// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Fuzz target: PNG pictures through the pack's own decoder (runtime/pack/pack_png.*), with the loader's size limits.
#include "pack/pack_api.h"
#include "pack/pack_png.h"

#include <cstdint>
#include <cstdlib>
#include <string>

using namespace x2::pack;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > (4u << 20)) return 0;
    std::string why;
    int w = 0, h = 0;
    png_size(data, size, w, h, why);
    Picture p;
    if (decode_png(data, size, p, why, 512, 1u << 18)) {
        if (p.w <= 0 || p.h <= 0 || p.px.size() != size_t(p.w) * size_t(p.h)) std::abort();
    }
    return 0;
}
