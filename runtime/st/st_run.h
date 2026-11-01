// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// `x2recomp --game st`: loads the disk, boots it on the StMachine and runs frames, headless (hashes, screenshots,
// an I/O register summary, a run report) or in an SDL window with the YM2149 sound. The ST target is in development
// (docs/re-notes/atari-st-machine.md). The disk is the imported one (<data dir>/assets-st: disk.bin and manifest.txt from
// import/st_game.h) or, for development, a plain ".st" file given with --st-disk.
#pragma once
#include "persist.h"
#include "st/st_disk.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace x2::st {

class StMachine;

struct RunOptions {
    bool headless = false;
    uint32_t frames = 500;       // 0 = until the window closes
    std::string disk_path;       // a plain .st image (--st-disk); wins over assets_dir
    std::string assets_dir;      // the imported assets-st folder
    std::string dump_dir;        // final.png and the run summary
    std::vector<std::pair<uint32_t, std::string>> screenshots; // frame, file
    std::vector<uint32_t> hash_frames;
    uint32_t hash_every = 0;
    uint32_t report_every = 0;
    int scale = 3;
    bool fullscreen = false;
    uint32_t fdc_speed = 1;      // 1 = a real drive; N = N times faster
    bool io_summary = false;
    bool fdc_log = false;
    uint32_t trace_tail = 0;     // on a fault, print the last N executed PCs
    // Scripted joystick input: frame, state byte (bit 0 up, 1 down, 2 left, 3 right, 7 fire), IKBD port 1.
    std::vector<std::pair<uint32_t, uint8_t>> joystick;
    // A frame-indexed input script (st/st_input.h): the project's format, or a Hatari harness script. Its `frames N` line sets
    // the frame count unless the count was given explicitly.
    std::string input_script;
    bool frames_explicit = false;
    // Sound (the YM2149 at `audio_rate` Hz, mono duplicated to stereo). Headless runs make none unless asked (audio_hash,
    // audio_out); a window plays it unless no_audio.
    bool no_audio = false;
    int volume = 100;            // live output volume, percent
    int audio_latency = 40;      // live output buffer, ms
    uint32_t audio_rate = 48000;
    std::string audio_out;       // write the sound to this WAV file (stereo, 16-bit)
    bool audio_hash = false;     // print 'audiohash F <fnv1a64>' with every framehash
    // Window input: <data_dir>input.cfg holds the key bindings (shared with the Amiga build); empty = the defaults.
    std::string data_dir;
    bool input_debug = false;
    // The in-game menu (ui/overlay.h), window runs only: the settings in force at the start (settings.cfg with the command line over it),
    // settings.cfg as it is (what the menu's changes are written into) and where it lives ("" = nothing is saved).
    Settings settings, file_settings;
    std::string settings_path;
    std::string menu_script;     // --menu-script (a test): FRAME:KEY,KEY,... pressed in the window, see ui/menu_script.h
    // RAM word addresses to watch: a line "watch FRAME $ADDR=$VALUE ..." is printed after every frame in which one changes.
    std::vector<uint32_t> watch;
    // A Hatari harness script's VBL numbers are shifted by this many frames (our run starts earlier than Hatari's EmuTOS boot).
    int input_offset = 0;
    // Per-frame comparison with Hatari: a log line per completed frame in the harness's frames.log format ("<n> pc= sr=
    // vbase= <addr>=<word>..." for the words in `watch`, n counting completed frames from 1) and PNGs of the completed frames
    // in `shots` ("100,200-260,1000-2000/50": a frame, a range, every n-th) written as <shots_dir>/shot_<n>.png.
    std::string frame_log;
    // An executed-instruction histogram over frames [profile_from, profile_to) (1-based, as the frame log): the busiest PCs.
    uint32_t profile_from = 0, profile_to = 0;
    // Whole-RAM dumps after the given completed frame (1-based, like the frame log): frame, file (--st-ram-dump-at).
    std::vector<std::pair<uint32_t, std::string>> ram_dumps;
    std::string shots_dir, shots;
    // The window (modern/present.h options reused where they apply to a 320x200 picture).
    VideoFilter filter = VideoFilter::Nearest;
    bool aspect = false, integer = true, curvature = false;
    // COVERAGE ONLY (like x2::Poke), from --poke on the command line; a script's poke lines (st/st_input.h) are applied the same way. A
    // write at the start of frames from..to (size 1, 2 or 4, big-endian); with a handler, addr is an offset written into every
    // hittable object whose hit handler (+$A) is that address.
    struct Poke { uint32_t from, to, addr, value; int size; uint32_t handler; };
    std::vector<Poke> pokes;
    // Assists, cheats and persistence (st_game.h). Autofire is the shared input layer's (input.h AutofirePolicy), window only.
    bool god_mode = false, cheat_scores = false, autofire = false;
    int autofire_rate = 17;
    int credits = 3;
    std::string persist_dir;     // high scores in <dir>/highscores-st.bin ("" = off)
    bool state_log = false;      // print the game state at the end of each frame (probe)
    // --st-watch-pc ADDR[:LEN]: print the frame and the PC of the instruction that changed any byte of the range (a probe).
    uint32_t watch_pc_addr = 0, watch_pc_len = 0;
    // --st-state-hash: with every framehash, print 'statehash F <digest>' (StMachine::state_digest: RAM, registers, cycles, interrupt
    // counts), the strict check of the recompiled executor against the interpreter.
    bool state_hash = false;
    // Coverage tracing (runtime/trace, tools/trace/run_corpus.py --game st): the executed PCs, branch and interrupt targets, code writes,
    // as JSON, with the front end and each level module as modules of their own.
    std::string trace_out, trace_label;
    // The recompiled executor (runtime/recomp, -DX2_RECOMP=ON), attached by the caller so that this library does not depend on it:
    // `attach_cpu` runs for each new machine before it boots and returns false with a reason to stop the run; `after_run` runs once
    // the frames are done (coverage summary, interpreted-PC profile).
    std::function<bool(StMachine&, std::string&)> attach_cpu;
    // Asset packs, screens and the status panel only (modern/screens.h): the pack directories, whether they are used (`upgrade`), the
    // canvas scale they are drawn at, `--dump-assets DIR` (the run's screens and panel cells as a version 2 pack), `--modern-check`
    // (what was drawn against the frame) and `--present-shot F:FILE` (the presented picture of a frame as PNG).
    std::vector<std::string> asset_packs;
    bool upgrade = false;
    int canvas_scale = 3;
    std::string dump_assets;
    bool pack_check = false;
    std::vector<std::pair<uint32_t, std::string>> present_shots;
    std::function<bool(const StMachine&)> after_run;
};

// The imported disk (import/st_game.h: manifest.txt and disk.bin in `dir`, re-imported from its recorded sources when stale).
bool load_st_assets(const std::string& dir, StDisk& out, std::string& why);

// Returns the process exit code: 0 ok, 2 bad arguments, 3 no usable disk, 4 window error, 5 the disk does not boot,
// 6 the machine faulted (a CPU exception landed on the stub vector).
int run(const RunOptions& opt);

// COVERAGE ONLY: apply a poke now; returns the number of writes (1, or the number of matching objects for a handler poke).
// objlist_head is where the game keeps the head pointer of its hittable-object list ($B4C, as in the Amiga disk build).
int apply_poke(StMachine& m, const RunOptions::Poke& p, uint32_t objlist_head = 0xB4C);

// Print the I/O register summary (like tools/acq/stemu.c --io-summary) to stdout.
void print_io_summary(const StMachine& m);

} // namespace x2::st
