// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// A minimal PNG writer (stored deflate blocks, RGBA) so tests and the gallery can dump a canvas for review
// without any image library. Output is deterministic.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ui_canvas.h"

namespace x2::ui {

std::vector<uint8_t> encode_png(const Canvas& c, int scale = 1);  // `scale`: whole-number nearest-neighbour enlargement
bool write_png(const Canvas& c, const std::string& path, int scale = 1);

}  // namespace x2::ui
