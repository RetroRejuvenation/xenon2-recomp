// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The command-line and settings parser (runtime/cli_options.*), no game data.
//
//   x2_cli_options_tests SOURCE_DIR [WORKDIR]
//   x2_cli_options_tests SOURCE_DIR --update      rewrite tests/cli/parse_baseline.txt from the current parser
//
// parse_equivalence replays every case of tests/cli/parse_cases.txt (each flag spelling, good and bad values, settings under
// arguments) through the parser and compares what it produced with tests/cli/parse_baseline.txt, which was made by the parser as
// it stood in main.cpp before the move to cli_options.cpp. A deliberate change of behaviour has to change the baseline in the same
// commit, where a reviewer sees it.
#include "cli_options.h"
#include "persist.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define X2_CHDIR _chdir
#define X2_DUP _dup
#define X2_DUP2 _dup2
#define X2_CLOSE _close
#define X2_FILENO _fileno
#else
#include <unistd.h>
#define X2_CHDIR chdir
#define X2_DUP dup
#define X2_DUP2 dup2
#define X2_CLOSE close
#define X2_FILENO fileno
#endif

using namespace x2;
using namespace x2::cli;
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

fs::path g_dir;

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        v.push_back(l);
    }
    while (!v.empty() && v.back().empty()) v.pop_back();
    return v;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Runs one case line; returns its block of the baseline ("case N: ...", "result=...", then the fields that differ from a default Options).
std::string run_case(int n, const std::string& line, const std::vector<std::string>& defaults) {
    std::string settings_text, args = line;
    bool with_settings = false;
    if (line.rfind("with-settings:", 0) == 0) {
        with_settings = true;
        const std::string rest = line.substr(14);
        const auto dd = rest.find(" -- ");
        settings_text = dd == std::string::npos ? rest : rest.substr(0, dd);
        args = dd == std::string::npos ? "" : rest.substr(dd + 4);
        for (char& c : settings_text)
            if (c == '@') c = '\n';
    } else if (line.rfind("args:", 0) == 0) {
        args = line.substr(5);
    }
    std::vector<std::string> toks{"x2recomp"};
    std::istringstream ts(args);
    for (std::string t; ts >> t;) toks.push_back(t);
    std::vector<char*> argv;
    for (auto& t : toks) argv.push_back(t.data());
    Options o;
    if (with_settings) {
        Settings s;
        std::string err;
        parse_settings(settings_text, s, err);
        apply_settings(s, o);
    }
    const bool ok = parse_args(int(argv.size()), argv.data(), o);
    std::string out = "case " + std::to_string(n) + ": " + line + "\nresult=" + (ok ? "ok" : "fail") + "\n";
    for (const std::string& l : split_lines(describe_options(o))) {
        bool same = false;
        for (const std::string& d : defaults)
            if (d == l) { same = true; break; }
        if (!same) out += l + "\n";
    }
    return out + "\n";
}

std::string run_all_cases(const std::string& cases_text, const std::string& header) {
    const std::vector<std::string> defaults = split_lines(describe_options(Options{}));
    std::string out = header;
    int n = 0;
    for (const std::string& line : split_lines(cases_text)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("args:", 0) != 0 && line.rfind("with-settings:", 0) != 0) continue;
        out += run_case(++n, line, defaults);
    }
    return out;
}

const char* kBaselineHeader =
    "# Baseline of tests/cli/parse_cases.txt made with the parser as it was in runtime/main.cpp before it moved to cli_options.cpp.\n"
    "# Per case: the result and every Options field that differs from a default-constructed Options (name=value).\n\n";

