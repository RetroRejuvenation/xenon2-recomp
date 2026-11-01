// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Path rules for files inside a pack (docs/re-notes/asset-packs-v2-design.md section 3.1).
#pragma once
#include <string>

namespace x2::pack {

// Relative path inside a pack: UTF-8, '/' separators, at most 240 bytes, no leading '/', no empty, "." or ".." component, no
// backslash, no ':' (drive letters and alternate streams), no control characters, no Windows reserved device names (CON, PRN,
// AUX, NUL, COM1-9, LPT1-9, with or without an extension), no component ending in a space or a dot.
bool safe_pack_path(const std::string& p);
// Why a path is unsafe ("" when it is fine): the loader and the validator quote it.
std::string path_problem(const std::string& p);
// ASCII case folding of a member name, for the "two members collide on a case-insensitive file system" check.
std::string fold_path(const std::string& p);
// The pack-internal path of a member name read from a ZIP: backslashes become '/' (old Windows-made archives); `changed` is
// set when that happened.
std::string normalise_member_name(const std::string& name, bool& changed);

} // namespace x2::pack
