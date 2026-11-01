// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The command line and the settings file as one set of options: `Options` is everything a run can be asked to do, `parse_args` reads
// the arguments into it, `apply_settings` reads settings.cfg into it (window runs; the arguments are applied on top), and
// `usage` prints the help. They were private to main.cpp; the launcher and the settings pages need the same parser, and the tests
// replay every flag spelling through it (tests/cli_options_tests.cpp).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "audio/paula.h"
#include "debug_dump.h"
#include "machine.h"
#include "modern/present.h"
#include "pack/pack_api.h"
#include "persist.h"
#ifdef X2_HAVE_SDL
#include "input/input.h"
#endif

namespace x2::cli {

#ifdef X2_HAVE_SDL
constexpr int kDefaultAutofireRate = x2::input::AutofirePolicy::kDefaultRate;
#else
constexpr int kDefaultAutofireRate = 17; // as input/input.h (a build without SDL3 has no input code)
#endif

struct InputEvent {
    uint32_t frame;
    enum Kind { Key, Fire, Joy } kind;
    uint8_t code;
    uint32_t hold;
    uint8_t dirs = 0; // Joy: 1 up, 2 down, 4 left, 8 right, 16 fire, 32 second button
};


// Coverage-only state pokes (x2::Poke): never applied unless given with --poke or a script "poke"/"pokeobj" line.
using x2::Poke;

struct Options {
    bool headless = false;
    uint32_t frames = 500; // headless default; a window runs until quit unless --frames is given
    bool frames_set = false, report_set = false;
    std::string dump_dir;
    std::string assets;   // "" = ./generated/assets if present, else the imported <data dir>/assets
    std::string game = "auto"; // --game: disk (the Amiga disk release), cdtv (the CDTV release), st (the Atari ST release) or auto
                               // (disk if imported, else CDTV, else ST)
    bool game_explicit = false; // --game given on the command line (it wins over settings.cfg)
    std::string data_dir; // per-user data directory (default: SDL_GetPrefPath)
    std::string st_disk;  // --game st: the plain .st image to boot (the Atari ST target is in development)
    std::vector<uint32_t> st_watch; // --st-watch HEX[,HEX...]: RAM words printed when they change
    std::string st_input_script; // --game st: --input-script FILE, parsed by the ST runner (st/st_input.h)
    bool for_st = false;         // --game st was given (decided before the arguments are read)
    uint32_t st_fdc_speed = 1, st_trace_tail = 0, st_watch_pc_addr = 0, st_watch_pc_len = 0;
    bool st_io_summary = false, st_fdc_log = false;
    std::vector<std::pair<uint32_t, uint8_t>> st_joy; // --st-joy-at F:STATE
    std::string st_frame_log, st_shots_dir, st_shots; // --st-frame-log, --st-shots-dir, --st-shots
    int st_input_offset = 0;
    uint32_t st_profile_from = 0, st_profile_to = 0;
    std::vector<std::pair<uint32_t, std::string>> st_ram_dumps; // --st-ram-dump-at F:FILE
    bool st_state_hash = false; // --st-state-hash: 'statehash F <digest>' with every framehash
    uint32_t chip_kb = 0; // --chip-ram: 512 or 1024 (0 = what the build asks for, 512 for every disk build)
    std::vector<std::string> import_paths;
    bool apply_protection_patch = false, confirm_own_original = false, list_variants = false;
    std::vector<std::string> diagnose_paths;
    uint32_t report_every = 50;
    uint32_t ppm_every = 0;
    bool reglog = true;
    std::vector<uint32_t> trace_frames;
    std::vector<InputEvent> input;
    std::vector<Poke> pokes;
    std::string trace_out, trace_label;
    bool audio_hash = false;
    std::vector<std::pair<uint32_t, std::string>> screenshots;
    std::vector<std::pair<uint32_t, std::string>> ram_dumps;
    std::vector<uint32_t> hash_frames;
    uint32_t hash_every = 0;
    bool interp_only = false;   // recomp builds: run everything on the Musashi interpreter
    int recomp_stats = -1;      // recomp builds: print coverage and the N most-interpreted PCs at exit
    std::string recomp_profile; // recomp builds: write interpreted PCs as seeds for tools/recomp/recomp.py
    int scale = 3;
    bool fullscreen = false, no_audio = false;
    int volume = 100;          // live output master volume, percent
    int audio_latency = 40;    // live output buffer, ms (runtime/audio/sdl_audio.h)
    std::string persist_dir;   // keep the high-score table in <dir>/highscores.bin ("" = off)
    bool no_persist = false;
    std::string audio_out, audio_cues;
    std::string menu_script;   // TEST ONLY (window): FRAME:KEY[,KEY...] pressed one by one once FRAME frames have run (see --help)
    x2::PaulaConfig audio;
    x2::Timing timing = x2::Timing::Accurate;
    std::string watch_log;                // per-frame chip words (reference comparison, tools/compare/)
    std::string pc_trace, blit_log;       // --pc-trace / --blit-log F-G:FILE
    std::string draw_log;                 // --draw-log F-G:FILE (modernisation analysis)
    uint32_t draw_log_from = 0, draw_log_to = 0;
    // Picture options (settings.cfg, --faithful, --modern, --filter, --aspect, ...; modern/present.h).
    x2::modern::PresentOptions present;
    bool faithful_forced = false, custom_given = false; // --faithful; any option that asks for a modern picture
    bool modern_check = false;
    int modern_rate = 60;
    std::string modern_out, modern_shots; // --modern-out F-G:DIR, --modern-shots F:DIR
    uint32_t modern_out_from = 0, modern_out_to = 0, modern_shots_at = 0;
    std::vector<std::pair<uint32_t, std::string>> present_shots; // --present-shot F:FILE
    std::vector<std::string> asset_packs; // settings asset_packs, then --asset-pack (later ones override)
    std::string dump_assets;              // --dump-assets DIR
    // Asset packs v2 (runtime/pack/pack_api.h, docs/re-notes/asset-packs-v2-design.md): the discovered packs to load (settings
    // packs_enabled), pack audio on or off (--modern audio|noaudio), speech volume, and the author tools.
    std::vector<std::string> packs_enabled;
    bool audio_upgrade = true;
    int speech_volume = 100;
    bool speech_volume_given = false;
    x2::pack::CheckOptions check_pack;    // --check-asset-pack PATH [--against DIR] [--build TAG] [--full] [--strict] [--json]
    bool check_flag_given = false, json_given = false, list_asset_packs = false;
    std::string dump_audio;               // --dump-audio DIR
    std::string pc_log;                   // --pc-log F-G:PC,PC,...:FILE
    uint32_t pc_log_from = 0, pc_log_to = 0;
    std::vector<uint32_t> pc_log_pcs;     // empty with a pc_log: every instruction (--insn-log)
    uint32_t pc_trace_from = 0, pc_trace_to = 0, blit_log_from = 0, blit_log_to = 0;
    std::vector<uint32_t> watch = {0x404, 0x41C, 0x1FEE, 0x1FF0, 0x1FF2, 0x1FF4};
    // CDTV CD-audio music (runtime/cdtv): source, volumes, loop crossfade, pause behaviour, disc, track map.
    std::string music = "amiga", cd_pause = "keep", cdtv_path, import_cdtv, cdtv_log, cdtv_tracks = "2,3,5,2,3,5";
    int music_volume = 100, sfx_volume = 100, cd_crossfade = 0;
    bool protect_music = false; // keep the tune audible through the effects that take its channels (not faithful)
    // Keyboard (runtime/input/input.h): the grab mode, and the --input-debug event log.
    std::string grab_keyboard = "auto", cursor = "auto";
    bool input_debug = false;
    // Assists and cheats (runtime/input/input.h; README "Assists and cheats"). All off by default.
    bool autofire = false, god_mode = false, cheat_scores = false;
    int autofire_rate = kDefaultAutofireRate;
    int credits = 3;
    bool help = false, version = false, print_data_dir = false, list_keys = false; // print and exit
    bool print_settings = false; // --print-settings: print the settings this run would use (settings.cfg format) and exit
    // --gui (1) and --no-gui (0); settings.cfg's `launcher = always` / `never` say the same, and the command line wins. -1: neither,
    // so the launcher shows when the program was started without arguments (launcher_wanted).
    int gui = -1;
};

// Whether a window run starts with the launcher (runtime/ui/launcher.h): never headless, always with --gui, never with --no-gui, and
// otherwise (launcher = auto) when the program was started without arguments. `argc` is the count main() got (1 = no arguments).
bool launcher_wanted(const Options& o, int argc);

void usage(std::FILE* out = stderr);
// False after a message on stderr for a flag that is unknown, lacks its value or has a bad one.
bool parse_args(int argc, char** argv, Options& o);
// settings.cfg -> Options (window mode only; the command line is applied on top afterwards).
void apply_settings(const x2::Settings& s, Options& o);
// The reverse of apply_settings: what the options say, written into the Settings fields they correspond to (the others keep their
// value, so start from the settings that were loaded). Values the file would refuse are brought into its range (--scale 12 is a window
// of 12 times the picture, settings.cfg allows 1..8); spellings the command line accepts and the file does not are written as the file
// spells them (--cursor on is `cursor = show`).
void settings_from_options(const Options& o, x2::Settings& s);
// Every field of Options as "name=value" lines: the tests compare these dumps.
std::string describe_options(const Options& o);

} // namespace x2::cli
