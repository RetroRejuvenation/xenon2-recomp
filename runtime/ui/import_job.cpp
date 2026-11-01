// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import_job.h"

#include <string>
#include <utility>

#include "cdtv/cdtv_disc.h"
#include "import/cdtv_game.h"
#include "import/importer.h"
#include "import/st_game.h"
#include "import/variants.h"

namespace x2::ui {

namespace {

Severity severity_of(import::Message::Level l) {
  return l == import::Message::Error ? Severity::Error : l == import::Message::Warn ? Severity::Warn : Severity::Info;
}

void post(JobContext& job, const std::vector<import::Message>& msgs) {
  for (const import::Message& m : msgs) job.message(severity_of(m.level), import::format_message(m));
}

std::string first_error(const std::vector<import::Message>& msgs, const char* fallback) {
  for (const import::Message& m : msgs)
    if (m.level == import::Message::Error) return m.text;
  return fallback;
}

ImportOutcome cancelled_outcome(GameKind g) {
  ImportOutcome o;
  o.kind = ImportOutcome::Kind::Cancelled;
  o.game = g;
  o.headline = "Import cancelled. Nothing was changed.";
  return o;
}

ImportOutcome failed(GameKind g, std::string headline) {
  ImportOutcome o;
  o.kind = ImportOutcome::Kind::Failed;
  o.game = g;
  o.headline = std::move(headline);
  return o;
}

ImportOutcome imported(GameKind g, const std::string& name) {
  ImportOutcome o;
  o.kind = ImportOutcome::Kind::Imported;
  o.game = g;
  o.variant_name = name;
  o.headline = name.empty() ? "Imported." : "Imported: " + name;
  return o;
}

ImportOutcome import_disk(const ImportRequest& req, JobContext& job) {
  import::ImportOptions opt;
  opt.apply_protection_patch = req.apply_protection_patch;
  opt.confirmed_ownership = req.apply_protection_patch && req.confirmed_ownership;  // never one without the other
  opt.cancel = job.cancel_flag();
  opt.progress = [&job](int permille, const char* stage) { job.progress(permille * 9 / 10, stage ? stage : ""); };  // the write is the last tenth
  import::Result r = import::import_paths(req.paths, opt);
  post(job, r.messages);
  if (r.cancelled) return cancelled_outcome(GameKind::Disk);
  if (r.status == import::Status::Ok) {
    job.progress(920, "Writing the game files");
    std::string err;
    if (!import::write_assets(assets_dir_for(req.data_dir, GameKind::Disk), r, err)) {
      job.message(Severity::Error, err);
      return failed(GameKind::Disk, "The game files could not be written: " + err);
    }
    job.progress(1000, "Done");
    return imported(GameKind::Disk, r.variant_name);
  }
  // A retail disc 1 from an image without the key track: offer the opt-in patch (the importer's message names the flag).
  for (const import::Message& m : r.messages)
    if (m.level == import::Message::Error && m.text.find("--apply-protection-patch") != std::string::npos) {
      ImportOutcome o;
      o.kind = ImportOutcome::Kind::PatchOffer;
      o.headline = "This is a retail disk 1 from an image without the copy-protection key track. The best fix is an IPF or flux dump of the disk.";
      return o;
    }
  // Exactly one of the two discs is missing and nothing else is wrong: keep what was given so the other one can follow.
  int missing = 0;
  bool other_errors = false;
  const char* which = "";
  for (const import::Message& m : r.messages) {
    if (m.level != import::Message::Error) continue;
    if (m.text.rfind("disc 1 not found", 0) == 0) { ++missing; which = "disk 1"; }
    else if (m.text.rfind("disc 2 not found", 0) == 0) { ++missing; which = "disk 2"; }
    else other_errors = true;
  }
  if (missing == 1 && !other_errors) {
    ImportOutcome o;
    o.kind = ImportOutcome::Kind::NeedMore;
    o.headline = std::string("Only one disk so far: ") + which + " is still missing. Drop it as well.";
    return o;
  }
  return failed(GameKind::Disk, first_error(r.messages, "These files could not be imported."));
}

ImportOutcome import_cdtv(const ImportRequest& req, JobContext& job) {
  const std::string disc = req.paths[0];
  job.progress(20, "Reading the disc image");
  import::CdtvGame g = import::import_cdtv_game(disc);
  post(job, g.messages);
  if (job.cancelled()) return cancelled_outcome(GameKind::Cdtv);
  if (g.status != import::Status::Ok) return failed(GameKind::Cdtv, first_error(g.messages, "This disc image could not be imported."));
  job.progress(500, "Writing the game files");
  std::string err;
  if (!import::write_cdtv_assets(assets_dir_for(req.data_dir, GameKind::Cdtv), g, err)) {
    job.message(Severity::Error, err);
    return failed(GameKind::Cdtv, "The game files could not be written: " + err);
  }
  job.message(Severity::Ok, "Imported the CDTV game (" + std::to_string(g.files.size()) + " files).");
  if (g.has_cue) {
    if (job.cancelled()) {
      job.message(Severity::Info, "The CD audio was not imported (cancelled). The game was.");
      return imported(GameKind::Cdtv, g.variant_name);
    }
    job.progress(550, "Importing the CD audio (about 680 MB), this takes a moment");
    cdtv::ImportReport rep = cdtv::import_disc(disc, cd_audio_dir(req.data_dir));
    for (const std::string& m : rep.messages)
      job.message(m.rfind("error", 0) == 0 ? Severity::Error : m.rfind("warning", 0) == 0 ? Severity::Warn : Severity::Info, m);
    if (rep.status == cdtv::Status::Ok) job.message(Severity::Ok, "Imported the CD audio.");
    else if (rep.status == cdtv::Status::Unsupported)
      job.message(Severity::Warn, "This image has no complete set of CD-audio tracks: only the game was imported.");
    else job.message(Severity::Warn, "The CD audio could not be imported; the game was.");
  } else {
    job.message(Severity::Info, "A bare data track has no CD audio. Give the .cue (or its ZIP) to import the music as well.");
  }
  job.progress(1000, "Done");
  return imported(GameKind::Cdtv, g.variant_name);
}

ImportOutcome import_st(const ImportRequest& req, JobContext& job) {
  job.progress(30, "Reading the disk image");
  import::StGame g = import::import_st_paths(req.paths);
  post(job, g.messages);
  if (job.cancelled()) return cancelled_outcome(GameKind::St);
  if (g.status != import::Status::Ok) return failed(GameKind::St, first_error(g.messages, "This disk image could not be imported."));
  job.progress(600, "Writing the game files");
  std::string err;
  if (!import::write_st_assets(assets_dir_for(req.data_dir, GameKind::St), g, err)) {
    job.message(Severity::Error, err);
    return failed(GameKind::St, "The game files could not be written: " + err);
  }
  job.progress(1000, "Done");
  return imported(GameKind::St, g.disk.release ? g.disk.release->name : std::string());
}

}  // namespace

GameKind detect_family(const std::vector<std::string>& paths) {
  if (paths.size() == 1 && import::looks_like_cdtv_game_image(paths[0])) return GameKind::Cdtv;
  if (!paths.empty() && import::looks_like_st_image(paths)) return GameKind::St;
  return GameKind::Disk;
}

ImportOutcome run_import(const ImportRequest& req, JobContext& job) {
  if (req.paths.empty()) return failed(GameKind::Disk, "Nothing was given to import.");
  if (req.data_dir.empty()) return failed(GameKind::Disk, "There is no per-user data folder to import into.");
  job.progress(0, "Looking at the files");
  switch (detect_family(req.paths)) {
    case GameKind::Cdtv: return import_cdtv(req, job);
    case GameKind::St: return import_st(req, job);
    case GameKind::Disk: break;
  }
  return import_disk(req, job);
}

}  // namespace x2::ui
