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
#include "audio/audio_dump.h"
#include "audio/pack_audio.h"
#include "audio/wav_writer.h"
#include "boot.h"
#include "cli_options.h"
#include "retail_protection.h"
#include "cdtv/cdtv_disc.h"
#include "cdtv/cdtv_game.h"
#include "cdtv/cdtv_music.h"
#include "debug_dump.h"
#include "disk_shim.h"
#include "gfx/image_io.h"
#include "gfx/pacer.h"
#include "gfx/video.h"
#include "import/cdtv_game.h"
#include "import/st_game.h"
#include "import/importer.h"
#include "import/sha1.h"
#include "machine.h"
#include "modern/assets.h"
#include "modern/draw_capture.h"
#include "modern/interp.h"
#include "modern/indexed.h"
#include "modern/present.h"
#include "modern/screen_dump.h"
#include "pack/pack_api.h"
#include "pack/pack_stack.h"
#include "persist.h"
#include "st/st_machine.h"
#include "st/st_run.h"
#include "trace/tracer.h"
#ifdef X2_HAVE_RECOMP
#include "recomp/recomp.h"
#endif
#ifdef X2_HAVE_SDL
#include "gfx/sdl_display.h"
#include "audio/sdl_audio.h"
#include "input/input.h"
#include "ui/launcher.h"
#include "ui/library.h"
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

using x2::cli::InputEvent;
using x2::cli::Options;
using x2::cli::apply_settings;
using x2::cli::parse_args;
using x2::cli::usage;
using x2::Poke; // coverage-only state pokes (cli_options.h)

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

// What the run decided about asset packs (pack/pack_api.h), from the settings and the command line.
x2::pack::AssetPackSettings pack_settings_from(const Options& o) {
    x2::pack::AssetPackSettings ps;
    ps.pack_paths = o.asset_packs;
    ps.packs_enabled = o.packs_enabled;
    ps.packs_dir = x2::pack::default_packs_dir(user_data_dir(o));
    ps.upgrade = o.present.upgrade;
    ps.audio_upgrade = o.audio_upgrade;
    ps.faithful = o.present.faithful || o.faithful_forced;
    ps.speech_volume = o.speech_volume;
    return ps;
}

} // namespace

// First line of the display window (DIWSTRT $3A) in the framebuffer.
constexpr int kTop = 0x3A - x2::Video::kFbTop;

// Time spent in Presenter::at for the headless picture output (hashing and writing the pictures excluded), for the cost gates.
static uint64_t g_present_ns = 0, g_present_pictures = 0;

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
            if (!same) ck.last_mismatch = f;
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
            const auto present_t0 = std::chrono::steady_clock::now();
            Canvas c = pr.at(tau, video.front());
            g_present_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - present_t0).count());
            ++g_present_pictures;
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
    x2::pack::set_audio_codec(&x2::audio::pack_codec()); // pack sound files are read by the decoders (before any pack is loaded or checked)
    Options opt;
    if (!parse_args(argc, argv, opt)) { usage(); return 2; }
    if (opt.help) { usage(stdout); return 0; }
    if (opt.version) { std::printf("x2recomp %s\n", X2_VERSION); return 0; }
    if (opt.print_settings) { // what a window run with these arguments would use: defaults < settings.cfg < command line
        x2::Settings eff;
        const std::string dir = user_data_dir(opt);
        std::string text, serr;
        if (!dir.empty() && x2::load_settings_file(dir + "settings.cfg", eff, text, serr) && !serr.empty())
            std::fprintf(stderr, "x2recomp: %ssettings.cfg:\n%s", dir.c_str(), serr.c_str());
        Options merged;
        x2::cli::apply_settings(eff, merged);
        if (!parse_args(argc, argv, merged)) { usage(); return 2; }
        x2::cli::settings_from_options(merged, eff);
        std::fputs(x2::format_settings(eff).c_str(), stdout);
        return 0;
    }
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
        if (opt.game == "st")
            std::printf("\nAtari ST (--game st): the game reads one joystick, IKBD port 1, from the primary bindings (the\n"
                        "Amiga port 2 set: p2_up .. p2_fire); the second joystick set drives port 0, which the game ignores.\n"
                        "Game keys go to the ST as scancodes: P pauses, Esc aborts the game, Space does a special action in the\n"
                        "game (not yet confirmed); button2 and the remote do not exist on the ST. Host keys are shared, including the assists:\n"
                        "autofire, god mode (a cheat: no high scores saved) and the extra credit key act as in the other builds.\n");
        return 0;
