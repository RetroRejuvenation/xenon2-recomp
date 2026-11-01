// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Asset packs v2, workstream 1a (runtime/pack/pack_api.h): the contract and its plumbing, on synthetic data only. Hash and
// key conventions (the same bytes in C++ and in the Python tools), game scoping, the null library, the settings struct and
// its helpers, the new settings.cfg keys (including files written before they existed), and the per-game layout tables.
//
//   x2_pack_tests
#include "cdtv/cdtv_game.h"
#include "cdtv/cdtv_music.h"
#include "modern/layout.h"
#include "modern/present.h"
#include "pack/pack_api.h"
#include "persist.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace x2;

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

std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// Reference values computed independently (FNV-1a 64: offset cbf29ce484222325, prime 100000001b3).
void test_hashing() {
    CHECK(pack::hash_hex(pack::fnv1a64(nullptr, 0)) == "cbf29ce484222325");
    const auto a = bytes_of("a"), foobar = bytes_of("foobar");
    CHECK(pack::hash_hex(pack::fnv1a64(a.data(), a.size())) == "af63dc4c8601ec8c");
    CHECK(pack::hash_hex(pack::fnv1a64(foobar.data(), foobar.size())) == "85944171f73967e8");
    CHECK(pack::hash_hex(0x1) == "0000000000000001");
    // Chaining: hashing in two pieces equals hashing the whole.
    CHECK(pack::fnv1a64(foobar.data() + 3, 3, pack::fnv1a64(foobar.data(), 3)) == pack::fnv1a64(foobar.data(), 6));

    // block hash: w and h as u16 LE, then the index bytes.
    const uint8_t idx[6] = {1, 2, 3, 4, 5, 6};
    CHECK(pack::hash_hex(pack::block_hash(3, 2, idx)) == "053640826b88a443");
    // the same block cut out of a frame with a larger stride
    uint8_t frame[16];
    for (int i = 0; i < 16; ++i) frame[i] = uint8_t(i);
    CHECK(pack::hash_hex(pack::block_hash_region(frame, 4, 1, 1, 2, 2)) == "41c2e3b4d0929831");
    const uint8_t cut[4] = {5, 6, 9, 10};
    CHECK(pack::block_hash(2, 2, cut) == pack::block_hash_region(frame, 4, 1, 1, 2, 2));
    // size is part of the key: the same bytes as 6x1 differ from 3x2
    CHECK(pack::block_hash(6, 1, idx) != pack::block_hash(3, 2, idx));

    // sample key: bytes as u32 LE, then the sample bytes; "<hash>-<bytes>"
    const uint8_t samp[8] = {0x7f, 0x80, 0, 1, 2, 3, 0xff, 0xfe};
    CHECK(pack::sample_key(samp, 8) == "19a81be8ca503d1b-8");
    CHECK(pack::panel_key(0x0123456789abcdefull, 3) == "0123456789abcdef-3");
}

void test_key_syntax() {
    CHECK(pack::is_hash_hex("0123456789abcdef"));
    CHECK(!pack::is_hash_hex("0123456789ABCDEF"));   // lowercase only
    CHECK(!pack::is_hash_hex("0123456789abcde"));
    CHECK(!pack::is_hash_hex("0123456789abcdeg"));
    CHECK(pack::valid_graphic_key("0048848ccf64d966-16x12"));
    CHECK(pack::valid_graphic_key("0048848ccf64d966-4096x1"));
    CHECK(!pack::valid_graphic_key("0048848ccf64d966-0x12"));
    CHECK(!pack::valid_graphic_key("0048848ccf64d966-4097x12"));
    CHECK(!pack::valid_graphic_key("0048848ccf64d966-16x"));
    CHECK(!pack::valid_graphic_key("0048848ccf64d966_16x12"));
    CHECK(!pack::valid_graphic_key("0048848ccf64d966-016x12"));  // no leading zeros
    CHECK(!pack::valid_graphic_key(""));
    CHECK(pack::valid_flash_key("f:0048848ccf64d966-16x12"));
    CHECK(!pack::valid_flash_key("0048848ccf64d966-16x12"));
    CHECK(!pack::valid_flash_key("f:"));
    CHECK(pack::valid_sample_key("19a81be8ca503d1b-8"));
    CHECK(pack::valid_sample_key("19a81be8ca503d1b-131070"));
    CHECK(!pack::valid_sample_key("19a81be8ca503d1b-0"));
    CHECK(!pack::valid_sample_key("19a81be8ca503d1b-"));
    CHECK(!pack::valid_sample_key("19a81be8ca503d1b-99999999999"));
    CHECK(pack::valid_panel_key("0123456789abcdef-1"));
    CHECK(pack::valid_panel_key("0123456789abcdef-8"));
    CHECK(!pack::valid_panel_key("0123456789abcdef-9"));
    CHECK(!pack::valid_panel_key("0123456789abcdef-0"));
}

