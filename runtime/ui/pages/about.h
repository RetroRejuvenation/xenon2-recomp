// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The About page: version and build, platform, SDL and its drivers, the licence, what the project does and does not contain, the
// third-party software with its licences, where to find the documentation and how to report a problem. No SDL: the host fills the strings.
#pragma once
#include <functional>
#include <string>

#include "../diagnostics.h"
#include "../ui_widgets.h"

namespace x2::ui {

struct AboutInfo {
  std::string version;
  HostInfo host;
  std::string project_url = "https://github.com/RetroRejuvenation/xenon2-recomp";
  std::function<void(const std::string& url)> open_url;
  std::function<void()> open_data_folder;
};

void page_about(Context& ctx, const AboutInfo& info, Rect body);

}  // namespace x2::ui