void test_parse_equivalence() {
    const std::string cases = read_file("tests/cli/parse_cases.txt");
    const std::string baseline = read_file("tests/cli/parse_baseline.txt");
    CHECK(!cases.empty() && !baseline.empty());
    const std::vector<std::string> want = split_lines(baseline), got = split_lines(run_all_cases(cases, kBaselineHeader));
    CHECK(got.size() == want.size());
    size_t shown = 0;
    std::string current_case;
    for (size_t i = 0; i < want.size() && i < got.size(); ++i) {
        if (want[i].rfind("case ", 0) == 0) current_case = want[i];
        if (want[i] != got[i]) {
            CHECK(want[i] == got[i]);
            if (shown++ < 5)
                std::fprintf(stderr, "    %s\n      baseline: %s\n      parser:   %s\n", current_case.c_str(), want[i].c_str(), got[i].c_str());
        }
    }
    CHECK(shown == 0);
}


std::string capture_stderr(const std::function<void()>& fn) {
    const fs::path p = g_dir / "stderr.txt";
    std::fflush(stderr);
    std::FILE* tmp = std::fopen(p.string().c_str(), "wb");
    if (!tmp) { fn(); return std::string(); }
    const int saved = X2_DUP(X2_FILENO(stderr));
    X2_DUP2(X2_FILENO(tmp), X2_FILENO(stderr));
    fn();
    std::fflush(stderr);
    X2_DUP2(saved, X2_FILENO(stderr));
    X2_CLOSE(saved);
    std::fclose(tmp);
    return read_file(p.string());
}

bool parse_tokens(const std::vector<std::string>& args, Options& o) {
    std::vector<std::string> toks{"x2recomp"};
    toks.insert(toks.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& t : toks) argv.push_back(t.data());
    return parse_args(int(argv.size()), argv.data(), o);
}

// Every flag the help text names is a flag the parser knows (a flag renamed or dropped without its help line being updated).
void test_help_flags_are_parsed() {
    const fs::path help_path = g_dir / "help.txt";
    std::FILE* f = std::fopen(help_path.string().c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) return;
    usage(f);
    std::fclose(f);
    const std::string text = read_file(help_path.string());
    std::set<std::string> flags;
    for (size_t i = 0; i + 2 < text.size(); ++i) {
        if (text[i] != '-' || text[i + 1] != '-' || !std::isalnum(static_cast<unsigned char>(text[i + 2]))) continue;
        if (i > 0 && (text[i - 1] == '-' || std::isalnum(static_cast<unsigned char>(text[i - 1])))) continue;
        size_t j = i + 2;
        while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '-')) ++j;
        std::string flag = text.substr(i, j - i);
        while (!flag.empty() && flag.back() == '-') flag.pop_back();
        flags.insert(flag);
    }
    CHECK(flags.size() > 100);
    for (const std::string& flag : flags) {
        Options o;
        const std::string err = capture_stderr([&] { parse_tokens({flag, "1:x", "1"}, o); });
        ++g_checks;
        if (err.find("unknown option " + flag + "\n") != std::string::npos) {
            ++g_failures;
            std::fprintf(stderr, "  FAIL: the help text names %s and the parser does not know it\n", flag.c_str());
        }
    }
    CHECK(flags.count("--print-settings") == 1);
}

// --modern threads=N arrived with the parallel presenter after the baseline was made, so it is checked here.
void test_modern_threads() {
    Options o;
    CHECK(parse_tokens({"--modern", "threads=4"}, o) && o.present.render_threads == 4 && !o.custom_given);
    Options z;
    CHECK(parse_tokens({"--modern=threads=0,interp"}, z) && z.present.render_threads == 0 && z.present.interp);
    Options big;
    CHECK(parse_tokens({"--modern", "threads=999"}, big) && big.present.render_threads == 64);
    Options neg;
    CHECK(parse_tokens({"--modern", "threads=-3"}, neg) && neg.present.render_threads == 0);
}

