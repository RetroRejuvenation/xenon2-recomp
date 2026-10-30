// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// x2recomp - Xenon 2 Megablast runtime: interpreter-driven machine model with a scanline renderer, shown in an
// SDL3 window or run headless.
//
//   x2recomp [--assets DIR] [options]                              (window)
//   x2recomp --headless --frames N [--dump-dir DIR] [--assets DIR] [options]
//   x2recomp --import PATH [PATH2]                                 (import your disk images, then exit)
//
// See docs/BUILDING.md for all options and the output format.
#include "audio/audio_cues.h"
#include "audio/wav_writer.h"
#include "boot.h"
#include "cdtv/cdtv_disc.h"
#include "cdtv/cdtv_music.h"
#include "debug_dump.h"
#include "disk_shim.h"
#include "gfx/image_io.h"
#include "gfx/pacer.h"
#include "gfx/video.h"
#include "import/importer.h"
#include "machine.h"
#include "modern/assets.h"
#include "modern/draw_capture.h"
#include "modern/interp.h"
#include "modern/present.h"
#include "persist.h"
#include "trace/tracer.h"
#ifdef X2_HAVE_RECOMP
#include "recomp/recomp.h"
#endif
#ifdef X2_HAVE_SDL
#include "gfx/sdl_display.h"
#include "audio/sdl_audio.h"
#include "import/import_screen.h"
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

#ifndef X2_VERSION
#define X2_VERSION "unknown" // set from project(VERSION) in CMakeLists.txt (docs/RELEASING.md)
#endif


