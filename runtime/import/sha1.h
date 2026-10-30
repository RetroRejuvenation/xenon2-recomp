// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// SHA-1 and CRC-32 for disk-image identification and archive integrity checks (not for security).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace x2::import {

std::string sha1_hex(const uint8_t* data, size_t size);
uint32_t crc32(const uint8_t* data, size_t size);

} // namespace x2::import
