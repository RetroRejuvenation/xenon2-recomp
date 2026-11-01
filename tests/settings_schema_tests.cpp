// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The settings schema (runtime/ui/settings_schema.*) against settings.cfg and the command line parser, no game data.
//
//   x2_settings_schema_tests [X2RECOMP_EXECUTABLE]
//
// With the executable path it also runs `x2recomp --print-settings --data-dir <empty> <command>` for the rendered command lines, which
// is the same round trip through the real binary: what the footer says must start the game with the settings the page showed.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cli_options.h"
#include "ui/settings_schema.h"

using namespace x2;
using namespace x2::ui;
namespace fs = std::filesystem;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                       \
    do {                                                                                                                     \
        ++g_checks;                                                                                                          \
        const std::string x_ = (a), y_ = (b);                                                                                \
        if (x_ != y_) {                                                                                                      \
            ++g_failures;                                                                                                    \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n    got:  %s\n    want: %s\n", __FILE__, __LINE__, #a, x_.c_str(), y_.c_str()); \
        }                                                                                                                    \
    } while (0)

std::string g_exe;
fs::path g_dir;

// settings.cfg keys as format_settings writes them.
std::vector<std::string> file_keys() {
    std::vector<std::string> keys;
    std::istringstream in(format_settings(Settings{}));
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        k.erase(k.find_last_not_of(" \t") + 1);
        keys.push_back(k);
    }
    return keys;
}

void test_schema_complete() {
    const std::vector<std::string> keys = file_keys();
    std::set<std::string> in_file(keys.begin(), keys.end()), in_schema;
    CHECK(keys.size() == in_file.size()); // no key twice in the file
    for (const SettingRow& r : schema()) {
        CHECK(in_schema.insert(r.key).second); // no row twice
        CHECK(in_file.count(r.key) == 1);      // every row is a key of settings.cfg
        CHECK(r.label[0] != 0 && r.help[0] != 0);
        CHECK(std::strlen(r.label) <= 18);     // fits the label column (148 pixels) without an ellipsis
        if (r.type == SettingType::Choice) CHECK(r.choices && r.nchoices >= 2);
        if (r.type == SettingType::Int) CHECK(r.lo < r.hi && r.step >= 1);
        if (r.type == SettingType::Float) CHECK(r.flo < r.fhi && r.fstep > 0);
        CHECK(r.games != 0 && (r.games & ~kAllGames) == 0);
    }
    for (const std::string& k : keys) {
        CHECK(in_schema.count(k) == 1); // a key with no row: add one to settings_schema.cpp
        if (!in_schema.count(k)) std::fprintf(stderr, "    settings.cfg key without a schema row: %s\n", k.c_str());
    }
    CHECK(find_row("scale") && !find_row("no_such_key"));
    // Every page has rows, and the pages that hold the three built pages are non-trivial.
    CHECK(rows_for_page(kPagePlay).size() >= 6 && rows_for_page(kPageDisplay).size() >= 12 && rows_for_page(kPageAudio).size() >= 10);
}

void test_schema_values() {
    const Settings def;
    for (const SettingRow& r : schema()) {
        std::string err;
        Settings s = def;
        const std::string dv = value_text(r, def);
        // The default spelling is accepted back (empty text rows are allowed to be empty).
        CHECK(set_value_text(r, s, dv, err));
        CHECK_EQ(value_text(r, s), dv);
        switch (r.type) {
        case SettingType::Bool: {
            for (bool v : {false, true}) {
                Settings t = def;
                set_bool(r, t, v);
                CHECK(get_bool(r, t) == v);
                Settings u = def;
                CHECK(set_value_text(r, u, value_text(r, t), err));
                CHECK(get_bool(r, u) == v);
            }
            break;
        }
        case SettingType::Int: {
            for (int v : {r.lo, r.hi, (r.lo + r.hi) / 2}) {
                Settings t = def;
                set_int(r, t, v);
                CHECK(get_int(r, t) == v);
                Settings u = def;
                CHECK(set_value_text(r, u, std::to_string(v), err));
                CHECK(get_int(r, u) == v);
            }
            Settings t = def;
            set_int(r, t, r.hi + 100);
            CHECK(get_int(r, t) == r.hi); // clamped
            set_int(r, t, r.lo - 100);
            CHECK(get_int(r, t) == r.lo);
            // The file refuses what the page would never offer.
            Settings bad = def;
            CHECK(!set_value_text(r, bad, std::to_string(r.hi + 1), err) && !err.empty());
            CHECK(!set_value_text(r, bad, std::to_string(r.lo - 1), err));
            CHECK_EQ(value_text(r, bad), dv); // untouched
            break;
        }
        case SettingType::Float: {
            for (float v : {r.flo, r.fhi, 0.5f}) {
                Settings t = def;
                set_float(r, t, v);
                CHECK(get_float(r, t) == v);
            }
            Settings bad = def;
            CHECK(!set_value_text(r, bad, "1.5", err));
            CHECK(!set_value_text(r, bad, "-0.1", err));
            break;
        }
        case SettingType::Choice: {
            for (int i = 0; i < r.nchoices; ++i) {
                Settings t = def;
                set_choice(r, t, i);
                CHECK(get_choice(r, t) == i);
                CHECK_EQ(value_text(r, t), r.choices[i].value);
                Settings u = def;
                CHECK(set_value_text(r, u, r.choices[i].value, err)); // every choice is a value the file accepts
                CHECK(get_choice(r, u) == i);
            }
            Settings bad = def;
            CHECK(!set_value_text(r, bad, "no-such-value", err));
            break;
        }
        case SettingType::Text: break;
        }
    }
    // A surround in pixels is a value the file accepts and the choice list does not name.
    const SettingRow* wide = find_row("wide");
    Settings s;
    std::string err;
    CHECK(set_value_text(*wide, s, "40", err) && get_choice(*wide, s) == -1 && value_text(*wide, s) == "40");
    // The two enum-backed choices follow the order of their enums.
    const SettingRow* af = find_row("audio_filter");
    Settings a;
    set_choice(*af, a, 1);
    CHECK(a.filter == FilterSetting::A1200);
    const SettingRow* vf = find_row("filter");
    set_choice(*vf, a, 4);
    CHECK(a.video_filter == VideoFilter::Edge);
    // "modern" is the Faithful switch.
    const SettingRow* faithful = find_row("modern");
    set_bool(*faithful, a, true);
    CHECK(a.faithful && value_text(*faithful, a) == "faithful");
    // The new keys.
    Settings n;
    CHECK(parse_settings("launcher = never\nui_text = large\nui_contrast = high\nui_motion = reduced\nlast_game = cdtv\n", n, err));
    CHECK(n.launcher == "never" && n.ui_text == "large" && n.ui_contrast == "high" && n.ui_motion == "reduced" && n.last_game == "cdtv");
    for (const char* bad : {"launcher = sometimes", "ui_text = huge", "ui_contrast = low", "ui_motion = slow", "last_game = amiga"}) {
        Settings x;
        std::string e;
        CHECK(!parse_settings(bad, x, e));
    }
}

// What main.cpp's Atari ST branch hands to the ST runner (its RunOptions), by settings key: everything else is the Amiga loop's.
void test_game_applicability() {
    const std::set<std::string> st = {"scale", "fullscreen", "audio", "volume", "audio_latency", "input_debug", "filter", "aspect", "integer",
                                      "curvature", "asset_packs", "upgrade", "interp_scale", "god_mode", "cheat_scores", "autofire",
                                      "autofire_rate", "credits", "high_scores", "modern", "game", "launcher", "ui_text", "ui_contrast",
                                      "ui_motion", "last_game", "packs_enabled"};
    for (const SettingRow& r : schema()) CHECK(applies_to(r, GameKind::St) == (st.count(r.key) == 1));
    // The Amiga disk and CDTV releases: speech and the CD track map differ.
    CHECK(applies_to(*find_row("speech_volume"), GameKind::Cdtv) && !applies_to(*find_row("speech_volume"), GameKind::Disk));
    CHECK(applies_to(*find_row("protect_music"), GameKind::Disk) && !applies_to(*find_row("protect_music"), GameKind::Cdtv));
    CHECK(applies_to(*find_row("music"), GameKind::Cdtv) && applies_to(*find_row("music"), GameKind::Disk));
    // Reasons.
    Settings s;
    CHECK(disabled_reason(*find_row("interp"), s, GameKind::Disk).empty());
    CHECK(!disabled_reason(*find_row("interp"), s, GameKind::St).empty());
    CHECK(!disabled_reason(*find_row("curvature"), s, GameKind::Disk).empty()); // not the crt filter
    s.video_filter = VideoFilter::Crt;
    CHECK(disabled_reason(*find_row("curvature"), s, GameKind::Disk).empty());
    s.faithful = true;
    CHECK(!disabled_reason(*find_row("filter"), s, GameKind::Disk).empty());
    CHECK(disabled_reason(*find_row("scale"), s, GameKind::Disk).empty()); // window options are not picture options
    s.faithful = false;
    CHECK(!disabled_reason(*find_row("smooth_far"), s, GameKind::Disk).empty());
    s.interp = true;
    CHECK(disabled_reason(*find_row("smooth_far"), s, GameKind::Disk).empty());
    s.audio = false;
    CHECK(!disabled_reason(*find_row("volume"), s, GameKind::Disk).empty());
    CHECK(disabled_reason(*find_row("audio"), s, GameKind::Disk).empty());
    CHECK(!is_cheat_active(*find_row("god_mode"), s));
    s.god_mode = true;
    s.credits = 3;
    CHECK(is_cheat_active(*find_row("god_mode"), s) && !is_cheat_active(*find_row("credits"), s));
    s.credits = 4;
    CHECK(is_cheat_active(*find_row("credits"), s));
}

std::string cmd(const Settings& s, GameKind g = GameKind::Disk, bool with_game = false) { return command_line(s, g, with_game).text(); }

void test_command_line_text() {
    Settings s;
    CHECK(command_line(s, GameKind::Disk, false).args.empty());
    CHECK_EQ(cmd(s), "x2recomp");
    CHECK_EQ(cmd(s, GameKind::Disk, true), "x2recomp --game disk");
    CHECK_EQ(cmd(s, GameKind::St, true), "x2recomp --game st");
    s.scale = 4;
    s.fullscreen = true;
    s.video_filter = VideoFilter::Crt;
    s.curvature = true;
    s.aspect = true;
    s.integer = false;
    s.interp = true;
    s.interp_scale = 4;
    s.wide = "16:9";
    s.hd_panel = true;
    s.smooth_far = false;
    CHECK_EQ(cmd(s), "x2recomp --scale 4 --fullscreen --filter crt --curvature --aspect --no-integer --modern interp,scale=4,nosmooth,wide=16:9,hdpanel");
    // The Atari ST honours no interpolation, surround or HD panel: they are left out, the canvas scale stays.
    CHECK_EQ(cmd(s, GameKind::St),
             "x2recomp --scale 4 --fullscreen --filter crt --curvature --aspect --no-integer --modern scale=4");
    // The Faithful switch overrides every picture option, so they are not written.
    s.faithful = true;
    CHECK_EQ(cmd(s), "x2recomp --faithful --scale 4 --fullscreen");
    // Sound.
    Settings a;
    a.audio = false;
    a.volume = 70;
    a.music_volume = 50;
    a.sfx_volume = 20;
    a.speech_volume = 10;
    a.filter = FilterSetting::A1200;
    a.stereo_separation = 0.5f;
    a.audio_latency = 120;
    a.audio_upgrade = false;
    a.music = "cdtv";
    a.cd_crossfade = 100;
    a.cd_pause = "duck";
    CHECK_EQ(cmd(a, GameKind::Cdtv),
             "x2recomp --no-audio --volume 70 --sfx-volume 20 --music-volume 50 --speech-volume 10 --audio-filter a1200 --stereo-sep 0.5 "
             "--audio-latency 120 --music cdtv --cd-crossfade 100 --cd-pause duck --modern noaudio");
    CHECK(cmd(a, GameKind::Disk).find("--speech-volume") == std::string::npos); // the disk release has no speech
    CHECK_EQ(cmd(a, GameKind::St), "x2recomp --no-audio --volume 70 --audio-latency 120");
    // Assists and cheats.
    Settings c;
    c.autofire = true;
    CHECK_EQ(cmd(c), "x2recomp --autofire");
    c.autofire_rate = 9;
    CHECK_EQ(cmd(c), "x2recomp --autofire=9");
    c.autofire = false;
    CHECK_EQ(cmd(c), "x2recomp");
    CHECK(!command_line(c, GameKind::Disk, false).notes.empty()); // the rate alone cannot be said
    c = Settings{};
    c.god_mode = true;
    c.credits = 7;
    c.cheat_scores = true;
    c.high_scores = false;
    CHECK_EQ(cmd(c), "x2recomp --no-persist --god-mode --credits 7 --cheat-scores");
    // Keyboard.
    Settings k;
    k.grab_keyboard = "on";
    k.cursor = "hide";
    k.input_debug = true;
    CHECK_EQ(cmd(k), "x2recomp --grab-keyboard --cursor hide --input-debug");
    k.grab_keyboard = "off";
    CHECK_EQ(cmd(k), "x2recomp --no-grab-keyboard --cursor hide --input-debug");
    CHECK_EQ(cmd(k, GameKind::St), "x2recomp --input-debug");
    // Packs: --asset-pack also turns them on, so the upgrade item is not repeated; paths with spaces are quoted.
    Settings p;
    p.upgrade = true;
    p.asset_packs = "/a/b;/c d/e.zip";
    CHECK_EQ(cmd(p), "x2recomp --asset-pack /a/b --asset-pack \"/c d/e.zip\"");
    p.upgrade = false;
    CHECK(!command_line(p, GameKind::Disk, false).notes.empty());
    Settings q;
    q.upgrade = true;
    CHECK_EQ(cmd(q), "x2recomp --modern upgrade");
    q.packs_enabled = "one;two";
    CHECK(!command_line(q, GameKind::Disk, false).notes.empty()); // packs_enabled is in settings.cfg only
    // The release.
    Settings g;
    g.game = "cdtv";
    CHECK_EQ(cmd(g), "x2recomp --game cdtv");
    CHECK_EQ(cmd(g, GameKind::Cdtv, true), "x2recomp --game cdtv");
}

// The valid non-default values to try for a row, as Settings changes. `more` lets a row set what it needs to be expressible.
struct Variant {
    Settings s;
    std::string what;
};
std::vector<Variant> variants_of(const SettingRow& r) {
    std::vector<Variant> out;
    auto add = [&](Settings s, const std::string& w) { out.push_back({s, std::string(r.key) + "=" + w}); };
    const Settings def;
    switch (r.type) {
    case SettingType::Bool: {
        Settings s = def;
        set_bool(r, s, !get_bool(r, def));
        add(s, "flipped");
        break;
    }
    case SettingType::Int: {
        for (int v : {r.lo, r.hi, (r.lo + r.hi) / 2 + 1}) {
            if (v == get_int(r, def)) continue;
            Settings s = def;
            set_int(r, s, v);
            add(s, std::to_string(v));
        }
        break;
    }
    case SettingType::Float: {
        for (float v : {0.0f, 0.5f}) {
            Settings s = def;
            set_float(r, s, v);
            add(s, std::to_string(v));
        }
        break;
    }
    case SettingType::Choice: {
        for (int i = 0; i < r.nchoices; ++i) {
            if (i == get_choice(r, def)) continue;
            Settings s = def;
            set_choice(r, s, i);
            add(s, r.choices[i].value);
        }
        break;
    }
    case SettingType::Text: {
        if (std::strcmp(r.key, "asset_packs") == 0) {
            Settings s = def;
            s.asset_packs = "/one/pack;/two packs/b.zip";
            s.upgrade = true; // --asset-pack turns the packs on
            add(s, "two paths");
        } else if (std::strcmp(r.key, "cdtv") == 0) {
            Settings s = def;
            s.cdtv = "/cd audio/disc.cue";
            add(s, "path");
        } else if (std::strcmp(r.key, "cdtv_tracks") == 0) {
            Settings s = def;
            s.cdtv_tracks = "4,5,6,7,8,9";
            add(s, "map");
        }
        break;
    }
    }
    // A row that only says something with another one on needs it.
    for (Variant& v : out) {
        if (std::strcmp(r.key, "autofire_rate") == 0) v.s.autofire = true;
    }
    return out;
}

// args -> Options -> Settings, as a run with a clean settings.cfg would see it.
Settings parse_command(const CommandLine& c, bool& ok) {
    std::vector<std::string> toks{"x2recomp"};
    toks.insert(toks.end(), c.args.begin(), c.args.end());
    std::vector<char*> argv;
    for (auto& t : toks) argv.push_back(t.data());
    cli::Options o;
    ok = cli::parse_args(int(argv.size()), argv.data(), o);
    Settings s;
    cli::settings_from_options(o, s);
    return s;
}

// The Settings a game run is expected to see: only what the release honours counts, so a row it ignores keeps its default.
void expect_equal_for_game(const Settings& want, const Settings& got, GameKind g, const std::string& what) {
    for (const SettingRow& r : schema()) {
        if (r.cli.kind == CliKind::None) continue;            // settings.cfg only: the command line cannot carry it
        if (!applies_to(r, g)) continue;                      // not written for this release
        if (want.faithful && r.picture) continue;             // overridden, and so not written
        if (std::strcmp(r.key, "game") == 0) continue;
        const std::string a = value_text(r, want), b = value_text(r, got);
        ++g_checks;
        if (a != b) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL %s (%s): %s says %s, the command line gave %s\n", what.c_str(), game_short_name(g), r.key, a.c_str(), b.c_str());
        }
    }
}