namespace {

struct InputEvent {
    uint32_t frame;
    enum Kind { Key, Fire, Joy } kind;
    uint8_t code;
    uint32_t hold;
    uint8_t dirs = 0; // Joy: 1 up, 2 down, 4 left, 8 right, 16 fire
};

// Coverage-only state pokes (x2::Poke): never applied unless given with --poke or a script "poke"/"pokeobj" line.
using x2::Poke;

struct Options {
    bool headless = false;
    uint32_t frames = 500; // headless default; a window runs until quit unless --frames is given
    bool frames_set = false, report_set = false;
    std::string dump_dir;
    std::string assets;   // "" = ./generated/assets if present, else the imported <data dir>/assets
    std::string data_dir; // per-user data directory (default: SDL_GetPrefPath)
    std::vector<std::string> import_paths;
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
    int autofire_rate = x2::input::AutofirePolicy::kDefaultRate;
    int credits = 3;
    bool help = false, version = false, print_data_dir = false, list_keys = false; // print and exit
};

void usage(std::FILE* out = stderr) {
    std::fprintf(out,
                 "x2recomp " X2_VERSION "\n"
                 "usage: x2recomp [--headless] [--frames N] [--dump-dir DIR] [options]\n"
                 "  --help, --version   print this text or the version and exit (status 0)\n"
                 "  --print-data-dir    print the per-user data directory (imports, settings) and exit\n"
                 "  --list-keys         print the active key bindings (input.cfg) and exit\n"
                 "  --headless          no window (the only mode when built without SDL3)\n"
                 "  --scale N           initial window scale (default 3); --fullscreen\n"
                 "  --no-audio          window without sound output\n"
                 "  --grab-keyboard, --no-grab-keyboard  send the system's own keyboard shortcuts to the window\n"
                 "                      while it has the focus (Scroll Lock or Cmd+G toggles it; default off, and\n"
                 "                      on macOS SDL's Cocoa backend cannot do it at all)\n"
                 "  --cursor MODE       mouse cursor: auto (hidden while playing, shown while frozen, the\n"
                 "                      default), show or hide. The game has no mouse input.\n"
                 "  --input-debug       log every key, focus change and frame to stdout\n"
                 "  --autofire[=RATE]   pulse the fire line while fire is held, during a level only (RATE =\n"
                 "                      pulses per second 1..25, default 17 = the game's own maximum rate);\n"
                 "                      --no-autofire turns it off. An assist, not a cheat.\n"
                 "  --god-mode          CHEAT: the ship's energy is refilled every frame, so damage never\n"
                 "                      kills it. Flagged in the window title; no high scores are saved.\n"
                 "  --credits N         credits a new game starts with, 1..9 (the game's own value is 3; the\n"
                 "                      display has one digit). Above 3 counts as a cheat.\n"
                 "  --cheat-scores      save high scores even in a session that used a cheat\n"
                 "  --volume N          window: master volume of the live output, percent (default 100)\n"
                 "  --audio-latency MS  window: target output buffer, 10..500 ms (default 40). Also how long the\n"
                 "                      host loop may stall before the sound breaks up; raise it if it does.\n"
                 "  --persist-dir DIR   keep the high-score table in DIR/highscores.bin (window default: the\n"
                 "                      per-user data directory; headless: off unless given)\n"
                 "  --no-persist        window: do not load or save high scores\n"
                 "  --screenshot-at F:FILE  save rendered frame F as FILE (.png, otherwise PPM)\n"
                 "  --dump-ram-at F:FILE  write the 512 KB of chip RAM after frame F to FILE\n"
                 "  --hash-at F         print 'framehash F <fnv1a64>' of rendered frame F\n"
                 "  --hash-every N      print 'framehash' for every frame F with F %% N == 0\n"
                 "  --interp-only       recompiled builds: run all code on the Musashi interpreter (A/B comparison)\n"
                 "  --recomp-stats N    recompiled builds: print translated/interpreted counts and the N hottest\n"
                 "                      interpreted PCs at exit\n"
                 "  --recomp-profile FILE  recompiled builds: write interpreted PCs (seeds for tools/recomp/recomp.py)\n"
                 "  --assets DIR        directory with game_0400.bin and disc2.bin (default: ./generated/assets\n"
                 "                      if present, else the imported assets in the per-user data directory)\n"
                 "  --import PATH...    import your disk images (ADF, ADZ, ZIP or a folder) into the per-user\n"
                 "                      data directory (or --assets DIR) and exit; exit code 0 ok, 2 bad input,\n"
                 "                      3 unsupported image\n"
                 "  --data-dir DIR      per-user data directory (default: the SDL preferences directory)\n"
                 "  --report-every N    status report interval in frames (default 50)\n"
                 "  --ppm-every N       also dump the bitplane framebuffer every N frames\n"
                 "  --no-reglog         do not write per-frame register write logs\n"
                 "  --trace-frame F     write a disassembly trace of every instruction in frame F\n"
                 "  --key F:CODE        press Amiga raw key CODE (hex) at frame F (released 2 frames later)\n"
                 "  --fire F[:N]        hold joystick fire from frame F for N frames (default 3)\n"
                 "  --joy F:DIRS[:N]    hold joystick directions DIRS (letters U D L R F) from frame F for N frames\n"
                 "  --input-script FILE frame-indexed input script (tests/replays/README.md)\n"
                 "  --poke F[-G]:ADDR:VALUE[.b|.w|.l]  COVERAGE ONLY: write chip RAM at the start of frames F..G\n"
                 "                      (scripts also have pokeobj HANDLER OFFSET VALUE: tests/replays/README.md)\n"
                 "  --trace-out FILE    record executed PCs, branches, indirect targets etc. as JSON (runtime/trace)\n"
                 "  --trace-label NAME  label stored in the trace file (default: the file name)\n"
                 "  --audio-hash        with --hash-at, also print 'audiohash F <fnv1a64>' of all audio up to frame F\n"
                 "  --audio-out FILE    write the Paula output (48 kHz stereo 16-bit WAV) of the whole run\n"
                 "  --audio-filter M    output filter model: a500 (default), a1200, none\n"
                 "  --stereo-sep X      stereo separation 0..1 (default 1 = hard left/right)\n"
                 "  --audio-cues FILE   log music/sound driver calls (frame, line, entry, d0, caller)\n"
                 "  --music SRC         music: amiga (tracker music, default), cdtv (the CDTV disc's CD tracks,\n"
                 "                      tracker music muted, sound effects kept) or mix (both)\n"
                 "  --music-volume N, --sfx-volume N  percent (default 100): music (CD and tracker), sound effects\n"
                 "  --protect-music     keep the tune's voices audible through the sound effects that take their\n"
                 "                      Paula channels (default off). This is NOT what the original does: heavy\n"
                 "                      firing silences a voice of the tune on real hardware too.\n"
                 "  --cd-crossfade MS   0..500: seamless CD loops and track changes (default 0 = as the CDTV game)\n"
                 "  --cd-pause M        during the game's pause (P): keep (default), duck or stop the CD\n"
                 "  --cdtv PATH         CDTV disc audio: an imported directory, or a .cue / folder with the .cue and\n"
                 "                      .bin files used in place (default: <data dir>/cdtv)\n"
                 "  --cdtv-tracks LIST  CD track for title,level1,...,level5 (default 2,3,5,2,3,5)\n"
                 "  --cdtv-log FILE     log the CD cues (frame, line, event, track, caller)\n"
                 "  --import-cdtv PATH  import your CDTV disc image (ZIP, folder or .cue) into <data dir>/cdtv (or\n"
                 "                      --cdtv DIR) and exit; --import detects a CDTV image too\n"
                 "  --timing MODE       accurate (default: OCS DMA slots, blitter cycle timing, CPU wait states) or\n"
                 "                      fast (zero-time blitter, no bus contention; docs/re-notes/timing.md)\n"
                 "  --watch-log FILE    per frame: chip words (--watch) at the start of the frame, the lines whose\n"
                 "                      end found the CPU in the swap wait $2034-$2041, and the lines whose end found\n"
                 "                      the blitter busy (compare with the reference's frames.log)\n"
                 "  --watch A,B,...     word addresses (hex) for --watch-log (default 404,41C,1FEE,1FF0,1FF2,1FF4)\n"
                 "  --pc-trace F-G:FILE write 'frame line pc waits' at the end of every line of frames F..G (waits: CPU\n"
                 "                      wait cycles so far, accurate timing)\n"
                 "  --blit-log F-G:FILE accurate timing: log each blit's start and BBUSY clear in frames F..G\n"
                 "  --pc-log F-G:PC,...:FILE  log 'frame line hpos cycle pc' whenever an instruction at one of the\n"
                 "                      PCs (hex) starts in frames F..G (as the reference harness X2REF_PCLOG)\n"
                 "  --insn-log F-G:FILE log 'frame line hpos cycle pc opcode' for every instruction of frames F..G\n"
                 "  --draw-log F-G:FILE log every blit, screen-buffer CPU write and the object lists, grouped by logic\n"
                 "                      step, for the steps swapped in during frames F..G (runtime/modern/draw_capture.h)\n"
                 "  --faithful          the Faithful switch: original picture only (no interpolation, nearest\n"
                 "                      integer scaling, square pixels); overrides every option below\n"
                 "  --modern SPEC       (or --modern=SPEC) comma-separated: 'interp' (interpolated frames at the\n"
                 "                      display rate), 'nointerp', scale=N (canvas N x Amiga pixels, default 3),\n"
                 "                      nosmooth (exact far layer), rate=HZ (headless --modern-out rate, default 60),\n"
                 "                      'upgrade' / 'noupgrade' (asset packs and palette overrides), wide=W:H or\n"
                 "                      wide=N (surround: 16:10, 16:9, 21:9, 32:9, or N <= 320 pixels per side;\n"
                 "                      wide=off), 'hdpanel' / 'nohdpanel' (edge-upscaled status panel),\n"
                 "                      'faithful' (= --faithful)\n"
                 "  --asset-pack PATH   load a replacement asset pack (directory or .zip, docs/ASSET-PACKS.md);\n"
                 "                      repeatable, later packs override earlier ones; implies --modern upgrade\n"
                 "  --dump-assets DIR   write every distinct original graphic drawn in gameplay (sprites, tiles, far\n"
                 "                      lines) as PNGs plus DIR/manifest.json, for pack authors (your data only)\n"
                 "  --filter NAME       window filter: nearest (default), sharp, scanlines, crt, edge\n"
                 "  --aspect, --no-aspect      4:3 picture (PAL pixels 16:15 wide) or square pixels\n"
                 "  --integer, --no-integer    integer scale factors only (default on)\n"
                 "  --curvature, --no-curvature  crt filter: curved screen\n"
                 "  --present-shot F:FILE  write the window picture (filter included, software renderer, window\n"
                 "                      size from --scale) at the end of frame F to FILE\n"
                 "  --modern-out F-G:DIR  headless: write the presented pictures for emulated frames F..G as PNGs\n"
                 "                      at the --modern rate; DIR '-' prints 'presenthash H F <fnv1a64>' instead\n"
                 "  --modern-shots F:DIR  write the first interpolable step pair swapped in at or after frame F at\n"
                 "                      t = 0, .25, .5, .75, 1 and the two real pictures\n"
                 "  --modern-check      check that t = 0 / 1 reproduce the real pictures (without far-layer smoothing)\n"
                 "                      and that the captured pictures match the rendered frames; print a summary\n");
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
//   FRAME pokeobj HANDLER OFFSET VALUE[.b|.w|.l] [N]   COVERAGE ONLY: poke OFFSET in every hittable object
//                            whose hit handler is HANDLER (x2::apply_poke), for N frames
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
            if (cmd == "poke" || cmd == "pokeobj" || i + 4 != tok.size() || tok[i + 2] != "until") return fail();
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
        } else if (cmd == "pokeobj") {
            // FRAME pokeobj HANDLER OFFSET VALUE[.b|.w|.l] [N]
            std::string n = tok.size() > 3 ? tok[3] : "";
            Poke p{f, f + count(n, 1) - 1, 0, 0, 2};
            Poke h{0, 0, 0, 0, 1};
            if (!parse_poke_value(b, c, p) || !parse_poke_value(a, "0", h) || !h.addr) return fail();
            p.handler = h.addr;
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
        else if (a == "--data-dir") { auto v = next(); if (!v) return false; o.data_dir = v; }
        else if (a == "--import") {
            o.import_paths.clear();
            while (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) o.import_paths.push_back(argv[++i]);
            if (o.import_paths.empty()) return false;
        }
        else if (a == "--report-every") { auto v = next(); if (!v) return false; o.report_every = uint32_t(std::strtoul(v, nullptr, 0)); o.report_set = true; }
        else if (a == "--ppm-every") { auto v = next(); if (!v) return false; o.ppm_every = uint32_t(std::strtoul(v, nullptr, 0)); }
        else if (a == "--no-reglog") o.reglog = false;
        else if (a == "--scale") { auto v = next(); if (!v) return false; o.scale = std::max(1, std::atoi(v)); }
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--aspect" || a == "--no-aspect") { o.present.aspect = a == "--aspect"; o.custom_given = true; }
        else if (a == "--integer" || a == "--no-integer") { o.present.integer = a == "--integer"; o.custom_given = true; }
        else if (a == "--curvature" || a == "--no-curvature") { o.present.curvature = a == "--curvature"; o.custom_given = true; }
        else if (a == "--faithful") o.faithful_forced = true;
        else if (a == "--filter") {
            auto v = next();
            if (!v || !x2::parse_video_filter(v, o.present.filter)) {
                std::fprintf(stderr, "--filter: nearest, sharp, scanlines, crt or edge\n");
                return false;
            }
            o.custom_given = true;
        }
        else if (a == "--no-audio") o.no_audio = true;
        else if (a == "--grab-keyboard" || a == "--no-grab-keyboard") o.grab_keyboard = a == "--grab-keyboard" ? "on" : "off";
        else if (a == "--cursor") {
            auto v = next();
            x2::input::CursorPolicy::Mode m;
            if (!v || !x2::input::CursorPolicy::parse_mode(v, m)) {
                std::fprintf(stderr, "--cursor: auto, show or hide\n");
                return false;
            }
            o.cursor = v;
        }
        else if (a == "--input-debug") o.input_debug = true;
        else if (a == "--autofire" || a.rfind("--autofire=", 0) == 0) {
            o.autofire = true;
            if (a.size() > 11) {
                const int r = std::atoi(a.c_str() + 11);
                if (r < 1 || r > 25) { std::fprintf(stderr, "--autofire: rate 1..25\n"); return false; }
                o.autofire_rate = r;
            }
        }
        else if (a == "--no-autofire") o.autofire = false;
        else if (a == "--god-mode") o.god_mode = true;
        else if (a == "--cheat-scores") o.cheat_scores = true;
        else if (a == "--credits") {
            auto v = next();
            if (!v) return false;
            const int n = std::atoi(v);
            if (n < 1 || n > 9) { std::fprintf(stderr, "--credits: 1..9 (the display has one digit)\n"); return false; }
            o.credits = n;
        }
        else if (a == "--list-keys") o.list_keys = true;
        else if (a == "--volume") { auto v = next(); if (!v) return false; o.volume = std::clamp(std::atoi(v), 0, 100); }
        else if (a == "--audio-latency") { auto v = next(); if (!v) return false; o.audio_latency = std::clamp(std::atoi(v), 10, 500); }
        else if (a == "--persist-dir") { auto v = next(); if (!v) return false; o.persist_dir = v; }
        else if (a == "--no-persist") o.no_persist = true;
        else if (a == "--hash-every") { auto v = next(); if (!v) return false; o.hash_every = uint32_t(std::strtoul(v, nullptr, 0)); }
        else if (a == "--interp-only") o.interp_only = true;
        else if (a == "--recomp-stats") { auto v = next(); if (!v) return false; o.recomp_stats = std::atoi(v); }
        else if (a == "--recomp-profile") { auto v = next(); if (!v) return false; o.recomp_profile = v; }
        else if (a == "--hash-at") { auto v = next(); if (!v) return false; o.hash_frames.push_back(uint32_t(std::strtoul(v, nullptr, 0))); }
        else if (a == "--screenshot-at" || a == "--dump-ram-at") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            uint32_t f = uint32_t(std::strtoul(v, &end, 0));
            if (*end != ':' || !end[1]) return false;
            (a == "--screenshot-at" ? o.screenshots : o.ram_dumps).push_back({f, end + 1});
        }
        else if (a == "--audio-out") { auto v = next(); if (!v) return false; o.audio_out = v; }
        else if (a == "--timing" || a.rfind("--timing=", 0) == 0) {
            const char* v = a.size() > 8 ? a.c_str() + 9 : next();
            if (!v) return false;
            std::string t = v;
            if (t == "fast") o.timing = x2::Timing::Fast;
            else if (t == "accurate") o.timing = x2::Timing::Accurate;
            else { std::fprintf(stderr, "--timing: fast or accurate\n"); return false; }
        }
        else if (a == "--watch-log") { auto v = next(); if (!v) return false; o.watch_log = v; }
        else if (a == "--insn-log") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            o.pc_log_from = uint32_t(std::strtoul(v, &end, 0));
            o.pc_log_to = *end == '-' ? uint32_t(std::strtoul(end + 1, &end, 0)) : o.pc_log_from;
            if (*end != ':') return false;
            o.pc_log = end + 1;
            o.pc_log_pcs.clear();
        }
        else if (a == "--pc-log") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            o.pc_log_from = uint32_t(std::strtoul(v, &end, 0));
            o.pc_log_to = *end == '-' ? uint32_t(std::strtoul(end + 1, &end, 0)) : o.pc_log_from;
            while (*end == ':' || *end == ',') {
                char* p = end + 1;
                uint32_t pc = uint32_t(std::strtoul(p, &end, 16));
                if (end == p || *end == '/' || *end == '.') { end = p - 1; break; }
                o.pc_log_pcs.push_back(pc);
            }
            if (*end != ':') return false;
            o.pc_log = end + 1;
        }
        else if (a == "--modern" || a.rfind("--modern=", 0) == 0) {
            const char* v = a == "--modern" ? next() : a.c_str() + 9;
            if (!v) return false;
            std::stringstream spec(v);
            for (std::string item; std::getline(spec, item, ',');) {
                if (item == "interp") { o.present.interp = true; o.custom_given = true; }
                else if (item == "nointerp") o.present.interp = false;
                else if (item == "faithful") o.faithful_forced = true;
                else if (item == "nosmooth") o.present.smooth = false;
                else if (item == "upgrade") { o.present.upgrade = true; o.custom_given = true; }
                else if (item == "noupgrade") o.present.upgrade = false;
                else if (item == "hdpanel") { o.present.hd_panel = true; o.custom_given = true; }
                else if (item == "nohdpanel") o.present.hd_panel = false;
                else if (item.rfind("wide=", 0) == 0) {
                    if (!x2::modern::parse_wide(item.substr(5), o.present.wide)) {
                        std::fprintf(stderr, "--modern wide=: off, W:H (16:10, 16:9, 21:9, 32:9) or pixels per side 0..%d\n",
                                     x2::modern::kMaxWide);
                        return false;
                    }
                    if (o.present.wide.on()) o.custom_given = true;
                }
                else if (item.rfind("scale=", 0) == 0) o.present.scale = std::clamp(std::atoi(item.c_str() + 6), 1, 8);
                else if (item.rfind("rate=", 0) == 0) o.modern_rate = std::clamp(std::atoi(item.c_str() + 5), 1, 1000);
                else { std::fprintf(stderr, "--modern: unknown item '%s'\n", item.c_str()); return false; }
            }
        }
        else if (a == "--modern-check") o.modern_check = true;
        else if (a == "--asset-pack") {
            auto v = next();
            if (!v) return false;
            o.asset_packs.push_back(v);
            o.present.upgrade = true;
            o.custom_given = true;
        }
        else if (a == "--dump-assets") { auto v = next(); if (!v) return false; o.dump_assets = v; }
        else if (a == "--present-shot") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            uint32_t f = uint32_t(std::strtoul(v, &end, 0));
            if (*end != ':' || !end[1]) return false;
            o.present_shots.push_back({f, end + 1});
        }
        else if (a == "--modern-out" || a == "--modern-shots") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            uint32_t from = uint32_t(std::strtoul(v, &end, 0)), to = from;
            if (*end == '-') to = uint32_t(std::strtoul(end + 1, &end, 0));
            if (*end != ':') return false;
            if (a == "--modern-out") { o.modern_out_from = from; o.modern_out_to = to; o.modern_out = end + 1; }
            else { o.modern_shots_at = from; o.modern_shots = end + 1; }
        }
        else if (a == "--draw-log") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            o.draw_log_from = o.draw_log_to = uint32_t(std::strtoul(v, &end, 0));
            if (*end == '-') o.draw_log_to = uint32_t(std::strtoul(end + 1, &end, 0));
            if (*end != ':') return false;
            o.draw_log = end + 1;
        }
        else if (a == "--pc-trace" || a == "--blit-log") {
            auto v = next();
            if (!v) return false;
            bool pt = a == "--pc-trace";
            char* end = nullptr;
            uint32_t from = uint32_t(std::strtoul(v, &end, 0)), to = from;
            if (*end == '-') to = uint32_t(std::strtoul(end + 1, &end, 0));
            if (*end != ':') return false;
            (pt ? o.pc_trace_from : o.blit_log_from) = from;
            (pt ? o.pc_trace_to : o.blit_log_to) = to;
            (pt ? o.pc_trace : o.blit_log) = end + 1;
        }
        else if (a == "--watch") {
            auto v = next();
            if (!v) return false;
            o.watch.clear();
            for (const char* p = v; *p;) {
                char* end = nullptr;
                o.watch.push_back(uint32_t(std::strtoul(p, &end, 16)) & ~1u);
                if (end == p) return false;
                p = *end ? end + 1 : end;
            }
        }
        else if (a == "--audio-cues") { auto v = next(); if (!v) return false; o.audio_cues = v; }
        else if (a == "--music") {
            auto v = next();
            x2::cdtv::MusicSource src;
            if (!v || !x2::cdtv::parse_music_source(v, src)) { std::fprintf(stderr, "--music: amiga, cdtv or mix\n"); return false; }
            o.music = v;
        }
        else if (a == "--music-volume" || a == "--sfx-volume") {
            auto v = next();
            if (!v) return false;
            (a == "--music-volume" ? o.music_volume : o.sfx_volume) = std::clamp(std::atoi(v), 0, 100);
        }
        else if (a == "--protect-music") o.protect_music = true;
        else if (a == "--no-protect-music") o.protect_music = false;
        else if (a == "--cd-crossfade") { auto v = next(); if (!v) return false; o.cd_crossfade = std::clamp(std::atoi(v), 0, 500); }
        else if (a == "--cd-pause") {
            auto v = next();
            x2::cdtv::PauseMode pm;
            if (!v || !x2::cdtv::parse_pause_mode(v, pm)) { std::fprintf(stderr, "--cd-pause: keep, duck or stop\n"); return false; }
            o.cd_pause = v;
        }
        else if (a == "--cdtv") { auto v = next(); if (!v) return false; o.cdtv_path = v; }
        else if (a == "--cdtv-log") { auto v = next(); if (!v) return false; o.cdtv_log = v; }
        else if (a == "--import-cdtv") { auto v = next(); if (!v) return false; o.import_cdtv = v; }
        else if (a == "--cdtv-tracks") {
            auto v = next();
            std::array<int, 6> t{};
            if (!v || !x2::parse_cdtv_tracks(v, t)) { std::fprintf(stderr, "--cdtv-tracks: six track numbers 2..13\n"); return false; }
            o.cdtv_tracks = v;
        }
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
        } else if (a == "--help" || a == "-h") o.help = true;
        else if (a == "--version") o.version = true;
        else if (a == "--print-data-dir") o.print_data_dir = true;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    // The Faithful switch from the command line wins; any modern picture option asks for the custom picture.
    if (o.faithful_forced) o.present.faithful = true;
    else if (o.custom_given) o.present.faithful = false;
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

