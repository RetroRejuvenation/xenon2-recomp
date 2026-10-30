// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Per-user persistence: the game's high-score table and the window front end's settings.
//
// High scores. The game keeps its table inside the program image: ten score longs at $891E-$8945 and ten
// 15-byte text lines (" 1. 1000000 BS1") at $8946-$89DB. Nothing initialises it at run time; it comes from the
// image the boot code copies to $400. The only writer is the insertion and name entry at $8A16, which ends at
// $8B1A (bsr $8C88) once the third letter is accepted. So the runtime restores a saved table straight after
// boot_game() (and after a host reset, which reloads the image) and saves it when $8B1A runs.
//
// File format (highscores.bin, all big-endian):
//   "X2HS"  u32 version (1)  u64 FNV-1a 64 of the pristine game image  u32 payload length (190)
//   payload (the 190 bytes of $891E-$89DB)  u32 CRC-32 of everything before it
// A file with another version, another image hash, a bad length or CRC, or text outside the font's
// characters is ignored with a message; the game then keeps its built-in table. Writes go to a temporary
// file that is renamed over the old one.
//
// Settings (settings.cfg): `key = value` lines, # comments. Unknown keys are reported and skipped.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace x2 {

class Machine;

constexpr uint32_t kHiscoreAddr = 0x891E;
constexpr uint32_t kHiscoreSize = 0x89DC - 0x891E; // 10 score longs + 10 lines of 15 characters
constexpr uint32_t kHiscoreSavedPc = 0x8B1A;       // name entry finished (bsr $8C88 / rts)
constexpr uint32_t kHiscoreVersion = 1;

using HiscoreTable = std::array<uint8_t, kHiscoreSize>;

uint64_t fnv1a64(const uint8_t* data, size_t n);
uint32_t crc32(const uint8_t* data, size_t n);

std::vector<uint8_t> encode_hiscores(const HiscoreTable& t, uint64_t image_hash);
// Returns false with a reason when the bytes are not a valid file for this image.
bool decode_hiscores(const std::vector<uint8_t>& file, uint64_t image_hash, HiscoreTable& out, std::string& why);

bool load_hiscore_file(const std::string& path, uint64_t image_hash, HiscoreTable& out, std::string& why);
bool save_hiscore_file(const std::string& path, uint64_t image_hash, const HiscoreTable& t, std::string& why);

HiscoreTable read_hiscores(const Machine& m);
void write_hiscores(Machine& m, const HiscoreTable& t);

enum class FilterSetting { A500, A1200, None };

// Output filter of the window (runtime/gfx/sdl_filters.h): nearest (blocky, the faithful look), sharp
// (bilinear-sharp: integer pre-scale, then bilinear to the window), scanlines, crt (scanlines, shadow mask,
// bloom, optional curvature) and edge (Scale2x/Scale3x edge-preserving upscale, then sharp).
enum class VideoFilter { Nearest, Sharp, Scanlines, Crt, Edge, kCount };
const char* video_filter_name(VideoFilter f);
bool parse_video_filter(const std::string& name, VideoFilter& out);

struct Settings {
    int scale = 3;               // 1..8
    bool fullscreen = false;
    bool aspect = false;         // 4:3 picture
    bool audio = true;           // false = --no-audio
    FilterSetting filter = FilterSetting::A500;
    float stereo_separation = 1.0f; // 0..1
    int volume = 100;            // master volume of the live output, percent 0..100
    // Target latency of the live output's ring buffer, ms 10..500. The emulation tops the buffer up whenever it
    // falls below this, so this is also how long the host loop may stall before the device runs dry and the
    // sound breaks up (docs/re-notes/music-dropouts.md §5). Raise it on a machine that cannot hold 50 fps.
    int audio_latency = 40;
    bool high_scores = true;     // keep the high-score table between runs
    // Keyboard (runtime/input/input.h). grab_keyboard: auto (the platform default, currently off everywhere),
    // on (route the system's shortcuts to the window while it has the focus) or off.
    std::string grab_keyboard = "auto";
    // Mouse cursor over the window: auto (hidden while playing, shown while frozen), show or hide. The game
    // has no mouse input.
    std::string cursor = "auto";
    bool input_debug = false;    // log every key, focus change and frame to stdout
    // Modernisation (docs/re-notes/modernisation-design.md). faithful = the single Faithful switch: original
    // picture only, the options below are kept but ignored.
    bool faithful = false;
    bool interp = false;         // interpolated frames at the display rate
    int interp_scale = 3;        // interpolation canvas, 1..8 x the Amiga resolution
    bool smooth_far = true;      // far layer at constant speed (off: exact step pictures)
    VideoFilter video_filter = VideoFilter::Nearest;
    bool integer = true;         // integer scale factors only
    bool curvature = false;      // crt filter: curved screen
    bool upgrade = false;        // replacement asset packs and palette overrides (docs/ASSET-PACKS.md)
    std::string asset_packs;     // pack paths (directories or .zip) separated by ';', later ones override earlier
    std::string wide = "off";    // widescreen surround: off, W:H (16:10, 16:9, 21:9, 32:9) or pixels per side 0..320
    bool hd_panel = false;       // status panel upscaled with the edge-preserving scaler
    // CDTV CD-audio music (runtime/cdtv/cdtv_music.h; needs the user's own CDTV disc, imported or in place).
    std::string music = "amiga"; // amiga (tracker music), cdtv (CD tracks, tracker music muted) or mix (both)
    int music_volume = 100;      // percent: CD tracks and the tracker music
    int sfx_volume = 100;        // percent: the sound effects
    int cd_crossfade = 0;        // ms, 0..500: seamless CD loops and track changes (0 = as the CDTV game)
    std::string cd_pause = "keep"; // during the game's pause: keep, duck or stop the CD
    std::string cdtv;            // CDTV disc: imported directory or .cue/folder in place ("" = <data dir>/cdtv)
    std::string cdtv_tracks = "2,3,5,2,3,5"; // CD track for title, level 1..5 (the CDTV game's defaults)
};

// "T,L1,L2,L3,L4,L5" CD track numbers 2..13 -> out. False for anything else.
bool parse_cdtv_tracks(const std::string& s, std::array<int, 6>& out);

// Applies the lines of `text` on top of `s`. Returns false and lists problems in `error` (one per line) for
// unknown keys or bad values; the valid lines are still applied.
bool parse_settings(const std::string& text, Settings& s, std::string& error);
std::string format_settings(const Settings& s);

} // namespace x2
