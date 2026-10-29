// x2recomp - Xenon 2 Megablast runtime: interpreter-driven machine model with a scanline renderer, shown in an
// SDL3 window or run headless.
//
//   x2recomp [--assets DIR] [options]                              (window)
//   x2recomp --headless --frames N [--dump-dir DIR] [--assets DIR] [options]
//
// See docs/BUILDING.md for all options and the output format.
#include "audio/audio_cues.h"
#include "audio/wav_writer.h"
#include "boot.h"
#include "debug_dump.h"
#include "disk_shim.h"
#include "gfx/image_io.h"
#include "gfx/pacer.h"
#include "gfx/video.h"
#include "machine.h"
#include "trace/tracer.h"
#ifdef X2_HAVE_RECOMP
#include "recomp/recomp.h"
#endif
#ifdef X2_HAVE_SDL
#include "gfx/sdl_display.h"
#include "audio/sdl_audio.h"
#include "input/input.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "m68k.h"
}

namespace {

struct InputEvent {
    uint32_t frame;
    enum Kind { Key, Fire, Joy } kind;
    uint8_t code;
    uint32_t hold;
    uint8_t dirs = 0; // Joy: 1 up, 2 down, 4 left, 8 right, 16 fire
};

// Coverage-only state poke: write VALUE (1, 2 or 4 bytes, big-endian) to chip RAM ADDR at the start of every
// frame from `from` to `to`. Never applied unless given with --poke or a script "poke" line.
struct Poke {
    uint32_t from, to, addr, value;
    int size;
};

struct Options {
    bool headless = false;
    uint32_t frames = 500; // headless default; a window runs until quit unless --frames is given
    bool frames_set = false, report_set = false;
    std::string dump_dir;
    std::string assets = "generated/assets";
    uint32_t report_every = 50;
    uint32_t ppm_every = 0;
    bool reglog = true;
    std::vector<uint32_t> trace_frames;
    std::vector<InputEvent> input;
    std::vector<Poke> pokes;
    std::string trace_out, trace_label;
    bool audio_hash = false;
    std::vector<std::pair<uint32_t, std::string>> screenshots;
    std::vector<uint32_t> hash_frames;
    uint32_t hash_every = 0;
    bool interp_only = false;   // recomp builds: run everything on the Musashi interpreter
    int recomp_stats = -1;      // recomp builds: print coverage and the N most-interpreted PCs at exit
    std::string recomp_profile; // recomp builds: write interpreted PCs as seeds for tools/recomp/recomp.py
    int scale = 3;
    bool fullscreen = false, aspect = false, no_audio = false;
    std::string audio_out, audio_cues;
    x2::PaulaConfig audio;
};

void usage() {
    std::fprintf(stderr,
                 "usage: x2recomp [--headless] [--frames N] [--dump-dir DIR] [options]\n"
                 "  --headless          no window (the only mode when built without SDL3)\n"
                 "  --scale N           initial window scale (default 3); --fullscreen; --aspect (4:3 picture)\n"
                 "  --no-audio          window without sound output\n"
                 "  --screenshot-at F:FILE  save rendered frame F as FILE (.png, otherwise PPM)\n"
                 "  --hash-at F         print 'framehash F <fnv1a64>' of rendered frame F\n"
                 "  --hash-every N      print 'framehash' for every frame F with F %% N == 0\n"
                 "  --interp-only       recompiled builds: run all code on the Musashi interpreter (A/B comparison)\n"
                 "  --recomp-stats N    recompiled builds: print translated/interpreted counts and the N hottest\n"
                 "                      interpreted PCs at exit\n"
                 "  --recomp-profile FILE  recompiled builds: write interpreted PCs (seeds for tools/recomp/recomp.py)\n"
                 "  --assets DIR        directory with game_0400.bin and disc2.bin (default generated/assets)\n"
                 "  --report-every N    status report interval in frames (default 50)\n"
                 "  --ppm-every N       also dump the bitplane framebuffer every N frames\n"
                 "  --no-reglog         do not write per-frame register write logs\n"
                 "  --trace-frame F     write a disassembly trace of every instruction in frame F\n"
                 "  --key F:CODE        press Amiga raw key CODE (hex) at frame F (released 2 frames later)\n"
                 "  --fire F[:N]        hold joystick fire from frame F for N frames (default 3)\n"
                 "  --joy F:DIRS[:N]    hold joystick directions DIRS (letters U D L R F) from frame F for N frames\n"
                 "  --input-script FILE frame-indexed input script (tests/replays/README.md)\n"
                 "  --poke F[-G]:ADDR:VALUE[.b|.w|.l]  COVERAGE ONLY: write chip RAM at the start of frames F..G\n"
                 "  --trace-out FILE    record executed PCs, branches, indirect targets etc. as JSON (runtime/trace)\n"
                 "  --trace-label NAME  label stored in the trace file (default: the file name)\n"
                 "  --audio-hash        with --hash-at, also print 'audiohash F <fnv1a64>' of all audio up to frame F\n"
                 "  --audio-out FILE    write the Paula output (48 kHz stereo 16-bit WAV) of the whole run\n"
                 "  --audio-filter M    output filter model: a500 (default), a1200, none\n"
                 "  --stereo-sep X      stereo separation 0..1 (default 1 = hard left/right)\n"
                 "  --audio-cues FILE   log music/sound driver calls (frame, line, entry, d0, caller)\n");
}

uint8_t parse_dirs(const std::string& s) {
    uint8_t d = 0;
    for (char c : s) {
        switch (c) {
        case 'U': case 'u': d |= 1; break;
        case 'D': case 'd': d |= 2; break;
        case 'L': case 'l': d |= 4; break;
        case 'R': case 'r': d |= 8; break;
        case 'F': case 'f': d |= 16; break;
        case '-': break;
        default: return 0xFF;
        }
    }
    return d;
}

// "ADDR:VALUE[.b|.w|.l]" with frames already parsed; addresses and values accept 0x/$ hex or decimal.
bool parse_poke_value(const std::string& addr, const std::string& value, Poke& p) {
    auto num = [](std::string t, uint32_t& out) {
        if (!t.empty() && t[0] == '$') t = "0x" + t.substr(1);
        char* end = nullptr;
        out = uint32_t(std::strtoul(t.c_str(), &end, 0));
        return !t.empty() && *end == 0;
    };
    std::string v = value;
    p.size = 2;
    if (v.size() > 2 && v[v.size() - 2] == '.') {
        char s = v.back();
        p.size = s == 'b' ? 1 : s == 'w' ? 2 : s == 'l' ? 4 : 0;
        v.resize(v.size() - 2);
    }
    return p.size && num(addr, p.addr) && num(v, p.value) && p.addr + uint32_t(p.size) <= x2::kChipRamSize;
}

// Input script: one event per line, "#" comments. FRAME is absolute, or "+N" relative to the previous line.
//   FRAME fire [N]           hold fire for N frames (default 3)
//   FRAME key HEX            press Amiga raw key HEX (released 2 frames later)
//   FRAME joy DIRS [N]       hold directions/fire (letters U D L R F) for N frames (default 1)
//   FRAME poke ADDR VALUE[.b|.w|.l] [N]   COVERAGE ONLY: poke for N frames (default 1)
//   frames N                 total frames to run (like --frames)
// fire, key and joy lines may end with "every P until G": the event repeats every P frames up to frame G.
bool load_input_script(const std::string& path, Options& o) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "x2recomp: cannot read %s\n", path.c_str()); return false; }
    std::string line;
    uint32_t last = 0;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::string t0, cmd;
        if (!(ss >> t0)) continue;
        auto fail = [&]() { std::fprintf(stderr, "x2recomp: %s:%d: bad line\n", path.c_str(), lineno); return false; };
        if (t0 == "frames") {
            uint32_t n;
            if (!(ss >> n)) return fail();
            o.frames = n;
            o.frames_set = true;
            continue;
        }
        char* end = nullptr;
        uint32_t f = uint32_t(std::strtoul(t0.c_str() + (t0[0] == '+'), &end, 0));
        if (*end) return fail();
        if (t0[0] == '+') f += last;
        last = f;
        if (!(ss >> cmd)) return fail();
        std::vector<std::string> tok;
        for (std::string t; ss >> t;) tok.push_back(t);
        uint32_t every = 0, until = f;
        for (size_t i = 0; i < tok.size(); ++i) {
            if (tok[i] != "every") continue;
            if (cmd == "poke" || i + 4 != tok.size() || tok[i + 2] != "until") return fail();
            every = uint32_t(std::strtoul(tok[i + 1].c_str(), nullptr, 0));
            until = uint32_t(std::strtoul(tok[i + 3].c_str(), nullptr, 0));
            if (!every || until < f) return fail();
            tok.resize(i);
            break;
        }
        tok.resize(std::max<size_t>(tok.size(), 3));
        const std::string &a = tok[0], &b = tok[1], &c = tok[2];
        size_t first_event = o.input.size();
        auto count = [](const std::string& s, uint32_t def) { return s.empty() ? def : uint32_t(std::strtoul(s.c_str(), nullptr, 0)); };
        if (cmd == "fire") o.input.push_back({f, InputEvent::Fire, 0, count(a, 3)});
        else if (cmd == "key") {
            if (a.empty()) return fail();
            o.input.push_back({f, InputEvent::Key, uint8_t(std::strtoul(a.c_str(), nullptr, 16)), 2});
        } else if (cmd == "joy") {
            InputEvent e{f, InputEvent::Joy, 0, count(b, 1)};
            e.dirs = parse_dirs(a);
            if (a.empty() || e.dirs == 0xFF) return fail();
            o.input.push_back(e);
        } else if (cmd == "poke") {
            Poke p{f, f + count(c, 1) - 1, 0, 0, 2};
            if (!parse_poke_value(a, b, p)) return fail();
            o.pokes.push_back(p);
        } else return fail();
        if (every && o.input.size() == first_event + 1) {
            InputEvent e = o.input.back();
            for (uint32_t g = f + every; g <= until; g += every) { e.frame = g; o.input.push_back(e); }
        }
    }
    return true;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--headless") o.headless = true;
        else if (a == "--frames") { auto v = next(); if (!v) return false; o.frames = uint32_t(std::strtoul(v, nullptr, 0)); o.frames_set = true; }
        else if (a == "--dump-dir") { auto v = next(); if (!v) return false; o.dump_dir = v; }
        else if (a == "--assets") { auto v = next(); if (!v) return false; o.assets = v; }
        else if (a == "--report-every") { auto v = next(); if (!v) return false; o.report_every = uint32_t(std::strtoul(v, nullptr, 0)); o.report_set = true; }
        else if (a == "--ppm-every") { auto v = next(); if (!v) return false; o.ppm_every = uint32_t(std::strtoul(v, nullptr, 0)); }
        else if (a == "--no-reglog") o.reglog = false;
        else if (a == "--scale") { auto v = next(); if (!v) return false; o.scale = std::max(1, std::atoi(v)); }
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--aspect") o.aspect = true;
        else if (a == "--no-audio") o.no_audio = true;
        else if (a == "--hash-every") { auto v = next(); if (!v) return false; o.hash_every = uint32_t(std::strtoul(v, nullptr, 0)); }
        else if (a == "--interp-only") o.interp_only = true;
        else if (a == "--recomp-stats") { auto v = next(); if (!v) return false; o.recomp_stats = std::atoi(v); }
        else if (a == "--recomp-profile") { auto v = next(); if (!v) return false; o.recomp_profile = v; }
        else if (a == "--hash-at") { auto v = next(); if (!v) return false; o.hash_frames.push_back(uint32_t(std::strtoul(v, nullptr, 0))); }
        else if (a == "--screenshot-at") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            uint32_t f = uint32_t(std::strtoul(v, &end, 0));
            if (*end != ':' || !end[1]) return false;
            o.screenshots.push_back({f, end + 1});
        }
        else if (a == "--audio-out") { auto v = next(); if (!v) return false; o.audio_out = v; }
        else if (a == "--audio-cues") { auto v = next(); if (!v) return false; o.audio_cues = v; }
        else if (a == "--stereo-sep") { auto v = next(); if (!v) return false; o.audio.stereo_separation = std::strtof(v, nullptr); }
        else if (a == "--audio-filter") {
            auto v = next();
            if (!v) return false;
            std::string f = v;
            if (f == "a500") o.audio.filter = x2::AudioFilterModel::A500;
            else if (f == "a1200") o.audio.filter = x2::AudioFilterModel::A1200;
            else if (f == "none") o.audio.filter = x2::AudioFilterModel::None;
            else return false;
        }
        else if (a == "--input-script") { auto v = next(); if (!v || !load_input_script(v, o)) return false; }
        else if (a == "--trace-out") { auto v = next(); if (!v) return false; o.trace_out = v; }
        else if (a == "--trace-label") { auto v = next(); if (!v) return false; o.trace_label = v; }
        else if (a == "--audio-hash") o.audio_hash = true;
        else if (a == "--joy") {
            // F:DIRS[:N]
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            InputEvent e{uint32_t(std::strtoul(v, &end, 0)), InputEvent::Joy, 0, 1};
            if (*end != ':') return false;
            std::string rest = end + 1;
            auto colon = rest.find(':');
            e.dirs = parse_dirs(rest.substr(0, colon));
            if (colon != std::string::npos) e.hold = uint32_t(std::strtoul(rest.c_str() + colon + 1, nullptr, 0));
            if (e.dirs == 0xFF) return false;
            o.input.push_back(e);
        } else if (a == "--poke") {
            // F[-G]:ADDR:VALUE[.b|.w|.l]
            auto v = next();
            if (!v) return false;
            std::string s = v;
            auto c1 = s.find(':'), c2 = s.find(':', c1 == std::string::npos ? 0 : c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) return false;
            std::string fr = s.substr(0, c1);
            Poke p{0, 0, 0, 0, 2};
            auto dash = fr.find('-');
            p.from = uint32_t(std::strtoul(fr.c_str(), nullptr, 0));
            p.to = dash == std::string::npos ? p.from : uint32_t(std::strtoul(fr.c_str() + dash + 1, nullptr, 0));
            if (!parse_poke_value(s.substr(c1 + 1, c2 - c1 - 1), s.substr(c2 + 1), p) || p.to < p.from) return false;
            o.pokes.push_back(p);
        }
        else if (a == "--trace-frame") { auto v = next(); if (!v) return false; o.trace_frames.push_back(uint32_t(std::strtoul(v, nullptr, 0))); }
        else if (a == "--key" || a == "--fire") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            InputEvent e{uint32_t(std::strtoul(v, &end, 0)), a == "--key" ? InputEvent::Key : InputEvent::Fire, 0, 3};
            if (*end == ':') {
                if (e.kind == InputEvent::Key) e.code = uint8_t(std::strtoul(end + 1, nullptr, 16));
                else e.hold = uint32_t(std::strtoul(end + 1, nullptr, 0));
            } else if (e.kind == InputEvent::Key) return false;
            o.input.push_back(e);
        } else if (a == "--help" || a == "-h") return false;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    return true;
}