// settings.cfg -> Options (window mode only; the command line is applied on top afterwards).
void apply_settings(const x2::Settings& s, Options& o) {
    o.scale = s.scale;
    o.fullscreen = s.fullscreen;
    o.present.aspect = s.aspect;
    o.present.faithful = s.faithful;
    o.present.interp = s.interp;
    o.present.scale = s.interp_scale;
    o.present.smooth = s.smooth_far;
    o.present.filter = s.video_filter;
    o.present.integer = s.integer;
    o.present.curvature = s.curvature;
    o.present.upgrade = s.upgrade;
    o.present.hd_panel = s.hd_panel;
    x2::modern::parse_wide(s.wide, o.present.wide); // validated by parse_settings
    std::stringstream packs(s.asset_packs);
    for (std::string p; std::getline(packs, p, ';');)
        if (p.find_first_not_of(" \t") != std::string::npos) o.asset_packs.push_back(p);
    o.no_audio = !s.audio;
    o.audio.filter = s.filter == x2::FilterSetting::A1200 ? x2::AudioFilterModel::A1200
                     : s.filter == x2::FilterSetting::None ? x2::AudioFilterModel::None
                                                           : x2::AudioFilterModel::A500;
    o.audio.stereo_separation = s.stereo_separation;
    o.volume = s.volume;
    o.audio_latency = s.audio_latency;
    o.no_persist = !s.high_scores;
    o.music = s.music;
    o.music_volume = s.music_volume;
    o.sfx_volume = s.sfx_volume;
    o.protect_music = s.protect_music;
    o.cd_crossfade = s.cd_crossfade;
    o.cd_pause = s.cd_pause;
    o.cdtv_path = s.cdtv;
    o.cdtv_tracks = s.cdtv_tracks;
    o.grab_keyboard = s.grab_keyboard;
    o.cursor = s.cursor;
    o.input_debug = s.input_debug;
    o.autofire = s.autofire;
    o.autofire_rate = s.autofire_rate;
    o.god_mode = s.god_mode;
    o.credits = s.credits;
    o.cheat_scores = s.cheat_scores;
}

// The per-user data directory, ending in a separator ("" if there is none): imported assets (<dir>assets/), and
// in window mode settings.cfg, input.cfg and highscores.bin. --data-dir overrides it for all of them.
std::string user_data_dir(const Options& o) {
    if (!o.data_dir.empty()) return o.data_dir + "/";
#ifdef X2_HAVE_SDL
    if (char* pref = SDL_GetPrefPath("xenon2-recomp", "x2recomp")) {
        std::string d = pref;
        SDL_free(pref);
        return d;
    }
#endif
    return x2::import::fallback_pref_path();
}

} // namespace

// First line of the display window (DIWSTRT $3A) in the framebuffer.
constexpr int kTop = 0x3A - x2::Video::kFbTop;

