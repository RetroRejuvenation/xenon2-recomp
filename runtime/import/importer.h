// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Native import of the user's own Xenon 2 disk images into the runtime's asset files (no Python needed).
//
// Accepted inputs, in any combination (the two discs are told apart by content, never by file name):
//   - raw 880 KB ADFs; ADZ (gzip'd ADF); DMS archives; extended ADFs (UAE-1ADF / UAE--ADF); IPF, SCP, HFE and
//     KryoFlux stream files; ZIP archives holding any of those or further ZIPs; folders of those. Containers are
//     decoded to a DiskImage (disk_image.h) and recognised by content. LHA files are recognised and reported as
//     unsupported.
// Output (write_assets): <assets dir>/game_0400.bin (the game image as it lives at $400, $3FC00 bytes), disc2.bin
// (disc 2 unchanged, 1760 x 512), manifest.txt (importer version, variant, sources and output hashes) and, for a
// retail disc imported by the key-track method, copylock.txt (the receipt, runtime/retail_protection.h).
// Supported releases (docs/re-notes/sources.md, variants.h): the two-disc BS1 "[t+3]" layout (canonical; the
// trainer loader is never run, the game image is used as packed), and the retail 2-disc release in both pressings
// (SPS 0297, SPS 2874), from an image that carries the Copylock key track (IPF, SCP, KryoFlux, HFE, extended ADF) or,
// as an explicit opt-in with a disclaimer and a confirmation, with the protection patch from any source.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr int kImporterVersion = 3;

// ConfirmationRequired: --apply-protection-patch was given without the ownership confirmation; nothing was imported.
enum class Status { Ok = 0, BadInput = 2, Unsupported = 3, ConfirmationRequired = 4 };

struct Catalogue;

// How to import. The defaults are the normal, never-patching behaviour.
struct ImportOptions {
    // The opt-in protection patch (variants.h): for a catalogued retail pressing, apply the 18-byte protection
    // removal instead of recording a key-track receipt. Needs `confirmed_ownership`; without it the import stops
    // with Status::ConfirmationRequired and the disclaimer as the messages.
    bool apply_protection_patch = false;
    bool confirmed_ownership = false;
    // The variant catalogue (nullptr: the built-in one). Tests give a synthetic one.
    const Catalogue* catalogue = nullptr;
    // When set, a per-track report of every disk image read is appended here (`x2recomp --diagnose-image`):
    // container, standard/raw/empty tracks, the key-track verdict, the detected variant. No game bytes.
    std::vector<std::string>* diagnostics = nullptr;
};

struct Message {
    enum Level { Info, Warn, Error } level;
    std::string text;
};

struct Result {
    Status status = Status::BadInput;
    std::vector<Message> messages;
    std::vector<uint8_t> game_image; // $3FC00 bytes when status == Ok
    std::vector<uint8_t> disc2;      // 901120 bytes when status == Ok
    std::vector<std::string> sources;
    std::string disc1_sha1, disc2_sha1, exe_sha1, disc1_what, disc2_what;
    // What was imported (variants.h): the registry id and name, the retail pressing (empty for BS1), the container
    // disc 1 came from, and the mode.
    std::string variant_id, variant_name, pressing_id, container;
    bool canonical = true;      // the BS1 image the replay goldens were recorded with
    bool patched = false;       // the opt-in protection patch was applied
    std::vector<std::string> patched_ranges; // "$70F6+14", ...
    std::string copylock_receipt;            // text of copylock.txt for a key-track import, else empty
};

// An in-memory input (tests, or a file already read). `name` is only used in messages.
struct Input {
    std::string name;
    std::vector<uint8_t> data;
    bool in_folder = false; // found in a given folder: unrelated files are only noted, not errors
};

Result import_inputs(const std::vector<Input>& inputs, const ImportOptions& opt = {});
// Reads files and folders (one level deep) and imports them. Unreadable paths are reported as BadInput.
Result import_paths(const std::vector<std::string>& paths, const ImportOptions& opt = {});

// Writes game_0400.bin, disc2.bin, copylock.txt (key-track imports; a stale one from an earlier import is removed
// otherwise) and manifest.txt into `assets_dir` (created if needed). Each file is written to a temporary name and
// renamed; the manifest goes last, so a set without a matching manifest is incomplete.
bool write_assets(const std::string& assets_dir, const Result& r, std::string& error);

// True if `assets_dir` holds an import by this importer version whose files (including copylock.txt when the manifest
// records one) match the manifest's hashes. Otherwise false with the reason; `sources` gets the recorded source
// paths (for an automatic re-import). The manifest's `patched` choice is not remembered across an automatic
// re-import: it only repeats key-track and BS1 imports, and a patched import is asked for again.
bool verify_assets(const std::string& assets_dir, std::string& reason, std::vector<std::string>* sources = nullptr);

// `x2recomp --import`: imports `paths` into `assets_dir`, printing every message. Returns the process exit
// code: 0 imported, 1 the assets could not be written, 2 bad input (missing disc, corrupted or unreadable file),
// 3 unsupported image or format (including a protected retail disc 1 from a source without the key track),
// 4 --apply-protection-patch without the ownership confirmation (the disclaimer is printed, nothing is imported).
int import_cli(const std::vector<std::string>& paths, const std::string& assets_dir, const ImportOptions& opt = {});

// `x2recomp --list-variants`: one block per registry row (variants.h). `x2recomp --diagnose-image PATH...`:
// a per-track report of every disk image in `paths` and what the importer makes of them; no game bytes are
// printed. Both return the exit code (0, or 2 for unreadable input).
int list_variants_cli();
int diagnose_cli(const std::vector<std::string>& paths);

// verify_assets, and when the manifest no longer matches (other importer version, changed files) a silent
// re-import from the recorded sources if they still exist. False with the reason if no valid import results.
bool ensure_imported(const std::string& assets_dir, std::string& reason);

// "error: ...", "warning: ..." or the plain text.
std::string format_message(const Message& m);

// Per-user data directory without SDL (same layout as SDL_GetPrefPath("xenon2-recomp", "x2recomp")), with a
// trailing separator; empty if no home directory is known.
std::string fallback_pref_path();

const char* status_name(Status s);

// Raw deflate (no zlib or gzip header) of exactly out_len bytes (vendor/puff).
bool inflate_raw(const uint8_t* src, size_t src_len, size_t out_len, std::vector<uint8_t>& out);

// The members of a ZIP archive (stored or deflate, each at most 96 MB unpacked), directories skipped. False with
// the reason for an unreadable archive, or when the members would unpack to more than max_total bytes.
struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
    std::string problem; // non-empty: entry not extracted
    bool unsupported = false;
};
bool unzip(const std::vector<uint8_t>& d, std::vector<ZipEntry>& entries, std::string& why, size_t max_total = SIZE_MAX);

} // namespace x2::import