// Names for the most common PCs in reports (docs/re-notes/ghidra-code-map.md).
const char* known_name(uint32_t pc) {
    static const std::map<uint32_t, const char*> names = {
        {0x0FAC, "irq2_cia_keyboard"}, {0x129A, "blit_bob_cookie"}, {0x15CE, "print_string_blit"},
        {0x17D6, "blit_objects_main"}, {0x18B6, "draw_background_a"}, {0x1BD0, "draw_background_b"},
        {0x2026, "wait_frame_swap_copper"}, {0x202E, "wait_frame_swap_copper_alt"}, {0x2098, "irq3_vbl_diskload"},
        {0x20C2, "irq3_vbl"}, {0x6372, "palette_fade"}, {0x7032, "reset_entry"}, {0x763C, "init"},
        {0x78A4, "main_loop"}, {0x7940, "key_dispatch"}, {0x8F9A, "disk_check_discid"},
        {0x8FF2, "level_load_and_run"}, {0x917E, "level_load_if_changed"}, {0x921A, "disk_read_sectors"},
        {0x95E4, "irq1_dskblk"}, {0x19FAC, "music_tick"}, {0x1A154, "music_tick_core"},
    };
    const char* best = "";
    uint32_t best_addr = 0;
    for (auto& [a, n] : names)
        if (a <= pc && a >= best_addr && pc - a < 0x200) { best = n; best_addr = a; }
    return best;
}