void test_scope() {
    pack::GameScope all;
    pack::PackContext disk{pack::GameFamily::Disk, ""}, cdtv{pack::GameFamily::Cdtv, "cdtv"}, st{pack::GameFamily::St, "atari-st-kixx"};
    CHECK(all.applies(disk) && all.applies(cdtv) && all.applies(st));
    CHECK(disk.build_name() == "canonical" && cdtv.build_name() == "cdtv");

    pack::GameScope only_cdtv;
    only_cdtv.games = 1u << unsigned(pack::GameFamily::Cdtv);
    CHECK(!only_cdtv.applies(disk) && only_cdtv.applies(cdtv) && !only_cdtv.applies(st));

    pack::GameScope pp;  // a disk build narrowed by tag
    pp.games = 1u << unsigned(pack::GameFamily::Disk);
    pp.builds = {"power-pack-2234"};
    pack::PackContext power{pack::GameFamily::Disk, "power-pack-2234"};
    CHECK(pp.applies(power) && !pp.applies(disk));
    pp.builds = {"canonical"};  // the canonical image has no tag but is named "canonical"
    CHECK(pp.applies(disk) && pp.applies(pack::PackContext{pack::GameFamily::Disk, "canonical"}) && !pp.applies(power));

    // An entry never widens its pack; two build lists with nothing in common apply nowhere.
    pack::GameScope packs_scope, entry_scope;
    packs_scope.games = (1u << unsigned(pack::GameFamily::Disk)) | (1u << unsigned(pack::GameFamily::Cdtv));
    entry_scope.games = (1u << unsigned(pack::GameFamily::Cdtv)) | (1u << unsigned(pack::GameFamily::St));
    const pack::GameScope both = pack::GameScope::intersect(packs_scope, entry_scope);
    CHECK(both.games == (1u << unsigned(pack::GameFamily::Cdtv)) && both.builds.empty());
    pack::GameScope a, b;
    a.builds = {"pdos-v5", "canonical"};
    b.builds = {"canonical", "compilation-1762"};
    const pack::GameScope ab = pack::GameScope::intersect(a, b);
    CHECK(ab.builds.size() == 1 && ab.builds[0] == "canonical" && ab.games == pack::GameScope::kAllGames);
    a.builds = {"pdos-v5"};
    CHECK(pack::GameScope::intersect(a, b).games == 0);
    CHECK(!pack::GameScope::intersect(a, b).applies(disk));
    pack::GameScope none, some;
    some.builds = {"cdtv"};
    CHECK(pack::GameScope::intersect(none, some).builds == some.builds);
    CHECK(pack::GameScope::intersect(some, none).builds == some.builds);

    pack::GameFamily g;
    CHECK(pack::parse_game("disk", g) && g == pack::GameFamily::Disk);
    CHECK(pack::parse_game("cdtv", g) && g == pack::GameFamily::Cdtv);
    CHECK(pack::parse_game("st", g) && g == pack::GameFamily::St);
    CHECK(!pack::parse_game("auto", g) && !pack::parse_game("", g) && !pack::parse_game("ST", g));
    CHECK(std::string(pack::game_name(pack::GameFamily::Cdtv)) == "cdtv");
}

