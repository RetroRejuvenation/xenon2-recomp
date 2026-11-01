// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// <data dir>/packs/: the packs found there, --list-asset-packs, and the load order of a run.
#pragma once
#include "pack/pack_api.h"

#include <string>
#include <vector>

namespace x2::pack {

// Scans `dir` for child directories and .zip files that hold a manifest.json. A pack whose manifest cannot be used is still listed
// (usable = false, problem = why). Sorted by file name; at most 1000 are read.
std::vector<PackInfo> discover_packs(const std::string& dir);

// What the manifest of the pack at `path` (a directory or a .zip) says about it, without loading its pictures or sounds: the same
// fields discover_packs fills. False when `path` holds no manifest.json at all (some other folder or archive); a manifest that cannot
// be used still returns true with usable = false and the reason in `problem`.
bool inspect_pack(const std::string& path, PackInfo& out);

// JSON string literal with the escapes JSON needs.
std::string json_quote(const std::string& s);

} // namespace x2::pack
