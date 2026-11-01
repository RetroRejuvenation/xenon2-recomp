// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/st_run.h"

#include "audio/wav_writer.h"
#include "gfx/image_io.h"
#include "import/importer.h"
#include "st/st_input.h"
#include "st/st_machine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <thread>

#ifdef X2_HAVE_SDL
#include "audio/sdl_audio.h"
#include "gfx/sdl_display.h"
#include "input/input.h"
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

// "100,200-260,1000-2000/50" -> a predicate over frame numbers.
struct FrameSet {
    struct Range { uint32_t from, to, step; };
    std::vector<Range> r;
    bool parse(const std::string& s) {
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            if (tok.empty()) continue;
            Range x{0, 0, 1};
            char* end = nullptr;
            x.from = uint32_t(std::strtoul(tok.c_str(), &end, 10));
            x.to = x.from;
            if (*end == '-') x.to = uint32_t(std::strtoul(end + 1, &end, 10));
            if (*end == '/') x.step = std::max<uint32_t>(1, uint32_t(std::strtoul(end + 1, &end, 10)));
            if (x.to < x.from || r.size() >= 4096) return false;
            r.push_back(x);
        }
        return true;
    }
    bool has(uint32_t n) const {
        for (const Range& x : r)
            if (n >= x.from && n <= x.to && (n - x.from) % x.step == 0) return true;
        return false;
    }
};

