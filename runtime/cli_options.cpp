// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "cli_options.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "cdtv/cdtv_music.h"

#ifndef X2_VERSION
#define X2_VERSION "unknown" // set from project(VERSION) in CMakeLists.txt (docs/RELEASING.md)
#endif

namespace x2::cli {

namespace {

// The names x2::input::CursorPolicy::parse_mode accepts (input/input.cpp), without needing the SDL-only input code.
bool cursor_mode_known(const std::string& name) {
#ifdef X2_HAVE_SDL
    x2::input::CursorPolicy::Mode m;
    return x2::input::CursorPolicy::parse_mode(name, m);
#else
    return name == "auto" || name == "show" || name == "on" || name == "yes" || name == "hide" || name == "off" || name == "no";
#endif
}

} // namespace

void usage(std::FILE* out) {
    std::fprintf(out,
                 "x2recomp " X2_VERSION "\n"
                 "usage: x2recomp [--headless] [--frames N] [--dump-dir DIR] [options]\n"
                 "  --help, --version   print this text or the version and exit (status 0)\n"
                 "  --print-data-dir    print the per-user data directory (imports, settings) and exit\n"
                 "  --list-keys         print the active key bindings (input.cfg) and exit\n"
                 "  --print-settings    print the settings a window run with these arguments would use (settings.cfg, then\n"
                 "                      the arguments on top of it) in settings.cfg format and exit; needs no window or game\n"
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
                 "  --game WHICH        disk (the Amiga disk release), cdtv (the CDTV release, imported from your CDTV\n"
                 "                      disc image), st (the Atari ST release, in development: --st-disk FILE.st or an\n"
                 "                      imported disk; --st-watch HEX,.. prints RAM words when they change; sound from the\n"
                 "                      YM2149, --audio-out/--audio-hash as for the Amiga; joystick = port 1, see --list-keys;\n"
                 "                      comparison with Hatari (docs/re-notes/atari-st-video.md): --st-frame-log FILE (a line\n"
                 "                      per frame), --st-shots-dir DIR --st-shots 100,200-260,1000-2000/50 (PNGs of those\n"
                 "                      frames), --st-input-offset N (a Hatari script's VBLs minus N), --st-profile FROM:TO\n"
                 "                      (busiest PCs), --st-ram-dump-at F:FILE, --st-state-hash: a digest of RAM, registers and\n"
                 "                      cycles with every framehash; --interp-only, --recomp-stats, --recomp-profile and\n"
                 "                      --trace-out apply to it too)\n"
                 "                      or auto (default: the disk release if imported, else the CDTV release, else the Atari ST one)\n"
                 "  --import PATH...    import your disk images (ADF, ADZ, DMS, extended ADF, IPF, SCP, KryoFlux, HFE, LHA,\n"
                 "                      WHDLoad, ZIP or a folder), your CDTV disc image (.cue, ZIP, folder, .bin, .iso), or\n"
                 "                      an Atari ST disk image (.st, .msa, .stx: into <data dir>/assets-st)\n"
                 "                      into the per-user data directory (or --assets DIR) and exit; exit code 0 ok,\n"
                 "                      1 cannot write, 2 bad input, 3 unsupported image (including a protected retail\n"
                 "                      disc 1 from a source without the key track), 4 confirmation required\n"
                 "  --apply-protection-patch  with --import: apply the 18-byte protection removal to a retail pressing\n"
                 "                      instead of keeping the key-track method; prints a disclaimer and needs\n"
                 "  --i-confirm-i-own-the-original  (never the default; the import is recorded as patched)\n"
                 "  --list-variants     list the disk variants the importer knows and how each is handled, and exit\n"
                 "  --diagnose-image PATH...  per-track report of disk images (formats, standard/raw tracks, key track,\n"
                 "                      detected variant; no game data is printed), and exit\n"
                 "  --data-dir DIR      per-user data directory (default: the SDL preferences directory)\n"
                 "  --report-every N    status report interval in frames (default 50)\n"
                 "  --ppm-every N       also dump the bitplane framebuffer every N frames\n"
                 "  --no-reglog         do not write per-frame register write logs\n"
                 "  --trace-frame F     write a disassembly trace of every instruction in frame F\n"
                 "  --key F:CODE        press Amiga raw key CODE (hex) at frame F (released 2 frames later)\n"
                 "  --fire F[:N]        hold joystick fire from frame F for N frames (default 3)\n"
                 "  --joy F:DIRS[:N]    hold joystick directions DIRS (letters U D L R F) from frame F for N frames\n"
                 "  --input-script FILE frame-indexed input script (tests/replays/README.md; with --game st: st/st_input.h)\n"
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
                 "  --import-cdtv PATH  import the CD audio of your CDTV disc image (ZIP, folder or .cue) into\n"
                 "                      <data dir>/cdtv (or --cdtv DIR) and exit. --import with a CDTV disc image also\n"
                 "                      imports the game (pre-linked into <data dir>/assets-cdtv, or --assets DIR; play it\n"
                 "                      with --game cdtv) and, with the audio tracks, the audio as above\n"
                 "  --chip-ram KB       chip RAM size, 512 or 1024; default: what the game build needs (512 for every disk\n"
                 "                      build). For tests; the 512 KB model is the faithful one\n"
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
                 "                      'upgrade' / 'noupgrade' (asset packs and palette overrides),\n"
                 "                      'audio' / 'noaudio' (the sound effects, speech and music of asset packs),\n"
                 "                      wide=W:H or\n"
                 "                      wide=N (surround: 16:10, 16:9, 21:9, 32:9, or N <= 320 pixels per side;\n"
                 "                      wide=off), 'hdpanel' / 'nohdpanel' (edge-upscaled status panel),\n"
                 "                      'faithful' (= --faithful)\n"
                 "  --asset-pack PATH   load a replacement asset pack (directory or .zip, docs/ASSET-PACKS.md);\n"
                 "                      repeatable, later packs override earlier ones; implies --modern upgrade\n"
                 "  --dump-assets DIR   write every distinct original graphic drawn in a run (sprites, tiles, far\n"
                 "                      layers, hit flashes, screens, status panel cells) as PNGs plus DIR/manifest.json,\n"
                 "                      for pack authors (your data only)\n"
                 "  --dump-audio DIR    write every distinct original sound effect and speech clip of a run as WAV files\n"
                 "                      plus DIR/manifest.json, for pack authors (your data only)\n"
                 "  --check-asset-pack PATH  validate an asset pack (directory or .zip) and print a report; exit code 0\n"
                 "                      valid, 1 warnings with --strict, 2 unusable or invalid entries, 3 unreadable.\n"
                 "                      Options: --game disk|cdtv|st and --build TAG (check\n"
                 "                      the scoping for one game), --against DUMPDIR (coverage and typos against a dump),\n"
                 "                      --full (decode every file), --strict (warnings are errors), --json\n"
                 "  --list-asset-packs [--json]  list the packs found in <data dir>/packs with id, name, version, author,\n"
                 "                      licence, games and whether they are enabled (settings packs_enabled), and exit\n"
                 "  --speech-volume N   percent (default 100): speech clips of asset packs (settings speech_volume)\n"
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
        case 'B': case 'b': d |= 32; break; // the second fire button (POTGOR bit 14; the CDTV release reads it)
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
    return p.size && num(addr, p.addr) && num(v, p.value) && p.addr + uint32_t(p.size) <= x2::kChipRamMax;
}

// Input script: one event per line, "#" comments. FRAME is absolute, or "+N" relative to the previous line.
//   FRAME fire [N]           hold fire for N frames (default 3)
//   FRAME key HEX            press Amiga raw key HEX (released 2 frames later)
//   FRAME joy DIRS [N]       hold directions/fire (letters U D L R F, B = second button) for N frames (default 1)
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
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--game" && std::string(argv[i + 1]) == "st") o.for_st = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--headless") o.headless = true;
        else if (a == "--frames") { auto v = next(); if (!v) return false; o.frames = uint32_t(std::strtoul(v, nullptr, 0)); o.frames_set = true; }
        else if (a == "--dump-dir") { auto v = next(); if (!v) return false; o.dump_dir = v; }
        else if (a == "--assets") { auto v = next(); if (!v) return false; o.assets = v; }
        else if (a == "--game") {
            auto v = next();
            if (!v || (std::string(v) != "disk" && std::string(v) != "cdtv" && std::string(v) != "auto" && std::string(v) != "st")) {
                std::fprintf(stderr, "--game: disk, cdtv, st or auto\n");
                return false;
            }
            o.game = v;
            o.game_explicit = true;
        }
        else if (a == "--st-disk") { auto v = next(); if (!v) return false; o.st_disk = v; }
        else if (a == "--st-fdc-speed") { auto v = next(); if (!v) return false; o.st_fdc_speed = std::max(1u, uint32_t(std::strtoul(v, nullptr, 0))); }
        else if (a == "--st-trace-tail") { auto v = next(); if (!v) return false; o.st_trace_tail = uint32_t(std::strtoul(v, nullptr, 0)); }
        else if (a == "--st-io-summary") o.st_io_summary = true;
        else if (a == "--st-watch") {
            auto v = next();
            if (!v) return false;
            std::istringstream ws(v);
            for (std::string tok; std::getline(ws, tok, ',');) o.st_watch.push_back(uint32_t(std::strtoul(tok.c_str(), nullptr, 16)));
        }
        else if (a == "--st-watch-pc") { // ADDR[:LEN]: the PC of the instruction that changes any byte of the range
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            const char* t = v + (v[0] == '$');
            o.st_watch_pc_addr = uint32_t(std::strtoul(t, &end, v[0] == '$' ? 16 : 0));
            o.st_watch_pc_len = *end == ':' ? uint32_t(std::strtoul(end + 1, nullptr, 0)) : 1;
            if (!o.st_watch_pc_len) return false;
        }
        else if (a == "--st-fdc-log") o.st_fdc_log = true;
        else if (a == "--st-state-hash") o.st_state_hash = true;
        else if (a == "--st-profile") { // FROM:TO
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            o.st_profile_from = uint32_t(std::strtoul(v, &end, 0));
            if (*end != ':') return false;
            o.st_profile_to = uint32_t(std::strtoul(end + 1, nullptr, 0));
        }
        else if (a == "--st-ram-dump-at") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            const uint32_t f = uint32_t(std::strtoul(v, &end, 0));
            if (*end != ':' || !end[1]) return false;
            o.st_ram_dumps.push_back({f, end + 1});
        }
        else if (a == "--st-input-offset") { auto v = next(); if (!v) return false; o.st_input_offset = std::atoi(v); }
        else if (a == "--st-frame-log") { auto v = next(); if (!v) return false; o.st_frame_log = v; }
        else if (a == "--st-shots-dir") { auto v = next(); if (!v) return false; o.st_shots_dir = v; }
        else if (a == "--st-shots") { auto v = next(); if (!v) return false; o.st_shots = v; }
        else if (a == "--st-joy-at") {
            auto v = next();
            if (!v) return false;
            char* end = nullptr;
            const uint32_t f = uint32_t(std::strtoul(v, &end, 0));
            if (*end != ':' || !end[1]) return false;
            o.st_joy.push_back({f, uint8_t(std::strtoul(end + 1, nullptr, 16))});
        }
        else if (a == "--data-dir") { auto v = next(); if (!v) return false; o.data_dir = v; }
        else if (a == "--import") {
            o.import_paths.clear();
            while (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) o.import_paths.push_back(argv[++i]);
            if (o.import_paths.empty()) return false;
        }
        else if (a == "--apply-protection-patch") o.apply_protection_patch = true;
        else if (a == "--i-confirm-i-own-the-original") o.confirm_own_original = true;
        else if (a == "--list-variants") o.list_variants = true;
        else if (a == "--diagnose-image") {
            o.diagnose_paths.clear();
            while (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) o.diagnose_paths.push_back(argv[++i]);
            if (o.diagnose_paths.empty()) return false;
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
            if (!v || !cursor_mode_known(v)) {
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
        else if (a == "--chip-ram") {
            auto v = next();
            if (!v) return false;
            const unsigned long kb = std::strtoul(v, nullptr, 10);
            if (kb != 512 && kb != 1024) { std::fprintf(stderr, "--chip-ram: 512 or 1024 (KB)\n"); return false; }
            o.chip_kb = uint32_t(kb);
        }
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
                else if (item == "audio" || item == "noaudio") {
                    x2::pack::AssetPackSettings ps;
                    x2::pack::apply_modern_item(item, ps);
                    o.audio_upgrade = ps.audio_upgrade;
                }
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
        else if (a == "--dump-audio") { auto v = next(); if (!v) return false; o.dump_audio = v; }
        else if (a == "--check-asset-pack") { auto v = next(); if (!v) return false; o.check_pack.path = v; }
        else if (a == "--against") { auto v = next(); if (!v) return false; o.check_pack.against = v; o.check_flag_given = true; }
        else if (a == "--build") { auto v = next(); if (!v) return false; o.check_pack.build = v; o.check_flag_given = true; }
        else if (a == "--full") { o.check_pack.full = true; o.check_flag_given = true; }
        else if (a == "--strict") { o.check_pack.strict = true; o.check_flag_given = true; }
        else if (a == "--json") { o.check_pack.json = true; o.json_given = true; }
        else if (a == "--list-asset-packs") o.list_asset_packs = true;
        else if (a == "--speech-volume") {
            auto v = next();
            if (!v) return false;
            o.speech_volume = std::clamp(std::atoi(v), 0, 100);
            o.speech_volume_given = true;
        }
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
        else if (a == "--input-script") {
            auto v = next();
            if (!v) return false;
            if (o.for_st) o.st_input_script = v; // the ST has its own script reader (st/st_input.h)
            else if (!load_input_script(v, o)) return false;
        }
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
        else if (a == "--print-settings") o.print_settings = true;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    // The Faithful switch from the command line wins; any modern picture option asks for the custom picture.
    if (o.faithful_forced) o.present.faithful = true;
    else if (o.custom_given) o.present.faithful = false;
    return true;
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
    o.game = s.game;
    o.music = s.music;
    o.music_volume = s.music_volume;
    o.sfx_volume = s.sfx_volume;
    o.speech_volume = s.speech_volume;
    o.audio_upgrade = s.audio_upgrade;
    o.packs_enabled = x2::pack::split_list(s.packs_enabled);
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

void settings_from_options(const Options& o, x2::Settings& s) {
    s.scale = std::clamp(o.scale, 1, 8);
    s.fullscreen = o.fullscreen;
    s.aspect = o.present.aspect;
    s.faithful = o.present.faithful;
    s.interp = o.present.interp;
    s.interp_scale = std::clamp(o.present.scale, 1, 8);
    s.smooth_far = o.present.smooth;
    s.video_filter = o.present.filter;
    s.integer = o.present.integer;
    s.curvature = o.present.curvature;
    s.upgrade = o.present.upgrade;
    s.hd_panel = o.present.hd_panel;
    s.wide = x2::modern::wide_name(o.present.wide);
    s.asset_packs.clear();
    for (size_t i = 0; i < o.asset_packs.size(); ++i) s.asset_packs += (i ? ";" : "") + o.asset_packs[i];
    s.audio = !o.no_audio;
    s.filter = o.audio.filter == x2::AudioFilterModel::A1200 ? x2::FilterSetting::A1200
               : o.audio.filter == x2::AudioFilterModel::None ? x2::FilterSetting::None
                                                              : x2::FilterSetting::A500;
    s.stereo_separation = std::clamp(o.audio.stereo_separation, 0.0f, 1.0f);
    s.volume = std::clamp(o.volume, 0, 100);
    s.audio_latency = std::clamp(o.audio_latency, 10, 500);
    s.high_scores = !o.no_persist;
    s.game = o.game;
    s.music = o.music;
    s.music_volume = std::clamp(o.music_volume, 0, 100);
    s.sfx_volume = std::clamp(o.sfx_volume, 0, 100);
    s.speech_volume = std::clamp(o.speech_volume, 0, 100);
    s.audio_upgrade = o.audio_upgrade;
    s.packs_enabled.clear();
    for (size_t i = 0; i < o.packs_enabled.size(); ++i) s.packs_enabled += (i ? ";" : "") + o.packs_enabled[i];
    s.protect_music = o.protect_music;
    s.cd_crossfade = std::clamp(o.cd_crossfade, 0, 500);
    s.cd_pause = o.cd_pause;
    s.cdtv = o.cdtv_path;
    s.cdtv_tracks = o.cdtv_tracks;
    s.grab_keyboard = o.grab_keyboard;
    s.cursor = o.cursor == "on" || o.cursor == "yes" ? "show" : o.cursor == "off" || o.cursor == "no" ? "hide" : o.cursor;
    s.input_debug = o.input_debug;
    s.autofire = o.autofire;
    s.autofire_rate = std::clamp(o.autofire_rate, 1, 25);
    s.god_mode = o.god_mode;
    s.credits = std::clamp(o.credits, 1, 9);
    s.cheat_scores = o.cheat_scores;
}

// Canonical text dump of every field of Options, for the parse-equivalence test (tests/cli/). One "name=value" per line.
std::string describe_options(const Options& o) {
    std::ostringstream s;
    auto num = [&](const char* n, auto v) { s << n << '=' << +v << '\n'; };
    auto str = [&](const char* n, const std::string& v) { s << n << "=[" << v << "]\n"; };
    auto strs = [&](const char* n, const std::vector<std::string>& v) {
        s << n << "=[";
        for (size_t i = 0; i < v.size(); ++i) s << (i ? "|" : "") << v[i];
        s << "]\n";
    };
    auto hexes = [&](const char* n, const std::vector<uint32_t>& v) {
        s << n << "=[";
        for (size_t i = 0; i < v.size(); ++i) s << (i ? "," : "") << std::hex << v[i] << std::dec;
        s << "]\n";
    };
    auto fstrs = [&](const char* n, const std::vector<std::pair<uint32_t, std::string>>& v) {
        s << n << "=[";
        for (size_t i = 0; i < v.size(); ++i) s << (i ? "|" : "") << v[i].first << ':' << v[i].second;
        s << "]\n";
    };
    num("headless", o.headless);
    num("frames", o.frames);
    num("frames_set", o.frames_set);
    num("report_set", o.report_set);
    str("dump_dir", o.dump_dir);
    str("assets", o.assets);
    str("game", o.game);
    num("game_explicit", o.game_explicit);
    str("data_dir", o.data_dir);
    str("st_disk", o.st_disk);
    hexes("st_watch", o.st_watch);
    str("st_input_script", o.st_input_script);
    num("for_st", o.for_st);
    num("st_fdc_speed", o.st_fdc_speed);
    num("st_trace_tail", o.st_trace_tail);
    num("st_watch_pc_addr", o.st_watch_pc_addr);
    num("st_watch_pc_len", o.st_watch_pc_len);
    num("st_io_summary", o.st_io_summary);
    num("st_fdc_log", o.st_fdc_log);
    s << "st_joy=[";
    for (size_t i = 0; i < o.st_joy.size(); ++i) s << (i ? "|" : "") << o.st_joy[i].first << ':' << +o.st_joy[i].second;
    s << "]\n";
    str("st_frame_log", o.st_frame_log);
    str("st_shots_dir", o.st_shots_dir);
    str("st_shots", o.st_shots);
    num("st_input_offset", o.st_input_offset);
    num("st_profile_from", o.st_profile_from);
    num("st_profile_to", o.st_profile_to);
    fstrs("st_ram_dumps", o.st_ram_dumps);
    num("st_state_hash", o.st_state_hash);
    num("chip_kb", o.chip_kb);
    strs("import_paths", o.import_paths);
    num("apply_protection_patch", o.apply_protection_patch);
    num("confirm_own_original", o.confirm_own_original);
    num("list_variants", o.list_variants);
    strs("diagnose_paths", o.diagnose_paths);
    num("report_every", o.report_every);
    num("ppm_every", o.ppm_every);
    num("reglog", o.reglog);
    s << "trace_frames=[";
    for (size_t i = 0; i < o.trace_frames.size(); ++i) s << (i ? "," : "") << o.trace_frames[i];
    s << "]\n";
    s << "input=[";
    for (size_t i = 0; i < o.input.size(); ++i)
        s << (i ? "|" : "") << o.input[i].frame << ':' << int(o.input[i].kind) << ':' << +o.input[i].code << ':' << o.input[i].hold << ':' << +o.input[i].dirs;
    s << "]\n";
    s << "pokes=[";
    for (size_t i = 0; i < o.pokes.size(); ++i)
        s << (i ? "|" : "") << o.pokes[i].from << ':' << o.pokes[i].to << ':' << o.pokes[i].addr << ':' << o.pokes[i].value << ':' << o.pokes[i].size
          << ':' << o.pokes[i].handler;
    s << "]\n";
    str("trace_out", o.trace_out);
    str("trace_label", o.trace_label);
    num("audio_hash", o.audio_hash);
    fstrs("screenshots", o.screenshots);
    fstrs("ram_dumps", o.ram_dumps);
    s << "hash_frames=[";
    for (size_t i = 0; i < o.hash_frames.size(); ++i) s << (i ? "," : "") << o.hash_frames[i];
    s << "]\n";
    num("hash_every", o.hash_every);
    num("interp_only", o.interp_only);
    num("recomp_stats", o.recomp_stats);
    str("recomp_profile", o.recomp_profile);
    num("scale", o.scale);
    num("fullscreen", o.fullscreen);
    num("no_audio", o.no_audio);
    num("volume", o.volume);
    num("audio_latency", o.audio_latency);
    str("persist_dir", o.persist_dir);
    num("no_persist", o.no_persist);
    str("audio_out", o.audio_out);
    str("audio_cues", o.audio_cues);
    num("audio.output_rate", o.audio.output_rate);
    {
        char b[32];
        std::snprintf(b, sizeof b, "%.6g", double(o.audio.stereo_separation));
        str("audio.stereo_separation", b);
        std::snprintf(b, sizeof b, "%.6g", double(o.audio.output_gain));
        str("audio.output_gain", b);
    }
    num("audio.filter", int(o.audio.filter));
    num("timing", int(o.timing));
    str("watch_log", o.watch_log);
    str("pc_trace", o.pc_trace);
    str("blit_log", o.blit_log);
    str("draw_log", o.draw_log);
    num("draw_log_from", o.draw_log_from);
    num("draw_log_to", o.draw_log_to);
    num("present.faithful", o.present.faithful);
    num("present.interp", o.present.interp);
    num("present.scale", o.present.scale);
    num("present.smooth", o.present.smooth);
    str("present.filter", x2::video_filter_name(o.present.filter));
    num("present.aspect", o.present.aspect);
    num("present.integer", o.present.integer);
    num("present.curvature", o.present.curvature);
    num("present.upgrade", o.present.upgrade);
    str("present.wide", x2::modern::wide_name(o.present.wide));
    num("present.hd_panel", o.present.hd_panel);
    num("faithful_forced", o.faithful_forced);
    num("custom_given", o.custom_given);
    num("modern_check", o.modern_check);
    num("modern_rate", o.modern_rate);
    str("modern_out", o.modern_out);
    str("modern_shots", o.modern_shots);
    num("modern_out_from", o.modern_out_from);
    num("modern_out_to", o.modern_out_to);
    num("modern_shots_at", o.modern_shots_at);
    fstrs("present_shots", o.present_shots);
    strs("asset_packs", o.asset_packs);
    str("dump_assets", o.dump_assets);
    strs("packs_enabled", o.packs_enabled);
    num("audio_upgrade", o.audio_upgrade);
    num("speech_volume", o.speech_volume);
    num("speech_volume_given", o.speech_volume_given);
    str("check_pack.path", o.check_pack.path);
    num("check_pack.game_set", o.check_pack.game.has_value());
    str("check_pack.build", o.check_pack.build);
    str("check_pack.against", o.check_pack.against);
    num("check_pack.full", o.check_pack.full);
    num("check_pack.strict", o.check_pack.strict);
    num("check_pack.json", o.check_pack.json);
    num("check_flag_given", o.check_flag_given);
    num("json_given", o.json_given);
    num("list_asset_packs", o.list_asset_packs);
    str("dump_audio", o.dump_audio);
    str("pc_log", o.pc_log);
    num("pc_log_from", o.pc_log_from);
    num("pc_log_to", o.pc_log_to);
    hexes("pc_log_pcs", o.pc_log_pcs);
    num("pc_trace_from", o.pc_trace_from);
    num("pc_trace_to", o.pc_trace_to);
    num("blit_log_from", o.blit_log_from);
    num("blit_log_to", o.blit_log_to);
    hexes("watch", o.watch);
    str("music", o.music);
    str("cd_pause", o.cd_pause);
    str("cdtv_path", o.cdtv_path);
    str("import_cdtv", o.import_cdtv);
    str("cdtv_log", o.cdtv_log);
    str("cdtv_tracks", o.cdtv_tracks);
    num("music_volume", o.music_volume);
    num("sfx_volume", o.sfx_volume);
    num("cd_crossfade", o.cd_crossfade);
    num("protect_music", o.protect_music);
    str("grab_keyboard", o.grab_keyboard);
    str("cursor", o.cursor);
    num("input_debug", o.input_debug);
    num("autofire", o.autofire);
    num("god_mode", o.god_mode);
    num("cheat_scores", o.cheat_scores);
    num("autofire_rate", o.autofire_rate);
    num("credits", o.credits);
    num("help", o.help);
    num("version", o.version);
    num("print_data_dir", o.print_data_dir);
    num("list_keys", o.list_keys);
    num("print_settings", o.print_settings);
    return s.str();
}

} // namespace x2::cli
