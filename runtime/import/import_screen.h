// First-run import screen (window builds): asks for the user's disk images, accepts files and folders dropped
// onto the window, imports them with x2::import and shows the importer's messages.
#pragma once
#include <string>

namespace x2 {

class SdlDisplay;

// Runs until an import into `assets_dir` succeeded (true) or the user closed the window / pressed Esc (false).
// `reason` says why no usable import exists (shown on the screen).
bool run_import_screen(SdlDisplay& display, const std::string& assets_dir, const std::string& reason);

} // namespace x2
