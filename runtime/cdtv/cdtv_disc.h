// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The user's own Xenon 2 CDTV disc image: identification, CD-audio import and track access (no Python needed).
//
// Accepted inputs: the Redump-style ZIP (one .cue plus one .bin per track), a folder holding the .cue and .bin
// files, or the .cue file itself. The disc is recognised by content: track 1 must be MODE1/2352 data whose ISO
// 9660 volume descriptor (sector 16) names the volume XENONII, followed by the twelve audio tracks 2..13.
//
// Programme start of each audio track (docs/re-notes/cdtv.md section 1): the CUE says INDEX 01 at 00:01:62/63
// (137/138 sectors), which the data does not bear out. Measured on the real image: sector 0 of each audio file
// holds the last sector of the previous track (sample-continuous with that file's end; garbage after the data
// track), sectors 1..150 are digital silence and the music starts at sector 151 (tracks 2-11) or 150 (tracks 12
// and 13, whose sector 0 is silent too). So the rule is evidence-based, not taken from the CUE: the programme
// starts at the first sector at or after max(1, INDEX 01) that holds a non-zero sample, searched over the first
// kStartSearchSectors sectors; INDEX 01 if there is none. The stray previous-track sector is dropped (13 ms).
//
// Two ways to use the audio (both bounded in memory; playback streams from disk):
//   - import (--import / --import-cdtv PATH): the programme audio of tracks 2..13 is written as raw 16-bit
//     little-endian stereo 44.1 kHz PCM, one file per track (<data dir>/cdtv/trackNN.pcm, about 700 MB in all),
//     with manifest.txt (sizes and CRC-32s). Files are written under temporary names and renamed; the manifest
//     goes last, so an interrupted import is never mistaken for a complete one. ZIP members are inflated as a
//     stream and checked against the ZIP's CRC-32.
//   - in place (--cdtv PATH to a .cue or a folder with the .cue and .bin files): the .bin files are read where
//     they are, nothing is copied. A ZIP cannot be played in place (deflate has no random access).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x2::cdtv {

constexpr int kManifestVersion = 1;
constexpr uint32_t kSectorBytes = 2352;
constexpr uint32_t kFramesPerSector = kSectorBytes / 4; // 16-bit stereo
constexpr int kSampleRate = 44100;
constexpr int kFirstAudioTrack = 2, kLastAudioTrack = 13;
constexpr uint32_t kStartSearchSectors = 226;          // programme-start search window (3 s)
constexpr uint64_t kMaxTrackBytes = 900ull << 20;      // longer than any audio CD track (80 min = 807 MB)
constexpr uint64_t kMaxDiscBytes = 1000ull << 20;      // all tracks of one disc
constexpr uint32_t kHeadCheckBytes = 1u << 20;         // quick manifest check: CRC of the first 1 MB
constexpr uint64_t kMaxDataTrackBytes = 64ull << 20;   // the data track (the real one is ~14 MB)

struct CueTrack {
    int number = 0;
    bool audio = false;
    std::string mode; // AUDIO, MODE1/2352, ...
    std::string file; // as written in the FILE line
    int index0 = -1, index1 = -1; // sectors from the start of the file, -1 = absent
};
// Parses a CUE sheet (FILE/TRACK/INDEX; other lines ignored). False with the reason for anything malformed.
bool parse_cue(const std::string& text, std::vector<CueTrack>& tracks, std::string& why);

// Programme start (sectors) of a raw audio track file, given its first bytes (see the rule above).
uint32_t find_programme_start(const uint8_t* data, size_t bytes, int index1);

// True if the raw MODE1/2352 data track (at least 17 sectors) carries an ISO 9660 volume named XENONII.
// `volume` receives the volume id that was found (for messages).
bool identify_data_track(const uint8_t* data, size_t bytes, std::string& volume, std::string& why);

struct TrackSource {
    int number = 0;
    std::string path;      // raw PCM (imported) or the .bin (in place)
    uint64_t offset = 0;   // byte offset of the programme in `path`
    uint64_t frames = 0;   // 44.1 kHz stereo frames
    uint32_t start_sector = 0;
    uint32_t crc = 0, head_crc = 0; // imported: CRC-32 of the PCM file and of its first kHeadCheckBytes
};

struct Disc {
    std::vector<TrackSource> tracks; // audio tracks in order
    std::string where;               // the imported directory or the .cue used in place
    bool imported = false;
    const TrackSource* track(int number) const;
    uint64_t total_frames() const;
};

enum class Status { Ok = 0, WriteFailed = 1, BadInput = 2, Unsupported = 3 };

struct ImportReport {
    Status status = Status::BadInput;
    std::vector<std::string> messages; // "error: ...", "warning: ..." or plain lines
    Disc disc;
};

// Cheap check used by --import to route an input here: a .cue file, a folder holding a .cue, or a ZIP whose
// central directory lists a .cue member.
bool looks_like_disc_image(const std::string& path);

// Imports the disc's audio into out_dir (created if needed). Without write access nothing is touched;
// `analyse_only` checks the input (identification, tracks, CRCs) without writing.
ImportReport import_disc(const std::string& path, const std::string& out_dir, bool analyse_only = false);
// Reads the whole data track (track 1, MODE1/2352) of a disc image given as a .cue, a folder holding one, or a ZIP holding
// one into `raw`, for the game importer (import/cdtv_game.h). Unlike import_disc it does not need the audio tracks. The track must
// be the Xenon 2 CDTV data track (volume XENONII). `where` receives the absolute path of the input; `st` is BadInput or
// Unsupported (not this disc, or no data track) when it returns false.
bool read_data_track(const std::string& path, std::vector<uint8_t>& raw, std::string& where, std::string& why, Status& st);

// `x2recomp --import-cdtv`: imports and prints every message. Exit code: 0 ok, 1 cannot write, 2 bad input
// (missing, short or corrupted track), 3 not the Xenon 2 CDTV disc or an unsupported format.
int import_cli(const std::string& path, const std::string& out_dir);

// An imported directory: manifest version, file sizes and the CRC of each file's first MB (full = every byte).
bool open_imported(const std::string& dir, Disc& disc, std::string& why, bool full_check = false);
// A .cue (or a folder with exactly one .cue) with its .bin files, read in place.
bool open_in_place(const std::string& path, Disc& disc, std::string& why);
// A directory with manifest.txt is opened as an import, anything else in place.
bool open_disc(const std::string& path, Disc& disc, std::string& why);

} // namespace x2::cdtv
