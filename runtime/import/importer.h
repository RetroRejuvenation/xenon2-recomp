// Native import of the user's own Xenon 2 disk images into the runtime's asset files (no Python needed).
//
// Accepted inputs, in any combination (the two discs are told apart by content, never by file name):
//   - raw 880 KB ADFs; ADZ (gzip'd ADF); ZIP archives holding ADFs/ADZs or further ZIPs; folders of those.
// Output (write_assets): <assets dir>/game_0400.bin (the depacked XENON-II image as it lives at $400, $3FC00
// bytes), disc2.bin (disc 2 unchanged, 1760 x 512) and manifest.txt (importer version, source and output
// hashes). Only the two-disc BS1 "[t+3]" layout is supported (docs/re-notes/sources.md); the trainer loader
// on disc 1 is never run, the game image is used as packed on the disc.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x2::import {

constexpr int kImporterVersion = 1;

enum class Status { Ok = 0, BadInput = 2, Unsupported = 3 };

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
};

// An in-memory input (tests, or a file already read). `name` is only used in messages.
struct Input {
    std::string name;
    std::vector<uint8_t> data;
    bool in_folder = false; // found in a given folder: unrelated files are only noted, not errors
};

Result import_inputs(const std::vector<Input>& inputs);
// Reads files and folders (one level deep) and imports them. Unreadable paths are reported as BadInput.
Result import_paths(const std::vector<std::string>& paths);

// Writes game_0400.bin, disc2.bin and manifest.txt into `assets_dir` (created if needed). Each file is written
// to a temporary name and renamed; the manifest goes last, so a set without a matching manifest is incomplete.
bool write_assets(const std::string& assets_dir, const Result& r, std::string& error);

// True if `assets_dir` holds an import by this importer version whose files match the manifest's hashes.
// Otherwise false with the reason; `sources` gets the recorded source paths (for an automatic re-import).
bool verify_assets(const std::string& assets_dir, std::string& reason, std::vector<std::string>* sources = nullptr);

// `x2recomp --import`: imports `paths` into `assets_dir`, printing every message. Returns the process exit
// code: 0 imported, 2 bad input (missing disc, corrupted or unreadable file), 3 unsupported image or format,
// 1 the assets could not be written.
int import_cli(const std::vector<std::string>& paths, const std::string& assets_dir);

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

// The members of a ZIP archive (stored or deflate, each at most 16 MB unpacked), directories skipped. False with
// the reason for an unreadable archive, or when the members would unpack to more than max_total bytes.
struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
    std::string problem; // non-empty: entry not extracted
    bool unsupported = false;
};
bool unzip(const std::vector<uint8_t>& d, std::vector<ZipEntry>& entries, std::string& why, size_t max_total = SIZE_MAX);

} // namespace x2::import
