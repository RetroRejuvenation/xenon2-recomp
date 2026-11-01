// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "diagnostics.h"

#include <cstring>

namespace x2::ui {

std::string folder_name(const std::string& path) {
  std::string p = path;
  while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
  const size_t cut = p.find_last_of("/\\");
  return cut == std::string::npos ? p : p.substr(cut + 1);
}

namespace {

// Settings that hold a path or a list of paths: reported as present or absent, never as text.
bool is_path_key(const char* key) { return std::strcmp(key, "asset_packs") == 0 || std::strcmp(key, "cdtv") == 0; }

}  // namespace

std::string diagnostics_text(const std::string& version, const HostInfo& host, const Library& lib, const Settings& s, GameKind game) {
  std::string t;
  t += "x2recomp " + (version.empty() ? std::string("(development build)") : version) + "\n";
  if (!host.build_id.empty()) t += "build: " + host.build_id + "\n";
  t += "platform: " + (host.platform.empty() ? std::string("unknown") : host.platform) + "\n";
  t += "SDL: " + (host.sdl_version.empty() ? std::string("none") : host.sdl_version);
  if (!host.video_driver.empty()) t += ", video " + host.video_driver;
  if (!host.audio_driver.empty()) t += ", audio " + host.audio_driver;
  if (!host.display.empty()) t += ", display " + host.display;
  t += "\n";
  t += std::string("selected release: ") + game_name(game) + "\n";
  t += "imported:\n";
  bool any = false;
  for (GameKind g : {GameKind::Disk, GameKind::Cdtv, GameKind::St}) {
    const LibraryEntry& e = lib.get(g);
    if (!e.present) continue;
    any = true;
    t += std::string("  ") + game_flag_value(g) + ": " + (e.variant.empty() ? std::string("imported") : e.variant);
    t += "; importer " + (e.importer_version.empty() ? std::string("?") : e.importer_version);
    t += "; protection: " + e.protection;
    if (!e.receipt_sha1.empty()) t += " (" + e.receipt_sha1 + ")";
    if (!e.source_name.empty()) t += "; source " + e.source_name;
    if (g == GameKind::Cdtv) t += e.has_cd_audio ? "; CD music imported" : "; no CD music";
    t += "\n";
  }
  if (!any) t += "  nothing\n";
  t += "packs enabled: " + (s.packs_enabled.empty() ? std::string("none") : s.packs_enabled) + "\n";
  t += std::string("packs added by path: ") + (s.asset_packs.empty() ? "none" : "yes") + "\n";
  t += "settings that differ from the defaults:\n";
  const Settings defaults;
  bool differs = false;
  for (const SettingRow& r : schema()) {
    const std::string now = value_text(r, s);
    if (now == value_text(r, defaults)) continue;
    differs = true;
    t += std::string("  ") + r.key + " = " + (is_path_key(r.key) ? std::string("(set)") : now) + "\n";
  }
  if (!differs) t += "  none\n";
  return t;
}

const std::vector<ThirdParty>& third_party() {
  static const std::vector<ThirdParty> list = {
      {"Musashi", "MIT-style", "the 68000 processor core"},
      {"SoftFloat", "SoftFloat notice", "headers only, for Musashi's build"},
      {"puff", "zlib", "inflate, for the disk image formats"},
      {"dr_flac", "public domain or MIT-0", "FLAC decoding of asset-pack audio"},
      {"stb_vorbis", "MIT or public domain", "Ogg Vorbis decoding of asset-pack audio (four local patches)"},
      {"Unscii", "public domain", "the pixel fonts of this launcher"},
      {"SDL 3", "zlib", "window, input, audio and file dialogs"},
  };
  return list;
}

}  // namespace x2::ui