std::string ext_of(const std::string& p) {
    std::string e = fs::path(p).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

// <dir>/manifest.txt (key=value lines, import/st_game.h) and <dir>/disk.bin: the logical image with its geometry.
bool load_assets_impl(const std::string& dir, StDisk& out, std::string& why) {
    if (!x2::import::ensure_imported(dir, why)) {
        why = "no usable imported ST disk in " + dir + " (" + why + "); import it first: x2recomp --import DISK.st";
        return false;
    }
    std::map<std::string, std::string> kv;
    {
        std::ifstream mf(fs::path(dir) / "manifest.txt");
        std::string line;
        while (std::getline(mf, line)) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    auto num = [&](const char* k) { auto it = kv.find(k); return it == kv.end() ? 0 : std::atoi(it->second.c_str()); };
    if (kv["game"] != "st" || num("sector_bytes") != 512) { why = dir + " is not an Atari ST import"; return false; }
    StDisk d;
    d.sides = num("sides");
    d.sectors_per_track = num("sectors_per_track");
    d.tracks = num("cylinders");
    if (!read_file((fs::path(dir) / "disk.bin").string(), d.data, why)) return false;
    if (!d.valid()) { why = dir + ": disk.bin does not match the geometry in the manifest"; return false; }
    out = std::move(d);
    return true;
}
} // namespace

bool load_st_assets(const std::string& dir, StDisk& out, std::string& why) { return load_assets_impl(dir, out, why); }

void print_io_summary(const StMachine& m) {
    {
        std::map<uint8_t, int> seen;
        for (uint8_t c : m.ikbd().commands_seen()) ++seen[c];
        std::printf("== IKBD commands sent by the program (command: count)\n ");
        for (const auto& [c, n] : seen) std::printf(" $%02X: %d", c, n);
        std::printf("\n  first ones:");
        for (size_t i = 0; i < m.ikbd().commands_seen().size() && i < 24; ++i) std::printf(" $%02X", m.ikbd().commands_seen()[i]);
        std::printf("\n");
    }
    std::printf("== ST I/O registers touched (address, reads, writes, first reading pc, first writing pc, last value written)\n");
    for (const auto& [addr, s] : m.io_summary())
        std::printf("  $%06X  r=%-9" PRIu64 " w=%-9" PRIu64 " rpc=$%06X wpc=$%06X last=$%X\n", addr, s.reads, s.writes, s.first_read_pc,
                    s.first_write_pc, s.last_value);
}

int run(const RunOptions& opt) {
    StDisk disk;
    std::string why, disk_name;
    if (!opt.disk_path.empty()) {
        const std::string e = ext_of(opt.disk_path);
        if (e == ".stx" || e == ".msa") {
            std::fprintf(stderr, "x2recomp: %s: --st-disk takes a plain .st image; import %s files with --import\n", opt.disk_path.c_str(),
                         e == ".stx" ? "Pasti .stx" : "MSA");
            return 3;
        }
        std::vector<uint8_t> bytes;
        if (!read_file(opt.disk_path, bytes, why)) { std::fprintf(stderr, "x2recomp: %s\n", why.c_str()); return 3; }
        if (!make_st_disk(std::move(bytes), disk, why)) {
            std::fprintf(stderr, "x2recomp: %s: %s\n", opt.disk_path.c_str(), why.c_str());
            return 3;
        }
        disk_name = opt.disk_path;
    } else if (!opt.assets_dir.empty()) {
        if (!load_st_assets(opt.assets_dir, disk, why)) { std::fprintf(stderr, "x2recomp: %s\n", why.c_str()); return 3; }
        disk_name = opt.assets_dir;
    } else {
        std::fprintf(stderr, "x2recomp: --game st needs an imported Atari ST disk (x2recomp --import DISK.st) or --st-disk FILE.st\n");
        return 3;
    }
    StScript script;
    if (!opt.input_script.empty()) {
        std::string serr;
        if (!load_st_script(opt.input_script, script, serr)) {
            std::fprintf(stderr, "x2recomp: %s\n", serr.c_str());
            return 2;
        }
    }
    ScriptPlayer player(script);
    const bool want_audio = opt.audio_hash || !opt.audio_out.empty() || (!opt.headless && !opt.no_audio);
    StConfig cfg;
    cfg.fdc_speed = opt.fdc_speed;
    cfg.audio_rate = want_audio ? opt.audio_rate : 0;
    StMachine m(cfg);
    m.insert_disk(std::move(disk));
    if (opt.io_summary) m.enable_io_summary();
    if (opt.fdc_log) m.fdc().log_reads(true);
    std::vector<uint32_t> tail(opt.trace_tail ? opt.trace_tail : 1, 0);
    uint64_t tail_n = 0;
    std::map<uint32_t, uint64_t> hist;
    if (opt.trace_tail || opt.profile_to)
        m.set_instruction_probe([&](StMachine& mm, uint32_t pc) {
            if (opt.trace_tail) tail[tail_n++ % tail.size()] = pc;
            if (opt.profile_to && mm.frame() + 1 >= opt.profile_from && mm.frame() + 1 < opt.profile_to) ++hist[pc];
        });
    auto print_tail = [&] {
        if (tail.empty()) return;
        std::fprintf(stderr, "  last %llu PCs:", (unsigned long long)std::min<uint64_t>(tail_n, tail.size()));
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
        std::fprintf(stderr, "x2recomp: %s: %s\n", disk_name.c_str(), why.c_str());
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
    if (script.frames_set && !opt.frames_explicit) total = script.frames;
    auto joy = opt.joystick;
    std::sort(joy.begin(), joy.end());
    size_t joy_next = 0;
    FrameSet shot_set;
    if (!opt.shots.empty() && !shot_set.parse(opt.shots)) {
        std::fprintf(stderr, "x2recomp: bad --st-shots '%s' (frames, ranges and /step, e.g. 100,200-260,1000-2000/50)\n", opt.shots.c_str());
        return 2;
    }
    if (!opt.shots_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opt.shots_dir, ec);
        if (ec) { std::fprintf(stderr, "x2recomp: cannot create '%s': %s\n", opt.shots_dir.c_str(), ec.message().c_str()); return 2; }
    }
    FILE* flog = nullptr;
    if (!opt.frame_log.empty()) {
        flog = std::fopen(opt.frame_log.c_str(), "w");
        if (!flog) { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.frame_log.c_str()); return 2; }
    }
    auto apply_script = [&](uint32_t f) {
        while (joy_next < joy.size() && joy[joy_next].first <= f) m.set_joystick(1, joy[joy_next++].second);
        player.apply(m, uint32_t(std::max<int64_t>(0, int64_t(f) + opt.input_offset)));
    };
    // Sound: the YM output of each frame goes to the hash, the WAV file and the live device, as stereo (mono duplicated).
    WavWriter wav;
    if (!opt.audio_out.empty() && !wav.open(opt.audio_out, int(opt.audio_rate), 2)) {
        std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.audio_out.c_str());
        return 2;
    }
    uint64_t audio_hash = 0xcbf29ce484222325ull;
    std::vector<int16_t> stereo;
    auto frame_audio = [&]() -> const std::vector<int16_t>& {
        const auto& mono = m.audio();
        stereo.resize(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
        if (opt.audio_hash)
            for (int16_t smp : stereo)
                for (int i = 0; i < 2; ++i) {
                    audio_hash ^= uint8_t(uint16_t(smp) >> (8 * i));
                    audio_hash *= 0x100000001b3ull;
                }
        if (wav.is_open()) wav.write(stereo.data(), stereo.size());
        return stereo;
    };
    std::vector<int> watch_last(opt.watch.size(), -1);
    auto after_frame = [&](uint32_t f) {
        if (!opt.watch.empty()) {
            std::string chg;
            for (size_t i = 0; i < opt.watch.size(); ++i) {
                const uint32_t a = opt.watch[i];
                if (a + 2 > m.ram_size()) continue;
                const int v = int(m.ram()[a]) << 8 | m.ram()[a + 1];
                if (v != watch_last[i]) {
                    watch_last[i] = v;
                    char buf[40];
                    std::snprintf(buf, sizeof buf, " $%X=$%04X", a, v);
                    chg += buf;
                }
            }
            if (!chg.empty()) std::printf("watch %u%s\n", f, chg.c_str());
        }
        if (opt.report_every && (f % opt.report_every) == 0)
            std::printf("frame %u pc=$%06X vbl=%" PRIu64 " mfp=%" PRIu64 " fdc-cmds=%" PRIu64 " sectors=%zu unmapped=%" PRIu64 "\n", f, m.pc(),
                        m.vbl_taken(), m.mfp_taken(), m.fdc().commands_issued(), m.fdc().read_log().size(), m.unmapped_accesses());
        const bool hash_now = (opt.hash_every && (f % opt.hash_every) == 0) ||
                              std::find(opt.hash_frames.begin(), opt.hash_frames.end(), f) != opt.hash_frames.end();
        if (hash_now) {
            std::printf("framehash %u %016" PRIx64 "\n", f, m.picture_hash());
            if (opt.audio_hash) std::printf("audiohash %u %016" PRIx64 "\n", f, audio_hash);
        }
        if (flog) {
            std::fprintf(flog, "%u pc=%06x sr=%04x vbase=%06x", f + 1, m.pc(), m.sr(), m.shifter().base());
            for (uint32_t a : opt.watch) std::fprintf(flog, " %x=%04x", a, (m.read32_ram(a & ~1u) >> 16) & 0xFFFF);
            std::fprintf(flog, "\n");
        }
        for (const auto& [rf, path] : opt.ram_dumps)
            if (rf == f + 1) {
                std::ofstream o(path, std::ios::binary);
                o.write(reinterpret_cast<const char*>(m.ram()), std::streamsize(m.ram_size()));
                if (!o) std::fprintf(stderr, "x2recomp: cannot write %s\n", path.c_str());
            }
        if (!opt.shots_dir.empty() && shot_set.has(f + 1)) {
            char name[64];
            std::snprintf(name, sizeof name, "/shot_%06u.png", f + 1);
            const std::string path = opt.shots_dir + name;
            if (!write_image(path, m.picture(), kFbWidth, kFbHeight)) std::fprintf(stderr, "x2recomp: cannot write %s\n", path.c_str());
        }
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
            if (want_audio) frame_audio();
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
        if (!display.open("Xenon 2 Megablast (Atari ST, in development) - x2recomp", kFbWidth, kFbHeight, opt.scale, opt.fullscreen, opt.aspect, derr)) {
            std::fprintf(stderr, "x2recomp: cannot open window: %s\n", derr.c_str());
            SDL_Quit();
            return 4;
        }
        View view;
        view.filter = opt.filter;
        view.aspect = opt.aspect;
        view.integer = opt.integer;
        view.curvature = opt.curvature;
        const auto period = std::chrono::nanoseconds(uint64_t(kCyclesPerFrame) * 1000000000ull / kCpuHz);
        auto next = std::chrono::steady_clock::now();
        // Host input: the key bindings are shared with the Amiga build (input.cfg). The game's joystick is IKBD port 1, fed by the
        // primary (Amiga port 2) bindings; the second set drives port 0. P, Esc, Space and the other game keys go as ST scancodes.
        x2::input::LoadedBindings bind;
        if (!opt.data_dir.empty()) bind = x2::input::load_bindings(opt.data_dir);
        else bind.bindings = x2::input::default_bindings();
        if (!bind.error.empty()) std::fprintf(stderr, "x2recomp: %s: %s", bind.path.c_str(), bind.error.c_str());
        if (!bind.path.empty()) std::printf("input bindings: %s%s%s\n", bind.path.c_str(), bind.note.empty() ? "" : ": ", bind.note.c_str());
        x2::input::InputManager input(bind.bindings);
        input.set_debug(opt.input_debug);
        SdlAudioOut audio_out;
        int volume = opt.volume;
        if (want_audio) {
            std::string aerr;
            if (audio_out.open(int(opt.audio_rate), opt.audio_latency, aerr)) audio_out.set_volume(float(volume) / 100.0f);
            else std::fprintf(stderr, "x2recomp: no sound: %s\n", aerr.c_str());
        }
        bool quit = false, paused = false;
        for (uint32_t f = 0; !quit && (total == 0 || f < total); ++f) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) quit = true;
                const x2::input::HostCommand cmd = input.handle_event(ev);
                switch (cmd) {
                case x2::input::HostCommand::Quit: quit = true; break;
                case x2::input::HostCommand::Fullscreen: display.toggle_fullscreen(); break;
                case x2::input::HostCommand::Pause:
                    paused = !paused;
                    audio_out.set_paused(paused);
                    break;
                case x2::input::HostCommand::VolumeDown:
                case x2::input::HostCommand::VolumeUp:
                    volume = std::clamp((volume + (cmd == x2::input::HostCommand::VolumeUp ? 10 : -10)) / 10 * 10, 0, 100);
                    audio_out.set_volume(float(volume) / 100.0f);
                    std::printf("volume %d%%\n", volume);
                    break;
                default: break;
                }
            }
            if (quit) break;
            if (paused) { // the frame is not run; keep the window alive
                --f;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                next = std::chrono::steady_clock::now();
                continue;
            }
            const x2::input::FrameInput in = input.collect(f);
            m.set_joystick(1, joy_bits(in.p2.up, in.p2.down, in.p2.left, in.p2.right, in.p2.fire));
            m.set_joystick(0, joy_bits(in.p1.up, in.p1.down, in.p1.left, in.p1.right, in.p1.fire));
            for (const auto& k : in.keys)
                if (const uint8_t sc = st_scancode_for_amiga_raw(k.first)) m.key_event(sc, k.second);
            apply_script(f);
            m.run_frame();
            const std::vector<int16_t>* snd = want_audio ? &frame_audio() : nullptr;
            if (snd && audio_out.is_open()) audio_out.push(snd->data(), snd->size() / 2);
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
        if (audio_out.is_open())
            std::printf("audio: %" PRIu64 " underruns, %" PRIu64 " dropped frames\n", audio_out.underruns(), audio_out.dropped_frames());
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
    if (flog) std::fclose(flog);
    if (opt.profile_to) {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        uint64_t total = 0;
        for (const auto& [pc, n] : hist) { top.push_back({n, pc}); total += n; }
        std::sort(top.rbegin(), top.rend());
        std::printf("== instructions executed in frames [%u,%u): %" PRIu64 " (the busiest PCs: count, share)\n", opt.profile_from, opt.profile_to, total);
        for (size_t i = 0; i < top.size() && i < 40; ++i)
            std::printf("  $%06X  %-10" PRIu64 " %.2f%%\n", top[i].second, top[i].first, 100.0 * double(top[i].first) / double(total ? total : 1));
    }
    if (opt.io_summary) print_io_summary(m);
    std::printf("bus rounding: %" PRIu64 " instructions rounded up, %" PRIu64 " cycles added\n", m.rounded_instructions(), m.rounding_cycles());
    if (!opt.dump_dir.empty()) write_png(opt.dump_dir + "/final.png", m.picture(), kFbWidth, kFbHeight);
    return rc;
}

} // namespace x2::st
