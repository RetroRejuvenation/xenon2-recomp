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
#include <string>
#include <utility>
#include <vector>

namespace x2::st {

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
    // Whole-RAM dumps after the given completed frame (1-based, like the frame log): frame, file.
    std::vector<std::pair<uint32_t, std::string>> ram_dumps;
    std::string shots_dir, shots;
    // The window (modern/present.h options reused where they apply to a 320x200 picture).
    VideoFilter filter = VideoFilter::Nearest;
    bool aspect = false, integer = true, curvature = false;
};

// The imported disk (import/st_game.h: manifest.txt and disk.bin in `dir`, re-imported from its recorded sources when stale).
bool load_st_assets(const std::string& dir, StDisk& out, std::string& why);

// Returns the process exit code: 0 ok, 2 bad arguments, 3 no usable disk, 4 window error, 5 the disk does not boot,
// 6 the machine faulted (a CPU exception landed on the stub vector).
int run(const RunOptions& opt);

// Print the I/O register summary (like tools/acq/stemu.c --io-summary) to stdout.
class StMachine;
void print_io_summary(const StMachine& m);

} // namespace x2::st
