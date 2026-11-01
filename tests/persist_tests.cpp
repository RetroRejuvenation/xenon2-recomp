// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
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
    // The live output's buffer: default 40 ms, 10..500, round trip, out-of-range ignored.
    Settings l;
    err.clear();
    CHECK(l.audio_latency == 40);
    CHECK(parse_settings("audio_latency = 120\n", l, err) && l.audio_latency == 120);
    Settings lr;
    CHECK(parse_settings(format_settings(l), lr, err) && lr.audio_latency == 120);
    CHECK(!parse_settings("audio_latency = 5\n", l, err) && l.audio_latency == 120);
    CHECK(!parse_settings("audio_latency = 501\n", l, err) && l.audio_latency == 120);
    // protect_music: off by default (the original silences a voice of the tune under heavy fire), round trip.
    CHECK(!l.protect_music);
    err.clear();
    CHECK(parse_settings("protect_music = on\n", l, err) && l.protect_music && err.empty());
    Settings pr;
    CHECK(parse_settings(format_settings(l), pr, err) && pr.protect_music);
    CHECK(parse_settings("protect_music = off\n", l, err) && !l.protect_music);
    CHECK(!parse_settings("protect_music = maybe\n", l, err) && !l.protect_music);
    // game: which imported release starts (auto = the disk release if imported, else CDTV); round trip, bad values rejected.
    Settings gm;
    CHECK(gm.game == "auto");
    err.clear();
    CHECK(parse_settings("game = cdtv\n", gm, err) && gm.game == "cdtv" && err.empty());
    Settings gr;
    CHECK(parse_settings(format_settings(gm), gr, err) && gr.game == "cdtv");
    CHECK(!parse_settings("game = amiga\n", gm, err) && gm.game == "cdtv");
    // Modernisation keys: defaults are the faithful picture; round trip; bad values rejected.
    CHECK(!d.faithful && !d.interp && d.video_filter == VideoFilter::Nearest && d.integer && !d.curvature);
    Settings m;
    err.clear();
    CHECK(parse_settings("modern = faithful\ninterp = on\ninterp_scale = 4\nsmooth_far = off\nfilter = crt\n"
                         "integer = off\ncurvature = on\n",
                         m, err));
    Settings mr;
    CHECK(parse_settings(format_settings(m), mr, err) && err.empty());
    CHECK(mr.faithful && mr.interp && mr.interp_scale == 4 && !mr.smooth_far && mr.video_filter == VideoFilter::Crt &&
          !mr.integer && mr.curvature);
    for (int f = 0; f < int(VideoFilter::kCount); ++f) {
        VideoFilter v = VideoFilter::Nearest;
        CHECK(parse_video_filter(video_filter_name(VideoFilter(f)), v) && v == VideoFilter(f));
    }
    Settings mb;
    err.clear();
    CHECK(!parse_settings("modern = shiny\nfilter = blur\ninterp_scale = 9\n", mb, err));
    CHECK(!mb.faithful && mb.video_filter == VideoFilter::Nearest && mb.interp_scale == 3);
    // Upgraded graphics (Phases 9d-9f): off by default; round trip; bad surround widths rejected.
    CHECK(!d.upgrade && d.asset_packs.empty() && d.wide == "off" && !d.hd_panel);
    Settings u;
    err.clear();
    CHECK(parse_settings("upgrade = on\nasset_packs = packs/a;b.zip\nwide = 21:9\nhd_panel = on\n", u, err));
    Settings ur;
    CHECK(parse_settings(format_settings(u), ur, err) && err.empty());
    CHECK(ur.upgrade && ur.asset_packs == "packs/a;b.zip" && ur.wide == "21:9" && ur.hd_panel);
    for (const char* w : {"wide = 40", "wide = off", "wide = 16:10"}) CHECK(parse_settings(w, ur, err));
    for (const char* w : {"wide = 400", "wide = wide", "wide = 16:", "wide = 0:9"}) {
        Settings x;
        std::string e;
        CHECK(!parse_settings(w, x, e) && x.wide == "off");
    }
}


bool same_settings(const Settings& a, const Settings& b) { return format_settings(a) == format_settings(b); }

void test_launcher_keys() {
    Settings d, s;
    std::string err;
    CHECK(d.launcher == "auto" && d.ui_text == "normal" && d.ui_contrast == "normal" && d.ui_motion == "auto" && d.last_game.empty());
    CHECK(parse_settings("launcher = never\nui_text = large\nui_contrast = high\nui_motion = reduced\nlast_game = st\n", s, err) && err.empty());
    CHECK(s.launcher == "never" && s.ui_text == "large" && s.ui_contrast == "high" && s.ui_motion == "reduced" && s.last_game == "st");
    Settings r;
    CHECK(parse_settings(format_settings(s), r, err) && err.empty() && same_settings(r, s));
    Settings e;
    CHECK(parse_settings("last_game =\n", e, err) && e.last_game.empty()); // empty before the first run
    for (const char* bad : {"launcher = x", "ui_text = x", "ui_contrast = x", "ui_motion = x", "last_game = x"}) {
        Settings x;
        std::string er;
        CHECK(!parse_settings(bad, x, er) && same_settings(x, d));
    }
}

