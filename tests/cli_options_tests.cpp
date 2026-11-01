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

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#define X2_CHDIR _chdir
#else
#include <unistd.h>
#define X2_CHDIR chdir
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
    };
    for (const T& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-22s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
