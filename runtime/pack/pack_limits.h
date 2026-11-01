// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Limits applied while loading asset packs (docs/re-notes/asset-packs-v2-design.md section 3.5). Every header is checked against
// them before the memory it describes is allocated: packs are untrusted files.
#pragma once
#include <cstddef>
#include <cstdint>

namespace x2::pack {

struct PackLimits {
    // v1 limits, unchanged.
    size_t max_file = 32u << 20;       // bytes of one PNG or of manifest.json
    int max_side = 4096;               // width or height of one picture
    size_t max_pixels = 8u << 20;      // pixels of one picture
    size_t max_total = 512u << 20;     // bytes of all decoded pictures of all packs
    size_t max_entries = 65536;        // entries of one pack (every kind, atlas frames included)
    // v2: archives and members.
    uint64_t max_zip = uint64_t(4096) << 20;      // bytes of a ZIP pack (no ZIP64: a member or archive of 4 GB cannot be addressed)
    size_t max_zip_members = 65536;
    uint64_t max_member = uint64_t(512) << 20;    // bytes of one member (any kind) unpacked
    uint64_t max_ratio = 1000;                    // deflate ratio above this is refused ...
    uint64_t ratio_free_bytes = 1u << 20;         // ... unless the member unpacks to at most this
    // v2: backgrounds are tall; their own picture limits.
    int max_background_side = 16384;
    size_t max_background_pixels = 32u << 20;
    // v2: manifests.
    size_t max_json_nodes = 1000000;              // values in one manifest.json (a 32 MB file of "1,1,1" would otherwise be 2 GB of nodes)
    size_t max_screens = 256;                     // per pack
    // v2: audio.
    uint64_t max_audio_file = uint64_t(512) << 20;
    double max_clip_seconds = 120.0;              // sfx and speech
    size_t max_clip_bytes = 32u << 20;            // decoded (int16) bytes of one clip
    size_t max_audio_total = 256u << 20;          // decoded sfx and speech, all packs
    int min_rate = 8000, max_rate = 192000;
    int max_channels = 2;
    double max_music_seconds = 7200.0;
};

} // namespace x2::pack
