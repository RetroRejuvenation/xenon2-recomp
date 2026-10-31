// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Rob Northen Compression ("ProPack" method 1), the packer of the PDos release's files (WHDLoad imager floppy version v5,
// docs/re-notes/acquisitions-flux-acorn-st.md). Written from the public format description and checked against the CRC16s
// every stream carries in its 18-byte header: a stream counts as decoded only when the CRC16 of its packed bytes and the
// CRC16 of its unpacked bytes both match, which is also the integrity check for a disk image read from flux. Bounds-checked;
// no game data is stored here.
//
// Header (big-endian): "RNC" 01, unpacked length (4), packed length (4), CRC16 of the unpacked data (2), CRC16 of the
// packed data (2), leeway (1), chunk count (1). Then an LSB-first bit stream of 16-bit little-endian words.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr size_t kRncHeaderBytes = 18;

struct RncHeader {
    uint32_t unpacked = 0, packed = 0;
    uint16_t unpacked_crc = 0, packed_crc = 0;
    uint8_t leeway = 0, chunks = 0;
};

// CRC-16 (polynomial 0xA001, reflected, initial 0), the one RNC uses.
uint16_t rnc_crc16(const uint8_t* data, size_t n);

// Parses the header at `off`. False if there is no "RNC\1" there or the header is cut short.
bool rnc_header(const uint8_t* d, size_t n, size_t off, RncHeader& h);

// Unpacks the stream at `off`. False with the reason when the header is invalid, the stream runs past the data, the packed
// CRC16 does not match (damaged or unreadable sectors), the bit stream is malformed, or the unpacked CRC16 does not match.
// `max_unpacked` bounds the output (a header may not ask for more).
bool rnc_unpack(const uint8_t* d, size_t n, size_t off, std::vector<uint8_t>& out, RncHeader& h, std::string& why,
                size_t max_unpacked = size_t(8) << 20);

} // namespace x2::import
