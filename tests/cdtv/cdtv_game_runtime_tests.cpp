// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The CDTV release's runtime services (cdtv/cdtv_game.h) on a SYNTHETIC image that holds only the instructions the traps are
// keyed to: the disc-file service, the OS start-up skip, the dos Open/Read/Close replacements, the CD-audio routines, the boot
// state and the assets loader. No game bytes.
//
//   x2_cdtv_game_runtime_tests WORKDIR
#include "boot.h"
#include "cdtv/cdtv_game.h"
#include "cdtv/cdtv_music.h"
#include "machine.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

using namespace x2;
using namespace x2::cdtv;
namespace fs = std::filesystem;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

void put(std::vector<uint8_t>& img, uint32_t addr, std::initializer_list<uint8_t> bytes) {
    size_t at = addr - kGameBase;
    for (uint8_t b : bytes) img[at++] = b;
}
constexpr uint8_t kLoop0 = 0x60, kLoop1 = 0xFE; // bra.s *: where a trap's continuation lands in these tests

fs::path g_work;

void write_file(const fs::path& p, const std::vector<uint8_t>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

// An image that is nothing like the game except where the traps are keyed: the entry, the program's first instructions, the
// OS block, the PAL check, ten dos call sites (each followed by a loop), and the start of the five CD routines.
GameAssets synthetic(const fs::path& cd0) {
    GameAssets a;
    a.game_image.assign(kCdtvImageSize, 0);
    a.build = &kBuildCdtv;
    a.cd0_dir = cd0.string();
    put(a.game_image, 0x400, {0x4E, 0xF9, 0x00, 0x03, 0xAA, 0x12});
    put(a.game_image, 0x3AA12, {0x23, 0xCF, 0x00, 0x03, 0x3D, 0x4E, 0x4F, 0xF9, 0x00, 0x08, 0x4F, 0x9C});
    put(a.game_image, 0x3AA4A, {0x2C, 0x79, 0x00, 0x00, 0x00, 0x04, 0x43, 0xFA});
    put(a.game_image, 0x3AAE8, {kLoop0, kLoop1});
    put(a.game_image, 0x3AB2C, {0x2C, 0x78, 0x00, 0x04, 0x0C, 0x2E, 0x00, 0x32, 0x02, 0x12, 0x67, 0x08});
    put(a.game_image, 0x3AB40, {kLoop0, kLoop1});
    for (uint32_t i = 0; i < kFileSites; ++i) {
        put(a.game_image, kOpenSites[i], {0x4E, 0xAE, 0xFF, 0xE2, kLoop0, kLoop1});
        put(a.game_image, kReadSites[i], {0x4E, 0xAE, 0xFF, 0xD6, kLoop0, kLoop1});
        put(a.game_image, kCloseSites[i], {0x4E, 0xAE, 0xFF, 0xDC, kLoop0, kLoop1});
    }
    for (uint32_t pc : {kCdStop, kCdPlay, kCdVolumeFull, kCdVolumeMute}) put(a.game_image, pc, {0x2C, 0x79, 0x00, 0x00, 0x00, 0x04});
    put(a.game_image, kCdTick, {0x4A, 0xB9, 0x00, 0x03, 0xA8, 0xF2});
    return a;
}

std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

void make_disc(const fs::path& cd0) {
    fs::remove_all(cd0);
    std::vector<uint8_t> level(1000), intro(300);
    for (size_t i = 0; i < level.size(); ++i) level[i] = uint8_t(i * 7);
    for (size_t i = 0; i < intro.size(); ++i) intro[i] = uint8_t(255 - i);
    write_file(cd0 / "LEVEL1", level);
    write_file(cd0 / "INTRO", intro);
    write_file(cd0 / "SOUNDS00", bytes_of("sounds"));
}

void file_service() {
    const fs::path cd0 = g_work / "files" / "cd0";
    make_disc(cd0);
    // The names the program builds: an optional device, any case, no version suffix.
    CHECK(DiscFiles::key_of("CD0:LEVEL1") == "LEVEL1");
    CHECK(DiscFiles::key_of("cd0:level1") == "LEVEL1");
    CHECK(DiscFiles::key_of("LEVEL1;1") == "LEVEL1");
    CHECK(DiscFiles::key_of("CD0:SPEECH63") == "SPEECH63");
    CHECK(DiscFiles::key_of(":x") == "X");
    DiscFiles f;
    std::string why;
    CHECK(!f.index((g_work / "files" / "missing").string(), why) && !why.empty());
    CHECK(f.index(cd0.string(), why));
    CHECK(f.file_count() == 3);
    CHECK(f.has("CD0:level1") && !f.has("CD0:LEVEL2"));
    CHECK(f.open("CD0:LEVEL2") == 0);
    CHECK(f.unknown().size() == 1 && f.unknown()[0] == "LEVEL2");
    CHECK(f.open("CD0:LEVEL3") == 0 && f.unknown().size() == 2);
    CHECK(f.open("CD0:LEVEL2") == 0 && f.unknown().size() == 2); // each unknown name once
    const uint32_t h = f.open("CD0:LEVEL1");
    CHECK(h != 0);
    std::vector<uint8_t> out;
    CHECK(f.read(h, 400, out) == 400 && out.size() == 400 && out[1] == 7);
    CHECK(f.read(h, 1000, out) == 600 && out.size() == 600 && out[0] == uint8_t(400 * 7)); // a short read at the end
    CHECK(f.read(h, 10, out) == 0 && out.empty());                                          // then end of file
    CHECK(f.read(999, 10, out) == -1);                                                      // a handle that is not open
    const uint32_t h2 = f.open("CD0:LEVEL1");
    CHECK(h2 != h && f.open_handles() == 2);
    CHECK(f.close(h) && !f.close(h) && f.close(h2) && f.open_handles() == 0);
    CHECK(f.read(h, 10, out) == -1);
    CHECK(f.open("CD0:SOUNDS00") != 0);
    DiscFiles empty;
    fs::create_directories(g_work / "files" / "empty");
    CHECK(!empty.index((g_work / "files" / "empty").string(), why));
}

void install_refusals() {
    const fs::path cd0 = g_work / "refuse" / "cd0";
    make_disc(cd0);
    { // a good synthetic image installs
        GameAssets a = synthetic(cd0);
        Machine m(1024 * 1024);
        CdtvGame g;
        std::string why;
        CHECK(g.install(m, a, nullptr, why));
    }
    { // an image that is not X2 as the importer links it
        GameAssets a = synthetic(cd0);
        a.game_image[0] ^= 0xFF;
        Machine m(1024 * 1024);
        CdtvGame g;
        std::string why;
        CHECK(!g.install(m, a, nullptr, why) && !why.empty());
    }
    { // a file loader that does not hold the dos Read call
        GameAssets a = synthetic(cd0);
        put(a.game_image, kReadSites[3] + 2, {0x12, 0x34});
        Machine m(1024 * 1024);
        CdtvGame g;
        std::string why;
        CHECK(!g.install(m, a, nullptr, why) && why.find("loader 3") != std::string::npos);
    }
    { // a CD routine that does not start where the trap expects it
        GameAssets a = synthetic(cd0);
        put(a.game_image, kCdPlay, {0x00, 0x00});
        Machine m(1024 * 1024);
        CdtvGame g;
        std::string why;
        CHECK(!g.install(m, a, nullptr, why));
    }
    { // no disc files
        GameAssets a = synthetic(g_work / "refuse" / "nowhere");
        Machine m(1024 * 1024);
        CdtvGame g;
        std::string why;
        CHECK(!g.install(m, a, nullptr, why) && why.find("missing") != std::string::npos);
    }
}

// Starts the CPU at `pc` with the stack set up so that a trap's rts lands on a loop at $1000.
void start_at(Machine& m, uint32_t pc) {
    m.start_cpu(pc, 0x2000, 0x2000);
    m.write32(0x1FFC, 0x1000);
    m.write16(0x1000, 0x60FE);
    m.set_areg(7, 0x1FFC);
}

void traps() {
    const fs::path cd0 = g_work / "traps" / "cd0";
    make_disc(cd0);
    GameAssets a = synthetic(cd0);
    Machine m(1024 * 1024);
    boot_game(m, a);
    CdtvMusic music;
    music.attach_game(m);
    CdtvGame g;
    std::string why;
    CHECK(g.install(m, a, &music, why));

    // The OS start-up block is skipped to its resume address, and so is the PAL check (which would read ExecBase).
    start_at(m, kOsBlockStart);
    m.run_cycles(2000);
    CHECK((m.pc() & 0xFFFFFF) == kOsBlockResume && g.stats().os_skips == 1);
    start_at(m, kPalCheck);
    m.run_cycles(2000);
    CHECK((m.pc() & 0xFFFFFF) == kPalResume);

    // Open: d1 = the name, d0 = a handle, the call itself (4 bytes) is not executed; an unknown name gives 0.
    m.load(0x3000, reinterpret_cast<const uint8_t*>("CD0:LEVEL1\0"), 11);
    start_at(m, kOpenSites[2]);
    m.set_dreg(1, 0x3000);
    m.run_cycles(2000);
    const uint32_t handle = m.dreg(0);
    CHECK(handle != 0 && (m.pc() & 0xFFFFFF) == kOpenSites[2] + 4);
    m.load(0x3000, reinterpret_cast<const uint8_t*>("CD0:NOPE\0"), 9);
    start_at(m, kOpenSites[0]);
    m.set_dreg(1, 0x3000);
    m.run_cycles(2000);
    CHECK(m.dreg(0) == 0 && g.stats().failed_opens == 1);

    // Read: d1 = handle, d2 = buffer, d3 = length; d0 = bytes read, the data in chip RAM; then a short read, then end of file.
    start_at(m, kReadSites[2]);
    m.set_dreg(1, handle);
    m.set_dreg(2, 0x50000);
    m.set_dreg(3, 700);
    m.run_cycles(2000);
    CHECK(m.dreg(0) == 700 && (m.pc() & 0xFFFFFF) == kReadSites[2] + 4);
    CHECK(m.read8(0x50000) == 0 && m.read8(0x50001) == 7 && m.read8(0x50000 + 699) == uint8_t(699 * 7));
    start_at(m, kReadSites[2]);
    m.set_dreg(1, handle);
    m.set_dreg(2, 0x60000);
    m.set_dreg(3, 700);
    m.run_cycles(2000);
    CHECK(m.dreg(0) == 300 && m.read8(0x60000) == uint8_t(700 * 7));
    start_at(m, kReadSites[2]);
    m.set_dreg(1, handle);
    m.set_dreg(2, 0x70000);
    m.set_dreg(3, 700);
    m.run_cycles(2000);
    CHECK(m.dreg(0) == 0);
    // A read that would leave chip RAM is cut short, not written past the end.
    const uint32_t h2 = [&] {
        m.load(0x3000, reinterpret_cast<const uint8_t*>("CD0:LEVEL1\0"), 11);
        start_at(m, kOpenSites[2]);
        m.set_dreg(1, 0x3000);
        m.run_cycles(2000);
        return m.dreg(0);
    }();
    start_at(m, kReadSites[2]);
    m.set_dreg(1, h2);
    m.set_dreg(2, 0xFFF00);
    m.set_dreg(3, 700);
    m.run_cycles(2000);
    CHECK(m.dreg(0) == 0x100);
    start_at(m, kCloseSites[2]);
    m.set_dreg(1, handle);
    m.run_cycles(2000);
    CHECK((m.pc() & 0xFFFFFF) == kCloseSites[2] + 4 && g.stats().closes == 1 && g.files().open_handles() == 1);
    CHECK(g.stats().opens == 3 && g.stats().reads == 4);

    // CD audio: play stores the track and cues the player; a second play stops the first; stop clears the variable;
    // the poll and the volume routines return at once.
    m.write32(kCdTrackVar, 0xFFFFFFFFu);
    start_at(m, kCdPlay);
    m.set_dreg(0, 0xABCD0003u); // only the low word is the track
    m.run_cycles(2000);
    CHECK(m.read32(kCdTrackVar) == 3 && (m.pc() & 0xFFFFFF) == 0x1000 && music.cues() == 1 && g.stats().last_track == 3);
    start_at(m, kCdPlay);
    m.set_dreg(0, 5);
    m.run_cycles(2000);
    CHECK(m.read32(kCdTrackVar) == 5 && music.cues() == 3 && g.stats().cd_play == 2 && g.stats().cd_stop == 1);
    start_at(m, kCdStop);
    m.run_cycles(2000);
    CHECK(int32_t(m.read32(kCdTrackVar)) == -1 && music.cues() == 4 && g.stats().last_track == -1);
    start_at(m, kCdStop); // nothing playing: the routine does nothing
    m.run_cycles(2000);
    CHECK(music.cues() == 4 && (m.pc() & 0xFFFFFF) == 0x1000);
    start_at(m, kCdTick);
    m.run_cycles(2000);
    CHECK((m.pc() & 0xFFFFFF) == 0x1000 && g.stats().cd_ticks == 1);
    start_at(m, kCdVolumeMute);
    m.run_cycles(2000);
    start_at(m, kCdVolumeFull);
    m.run_cycles(2000);
    CHECK((m.pc() & 0xFFFFFF) == 0x1000 && g.stats().cd_volume == 2);
}

void boot_and_assets() {
    const fs::path dir = g_work / "assets";
    fs::remove_all(dir);
    make_disc(dir / "cd0");
    GameAssets a = synthetic(dir / "cd0");
    write_file(dir / "game_0400.bin", a.game_image);
    write_file(dir / "manifest.txt", bytes_of("importer_version=3\nbuild=cdtv\nchip_kb=1024\n"));
    GameAssets got;
    std::string err;
    CHECK(load_game_assets(dir.string(), got, err));
    CHECK(got.build == &kBuildCdtv && got.disc2.empty() && got.game_image.size() == kCdtvImageSize);
    CHECK(got.cd0_dir == dir.string() + "/cd0");
    CHECK(find_build("cdtv") == &kBuildCdtv);
    CHECK(kBuildCdtv.chip_kb == 1024 && kBuildCdtv.cdtv && kBuildCdtv.load_sectors_pc == 0 && kBuildCdtv.restart_pc == 0);
    CHECK(kBuildCdtv.var_energy == 0xCCC && kBuildCdtv.var_ships == 0xCD0 && kBuildCdtv.var_credits == 0xCF6 && kBuildCdtv.var_pushback == 0x434);
    // The disk builds keep their own variables.
    CHECK(kBuildCanonical.var_energy == 0xCC6 && kBuildCanonical.var_ships == 0xCCA && kBuildCanonical.var_credits == 0xCF0);

    // What boot leaves for the program: the image at $400, the level-3 stand-in in the vector, VERTB enabled, interrupts open.
    Machine m(1024 * 1024);
    boot_game(m, got);
    CHECK(m.read32(0x6C) == kCdtvOsLevel3Stub);
    CHECK((m.read16(0xDFF01C) & 0x4020) == 0x4020); // INTENAR: master and VERTB
    CHECK((m.sr() & 0x0700) == 0);                    // interrupt mask 0, as the OS left it
    CHECK(m.read16(0x400) == 0x4EF9);
    CHECK(m.restart_pc == 0);

    { // a wrong size, or an image that is not X2, is refused with its own message
        write_file(dir / "game_0400.bin", std::vector<uint8_t>(1000, 0));
        GameAssets x;
        CHECK(!load_game_assets(dir.string(), x, err) && err.find("expected 809564") != std::string::npos);
        std::vector<uint8_t> other(kCdtvImageSize, 0);
        write_file(dir / "game_0400.bin", other);
        CHECK(!load_game_assets(dir.string(), x, err) && err.find("not the CDTV release") != std::string::npos);
    }
    CHECK(is_cdtv_image(a.game_image.data(), a.game_image.size()));
    CHECK(!is_cdtv_image(a.game_image.data(), a.game_image.size() - 1));
}

// The game's poll counter, step by step as `subq.w #1,counter ; bpl rts ; move.w #$32,counter` does it.
void poll_counter() {
    uint16_t c = 0; // the game's memory starts zeroed: the first poll comes with the first tick
    CHECK(cd_poll_step(c) && c == 0x32);
    int ticks_to_next = 0;
    bool polled = false;
    while (!polled && ticks_to_next < 200) {
        ++ticks_to_next;
        polled = cd_poll_step(c);
    }
    CHECK(polled && ticks_to_next == 51); // 50 ticks of countdown, then the one that finds it negative: every 51 ticks
    for (int round = 0; round < 3; ++round) {
        int n = 0;
        bool p = false;
        while (!p && n < 200) { ++n; p = cd_poll_step(c); }
        CHECK(n == 51);
    }
    c = 0xFFFF; // already negative: due at once
    CHECK(cd_poll_step(c) && c == 0x32);
    c = 1;
    CHECK(!cd_poll_step(c) && c == 0);
    CHECK(cd_poll_step(c) && c == 0x32); // 0 - 1 = $FFFF
}

// A synthetic disc: raw 44.1 kHz stereo PCM files of `frames` frames, a ramp so a restart is visible in the position.
Disc synthetic_disc(const fs::path& dir, int track, uint64_t frames) {
    std::vector<uint8_t> pcm(size_t(frames) * 4);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t v = int16_t(8000 * ((i / 40) % 2 ? 1 : -1));
        pcm[i * 4] = uint8_t(v & 0xFF); pcm[i * 4 + 1] = uint8_t(v >> 8);
        pcm[i * 4 + 2] = pcm[i * 4]; pcm[i * 4 + 3] = pcm[i * 4 + 1];
    }
    const fs::path f = dir / ("track" + std::to_string(track) + ".pcm");
    write_file(f, pcm);
    Disc d;
    TrackSource t;
    t.number = track;
    t.path = f.string();
    t.frames = frames;
    d.tracks.push_back(t);
    return d;
}

