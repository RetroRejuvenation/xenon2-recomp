// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/st_run.h"

#include "gfx/image_io.h"
#include "st/st_machine.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#ifdef X2_HAVE_SDL
#include "gfx/sdl_display.h"
#include <SDL3/SDL.h>
#endif

namespace x2::st {

namespace fs = std::filesystem;

namespace {
bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { why = "cannot open " + path; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (out.empty()) { why = path + " is empty"; return false; }
    return true;
}

std::string ext_of(const std::string& p) {
    std::string e = fs::path(p).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}
} // namespace

void print_io_summary(const StMachine& m) {
    std::printf("== ST I/O registers touched (address, reads, writes, first reading pc, first writing pc, last value written)\n");
    for (const auto& [addr, s] : m.io_summary())
        std::printf("  $%06X  r=%-9" PRIu64 " w=%-9" PRIu64 " rpc=$%06X wpc=$%06X last=$%X\n", addr, s.reads, s.writes, s.first_read_pc,
                    s.first_write_pc, s.last_value);
}

int run(const RunOptions& opt) {
    if (opt.disk_path.empty()) {
        std::fprintf(stderr, "x2recomp: --game st needs a disk image: --st-disk FILE.st (the ST importer is not available yet)\n");
        return 3;
    }
    const std::string e = ext_of(opt.disk_path);
    if (e == ".stx" || e == ".msa") {
        std::fprintf(stderr, "x2recomp: %s: only plain .st images can be run for now (the ST importer reads %s)\n", opt.disk_path.c_str(),
                     e == ".stx" ? "Pasti .stx" : "MSA");
        return 3;
    }
    std::vector<uint8_t> bytes;
    std::string why;
    if (!read_file(opt.disk_path, bytes, why)) { std::fprintf(stderr, "x2recomp: %s\n", why.c_str()); return 3; }
    StDisk disk;
    if (!make_st_disk(std::move(bytes), disk, why)) {
        std::fprintf(stderr, "x2recomp: %s: %s\n", opt.disk_path.c_str(), why.c_str());
        return 3;
    }
    StConfig cfg;
    cfg.fdc_speed = opt.fdc_speed;
    StMachine m(cfg);
    m.insert_disk(std::move(disk));
    if (opt.io_summary) m.enable_io_summary();
    if (opt.fdc_log) m.fdc().log_reads(true);
    std::vector<uint32_t> tail(opt.trace_tail, 0);
    uint64_t tail_n = 0;
    if (opt.trace_tail)
        m.set_instruction_probe([&](StMachine&, uint32_t pc) { tail[tail_n++ % tail.size()] = pc; });
    auto print_tail = [&] {
        if (tail.empty()) return;
        std::fprintf(stderr, "  last %zu PCs:", std::min<uint64_t>(tail_n, tail.size()));
        for (uint64_t i = tail_n > tail.size() ? tail_n - tail.size() : 0; i < tail_n; ++i) std::fprintf(stderr, " %06X", tail[i % tail.size()]);
        std::fprintf(stderr, "\n");
        const uint32_t last = tail[(tail_n - 1) % tail.size()];
        if (last + 16 <= m.ram_size()) {
            std::fprintf(stderr, "  bytes at the last PC $%06X:", last);
            for (uint32_t i = 0; i < 16; ++i) std::fprintf(stderr, " %02X", m.ram()[last + i]);
            std::fprintf(stderr, "\n");
        }
        const uint32_t prev = tail_n > 1 ? tail[(tail_n - 2) % tail.size()] : 0;
        if (prev + 16 <= m.ram_size()) {
            std::fprintf(stderr, "  bytes at the one before $%06X:", prev);
            for (uint32_t i = 0; i < 16; ++i) std::fprintf(stderr, " %02X", m.ram()[prev + i]);
            std::fprintf(stderr, "\n");
        }
    };
    if (!m.boot(why)) {
        std::fprintf(stderr, "x2recomp: %s: %s\n", opt.disk_path.c_str(), why.c_str());
        return 5;
    }
    if (!opt.dump_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opt.dump_dir, ec);
        if (ec) {
            std::fprintf(stderr, "x2recomp: cannot create --dump-dir '%s': %s\n", opt.dump_dir.c_str(), ec.message().c_str());
            return 2;
        }
    }
    uint32_t total = opt.frames;
    auto joy = opt.joystick;
    std::sort(joy.begin(), joy.end());
    size_t joy_next = 0;
    auto apply_script = [&](uint32_t f) {
        while (joy_next < joy.size() && joy[joy_next].first <= f) m.set_joystick(1, joy[joy_next++].second);
    };
    auto after_frame = [&](uint32_t f) {
        if (opt.report_every && (f % opt.report_every) == 0)
            std::printf("frame %u pc=$%06X vbl=%" PRIu64 " mfp=%" PRIu64 " fdc-cmds=%" PRIu64 " sectors=%zu unmapped=%" PRIu64 "\n", f, m.pc(),
                        m.vbl_taken(), m.mfp_taken(), m.fdc().commands_issued(), m.fdc().read_log().size(), m.unmapped_accesses());
        const bool hash_now = (opt.hash_every && (f % opt.hash_every) == 0) ||
                              std::find(opt.hash_frames.begin(), opt.hash_frames.end(), f) != opt.hash_frames.end();
        if (hash_now) std::printf("framehash %u %016" PRIx64 "\n", f, m.picture_hash());
        for (const auto& [sf, path] : opt.screenshots)
            if (sf == f) {
                if (write_image(path, m.picture(), kFbWidth, kFbHeight)) std::printf("screenshot of frame %u -> %s\n", f, path.c_str());
                else std::fprintf(stderr, "x2recomp: cannot write %s\n", path.c_str());
            }
    };
    int rc = 0;
    if (opt.headless) {
        for (uint32_t f = 0; f < total; ++f) {
            apply_script(f);
            m.run_frame();
            after_frame(f);
            if (m.fatal_vector_hits()) {
                std::fprintf(stderr, "x2recomp: the program faulted at frame %u (a CPU exception reached the stub vector; pc $%06X)\n  %s\n", f, m.pc(), m.fault_report().c_str());
                print_tail();
                rc = 6;
                break;
            }
        }
    } else {
#ifdef X2_HAVE_SDL
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "x2recomp: SDL_Init failed: %s (use --headless)\n", SDL_GetError());
            return 4;
        }
        SdlDisplay display;
        std::string derr;
        if (!display.open("Xenon 2 Megablast (Atari ST, in development) - x2recomp", kFbWidth, kFbHeight, opt.scale, opt.fullscreen, false, derr)) {
            std::fprintf(stderr, "x2recomp: cannot open window: %s\n", derr.c_str());
            SDL_Quit();
            return 4;
        }
        View view;
        const auto period = std::chrono::nanoseconds(uint64_t(kCyclesPerFrame) * 1000000000ull / kCpuHz);
        auto next = std::chrono::steady_clock::now();
        bool quit = false;
        uint8_t keys = 0;
        for (uint32_t f = 0; !quit && (total == 0 || f < total); ++f) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) quit = true;
                if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
                    const bool down = ev.type == SDL_EVENT_KEY_DOWN;
                    uint8_t bit = 0;
                    switch (ev.key.key) {
                    case SDLK_UP: bit = 0x01; break;
                    case SDLK_DOWN: bit = 0x02; break;
                    case SDLK_LEFT: bit = 0x04; break;
                    case SDLK_RIGHT: bit = 0x08; break;
                    case SDLK_Z: case SDLK_LCTRL: case SDLK_RCTRL: case SDLK_SPACE: bit = 0x80; break;
                    case SDLK_ESCAPE: if (down) quit = true; break;
                    case SDLK_F11: if (down) display.toggle_fullscreen(); break;
                    default: break;
                    }
                    if (bit) {
                        keys = uint8_t(down ? (keys | bit) : (keys & ~bit));
                        m.set_joystick(1, keys);
                    }
                }
            }
            apply_script(f);
            m.run_frame();
            display.present(m.picture().data(), kFbWidth, kFbHeight, 1, view);
            after_frame(f);
            if (m.fatal_vector_hits()) {
                std::fprintf(stderr, "x2recomp: the program faulted at frame %u (pc $%06X)\n", f, m.pc());
                rc = 6;
                break;
            }
            next += period;
            const auto t = std::chrono::steady_clock::now();
            if (next > t) std::this_thread::sleep_until(next);
            else if (t - next > std::chrono::milliseconds(200)) next = t; // catching up after a stall: do not run away
        }
        SDL_Quit();
#else
        std::fprintf(stderr, "x2recomp: built without SDL3, only --headless is available\n");
        return 2;
#endif
    }
    std::printf("st run: %u frames, pc=$%06X, %" PRIu64 " VBLs taken, %" PRIu64 " MFP interrupts, %" PRIu64 " FDC commands, %zu sectors logged, "
                "%" PRIu64 " unmapped accesses, %" PRIu64 " lines with the mask below 2\n",
                m.frame(), m.pc(), m.vbl_taken(), m.mfp_taken(), m.fdc().commands_issued(), m.fdc().read_log().size(), m.unmapped_accesses(),
                m.hbl_unmasked_lines());
    if (opt.io_summary) print_io_summary(m);
    if (!opt.dump_dir.empty()) write_png(opt.dump_dir + "/final.png", m.picture(), kFbWidth, kFbHeight);
    return rc;
}

} // namespace x2::st
