// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The asset pack validator behind `x2recomp --check-asset-pack` (docs/re-notes/asset-packs-v2-design.md section 6). tools/assetpack/validate.py
// applies the same rules to the manifest without the runtime (it cannot decode audio and has no run-time scoping).
#pragma once
#include "pack/pack_api.h"

#include <string>

namespace x2::pack {

struct CheckResult {
    PackReport report;     // every finding, in order: the loader's, then the validator's own
    int exit_code = 0;     // 0 valid, 1 warnings with --strict, 2 unusable pack or invalid entries, 3 the path cannot be read
    size_t entries = 0, skipped = 0;
    std::string label;     // the pack's name
};

CheckResult check_asset_pack(const CheckOptions& o);
// "error: asset pack: ..." lines and a summary, or the JSON array.
std::string format_check(const CheckResult& r, bool json);

} // namespace x2::pack