struct Milestones {
    long cop1lc = -1, copper_running = -1, bitplanes = -1, vbl_handler = -1, disc_check_ok = -1, main_loop = -1;
};

// Hard reset: stop DMA and interrupts, reload the image and restart the CPU at $400, as at power-on. The game
// initialises all hardware itself from there.
void reset_machine(x2::Machine& m, const x2::GameAssets& assets) {
    m.custom_write(x2::reg::DMACON, 0x7FFF);
    m.custom_write(x2::reg::INTENA, 0x7FFF);
    m.custom_write(x2::reg::INTREQ, 0x7FFF);
    x2::boot_game(m, assets);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) { usage(); return 2; }
#ifndef X2_HAVE_SDL
    if (!opt.headless) {
        std::fprintf(stderr, "x2recomp: built without SDL3, only --headless is available\n");
        usage();
        return 2;
    }
#endif
    if (!opt.headless && !opt.frames_set) opt.frames = 0; // 0 = until quit
    if (!opt.headless && !opt.report_set) opt.report_every = 0;
    if (opt.dump_dir.empty()) opt.reglog = false;

    x2::GameAssets assets;
    std::string err;
    if (!x2::load_game_assets(opt.assets, assets, err)) {
        std::fprintf(stderr, "x2recomp: %s\n", err.c_str());
        return 3;
    }
    namespace fs = std::filesystem;
    if (!opt.dump_dir.empty()) fs::create_directories(opt.dump_dir);
    if (opt.reglog) fs::create_directories(opt.dump_dir + "/regs");

    x2::Machine m;
    x2::DiskShim disk(assets.disc2);
    disk.install(m);
    Milestones ms;
    uint32_t vbl_entries = 0, main_loop_entries = 0, discid_ok = 0, discid_calls = 0;
    // Entry-count probes; PC traps run before the instruction, then execution continues normally.
    m.add_trap(0x20C2, [&](x2::Machine&) { ++vbl_entries; });
    // $78CE (addq.w #1,$CDC): the main loop's back-edge target, reached once per game-logic frame.
    m.add_trap(0x78CE, [&](x2::Machine&) { ++main_loop_entries; });
    // $8FDE: cmpi.l #'X2B ',$49602 right after the shim returns inside the disc-id check.
    m.add_trap(0x8FDE, [&](x2::Machine& mm) {
        ++discid_calls;
        if (mm.read32(0x49602) == 0x58324220) ++discid_ok;
    });
    x2::boot_game(m, assets);
    m.log_writes = opt.reglog;
