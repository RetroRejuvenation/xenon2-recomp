// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The block of text a bug report needs, as the Advanced page's "Copy diagnostics" produces it: version, platform, SDL and its drivers, the
// imported releases with their variant, importer version and protection method, the enabled packs and every setting that differs from the
// default. It names no folder outside the data folder's own name, no image path (only file names) and holds no game bytes.
#pragma once
#include <string>
#include <vector>

#include "host_info.h"
#include "library.h"

namespace x2::ui {

// The last component of a folder path (the data folder's own name, "x2recomp"), so a copied report does not carry a user name.
std::string folder_name(const std::string& path);

std::string diagnostics_text(const std::string& version, const HostInfo& host, const Library& lib, const Settings& s, GameKind game);

// "<name> (<licence>): <what it is used for>" lines for the About page, in the order of THIRD-PARTY-NOTICES.md.
struct ThirdParty {
  const char* name;
  const char* licence;
  const char* use;
};
const std::vector<ThirdParty>& third_party();

}  // namespace x2::ui