template <class Check>
void modern_after_frame(x2::modern::Presenter& pr, const Options& opt, Check& ck, const x2::Video& video, uint32_t f,
                        uint64_t& host_frame) {
    using namespace x2::modern;
    constexpr int kW = x2::Video::kFbWidth, kH = x2::Video::kFbHeight;
    static_assert(kW == kFrameWidth && kH == kFrameHeight && kTop == kFrameTop, "modern/present.h frame geometry");
    if (InterpPresenter* md = pr.interp()) {
        const auto& st = md->capture.steps();
        // Captured picture vs the frame the video model rendered after the swap (same palette and geometry).
        if (ck.pending_step != ~0u && f == ck.pending_frame) {
            ++ck.compared;
            const auto& fr = video.front();
            bool same = true;
            for (int y = 0; y < kLines && same; ++y)
                same = std::equal(&ck.pending_picture[size_t(y) * kWidth * 4], &ck.pending_picture[size_t(y + 1) * kWidth * 4],
                                  &fr[size_t(y + kTop) * kW * 4]);
            ck.frame_exact += same;
            ck.pending_step = ~0u;
        }
        if (!st.empty() && st.back().index != ck.last_step) {
            ck.last_step = st.back().index;
            const Step& b = st.back();
            if (opt.modern_check && !b.image.empty()) {
                Interpolator::picture(b, 1, ck.pending_picture);
                ck.pending_step = b.index;
                ck.pending_frame = b.swap_frame + 1;
            }
            if (st.size() >= 2) {
                const Step& a = st[st.size() - 2];
                const Step* z = st.size() >= 3 ? &st[st.size() - 3] : nullptr;
                if (opt.modern_check) {
                    Interpolator exact;
                    exact.opt.scale = 1;
                    exact.opt.smooth_far_layer = false;
                    PairMotion mo = exact.analyse(z, a, b);
                    if (mo.ok) {
                        std::vector<uint8_t> pa, pb, r;
                        Interpolator::picture(a, 1, pa);
                        Interpolator::picture(b, 1, pb);
                        ++ck.pairs;
                        exact.render(a, b, mo, 0.0f, r);
                        ck.start_exact += r == pa;
                        exact.render(a, b, mo, 1.0f, r);
                        ck.end_exact += r == pb;
                        if (const int wd = md->interp.opt.wide; wd > 0) {
                            // The surround keeps the playfield: its centre 320 columns are the pictures exactly.
                            Interpolator wide;
                            wide.opt = exact.opt;
                            wide.opt.wide = wd;
                            bool same = true;
                            for (float t : {0.0f, 1.0f}) {
                                wide.render(a, b, mo, t, r);
                                const std::vector<uint8_t>& p = t == 0.0f ? pa : pb;
                                for (int y = 0; y < kLines && same; ++y)
                                    same = std::equal(&r[(size_t(y) * size_t(kWidth + 2 * wd) + size_t(wd)) * 4],
                                                      &r[(size_t(y) * size_t(kWidth + 2 * wd) + size_t(wd + kWidth)) * 4],
                                                      &p[size_t(y) * kWidth * 4]);
                            }
                            ck.wide_exact += same;
                        }
                        Interpolator smooth;
                        PairMotion ms = smooth.analyse(z, a, b);
                        smooth.render(a, b, ms, 1.0f, r);
                        ck.smooth_end_exact += r == pb;
                        for (size_t i = 0; i < r.size(); i += 4) ck.smooth_diff_pixels += std::memcmp(&r[i], &pb[i], 4) != 0;
                    }
                }
                if (!opt.modern_shots.empty() && !ck.shots_done && b.swap_frame >= opt.modern_shots_at) {
                    Interpolator ip;
                    ip.opt = md->interp.opt;
                    PairMotion mo = ip.analyse(z, a, b);
                    if (mo.ok) {
                        ck.shots_done = true;
                        const int S = ip.opt.scale, IW = (kWidth + 2 * ip.opt.wide) * S;
                        std::vector<uint8_t> img;
                        Interpolator::picture(a, S, img);
                        x2::write_png(opt.modern_shots + "/real_a.png", img, kWidth * S, kLines * S);
                        Interpolator::picture(b, S, img);
                        x2::write_png(opt.modern_shots + "/real_b.png", img, kWidth * S, kLines * S);
                        for (int q = 0; q <= 4; ++q) {
                            ip.render(a, b, mo, float(q) / 4.0f, img);
                            x2::write_png(opt.modern_shots + "/interp_t" + std::to_string(q * 25) + ".png", img, IW, kLines * S);
                            std::printf("modernshot t%d %016llx\n", q * 25, (unsigned long long)x2::fnv1a64(img.data(), img.size()));
                        }
                        std::printf("modern shots: steps %u -> %u (swapped in frames %u, %u): far %+d (%.2f -> %.2f), tiles %+d, "
                                    "%zu objects moving -> %s\n",
                                    a.index, b.index, a.swap_frame, b.swap_frame, mo.far_step, mo.far_from, mo.far_to, mo.fg_dy,
                                    mo.objects.size(), opt.modern_shots.c_str());
                    }
                }
            }
        }
    }
    // Headless host-rate output: host frame h is at emulated time from + h * 50 / rate.
    if (!opt.modern_out.empty() && f >= opt.modern_out_from && f <= opt.modern_out_to) {
        for (;;) {
            double tau = double(opt.modern_out_from) + double(host_frame) * 50.0 / opt.modern_rate;
            if (tau > double(f + 1)) break;
            Canvas c = pr.at(tau, video.front());
            const size_t bytes = size_t(c.w) * size_t(c.h) * 4;
            if (opt.modern_out == "-") {
                std::printf("presenthash %llu %u %016llx\n", (unsigned long long)host_frame, f,
                            (unsigned long long)x2::fnv1a64(c.rgba, bytes));
                if (const int cw = kFrameWidth * c.scale; c.w > cw) { // surround: the centre on its own
                    std::vector<uint8_t> centre;
                    for (int y = 0; y < c.h; ++y) {
                        const uint8_t* row = c.rgba + (size_t(y) * size_t(c.w) + size_t(c.w - cw) / 2) * 4;
                        centre.insert(centre.end(), row, row + size_t(cw) * 4);
                    }
                    std::printf("centerhash %llu %u %016llx\n", (unsigned long long)host_frame, f,
                                (unsigned long long)x2::fnv1a64(centre.data(), centre.size()));
                }
            } else {
                char name[32];
                std::snprintf(name, sizeof name, "/%06llu.png", (unsigned long long)host_frame);
                x2::write_png(opt.modern_out + name, std::vector<uint8_t>(c.rgba, c.rgba + bytes), c.w, c.h);
            }
            ++host_frame;
        }
    }
}

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) { usage(); return 2; }
    if (opt.help) { usage(stdout); return 0; }
    if (opt.version) { std::printf("x2recomp %s\n", X2_VERSION); return 0; }
#if defined(X2_HAVE_SDL) && defined(__APPLE__)
    // Before any SDL_Init; both end up as NSUserDefaults (ApplePressAndHoldEnabled and the IME's Option
    // handling) that are read once at init. Holding a letter key should repeat it rather than suppress the
    // repeat and offer the accent menu, which is what macOS does by default for A, S, Z and the rest -- those
    // are steering and fire keys here, not text. The Option keys are fire too, so they must not start a
    // composition. SDL_SetHint ranks below the environment, so SDL_MAC_PRESS_AND_HOLD=1 still wins.
    SDL_SetHint(SDL_HINT_MAC_PRESS_AND_HOLD, "0");
    SDL_SetHint(SDL_HINT_MAC_OPTION_AS_ALT, "both");
#endif
    if (opt.list_keys) { // the bindings this run would use, without touching the file
#ifndef X2_HAVE_SDL
        std::fprintf(stderr, "x2recomp: built without SDL3, there are no key bindings\n");
        return 2;
#else
        const x2::input::LoadedBindings cfg = x2::input::preview_bindings(user_data_dir(opt));
        if (cfg.path.empty()) std::printf("bindings: the defaults for this platform (no per-user data directory)\n");
        else std::printf("bindings: %s%s%s\n", cfg.path.c_str(), cfg.note.empty() ? "" : "\n  ", cfg.note.c_str());
        if (!cfg.error.empty()) std::fprintf(stderr, "%s", cfg.error.c_str());
        std::printf("%s", x2::input::describe_bindings(cfg.bindings).c_str());
        return 0;
#endif
    }
    if (opt.print_data_dir) { // for scripts (tools/build_all.sh, .ps1): where --import puts the assets
        const std::string d = user_data_dir(opt);
        if (d.empty()) { std::fprintf(stderr, "x2recomp: no per-user data directory\n"); return 1; }
        std::printf("%s\n", d.c_str());
        return 0;
    }
    // CDTV disc image: --import-cdtv PATH, or --import with a single CD image (.cue, folder or ZIP holding a .cue).
    if (!opt.import_cdtv.empty() || (opt.import_paths.size() == 1 && x2::cdtv::looks_like_disc_image(opt.import_paths[0]))) {
        std::string dir = opt.cdtv_path;
        if (dir.empty()) {
            std::string data = user_data_dir(opt);
            if (data.empty()) {
                std::fprintf(stderr, "x2recomp: no per-user data directory; use --data-dir DIR or --cdtv DIR\n");
                return 1;
            }
            dir = data + "cdtv";
        }
        return x2::cdtv::import_cli(opt.import_cdtv.empty() ? opt.import_paths[0] : opt.import_cdtv, dir);
    }
    if (!opt.import_paths.empty()) { // --import: into --assets DIR if given, else <data dir>/assets; then exit
        std::string data = opt.assets.empty() ? user_data_dir(opt) : "";
        if (opt.assets.empty() && data.empty()) {
            std::fprintf(stderr, "x2recomp: no per-user data directory; use --data-dir DIR or --assets DIR\n");
            return 1;
        }
        return x2::import::import_cli(opt.import_paths, opt.assets.empty() ? data + "assets" : opt.assets);
    }
#ifdef X2_HAVE_SDL
    // Window mode: defaults < settings.cfg < command line. Headless runs never read it, so tests and replays
    // do not depend on the user's settings.
    std::string pref_dir, settings_path;
    x2::Settings file_settings;
    bool settings_dirty = false;
    if (!opt.headless) {
        pref_dir = user_data_dir(opt);
        std::error_code ec;
        if (!pref_dir.empty()) std::filesystem::create_directories(pref_dir, ec);
        if (!pref_dir.empty() && !ec) {
            settings_path = pref_dir + "settings.cfg";
            if (std::ifstream in(settings_path); in) {
                std::stringstream text;
                text << in.rdbuf();
                std::string serr;
                if (!x2::parse_settings(text.str(), file_settings, serr))
                    std::fprintf(stderr, "x2recomp: %s:\n%s", settings_path.c_str(), serr.c_str());
            } else if (std::ofstream out(settings_path); out) {
                out << x2::format_settings(file_settings);
            }
            std::printf("settings: %s\n", settings_path.c_str());
            Options merged;
            apply_settings(file_settings, merged);
            if (!parse_args(argc, argv, merged)) { usage(); return 2; }
            opt = std::move(merged);
            if (opt.persist_dir.empty() && !opt.no_persist) opt.persist_dir = pref_dir;
        } else {
            std::fprintf(stderr, "x2recomp: no per-user data directory%s%s: settings and high scores are not kept\n",
                         pref_dir.empty() ? "" : " ", pref_dir.c_str());
            pref_dir.clear();
        }
    }
#endif
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

    constexpr int kW = x2::Video::kFbWidth, kH = x2::Video::kFbHeight;
#ifdef X2_HAVE_SDL
    std::unique_ptr<x2::SdlDisplay> display;
