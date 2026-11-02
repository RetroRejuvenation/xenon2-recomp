// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "library.h"
#include "../manifest_text.h"
#include "../text_safety.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace x2::ui {

namespace fs = std::filesystem;

int Library::count() const {
  int n = 0;
  for (const LibraryEntry& e : entries) n += e.present ? 1 : 0;
  return n;
}

GameKind Library::first_present() const {
  for (GameKind g : {GameKind::Disk, GameKind::Cdtv})
    if (get(g).present) return g;
  return GameKind::Disk;
}

namespace {

fs::path data_path(const std::string& data_dir) { return fs::path(data_dir); }

// `ok` is false when the file repeats a key: such a manifest is damaged (manifest_text.h) and the game is not listed until it is imported again.
std::map<std::string, std::string> read_manifest(const fs::path& file, bool& ok) {
  std::ifstream in(file);
  std::ostringstream all;
  all << in.rdbuf();
  const ManifestText m = parse_manifest_text(all.str());
  ok = m.consistent();
  std::map<std::string, std::string> kv = m.kv;
  if (!m.sources.empty()) kv["source"] = m.sources.front();  // the first source is enough here
  return kv;
}

}  // namespace

std::string assets_dir_for(const std::string& data_dir, GameKind g) {
  const char* name = g == GameKind::Disk ? "assets" : "assets-cdtv";
  return (data_path(data_dir) / name).string();
}

std::string cd_audio_dir(const std::string& data_dir) { return (data_path(data_dir) / "cdtv").string(); }

Library scan_library(const std::string& data_dir) {
  Library lib;
  for (GameKind g : {GameKind::Disk, GameKind::Cdtv}) {
    LibraryEntry& e = lib.entries[static_cast<int>(g)];
    e.game = g;
    if (data_dir.empty()) continue;
    e.dir = assets_dir_for(data_dir, g);
    std::error_code ec;
    const fs::path manifest = fs::path(e.dir) / "manifest.txt";
    if (!fs::exists(manifest, ec)) continue;
    bool manifest_ok = true;
    const auto kv = read_manifest(manifest, manifest_ok);
    if (!manifest_ok) continue;
    e.present = true;
    auto get = [&](const char* k) {
      const auto it = kv.find(k);
      return it == kv.end() ? std::string() : sanitize_text(it->second);  // shown on the home page: no control bytes
    };
    e.variant = get("variant_name");
    e.canonical = get("canonical") == "true";
    e.patched = get("patched") == "true";
    e.status = e.patched ? "patched" : e.canonical ? "canonical" : "imported";
    e.importer_version = get("importer_version");
    e.protection = e.patched ? "opt-in patch applied" : get("copylock") == "present" ? "key-track receipt" : "none to answer";
    {
      std::string src = get("source");
      const size_t cut = src.find_last_of("/\\");
      e.source_name = cut == std::string::npos ? src : src.substr(cut + 1);
      const std::string sha = get("copylock.txt_sha1");
      e.receipt_sha1 = sha.substr(0, 12);
    }
    if (g == GameKind::Cdtv) e.has_cd_audio = fs::exists(fs::path(cd_audio_dir(data_dir)) / "manifest.txt", ec);
  }
  return lib;
}

bool parse_game_kind(const std::string& s, GameKind& out) {
  if (s == "disk") out = GameKind::Disk;
  else if (s == "cdtv") out = GameKind::Cdtv;
  else return false;
  return true;
}

}  // namespace x2::ui
