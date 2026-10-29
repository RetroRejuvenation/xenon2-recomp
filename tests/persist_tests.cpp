// Persistence unit tests (no game data): high-score file round trip and rejection of short, corrupt, foreign
// and other-version files, unwritable directories, the chip RAM window, and the settings file parser.
//
//   x2_persist_tests WORKDIR
#include "machine.h"
#include "persist.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace x2;
namespace fs = std::filesystem;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

fs::path g_dir;

HiscoreTable sample_table() {
    HiscoreTable t{};
    const char* names[10] = {"AAA", "BBB", "CCC", "DDD", "EEE", "FFF", "GGG", "HHH", "III", "JJJ"};
    for (int i = 0; i < 10; ++i) {
        uint32_t score = 100000u - uint32_t(i) * 5000u;
        for (int b = 0; b < 4; ++b) t[size_t(i * 4 + b)] = uint8_t(score >> (24 - 8 * b));
        char line[16];
        std::snprintf(line, sizeof line, "%2d. %07u %s", i + 1, score, names[i]);
        std::memcpy(&t[size_t(40 + i * 15)], line, 15);
    }
    return t;
}

void write_file(const fs::path& p, const std::vector<uint8_t>& d) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

void test_crc_and_hash() {
    const uint8_t s[] = "123456789";
    CHECK(crc32(s, 9) == 0xCBF43926u); // the standard CRC-32 check value
    CHECK(fnv1a64(s, 0) == 0xcbf29ce484222325ull);
}

void test_round_trip() {
    HiscoreTable t = sample_table(), u{};
    std::string why;
    fs::path p = g_dir / "highscores.bin";
    CHECK(save_hiscore_file(p.string(), 0x1234, t, why));
    CHECK(fs::file_size(p) == 20 + kHiscoreSize + 4);
    CHECK(!fs::exists(p.string() + ".tmp"));
    CHECK(load_hiscore_file(p.string(), 0x1234, u, why));
    CHECK(t == u);
    // Overwriting replaces the old table.
    t[0] = 0x7F;
    CHECK(save_hiscore_file(p.string(), 0x1234, t, why));
    CHECK(load_hiscore_file(p.string(), 0x1234, u, why) && u[0] == 0x7F);
}

void test_rejections() {
    HiscoreTable t = sample_table(), u{};
    std::vector<uint8_t> good = encode_hiscores(t, 42);
    std::string why;
    CHECK(decode_hiscores(good, 42, u, why));
    CHECK(!decode_hiscores(good, 43, u, why) && why.find("different game image") != std::string::npos);
    std::vector<uint8_t> v = good;
    v[7] = 2; // version 2
    CHECK(!decode_hiscores(v, 42, u, why) && why.find("version") != std::string::npos);
    v = good;
    v.resize(v.size() - 10);
    CHECK(!decode_hiscores(v, 42, u, why));
    CHECK(!decode_hiscores({}, 42, u, why));
    v = good;
    v[30] ^= 1; // payload bit flip
    CHECK(!decode_hiscores(v, 42, u, why) && why == "CRC mismatch");
    v = good;
    v[0] = 'Y';
    CHECK(!decode_hiscores(v, 42, u, why));
    // A valid CRC over a table with a character the game cannot draw.
    HiscoreTable bad = t;
    bad[60] = 0x01;
    CHECK(!decode_hiscores(encode_hiscores(bad, 42), 42, u, why) && why.find("character") != std::string::npos);
    // Files on disk: too short, and missing.
    fs::path p = g_dir / "short.bin";
    write_file(p, {'X', '2', 'H', 'S', 0, 0});
    CHECK(!load_hiscore_file(p.string(), 42, u, why));
    CHECK(!load_hiscore_file((g_dir / "missing.bin").string(), 42, u, why) && why == "no saved table");
}

void test_unwritable() {
    std::string why;
    fs::path p = g_dir / "no-such-dir" / "deeper" / "highscores.bin";
    CHECK(!save_hiscore_file(p.string(), 1, sample_table(), why));
    CHECK(!why.empty());
}

void test_machine_window() {
    Machine m;
    HiscoreTable t = sample_table();
    write_hiscores(m, t);
    CHECK(m.chip[0x891D] == 0 && m.chip[0x89DC] == 0);
    CHECK(std::memcmp(&m.chip[0x8946], " 1. 0100000 AAA", 15) == 0);
    CHECK(read_hiscores(m) == t);
}

void test_settings() {
    Settings s;
    std::string err;
    CHECK(parse_settings(format_settings(s), s, err) && err.empty());
    Settings d;
    CHECK(s.scale == d.scale && s.volume == d.volume && s.audio == d.audio && s.filter == d.filter &&
          s.stereo_separation == d.stereo_separation && s.high_scores == d.high_scores);
    CHECK(parse_settings("scale = 4\nfullscreen = on # comment\naspect=yes\naudio = off\naudio_filter = a1200\n"
                         "stereo_separation = 0.5\nvolume = 70\nhigh_scores = off\n",
                         s, err));
    CHECK(s.scale == 4 && s.fullscreen && s.aspect && !s.audio && s.filter == FilterSetting::A1200 &&
          s.stereo_separation == 0.5f && s.volume == 70 && !s.high_scores);
    Settings r;
    CHECK(parse_settings(format_settings(s), r, err));
    CHECK(r.scale == 4 && r.fullscreen && r.aspect && !r.audio && r.filter == FilterSetting::A1200 &&
          r.stereo_separation == 0.5f && r.volume == 70 && !r.high_scores);
    // Bad lines are reported, valid ones still applied, out-of-range values ignored.
    Settings b;
    err.clear();
    CHECK(!parse_settings("volume = 150\nscale = 2\nbogus = 1\naudio_filter = led\njunk\n", b, err));
    CHECK(b.volume == 100 && b.scale == 2 && b.filter == FilterSetting::A500);
    CHECK(err.find("line 1") != std::string::npos && err.find("line 3") != std::string::npos &&
          err.find("line 4") != std::string::npos && err.find("line 5") != std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    g_dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_persist_test";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"crc_and_hash", test_crc_and_hash}, {"round_trip", test_round_trip}, {"rejections", test_rejections},
        {"unwritable", test_unwritable},     {"machine_window", test_machine_window}, {"settings", test_settings},
    };
    for (const T& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%-16s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
