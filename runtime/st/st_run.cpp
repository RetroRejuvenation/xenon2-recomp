// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/st_run.h"

#include "audio/wav_writer.h"
#include "debug_dump.h"
#include "gfx/image_io.h"
#include "import/importer.h"
#include "modern/indexed.h"
#include "modern/layout.h"
#include "modern/screen_dump.h"
#include "pack/pack_stack.h"
#include "modern/screens.h"
#include "pack/pack_api.h"
#include "st/st_game.h"
#include "st/st_input.h"
#include "st/st_machine.h"
#include "trace/tracer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

#ifdef X2_HAVE_SDL
#include "audio/sdl_audio.h"
#include "gfx/sdl_display.h"
#include "input/input.h"
#include "ui/live_apply.h"
#include "ui/menu_script.h"
#include "ui/overlay.h"
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

// Big-endian RAM access without side effects.
uint32_t ram_get(const StMachine& m, uint32_t a, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; ++i) v = (v << 8) | (a + uint32_t(i) < m.ram_size() ? m.ram()[a + uint32_t(i)] : 0);
    return v;
}
void ram_put(StMachine& m, uint32_t a, uint32_t v, int size) {
    for (int i = 0; i < size; ++i)
        if (a + uint32_t(i) < m.ram_size()) m.ram()[a + uint32_t(i)] = uint8_t(v >> (8 * (size - 1 - i)));
}
} // namespace

int apply_poke(StMachine& m, const RunOptions::Poke& p, uint32_t objlist_head) {
    if (!p.handler) {
        ram_put(m, p.addr, p.value, p.size);
        return 1;
    }
    // The hittable-object list as the collision loop walks it: head pointer at objlist_head, next at +$12, a type word
    // (+0) of 0 ends it, hit handler at +$A. Bounded so a corrupt list cannot hang it.
    int n = 0;
    uint32_t node = ram_get(m, objlist_head, 4);
    for (int guard = 0; node && node + 0x20 < m.ram_size() && guard < kObjWalkLimit; ++guard) {
        if (ram_get(m, node, 2) == 0) break;
        if (ram_get(m, node + 0xA, 4) == p.handler) {
            ram_put(m, node + p.addr, p.value, p.size);
            ++n;
        }
        node = ram_get(m, node + 0x12, 4);
    }
    return n;
}

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

