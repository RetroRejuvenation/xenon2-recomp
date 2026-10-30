// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// LHA / LZH archives (.lha, .lzh): the usual Amiga archive format, and how WHDLoad installs are distributed.
// An archive is a sequence of member headers (levels 0, 1, 2 and 3) each followed by the member's data, ended by a
// zero byte. Methods: -lh0- (stored), -lh1- (adaptive Huffman, 4 KB window), -lh4- and -lh5- (static Huffman, 4-8 KB
// window), -lh6- and -lh7- (32 and 64 KB windows), -lz4- (stored, as LArc writes it), -lhd- (directory). Every member carries a CRC-16 of its data.
// This is our own decoder, written from the public format descriptions and checked against the ISC-licensed lhasa
// by Simon Howard (see docs/PROVENANCE.md). Docs: docs/re-notes/whdload.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

struct LhaEntry {
    std::string name;   // path with '/' separators
    std::string method; // e.g. "-lh5-"
    uint8_t header_level = 0;
    uint32_t packed_size = 0, original_size = 0;
    uint16_t crc16 = 0;
    bool is_dir = false;
    std::vector<uint8_t> data;
    std::string problem; // non-empty: the member was not extracted (data is empty)
    bool unsupported = false;
};

struct LhaArchive {
    std::vector<LhaEntry> entries;
    std::string note; // e.g. data after the last member that was ignored
};

// Reads every member of an LHA archive. False (with `why`) when the first header is not a valid LHA header or the
// members would unpack to more than `max_total` bytes. A member that cannot be decoded (unsupported method,
// CRC mismatch, truncated data) stays in the list with `problem` set. A member larger than `max_member` is not
// extracted. A damaged header after a valid member ends the archive (see `note`).
bool read_lha(const std::vector<uint8_t>& data, LhaArchive& out, std::string& why, size_t max_member = size_t(96) << 20,
              size_t max_total = size_t(512) << 20);

// CRC-16/ARC (polynomial 0xA001 reflected, initial value 0), the check LHA stores for each member.
uint16_t lha_crc16(const uint8_t* data, size_t n);

// The member decoders, exposed for the tests: unpack `n` packed bytes of the given method ("-lh5-" ...) to exactly
// `out_len` bytes. False with a reason on a damaged stream or an unsupported method.
bool lha_decode(const std::string& method, const uint8_t* src, size_t n, size_t out_len, std::vector<uint8_t>& out,
                std::string& why);

// An -lh1- encoder driven by the decoder's own adaptive model (greedy matches within the 4 KB window). It exists so the
// tests can round-trip data through every part of the model, including the periodic tree rebuild; the importer
// never compresses.
bool lha_lh1_encode(const uint8_t* data, size_t n, std::vector<uint8_t>& out);

} // namespace x2::import
