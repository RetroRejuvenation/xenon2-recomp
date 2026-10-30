// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Streaming raw deflate (RFC 1951) decoder with bounded memory, for unpacking the CDTV disc image's ZIP members
// (each track is 35-75 MB) without holding a whole member in memory. vendor/puff needs the complete input and
// output buffers; this decoder needs a 64 KB output window plus whatever the source and sink buffer.
//
// The source is pulled in chunks; decoded bytes are pushed to the sink in pieces of at most 32 KB. The sink can
// stop the decode early (e.g. after the first sectors of a track have been seen).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace x2::cdtv {

enum class InflateResult {
    Done,      // final block decoded
    Stopped,   // the sink returned false
    Corrupt,   // invalid block type, code lengths or distance
    Truncated, // the source ended before the final block
};

const char* inflate_result_name(InflateResult r);

// Source: fill buf with up to cap bytes, return the count (0 = end of input).
// Sink: consume n bytes; return false to stop.
using InflateSource = std::function<size_t(uint8_t* buf, size_t cap)>;
using InflateSink = std::function<bool(const uint8_t* data, size_t n)>;

// Decodes one raw deflate stream. `out_total` (optional) receives the number of bytes passed to the sink.
InflateResult inflate_stream(const InflateSource& source, const InflateSink& sink, uint64_t* out_total = nullptr);

// Incremental CRC-32 (IEEE, as ZIP): crc = crc32_update(crc, data, n), starting from 0.
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t n);

} // namespace x2::cdtv