// Every lookup of the null library answers "nothing".
void test_null_library() {
    const pack::PackLibrary& n = pack::null_library();
    const uint16_t pal[16] = {};
    CHECK(n.empty() && !n.has_graphics() && !n.has_audio());
    CHECK(n.packs().empty());
    CHECK(!n.graphic(1, 16, 16) && !n.flash(1, 16, 16) && !n.far_line(1) && !n.background(1) && !n.stars() && !n.palette(pal) &&
          !n.panel());
    CHECK(n.screens().empty() && n.screens_for_block(1).empty());
    CHECK(!n.has_clip_length(640) && !n.sfx("k") && !n.speech("k") && !n.speech_for_file("SPEECH01") && !n.music_slot("title"));
    const pack::MusicDefaults d = n.music_defaults();
    CHECK(d.crossfade_ms == 0 && d.fade_in_ms == 0 && d.fade_out_ms == 0 && d.loop_mode == pack::LoopMode::Pack &&
          d.retrigger == pack::Retrigger::Restart);
    std::vector<uint8_t> out;
    std::string why;
    CHECK(!n.read_file(pack::FileRef{0, "a.wav"}, out, why) && !why.empty());
    why.clear();
    CHECK(!n.open_file(pack::FileRef{0, "a.wav"}, why) && !why.empty());
    CHECK(n.context().game == pack::GameFamily::Disk);

    // The accessor: the null library until one is installed, then back after nullptr.
    CHECK(&pack::packs() == &pack::null_library());
    CHECK(pack::packs().empty());
}

// A library that reports graphics and forwards everything else to the null library: enough to test installing one.
class OneGraphicLibrary final : public pack::PackLibrary {
public:
    const pack::PackContext& context() const override { return n().context(); }
    const std::vector<pack::PackInfo>& packs() const override { return n().packs(); }
    bool has_graphics() const override { return true; }
    bool has_audio() const override { return false; }
    const pack::GraphicEntry* graphic(uint64_t h, int w, int hh) const override { return h == 7 && w == 16 && hh == 12 ? &g_ : nullptr; }
    const pack::GraphicEntry* flash(uint64_t a, int b, int c) const override { return n().flash(a, b, c); }
    const pack::FarLineRef* far_line(uint64_t h) const override { return n().far_line(h); }
    const pack::BackgroundEntry* background(int l) const override { return n().background(l); }
    const pack::StarsEntry* stars() const override { return n().stars(); }
    const pack::PaletteOverride* palette(const uint16_t* c) const override { return n().palette(c); }
    const pack::PanelSet* panel() const override { return n().panel(); }
    const std::vector<const pack::ScreenEntry*>& screens() const override { return n().screens(); }
    const std::vector<const pack::ScreenEntry*>& screens_for_block(uint64_t h) const override { return n().screens_for_block(h); }
    bool has_clip_length(uint32_t b) const override { return n().has_clip_length(b); }
    const pack::ClipEntry* sfx(const std::string& k) const override { return n().sfx(k); }
    const pack::ClipEntry* speech(const std::string& k) const override { return n().speech(k); }
    const pack::ClipEntry* speech_for_file(const std::string& f) const override { return n().speech_for_file(f); }
    const pack::MusicDefaults& music_defaults() const override { return n().music_defaults(); }
    const pack::MusicSlot* music_slot(const std::string& s) const override { return n().music_slot(s); }
    bool read_file(const pack::FileRef& f, std::vector<uint8_t>& o, std::string& w) const override { return n().read_file(f, o, w); }
    std::unique_ptr<pack::ByteSource> open_file(const pack::FileRef& f, std::string& w) const override { return n().open_file(f, w); }

private:
    static const pack::PackLibrary& n() { return pack::null_library(); }
    pack::GraphicEntry g_{};
};

void test_install() {
    pack::set_packs(std::make_shared<OneGraphicLibrary>());
    CHECK(&pack::packs() != &pack::null_library());
    CHECK(!pack::packs().empty() && pack::packs().has_graphics());
    CHECK(pack::packs().graphic(7, 16, 12) != nullptr && pack::packs().graphic(7, 16, 13) == nullptr);
    pack::set_packs(nullptr);
    CHECK(&pack::packs() == &pack::null_library());
}

