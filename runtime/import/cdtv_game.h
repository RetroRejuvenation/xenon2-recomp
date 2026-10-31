// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Import of the game from the user's own Xenon 2 CDTV disc (docs/re-notes/cdtv-runtime-gap.md, architecture 3: pre-link).
//
// The CDTV release's game is `X2`, an ordinary AmigaDOS hunk file that the OS would relocate to wherever memory is free.
// The importer links it once, at fixed addresses (hunk 0 at $400, the second hunk directly behind it), and stores the
// relocated memory image together with the files the game loads from the disc by name (CD0:LEVEL1, CD0:SPEECH05, ...).
// The runtime then starts the image bare-metal, like the disk build, and answers the file loads itself (a later phase).
//
// Input: the disc image as the Redump-style .cue (or a folder holding it, or a ZIP holding it) or as a bare data track
// (.bin raw MODE1/2352, .iso cooked). Only the data track is used for the game; the CD-audio tracks are a separate
// import (runtime/cdtv/cdtv_disc.h) that the same command also runs when the input has them.
//
// Output (write_cdtv_assets), in its own folder (<data dir>/assets-cdtv by default; never the disk build's assets folder):
//   game_0400.bin   the linked image: hunk 0 at $400 through the end of the last hunk (BSS zero-filled), 809,564 bytes for
//                   the known disc
//   cd0/NAME        every file the game opens, byte for byte from the disc, names without the ISO ";1"
//   manifest.txt    importer version, build=cdtv, chip_kb=1024, variant, languages, hashes, the relocation facts and the
//                   file table (name, size, SHA-1); written last, so a folder without one is incomplete
// There is no disc2.bin: the CDTV game has no second disk (the requirement is build-dependent, runtime/boot.cpp).
#pragma once
#include "import/hunk.h"
#include "import/importer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr const char* kCdtvBuildTag = "cdtv";
constexpr uint32_t kCdtvBase = 0x400;        // hunk 0
constexpr uint32_t kCdtvChipLimit = 0x100000; // the CDTV's 1 MB of chip RAM
constexpr uint32_t kCdtvChipKb = 1024;

struct CdtvFile {
    std::string name; // "LEVEL1"
    std::vector<uint8_t> data;
    std::string sha1;
};

struct CdtvGame {
    Status status = Status::BadInput;
    std::vector<Message> messages;
    std::string source;      // absolute path of the input, recorded for the automatic re-import
    std::string container;   // "CDTV disc image (.cue and .bin)", "ZIP", "folder", "data track (.bin)", "data track (.iso)"
    std::string volume_id;   // XENONII
    size_t iso_files = 0;    // files in the volume
    bool recognised = false; // the X2 of the catalogued disc
    std::string variant_id, variant_name;
    std::string languages;   // "EN,FR,DE,IT" as found in X2, in that order
    std::string x2_sha1, image_sha1;
    std::vector<uint8_t> image; // game_0400.bin
    LinkedImage link;           // the relocation facts
    std::vector<uint8_t> hunk_kinds; // 0 code, 1 data, 2 bss, one per hunk
    std::vector<uint32_t> hunk_flags;
    std::vector<CdtvFile> files; // under cd0/
    bool has_cue = false;        // the input was a cue sheet/ZIP/folder: CD-audio tracks may follow
};

// True for a .cue file, a folder holding one, a ZIP holding one, or a file whose sector 16 is an ISO 9660 descriptor
// naming the volume XENONII (cooked or raw).
bool looks_like_cdtv_game_image(const std::string& path);

// Reads the disc, finds and links X2 and collects the game's files. Never writes anything.
CdtvGame import_cdtv_game(const std::string& path);

// Writes game_0400.bin, cd0/, and manifest.txt into `dir` (created if needed; stale cd0 files, disc2.bin and copylock.txt
// of an earlier import are removed first). Each file goes under a temporary name and is renamed; the manifest is last.
bool write_cdtv_assets(const std::string& dir, const CdtvGame& g, std::string& error);

// verify_assets for a manifest that says build=cdtv: game_0400.bin and every file of the table must match.
bool verify_cdtv_assets(const std::string& dir, const std::string& manifest_text, std::string& reason);

// `x2recomp --import` for a CDTV disc: the game assets into `assets_dir`, then (when the input has audio tracks) the CD audio
// into `audio_dir` as `--import-cdtv` does. Exit code: 0 imported, 1 cannot write, 2 bad input, 3 not the Xenon 2 CDTV disc;
// an input without audio tracks is not an error, a damaged audio import is reported with its own code after the game assets
// were written.
int import_cdtv_cli(const std::string& path, const std::string& assets_dir, const std::string& audio_dir);
// The game assets only (the automatic re-import uses it).
int import_cdtv_game_cli(const std::string& path, const std::string& assets_dir);

// `x2recomp --diagnose-image` for a disc image: the ISO summary, the X2 hunk summary and the file count, no game bytes.
// Returns false when `path` is not a CDTV disc image at all (the lines are then empty).
bool diagnose_cdtv(const std::string& path, std::vector<std::string>& lines);

// True if the manifest text records build=cdtv.
bool manifest_is_cdtv(const std::string& manifest_text);

} // namespace x2::import