#ifdef X2_HAVE_RECOMP
    if (!opt.interp_only) {
        std::string rerr;
        if (!x2::recomp::enable(m, rerr)) {
            std::fprintf(stderr, "x2recomp: recompiled code disabled: %s\n", rerr.c_str());
            return 5;
        }
        std::printf("recomp: hybrid executor on, %zu translated instructions\n", x2::recomp::translated_instructions());
    } else {
        std::printf("recomp: --interp-only, Musashi interpreter only\n");
    }
#else
    if (opt.recomp_stats >= 0 || !opt.recomp_profile.empty())
        std::printf("recomp: not built with -DX2_RECOMP=ON, interpreter only\n");
#endif
    x2::Video video;
    video.attach(m);
    std::unique_ptr<x2::Tracer> tracer;
    if (!opt.trace_out.empty()) {
        tracer = std::make_unique<x2::Tracer>(m);
        m.tracer = tracer.get();
        disk.on_load = [&](uint32_t dest, uint16_t sector, uint16_t count, const uint8_t* data) {
            tracer->on_disk_load(dest, sector, count, data);
        };
    }
    if (!opt.pokes.empty())
        std::printf("coverage pokes active: %zu (state is altered; not a faithful run)\n", opt.pokes.size());
    uint64_t audio_hash = 0xcbf29ce484222325ull;
    if (opt.audio_hash) m.paula.set_output_enabled(true);
    constexpr int kW = x2::Video::kFbWidth, kH = x2::Video::kFbHeight;