#endif
    }
    if ((opt.apply_protection_patch || opt.confirm_own_original) && opt.import_paths.empty()) {
        std::fprintf(stderr, "x2recomp: --apply-protection-patch and --i-confirm-i-own-the-original only apply to --import\n");
        return 2;
    }
    if (opt.check_flag_given && opt.check_pack.path.empty()) {
        std::fprintf(stderr, "x2recomp: --against, --build, --full and --strict only apply to --check-asset-pack\n");
        return 2;
    }
    if (opt.json_given && opt.check_pack.path.empty() && !opt.list_asset_packs) {
        std::fprintf(stderr, "x2recomp: --json only applies to --check-asset-pack and --list-asset-packs\n");
        return 2;
    }
    if (!opt.check_pack.path.empty()) {
        if (opt.game_explicit && opt.game != "auto") {
            x2::pack::GameFamily g;
            if (x2::pack::parse_game(opt.game, g)) opt.check_pack.game = g;
        }
        return x2::pack::check_asset_pack_cli(opt.check_pack);
    }
    if (opt.list_variants) return x2::import::list_variants_cli();
    if (!opt.diagnose_paths.empty()) return x2::import::diagnose_cli(opt.diagnose_paths);
    if (opt.print_data_dir) { // for scripts (tools/build_all.sh, .ps1): where --import puts the assets
        const std::string d = user_data_dir(opt);
        if (d.empty()) { std::fprintf(stderr, "x2recomp: no per-user data directory\n"); return 1; }
        std::printf("%s\n", d.c_str());
        return 0;
    }
    // CDTV disc image: --import-cdtv PATH imports the CD audio only. --import with a single CD image (a .cue, a folder or ZIP
    // holding one, or the bare data track as .bin/.iso) imports the game (pre-linked into --assets DIR, default
    // <data dir>/assets-cdtv) and, when the input has the audio tracks, the CD audio too (--cdtv DIR, default <data dir>/cdtv).
    if (!opt.import_cdtv.empty() || (opt.import_paths.size() == 1 && x2::import::looks_like_cdtv_game_image(opt.import_paths[0]))) {
        std::string dir = opt.cdtv_path;
        std::string data;
        if (dir.empty() || (opt.import_cdtv.empty() && opt.assets.empty())) {
            data = user_data_dir(opt);
            if (data.empty()) {
                std::fprintf(stderr, "x2recomp: no per-user data directory; use --data-dir DIR or --cdtv DIR%s\n",
                             opt.import_cdtv.empty() ? " and --assets DIR" : "");
                return 1;
            }
            if (dir.empty()) dir = data + "cdtv";
        }
        if (!opt.import_cdtv.empty()) return x2::cdtv::import_cli(opt.import_cdtv, dir);
        return x2::import::import_cdtv_cli(opt.import_paths[0], opt.assets.empty() ? data + "assets-cdtv" : opt.assets, dir);
    }
    // Atari ST disk images (.st, .msa, .stx, also in a ZIP or folder): validated and normalised into --assets DIR, default
    // <data dir>/assets-st (import/st_game.h). The ST machine itself is a later phase.
    if (!opt.import_paths.empty() && x2::import::looks_like_st_image(opt.import_paths)) {
        std::string data = opt.assets.empty() ? user_data_dir(opt) : "";
        if (opt.assets.empty() && data.empty()) {
            std::fprintf(stderr, "x2recomp: no per-user data directory; use --data-dir DIR or --assets DIR\n");
            return 1;
        }
        return x2::import::import_st_cli(opt.import_paths, opt.assets.empty() ? data + "assets-st" : opt.assets);
    }
    if (!opt.import_paths.empty()) { // --import: into --assets DIR if given, else <data dir>/assets; then exit
        std::string data = opt.assets.empty() ? user_data_dir(opt) : "";
        if (opt.assets.empty() && data.empty()) {
            std::fprintf(stderr, "x2recomp: no per-user data directory; use --data-dir DIR or --assets DIR\n");
            return 1;
        }
        x2::import::ImportOptions io;
        io.apply_protection_patch = opt.apply_protection_patch;
        io.confirmed_ownership = opt.confirm_own_original;
        return x2::import::import_cli(opt.import_paths, opt.assets.empty() ? data + "assets" : opt.assets, io);
    }
    // The launcher window (runtime/ui/launcher.h): shown with --gui, when x2recomp is started without arguments (settings.cfg
    // `launcher = auto`), with `launcher = always`, and on a first run when nothing is imported yet. It saves what the user changes to
    // settings.cfg and answers with the release to play; from here on main() runs as if --game had been given. --no-gui and every
    // headless run skip it.
    std::string launched_game;
#ifdef X2_HAVE_SDL
    if (!opt.headless) {
        const std::string data = user_data_dir(opt);
        x2::Settings early_s;
        if (!data.empty()) {
            std::string text, serr;
            if (x2::load_settings_file(data + "settings.cfg", early_s, text, serr) && !serr.empty())
                std::fprintf(stderr, "x2recomp: %ssettings.cfg:\n%s", data.c_str(), serr.c_str());
        }
        Options rule;
        rule.headless = opt.headless;
        rule.gui = opt.gui >= 0 ? opt.gui : early_s.launcher == "always" ? 1 : early_s.launcher == "never" ? 0 : -1;
        bool show = x2::cli::launcher_wanted(rule, argc), first_run = false;
        if (!data.empty() && opt.assets.empty() && opt.st_disk.empty()) {
            std::error_code lib_ec;
            const bool imported = x2::ui::scan_library(data).any();
            const bool dev_assets = std::filesystem::exists("generated/assets/game_0400.bin", lib_ec) ||
                                    std::filesystem::exists("generated/assets-st/manifest.txt", lib_ec);
            const std::string wanted_game = opt.game_explicit ? opt.game : early_s.game;
            if (!imported && dev_assets && opt.gui != 1) show = false; // a development checkout plays ./generated/assets without a launcher
            else if (!imported && !dev_assets && rule.gui != 0 && (wanted_game == "auto" || wanted_game == "disk")) first_run = true;
        } else if (!opt.assets.empty() || !opt.st_disk.empty()) {
            show = show && opt.gui == 1; // an explicit game location is a game start, unless the launcher was asked for
        }
        if ((show || first_run) && !data.empty()) {
            x2::ui::LauncherEnv env;
            env.data_dir = data;
            env.settings_path = data + "settings.cfg";
            env.version = X2_VERSION;
            env.initial_game = opt.game_explicit && opt.game != "auto" ? opt.game : "";
            env.first_run = first_run;
            env.first_run_reason = "no imported game data";
            if (opt.gui == 1) {
                for (int i = 1; i < argc; ++i)
                    if (std::string(argv[i]) != "--gui") env.args_note += (env.args_note.empty() ? "x2recomp " : " ") + std::string(argv[i]);
            }
            const x2::ui::LaunchResult lr = x2::ui::run_launcher(std::move(env));
            if (!lr.start) return 0;
            launched_game = lr.game;
            opt.game = lr.game;
            opt.game_explicit = true;
        }
    }
