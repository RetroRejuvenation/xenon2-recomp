// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// What the user has imported: one slot per release family (Amiga disk, Amiga CDTV, Atari ST), read from the manifests the importers write.
// The cards of the launcher show this. No SDL, no game data: a manifest is a handful of text lines.
#pragma once
#include <string>

#include "settings_schema.h"

namespace x2::ui {

struct LibraryEntry {
  GameKind game = GameKind::Disk;
  bool present = false;       // an import manifest exists (it may still need a silent re-import; main.cpp does that when the game starts)
  std::string dir;            // the folder holding the import
  std::string variant;        // variant_name of the manifest ("Xenon 2 Megablast, BS1 [t+3]")
  std::string status;         // one word for the card: canonical, patched, imported, preview
  bool canonical = false, patched = false;
  bool has_cd_audio = false;  // CDTV: the disc's CD music was imported too
  // What the import recorded (the Advanced page lists it, the diagnostics text copies it). No paths: only the file name of the source.
  std::string importer_version;  // "3"
  std::string protection;        // "key-track receipt", "opt-in patch applied" or "none to answer"
  std::string source_name;       // the first source image's file name
  std::string receipt_sha1;      // the first 12 hex digits of the key-track receipt's checksum, or ""
};

struct Library {
  LibraryEntry entries[3];
  const LibraryEntry& get(GameKind g) const { return entries[static_cast<int>(g)]; }
  int count() const;
  bool any() const { return count() > 0; }
  // The first imported family in the order the program itself uses for `game = auto` (disk, then CDTV, then ST); Disk when none.
  GameKind first_present() const;
};

// `data_dir` is the per-user data directory (with or without a trailing separator).
std::string assets_dir_for(const std::string& data_dir, GameKind g);  // <data>/assets, assets-cdtv, assets-st
std::string cd_audio_dir(const std::string& data_dir);                // <data>/cdtv
Library scan_library(const std::string& data_dir);

// "disk", "cdtv", "st" (the --game and last_game values) to a family; false for anything else (including "auto" and "").
bool parse_game_kind(const std::string& s, GameKind& out);

}  // namespace x2::ui
