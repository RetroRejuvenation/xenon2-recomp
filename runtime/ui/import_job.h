// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The launcher's import, as a job for the Worker: what the first-run window used to do on the UI thread (disk images, a CDTV disc, an
// Atari ST disk), with progress, messages and cancel. It decides the release family from what was dropped, runs the right importer, writes
// the assets and says in one plain sentence what happened. The opt-in protection patch is only ever applied when the request carries
// both flags, which the launcher sets only after the disclaimer and the separate "I own the original" confirmation.
#pragma once
#include <string>
#include <vector>

#include "library.h"
#include "worker.h"

namespace x2::ui {

struct ImportRequest {
  std::vector<std::string> paths;   // everything dropped or chosen so far (a two-disc release needs both)
  std::string data_dir;             // the per-user data directory
  bool apply_protection_patch = false;
  bool confirmed_ownership = false;
};

struct ImportOutcome {
  enum class Kind {
    Imported,    // written; `game` is the family
    Failed,      // nothing written, `headline` says why
    NeedMore,    // one disc was read, the other one is still to come (the paths are kept)
    PatchOffer,  // a retail disc 1 without its key track: the opt-in patch can be offered
    Cancelled,   // nothing was changed
  };
  Kind kind = Kind::Failed;
  GameKind game = GameKind::Disk;
  std::string headline;       // one plain sentence
  std::string variant_name;   // what was imported
};

// What the paths look like: a CDTV disc image (one path), Atari ST disk images, or Amiga disk images (the default).
GameKind detect_family(const std::vector<std::string>& paths);

// Runs on the worker thread (or directly in tests). Streams messages and progress to `job`.
ImportOutcome run_import(const ImportRequest& req, JobContext& job);

}  // namespace x2::ui
