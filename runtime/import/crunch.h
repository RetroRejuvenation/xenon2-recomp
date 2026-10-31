// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Decrunchers for the crunched variants of the Xenon 2 disks (docs/re-notes/cracks.md, single-disk-variants.md):
//   - ONYX, the cruncher of the one-disk Vision Factory disks (the executable and the six data files),
//   - the Ice-style cruncher of the Vision Factory [a] executable,
//   - the BS1 ByteKiller flavour of the Black Monks [h][t +36] executable,
//   - the Power Pack's two crunchers (an MSB-first one for the intro and the main image, an LSB-first one for the six
//     game files) and its marker-based RLE,
// and a minimal AmigaDOS hunk walker to find the crunched block inside an executable.
//
// All of them are re-implementations written from a study of the public stubs' behaviour and verified byte for byte
// against the stubs run on a 68000 core (tools/variants/stubrun.c) and against the Python versions in tools/variants/.
// They are bounds-checked: damaged input is reported, never read or written out of range. No game data is stored here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

// Largest unpacked size any of these accept (the biggest real block is a 0x4xxxx-byte memory snapshot).
constexpr size_t kMaxCrunchedSize = 0x400000;

// ONYX: blob[offset..] = 4 parameter bytes, unpacked length (long), packed length (long), then the packed stream.
bool onyx_decrunch(const uint8_t* blob, size_t size, size_t offset, std::vector<uint8_t>& out, std::string& why);

// Ice-style: the packed stream is blob[stream_start .. stream_start + packed_len); `unpacked_len` bytes come out.
bool ice_depack(const uint8_t* blob, size_t size, size_t stream_start, size_t unpacked_len, size_t packed_len,
                std::vector<uint8_t>& out, std::string& why);

// BS1 ByteKiller flavour: blob[hdr..] = packed length, unpacked length, checksum (three longs), then the stream. The
// checksum (the XOR of every long read, with the stored value) must come out as zero.
bool bs1_depack(const uint8_t* blob, size_t size, size_t hdr, std::vector<uint8_t>& out, std::string& why);

// The Power Pack's crunchers (docs/re-notes/single-disk-variants.md). `consumed` is true when the whole stream was used,
// which holds for an intact one.
bool powerpack_decrunch_msb(const uint8_t* packed, size_t packed_len, size_t unpacked_len, std::vector<uint8_t>& out, bool& consumed,
                            std::string& why);
bool powerpack_decrunch_lsb(const uint8_t* packed, size_t packed_len, size_t unpacked_len, std::vector<uint8_t>& out, bool& consumed,
                            std::string& why);
// `marker value count` triples (count 0 = 256) and literal bytes, from the end of `src`. `left` is the number of output
// bytes not filled (0 for an intact stream).
bool powerpack_unrle(const uint8_t* src, size_t src_len, uint8_t marker, size_t out_len, std::vector<uint8_t>& out, size_t& left,
                     std::string& why);

// The data of hunk `index` (0-based position in the file's hunk table) of an AmigaDOS load file: a pointer into `blob` and
// its length. False when the file is not a hunk file or the hunk is not a CODE/DATA hunk with data.
bool hunk_data(const uint8_t* blob, size_t size, size_t index, const uint8_t*& data, size_t& len);

} // namespace x2::import
