// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "library.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace x2::ui {

namespace fs = std::filesystem;

int Library::count() const {
  int n = 0;
  for (const LibraryEntry& e : entries) n += e.present ? 1 : 0;
  return n;
}

GameKind Library::first_present() const {
  for (GameKind g : {GameKind::Disk, GameKind::Cdtv, GameKind::St})
    if (get(g).present) return g;
  return GameKind::Disk;
}

namespace {

fs::path data_path(const std::string& data_dir) { return fs::path(data_dir); }

std::map<std::string, std::string> read_manifest(const fs::path& file) {
  std::map<std::string, std::string> kv;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv.emplace(line.substr(0, eq), line.substr(eq + 1));  // the first of a repeated key (source=) is enough here
  }
  return kv;
}

}  // namespace

std::string assets_dir_for(const std::string& data_dir, GameKind g) {
  const char* name = g == GameKind::Disk ? "assets" : g == GameKind::Cdtv ? "assets-cdtv" : "assets-st";
  return (data_path(data_dir) / name).string();
}

std::string cd_audio_dir(const std::string& data_dir) { return (data_path(data_dir) / "cdtv").string(); }

Library scan_library(const std::string& data_dir) {
  Library lib;
  for (GameKind g : {GameKind::Disk, GameKind::Cdtv, GameKind::St}) {
    LibraryEntry& e = lib.entries[static_cast<int>(g)];
    e.game = g;
    if (data_dir.empty()) continue;
    e.dir = assets_dir_for(data_dir, g);
    std::error_code ec;
    const fs::path manifest = fs::path(e.dir) / "manifest.txt";
    if (!fs::exists(manifest, ec)) continue;
    const auto kv = read_manifest(manifest);
    e.present = true;
    auto get = [&](const char* k) {
      const auto it = kv.find(k);
      return it == kv.end() ? std::string() : it->second;
    };
    e.variant = get("variant_name");
    e.canonical = get("canonical") == "true";
    e.patched = get("patched") == "true";
    e.status = e.patched ? "patched" : g == GameKind::St ? "preview" : e.canonical ? "canonical" : "imported";
    if (g == GameKind::Cdtv) e.has_cd_audio = fs::exists(fs::path(cd_audio_dir(data_dir)) / "manifest.txt", ec);
  }
  return lib;
}

bool parse_game_kind(const std::string& s, GameKind& out) {
  if (s == "disk") out = GameKind::Disk;
  else if (s == "cdtv") out = GameKind::Cdtv;
  else if (s == "st") out = GameKind::St;
  else return false;
  return true;
}

}  // namespace x2::ui
