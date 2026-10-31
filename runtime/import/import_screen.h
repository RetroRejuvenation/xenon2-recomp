// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// First-run import screen (window builds): asks for the user's disk images, accepts files and folders dropped
// onto the window, imports them with x2::import and shows the importer's messages.
#pragma once
#include <string>

namespace x2 {

class SdlDisplay;

// Runs until an import succeeded (true) or the user closed the window / pressed Esc (false). A disk-release import goes into
// `assets_dir`; a CDTV disc image goes into its sibling assets-cdtv folder, and `chosen` (if given) then names the folder to play.
// `reason` says why no usable import exists (shown on the screen).
bool run_import_screen(SdlDisplay& display, const std::string& assets_dir, const std::string& reason, std::string* chosen = nullptr);

} // namespace x2