#endif
    // Which release runs. A window run honours settings.cfg's `game` unless --game was given (the launcher counts as giving it); `auto`
    // is the disk release when it is imported (or ./generated/assets exists), else the CDTV release, else the Atari ST release.
    std::string game_choice = opt.game;
    if (!opt.headless && !opt.game_explicit && opt.assets.empty() && opt.st_disk.empty()) {
        if (const std::string d = user_data_dir(opt); !d.empty()) {
            x2::Settings early;
            if (std::ifstream in(d + "settings.cfg"); in) {
                std::stringstream text;
                text << in.rdbuf();
                std::string ignored;
                x2::parse_settings(text.str(), early, ignored);
                game_choice = early.game;
                // `launcher = never` starts the release played last (Play on the home page records it) when `game` is automatic.
                x2::ui::GameKind last;
                if (early.game == "auto" && early.launcher == "never" && x2::ui::parse_game_kind(early.last_game, last) &&
                    x2::ui::scan_library(d).get(last).present)
                    game_choice = early.last_game;
            }
        }
    }
    // The imported ST disk: --assets DIR, else ./generated/assets-st (dev checkout), else <data dir>/assets-st.
    auto find_st_assets = [&]() -> std::string {
        std::error_code st_ec;
        if (!opt.assets.empty()) return opt.assets;
        if (std::filesystem::exists("generated/assets-st/manifest.txt", st_ec)) return "generated/assets-st";
        if (const std::string d = user_data_dir(opt); !d.empty() && std::filesystem::exists(d + "assets-st/manifest.txt", st_ec)) return d + "assets-st";
        return std::string();
    };
    if (game_choice == "auto" && opt.assets.empty() && opt.st_disk.empty()) {
        std::error_code auto_ec;
        const std::string d = user_data_dir(opt);
        std::string why_unused;
        const bool disk_there = std::filesystem::exists("generated/assets/game_0400.bin", auto_ec) ||
                                (!d.empty() && x2::import::ensure_imported(d + "assets", why_unused));
        const bool cdtv_there = !d.empty() && x2::import::ensure_imported(d + "assets-cdtv", why_unused);
        const std::string st_dir = find_st_assets();
        if (!disk_there && !cdtv_there && !st_dir.empty()) {
            game_choice = "st";
            std::printf("only the Atari ST release is imported: starting it (%s)\n", st_dir.c_str());
        } else if ((disk_there || cdtv_there) && !st_dir.empty() && !opt.headless) {
            std::printf("the Atari ST release is imported too: --game st (or game = st in settings.cfg) starts it\n");
        }
    }
    if (game_choice == "st") { // the Atari ST machine: its own runner, no Amiga assets or settings involved
        if (!opt.dump_audio.empty()) {
            std::fprintf(stderr, "x2recomp: --dump-audio: the Atari ST release plays no sampled effects through Paula (its sounds are "
                                 "YM2149 register patterns), so there is nothing to dump\n");
            return 3;
        }
        if (opt.protect_music) std::printf("protect music: no effect on the Atari ST release (it has no CD music to protect)\n");
        x2::st::RunOptions so;
        so.headless = opt.headless;
        so.frames = opt.headless || opt.frames_set ? opt.frames : 0;
        so.disk_path = opt.st_disk;
        if (opt.st_disk.empty()) {
            so.assets_dir = find_st_assets();
            if (so.assets_dir.empty()) {
                const std::string d = user_data_dir(opt);
                so.assets_dir = d.empty() ? std::string() : d + "assets-st"; // the runner says what is missing
            }
        }
        // High scores (our addition: the original keeps none): a window run keeps highscores-st.bin in the data directory unless
        // --no-persist; a headless run only with --persist-dir.
        so.persist_dir = opt.persist_dir;
        if (so.persist_dir.empty() && !opt.no_persist && !opt.headless) so.persist_dir = user_data_dir(opt);
        if (opt.no_persist) so.persist_dir.clear();
        so.dump_dir = opt.dump_dir;
        so.screenshots = opt.screenshots;
        so.hash_frames = opt.hash_frames;
        so.hash_every = opt.hash_every;
        so.report_every = opt.report_set ? opt.report_every : 0;
        so.scale = opt.scale;
        so.fullscreen = opt.fullscreen;
        so.fdc_speed = opt.st_fdc_speed;
        so.io_summary = opt.st_io_summary;
        so.fdc_log = opt.st_fdc_log;
        so.trace_tail = opt.st_trace_tail;
        so.watch_pc_addr = opt.st_watch_pc_addr;
        so.watch_pc_len = opt.st_watch_pc_len;
        so.joystick = opt.st_joy;
        so.input_script = opt.st_input_script;
        so.frames_explicit = opt.frames_set;
        so.no_audio = opt.no_audio;
        so.volume = opt.volume;
        so.audio_latency = opt.audio_latency;
        so.audio_out = opt.audio_out;
        so.audio_hash = opt.audio_hash;
        so.data_dir = user_data_dir(opt);
        so.input_debug = opt.input_debug;
        so.watch = opt.st_watch;
        so.input_offset = opt.st_input_offset;
        so.ram_dumps = opt.st_ram_dumps;
        so.profile_from = opt.st_profile_from;
        so.profile_to = opt.st_profile_to;
        so.frame_log = opt.st_frame_log;
        so.shots_dir = opt.st_shots_dir;
        so.shots = opt.st_shots;
        so.filter = opt.present.filter;
        so.aspect = opt.present.aspect;
        so.integer = opt.present.integer;
        so.curvature = opt.present.curvature;
        for (const x2::Poke& p : opt.pokes) so.pokes.push_back({p.from, p.to, p.addr, p.value, p.size, p.handler});
        so.asset_packs = opt.asset_packs;
        x2::pack::set_pack_settings(pack_settings_from(opt)); // st_run loads the packs from these (the shared loader, pack/pack_stack.h)
        so.upgrade = opt.present.upgrade;
        so.canvas_scale = opt.present.scale;
        so.dump_assets = opt.dump_assets;
        so.pack_check = opt.modern_check;
        so.present_shots = opt.present_shots;
        so.god_mode = opt.god_mode;
        so.cheat_scores = opt.cheat_scores;
        so.autofire = opt.autofire;
        so.autofire_rate = opt.autofire_rate;
        so.credits = opt.credits;
        so.state_hash = opt.st_state_hash;
        so.trace_out = opt.trace_out;
        so.trace_label = opt.trace_label;
#ifdef X2_HAVE_RECOMP
        if (opt.interp_only) {
            std::printf("recomp: --interp-only, Musashi interpreter only\n");
        } else if (x2::recomp::translated_instructions(x2::recomp::Target::St) == 0) {
            opt.interp_only = true; // nothing to report or profile either
            std::printf("recomp: no translated code for the Atari ST release is linked in, Musashi interpreter only\n");
        } else {
            so.attach_cpu = [](x2::st::StMachine& m, std::string& err) {
                if (!x2::recomp::enable_raw(m.ram(), m.ram_size(), err, x2::recomp::Target::St)) return false;
                m.set_cpu_execute(&x2::recomp::execute);
                std::printf("recomp: hybrid executor on, %zu translated instructions\n",
                            x2::recomp::translated_instructions(x2::recomp::Target::St));
                return true;
            };
        }
        if (!opt.interp_only) {
            const int stats = opt.recomp_stats;
            const std::string profile = opt.recomp_profile;
            so.after_run = [stats, profile](const x2::st::StMachine& m) {
                if (stats >= 0) x2::recomp::print_summary_raw(m.ram(), m.ram_size(), stats);
                if (!profile.empty()) {
                    if (!x2::recomp::write_profile_raw(m.ram(), m.ram_size(), profile)) {
                        std::fprintf(stderr, "x2recomp: cannot write %s\n", profile.c_str());
                        return false;
                    }
                    std::printf("recomp profile: %s\n", profile.c_str());
                }
                return true;
            };
        }
#else
        if (opt.recomp_stats >= 0 || !opt.recomp_profile.empty())
            std::printf("recomp: not built with -DX2_RECOMP=ON, interpreter only\n");
#endif
        return x2::st::run(so);
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
            } else if (std::string why; !x2::save_settings_file(settings_path, file_settings, why)) {
                std::fprintf(stderr, "x2recomp: %s\n", why.c_str());
            }
            std::printf("settings: %s\n", settings_path.c_str());
            Options merged;
            apply_settings(file_settings, merged);
            if (!parse_args(argc, argv, merged)) { usage(); return 2; }
            opt = std::move(merged);
            if (!launched_game.empty()) { // the launcher chose the release: it counts as --game
                opt.game = launched_game;
                opt.game_explicit = true;
            }
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
    x2::pack::set_pack_settings(pack_settings_from(opt)); // the one place later work reads them from (pack/pack_api.h)
    // --list-asset-packs needs the merged settings (packs_enabled).
    if (opt.list_asset_packs) return x2::pack::list_asset_packs_cli(x2::pack::pack_settings(), opt.check_pack.json);
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
        std::error_code exists_ec;
        bool dev_assets = opt.game != "cdtv" && std::filesystem::exists("generated/assets/game_0400.bin", exists_ec);
        std::string data = dev_assets ? "" : user_data_dir(opt);
        if (!dev_assets && data.empty()) {
            std::fprintf(stderr, "x2recomp: no per-user data directory; use --data-dir DIR or --assets DIR\n");
            return 3;
        }
        std::string imported = data + "assets", imported_cdtv = data + "assets-cdtv", why;
        if (dev_assets) opt.assets = "generated/assets";
        else if (opt.game != "cdtv" && x2::import::ensure_imported(imported, why)) {
            opt.assets = imported;
            std::error_code both_ec;
            if (opt.game == "auto" && std::filesystem::exists(imported_cdtv + "/manifest.txt", both_ec) && !opt.headless)
                std::printf("both releases are imported: starting the Amiga disk release (--game cdtv, or game = cdtv in settings.cfg, "
                            "starts the CDTV release)\n");
        }
        else if (opt.game != "disk" && x2::import::ensure_imported(imported_cdtv, why)) opt.assets = imported_cdtv;
        else if (opt.game == "cdtv") {
            std::fprintf(stderr, "x2recomp: no imported CDTV release (%s). Import your CDTV disc image first: x2recomp --import DISC.cue "
                                 "(or its ZIP or folder), or give --assets DIR\n", why.c_str());
            return 3;
        }
        else {
            std::fprintf(stderr, "x2recomp: no game data: %s.\nImport your own disk images first: x2recomp --import "
                                 "PATH [PATH2] (ADF, DMS, IPF, flux dump, ZIP or a folder), or give --assets DIR%s\n",
                         why.c_str(), opt.headless ? "" : "; x2recomp --gui opens the launcher, which imports them for you");
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
    // Output folders come from the command line: a bad one is an error message, not an uncaught filesystem_error.
    auto make_dir = [](const std::string& dir, const char* what) {
        std::error_code mkdir_ec;
        fs::create_directories(fs::path(dir), mkdir_ec);
        if (mkdir_ec) std::fprintf(stderr, "x2recomp: cannot create %s '%s': %s\n", what, dir.c_str(), mkdir_ec.message().c_str());
        return !mkdir_ec;
    };
    if (!opt.dump_dir.empty() && !make_dir(opt.dump_dir, "--dump-dir")) return 3;
    if (opt.reglog && !make_dir(opt.dump_dir + "/regs", "the register log folder")) return 3;

    x2::Machine m((opt.chip_kb ? opt.chip_kb : assets.build->chip_kb) * 1024u);
    for (const Poke& p : opt.pokes)
        if (p.addr + uint32_t(p.size) > m.chip_size) {
            std::fprintf(stderr, "x2recomp: poke at $%X is outside the %u KB of chip RAM\n", p.addr, m.chip_size / 1024);
            return 2;
        }
    if (assets.build->cdtv && m.chip_size < 0x100000) {
        std::fprintf(stderr, "x2recomp: the CDTV release needs 1024 KB of chip RAM (--chip-ram 1024)\n");
        return 2;
    }
    m.set_timing(opt.timing);
    x2::DiskShim disk(assets.disc2, assets.build->load_sectors_pc, assets.build->disc_sector_bias, assets.build->disc_sectors_per_track);
    if (assets.build->load_sectors_pc) disk.install(m); // the CDTV release has no disk loader
    Milestones ms;
    uint32_t vbl_entries = 0, main_loop_entries = 0, discid_ok = 0, discid_calls = 0;
    // Entry-count probes; PC traps run before the instruction, then execution continues normally.
    const x2::BuildInfo& build = *assets.build;
    m.add_trap(build.vbl_pc, [&](x2::Machine&) { ++vbl_entries; });
    // $78CE (addq.w #1,$CDC): the main loop's back-edge target, reached once per game-logic frame.
    m.add_trap(build.main_loop_pc, [&](x2::Machine&) { ++main_loop_entries; });
    // $8FDE: cmpi.l #'X2B ',$49602 right after the shim returns inside the disc-id check.
    if (build.discid_cmp_pc)
        m.add_trap(build.discid_cmp_pc, [&](x2::Machine& mm) {
            ++discid_calls;
            if (mm.read32(0x49602) == 0x58324220) ++discid_ok;
        });
    x2::boot_game(m, assets);
    if (!build.cdtv) {
        // A retail image still holds the in-game Copylock routine (runtime/retail_protection.h): answered when
        // the importer recorded a verified key track, refused otherwise. The canonical image is left alone.
        x2::CopylockDatum copylock;
        std::string cl_why;
        if (!x2::load_copylock_datum(opt.assets, copylock, cl_why)) {
            std::fprintf(stderr, "x2recomp: %s\n", cl_why.c_str());
            return 3;
        }
        const std::string image_sha1 = x2::import::sha1_hex(assets.game_image.data(), assets.game_image.size());
        switch (x2::install_retail_protection(m, assets, copylock, image_sha1)) {
        case x2::RetailProtection::NotRetail: break;
        case x2::RetailProtection::Installed:
            std::printf("retail image: key track verified at import (seed 0x%06X); the in-game Copylock check at $70F6 is answered\n",
                        copylock.seed);
            break;
        case x2::RetailProtection::UnboundKey:
        case x2::RetailProtection::WrongImage:
            std::fprintf(stderr,
                         "x2recomp: %s/%s %s. Import the original disc again so that the importer writes a receipt for this "
                         "image.\n",
                         opt.assets.c_str(), x2::kCopylockDatumFile,
                         copylock.image_sha1.empty() ? "is an old receipt that names no game image" : "was written for a different game image");
            return 3;
        case x2::RetailProtection::MissingKey:
            std::fprintf(stderr,
                         "x2recomp: %s/game_0400.bin is a retail image (it contains the Copylock routine) but there is no %s "
                         "next to it, which the importer writes after verifying the key track of your disc. Import the "
                         "original disc from an IPF or flux image that carries that track, or import it again with the opt-in "
                         "--apply-protection-patch (docs/re-notes/retail-protection.md).\n",
                         opt.assets.c_str(), x2::kCopylockDatumFile);
            return 3;
        }
    }
    // Assists and cheats (README "Assists and cheats"). Game variables, all verified in
    // docs/re-notes/trace-coverage.md §2: energy $CC6 (full = $27, refilled to that on respawn at $7AF6),
    // ships $CCA (0 whenever no game is in progress, which is what tells a level from the menus), credits
    // $CF0 (the display at $8F56 pokes one ASCII digit, so 9 is the most that can be shown).
    const uint32_t kEnergy = build.var_energy, kShips = build.var_ships, kCredits = build.var_credits;
    constexpr uint32_t kEnergyFull = 0x27;
    constexpr int kMaxCredits = 9, kGameCredits = 3; // the game's own starting value
    // The ship's death routine, and the counter of the push-back sequence that can reach it without any
    // energy being lost ($42E counts down from -1; at -$11 the ship is crushed, $5FE0).
    const uint32_t kPushBack = build.var_pushback;
    const uint32_t kDeathRoutine = build.death_pc;
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
            x2::write_hiscores(m, t, build.hiscore_addr);
            std::printf("high scores: loaded %s\n", hs_path.c_str());
        } else {
            std::printf("high scores: %s: %s; using the game's own table\n", hs_path.c_str(), why.c_str());
        }
        hs_saved = x2::read_hiscores(m, build.hiscore_addr);
    };
    if (!opt.persist_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opt.persist_dir, ec);
        // One table file per release: the file carries the hash of the game image it belongs to, so a shared name would make each
        // release discard and overwrite the other's table.
        hs_path = (fs::path(opt.persist_dir) / (build.cdtv ? "highscores-cdtv.bin" : "highscores.bin")).string();
        restore_hiscores();
        m.add_trap(build.hiscore_saved_pc, [&](x2::Machine& mm) {
            x2::HiscoreTable t = x2::read_hiscores(mm, build.hiscore_addr);
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
    if (opt.interp_only) {
        std::printf("recomp: --interp-only, Musashi interpreter only\n");
    } else {
        // Each game has its own translation (tools/recomp/recomp.py --build disk|cdtv); a build that carries only the
        // other game's runs this one on the interpreter, and says so.
        const auto target = build.cdtv ? x2::recomp::Target::Cdtv : x2::recomp::Target::Disk;
        if (x2::recomp::translated_instructions(target) == 0) {
            opt.interp_only = true; // nothing to report or profile either
            std::printf("recomp: no translated code for the %s release is linked in, Musashi interpreter only\n",
                        build.cdtv ? "CDTV" : "disk");
        } else {
            std::string rerr;
            if (!x2::recomp::enable(m, rerr, target)) {
                std::fprintf(stderr, "x2recomp: recompiled code disabled: %s\n", rerr.c_str());
                return 5;
            }
            std::printf("recomp: hybrid executor on, %zu translated instructions\n", x2::recomp::translated_instructions(target));
        }
    }
#else
    if (opt.recomp_stats >= 0 || !opt.recomp_profile.empty())
        std::printf("recomp: not built with -DX2_RECOMP=ON, interpreter only\n");
#endif
    x2::Video video;
    video.attach(m);
    std::unique_ptr<x2::Tracer> tracer;
    if (!opt.trace_out.empty()) {
        tracer = std::make_unique<x2::Tracer>(m, build.cdtv ? x2::Tracer::Layout::cdtv(m.chip_size) : x2::Tracer::Layout::disk());
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
            if (!display->open(std::string("Xenon 2 Megablast") + (build.cdtv ? " (CDTV)" : "") + " - x2recomp " X2_VERSION, kW, kH, opt.scale,
                               opt.fullscreen, opt.present.effective().aspect, derr)) {
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
    // The packs load before the music cue layer so that it exists whenever a loaded pack has music.
    auto asset_lib = std::make_shared<x2::modern::AssetLibrary>();
    {
        // The packs of this run (settings packs_enabled, asset_packs, --asset-pack), layered for this game and build (pack/pack_api.h).
        x2::pack::PackContext pack_ctx;
        pack_ctx.game = build.cdtv ? x2::pack::GameFamily::Cdtv : x2::pack::GameFamily::Disk;
        pack_ctx.build = build.id;
        x2::pack::PackReport pack_report;
        std::shared_ptr<x2::pack::PackStack> stack = x2::pack::load_pack_stack(x2::pack::pack_settings(), pack_ctx, pack_report);
        for (const x2::pack::PackMessage& pm : pack_report.messages) std::printf("%s\n", pm.format().c_str());
        if (stack) {
            x2::pack::set_packs(stack);
            asset_lib->adopt(stack);
        }
    }
    // CDTV CD music (runtime/cdtv/cdtv_music.h). Headless runs leave the machine untouched unless it is asked for;
    // the window installs it so the music-source hotkey works. It never changes the game's state.
    std::unique_ptr<x2::cdtv::CdtvMusic> cdtv;
    x2::cdtv::Disc cd_disc;
    FILE* cdtv_log = nullptr;
    x2::cdtv::MusicSource music_source = x2::cdtv::MusicSource::Amiga;
    x2::cdtv::parse_music_source(opt.music, music_source);
    // The CDTV release's music IS the CD: a window plays it unless asked otherwise ("amiga", the default, means the tracker
    // music, which that release does not have); a headless run stays silent unless --music cdtv is given, so that replay
    // hashes do not depend on the CD audio files.
    if (build.cdtv && !opt.headless && opt.music == "amiga") music_source = x2::cdtv::MusicSource::Cdtv;
    // Asset packs may replace the music (audio/music_slots.h) and the disk builds' sound effects (audio/pack_audio.h, whose effect tags
    // this layer installs), and --dump-audio needs the tags: the cue layer is needed for them too. It changes nothing in the machine.
    const bool pack_music = x2::pack::packs().has_audio() || !opt.dump_audio.empty();
    if (build.cdtv || !opt.headless || pack_music || music_source != x2::cdtv::MusicSource::Amiga || opt.music_volume != 100 ||
        opt.sfx_volume != 100 || opt.protect_music || !opt.cdtv_path.empty() || !opt.cdtv_log.empty()) {
        cdtv = std::make_unique<x2::cdtv::CdtvMusic>();
        std::string path = opt.cdtv_path, why;
        if (path.empty()) {
            std::string data = user_data_dir(opt);
            std::error_code cdtv_ec;
            if (!data.empty() && fs::exists(data + "cdtv/manifest.txt", cdtv_ec)) path = data + "cdtv";
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
        if (build.cdtv) cdtv->attach_game(m);
        else {
            // The tags that tell the effect code's sample starts from the tracker's are per build (the 1762 compilation moved its driver).
            const x2::modern::AudioLayout& al = x2::modern::audio_layout_for(x2::pack::GameFamily::Disk, build.id ? build.id : "");
            x2::cdtv::EffectTags tags;
            tags.effect_lo = al.effect_lo;
            tags.effect_hi = al.effect_hi;
            tags.music_restore = al.music_restore;
            tags.music_lo = al.music_init;
            tags.music_tick = al.music_tick;
            tags.music_ptr = al.music_ptr;
            cdtv->set_effect_tags(tags);
            cdtv->install(m);
        }
        cdtv->set_volumes(opt.music_volume, opt.sfx_volume);
        if (build.cdtv && opt.cd_pause != "keep")
            std::printf("--cd-pause: the CDTV release has no pause of its own (it never reads the keyboard), so the CD keeps playing; "
                        "pausing the emulation itself holds the CD too\n");
        if (build.cdtv && opt.cdtv_tracks != "2,3,5,2,3,5")
            std::printf("--cdtv-tracks: not used for the CDTV release (the game picks its own tracks; its SELECT TITLES screen changes them)\n");
        if (build.cdtv && opt.protect_music) {
            std::printf("protect music: not applicable to the CDTV release (its music is the CD)\n");
            opt.protect_music = false;
        }
        cdtv->set_protect_music(opt.protect_music);
        cdtv->set_source(music_source);
        if (opt.protect_music)
            std::printf("protect music: on (the tune keeps its voices through the sound effects; not the "
                        "original's behaviour)\n");
        if (music_source != x2::cdtv::MusicSource::Amiga || !opt.headless)
            std::printf("music: %s\n", x2::cdtv::music_source_name(cdtv->effective_source()));
    }

    // The CDTV release: answer its OS start-up, file loads and CD-audio calls (runtime/cdtv/cdtv_game.h).
    x2::cdtv::CdtvGame cdtv_game;
    if (build.cdtv) {
        std::string cg_why;
        cdtv_game.set_log(cdtv_log);
        if (!cdtv_game.install(m, assets, cdtv.get(), cg_why)) {
            std::fprintf(stderr, "x2recomp: CDTV build: %s\n", cg_why.c_str());
            return 3;
        }
        const bool cd_on = cdtv->effective_source() != x2::cdtv::MusicSource::Amiga;
        std::printf("CDTV release: %zu disc files, OS start-up and file loads answered by the runtime, CD audio %s\n",
                    cdtv_game.files().file_count(),
                    cd_on ? "on" : (cd_disc.tracks.empty() ? "off (no CD tracks imported: the music is silent)" : "off (--music cdtv plays it)"));
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
    // Where this build keeps the screen buffers, object lists and drawing routines (modern/layout.h).
    const x2::modern::CaptureLayout* const capture_layout = build.cdtv ? &x2::modern::kCdtvLayout : &x2::modern::kDiskLayout;
    x2::modern::DrawCapture draw_capture;
    draw_capture.layout = capture_layout;
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
    presenter.set_assets(asset_lib);
    presenter.set_layout(capture_layout);
    if (const auto* stack = x2::pack::packs().has_graphics() ? &x2::pack::packs() : nullptr; stack && (!stack->screens().empty() || stack->panel()))
        std::printf("asset packs: %zu screens%s from the pack%s\n", stack->screens().size(), stack->panel() ? " and a status panel" : "",
                    stack->packs().size() > 1 ? "s" : "");
    // Sound effects and speech of the packs, and --dump-audio (audio/pack_audio.h): an output-mix stage fed by Paula's events.
    x2::audio::PackAudio pack_audio;
    x2::audio::AudioDumper audio_dumper;
    bool pack_audio_on = false;
    if (x2::pack::packs().has_audio() || !opt.dump_audio.empty()) {
        const x2::pack::GameFamily family = build.cdtv ? x2::pack::GameFamily::Cdtv : x2::pack::GameFamily::Disk;
        pack_audio.attach(m.paula, m.chip.data(), m.chip_mask, family, x2::modern::audio_layout_for(family, build.id ? build.id : ""));
        pack_audio.set_sfx_volume(opt.sfx_volume);
        if (!pack_audio.effects_mapped())
            std::printf("asset packs: the sound effects of the %s build are not mapped, so none are replaced or dumped\n", build.id ? build.id : "this");
        pack_audio.set_frame_source([&m]() { return uint64_t(m.frame); });
        if (!opt.dump_audio.empty()) {
            std::string why;
            if (!audio_dumper.open(opt.dump_audio, why)) { std::fprintf(stderr, "x2recomp: --dump-audio: %s\n", why.c_str()); return 3; }
            pack_audio.set_dumper(&audio_dumper);
            if (!build.cdtv && !cdtv) { std::fprintf(stderr, "x2recomp: --dump-audio: the effect tags are not installed\n"); return 3; }
        }
        if (build.cdtv)
            cdtv_game.set_file_loaded_hook([&pack_audio](uint32_t dest, const std::string& name, uint32_t size) { pack_audio.on_file_loaded(dest, name, size); });
        pack_audio_on = true;
    }
    x2::modern::AssetDumper dumper;
    std::unique_ptr<x2::modern::ScreenDumper> screen_dumper;
    const bool keep_capture = opt.modern_check || !opt.modern_shots.empty() || !opt.dump_assets.empty();
    presenter.set_index_source(&video);
    presenter.configure(m, opt.present, m.lines_per_frame(), keep_capture);
    video.set_index_capture(presenter.screens_active() || !opt.dump_assets.empty());
    if (!opt.dump_assets.empty()) {
        std::string why;
        if (!dumper.open(opt.dump_assets, why)) { std::fprintf(stderr, "x2recomp: --dump-assets: %s\n", why.c_str()); return 3; }
        screen_dumper = std::make_unique<x2::modern::ScreenDumper>(capture_layout->panel);
        presenter.interp()->capture.on_step = [&dumper](const x2::modern::Step& s) { dumper.step(s); };
    }
    if (presenter.interp()) {
        if (draw_log_file) { std::fprintf(stderr, "x2recomp: --draw-log and --modern cannot be combined\n"); return 2; }
        if (!opt.modern_out.empty() && opt.modern_out != "-" && !make_dir(opt.modern_out, "--modern-out")) return 3;
        if (!opt.modern_shots.empty() && !make_dir(opt.modern_shots, "--modern-shots")) return 3;
    }
    if (!opt.headless || !opt.modern_out.empty()) std::printf("picture: %s\n", presenter.options().describe().c_str());
    struct ModernCheck {
        uint32_t pairs = 0, start_exact = 0, end_exact = 0, smooth_end_exact = 0, compared = 0, frame_exact = 0, wide_exact = 0;
        uint64_t smooth_diff_pixels = 0;
        uint32_t last_step = ~0u, pending_step = ~0u, pending_frame = 0, last_mismatch = 0;
        std::vector<uint8_t> pending_picture;
        bool shots_done = false;
        x2::modern::ScreensCheck screens;
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
        display->set_title(std::string("Xenon 2 Megablast") + (build.cdtv ? " (CDTV)" : "") + " - " + presenter.options().describe() +
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
        x2::pack::set_pack_settings(pack_settings_from(opt)); // the Faithful switch also turns pack audio off (the music cue layer polls it)
        presenter.configure(m, opt.present, m.lines_per_frame(), keep_capture);
        video.set_index_capture(presenter.screens_active() || !opt.dump_assets.empty());
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
                uint32_t a = pc & mm.chip_mask;
                std::fprintf(pc_log_file, "%u %d %u %lld %06X %04X\n", mm.frame, mm.line, mm.hpos(), (long long)mm.cpu_now(),
                             pc, (mm.chip[a] << 8) | mm.chip[(a + 1) & mm.chip_mask]);
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
                        if (pack_audio_on) pack_audio.reset(m.paula);
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
                        if (opt.present.upgrade && asset_lib->empty() && x2::pack::packs().empty())
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
                        if (cdtv && build.cdtv) {
                            // The CDTV release's music is the CD and there is no tracker tune to switch to: the key mutes and
                            // unmutes the CD for this session (the saved setting is the disk build's and is left alone).
                            music_source = cdtv->source() == x2::cdtv::MusicSource::Cdtv ? x2::cdtv::MusicSource::Amiga
                                                                                         : x2::cdtv::MusicSource::Cdtv;
                            cdtv->set_source(music_source);
                            std::printf("music: CD audio %s%s\n", music_source == x2::cdtv::MusicSource::Cdtv ? "on" : "off",
                                        music_source == x2::cdtv::MusicSource::Cdtv && cdtv->effective_source() != music_source
                                            ? " (no CD tracks imported: silent)" : "");
                        } else if (cdtv) {
                            music_source = x2::cdtv::MusicSource((int(cdtv->source()) + 1) % 3);
                            cdtv->set_source(music_source);
                            file_settings.music = x2::cdtv::music_source_name(music_source);
                            settings_dirty = true;
                            std::printf("music: %s%s\n", file_settings.music.c_str(),
                                        cdtv->effective_source() != music_source ? " (no CDTV disc audio: Amiga music)" : "");
                        }
                        break;
                    case x2::input::HostCommand::ProtectMusic: {
                        if (!cdtv || build.cdtv) { // no CD layer at all, or the CDTV release, whose music is the CD (no tracker voices)
                            std::printf("protect music: %s\n", build.cdtv ? "not applicable to the CDTV release (its music is the CD)" : "not available in this run");
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
        if (opt.headless || fire || (dirs & 31)) m.set_joystick(dirs & 1, dirs & 2, dirs & 4, dirs & 8, fire);
        if (opt.headless || (dirs & 32)) m.set_button2(dirs & 32);
        for (const Poke& p : opt.pokes)
            if (f >= p.from && f <= p.to) x2::apply_poke(m, p, assets.build->var_objlist);
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
        if (pack_audio_on) pack_audio.after_frame(m.paula); // pack sound effects and speech, the same way, after the CD and the music
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
            for (uint32_t a : opt.watch) std::fprintf(watch_file, " %04X", (m.chip[a & m.chip_mask] << 8) | m.chip[(a + 1) & m.chip_mask]);
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
        {
            const bool in_game = m.read16(kShips) != 0;
            presenter.set_in_game(in_game);
            if (opt.modern_check && presenter.screens_active() && !presenter.options().wide.on() && !presenter.interpolating()) {
                const x2::modern::Canvas c = presenter.at(double(f + 1), video.front());
                const x2::modern::IndexedFrame idx = x2::modern::amiga_indexed(video);
                if (idx.valid()) mcheck.screens.add(presenter.pack_screens(), idx, reinterpret_cast<const uint32_t*>(c.rgba), size_t(c.w), c.scale, 0,
                                                    kTop * c.scale, f);
            }
            if (screen_dumper) {
                // Frames a captured step covers are play; a step is captured every few frames at most.
                bool play = false;
                if (x2::modern::InterpPresenter* ip = presenter.interp(); ip && !ip->capture.steps().empty()) {
                    const x2::modern::Step& last = ip->capture.steps().back();
                    play = x2::modern::Interpolator::gameplay(last) && f <= last.swap_frame + 4;
                }
                screen_dumper->frame(x2::modern::amiga_indexed(video), f, in_game, play);
            }
        }
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
                std::printf(" %06X %s(%.0f%%)", pcs[i].first, (assets.build->canonical_layout ? known_name(pcs[i].first) : ""), 100.0 * pcs[i].second / std::max(1u, total));
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
        if (std::string why; !x2::save_settings_file(settings_path, file_settings, why))
            std::fprintf(stderr, "x2recomp: cannot write %s (%s)\n", settings_path.c_str(), why.c_str());
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
    if (build.cdtv) {
        const x2::cdtv::GameStats& g = cdtv_game.stats();
        std::printf("cdtv game: %u OS start-ups skipped, files: %u opens (%u failed), %u reads (%llu bytes), %u closes; CD: %u play, "
                    "%u stop, %u ticks (%u polls, %u tracks started again), %u volume, last track %d\n", g.os_skips, g.opens, g.failed_opens,
                    g.reads, (unsigned long long)g.bytes, g.closes, g.cd_play, g.cd_stop, g.cd_ticks, g.cd_polls, g.cd_restarts,
                    g.cd_volume, g.last_track);
    }
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
        std::string why, members;
        if (screen_dumper) {
            if (!screen_dumper->finish(opt.dump_assets, members, why)) {
                std::fprintf(stderr, "x2recomp: --dump-assets: %s\n", why.c_str());
                return 3;
            }
            dumper.set_extra_members(members);
            std::printf("dump-assets: %zu screens (%zu transitions skipped, %zu not matchable), %zu panel cells (the strip had %zu palettes)\n",
                        screen_dumper->screens(), screen_dumper->transitions(), screen_dumper->not_matchable(), screen_dumper->panel_cells(),
                        screen_dumper->strip_palettes());
            for (const std::string& w : screen_dumper->warnings()) std::printf("dump-assets: warning: %s\n", w.c_str());
        }
        {   // a manifest that already has an audio section (an earlier --dump-audio into this directory) keeps it
            const std::string keep = x2::audio::AudioDumper::existing_audio_member(opt.dump_assets);
            if (!keep.empty()) {
                members += std::string(members.empty() ? "" : ",\n  ") + keep;
                dumper.set_extra_members(members);
            }
        }
        const bool ok = dumper.finish(why);
        for (const std::string& w : dumper.warnings()) std::printf("dump-assets: warning: %s\n", w.c_str());
        std::printf("dump-assets: %zu sprites, %zu tiles, %zu far lines, %zu flashes, %zu far layers, %zu palettes -> %s/manifest.json%s%s\n",
                    dumper.sprites(), dumper.tiles(), dumper.far_lines(), dumper.flashes(), dumper.backgrounds(), dumper.palettes(),
                    opt.dump_assets.c_str(),
                    ok ? "" : ": ", why.c_str());
        if (!ok) return 3;
    }
    if (!opt.dump_audio.empty()) {
        std::string why;
        const bool ok = audio_dumper.finish(why);
        std::printf("dump-audio: %zu sound effects, %zu speech clips%s -> %s/manifest.json%s%s\n", audio_dumper.sfx_count(),
                    audio_dumper.speech_count(),
                    audio_dumper.skipped() ? (" (" + std::to_string(audio_dumper.skipped()) + " more over the limits)").c_str() : "",
                    opt.dump_audio.c_str(), ok ? "" : ": ", why.c_str());
        if (!ok) return 3;
    }
    if (opt.modern_check && mcheck.screens.frames) std::printf("%s\n", mcheck.screens.summary(presenter.pack_screens()).c_str());
    if (x2::modern::InterpPresenter* modern = presenter.interp()) {
        modern->detach(m);
        if (opt.modern_check)
            std::printf("modern check: %u interpolable step pairs; t=0 exact %u, t=1 exact %u (far-layer smoothing off); "
                        "t=1 exact with smoothing %u (%.2f differing pixels per pair); captured picture = rendered frame "
                        "%u of %u (last mismatch at frame %u)\n",
                        mcheck.pairs, mcheck.start_exact, mcheck.end_exact, mcheck.smooth_end_exact,
                        mcheck.pairs ? double(mcheck.smooth_diff_pixels) / mcheck.pairs : 0.0, mcheck.frame_exact,
                        mcheck.compared, mcheck.last_mismatch);
        if (opt.modern_check && modern->interp.opt.wide > 0)
            std::printf("modern check: surround %d pixels per side, centre exact at t=0 and t=1 in %u of %u pairs\n",
                        modern->interp.opt.wide, mcheck.wide_exact, mcheck.pairs);
        std::printf("modern: %llu frames presented, %llu interpolated, %.2f ms per interpolated frame (scale %d)\n",
                    (unsigned long long)modern->presented, (unsigned long long)modern->interpolated,
                    modern->interpolated ? double(modern->render_ns) / 1e6 / double(modern->interpolated) : 0.0,
                    modern->interp.opt.scale);
        if (g_present_pictures)
            std::printf("present time: %llu pictures, %.1f us per picture (headless output; hashing and writing the pictures excluded)\n",
                        (unsigned long long)g_present_pictures, double(g_present_ns) / 1e3 / double(g_present_pictures));
        const auto& ps = modern->interp.stats;
        if (ps.background_plans || ps.flashes_replaced || ps.round_stars)
            std::printf("modern packs: %llu steps with a background, %llu hit flashes replaced, %llu round stars\n",
                        (unsigned long long)ps.background_plans, (unsigned long long)ps.flashes_replaced, (unsigned long long)ps.round_stars);
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
        if (tp.has_parent_path() && !make_dir(tp.parent_path().string(), "the trace folder")) return 3;
        std::string label = opt.trace_label.empty() ? tp.stem().string() : opt.trace_label;
        if (tracer->write_json(opt.trace_out, label, m.frame)) std::printf("trace: %s\n", opt.trace_out.c_str());
        else { std::fprintf(stderr, "x2recomp: cannot write %s\n", opt.trace_out.c_str()); return 3; }
    }
    if (!opt.dump_dir.empty())
        std::printf("outputs: %s/{chipram.bin,final.ppm,final.png%s}\n", opt.dump_dir.c_str(), opt.reglog ? ",regs/frame_NNNNN.log" : "");
    return 0;
}
