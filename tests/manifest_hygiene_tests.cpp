// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
//
// manifest.txt hygiene (runtime/manifest_text.h): one parser with one rule for repeated keys, values that cannot start a new
// line, and a start-up re-import that only follows an ordinary file that is still there. Synthetic data only.
//
//   x2_manifest_hygiene_tests SCRATCH_DIR
#include "import/importer.h"
#include "manifest_text.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace x2;

namespace {
int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void spit(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

void test_parser() {
    const ManifestText a = parse_manifest_text("# comment\nimporter_version=3\r\nsource=/a.adf\nsource=/b.adf\nbuild=x\n\nnoequals\nvariant=\n");
    CHECK(a.consistent());
    CHECK(a.kv.size() == 3 && a.kv.at("importer_version") == "3" && a.kv.at("build") == "x" && a.kv.at("variant").empty());
    CHECK(a.sources.size() == 2 && a.sources[0] == "/a.adf" && a.sources[1] == "/b.adf");
    const ManifestText b = parse_manifest_text("build=disk\nvariant=v\nbuild=cdtv\n");
    CHECK(!b.consistent() && b.repeated == "build");
    CHECK(b.kv.at("build") == "disk"); // the first value is kept; callers refuse the file anyway
    CHECK(parse_manifest_text("").consistent());
    CHECK(parse_manifest_text("no newline at the end=1").kv.at("no newline at the end") == "1");
    CHECK(manifest_value("a\nb=c") == "a\\x0ab=c");
    CHECK(is_network_path("\\\\server\\share\\x.adf") && is_network_path("//server/share/x.adf") && !is_network_path("/home/u/x.adf") &&
          !is_network_path("C:\\games\\x.adf") && !is_network_path("x.adf"));
}

import::Result evil_result() {
    import::Result r;
    r.status = import::Status::Ok;
    r.game_image.assign(0x3FC00, 0x11);
    r.disc2.assign(901120, 0x22);
    r.sources = {"/safe/one.adf", std::string("/evil\nsource=//attacker/share/x.adf"), "/safe/two disk.zip"};
    r.variant_id = "test-variant";
    r.variant_name = "Name\nsource=//attacker/share/y.adf\nbuild=cdtv\nimporter_version=99";
    r.pressing_id = "p";
    r.container = "adf";
    r.disc1_what = "volume \"X\"\r\nvariant=other";
    r.disc2_what = "disc2";
    return r;
}

void test_writer(const fs::path& root) {
    const fs::path dir = root / "writer";
    fs::create_directories(dir);
    import::Result r = evil_result();
    std::string err;
    CHECK(import::write_assets(dir.string(), r, err));
    const ManifestText m = parse_manifest_text(slurp(dir / "manifest.txt"));
    CHECK(m.consistent());                                    // the injected duplicate keys did not become lines
    CHECK(m.sources.size() == 2);                             // the source with a line break was not recorded
    CHECK(m.sources.size() == 2 && m.sources[0] == "/safe/one.adf" && m.sources[1] == "/safe/two disk.zip");
    CHECK(m.kv.count("build") == 0);                          // `build=cdtv` inside the name stayed inside the name
    CHECK(m.kv.at("importer_version") == std::to_string(import::kImporterVersion));
    CHECK(m.kv.at("variant_name").find("\\x0a") != std::string::npos && m.kv.at("variant_name").find('\n') == std::string::npos);
    CHECK(m.kv.at("disc1").find("\\x0d\\x0a") != std::string::npos);
    CHECK(m.kv.at("variant") == "test-variant");

    // A manifest that repeats a key is refused by the reader, whatever else is right with the folder.
    spit(dir / "manifest.txt", slurp(dir / "manifest.txt") + "importer_version=" + std::to_string(import::kImporterVersion) + "\n");
    std::string reason;
    CHECK(!import::verify_assets(dir.string(), reason));
    CHECK(reason.find("repeats the key importer_version") != std::string::npos);
}

void test_ensure_imported(const fs::path& root) {
    const fs::path dir = root / "reimport";
    fs::create_directories(dir);
    const std::string head = "importer_version=" + std::to_string(import::kImporterVersion) + "\n";
    // The game files are missing, so the folder fails verification and the recorded sources are looked at.
    struct Case { std::string source; const char* expect; };
    const fs::path folder = root / "a_folder";
    fs::create_directories(folder);
    const fs::path file = root / "junk.adf";
    spit(file, std::string(1000, 'x'));
    const std::vector<Case> cases = {
        {folder.string(), "is a folder"},
        {"\\\\server\\share\\x.adf", "network path"},
        {"//server/share/x.adf", "network path"},
        {(root / "gone.adf").string(), "is gone"},
        {std::string("/tmp/x\x1b[2J.adf"), "control characters"},
        {file.string(), "re-import from the recorded sources failed"}, // an ordinary file is still tried
    };
    for (const Case& c : cases) {
        spit(dir / "manifest.txt", head + "source=" + c.source + "\n");
        std::string reason;
        const bool ok = import::ensure_imported(dir.string(), reason);
        CHECK(!ok);
        if (reason.find(c.expect) == std::string::npos) std::fprintf(stderr, "  expected \"%s\" in: %s\n", c.expect, reason.c_str());
        CHECK(reason.find(c.expect) != std::string::npos);
    }
    // One bad source anywhere in the list stops the re-import, even when another one is fine.
    spit(dir / "manifest.txt", head + "source=" + file.string() + "\nsource=" + folder.string() + "\n");
    std::string reason;
    CHECK(!import::ensure_imported(dir.string(), reason) && reason.find("is a folder") != std::string::npos);
    // A CRLF manifest still trips the "patched" guard.
    spit(dir / "manifest.txt", head + "source=" + file.string() + "\r\npatched=true\r\n");
    reason.clear();
    CHECK(!import::ensure_imported(dir.string(), reason) && reason.find("protection patch") != std::string::npos);
}
} // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_manifest_hygiene";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    test_parser();
    test_writer(root);
    test_ensure_imported(root);
    fs::remove_all(root, ec);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
