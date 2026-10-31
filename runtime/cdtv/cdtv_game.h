// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// What the CDTV release of Xenon 2 needs from the machine, answered by PC traps instead of an operating system.
//
// The importer links the CDTV program (`X2`, an AmigaDOS hunk file) at $400 and stores the disc's files next to it
// (import/cdtv_game.h). Once started it takes over the hardware like the disk build does, and the only things it still asks
// the OS for are small and local (docs/re-notes/cdtv-c0.md, section 7.2):
//   * a start-up block that opens libraries and the CD device and builds a message port: skipped;
//   * ten file loads (dos Open/Read/Close of `CD0:NAME`): answered from the files the importer stored;
//   * five CD-audio routines (play a track, stop, the once-a-second poll, volume): mapped onto the runtime's CD player.
// Nothing here modifies the image: every replacement is a trap at the address of the instruction or routine it replaces,
// the idiom of DiskShim and retail_protection, so the translated executor and the interpreter see the same thing.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace x2 {
class Machine;
struct GameAssets;
} // namespace x2

namespace x2::cdtv {

class CdtvMusic;

// The files of the disc as the game opens them. Pure logic, so it can be tested without a machine.
class DiscFiles {
public:
    // Indexes every regular file of `dir` (the importer's cd0/ folder). False with a reason if there is none.
    bool index(const std::string& dir, std::string& why);
    // The key a name is looked up by: an optional `CD0:` (any device prefix up to ':'), then upper case, `;version` dropped.
    static std::string key_of(const std::string& name);
    size_t file_count() const { return paths_.size(); }
    bool has(const std::string& name) const { return paths_.count(key_of(name)) != 0; }

    // dos Open of an existing file: a non-zero handle, or 0 if there is no such file (the name is remembered in unknown()).
    uint32_t open(const std::string& name);
    // dos Read: up to `n` bytes from the handle's position into `out` (resized); returns the count, 0 at end of file, -1 for
    // a handle that is not open.
    int32_t read(uint32_t handle, size_t n, std::vector<uint8_t>& out);
    // dos Close; false for a handle that is not open.
    bool close(uint32_t handle);

    const std::vector<std::string>& unknown() const { return unknown_; }
    size_t open_handles() const { return open_.size(); }

private:
    struct Open {
        std::vector<uint8_t> data;
        size_t pos = 0;
    };
    std::map<std::string, std::string> paths_; // key -> file path
    std::map<uint32_t, Open> open_;
    std::vector<std::string> unknown_;
    uint32_t next_ = 1;
};

// What the game asked of the services, for the front end's report and the tests.
struct GameStats {
    uint32_t opens = 0, failed_opens = 0, reads = 0, closes = 0;
    uint64_t bytes = 0;
    uint32_t cd_play = 0, cd_stop = 0, cd_ticks = 0, cd_volume = 0;
    uint32_t cd_polls = 0, cd_restarts = 0; // CheckIO polls the game made, and finished tracks it started again
    uint32_t os_skips = 0;
    int last_track = -1; // the CD track the game last asked for (-1: none playing)
};

// Hunk-relative sites + $400 (the link base), see cdtv-c0.md. Exposed for the tests.
constexpr uint32_t kOsBlockStart = 0x3AA4A, kOsBlockResume = 0x3AAE8;   // OpenLibrary .. OpenDevice: skipped
constexpr uint32_t kPalCheck = 0x3AB2C, kPalResume = 0x3AB40;           // reads ExecBase.VBlankFrequency: skipped (the PAL word stays 3)
constexpr uint32_t kCdStop = 0x3A8F6, kCdPlay = 0x3A924, kCdTick = 0x3A95C, kCdVolumeFull = 0x3A9BA, kCdVolumeMute = 0x3A9E6;
constexpr uint32_t kCdTrackVar = 0x3A8F2; // long: the track playing, -1 for none
constexpr uint32_t kCdPollCounter = 0x3A8F0; // word: ticks to the next CheckIO (the routine's own counter, kept in the game's memory)
constexpr uint16_t kCdPollReload = 0x32;
// One step of the poll's counter exactly as the game does it (`subq.w #1,counter ; bpl rts ; move.w #$32,counter`): the counter
// only runs while a track is selected, starts at 0 (so the first poll comes with the first tick) and then comes round every
// 51 ticks, not 50. Returns true when this tick is a poll.
inline bool cd_poll_step(uint16_t& counter) {
    counter = uint16_t(counter - 1);
    if (!(counter & 0x8000)) return false;
    counter = kCdPollReload;
    return true;
}
constexpr uint32_t kFileSites = 10;
extern const uint32_t kOpenSites[kFileSites], kReadSites[kFileSites], kCloseSites[kFileSites];

class CdtvGame {
public:
    // Checks that the image has the instructions the traps replace (an `X2` other than the one the traps are keyed to is
    // refused), then installs them. `music` receives the CD-audio commands (it may be null: they are then only recorded).
    // `log` (optional) gets one line per file load and CD command.
    bool install(Machine& m, const GameAssets& assets, CdtvMusic* music, std::string& why);
    void set_log(std::FILE* f) { log_ = f; }
    const GameStats& stats() const { return stats_; }
    DiscFiles& files() { return files_; }

private:
    void on_open(Machine& m);
    void on_read(Machine& m);
    void on_close(Machine& m);
    void cd_stop(Machine& m, uint32_t caller);
    void cd_play(Machine& m, int track, uint32_t caller);

    DiscFiles files_;
    GameStats stats_;
    CdtvMusic* music_ = nullptr;
    std::FILE* log_ = nullptr;
    uint32_t handle_of_last_open_ = 0;
};

} // namespace x2::cdtv