void test_settings_struct() {
    pack::AssetPackSettings s;
    CHECK(s.pack_paths.empty() && s.packs_enabled.empty() && s.packs_dir.empty());
    CHECK(!s.upgrade && s.audio_upgrade && !s.faithful && s.speech_volume == 100);
    CHECK(!s.graphics_active() && s.audio_active());  // audio is on by default, but only matters when a pack has audio
    s.upgrade = true;
    CHECK(s.graphics_active() && s.audio_active());
    s.faithful = true;  // the Faithful switch beats both
    CHECK(!s.graphics_active() && !s.audio_active());
    s.faithful = false;
    CHECK(pack::apply_modern_item("noaudio", s) && !s.audio_upgrade && !s.audio_active());
    CHECK(pack::apply_modern_item("audio", s) && s.audio_upgrade);
    CHECK(!pack::apply_modern_item("upgrade", s) && !pack::apply_modern_item("", s) && !pack::apply_modern_item("Audio", s));

    pack::set_pack_settings(s);
    CHECK(pack::pack_settings().upgrade && pack::pack_settings().audio_upgrade);
    pack::set_pack_settings(pack::AssetPackSettings{});
    CHECK(!pack::pack_settings().upgrade);
}

void test_helpers() {
    CHECK(pack::split_list("").empty());
    CHECK(pack::split_list(" ; ;").empty());
    CHECK((pack::split_list("a;b") == std::vector<std::string>{"a", "b"}));
    CHECK((pack::split_list("  my pack ; other.zip;;  ") == std::vector<std::string>{"my pack", "other.zip"}));
    CHECK((pack::split_list("one") == std::vector<std::string>{"one"}));
    CHECK(pack::join_path("", "packs").empty());
    CHECK(pack::join_path("/data/", "packs") == "/data/packs");
    CHECK(pack::join_path("/data", "packs") == "/data/packs");
    CHECK(pack::join_path("C:\\data\\", "packs") == "C:\\data\\packs");
    CHECK(pack::default_packs_dir("/home/u/.local/share/x2/") == "/home/u/.local/share/x2/packs");
    CHECK(pack::default_packs_dir("").empty());
}

void test_messages() {
    pack::PackMessage m{pack::Severity::Warning, "My pack", "graphics", "0048848ccf64d966-16x12", "picture is 31x24, expected 32x24"};
    CHECK(m.format() == "asset pack: My pack: graphics[\"0048848ccf64d966-16x12\"]: picture is 31x24, expected 32x24");
    pack::PackMessage no_key{pack::Severity::Error, "My pack", "manifest", "", "invalid JSON"};
    CHECK(no_key.format() == "asset pack: My pack: manifest: invalid JSON");
    pack::PackMessage bare{pack::Severity::Info, "", "", "", "loaded 3 packs"};
    CHECK(bare.format() == "asset pack: loaded 3 packs");
    pack::PackMessage pack_only{pack::Severity::Info, "My pack", "", "", "needs a newer x2recomp"};
    CHECK(pack_only.format() == "asset pack: My pack: needs a newer x2recomp");

    pack::PackReport r;
    r.add(pack::Severity::Info, "p", "s", "k", "i");
    r.add(pack::Severity::Warning, "p", "s", "k", "w");
    r.add(pack::Severity::Warning, "p", "s", "k2", "w2");
    r.add(pack::Severity::Error, "p", "s", "k", "e");
    CHECK(r.messages.size() == 4 && r.count(pack::Severity::Info) == 1 && r.count(pack::Severity::Warning) == 2 &&
          r.count(pack::Severity::Error) == 1);

    // The WS1a entry points report "not implemented yet" and a null result.
    pack::PackReport lr;
    CHECK(pack::load_packs(pack::AssetPackSettings{}, pack::PackContext{}, lr) == nullptr);
    CHECK(lr.count(pack::Severity::Info) == 1);
    CHECK(pack::PackInfo{}.usable == false && pack::PackInfo{}.scope.games == pack::GameScope::kAllGames);
}

