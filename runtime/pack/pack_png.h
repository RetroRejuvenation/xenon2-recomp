// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// PNG decoding for pack pictures. Moved from runtime/modern/assets.cpp (modern::decode_png is a wrapper).
#pragma once
#include "pack/pack_api.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace x2::pack {

// 8-bit and 16-bit RGB/RGBA/grey, palette and grey at 1-8 bits, tRNS; not interlaced; into premultiplied RGBA. The size is checked
// against max_side and max_pixels (and a deflate stream cannot expand beyond ~1032:1) before anything is inflated.
bool decode_png(const uint8_t* data, size_t n, Picture& out, std::string& why, int max_side = 4096, size_t max_pixels = 8u << 20);
// Only the header: width and height of a PNG whose first chunk (IHDR, CRC checked) is readable. Used by the validator's
// header-level checks; applies no size limits.
bool png_size(const uint8_t* data, size_t n, int& w, int& h, std::string& why);
// premultiplied 0xAABBGGRR from straight components
uint32_t premultiply(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

} // namespace x2::pack