#ifdef X2_HAVE_SDL
    std::unique_ptr<x2::SdlDisplay> display;
    std::unique_ptr<x2::input::InputManager> input;
    x2::SdlAudioOut audio_out;
    size_t audio_max_queued = 0;
    uint64_t last_frame_ns = 0;
    x2::FramePacer pacer; // 50 Hz
    uint32_t frames_due = 0;
    bool paused = false;
    if (!opt.headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "x2recomp: SDL_Init failed: %s (use --headless)\n", SDL_GetError());
            return 4;
        }
        display = std::make_unique<x2::SdlDisplay>();
        std::string derr;
        if (!display->open("Xenon 2 Megablast", kW, kH, opt.scale, opt.fullscreen, opt.aspect, derr)) {
            std::fprintf(stderr, "x2recomp: cannot open window: %s\n", derr.c_str());
            return 4;
        }
        std::string cfg_path, cfg_err;
        x2::input::Bindings bindings = x2::input::load_bindings(cfg_path, cfg_err);
        if (!cfg_err.empty()) std::fprintf(stderr, "x2recomp: %s: %s", cfg_path.c_str(), cfg_err.c_str());
        std::printf("input bindings: %s\n", cfg_path.c_str());
        input = std::make_unique<x2::input::InputManager>(bindings);
        if (!opt.no_audio) {
            std::string aerr;
            if (audio_out.open(opt.audio.output_rate, 40, aerr)) m.paula.set_output_enabled(true);
            else std::fprintf(stderr, "x2recomp: no audio output: %s\n", aerr.c_str());
        }
        pacer.reset(SDL_GetTicksNS());
    }
