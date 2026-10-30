// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Screenshot writers for RGBA8 framebuffers: binary PPM (P6) and uncompressed PNG (stored deflate blocks), so
// no image library is needed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x2 {

bool write_ppm(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height);
// alpha: keep the alpha channel (RGBA PNG); otherwise RGB.
bool write_png(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height, bool alpha = false);
// Picks PNG or PPM from the file extension (".png" -> PNG, anything else -> PPM).
bool write_image(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height);

} // namespace x2