#endif
    // Asset lookup: --assets DIR, else ./generated/assets (dev checkout, tools/prep_assets.py), else the import in
    // the per-user data directory. Without any, the window asks for disk images (first run).
    if (opt.assets.empty()) {
        bool dev_assets = std::filesystem::exists("generated/assets/game_0400.bin");
        std::string data = dev_assets ? "" : user_data_dir(opt);
        if (!dev_assets && data.empty()) {
            std::fprintf(stderr, "x2recomp: no per-user data directory; use --data-dir DIR or --assets DIR\n");
            return 3;
        }
        std::string imported = data + "assets", why;
        if (dev_assets) opt.assets = "generated/assets";
        else if (x2::import::ensure_imported(imported, why)) opt.assets = imported;
#ifdef X2_HAVE_SDL
        else if (!opt.headless) {
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                std::fprintf(stderr, "x2recomp: SDL_Init failed: %s (use --headless)\n", SDL_GetError());
                return 4;
            }
            display = std::make_unique<x2::SdlDisplay>();
            std::string derr;
            if (!display->open("Xenon 2 Megablast - x2recomp " X2_VERSION, kW, kH, opt.scale, opt.fullscreen, opt.present.effective().aspect, derr)) {
                std::fprintf(stderr, "x2recomp: cannot open window: %s\n", derr.c_str());
                return 4;
            }
            if (!x2::run_import_screen(*display, imported, why)) { SDL_Quit(); return 0; }
            opt.assets = imported;
        }
#endif
        else {
            std::fprintf(stderr, "x2recomp: no game data: %s.\nImport your own disk images first: x2recomp --import "
                                 "PATH [PATH2] (ADF, ADZ, ZIP or a folder), or give --assets DIR\n",
                         why.c_str());
            return 3;
        }
    }
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
    m.set_timing(opt.timing);
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
    // Assists and cheats (README "Assists and cheats"). Game variables, all verified in
    // docs/re-notes/trace-coverage.md §2: energy $CC6 (full = $27, refilled to that on respawn at $7AF6),
    // ships $CCA (0 whenever no game is in progress, which is what tells a level from the menus), credits
    // $CF0 (the display at $8F56 pokes one ASCII digit, so 9 is the most that can be shown).
    constexpr uint32_t kEnergy = 0xCC6, kEnergyFull = 0x27, kShips = 0xCCA, kCredits = 0xCF0;
    constexpr int kMaxCredits = 9, kGameCredits = 3; // the game's own starting value
    // The ship's death routine, and the counter of the push-back sequence that can reach it without any
    // energy being lost ($42E counts down from -1; at -$11 the ship is crushed, $5FE0).
    constexpr uint32_t kDeathRoutine = 0x5D00, kPushBack = 0x42E;
    bool god_mode = opt.god_mode;
    bool cheated = opt.god_mode || opt.credits > kGameCredits;
    uint16_t last_ships = 0;
    uint32_t god_saves = 0;
    // High-score persistence (runtime/persist.h): restore after every image load, save when name entry ends.
    std::string hs_path;
    const uint64_t image_hash = x2::fnv1a64(assets.game_image.data(), assets.game_image.size());
    x2::HiscoreTable hs_saved{};
    auto restore_hiscores = [&]() {
        if (hs_path.empty()) return;
        x2::HiscoreTable t;
        std::string why;
        if (x2::load_hiscore_file(hs_path, image_hash, t, why)) {
            x2::write_hiscores(m, t);
            std::printf("high scores: loaded %s\n", hs_path.c_str());
        } else {
            std::printf("high scores: %s: %s; using the game's own table\n", hs_path.c_str(), why.c_str());
        }
        hs_saved = x2::read_hiscores(m);
    };
    if (!opt.persist_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opt.persist_dir, ec);
        hs_path = (fs::path(opt.persist_dir) / "highscores.bin").string();
        restore_hiscores();
        m.add_trap(x2::kHiscoreSavedPc, [&](x2::Machine& mm) {
            x2::HiscoreTable t = x2::read_hiscores(mm);
            if (t == hs_saved) return;
            if (cheated && !opt.cheat_scores) {
                // The game's own table still shows the score; it just does not outlive the session.
                std::printf("high scores: not saved, a cheat was used this session (cheat_scores = on to save anyway)\n");
                hs_saved = t;
                return;
            }
            std::string why;
            if (x2::save_hiscore_file(hs_path, image_hash, t, why)) std::printf("high scores: saved %s\n", hs_path.c_str());
            else std::fprintf(stderr, "x2recomp: high scores not saved: %s\n", why.c_str());
            hs_saved = t; // do not retry every entry; the next change tries again
        });
    }
    // God mode, the part energy cannot cover: catch the death routine at its entry and return straight away.
    // A trap rather than a byte patch in chip RAM, because the translated executor verifies each
    // instruction's bytes and a patched routine would fall back to the interpreter; a trap that moves the PC
    // behaves identically either way (runtime/recomp/x2r_runtime.h, X2R_BOUNDARY). $5D00 has exactly two
    // callers -- $5D86 when the energy runs out and $5FE0 when the push-back sequence reaches its last step,
    // which costs no energy at all -- so hooking the routine itself covers every way the ship can die.
    // Returning leaves the dying flag $D84 clear, so the ship's handler carries on as if the hit had been
    // survived, and clearing $42E ends the push-back so it cannot sit on its last step and die again at once.
    // Installed only when god mode could be used: a headless run without it is left completely alone, so no
    // replay or golden hash can move.
    if (opt.god_mode || !opt.headless) {
        m.add_trap(kDeathRoutine, [&](x2::Machine& mm) {
            if (!god_mode) return;
            ++god_saves;
            if (opt.input_debug)
                std::printf("[god] frame %u: death skipped, called from $%06X (energy %u, push-back %d)\n",
                            mm.frame, mm.read32(mm.areg(7)) & 0xFFFFFF, mm.read16(kEnergy),
                            int16_t(mm.read16(kPushBack)));
            mm.write16(kEnergy, kEnergyFull);
            mm.write16(kPushBack, 0);
            mm.do_rts();
        });
    }
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
#ifdef X2_HAVE_SDL
    std::unique_ptr<x2::input::InputManager> input;
    x2::SdlAudioOut audio_out;
    size_t audio_max_queued = 0;
    uint64_t audio_underruns = 0, audio_underrun_ns = 0;
    uint64_t last_frame_ns = 0;
    // One emulated frame of host time: 313 or 312 lines of 227 colour clocks (49.92 or 50.08 Hz).
    x2::FramePacer pacer(uint64_t(m.lines_per_frame()) * x2::kColorClocksPerLine * 1'000'000'000ull / x2::Paula::kPalColorClock);
    uint32_t frames_due = 0;
    bool paused = false;
    x2::input::GrabPolicy grab;
    auto grab_mode = x2::input::GrabPolicy::Mode::Auto;
    bool grabbed = false, grab_warned = false;
#ifdef __APPLE__
    bool grab_noted = false; // the one-off note that SDL's Cocoa grab cannot do anything
#endif
    x2::input::CursorPolicy cursor;
    bool cursor_visible = true; // SDL starts with it shown
    if (!opt.headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            std::fprintf(stderr, "x2recomp: SDL_Init failed: %s (use --headless)\n", SDL_GetError());
            return 4;
        }
        if (!display) { // else already opened by the first-run import screen
            display = std::make_unique<x2::SdlDisplay>();
            std::string derr;
            if (!display->open("Xenon 2 Megablast - x2recomp " X2_VERSION, kW, kH, opt.scale, opt.fullscreen, opt.present.effective().aspect, derr)) {
                std::fprintf(stderr, "x2recomp: cannot open window: %s\n", derr.c_str());
                return 4;
            }
        }
        x2::input::LoadedBindings cfg = x2::input::load_bindings(pref_dir);
        if (!cfg.error.empty()) std::fprintf(stderr, "x2recomp: %s: %s", cfg.path.c_str(), cfg.error.c_str());
        std::printf("input bindings: %s%s%s\n", cfg.path.c_str(), cfg.note.empty() ? "" : ": ", cfg.note.c_str());
        input = std::make_unique<x2::input::InputManager>(cfg.bindings);
        input->set_debug(opt.input_debug);
        if (opt.input_debug) std::printf("%s", x2::input::describe_bindings(cfg.bindings).c_str());
        if (!x2::input::GrabPolicy::parse_mode(opt.grab_keyboard, grab_mode))
            std::fprintf(stderr, "x2recomp: grab_keyboard: expected auto, on or off\n");
        grab.set_mode(grab_mode);
        grab.set_focused(true);
        for (int port : {1, 2}) {
            input->autofire(port).set_enabled(opt.autofire);
            input->autofire(port).set_rate(opt.autofire_rate);
        }
        if (opt.autofire)
            std::printf("autofire: on, %d pulses per second (every %d frames), during a level only\n",
                        opt.autofire_rate, input->autofire(2).period_frames());
        if (opt.god_mode) std::printf("god mode: on (CHEAT: the ship's energy never runs out)\n");
        if (opt.credits != kGameCredits)
            std::printf("credits: a new game will start with %d%s\n", opt.credits,
                        opt.credits > kGameCredits ? " (CHEAT)" : "");
        x2::input::CursorPolicy::Mode cursor_mode;
        if (!x2::input::CursorPolicy::parse_mode(opt.cursor, cursor_mode))
            std::fprintf(stderr, "x2recomp: cursor: expected auto, show or hide\n");
        else
            cursor.set_mode(cursor_mode);
        cursor.set_focused(true);
        if (!opt.no_audio) {
            std::string aerr;
            if (audio_out.open(opt.audio.output_rate, opt.audio_latency, aerr)) {
                m.paula.set_output_enabled(true);
                audio_out.set_volume(float(opt.volume) / 100.0f);
            } else std::fprintf(stderr, "x2recomp: no audio output: %s\n", aerr.c_str());
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
    // CDTV CD music (runtime/cdtv/cdtv_music.h). Headless runs leave the machine untouched unless it is asked for;
    // the window installs it so the music-source hotkey works. It never changes the game's state.
    std::unique_ptr<x2::cdtv::CdtvMusic> cdtv;
    x2::cdtv::Disc cd_disc;
    FILE* cdtv_log = nullptr;
    x2::cdtv::MusicSource music_source = x2::cdtv::MusicSource::Amiga;
    x2::cdtv::parse_music_source(opt.music, music_source);
    if (!opt.headless || music_source != x2::cdtv::MusicSource::Amiga || opt.music_volume != 100 || opt.sfx_volume != 100 ||
        opt.protect_music || !opt.cdtv_path.empty() || !opt.cdtv_log.empty()) {
        cdtv = std::make_unique<x2::cdtv::CdtvMusic>();
        std::string path = opt.cdtv_path, why;
        if (path.empty()) {
            std::string data = user_data_dir(opt);
            if (!data.empty() && fs::exists(data + "cdtv/manifest.txt")) path = data + "cdtv";
        }
        if (!path.empty()) {
            if (x2::cdtv::open_disc(path, cd_disc, why)) {
                std::printf("cdtv: %zu CD tracks (%.1f min) %s %s\n", cd_disc.tracks.size(),
                            double(cd_disc.total_frames()) / x2::cdtv::kSampleRate / 60.0,
                            cd_disc.imported ? "imported in" : "in place from", cd_disc.where.c_str());
                cdtv->set_disc(&cd_disc);
            } else {
                std::fprintf(stderr, "x2recomp: CDTV disc audio: %s\n", why.c_str());
            }
        }
        x2::cdtv::MusicMap map;
        x2::parse_cdtv_tracks(opt.cdtv_tracks, map.track);
        cdtv->set_map(map);
        x2::cdtv::PauseMode pm = x2::cdtv::PauseMode::Keep;
        x2::cdtv::parse_pause_mode(opt.cd_pause, pm);
        cdtv->player().configure(opt.audio.output_rate, opt.cd_crossfade);
        cdtv->player().set_pause_mode(pm);
        if (!opt.cdtv_log.empty()) {
            cdtv_log = std::fopen(opt.cdtv_log.c_str(), "w");
            if (!cdtv_log) { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.cdtv_log.c_str()); return 3; }
            std::fprintf(cdtv_log, "# frame line event track caller\n");
            cdtv->set_log(cdtv_log);
        }
        cdtv->install(m);
        cdtv->set_volumes(opt.music_volume, opt.sfx_volume);
        cdtv->set_protect_music(opt.protect_music);
        cdtv->set_source(music_source);
        if (opt.protect_music)
            std::printf("protect music: on (the tune keeps its voices through the sound effects; not the "
                        "original's behaviour)\n");
        if (music_source != x2::cdtv::MusicSource::Amiga || !opt.headless)
            std::printf("music: %s\n", x2::cdtv::music_source_name(cdtv->effective_source()));
    }

    FILE* watch_file = nullptr;
    if (!opt.watch_log.empty()) {
        watch_file = std::fopen(opt.watch_log.c_str(), "w");
        if (!watch_file) { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.watch_log.c_str()); return 3; }
        std::fprintf(watch_file, "# frame");
        for (uint32_t a : opt.watch) std::fprintf(watch_file, " w%04X", a);
        std::fprintf(watch_file, " idle blt\n");
    }
    if (!opt.blit_log.empty()) {
        m.blit_log = std::fopen(opt.blit_log.c_str(), "w");
        if (!m.blit_log) { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.blit_log.c_str()); return 3; }
        m.blit_log_from = opt.blit_log_from;
        m.blit_log_to = opt.blit_log_to;
    }
    x2::modern::DrawCapture draw_capture;
    FILE* draw_log_file = nullptr;
    if (!opt.draw_log.empty()) {
        draw_log_file = std::fopen(opt.draw_log.c_str(), "w");
        if (!draw_log_file) { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.draw_log.c_str()); return 3; }
        draw_capture.keep = false;
        draw_capture.on_step = [draw_log_file, &opt](const x2::modern::Step& s) {
            if (s.swap_frame >= opt.draw_log_from && s.swap_frame <= opt.draw_log_to) x2::modern::write_step_log(draw_log_file, s);
        };
        draw_capture.attach(m);
    }
    // The picture: faithful frames, or interpolated ones from the draw capture (modern/present.h).
    x2::modern::Presenter presenter;
    auto asset_lib = std::make_shared<x2::modern::AssetLibrary>();
    for (const std::string& pack : opt.asset_packs) {
        std::vector<std::string> msgs;
        asset_lib->load(pack, msgs);
        for (const std::string& msg : msgs) std::printf("asset pack: %s\n", msg.c_str());
    }
    presenter.set_assets(asset_lib);
    x2::modern::AssetDumper dumper;
    const bool keep_capture = opt.modern_check || !opt.modern_shots.empty() || !opt.dump_assets.empty();
    presenter.configure(m, opt.present, m.lines_per_frame(), keep_capture);
    if (!opt.dump_assets.empty()) {
        std::string why;
        if (!dumper.open(opt.dump_assets, why)) { std::fprintf(stderr, "x2recomp: --dump-assets: %s\n", why.c_str()); return 3; }
        presenter.interp()->capture.on_step = [&dumper](const x2::modern::Step& s) { dumper.step(s); };
    }
    if (presenter.interp()) {
        if (draw_log_file) { std::fprintf(stderr, "x2recomp: --draw-log and --modern cannot be combined\n"); return 2; }
        if (!opt.modern_out.empty() && opt.modern_out != "-") fs::create_directories(opt.modern_out);
        if (!opt.modern_shots.empty()) fs::create_directories(opt.modern_shots);
    }
    if (!opt.headless || !opt.modern_out.empty()) std::printf("picture: %s\n", presenter.options().describe().c_str());
    struct ModernCheck {
        uint32_t pairs = 0, start_exact = 0, end_exact = 0, smooth_end_exact = 0, compared = 0, frame_exact = 0, wide_exact = 0;
        uint64_t smooth_diff_pixels = 0;
        uint32_t last_step = ~0u, pending_step = ~0u, pending_frame = 0;
        std::vector<uint8_t> pending_picture;
        bool shots_done = false;
    } mcheck;
    uint64_t modern_host_frame = 0;