void test_command_line_round_trip() {
    for (const SettingRow& r : schema()) {
        for (const Variant& v : variants_of(r)) {
            for (GameKind g : {GameKind::Disk, GameKind::Cdtv, GameKind::St}) {
                for (bool with_game : {false, true}) {
                    const CommandLine c = command_line(v.s, g, with_game);
                    bool ok = false;
                    const Settings got = parse_command(c, ok);
                    CHECK(ok);
                    if (!ok) std::fprintf(stderr, "    %s: %s\n", v.what.c_str(), c.text().c_str());
                    expect_equal_for_game(v.s, got, g, v.what + " [" + c.text() + "]");
                    if (with_game) CHECK(!c.args.empty() && c.args[0] == "--game" && c.args[1] == game_flag_value(g));
                }
            }
        }
    }
    // Everything at once, and the Faithful switch on top.
    Settings all;
    for (const SettingRow& r : schema())
        for (const Variant& v : variants_of(r))
            if (r.cli.kind != CliKind::None) {
                const std::string t = value_text(r, v.s);
                std::string err;
                set_value_text(r, all, t, err);
            }
    all.upgrade = true;
    all.autofire = true;
    for (GameKind g : {GameKind::Disk, GameKind::Cdtv, GameKind::St}) {
        bool ok = false;
        const CommandLine c = command_line(all, g, true);
        const Settings got = parse_command(c, ok);
        CHECK(ok);
        expect_equal_for_game(all, got, g, "all rows [" + c.text() + "]");
        Settings f = all;
        f.faithful = true;
        const CommandLine cf = command_line(f, g, true);
        const Settings gotf = parse_command(cf, ok);
        CHECK(ok && gotf.faithful);
        expect_equal_for_game(f, gotf, g, "all rows, faithful [" + cf.text() + "]");
    }
}