// settings.cfg: the new keys parse, bad values are reported and skipped, a file from before they existed keeps the defaults
// (graphics-only settings written by an older x2recomp), and format -> parse is stable.
void test_settings_file() {
    Settings d;
    CHECK(d.audio_upgrade && d.packs_enabled.empty() && d.speech_volume == 100);

    Settings s;
    std::string err;
    CHECK(parse_settings("audio_upgrade = off\npacks_enabled = alpha; beta\nspeech_volume = 40\n", s, err) && err.empty());
    CHECK(!s.audio_upgrade && s.packs_enabled == "alpha; beta" && s.speech_volume == 40);

    Settings bad;
    err.clear();
    CHECK(!parse_settings("speech_volume = 101\naudio_upgrade = maybe\nsfx_volume = 20\n", bad, err));
    CHECK(err.find("speech_volume") != std::string::npos && err.find("audio_upgrade") != std::string::npos);
    CHECK(bad.speech_volume == 100 && bad.audio_upgrade && bad.sfx_volume == 20);  // the valid line still applied

    // A file written by v0.3.0 (no pack keys at all): everything else parses, the new fields keep their defaults.
    Settings old;
    err.clear();
    CHECK(parse_settings("# x2recomp settings\nscale = 4\nupgrade = on\nasset_packs = a;b\nsfx_volume = 60\nmusic = mix\n", old, err) &&
          err.empty());
    CHECK(old.scale == 4 && old.upgrade && old.asset_packs == "a;b" && old.sfx_volume == 60 && old.music == "mix");
    CHECK(old.audio_upgrade && old.packs_enabled.empty() && old.speech_volume == 100);

    // Round trip: what is written reads back identically, and the keys are in the file.
    Settings w;
    w.audio_upgrade = false;
    w.packs_enabled = "x;y";
    w.speech_volume = 25;
    const std::string text = format_settings(w);
    CHECK(text.find("audio_upgrade = off") != std::string::npos && text.find("packs_enabled = x;y") != std::string::npos &&
          text.find("speech_volume = 25") != std::string::npos);
    Settings r;
    err.clear();
    CHECK(parse_settings(text, r, err) && err.empty());
    CHECK(!r.audio_upgrade && r.packs_enabled == "x;y" && r.speech_volume == 25);
    CHECK(format_settings(r) == text);
    // The defaults are written too, so a new settings file documents the keys.
    const std::string def_text = format_settings(Settings{});
    CHECK(def_text.find("audio_upgrade = on") != std::string::npos && def_text.find("packs_enabled =") != std::string::npos &&
          def_text.find("speech_volume = 100") != std::string::npos);
}

void test_layouts() {
    using namespace x2::modern;
    const CaptureLayout& D = kDiskLayout;
    const CaptureLayout& C = kCdtvLayout;
    const CaptureLayout& S = kStLayout;
    // The level word: $E04 on the disk builds, the CDTV release's block is the disk addresses + 6, the ST keeps it where the disk does.
    CHECK(D.level_var == 0xE04 && C.level_var == D.level_var + 6 && S.level_var == D.level_var);
    // Measure items stay unmapped until WS2 fills them.
    CHECK(D.far_ring_lines == kUnknown && C.far_ring_lines == kUnknown && S.far_ring_lines == kUnknown);
    CHECK(D.far_scroll_sign == 0 && C.far_scroll_sign == 0 && S.far_scroll_sign == 0);
    // The status panel strip: window lines 192-199 as the HD panel draws them; the ST's is not measured.
    CHECK(D.panel.known() && D.panel.first_line == 192 && D.panel.lines == 8 && D.panel.cell_w == 8 && D.panel.max_span == 6);
    CHECK(C.panel.first_line == D.panel.first_line && C.panel.lines == D.panel.lines && C.panel.cell_w == D.panel.cell_w &&
          C.panel.max_span == D.panel.max_span);
    CHECK(!S.panel.known() && S.panel.first_line == kUnknown && S.panel.lines == kUnknown);
    // The ST has no capture: every capture address is the sentinel (nothing may read them).
    CHECK(S.draw_ptr == kUnknownAddr && S.show_ptr == kUnknownAddr && S.star_table == kUnknownAddr && S.object_draw_return == kUnknownAddr);
    for (uint32_t a : S.list_heads) CHECK(a == kUnknownAddr);
    for (uint32_t a : S.var_addr) CHECK(a == kUnknownAddr);
    CHECK(S.far_copy_a == kUnknownAddr && S.tile_lo == kUnknownAddr && S.star_lo == kUnknownAddr && !S.star_writes_exact);
    // The existing layouts did not move (values also pinned by modern_tests).
    CHECK(D.draw_ptr == 0x406 && C.draw_ptr == 0x408 && D.var_addr[kRing] == 0x436);

    // The accessors by game family.
    CHECK(&capture_layout_for(pack::GameFamily::Disk) == &kDiskLayout && &capture_layout_for(pack::GameFamily::Cdtv) == &kCdtvLayout &&
          &capture_layout_for(pack::GameFamily::St) == &kStLayout);
    CHECK(&screen_geometry_for(pack::GameFamily::Disk) == &kAmigaScreen && &screen_geometry_for(pack::GameFamily::Cdtv) == &kAmigaScreen &&
          &screen_geometry_for(pack::GameFamily::St) == &kStScreen);
    CHECK(&audio_layout_for(pack::GameFamily::Disk) == &kDiskAudio && &audio_layout_for(pack::GameFamily::Cdtv) == &kCdtvAudio &&
          &audio_layout_for(pack::GameFamily::St) == &kStAudio);

    // Screen geometry: the Amiga window inside the faithful frame (modern/present.h), the ST's whole picture.
    CHECK(kAmigaScreen.x == 0 && kAmigaScreen.y == kFrameTop && kAmigaScreen.w == kFrameWidth && kAmigaScreen.h == kLines &&
          kAmigaScreen.colours == 32);
    CHECK(kStScreen.x == 0 && kStScreen.y == 0 && kStScreen.w == 320 && kStScreen.h == 200 && kStScreen.colours == 16);
}