#ifdef X2_HAVE_SDL
    // Interpolated presentation: emulated time for each display refresh (modern/present.h).
    const double frame_ns = double(m.lines_per_frame()) * x2::kColorClocksPerLine * 1e9 / x2::Paula::kPalColorClock;
    x2::modern::PresentClock present_clock(frame_ns);
    x2::modern::VsyncTimeline present_grid;
    uint64_t presents = 0, present_ns = 0, picture_ns = 0, frame_run_ns = 0, frames_timed = 0, slow_presents = 0;
    auto view_of = [](const x2::modern::PresentOptions& p) {
        x2::View v;
        v.filter = p.filter;
        v.aspect = p.aspect;
        v.integer = p.integer;
        v.curvature = p.curvature;
        return v;
    };
    auto show = [&](const x2::modern::Canvas& c) {
        display->present(c.rgba, c.w, c.h, c.scale, view_of(presenter.options()));
    };
    auto retitle = [&]() {
        x2::input::WindowModes modes;
        modes.keyboard_grabbed = grabbed;
        modes.autofire = input && input->autofire(2).enabled();
        modes.protect_music = cdtv && cdtv->protect_music(); // what was asked, not the 20 ms fade state
        modes.god_mode = god_mode;
        modes.cheat_used = cheated;
        display->set_title("Xenon 2 Megablast - " + presenter.options().describe() +
                           x2::input::window_mode_suffix(modes));
    };
    // Keyboard grab (runtime/input/input.h GrabPolicy): follow the policy whenever the focus, the pause or the
    // mode changes. Releasing it is always safe, so a failure only ever leaves it off.
    auto follow_grab = [&]() {
        const bool want = grab.want_grab();
        if (want == grabbed) return;
        std::string why;
        if (!display->set_keyboard_grab(want, why)) {
            if (!grab_warned) {
                std::fprintf(stderr, "x2recomp: cannot grab the keyboard: %s\n", why.c_str());
                grab_warned = true;
            }
            return;
        }
        grabbed = want;
        input->log_grab(want, "policy");
        retitle();
#ifdef __APPLE__
        if (want && !grab_noted) {
            std::printf("keyboard grab: SDL's macOS backend only silences the system shortcuts when SDL itself was\n"
                        "  built with SDL_MAC_NO_SANDBOX, which this build is not, so expect no change. The default\n"
                        "  bindings avoid the shortcut keys instead (x2recomp --list-keys).\n");
            grab_noted = true;
        }
#endif
    };
    // Mouse cursor (runtime/input/input.h CursorPolicy): the game has no mouse input, so the arrow is only in
    // the way while playing. SDL_HideCursor only stops drawing it; the pointer is never confined or captured.
    auto follow_cursor = [&](const char* why) {
        const bool want = cursor.want_visible();
        if (want == cursor_visible) return;
        if (!(want ? SDL_ShowCursor() : SDL_HideCursor())) {
            std::fprintf(stderr, "x2recomp: cannot %s the mouse cursor: %s\n", want ? "show" : "hide", SDL_GetError());
            return;
        }
        cursor_visible = want;
        input->log_cursor(want, why);
    };
    // After a picture hotkey: apply, retitle, remember in settings.cfg.
    auto apply_picture = [&]() {
        presenter.configure(m, opt.present, m.lines_per_frame(), keep_capture);
        present_clock.reset();
        const auto& e = presenter.options();
        display->set_aspect(e.aspect);
        retitle();
        std::printf("picture: %s\n", e.describe().c_str());
        file_settings.faithful = opt.present.faithful;
        file_settings.interp = opt.present.interp;
        file_settings.video_filter = opt.present.filter;
        file_settings.aspect = opt.present.aspect;
        file_settings.upgrade = opt.present.upgrade;
        file_settings.wide = x2::modern::wide_name(opt.present.wide);
        file_settings.hd_panel = opt.present.hd_panel;
        settings_dirty = true;
        display->set_picture_width(kW + 2 * presenter.wide());
        display->set_scale(opt.scale);
    };
    if (display) {
        retitle();
        display->set_picture_width(kW + 2 * presenter.wide());
        display->set_scale(opt.scale);
        follow_grab();
        follow_cursor("start");
    }