// The same round trip through the real binary's --print-settings.
std::string run_capture(const std::string& command) {
    const fs::path out = g_dir / "out.txt";
    std::string full = command + " > \"" + out.string() + "\" 2>&1";
#ifdef _WIN32
    full = "\"" + full + "\""; // cmd.exe strips one pair of quotes from the whole line
#endif
    std::system(full.c_str());
    std::ifstream in(out, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void test_print_settings_binary() {
    if (g_exe.empty()) {
        std::printf("  (no executable given: --print-settings through the binary is skipped)\n");
        return;
    }
    const fs::path data = g_dir / "clean-data";
    fs::create_directories(data);
    int n = 0;
    for (const SettingRow& r : schema()) {
        for (const Variant& v : variants_of(r)) {
            if (v.s.faithful) continue;
            const CommandLine c = command_line(v.s, GameKind::Disk, false);
            if (c.args.empty()) continue;
            if (++n > 90) break;
            const std::string text = run_capture("\"" + g_exe + "\" --print-settings --data-dir \"" + data.string() + "\" " + c.text("").substr(1));
            Settings got;
            std::string err;
            CHECK(parse_settings(text, got, err));
            if (!err.empty()) std::fprintf(stderr, "    %s: %s\n", c.text().c_str(), err.c_str());
            expect_equal_for_game(v.s, got, GameKind::Disk, "binary: " + c.text());
        }
    }
    CHECK(n > 50);
    // settings.cfg under the arguments: the file is read, the arguments win, and the file is not written.
    {
        std::ofstream(data / "settings.cfg") << "scale = 5\nfilter = edge\nvolume = 40\nmystery = 1\n";
        const std::string text = run_capture("\"" + g_exe + "\" --print-settings --data-dir \"" + data.string() + "\" --scale 2");
        Settings got;
        std::string err;
        parse_settings(text, got, err);
        CHECK(got.scale == 2 && got.video_filter == VideoFilter::Edge && got.volume == 40);
        std::ifstream f(data / "settings.cfg");
        std::stringstream ss;
        ss << f.rdbuf();
        CHECK_EQ(ss.str(), "scale = 5\nfilter = edge\nvolume = 40\nmystery = 1\n");
    }
}

void test_usage_mentions_every_flag() {
    const fs::path help_path = g_dir / "help.txt";
    std::FILE* f = std::fopen(help_path.string().c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) return;
    cli::usage(f);
    std::fclose(f);
    std::string text;
    {
        std::ifstream in(help_path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    for (const SettingRow& r : schema()) {
        if (r.cli.kind == CliKind::None) continue;
        if (r.cli.kind == CliKind::Map) {
            for (int i = 0; i < r.nchoices; ++i)
                if (r.cli.map_flags[i][0]) CHECK(text.find(r.cli.map_flags[i]) != std::string::npos);
            continue;
        }
        if (r.cli.kind == CliKind::Modern || r.cli.kind == CliKind::ModernVal) {
            for (const char* item : {r.cli.item, r.cli.item_off})
                if (item[0]) {
                    ++g_checks;
                    if (text.find(item) == std::string::npos) {
                        ++g_failures;
                        std::fprintf(stderr, "  FAIL: --modern item '%s' is not in the help text\n", item);
                    }
                }
            continue;
        }
        const std::string flag = std::string("--") + r.cli.flag;
        ++g_checks;
        if (text.find(flag) == std::string::npos) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL: %s (setting %s) is not in the help text\n", flag.c_str(), r.key);
        }
    }
    CHECK(text.find("--print-settings") != std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_exe = argv[1];
    g_dir = fs::temp_directory_path() / "x2_settings_schema_test";
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"schema_complete", test_schema_complete},
        {"schema_values", test_schema_values},
        {"game_applicability", test_game_applicability},
        {"command_line_text", test_command_line_text},
        {"command_line_round_trip", test_command_line_round_trip},
        {"usage_mentions_every_flag", test_usage_mentions_every_flag},
        {"print_settings_binary", test_print_settings_binary},
    };
    for (const T& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-28s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