// One boot of the machine and its frame loop. `reset_requested` is set when the host asked for a reset (F12): the caller starts
// another session from the same disk image, with the same StGame (a cheat stays flagged for the process).
static int run_session(const RunOptions& opt, StDisk disk, const std::string& disk_name, const StScript& script, StGame& game,
                       bool& reset_requested) {
    std::string why;
    ScriptPlayer player(script);
    const bool want_audio = opt.audio_hash || !opt.audio_out.empty() || (!opt.headless && !opt.no_audio);
    StConfig cfg;
    cfg.fdc_speed = opt.fdc_speed;
    cfg.audio_rate = want_audio ? opt.audio_rate : 0;
    StMachine m(cfg);
    m.insert_disk(std::move(disk));
    if (opt.io_summary) m.enable_io_summary();
    if (opt.fdc_log) m.fdc().log_reads(true);
    if (!opt.pokes.empty() || !script.pokes.empty())
        std::printf("coverage pokes active: %zu (state is altered; not a faithful run)\n", opt.pokes.size() + script.pokes.size());
    std::vector<uint32_t> tail(opt.trace_tail ? opt.trace_tail : 1, 0);
    uint64_t tail_n = 0;
    std::map<uint32_t, uint64_t> hist;
    std::vector<uint8_t> watch_prev;
    uint32_t watch_last_pc = 0;
    if (opt.watch_pc_len && opt.watch_pc_addr + opt.watch_pc_len <= m.ram_size()) watch_prev.assign(opt.watch_pc_len, 0);
    if (opt.trace_tail || opt.profile_to || !watch_prev.empty())
        m.set_instruction_probe([&](StMachine& mm, uint32_t pc) {
            if (opt.trace_tail) tail[tail_n++ % tail.size()] = pc;
            if (opt.profile_to && mm.frame() + 1 >= opt.profile_from && mm.frame() + 1 < opt.profile_to) ++hist[pc];
            if (!watch_prev.empty()) {
                if (std::memcmp(watch_prev.data(), mm.ram() + opt.watch_pc_addr, watch_prev.size()) != 0) {
                    std::printf("watch: frame %u the instruction at $%06X changed $%06X..$%06X\n", mm.frame(), watch_last_pc, opt.watch_pc_addr,
                                opt.watch_pc_addr + opt.watch_pc_len - 1);
                    std::memcpy(watch_prev.data(), mm.ram() + opt.watch_pc_addr, watch_prev.size());
                }
                watch_last_pc = pc;
            }
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
    if (opt.attach_cpu && !opt.attach_cpu(m, why)) {
        std::fprintf(stderr, "x2recomp: recompiled code disabled: %s\n", why.c_str());
        return 5;
    }
    std::unique_ptr<x2::Tracer> tracer;
    if (!opt.trace_out.empty()) {
        // Interrupt entries: the autovectors (levels 1-6) and the 16 MFP vectors, which the ST raises at level 6.
        x2::Tracer::Env env;
        env.mem = m.ram();
        env.size = m.ram_size();
        env.frame = m.frame_ptr();
        env.sp = [&m] { return m.sp(); };
        env.sr = [&m] { return m.sr(); };
        for (int lvl = 1; lvl <= 6; ++lvl) env.irq_vectors.push_back({lvl, 0x60u + 4u * uint32_t(lvl)});
        for (uint32_t v = 0; v < 16; ++v) env.irq_vectors.push_back({6, 0x100u + 4u * v});
        tracer = std::make_unique<x2::Tracer>(std::move(env), x2::Tracer::Layout::atari_st());
        m.set_tracer(tracer.get());
    }
    if (!m.boot(why)) {
        std::fprintf(stderr, "x2recomp: %s: %s\n", disk_name.c_str(), why.c_str());
        return 5;
    }
    game.install(m);
    // Screens and the status panel from asset packs, and --dump-assets (modern/screens.h, modern/screen_dump.h).
    std::shared_ptr<x2::pack::PackStack> own_packs;
    if (opt.upgrade) {
        x2::pack::PackContext ctx;
        ctx.game = x2::pack::GameFamily::St;
        ctx.build = "atari-st-kixx";
        x2::pack::PackReport report;
        own_packs = x2::pack::load_pack_stack(x2::pack::pack_settings(), ctx, report);
        for (const auto& msg : report.messages)
            if (msg.severity != x2::pack::Severity::Info) std::printf("%s\n", msg.format().c_str());
        if (own_packs && own_packs->has_graphics()) {
            x2::pack::set_packs(own_packs);
            std::printf("asset packs: %zu screens%s from the pack%s\n", own_packs->screens().size(), own_packs->panel() ? " and a status panel" : "",
                        own_packs->packs().size() > 1 ? "s" : "");
        }
    }
    bool in_game = false;    // ships > 0 as of the last frame
    x2::modern::PackScreens pack_screens;
    if (opt.upgrade) pack_screens.set_library(&x2::pack::packs(), x2::modern::kStLayout.panel);
    const bool use_packs = pack_screens.active();
    const int pack_scale = std::clamp(opt.canvas_scale, 1, 8);
    std::unique_ptr<x2::modern::ScreenDumper> screen_dumper;
    if (!opt.dump_assets.empty()) {
        std::error_code dec;
        fs::create_directories(opt.dump_assets, dec);
        if (dec) {
            std::fprintf(stderr, "x2recomp: cannot create --dump-assets '%s': %s\n", opt.dump_assets.c_str(), dec.message().c_str());
            return 2;
        }
        screen_dumper = std::make_unique<x2::modern::ScreenDumper>(x2::modern::kStLayout.panel);
    }
    m.set_index_capture(use_packs || screen_dumper);
    x2::modern::ScreensCheck screens_check;
    std::vector<uint32_t> pack_canvas, replicated;
    auto st_indexed = [&]() {
        x2::modern::IndexedFrame f;
        f.w = kFbWidth;
        f.h = kFbHeight;
        f.colours = 16;
        f.index = m.index_plane().data();
        f.stride = kFbWidth;
        f.pal = m.palette_plane().data();
        f.rgba = reinterpret_cast<const uint32_t*>(m.picture().data());
        f.rgba_stride = kFbWidth;
        return f;
    };
    // The picture presented for the frame just run: the packs' canvas (the window `pack_scale` times enlarged), or, when no pack is
    // in force, the frame as it is. Returns the scale of what `out` holds.
    auto presented = [&](uint32_t f, const std::vector<uint32_t>*& out) {
        if (use_packs) {
            pack_screens.render_window(st_indexed(), f + 1, in_game, pack_scale, pack_canvas);
            out = &pack_canvas;
            return pack_scale;
        }
        replicated.resize(size_t(kFbWidth) * kFbHeight * size_t(pack_scale) * size_t(pack_scale));
        const uint32_t* px = reinterpret_cast<const uint32_t*>(m.picture().data());
        for (int y = 0; y < kFbHeight * pack_scale; ++y)
            for (int x = 0; x < kFbWidth * pack_scale; ++x)
                replicated[size_t(y) * size_t(kFbWidth * pack_scale) + size_t(x)] = px[size_t(y / pack_scale) * kFbWidth + size_t(x / pack_scale)];
        out = &replicated;
        return pack_scale;
    };
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
        for (const auto& p : opt.pokes)
            if (f >= p.from && f <= p.to) apply_poke(m, p);
        for (const ScriptPoke& p : script.pokes)
            if (f >= p.from && f <= p.to) apply_poke(m, {p.from, p.to, p.addr, p.value, p.size, p.handler});
        // The assists at the same frame boundary as the pokes (docs/re-notes/atari-st-hooks.md): the saved table, god mode.
        in_game = game.begin_frame(m);
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
        if (screen_dumper) screen_dumper->frame(st_indexed(), f, in_game, false);
        if (opt.pack_check && use_packs) {
            const std::vector<uint32_t>* c = nullptr;
            const int S = presented(f, c);
            screens_check.add(pack_screens, st_indexed(), c->data(), size_t(kFbWidth) * size_t(S), S, 0, 0, f);
        }
        for (const auto& [pf, path] : opt.present_shots)
            if (pf == f) {
                const std::vector<uint32_t>* c = nullptr;
                const int S = presented(f, c);
                std::vector<uint8_t> bytes(c->size() * 4);
                std::memcpy(bytes.data(), c->data(), bytes.size());
                if (write_png(path, bytes, kFbWidth * S, kFbHeight * S)) std::printf("present shot of frame %u -> %s\n", f, path.c_str());
                else std::fprintf(stderr, "x2recomp: cannot write %s\n", path.c_str());
            }
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
            std::printf("frame %u pc=$%06X vbl=%" PRIu64 " mfp=%" PRIu64 " fdc-cmds=%" PRIu64 " sectors=%zu unmapped=%" PRIu64
                        " level=%u energy=%u ships=%u credits=%u score=%u joy=%02x/%02x ikbd=%llu/%llu\n",
                        f, m.pc(), m.vbl_taken(), m.mfp_taken(), m.fdc().commands_issued(), m.fdc().read_log().size(), m.unmapped_accesses(),
                        ram_get(m, 0xE04, 2), ram_get(m, 0xCC6, 2), ram_get(m, 0xCCA, 2), ram_get(m, 0xCF0, 2), ram_get(m, 0xC92, 4),
                        ram_get(m, 0xA00, 1), ram_get(m, 0xA01, 1), (unsigned long long)m.ikbd().bytes_sent(),
                        (unsigned long long)m.ikbd().bytes_received_by_cpu());
        const bool hash_now = (opt.hash_every && (f % opt.hash_every) == 0) ||
                              std::find(opt.hash_frames.begin(), opt.hash_frames.end(), f) != opt.hash_frames.end();
        if (hash_now) {
            std::printf("framehash %u %016" PRIx64 "\n", f, m.picture_hash());
            if (opt.audio_hash) std::printf("audiohash %u %016" PRIx64 "\n", f, audio_hash);
            if (opt.state_hash) std::printf("statehash %u %016" PRIx64 "\n", f, m.state_digest());
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
        const std::string title_base = "Xenon 2 Megablast (Atari ST)";
        if (!display.open(title_base, kFbWidth, kFbHeight, opt.scale, opt.fullscreen, opt.aspect, derr)) {
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
        // Autofire, god mode and extra credits are the shared host commands (input.h) with the shared autofire policy.
        x2::input::LoadedBindings bind;
        if (!opt.data_dir.empty()) bind = x2::input::load_bindings(opt.data_dir);
        else bind.bindings = x2::input::default_bindings();
        if (!bind.error.empty()) std::fprintf(stderr, "x2recomp: %s: %s", bind.path.c_str(), bind.error.c_str());
        if (!bind.path.empty()) std::printf("input bindings: %s%s%s\n", bind.path.c_str(), bind.note.empty() ? "" : ": ", bind.note.c_str());
        x2::input::InputManager input(bind.bindings);
        input.set_debug(opt.input_debug);
        for (int port : {1, 2}) {
            input.autofire(port).set_enabled(opt.autofire);
            input.autofire(port).set_rate(opt.autofire_rate);
        }
        SdlAudioOut audio_out;
        int volume = opt.volume;
        if (want_audio) {
            std::string aerr;
            if (audio_out.open(int(opt.audio_rate), opt.audio_latency, aerr)) audio_out.set_volume(float(volume) / 100.0f);
            else std::fprintf(stderr, "x2recomp: no sound: %s\n", aerr.c_str());
        }
        auto retitle = [&]() {
            x2::input::WindowModes modes;
            modes.autofire = input.autofire(2).enabled();
            modes.god_mode = game.god_mode();
            modes.cheat_used = game.cheated();
            display.set_title(title_base + x2::input::window_mode_suffix(modes));
        };
        retitle();
        bool quit = false, paused = false, menu_open = false;
        // The in-game menu (ui/overlay.h): F1 or the gamepad's menu button. It freezes the game like the P pause and changes what the
        // Atari ST release takes live (window size, fullscreen, volume, filter, aspect, whole pixels, curvature, autofire, god mode);
        // everything else it saves for the next start.
        Settings live = opt.settings, file = opt.file_settings;
        std::unique_ptr<ui::Overlay> overlay;
        {
            ui::OverlayHooks hk;
            hk.current = [&]() {
                live.fullscreen = display.fullscreen();
                live.autofire = input.autofire(2).enabled();
                live.autofire_rate = input.autofire(2).rate();
                live.god_mode = game.god_mode();
                return live;
            };
            hk.apply = [&](const Settings& before, const Settings& after) {
                ui::LiveHooks lh;
                lh.scale = [&](int v) { display.set_scale(v); };
                lh.fullscreen = [&](bool on) { if (display.fullscreen() != on) display.toggle_fullscreen(); };
                lh.volume = [&](int v) {
                    volume = v;
                    audio_out.set_volume(float(volume) / 100.0f);
                };
                lh.picture = [&](const Settings& a) {
                    view.filter = a.video_filter;
                    view.aspect = a.aspect;
                    view.integer = a.integer;
                    view.curvature = a.curvature;
                    display.set_aspect(view.aspect);
                    display.set_scale(a.scale);
                };
                lh.picture_keys = {"filter", "aspect", "integer", "curvature"};
                lh.autofire = [&](bool on, int rate) {
                    for (int port : {1, 2}) {
                        input.autofire(port).set_enabled(on);
                        input.autofire(port).set_rate(rate);
                    }
                    retitle();
                    std::printf("autofire: %s%s\n", on ? "on" : "off", on ? " (while fire is held, during a level only)" : "");
                };
                lh.god_mode = [&](bool on) {
                    if (game.god_mode() != on) game.command(StHostCommand::GodMode, m);
                    retitle();
                };
                const ui::LiveResult r = ui::apply_live(before, after, ui::GameKind::St, lh);
                for (const std::string& key : r.applied) // what is in force now; what waits for the next start is only saved
                    if (const ui::SettingRow* row = ui::find_row(key)) {
                        std::string err;
                        ui::set_value_text(*row, live, ui::value_text(*row, after), err);
                    }
                ui::merge_changed(before, after, file);
                if (!opt.settings_path.empty()) {
                    if (std::string why; !save_settings_file(opt.settings_path, file, why)) std::fprintf(stderr, "x2recomp: %s\n", why.c_str());
                }
                return r;
            };
            hk.save_bindings = [&](const x2::input::Bindings& b, std::string& why) {
                input.set_bindings(b);
                return x2::input::save_bindings_file(opt.data_dir, b, why);
            };
            hk.reset_game = [&]() { reset_requested = true; }; // boots the disk again
            hk.quit = [&]() { quit = true; };
            hk.set_open = [&](bool open) {
                menu_open = open;
                std::printf("menu: %s\n", open ? "open (the game is paused)" : "closed");
                input.release_all(open ? "menu opened" : "menu closed");
                audio_out.set_paused(paused || menu_open);
            };
            overlay = std::make_unique<ui::Overlay>(ui::GameKind::St, std::move(hk));
            display.set_overlay([&overlay](SDL_Renderer* r, int w, int h) { overlay->draw(r, w, h); });
        }
        uint64_t menu_last_ms = SDL_GetTicks();
        ui::MenuScript menu_script;
        if (std::string why; !menu_script.parse(opt.menu_script, why)) {
            std::fprintf(stderr, "x2recomp: --menu-script: %s\n", why.c_str());
            SDL_Quit();
            return 2;
        }
        for (uint32_t f = 0; !quit && !reset_requested && (total == 0 || f < total); ++f) {
            SDL_Event ev;
            menu_script.pump(f); // --menu-script: the next key, down and up
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) quit = true;
                x2::input::HostCommand cmd = x2::input::HostCommand::None;
                if (overlay->active()) {
                    // The menu has the keyboard and the pointer; the input layer still sees what concerns it alone.
                    switch (ev.type) {
                    case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    case SDL_EVENT_WINDOW_FOCUS_LOST:
                    case SDL_EVENT_GAMEPAD_ADDED:
                    case SDL_EVENT_GAMEPAD_REMOVED: cmd = input.handle_event(ev); break;
                    default: break;
                    }
                    overlay->handle(ev);
                } else {
                    cmd = input.handle_event(ev);
                }
                switch (cmd) {
                case x2::input::HostCommand::Quit: quit = true; break;
                case x2::input::HostCommand::Menu: {
                    int ow = 0, oh = 0;
                    SDL_GetRenderOutputSize(display.renderer(), &ow, &oh);
                    menu_last_ms = SDL_GetTicks();
                    overlay->open(ow, oh, input.bindings());
                    break;
                }
                case x2::input::HostCommand::Fullscreen: display.toggle_fullscreen(); break;
                case x2::input::HostCommand::Reset: reset_requested = true; break; // boots the disk again
                case x2::input::HostCommand::Pause:
                    paused = !paused;
                    audio_out.set_paused(paused || menu_open);
                    break;
                case x2::input::HostCommand::VolumeDown:
                case x2::input::HostCommand::VolumeUp:
                    volume = std::clamp((volume + (cmd == x2::input::HostCommand::VolumeUp ? 10 : -10)) / 10 * 10, 0, 100);
                    audio_out.set_volume(float(volume) / 100.0f);
                    std::printf("volume %d%%\n", volume);
                    break;
                case x2::input::HostCommand::Autofire: {
                    const bool on = !input.autofire(2).enabled();
                    for (int port : {1, 2}) input.autofire(port).set_enabled(on);
                    retitle();
                    std::printf("autofire: %s%s\n", on ? "on" : "off", on ? " (while fire is held, during a level only)" : "");
                    break;
                }
                case x2::input::HostCommand::GodMode: game.command(StHostCommand::GodMode, m); retitle(); break;
                case x2::input::HostCommand::AddCredit: game.command(StHostCommand::AddCredit, m); retitle(); break;
                default: break;
                }
            }
            if (quit) break;
            if (overlay->active()) {
                const uint64_t ms = SDL_GetTicks();
                overlay->tick(int(ms - menu_last_ms));
                menu_last_ms = ms;
            }
            if (paused || menu_open) { // the frame is not run; keep the window alive (and the menu drawn)
                display.redraw(view);
                --f;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                next = std::chrono::steady_clock::now();
                continue;
            }
            for (int port : {1, 2}) input.autofire(port).set_in_game(in_game);
            const x2::input::FrameInput in = input.collect(f);
            m.set_joystick(1, joy_bits(in.p2.up, in.p2.down, in.p2.left, in.p2.right, in.p2.fire));
            m.set_joystick(0, joy_bits(in.p1.up, in.p1.down, in.p1.left, in.p1.right, in.p1.fire));
            for (const auto& k : in.keys)
                if (const uint8_t sc = st_scancode_for_amiga_raw(k.first)) m.key_event(sc, k.second);
            apply_script(f);
            m.run_frame();
            const std::vector<int16_t>* snd = want_audio ? &frame_audio() : nullptr;
            if (snd && audio_out.is_open()) audio_out.push(snd->data(), snd->size() / 2);
            if (use_packs) {
                const std::vector<uint32_t>* c = nullptr;
                const int S = presented(f, c);
                display.present(reinterpret_cast<const uint8_t*>(c->data()), kFbWidth * S, kFbHeight * S, S, view);
            } else {
                display.present(m.picture().data(), kFbWidth, kFbHeight, 1, view);
            }
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
    if (opt.fdc_log) {
        // The sector reads as runs: track/side, first and last sector, where the DMA put the first one (and whether it stored).
        const auto& log = m.fdc().read_log();
        for (size_t i = 0; i < log.size();) {
            size_t j = i;
            while (j + 1 < log.size() && log[j + 1].track == log[i].track && log[j + 1].side == log[i].side && log[j + 1].sector == log[j].sector + 1) ++j;
            std::printf("fdc read: frame %llu track %u side %u sectors %u-%u -> $%06X%s\n", (unsigned long long)(log[i].cycle / kCyclesPerFrame), log[i].track,
                        log[i].side, log[i].sector, log[j].sector, log[i].addr, log[i].stored ? "" : " (not stored)");
            i = j + 1;
        }
    }
    if (opt.io_summary) print_io_summary(m);
    if (opt.after_run && !opt.after_run(m)) rc = rc ? rc : 3;
    if (tracer) {
        m.set_tracer(nullptr);
        std::error_code ec;
        const fs::path tp(opt.trace_out);
        if (tp.has_parent_path()) fs::create_directories(tp.parent_path(), ec);
        const std::string label = opt.trace_label.empty() ? tp.stem().string() : opt.trace_label;
        if (!tracer->write_json(opt.trace_out, label, m.frame())) {
            std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.trace_out.c_str());
            rc = rc ? rc : 3;
        } else {
            std::printf("trace: %s\n", opt.trace_out.c_str());
        }
    }
    std::printf("bus rounding: %" PRIu64 " instructions rounded up, %" PRIu64 " cycles added\n", m.rounded_instructions(), m.rounding_cycles());
    std::printf("device wait states: %" PRIu64 " cycles\n", m.io_wait_cycles());
    if (!opt.dump_dir.empty()) write_png(opt.dump_dir + "/final.png", m.picture(), kFbWidth, kFbHeight);
    if (opt.pack_check && screens_check.frames) std::printf("%s\n", screens_check.summary(pack_screens).c_str());
    if (screen_dumper) {
        std::string members, dwhy;
        if (!screen_dumper->finish(opt.dump_assets, members, dwhy)) {
            std::fprintf(stderr, "x2recomp: --dump-assets: %s\n", dwhy.c_str());
            rc = rc ? rc : 3;
        } else {
            const std::string j = std::string("{\n  \"format\": \"x2-asset-pack\",\n  \"version\": 2,\n  \"name\": \"dump\",\n  \"games\": [\"st\"],\n"
                                              "  \"note\": \"Original screens and status panel of your own copy of the game, written by x2recomp --dump-assets. "
                                              "User data: do not distribute.\",\n  \"scale\": 1") +
                                  (members.empty() ? "" : ",\n  " + members) + "\n}\n";
            std::ofstream mf(opt.dump_assets + "/manifest.json", std::ios::binary);
            if (!mf || !mf.write(j.data(), std::streamsize(j.size()))) {
                std::fprintf(stderr, "x2recomp: cannot write %s/manifest.json\n", opt.dump_assets.c_str());
                rc = rc ? rc : 3;
            }
            std::printf("dump-assets: %zu screens (%zu transitions skipped, %zu not matchable), %zu panel cells (the strip had %zu palettes) -> %s/manifest.json\n",
                        screen_dumper->screens(), screen_dumper->transitions(), screen_dumper->not_matchable(), screen_dumper->panel_cells(),
                        screen_dumper->strip_palettes(), opt.dump_assets.c_str());
            for (const std::string& w : screen_dumper->warnings()) std::printf("dump-assets: warning: %s\n", w.c_str());
        }
    }
    if (game.god_saves()) std::printf("god mode: %" PRIu64 " deaths undone by the hook\n", game.god_saves());
    game.save_scores(m); // a name entry the game had not finished returning from is saved too
    return rc;
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
    StGameConfig gc;
    gc.god_mode = opt.god_mode;
    gc.cheat_scores = opt.cheat_scores;
    gc.credits = opt.credits;
    gc.persist_dir = opt.persist_dir;
    gc.image_hash = fnv1a64(disk.data.data(), disk.data.size());
    gc.god_hotkey = !opt.headless; // a headless run without --god-mode is left completely alone (no hook, no moved hash)
    StGame game(gc);
    if (opt.god_mode) std::printf("god mode: on (CHEAT: the ship's energy never runs out)\n");
    if (opt.autofire && !opt.headless) std::printf("autofire: on, %d pulses per second, during a level only\n", opt.autofire_rate);
    if (opt.credits != kGameCredits) std::printf("credits: a new game will start with %d%s\n", opt.credits, opt.credits > kGameCredits ? " (CHEAT)" : "");
    int rc = 0;
    for (;;) {
        bool reset = false;
        rc = run_session(opt, disk, disk_name, script, game, reset);
        if (!reset) break;
        std::printf("reset: booting the disk again\n");
    }
    return rc;
}

} // namespace x2::st
