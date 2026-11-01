// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "menu_script.h"

#include <cstdlib>
#include <sstream>

namespace x2::ui {

bool MenuScript::parse(const std::string& spec, std::string& err) {
  keys_.clear();
  next_ = 0;
  from_ = 0;
  if (spec.empty()) return true;
  const size_t colon = spec.find(':');
  from_ = static_cast<uint32_t>(std::strtoul(spec.c_str(), nullptr, 10));
  std::stringstream keys(colon == std::string::npos ? spec : spec.substr(colon + 1));
  for (std::string name; std::getline(keys, name, ',');) {
    const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) {
      err = "unknown key '" + name + "'";
      keys_.clear();
      return false;
    }
    keys_.push_back(sc);
  }
  return true;
}

bool MenuScript::pump(uint32_t frames_run) {
  if (done() || frames_run < from_) return false;
  SDL_Event k{};
  k.type = SDL_EVENT_KEY_DOWN;
  k.key.down = true;
  k.key.scancode = keys_[next_];
  k.key.key = SDL_GetKeyFromScancode(k.key.scancode, SDL_KMOD_NONE, false);
  SDL_PushEvent(&k);
  k.type = SDL_EVENT_KEY_UP;
  k.key.down = false;
  SDL_PushEvent(&k);
  ++next_;
  return true;
}

}  // namespace x2::ui