// The loop mechanism end to end on the music layer: play, the track ends, the game's poll finds it finished at its next
// due tick and plays it again. 0.68 s of CD (34 frames) against polls every 51 frames.
void poll_loop() {
    const fs::path dir = g_work / "poll";
    Disc disc = synthetic_disc(dir, 3, 30000);
    Machine m(1024 * 1024);
    m.paula.set_output_enabled(true); // the CD is mixed into Paula's output frames, which exist only while it is produced
    m.start_cpu(0x1000, 0x2000, 0x2000);
    m.write16(0x1000, 0x60FE);
    CdtvMusic music;
    music.attach_game(m);
    music.set_disc(&disc);
    music.set_source(MusicSource::Cdtv);
    music.player().configure(48000, 0);
    CHECK(music.effective_source() == MusicSource::Cdtv);

    uint16_t counter = 0;
    std::vector<int> restarts; // frames at which the poll restarted the track
    auto frame = [&](int f, bool tick) {
        if (tick && cd_poll_step(counter) && music.cue_poll(m, 3, 0x350B8)) restarts.push_back(f);
        m.run_frame();
        music.after_frame(m);
    };
    music.cue_play(m, 3);
    CHECK(!music.cue_poll(m, 3)); // a cue is waiting: the request has only just been sent, CheckIO says not done
    frame(0, false);
    CHECK(music.player().track() == 3 && !music.player().finished());
    for (int f = 1; f < 160; ++f) frame(f, true);
    // The track is 0.68 s long: it is finished from about frame 35; the polls fall on frames 1, 52, 103, 154.
    CHECK(restarts.size() == 3); // polls at 1, 52, 103, 154 (every 51): the first finds the track playing, the others find it over
    CHECK(!restarts.empty() && restarts[0] == 52);
    CHECK(restarts.size() >= 2 && restarts[1] == 103 && restarts.back() == 154);
    CHECK(music.restarts() == restarts.size());
    CHECK(!music.cue_poll(m, 4)); // not the playing track: nothing
    music.cue_stop(m);
    frame(160, false);
    CHECK(music.player().track() == 0 && !music.cue_poll(m, 3)); // stopped: the game does not poll, and nothing restarts
    // With the Amiga source (CD off) the poll never restarts anything.
    music.set_source(MusicSource::Amiga);
    CHECK(!music.cue_poll(m, 3));
}

void audio_layer() {
    // The game's own volume command scales the music volume; the cues are queued without a disc (nothing plays).
    Machine m(1024 * 1024);
    CdtvMusic music;
    music.attach_game(m);
    music.set_game_volume(0);
    music.set_game_volume(100);
    music.cue_play(m, 4);
    music.cue_stop(m);
    CHECK(music.cues() == 2);
    music.set_game_volume(500); // clamped
}
} // namespace

int main(int argc, char** argv) {
    g_work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "cdtv_game_runtime_test";
    std::error_code ec;
    fs::remove_all(g_work, ec);
    fs::create_directories(g_work, ec);
    file_service();
    install_refusals();
    traps();
    boot_and_assets();
    audio_layer();
    poll_counter();
    poll_loop();
    std::error_code ec2;
    fs::remove_all(g_work, ec2);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