#endif
    m.paula.configure(opt.audio);
    x2::WavWriter wav;
    if (!opt.audio_out.empty()) {
        if (!wav.open(opt.audio_out, opt.audio.output_rate, 2)) {
            std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.audio_out.c_str());
            return 3;
        }
        m.paula.set_output_enabled(true);
    }
    FILE* cue_file = nullptr;
    if (!opt.audio_cues.empty()) {
        cue_file = std::fopen(opt.audio_cues.c_str(), "w");
        if (!cue_file) { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.audio_cues.c_str()); return 3; }
        x2::install_audio_cue_log(m, cue_file);
    }

    FILE* trace_file = nullptr;
    std::map<uint32_t, uint32_t> window_pcs;
    std::array<uint32_t, 0x100> window_reads{};
    std::array<uint32_t, 32> window_cia{};
    uint32_t window_start = 0;

    std::printf("x2recomp headless: %u frames, %d lines x %d CPU cycles per frame\n", opt.frames,
                x2::kLinesPerFrame, x2::kCpuCyclesPerLine);
    for (uint32_t f = 0; opt.frames == 0 || f < opt.frames; ++f) {
#ifdef X2_HAVE_SDL
        if (display) {
            // Wait until the pacer says the next 50 Hz frame is due, handling host events meanwhile.
            bool quit = false;
            while (!quit && frames_due == 0) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    switch (input->handle_event(e)) {
                    case x2::input::HostCommand::Quit: quit = true; break;
                    case x2::input::HostCommand::Pause:
                        paused = !paused;
                        audio_out.set_paused(paused); // stops the device and drops queued audio
                        break;
                    case x2::input::HostCommand::Reset:
                        reset_machine(m, assets);
                        if (!wav.is_open()) m.paula.output().clear(); // drop the pre-reset tail
                        audio_out.flush();
                        break;
                    case x2::input::HostCommand::Fullscreen: display->toggle_fullscreen(); break;
                    case x2::input::HostCommand::None: break;
                    }
                }
                uint64_t now = SDL_GetTicksNS();
                if (paused) {
                    pacer.reset(now);
                    display->present(video.front());
                    SDL_Delay(10);
                    continue;
                }
                if (audio_out.is_open()) {
                    // The audio device is the master clock: run a frame whenever its queue falls below the
                    // latency target. If the device stops consuming, fall back to wall-clock pacing after 100 ms.
                    if (audio_out.queued_frames() < audio_out.target_frames() || now - last_frame_ns > 100'000'000) {
                        frames_due = 1;
                        last_frame_ns = now;
                        pacer.reset(now);
                    } else {
                        SDL_DelayNS(500'000);
                    }
                    continue;
                }
                frames_due = pacer.frames_due(now);
                if (!frames_due) {
                    uint64_t next = pacer.next_due_ns();
                    if (next > now + 1'000'000) SDL_DelayNS(next - now - 1'000'000);
                    else SDL_DelayNS(100'000);
                }
            }
            if (quit) break;
            input->apply(m);
        }
#endif
        // Scripted input for this frame.
        bool fire = false;
        uint8_t dirs = 0;
        for (const InputEvent& e : opt.input) {
            if (e.kind == InputEvent::Key) {
                if (e.frame == f) m.key_event(e.code, false);
                if (e.frame + 2 == f) m.key_event(e.code, true);
            } else if (f >= e.frame && f < e.frame + e.hold) {
                if (e.kind == InputEvent::Fire) fire = true;
                else dirs |= e.dirs;
            }
        }
        if (dirs & 16) fire = true;
        if (opt.headless || fire || dirs) m.set_joystick(dirs & 1, dirs & 2, dirs & 4, dirs & 8, fire);
        for (const Poke& p : opt.pokes)
            if (f >= p.from && f <= p.to)
                for (int i = 0; i < p.size; ++i) m.chip[(p.addr + uint32_t(i)) & x2::kChipMask] = uint8_t(p.value >> (8 * (p.size - 1 - i)));

        bool tracing = std::find(opt.trace_frames.begin(), opt.trace_frames.end(), f) != opt.trace_frames.end();
        if (tracing) {
            trace_file = std::fopen((opt.dump_dir + "/trace_" + std::to_string(f) + ".txt").c_str(), "w");
            m.trace = [&](x2::Machine& mm, uint32_t pc) {
                char buf[128];
                m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
                std::fprintf(trace_file, "%3d %06X  %-36s d0=%08X d1=%08X a0=%08X a7=%08X sr=%04X\n", mm.line, pc, buf,
                             mm.dreg(0), mm.dreg(1), mm.areg(0), mm.areg(7), mm.sr());
            };
        }
        m.frame_writes.clear();
        m.pc_samples.clear();
        m.custom_reads.fill(0);
        m.cia_reads.fill(0);
        m.run_frame();
        if (tracing) { m.trace = nullptr; std::fclose(trace_file); trace_file = nullptr; }
        for (const auto& [sf, path] : opt.screenshots) {
            if (sf != f) continue;
            if (x2::write_image(path, video.front(), kW, kH)) std::printf("screenshot of frame %u -> %s\n", f, path.c_str());
            else std::fprintf(stderr, "x2recomp: cannot write %s\n", path.c_str());
        }
        if (opt.audio_hash) {
            for (int16_t smp : m.paula.output()) {
                for (int i = 0; i < 2; ++i) {
                    audio_hash ^= uint8_t(uint16_t(smp) >> (8 * i));
                    audio_hash *= 0x100000001b3ull;
                }
            }
        }
        for (uint32_t hf : opt.hash_frames) {
            if (hf != f) continue;
            if (opt.hash_every && f % opt.hash_every == 0) continue; // printed below
            std::printf("framehash %u %016llx\n", f, (unsigned long long)video.front_hash());
            if (opt.audio_hash) std::printf("audiohash %u %016llx\n", f, (unsigned long long)audio_hash);
        }
        if (opt.hash_every && f % opt.hash_every == 0) {
            std::printf("framehash %u %016llx\n", f, (unsigned long long)video.front_hash());
            if (opt.audio_hash) std::printf("audiohash %u %016llx\n", f, (unsigned long long)audio_hash);
        }
#ifdef X2_HAVE_SDL
        if (display && --frames_due == 0) display->present(video.front()); // skip presents when catching up
        if (audio_out.is_open()) {
            audio_out.push(m.paula.output().data(), m.paula.output().size() / 2);
            audio_max_queued = std::max(audio_max_queued, audio_out.queued_frames());
            if (!wav.is_open()) m.paula.output().clear();
        }
#endif
        if (wav.is_open()) {
            wav.write(m.paula.output().data(), m.paula.output().size());
            m.paula.output().clear();
        }
        if (opt.audio_hash) m.paula.output().clear();

        for (auto& [pc, n] : m.pc_samples) window_pcs[pc] += n;
        for (int i = 0; i < 0x100; ++i) window_reads[size_t(i)] += m.custom_reads[size_t(i)];
        for (int i = 0; i < 32; ++i) window_cia[size_t(i)] += m.cia_reads[size_t(i)];

        if (opt.reglog)
            x2::write_reglog(m, opt.dump_dir + "/regs/frame_" + std::string(5 - std::min<size_t>(5, std::to_string(f).size()), '0') +
                                    std::to_string(f) + ".log");
        if (opt.ppm_every && f % opt.ppm_every == 0)
            x2::write_bitplane_ppm(m, opt.dump_dir + "/frame_" + std::to_string(f) + ".ppm");

        // Milestones.
        bool cop_set = m.regs[x2::reg::COP1LCH >> 1] || m.regs[(x2::reg::COP1LCH >> 1) + 1];
        if (ms.cop1lc < 0 && cop_set) ms.cop1lc = f;
        if (ms.copper_running < 0 && m.copper_moves) ms.copper_running = f;
        if (ms.bitplanes < 0 && m.display.valid && ((m.display.bplcon0 >> 12) & 7) &&
            (m.display.dmacon & 0x0300) == 0x0300 && m.display.bplpt[0])
            ms.bitplanes = f;
        if (ms.vbl_handler < 0 && vbl_entries) ms.vbl_handler = f;
        if (ms.disc_check_ok < 0 && discid_ok) ms.disc_check_ok = f;
        if (ms.main_loop < 0 && main_loop_entries) ms.main_loop = f;

        if (opt.report_every && (f + 1) % opt.report_every == 0) {
            std::vector<std::pair<uint32_t, uint32_t>> pcs(window_pcs.begin(), window_pcs.end());
            std::sort(pcs.begin(), pcs.end(), [](auto& a, auto& b) { return a.second > b.second; });
            uint32_t total = 0;
            for (auto& p : pcs) total += p.second;
            std::printf("[frames %u-%u] pc=%06X sr=%04X intena=%04X intreq=%04X dmacon=%04X blits=%u copmoves=%u "
                        "disk=%u/%u sectors vbl=%u mainloop=%u restarts=%u unmapped=%u\n",
                        window_start, f, m.pc(), m.sr(), m.intena, m.intreq, m.dmacon, m.blits, m.copper_moves,
                        disk.calls, disk.sectors_read, vbl_entries, main_loop_entries,
                        m.restarts ? m.restarts - 1 : 0, m.unmapped_accesses);
            std::printf("   hot PCs:");
            for (size_t i = 0; i < pcs.size() && i < 5; ++i)
                std::printf(" %06X %s(%.0f%%)", pcs[i].first, known_name(pcs[i].first), 100.0 * pcs[i].second / std::max(1u, total));
            std::printf("\n   polled:");
            std::vector<std::pair<uint32_t, std::string>> polls;
            for (int i = 0; i < 0x100; ++i)
                if (window_reads[size_t(i)]) polls.push_back({window_reads[size_t(i)], x2::reg::name(uint16_t(i * 2))});
            static const char* const cia_names[16] = {"PRA", "PRB", "DDRA", "DDRB", "TALO", "TAHI", "TBLO", "TBHI",
                                                       "TODLO", "TODMID", "TODHI", "R0B", "SDR", "ICR", "CRA", "CRB"};
            for (int i = 0; i < 32; ++i)
                if (window_cia[size_t(i)]) polls.push_back({window_cia[size_t(i)], std::string(i < 16 ? "CIAA_" : "CIAB_") + cia_names[i & 15]});
            std::sort(polls.begin(), polls.end(), [](auto& a, auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < polls.size() && i < 6; ++i) std::printf(" %s x%u", polls[i].second.c_str(), polls[i].first);
            std::printf("\n");
            std::fflush(stdout);
            window_pcs.clear();
            window_reads.fill(0);
            window_cia.fill(0);
            window_start = f + 1;
        }
    }

#ifdef X2_HAVE_SDL
    if (audio_out.is_open())
        std::printf("live audio: underruns %llu, dropped %llu frames, queued %zu at exit, max %zu (target %zu)\n",
                    (unsigned long long)audio_out.underruns(), (unsigned long long)audio_out.dropped_frames(),
                    audio_out.queued_frames(), audio_max_queued, audio_out.target_frames());
    audio_out.close();
    input.reset();
    display.reset();
    if (!opt.headless) SDL_Quit();
#endif
    if (!opt.dump_dir.empty()) {
        x2::write_chip_ram(m, opt.dump_dir + "/chipram.bin");
        x2::write_bitplane_ppm(m, opt.dump_dir + "/final.ppm");
        x2::write_png(opt.dump_dir + "/final.png", video.front(), kW, kH);
    }
    std::printf("milestones (frame numbers, -1 = never):\n"
                "  first VBL handler entry ($20C2)       %ld\n"
                "  COP1LC first programmed               %ld\n"
                "  copper first executing MOVEs          %ld\n"
                "  bitplanes first displayed (DMA+BPU)   %ld\n"
                "  disc-id check passed ('X2B ')         %ld (%u/%u checks ok)\n"
                "  main loop $78A4 first reached         %ld (%u logic frames)\n",
                ms.vbl_handler, ms.cop1lc, ms.copper_running, ms.bitplanes, ms.disc_check_ok, discid_ok, discid_calls,
                ms.main_loop, main_loop_entries);
    std::printf("disk shim: %u calls, %u sectors, %u bad; last sector=%u count=%u dest=%06X\n", disk.calls,
                disk.sectors_read, disk.bad_requests, disk.last_sector, disk.last_count, disk.last_dest);
    std::printf("blits=%u (line mode: %u) restarts-via-$7032=%u (last from %06X) unmapped=%u (last %06X)\n",
                m.blits, m.line_blits, m.restarts ? m.restarts - 1 : 0, m.last_restart_from, m.unmapped_accesses,
                m.last_unmapped_addr);
    if (wav.is_open()) {
        wav.close();
        std::printf("audio: %llu frames at %d Hz -> %s, clipped samples %llu (peak %.3f), led filter %s, adkcon modulation writes %u\n",
                    (unsigned long long)m.paula.frames_out(), opt.audio.output_rate, opt.audio_out.c_str(),
                    (unsigned long long)m.paula.clipped_samples(), m.paula.peak_level(), m.paula.led_filter() ? "on" : "off",
                    m.paula.adkcon_modulation_writes());
        std::vector<uint32_t> sample_addrs;
        for (int c = 0; c < 4; ++c) {
            const auto& s = m.paula.stats()[size_t(c)];
            for (auto& st : s.starts)
                if (std::find(sample_addrs.begin(), sample_addrs.end(), st.first) == sample_addrs.end()) sample_addrs.push_back(st.first);
            std::printf("  AUD%d: dma starts %u, loops %u, irqs %u, distinct (LC,LEN) %zu, period %u-%u, clamps %u, "
                        "audible %.1f s\n",
                        c, s.dma_starts, s.buffer_reloads, s.irqs, s.starts.size(), s.min_period == 0xFFFF ? 0 : s.min_period,
                        s.max_period, s.period_clamps, double(s.active_cck) / x2::Paula::kPalColorClock);
        }
        std::sort(sample_addrs.begin(), sample_addrs.end());
        std::printf("  distinct sample start addresses (all channels): %zu:", sample_addrs.size());
        for (uint32_t a : sample_addrs) std::printf(" %05X", a);
        std::printf("\n");
    }
    if (cue_file) std::fclose(cue_file);
#ifdef X2_HAVE_RECOMP
    if (!opt.interp_only && opt.recomp_stats >= 0) x2::recomp::print_summary(m, opt.recomp_stats);
    if (!opt.interp_only && !opt.recomp_profile.empty()) {
        if (x2::recomp::write_profile(m, opt.recomp_profile)) std::printf("recomp profile: %s\n", opt.recomp_profile.c_str());
        else { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.recomp_profile.c_str()); return 3; }
    }
#endif
    if (tracer) {
        m.tracer = nullptr;
        fs::path tp(opt.trace_out);
        if (tp.has_parent_path()) fs::create_directories(tp.parent_path());
        std::string label = opt.trace_label.empty() ? tp.stem().string() : opt.trace_label;
        if (tracer->write_json(opt.trace_out, label, m.frame)) std::printf("trace: %s\n", opt.trace_out.c_str());
        else { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.trace_out.c_str()); return 3; }
    }
    if (!opt.dump_dir.empty())
        std::printf("outputs: %s/{chipram.bin,final.ppm,final.png%s}\n", opt.dump_dir.c_str(), opt.reglog ? ",regs/frame_NNNNN.log" : "");
    return 0;
}
