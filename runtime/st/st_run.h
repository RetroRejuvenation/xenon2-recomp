// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// `x2recomp --game st`: loads a disk image, boots it on the StMachine and runs frames, headless (hashes, screenshots,
// an I/O register summary, a run report) or in an SDL window with no sound yet. The ST target is in development
// (docs/re-notes/atari-st-machine.md): until the importer lands (S1) the disk is a plain ".st" file given with --st-disk.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace x2::st {

struct RunOptions {
    bool headless = false;
    uint32_t frames = 500;       // 0 = until the window closes
    std::string disk_path;       // a plain .st image
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
};

// Returns the process exit code: 0 ok, 2 bad arguments, 3 no usable disk, 4 window error, 5 the disk does not boot,
// 6 the machine faulted (a CPU exception landed on the stub vector).
int run(const RunOptions& opt);

// Print the I/O register summary (like tools/acq/stemu.c --io-summary) to stdout.
class StMachine;
void print_io_summary(const StMachine& m);

} // namespace x2::st
