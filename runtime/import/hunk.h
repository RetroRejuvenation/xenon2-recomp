// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// AmigaDOS load files (hunk format): parsing, and linking at a fixed base address without the operating system.
//
// Used by the CDTV importer (docs/re-notes/cdtv-runtime-gap.md, architecture 3): the game executable `X2` is a normal
// hunk file that the OS would relocate to wherever AllocMem puts it. The importer links it once, at fixed addresses
// (hunk 0 at $400, the next hunk directly behind it), so that the runtime can start it bare-metal and translated code
// stays keyed by absolute addresses.
//
// Supported block types: HUNK_HEADER, HUNK_CODE, HUNK_DATA, HUNK_BSS, HUNK_RELOC32, HUNK_END, and the harmless
// HUNK_NAME / HUNK_SYMBOL / HUNK_DEBUG, which are skipped. Anything else (overlays, 16-bit or PC-relative relocations,
// libraries, resident libraries, extended memory flags) is refused with a message naming it. Every length is checked
// against the file and against hard limits, so a crafted file cannot make the parser read or allocate without bound.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr uint32_t kHunkMaxHunks = 64;
constexpr uint32_t kHunkMaxBytes = 8u << 20;       // one hunk
constexpr uint32_t kHunkMaxTotalBytes = 16u << 20; // all hunks of a file

struct HunkReloc {
    uint32_t target = 0;               // the hunk whose base is added
    std::vector<uint32_t> offsets;     // byte offsets, in the hunk the block follows, of the longs to fix up
};

struct Hunk {
    enum class Kind { Code, Data, Bss } kind = Kind::Code;
    uint32_t mem_bytes = 0;       // allocation size in bytes (from the header table; a multiple of 4)
    uint32_t mem_flags = 0;       // 0 any, 1 chip, 2 fast (the two top bits of the table entry)
    std::vector<uint8_t> data;    // the stored bytes (empty for BSS); never longer than mem_bytes
    std::vector<HunkReloc> relocs;
};

struct HunkFile {
    std::vector<Hunk> hunks;
    size_t reloc_total = 0;       // every offset of every block
};

// Parses a hunk file. False with the reason for a damaged file or one that uses anything unsupported.
bool parse_hunk_file(const uint8_t* d, size_t size, HunkFile& out, std::string& why);

struct LinkedImage {
    uint32_t base = 0;                          // address of byte 0 of `image`
    std::vector<uint8_t> image;                 // hunk 0 at `base`, each next hunk directly behind it; BSS zero-filled
    std::vector<uint32_t> hunk_base;            // absolute address of each hunk
    std::vector<uint32_t> hunk_bytes;           // allocation size of each hunk
    std::vector<size_t> reloc_count;            // fix-ups applied at addresses inside each hunk
    std::vector<size_t> target_count;           // fix-ups whose value points into each hunk
    size_t reloc_total = 0;
    size_t odd_offsets = 0;                     // fix-ups at an odd address (the 68000 cannot execute them as code, but data may)
    size_t pointers_to_end = 0;                 // relocated longs that point exactly one past the end of their target hunk
    size_t high_byte_values = 0;                // relocated longs with a non-zero upper byte (a 68000 ignores it: 24-bit address bus); the byte is kept
};

// Links `file` at `base`: copies the stored bytes, zero-fills the rest, adds each target's base to every relocated long.
// Refuses (with the reason) a fix-up outside its hunk, a relocated value whose low 24 bits lie beyond the end of its target
// hunk, a total that does not fit below `limit` (the end of chip RAM) or a base that is not a multiple of 4. The upper byte of
// a value is left as the loader would leave it (the sum wraps at 32 bits): programs of the 24-bit-address era keep flags there.
bool link_hunk_file(const HunkFile& file, uint32_t base, uint32_t limit, LinkedImage& out, std::string& why);

} // namespace x2::import
