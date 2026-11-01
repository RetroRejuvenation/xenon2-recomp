// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Where a pack's files come from: a directory, or a ZIP read lazily (central directory first, members extracted on demand).
// Both enforce the path rules of pack_paths.h and the limits of pack_limits.h.
#pragma once
#include "pack/pack_api.h"
#include "pack/pack_limits.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x2::pack {

class PackSource {
public:
    virtual ~PackSource() = default;
    // The pack's path as given (a directory or a .zip).
    virtual const std::string& path() const = 0;
    virtual bool is_zip() const = 0;
    // Every file of the pack (names relative to the pack root, '/' separators), in a stable order; capped for huge folders.
    virtual std::vector<std::string> names() const = 0;
    // Whether the file exists (a safe path naming a regular file, or an extractable ZIP member).
    bool exists(const std::string& rel) const;
    // The size of the file in bytes (a ZIP member: its unpacked size); false when it does not exist.
    virtual bool file_size(const std::string& rel, uint64_t& size) const = 0;
    // True for a ZIP member stored without compression (streams straight from the archive), false for deflated members and files
    // of a directory pack (which stream from disk anyway: `true` too).
    virtual bool streamable(const std::string& rel) const = 0;
    // Reads the whole file when it is at most `limit` bytes. Messages: "unsafe path 'x' (relative paths inside the pack only)",
    // "missing file 'x'", "'x' leaves the pack directory", "'x': too large (N bytes, limit M)", "'x': <zip problem>".
    bool read(const std::string& rel, uint64_t limit, std::vector<uint8_t>& out, std::string& why) const;
    // A seekable stream of the file. Each call returns an independent stream (own file handle); a deflated ZIP member is inflated
    // into memory once per call (at most `limit` bytes).
    std::unique_ptr<ByteSource> open(const std::string& rel, uint64_t limit, std::string& why) const;
    // Problems found while opening that are not fatal ("member names with backslashes were normalised", unsafe members skipped).
    const std::vector<std::string>& notes() const { return notes_; }

protected:
    virtual bool do_read(const std::string& rel, uint64_t limit, std::vector<uint8_t>& out, std::string& why) const = 0;
    virtual std::unique_ptr<ByteSource> do_open(const std::string& rel, uint64_t limit, std::string& why) const = 0;
    std::vector<std::string> notes_;
};

// Opens the directory or ZIP at `path`. False with `why` ("no such asset pack (a directory or a .zip file)", "cannot open the
// directory", "not a usable ZIP file: ...", "no manifest.json in the ZIP file"). A ZIP's manifest.json may be at its root or in a
// single top-level folder, which then is the pack root.
std::unique_ptr<PackSource> open_pack_source(const std::string& path, const PackLimits& limits, std::string& why);

// A ByteSource over memory (owns the bytes).
std::unique_ptr<ByteSource> memory_byte_source(std::vector<uint8_t> bytes);

} // namespace x2::pack