// settings.cfg -> Options -> settings.cfg gives back what was loaded, for everything Options holds.
void test_settings_round_trip() {
    Settings d;
    {
        Options o;
        apply_settings(d, o);
        Settings back = d;
        back.scale = 7; // overwritten from the options
        settings_from_options(o, back);
        CHECK(format_settings(back) == format_settings(d));
    }
    Settings s;
    s.scale = 5;
    s.fullscreen = true;
    s.aspect = true;
    s.audio = false;
    s.filter = FilterSetting::A1200;
    s.stereo_separation = 0.25f;
    s.volume = 37;
    s.audio_latency = 90;
    s.high_scores = false;
    s.grab_keyboard = "on";
    s.cursor = "hide";
    s.input_debug = true;
    s.faithful = true;
    s.interp = true;
    s.interp_scale = 6;
    s.smooth_far = false;
    s.video_filter = VideoFilter::Edge;
    s.integer = false;
    s.curvature = true;
    s.upgrade = true;
    s.asset_packs = "/p/one;/p two/two.zip";
    s.audio_upgrade = false;
    s.packs_enabled = "alpha;beta";
    s.wide = "21:9";
    s.hd_panel = true;
    s.music = "mix";
    s.music_volume = 11;
    s.sfx_volume = 22;
    s.speech_volume = 33;
    s.protect_music = true;
    s.cd_crossfade = 250;
    s.cd_pause = "stop";
    s.cdtv = "/cd/disc.cue";
    s.cdtv_tracks = "4,5,6,7,8,9";
    s.autofire = true;
    s.autofire_rate = 9;
    s.god_mode = true;
    s.credits = 8;
    s.cheat_scores = true;
    s.game = "st";
    s.launcher = "never";
    s.ui_text = "large";
    s.ui_contrast = "high";
    s.ui_motion = "reduced";
    s.last_game = "cdtv";
    Options o;
    apply_settings(s, o);
    Settings back = s; // the keys Options does not hold keep their value
    back.scale = 1;
    settings_from_options(o, back);
    CHECK(format_settings(back) == format_settings(s));
    // The command line on top changes only what it names.
    Options o2;
    apply_settings(s, o2);
    CHECK(parse_tokens({"--scale", "2", "--no-audio"}, o2));
    Settings top = s;
    settings_from_options(o2, top);
    CHECK(top.scale == 2 && !top.audio && top.volume == 37 && top.video_filter == VideoFilter::Edge);
    // Values the command line takes and the file would refuse are brought into the file's range.
    Options o3;
    CHECK(parse_tokens({"--scale", "12", "--stereo-sep", "3", "--cursor", "on"}, o3));
    Settings clamp;
    settings_from_options(o3, clamp);
    CHECK(clamp.scale == 8 && clamp.stereo_separation == 1.0f && clamp.cursor == "show");
    Options o4;
    CHECK(parse_tokens({"--cursor", "off"}, o4));
    settings_from_options(o4, clamp);
    CHECK(clamp.cursor == "hide");
    std::string err;
    Settings parsed;
    CHECK(parse_settings(format_settings(clamp), parsed, err) && err.empty()); // what --print-settings writes is a file the parser reads
    // --print-settings is a flag of its own and changes nothing else.
    Options p;
    CHECK(parse_tokens({"--print-settings"}, p) && p.print_settings);
    p.print_settings = false;
    Options blank;
    CHECK(describe_options(p) == describe_options(blank));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: x2_cli_options_tests SOURCE_DIR [WORKDIR | --update]\n");
        return 2;
    }
    if (X2_CHDIR(argv[1]) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", argv[1]);
        return 2;
    }
    if (argc > 2 && std::strcmp(argv[2], "--update") == 0) {
        std::ofstream("tests/cli/parse_baseline.txt", std::ios::binary) << run_all_cases(read_file("tests/cli/parse_cases.txt"), kBaselineHeader);
        std::printf("tests/cli/parse_baseline.txt rewritten\n");
        return 0;
    }
    g_dir = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path() / "x2_cli_options_test";
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"parse_equivalence", test_parse_equivalence},
        {"help_flags_are_parsed", test_help_flags_are_parsed},
        {"modern_threads", test_modern_threads},
        {"settings_round_trip", test_settings_round_trip},
    };
    for (const T& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-22s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