// The audio layout holds the same addresses the audio stages already use (cdtv/cdtv_music.h, cdtv/cdtv_game.h).
void test_audio_layout() {
    using namespace x2::modern;
    const AudioLayout& D = kDiskAudio;
    const AudioLayout& C = kCdtvAudio;
    const AudioLayout& S = kStAudio;
    CHECK(D.effect_lo == cdtv::kEffectCode && D.effect_hi == cdtv::kDriverEnd);
    CHECK(D.music_init == cdtv::kMusicInit && D.music_stop == cdtv::kMusicStop && D.level_word == cdtv::kLevelWord &&
          D.music_shadow == cdtv::kMusicShadow);
    CHECK(D.title_call == 0x8230 && D.title_call + 6 == cdtv::kTitleReturn);
    CHECK((D.level_calls == std::array<uint32_t, 4>{0x7896, 0x7C50, 0x7CDA, 0x7D8C}));
    CHECK((D.side == std::array<uint8_t, 4>{0, 1, 1, 0}));  // AUD0 and AUD3 left, AUD1 and AUD2 right
    CHECK(C.cd_play == cdtv::kCdPlay && C.cd_stop == cdtv::kCdStop && C.cd_tick == cdtv::kCdTick &&
          C.cd_volume_full == cdtv::kCdVolumeFull && C.cd_volume_mute == cdtv::kCdVolumeMute && C.cd_track_var == cdtv::kCdTrackVar);
    CHECK(C.level_word == kDiskAudio.level_word + 6 && C.speech_lo == 0xB645C && C.speech_hi == kUnknownAddr);
    CHECK(C.effect_lo == kUnknownAddr && C.music_init == kUnknownAddr);   // the CDTV program has no tracker
    CHECK(S.effect_lo == kUnknownAddr && S.cd_play == kUnknownAddr && S.music_init == kUnknownAddr && S.side[0] == 0xFF);
}

} // namespace

int main() {
    struct T {
        const char* name;
        void (*fn)();
    };
    const T tests[] = {
        {"hashing", test_hashing},         {"key_syntax", test_key_syntax},   {"scope", test_scope},
        {"null_library", test_null_library}, {"install", test_install},       {"settings_struct", test_settings_struct},
        {"helpers", test_helpers},         {"messages", test_messages},       {"settings_file", test_settings_file},
        {"layouts", test_layouts},         {"audio_layout", test_audio_layout},
    };
    for (const T& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-16s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