void test_merge() {
    // A file somebody edited: comments, their own spacing, a spelling the writer would not use, a key from another version.
    const std::string old = "# my notes\nscale = 4   # window size\nfullscreen = yes\nmystery = 1\nvolume=70\n\n# end\n";
    Settings s;
    std::string err;
    parse_settings(old, s, err);
    CHECK(s.scale == 4 && s.fullscreen && s.volume == 70);
    // Nothing changed: every line the file has stays exactly as it is, the keys it lacks are appended.
    const std::string same = merge_settings_text(old, s);
    CHECK(same.rfind("# my notes\nscale = 4   # window size\nfullscreen = yes\nmystery = 1\nvolume=70\n\n# end\n", 0) == 0);
    CHECK(same.find("# Settings this version added:") != std::string::npos && same.find("launcher = auto") != std::string::npos);
    CHECK(same.find("\nscale = ", same.find("# Settings this version added:")) == std::string::npos); // not twice
    Settings back;
    std::string e2;
    parse_settings(same, back, e2);
    CHECK(same_settings(back, s));
    CHECK(e2.find("mystery") != std::string::npos && e2.find("line 4") != std::string::npos); // reported, not dropped
    // A change replaces the value, keeps the comment and the layout.
    s.scale = 6;
    s.volume = 20;
    s.filter = FilterSetting::A1200;
    const std::string changed = merge_settings_text(old, s);
    CHECK(changed.find("# my notes\nscale = 6  # window size\nfullscreen = yes\nmystery = 1\nvolume = 20\n\n# end\n") == 0);
    CHECK(changed.find("audio_filter = a1200") != std::string::npos);
    Settings back2;
    parse_settings(changed, back2, e2);
    CHECK(same_settings(back2, s));
    // Merging again changes nothing.
    CHECK(merge_settings_text(changed, s) == changed);
    // No file: the usual text.
    CHECK(merge_settings_text("", s) == format_settings(s));
    // Duplicate keys: the last one is the one that counts, so it is the one that changes.
    Settings dup;
    parse_settings("scale = 2\nscale = 3\n", dup, err);
    CHECK(dup.scale == 3);
    dup.scale = 7;
    CHECK(merge_settings_text("scale = 2\nscale = 3\n", dup).rfind("scale = 2\nscale = 7\n", 0) == 0);
    // A value the file refuses is replaced by what is in use.
    Settings bad;
    parse_settings("volume = 500\n", bad, err);
    CHECK(merge_settings_text("volume = 500\n", bad).rfind("volume = 100\n", 0) == 0);
    // CRLF files are read, and written back with LF.
    Settings crlf;
    parse_settings("scale = 2\r\nvolume = 10\r\n", crlf, err);
    CHECK(crlf.scale == 2 && crlf.volume == 10);
    CHECK(merge_settings_text("scale = 2\r\nvolume = 10\r\n", crlf).rfind("scale = 2\nvolume = 10\n", 0) == 0);
    // Every key written by the other spelling of a bool or a wide width is "the same" and is left alone.
    Settings w;
    parse_settings("wide = 0\nhd_panel = 1\n", w, err);
    CHECK(merge_settings_text("wide = 0\nhd_panel = 1\n", w).rfind("wide = 0\nhd_panel = 1\n", 0) == 0);
}

void test_save_file() {
    const fs::path path = g_dir / "settings.cfg";
    std::string why;
    Settings s;
    s.scale = 5;
    CHECK(save_settings_file(path.string(), s, why) && why.empty());
    Settings r;
    std::string text, err;
    CHECK(load_settings_file(path.string(), r, text, err) && err.empty() && r.scale == 5);
    CHECK(text == format_settings(s));
    // A newer version's key and the user's own comments survive a save.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << "future_option = 12  # from a newer x2recomp\n";
    }
    s.volume = 33;
    CHECK(save_settings_file(path.string(), s, why));
    Settings r2;
    CHECK(load_settings_file(path.string(), r2, text, err) && r2.volume == 33 && r2.scale == 5);
    CHECK(text.find("future_option = 12  # from a newer x2recomp") != std::string::npos);
    CHECK(err.find("future_option") != std::string::npos);
    // The temporary file does not stay behind, and the file is whole.
    CHECK(!fs::exists(path.string() + ".tmp"));
    // A directory that does not exist: refused with a reason, nothing created.
    const fs::path bad = g_dir / "no-such-dir" / "settings.cfg";
    CHECK(!save_settings_file(bad.string(), s, why) && !why.empty() && !fs::exists(bad));
    Settings none;
    CHECK(!load_settings_file(bad.string(), none, text, err));
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
        {"launcher_keys", test_launcher_keys}, {"merge", test_merge}, {"save_file", test_save_file},
    };
    for (const T& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%-16s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