#endif
    FILE* pc_log_file = nullptr;
    if (!opt.pc_log.empty()) {
        pc_log_file = std::fopen(opt.pc_log.c_str(), "w");
        if (!pc_log_file) { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.pc_log.c_str()); return 3; }
        m.on_insn_start = [pc_log_file, &opt](x2::Machine& mm, uint32_t pc) {
            if (mm.frame < opt.pc_log_from || mm.frame > opt.pc_log_to) return;
            if (opt.pc_log_pcs.empty()) { // --insn-log
                uint32_t a = pc & x2::kChipMask;
                std::fprintf(pc_log_file, "%u %d %u %lld %06X %04X\n", mm.frame, mm.line, mm.hpos(), (long long)mm.cpu_now(),
                             pc, (mm.chip[a] << 8) | mm.chip[(a + 1) & x2::kChipMask]);
                return;
            }
            if (std::find(opt.pc_log_pcs.begin(), opt.pc_log_pcs.end(), pc) == opt.pc_log_pcs.end()) return;
            std::fprintf(pc_log_file, "%u %d %u %lld %06X\n", mm.frame, mm.line, mm.hpos(), (long long)mm.cpu_now(), pc);
        };
    }
    FILE* pc_trace_file = nullptr;
    if (!opt.pc_trace.empty()) {
        pc_trace_file = std::fopen(opt.pc_trace.c_str(), "w");
        if (!pc_trace_file) { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.pc_trace.c_str()); return 3; }
        auto video_line_end = m.on_line_end;
        m.on_line_end = [video_line_end, pc_trace_file, &opt](x2::Machine& mm, int line) {
            if (video_line_end) video_line_end(mm, line);
            if (mm.frame >= opt.pc_trace_from && mm.frame <= opt.pc_trace_to)
                std::fprintf(pc_trace_file, "%u %d %06X %llu\n", mm.frame, line, mm.pc() & 0xFFFFFF,
                             (unsigned long long)mm.cpu_wait_cycles);
        };
    }
    FILE* trace_file = nullptr;
    std::map<uint32_t, uint32_t> window_pcs;
    std::array<uint32_t, 0x100> window_reads{};
    std::array<uint32_t, 32> window_cia{};
    uint32_t window_start = 0;

    std::printf("x2recomp headless: %u frames, %d lines x %d CPU cycles per frame\n", opt.frames,
                m.lines_per_frame(), x2::kCpuCyclesPerLine);
    for (uint32_t f = 0; opt.frames == 0 || f < opt.frames; ++f) {
#ifdef X2_HAVE_SDL
        if (display) {
            // Wait until the pacer says the next 50 Hz frame is due, handling host events meanwhile.
            bool quit = false;
            while (!quit && frames_due == 0) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    auto cmd = input->handle_event(e);
                    switch (cmd) {
                    case x2::input::HostCommand::Quit: quit = true; break;
                    case x2::input::HostCommand::Pause:
                        paused = !paused;
                        audio_out.set_paused(paused); // stops the device and drops queued audio
                        grab.set_paused(paused);      // a paused game must not hold the system's shortcuts
                        cursor.set_paused(paused);    // ... and the user wants the pointer back while frozen
                        break;
                    case x2::input::HostCommand::Grab:
                        grab_mode = grab_mode == x2::input::GrabPolicy::Mode::On ? x2::input::GrabPolicy::Mode::Off
                                                                                : x2::input::GrabPolicy::Mode::On;
                        grab.set_mode(grab_mode);
                        file_settings.grab_keyboard = x2::input::GrabPolicy::mode_name(grab_mode);
                        settings_dirty = true;
                        std::printf("keyboard grab: %s\n", file_settings.grab_keyboard.c_str());
                        break;
                    case x2::input::HostCommand::Reset:
                        reset_machine(m, assets);
                        if (cdtv) cdtv->reset();
                        presenter.reset();
                        present_clock.reset();
                        restore_hiscores();
                        if (!wav.is_open()) m.paula.output().clear(); // drop the pre-reset tail
                        audio_out.flush();
                        break;
                    case x2::input::HostCommand::Fullscreen:
                        display->toggle_fullscreen();
                        file_settings.fullscreen = display->fullscreen();
                        settings_dirty = true;
                        break;
                    case x2::input::HostCommand::VolumeDown:
                    case x2::input::HostCommand::VolumeUp: {
                        int step = cmd == x2::input::HostCommand::VolumeUp ? 10 : -10;
                        opt.volume = std::clamp((opt.volume + step) / 10 * 10, 0, 100);
                        audio_out.set_volume(float(opt.volume) / 100.0f);
                        file_settings.volume = opt.volume;
                        settings_dirty = true;
                        std::printf("volume %d%%\n", opt.volume);
                        break;
                    }
                    case x2::input::HostCommand::ScaleDown:
                    case x2::input::HostCommand::ScaleUp:
                        opt.scale = std::clamp(opt.scale + (cmd == x2::input::HostCommand::ScaleUp ? 1 : -1), 1, 8);
                        display->set_scale(opt.scale);
                        file_settings.scale = opt.scale;
                        settings_dirty = true;
                        break;
                    case x2::input::HostCommand::Faithful:
                        opt.present.faithful = !opt.present.faithful;
                        apply_picture();
                        display->set_scale(opt.scale);
                        break;
                    // The other picture keys change the custom picture, and switch to it.
                    case x2::input::HostCommand::Filter:
                        opt.present.filter = x2::VideoFilter((int(opt.present.filter) + 1) % int(x2::VideoFilter::kCount));
                        opt.present.faithful = false;
                        apply_picture();
                        break;
                    case x2::input::HostCommand::Interp:
                        opt.present.interp = !opt.present.interp;
                        opt.present.faithful = false;
                        apply_picture();
                        break;
                    case x2::input::HostCommand::Aspect:
                        opt.present.aspect = !opt.present.aspect;
                        opt.present.faithful = false;
                        apply_picture();
                        break;
                    case x2::input::HostCommand::Upgrade:
                        opt.present.upgrade = !opt.present.upgrade;
                        opt.present.faithful = false;
                        if (opt.present.upgrade && asset_lib->empty())
                            std::printf("asset packs: none loaded (--asset-pack PATH or asset_packs in settings.cfg)\n");
                        apply_picture();
                        break;
                    case x2::input::HostCommand::Wide:
                        opt.present.wide = x2::modern::next_wide(opt.present.wide);
                        opt.present.faithful = false;
                        apply_picture();
                        break;
                    case x2::input::HostCommand::Panel:
                        opt.present.hd_panel = !opt.present.hd_panel;
                        opt.present.faithful = false;
                        apply_picture();
                        break;
                    case x2::input::HostCommand::Music:
                        if (cdtv) {
                            music_source = x2::cdtv::MusicSource((int(cdtv->source()) + 1) % 3);
                            cdtv->set_source(music_source);
                            file_settings.music = x2::cdtv::music_source_name(music_source);
                            settings_dirty = true;
                            std::printf("music: %s%s\n", file_settings.music.c_str(),
                                        cdtv->effective_source() != music_source ? " (no CDTV disc audio: Amiga music)" : "");
                        }
                        break;
                    case x2::input::HostCommand::ProtectMusic: {
                        if (!cdtv) { // only when no CD layer was installed at all; the window always has one
                            std::printf("protect music: not available in this run\n");
                            break;
                        }
                        cdtv->set_protect_music(!cdtv->protect_music());
                        opt.protect_music = cdtv->protect_music();
                        file_settings.protect_music = opt.protect_music;
                        settings_dirty = true;
                        retitle();
                        // Nothing to protect when the CD is the only music: the call is a no-op, so say so
                        // rather than claim an effect the user will not hear.
                        const bool inert = cdtv->effective_source() == x2::cdtv::MusicSource::Cdtv;
                        std::printf("protect music: %s%s\n", opt.protect_music ? "on" : "off",
                                    !inert ? (opt.protect_music ? " (about 3.9 dB quieter by design; F8 turns it up)" : "")
                                           : " (no effect while the CDTV disc is the music)");
                        break;
                    }
                    case x2::input::HostCommand::Autofire: {
                        const bool on = !input->autofire(2).enabled();
                        for (int port : {1, 2}) input->autofire(port).set_enabled(on);
                        file_settings.autofire = on;
                        settings_dirty = true;
                        retitle();
                        std::printf("autofire: %s%s\n", on ? "on" : "off",
                                    on ? " (while fire is held, during a level only)" : "");
                        break;
                    }
                    case x2::input::HostCommand::GodMode:
                        god_mode = !god_mode;
                        if (god_mode) cheated = true; // stays set: the session is flagged once a cheat is used
                        file_settings.god_mode = god_mode;
                        settings_dirty = true;
                        retitle();
                        std::printf("god mode: %s%s\n", god_mode ? "on" : "off",
                                    god_mode ? " (CHEAT: the ship's energy never runs out; no high scores saved)" : "");
                        break;
                    case x2::input::HostCommand::AddCredit: {
                        if (m.read16(kShips) == 0) { // nothing to credit outside a game
                            std::printf("credits: only during a game\n");
                            break;
                        }
                        const uint16_t now = m.read16(kCredits);
                        if (now >= kMaxCredits) {
                            std::printf("credits: already %d, the most the display can show\n", kMaxCredits);
                            break;
                        }
                        m.write16(kCredits, uint16_t(now + 1));
                        cheated = true;
                        retitle();
                        std::printf("credits: %d (CHEAT: no high scores saved)\n", now + 1);
                        break;
                    }
                    case x2::input::HostCommand::None: break;
                    }
                }
                grab.set_focused(input->focused());
                cursor.set_focused(input->focused());
                follow_grab();
                follow_cursor("focus or pause");
                uint64_t now = SDL_GetTicksNS();
                if (paused) {
                    pacer.reset(now);
                    present_clock.reset();
                    display->redraw(view_of(presenter.options())); // hold the picture
                    SDL_Delay(10);
                    continue;
                }
                if (presenter.interpolating()) {
                    // One present per display refresh, each showing the emulated time of the refresh it will be
                    // displayed at. With vsync the present waits for that refresh; without, wait for its slot.
                    // The display mode's rate until the vsync waits have been measured (a window can be composited
                    // at a lower rate than the display's: 120 Hz on a 260 Hz macOS display).
                    if (float hz = display->refresh_hz(); hz > 20) present_grid.set_mode_period(1e9 / hz);
                    present_clock.lag = x2::modern::PresentClock::kMargin + present_grid.period() / frame_ns;
                    uint64_t at = present_grid.next(now);
                    if (!display->vsync() && at > now + 1'000'000) SDL_DelayNS(at - now - 1'000'000);
                    const uint64_t t0 = SDL_GetTicksNS();
                    presenter.edge_factor = std::clamp(display->output_scale(view_of(presenter.options())), 2, 4);
                    const x2::modern::Canvas c = presenter.at(present_clock.tau(at), video.front());
                    picture_ns += SDL_GetTicksNS() - t0;
                    show(c);
                    now = SDL_GetTicksNS();
                    present_grid.presented(now);
                    ++presents;
                    present_ns += now - t0;
                    slow_presents += double(now - t0) > present_grid.period() * 1.5;
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
                if (!frames_due && !presenter.interpolating()) { // (interpolating: the presents pace the loop)
                    uint64_t next = pacer.next_due_ns();
                    if (next > now + 1'000'000) SDL_DelayNS(next - now - 1'000'000);
                    else SDL_DelayNS(100'000);
                }
            }
            if (quit) {
                if (grabbed) { // never leave the desktop without its own shortcuts
                    std::string why;
                    display->set_keyboard_grab(false, why);
                    grabbed = false;
                }
                if (!cursor_visible) { // nor without a pointer
                    SDL_ShowCursor();
                    cursor_visible = true;
                    input->log_cursor(true, "quit");
                }
                break;
            }
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
            if (f >= p.from && f <= p.to) x2::apply_poke(m, p);
        // Assists and cheats, at the same frame boundary as the pokes above (not in an interpreter hook), so
        // the interpreter and the translated executor see exactly the same thing.
        {
            const uint16_t ships = m.read16(kShips);
            const bool in_game = ships != 0;
            if (in_game && last_ships == 0 && opt.credits != kGameCredits) {
                // A new game has just started: the game has set its own three credits, so replace them.
                m.write16(kCredits, uint16_t(opt.credits));
                std::printf("credits: %d\n", opt.credits);
            }
            last_ships = ships;
            if (god_mode && in_game && m.read16(kEnergy) < kEnergyFull) m.write16(kEnergy, kEnergyFull);
#ifdef X2_HAVE_SDL
            if (input) {
                input->autofire(2).set_in_game(in_game);
                input->autofire(1).set_in_game(in_game);
            }
#endif
        }

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
        m.blit_busy_lines = 0;
#ifdef X2_HAVE_SDL
        const uint64_t run_t0 = display ? SDL_GetTicksNS() : 0;
#endif
        m.run_frame();
        if (cdtv) cdtv->after_frame(m); // CD audio into this frame's Paula output, before any consumer
#ifdef X2_HAVE_SDL
        if (display) {
            const uint64_t t1 = SDL_GetTicksNS();
            present_clock.frame_done(t1, double(m.frame));
            frame_run_ns += t1 - run_t0;
            ++frames_timed;
        }
#endif
        if (watch_file) {
            // Labelled f + 1: the state at the start of the next frame, as the reference logs it at that vsync.
            uint32_t idle = 0;
            for (auto& [pc, n] : m.pc_samples)
                if (pc >= 0x2034 && pc < 0x2042) idle += n;
            std::fprintf(watch_file, "%u", f + 1);
            for (uint32_t a : opt.watch) std::fprintf(watch_file, " %04X", (m.chip[a & x2::kChipMask] << 8) | m.chip[(a + 1) & x2::kChipMask]);
            std::fprintf(watch_file, " %u %u\n", idle, m.blit_busy_lines);
        }
        if (tracing) { m.trace = nullptr; std::fclose(trace_file); trace_file = nullptr; }
        for (const auto& [sf, path] : opt.screenshots) {
            if (sf != f) continue;
            if (x2::write_image(path, video.front(), kW, kH)) std::printf("screenshot of frame %u -> %s\n", f, path.c_str());
            else std::fprintf(stderr, "x2recomp: cannot write %s\n", path.c_str());
        }
        for (const auto& [rf, path] : opt.ram_dumps)
            if (rf == f) x2::write_chip_ram(m, path);
        modern_after_frame(presenter, opt, mcheck, video, f, modern_host_frame);
#ifdef X2_HAVE_SDL
        for (const auto& [sf, path] : opt.present_shots) {
            if (sf != f) continue;
            const x2::modern::Canvas c = presenter.at(double(f + 1), video.front());
            const x2::View v = view_of(presenter.options());
            const int ow = c.w / c.scale * opt.scale, oh = v.aspect ? kW * opt.scale * 3 / 4 : kH * opt.scale;
            std::vector<uint8_t> img;
            if (x2::render_offscreen(c.rgba, c.w, c.h, c.scale, v, ow, oh, img) && x2::write_image(path, img, ow, oh))
                std::printf("present shot of frame %u (%s) -> %s\n", f, presenter.options().describe().c_str(), path.c_str());
            else std::fprintf(stderr, "x2recomp: cannot write %s: %s\n", path.c_str(), SDL_GetError());
        }
#endif
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
        if (display && --frames_due == 0 && !presenter.interpolating())
            show(presenter.at(double(m.frame), video.front())); // skip presents when catching up
        if (audio_out.is_open()) {
            audio_out.push(m.paula.output().data(), m.paula.output().size() / 2);
            audio_max_queued = std::max(audio_max_queued, audio_out.queued_frames());
            if (!wav.is_open()) m.paula.output().clear();
            // A dropout the player can hear: say so, at most once a second, so it is not mistaken for the
            // game's own silences (docs/re-notes/music-dropouts.md).
            if (uint64_t u = audio_out.underruns(); u != audio_underruns) {
                audio_underruns = u;
                if (uint64_t now = SDL_GetTicksNS(); now - audio_underrun_ns > 1'000'000'000) {
                    audio_underrun_ns = now;
                    std::printf("live audio: the output buffer ran dry (%llu so far); the host loop stalled for "
                                "more than %d ms. Raise --audio-latency if it keeps happening.\n",
                                (unsigned long long)u, opt.audio_latency);
                    std::fflush(stdout);
                }
            }
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
    if (frames_timed)
        std::printf("window: %.2f ms per emulated frame\n", double(frame_run_ns) / 1e6 / double(frames_timed));
    if (presents)
        std::printf("interpolated window: %llu presents on a %.1f Hz grid, %.2f ms per present (picture %.2f ms), %llu over "
                    "1.5 grid periods, clock snaps %u, extra lag %.2f frames, grid resyncs %u\n",
                    (unsigned long long)presents, 1e9 / present_grid.period(), double(present_ns) / 1e6 / double(presents),
                    double(picture_ns) / 1e6 / double(presents), (unsigned long long)slow_presents, present_clock.snaps(),
                    present_clock.extra_lag(), present_grid.resyncs());
    if (audio_out.is_open())
        std::printf("live audio: underruns %llu, dropped %llu frames, queued %zu at exit, max %zu (target %zu = %d ms)\n",
                    (unsigned long long)audio_out.underruns(), (unsigned long long)audio_out.dropped_frames(),
                    audio_out.queued_frames(), audio_max_queued, audio_out.target_frames(), opt.audio_latency);
    audio_out.close();
    input.reset();
    if (settings_dirty && !settings_path.empty()) {
        if (std::ofstream out(settings_path); out) out << x2::format_settings(file_settings);
        else std::fprintf(stderr, "x2recomp: cannot write %s\n", settings_path.c_str());
    }
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
    if (cdtv && cdtv->effective_source() != x2::cdtv::MusicSource::Amiga)
        std::printf("cdtv: %u cues, track %d at exit, %llu loops, %llu clipped samples\n", cdtv->cues(), cdtv->player().track(),
                    (unsigned long long)cdtv->player().loops(), (unsigned long long)cdtv->player().clipped());
    if (cdtv_log) std::fclose(cdtv_log);
    if (watch_file) std::fclose(watch_file);
    if (pc_trace_file) std::fclose(pc_trace_file);
    if (pc_log_file) std::fclose(pc_log_file);
    if (draw_log_file) { draw_capture.detach(m); std::fclose(draw_log_file); }
    if (!opt.dump_assets.empty()) {
        std::string why;
        const bool ok = dumper.finish(why);
        std::printf("dump-assets: %zu sprites, %zu tiles, %zu far lines, %zu palettes -> %s/manifest.json%s%s\n",
                    dumper.sprites(), dumper.tiles(), dumper.far_lines(), dumper.palettes(), opt.dump_assets.c_str(),
                    ok ? "" : ": ", why.c_str());
        if (!ok) return 3;
    }
    if (x2::modern::InterpPresenter* modern = presenter.interp()) {
        modern->detach(m);
        if (opt.modern_check)
            std::printf("modern check: %u interpolable step pairs; t=0 exact %u, t=1 exact %u (far-layer smoothing off); "
                        "t=1 exact with smoothing %u (%.2f differing pixels per pair); captured picture = rendered frame "
                        "%u of %u\n",
                        mcheck.pairs, mcheck.start_exact, mcheck.end_exact, mcheck.smooth_end_exact,
                        mcheck.pairs ? double(mcheck.smooth_diff_pixels) / mcheck.pairs : 0.0, mcheck.frame_exact,
                        mcheck.compared);
        if (opt.modern_check && modern->interp.opt.wide > 0)
            std::printf("modern check: surround %d pixels per side, centre exact at t=0 and t=1 in %u of %u pairs\n",
                        modern->interp.opt.wide, mcheck.wide_exact, mcheck.pairs);
        std::printf("modern: %llu frames presented, %llu interpolated, %.2f ms per interpolated frame (scale %d)\n",
                    (unsigned long long)modern->presented, (unsigned long long)modern->interpolated,
                    modern->interpolated ? double(modern->render_ns) / 1e6 / double(modern->interpolated) : 0.0,
                    modern->interp.opt.scale);
    }
    if (m.blit_log) std::fclose(m.blit_log);
    if (m.accurate())
        std::printf("timing: accurate, CPU wait %.1f%% of cycles, blitter busy %.1f%% of colour clocks, blits restarted "
                    "while busy %u, blitter register writes while busy %u\n",
                    100.0 * double(m.cpu_wait_cycles) / (double(m.frame) * m.lines_per_frame() * x2::kCpuCyclesPerLine + 1),
                    100.0 * double(m.blit_cck) / (double(m.frame) * m.lines_per_frame() * x2::kColorClocksPerLine + 1),
                    m.blit_restarts, m.blit_reg_writes_busy);
    else
        std::printf("timing: fast\n");
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
