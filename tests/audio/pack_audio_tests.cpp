// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Pack sound effects and speech (runtime/audio/pack_audio.*, audio_dump.*, the Paula event sink and channel gates) on synthetic
// chip RAM and synthetic packs: no game data. The level of a voice is exact integer arithmetic (docs/re-notes/asset-packs-sfx.md),
// so most checks compare exact sample values.
#include "audio/audio_dump.h"
#include "audio/decode.h"
#include "audio/pack_audio.h"
#include "audio/paula.h"
#include "modern/layout.h"
#include "pack/pack_api.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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
#define CHECK_NEAR(a, b, tol)                                                                                       \
    do {                                                                                                            \
        ++g_checks;                                                                                                 \
        if (std::fabs(double(a) - double(b)) > double(tol)) {                                                       \
            ++g_failures;                                                                                           \
            std::fprintf(stderr, "  FAIL %s:%d: %s = %g, want %g +- %g\n", __FILE__, __LINE__, #a, double(a), double(b), double(tol)); \
        }                                                                                                           \
    } while (0)

// ---- a synthetic pack library -------------------------------------------------------------------------------------

class Lib final : public pack::PackLibrary {
public:
    pack::PackContext ctx;
    std::vector<pack::PackInfo> infos;
    std::map<std::string, pack::ClipEntry> sfx_map, speech_map, file_map;
    std::set<uint32_t> lengths;
    mutable int sfx_calls = 0, speech_calls = 0;
    pack::MusicDefaults defaults;
    std::vector<const pack::ScreenEntry*> no_screens;

    const pack::PackContext& context() const override { return ctx; }
    const std::vector<pack::PackInfo>& packs() const override { return infos; }
    bool has_graphics() const override { return false; }
    bool has_audio() const override { return !sfx_map.empty() || !speech_map.empty() || !file_map.empty(); }
    const pack::GraphicEntry* graphic(uint64_t, int, int) const override { return nullptr; }
    const pack::GraphicEntry* flash(uint64_t, int, int) const override { return nullptr; }
    const pack::FarLineRef* far_line(uint64_t) const override { return nullptr; }
    const pack::BackgroundEntry* background(int) const override { return nullptr; }
    const pack::StarsEntry* stars() const override { return nullptr; }
    const pack::PaletteOverride* palette(const uint16_t*) const override { return nullptr; }
    const pack::PanelSet* panel() const override { return nullptr; }
    const std::vector<const pack::ScreenEntry*>& screens() const override { return no_screens; }
    const std::vector<const pack::ScreenEntry*>& screens_for_block(uint64_t) const override { return no_screens; }
    bool has_clip_length(uint32_t n) const override { return lengths.count(n) != 0; }
    const pack::ClipEntry* sfx(const std::string& k) const override {
        ++sfx_calls;
        auto it = sfx_map.find(k);
        return it == sfx_map.end() ? nullptr : &it->second;
    }
    const pack::ClipEntry* speech(const std::string& k) const override {
        ++speech_calls;
        auto it = speech_map.find(k);
        return it == speech_map.end() ? nullptr : &it->second;
    }
    const pack::ClipEntry* speech_for_file(const std::string& f) const override {
        auto it = file_map.find(f);
        return it == file_map.end() ? nullptr : &it->second;
    }
    const pack::MusicDefaults& music_defaults() const override { return defaults; }
    const pack::MusicSlot* music_slot(const std::string&) const override { return nullptr; }
    bool read_file(const pack::FileRef&, std::vector<uint8_t>&, std::string& why) const override { why = "synthetic"; return false; }
    std::unique_ptr<pack::ByteSource> open_file(const pack::FileRef&, std::string& why) const override { why = "synthetic"; return nullptr; }
};

std::shared_ptr<pack::DecodedClip> make_clip(int channels, int rate, size_t frames, int (*fn)(size_t i, int ch)) {
    auto c = std::make_shared<pack::DecodedClip>();
    c->channels = channels;
    c->rate = rate;
    c->pcm.resize(frames * size_t(channels));
    for (size_t i = 0; i < frames; ++i)
        for (int ch = 0; ch < channels; ++ch) c->pcm[i * size_t(channels) + size_t(ch)] = int16_t(fn(i, ch));
    return c;
}
int dc20000(size_t, int) { return 20000; }
int ramp4(size_t i, int) { return int(i) * 4; }
int stereo_lr(size_t, int ch) { return ch == 0 ? 20000 : -10000; }
int silence(size_t, int) { return 0; }

pack::ClipEntry entry(std::shared_ptr<pack::DecodedClip> clip, int ref_period = 400) {
    pack::ClipEntry e;
    e.clip = std::move(clip);
    e.ref_period = ref_period;
    e.file = "synthetic.wav";
    return e;
}

// ---- a Paula with the stage attached -------------------------------------------------------------------------------

constexpr uint32_t kSampleAt = 0x1000;
constexpr int kRate = 48000;

struct Stage {
    std::vector<uint8_t> ram;
    Paula p;
    audio::PackAudio pa;
    std::shared_ptr<Lib> lib = std::make_shared<Lib>();
    std::vector<int16_t> out;  // everything the stage left in Paula's output, as a consumer would take it
    uint16_t dma = 0;
    bool attached = false;

    explicit Stage(pack::GameFamily game = pack::GameFamily::Disk, uint32_t ram_size = 0x80000, bool attach = true) : ram(ram_size, 0) {
        p.attach(ram.data(), ram_size - 1, [](uint16_t) {});
        PaulaConfig c;
        c.filter = AudioFilterModel::None;
        c.stereo_separation = 1.0f;
        c.output_gain = 1.0f;
        c.output_rate = kRate;
        p.configure(c);
        p.set_output_enabled(true);
        if (game == pack::GameFamily::Disk) p.set_writer_query([]() { return AudioSource::Effect; });
        lib->ctx.game = game;
        pack::AssetPackSettings s;
        s.upgrade = true;
        s.audio_upgrade = true;
        pack::set_pack_settings(s);
        pack::set_packs(lib);
        if (attach) {
            pa.attach(p, ram.data(), ram_size - 1, game, modern::audio_layout_for(game));
            attached = true;
        }
    }
    ~Stage() {
        pack::set_packs(nullptr);
        pack::set_pack_settings(pack::AssetPackSettings{});
    }
    // Fills the sample bytes, registers the replacement under its key and returns the key.
    std::string add_sfx(uint32_t at, size_t bytes, int fill, pack::ClipEntry e) {
        for (size_t i = 0; i < bytes; ++i) ram[at + i] = uint8_t(fill);
        const std::string key = pack::sample_key(&ram[at], uint32_t(bytes));
        e.key = key;
        lib->sfx_map[key] = std::move(e);
        lib->lengths.insert(uint32_t(bytes));
        return key;
    }
    void setup(int ch, uint32_t lc, uint16_t words, uint16_t per, uint16_t vol) {
        const uint16_t base = uint16_t(0xA0 + 16 * ch);
        p.write_reg(base + 0, uint16_t(lc >> 16));
        p.write_reg(base + 2, uint16_t(lc));
        p.write_reg(base + 4, words);
        p.write_reg(base + 6, per);
        p.write_reg(base + 8, vol);
    }
    void play(int ch, uint32_t lc, uint16_t words, uint16_t per, uint16_t vol) {
        setup(ch, lc, words, per, vol);
        dma = uint16_t(dma | (1 << ch));
        p.set_dmacon(uint16_t(0x0200 | dma), uint16_t(0x8000 | (1 << ch)));
    }
    void stop(int ch) {
        dma = uint16_t(dma & ~(1 << ch));
        p.set_dmacon(uint16_t(0x0200 | dma), uint16_t(1 << ch));
    }
    void run(size_t frames) {
        const uint64_t target = p.frames_out() + frames;
        while (p.frames_out() < target) {
            p.run_until(p.now() + 3697);  // about 50 output frames
            pa.after_frame(p);
            out.insert(out.end(), p.output().begin(), p.output().end());
            p.output().clear();
        }
    }
    int16_t l(size_t frame) const { return out[frame * 2]; }
    int16_t r(size_t frame) const { return out[frame * 2 + 1]; }
};

// ---- Paula: no sink, bit-identical; the events; the gate --------------------------------------------------------------

std::vector<int16_t> run_plain(bool with_sink, bool with_stage_no_pack) {
    Stage s(pack::GameFamily::Disk, 0x80000, with_stage_no_pack);
    if (with_sink && !with_stage_no_pack) s.p.set_event_sink([](const SoundEvent&) {});
    for (int i = 0; i < 4000; ++i) s.ram[kSampleAt + size_t(i)] = uint8_t(int8_t((i * 37) % 200 - 100));
    s.play(0, kSampleAt, 2000, 300, 64);
    s.play(1, kSampleAt + 100, 1500, 450, 40);
    s.run(1500);
    s.setup(0, kSampleAt + 400, 800, 250, 30);
    s.run(1500);
    s.stop(1);
    s.run(1500);
    return s.out;
}

void test_paula_identity_without_replacement() {
    const std::vector<int16_t> plain = run_plain(false, false);
    CHECK(!plain.empty());
    CHECK(run_plain(true, false) == plain);   // a sink that ignores everything changes nothing
    CHECK(run_plain(false, true) == plain);   // the stage attached with no pack (and no dump) changes nothing
}

void test_event_sink() {
    std::vector<uint8_t> ram(0x80000, 0);
    Paula p;
    p.attach(ram.data(), 0x7FFFF, [](uint16_t) {});
    PaulaConfig c;
    c.filter = AudioFilterModel::None;
    p.configure(c);
    p.set_output_enabled(true);
    p.set_writer_query([]() { return AudioSource::Effect; });
    std::vector<SoundEvent> ev;
    p.set_event_sink([&](const SoundEvent& e) { ev.push_back(e); });
    auto reg = [&](int ch, int off, uint16_t v) { p.write_reg(uint16_t(0xA0 + 16 * ch + off), v); };
    reg(2, 0, 0);
    reg(2, 2, 0x2000);
    reg(2, 4, 8);
    reg(2, 6, 200);
    reg(2, 8, 50);
    CHECK(ev.empty());  // writes while the channel is idle are not events
    p.run_until(1000);
    p.set_dmacon(0x0204, 0x8004);
    CHECK(ev.size() == 1);
    CHECK(ev[0].kind == SoundEvent::Start && ev[0].ch == 2 && ev[0].lc == 0x2000 && ev[0].len == 8 && ev[0].per == 200 && ev[0].vol == 50);
    CHECK(ev[0].src == AudioSource::Effect);
    p.run_until(1500);
    reg(2, 6, 300);
    reg(2, 8, 20);
    CHECK(ev.size() == 3 && ev[1].kind == SoundEvent::Period && ev[1].per == 300 && ev[2].kind == SoundEvent::Volume && ev[2].vol == 20);
    reg(2, 2, 0x3000);
    reg(2, 4, 1);
    p.run_until(1500 + 16 * 300 * 2);  // the 8-word buffer is over: the registers are latched
    size_t reloads = 0;
    for (const SoundEvent& e : ev)
        if (e.kind == SoundEvent::Reload) {
            ++reloads;
            CHECK(e.lc == 0x3000 && e.len == 1 && e.ch == 2);
        }
    CHECK(reloads >= 1);
    p.set_dmacon(0x0200, 4);
    const size_t before = ev.size();
    p.run_until(p.now() + 2000);
    CHECK(ev.size() > before && ev.back().kind == SoundEvent::Stop);  // the event is the idle edge, not the request
    CHECK(!p.channel_active(2));
}

void test_channel_gate() {
    auto level = [](int mix) {
        Stage s(pack::GameFamily::Disk, 0x80000, false);
        for (size_t i = 0; i < 4000; ++i) s.ram[kSampleAt + i] = 100;
        s.play(0, kSampleAt, 2000, 200, 64);
        s.p.set_channel_mix(0, mix);
        s.run(1000);
        return s.out[2 * 800];
    };
    const int full = level(256), half = level(128), none = level(0);
    CHECK(full > 1000);
    CHECK(none == 0);
    CHECK_NEAR(half, full / 2.0, 1);
    // The gate lasts until the channel's next DMA start and is gone when it goes idle.
    Stage s(pack::GameFamily::Disk, 0x80000, false);
    for (size_t i = 0; i < 4000; ++i) s.ram[kSampleAt + i] = 100;
    s.play(0, kSampleAt, 2000, 200, 64);
    s.p.set_channel_mix(0, 0);
    s.run(100);
    CHECK(s.p.channel_mix(0) == 0);
    s.stop(0);
    s.run(200);
    CHECK(s.p.channel_mix(0) == 256);
    s.play(0, kSampleAt, 2000, 200, 64);
    CHECK(s.p.channel_mix(0) == 256);
}

// ---- replacement ---------------------------------------------------------------------------------------------------

void test_basic_replacement() {
    Stage s;
    s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 24000, dc20000)));
    s.play(0, kSampleAt, 2000, 400, 64);
    s.run(3000);
    // AUD0 is the left side, hard-panned, at the output gain of 1.0 here: a full-scale file is one Paula channel at full volume
    // (half of a side's full scale), so a constant 20000 gives 10000 on the left and nothing on the right, and the original
    // (DC 64 on the same channel) is gone from the mix.
    for (size_t f : {100u, 500u, 1500u, 2900u}) {
        CHECK(s.l(f) == 10000);
        CHECK(s.r(f) == 0);
    }
    CHECK(s.pa.stats().replaced == 1);
    CHECK(s.pa.stats().hashed == 1);
    // The channel state is the original's: it is playing, and has latched what the game wrote.
    CHECK(s.p.channel_active(0));
    CHECK(s.p.stats()[0].dma_starts == 1);
}

void test_no_hash_without_matching_length() {
    Stage s;
    s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 2400, dc20000)));
    for (size_t i = 0; i < 1000; ++i) s.ram[0x4000 + i] = 50;
    s.play(1, 0x4000, 500, 400, 64);  // 1000 bytes: no replacement has that length
    s.run(500);
    CHECK(s.pa.stats().candidates == 1);
    CHECK(s.pa.stats().hashed == 0);
    CHECK(s.lib->sfx_calls == 0);
    CHECK(s.pa.stats().replaced == 0);
}

void test_music_starts_never_replaced() {
    Stage s;
    s.p.set_writer_query([]() { return AudioSource::Music; });  // the tracker wrote the pointer
    s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 2400, dc20000)));
    s.play(0, kSampleAt, 2000, 400, 64);
    s.run(500);
    CHECK(s.pa.stats().candidates == 0);
    CHECK(s.pa.stats().replaced == 0);
    CHECK(s.out[2 * 300] != 0);  // the original is heard
}

void test_unmapped_build() {
    Stage s(pack::GameFamily::Disk, 0x80000, false);
    s.pa.attach(s.p, s.ram.data(), 0x7FFFF, pack::GameFamily::Disk, modern::kDiskPdosAudio);
    CHECK(!s.pa.effects_mapped());
    s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 24000, dc20000)));
    s.play(0, kSampleAt, 2000, 400, 64);
    s.run(500);
    CHECK(s.pa.stats().starts == 1 && s.pa.stats().candidates == 0 && s.pa.stats().replaced == 0);
    CHECK(s.out[2 * 300] != 0 && s.out[2 * 300] != 10000);
}

void test_side_pan_gain_and_stereo() {
    {
        Stage s;
        auto e = entry(make_clip(1, kRate, 24000, dc20000));
        s.add_sfx(kSampleAt, 4000, 64, e);
        s.play(1, kSampleAt, 2000, 400, 64);  // AUD1: right
        s.run(500);
        CHECK(s.l(300) == 0 && s.r(300) == 10000);
    }
    {
        Stage s;
        auto e = entry(make_clip(1, kRate, 24000, dc20000));
        e.pan = pack::PanMode::Position;
        e.pan_pos = 0.0f;
        s.add_sfx(kSampleAt, 4000, 64, e);
        s.play(0, kSampleAt, 2000, 400, 64);
        s.run(500);
        CHECK_NEAR(s.l(300), 7071, 2);
        CHECK_NEAR(s.r(300), 7071, 2);
    }
    {
        Stage s;
        auto e = entry(make_clip(1, kRate, 24000, dc20000));
        e.gain_db = -6.0206f;
        s.add_sfx(kSampleAt, 4000, 64, e);
        s.play(0, kSampleAt, 2000, 400, 64);
        s.run(500);
        CHECK_NEAR(s.l(300), 5000, 2);
    }
    {
        Stage s;  // a stereo file plays as it is
        s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(2, kRate, 24000, stereo_lr)));
        s.play(0, kSampleAt, 2000, 400, 64);
        s.run(500);
        CHECK(s.l(300) == 10000 && s.r(300) == -5000);
    }
    {
        Stage s;  // a stereo file started on a pair of channels plays once, not twice
        s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(2, kRate, 24000, stereo_lr)));
        s.play(0, kSampleAt, 2000, 400, 64);
        s.play(1, kSampleAt, 2000, 400, 64);
        s.run(500);
        CHECK(s.l(300) == 10000 && s.r(300) == -5000);
        CHECK(s.pa.stats().replaced == 1);
        CHECK(s.p.channel_mix(1) == 0);  // but its own original is gated too
    }
    {
        Stage s;  // a mono file started on a pair plays on both, as the original has a channel per start
        s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 24000, dc20000)));
        s.play(0, kSampleAt, 2000, 400, 64);
        s.play(1, kSampleAt, 2000, 400, 64);
        s.run(500);
        CHECK(s.l(300) == 10000 && s.r(300) == 10000);
        CHECK(s.pa.stats().replaced == 2);
    }
}

void test_volume_follow() {
    Stage s;
    s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 48000, dc20000)));
    s.play(0, kSampleAt, 2000, 400, 32);
    s.run(500);
    CHECK(s.l(300) == 5000);
    s.p.write_reg(0xA8, 16);  // the game fades the channel
    s.run(1500);
    CHECK(s.l(1200) == 2500);
    s.p.write_reg(0xA8, 0);
    s.run(500);
    CHECK(s.l(2300) == 0);
}

void test_pitch_follow() {
    auto slope = [](Stage& s, size_t from, size_t span) { return double(s.l(from + span) - s.l(from)) / double(span); };
    {
        Stage s;
        s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 8000, ramp4), 400));
        s.play(0, kSampleAt, 2000, 400, 64);  // period = ref_period: the file at its own speed
        s.run(1500);
        CHECK_NEAR(slope(s, 200, 100), 2.0, 0.02);  // ramp 4 per frame, half gain
    }
    {
        Stage s;
        s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 8000, ramp4), 400));
        s.play(0, kSampleAt, 2000, 800, 64);  // twice the period: half the speed
        s.run(1500);
        CHECK_NEAR(slope(s, 200, 100), 1.0, 0.02);
    }
    {
        Stage s;
        s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 8000, ramp4), 400));
        s.play(0, kSampleAt, 2000, 400, 64);
        s.run(1000);
        CHECK_NEAR(slope(s, 300, 100), 2.0, 0.02);
        s.p.write_reg(0xA6, 200);  // the game bends the pitch up an octave
        s.run(1000);
        CHECK_NEAR(slope(s, 1500, 100), 4.0, 0.05);
    }
    {
        auto e = entry(make_clip(1, kRate, 8000, ramp4), 400);
        e.pitch = pack::PitchMode::Fixed;
        Stage s;
        s.add_sfx(kSampleAt, 4000, 64, e);
        s.play(0, kSampleAt, 2000, 800, 64);  // fixed: the period is ignored
        s.run(1500);
        CHECK_NEAR(slope(s, 200, 100), 2.0, 0.02);
    }
    {
        // A file at another rate keeps its duration: 24000 frames at 24 kHz are one second, played at the reference period.
        auto e = entry(make_clip(1, 24000, 12000, dc20000), 400);
        Stage s;
        s.add_sfx(kSampleAt, 4000, 64, e);
        s.play(0, kSampleAt, 2000, 400, 64);
        s.run(30000);
        size_t last = 0;
        for (size_t f = 0; f < 30000; ++f)
            if (s.l(f) > 5000) last = f;
        CHECK_NEAR(double(last), 24000.0, 100.0);  // 12000 frames at 24 kHz: half a second = 24000 frames at 48 kHz
        CHECK(s.pa.active_voices() == 0);
    }
}

// The effect code parks a channel (period 124) right before it turns DMA off and may start the next sound at once: the parking
// write belongs to the voice that is ending, never to the one that starts a moment later; and a real pitch write applies at once.
void test_park_write_and_restart() {
    Stage s;
    s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 24000, ramp4), 400));
    s.play(0, kSampleAt, 2000, 400, 64);
    s.run(300);
    s.p.write_reg(0xA6, 124);  // park, then off, then straight away the same sound again
    s.stop(0);
    s.run(20);
    s.play(0, kSampleAt, 2000, 400, 64);
    s.run(1500);
    const size_t from = 300 + 20 + 400;  // well after the second start
    CHECK_NEAR(double(s.l(from + 100) - s.l(from)) / 100.0, 2.0, 0.05);  // the new voice runs at its own period, not at the park period
    Stage t;
    t.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 24000, ramp4), 400));
    t.play(0, kSampleAt, 2000, 400, 64);
    t.run(100);
    t.p.write_reg(0xA6, 200);
    t.run(50);  // a pitch write is heard at once, not 3 ms late
    CHECK_NEAR(double(t.l(145) - t.l(135)) / 10.0, 4.0, 0.2);
}

void test_cut_and_never() {
    for (bool never : {false, true}) {
        auto e = entry(make_clip(1, kRate, 24000, dc20000));
        e.cut = never ? pack::CutMode::Never : pack::CutMode::Channel;
        e.fade_out_ms = 5;
        Stage s;
        s.add_sfx(kSampleAt, 4000, 64, e);
        s.play(0, kSampleAt, 2000, 400, 64);
        s.run(1000);
        s.stop(0);
        s.run(1500);
        CHECK(s.l(900) == 10000);
        if (never) {
            CHECK(s.l(2000) == 10000);  // rings out to the end of the file
        } else {
            CHECK(s.l(2400) == 0);      // faded out within a few milliseconds of the stop
            CHECK(s.l(1050) > 0 && s.l(1050) <= 10000);
        }
        s.run(30000);
        CHECK(s.pa.active_voices() == 0);
    }
}

void test_loop_and_reload() {
    auto e = entry(make_clip(1, kRate, 1000, dc20000));
    e.loop = true;
    Stage s;
    s.add_sfx(kSampleAt, 1000, 64, e);
    for (size_t i = 0; i < 64; ++i) s.ram[0x6000 + i] = 0;
    s.play(0, kSampleAt, 500, 400, 64);
    s.setup(0, kSampleAt, 500, 400, 64);  // the registers keep pointing at the same sample: the buffer loops
    s.run(8000);
    for (size_t f : {300u, 1500u, 4000u, 7900u}) CHECK(s.l(f) == 10000);
    CHECK(s.pa.active_voices() == 1);
    s.setup(0, 0x6000, 16, 400, 64);  // the game moves the channel to its silent loop: the voice goes with it
    s.run(8000);
    CHECK(s.l(8000 + 7900) == 0);
    CHECK(s.pa.active_voices() == 0);
    // Without `loop` the file ends and the original stays muted until the channel changes.
    Stage t;
    t.add_sfx(kSampleAt, 1000, 64, entry(make_clip(1, kRate, 1000, dc20000)));
    t.play(0, kSampleAt, 500, 400, 64);
    t.run(3000);
    CHECK(t.l(500) == 10000);
    CHECK(t.l(1500) == 0 && t.l(2900) == 0);
}

void test_original_mix() {
    auto e = entry(make_clip(1, kRate, 24000, silence));
    e.original = pack::OriginalMode::Mix;
    e.original_mix_db = -6.0206f;
    Stage s;
    s.add_sfx(kSampleAt, 4000, 64, e);
    s.play(0, kSampleAt, 2000, 200, 64);
    s.run(1000);
    Stage plain(pack::GameFamily::Disk, 0x80000, false);
    for (size_t i = 0; i < 4000; ++i) plain.ram[kSampleAt + i] = 64;
    plain.play(0, kSampleAt, 2000, 200, 64);
    plain.run(1000);
    CHECK(plain.l(800) > 1000);
    CHECK_NEAR(s.l(800), plain.l(800) * 0.5, 2);
}

void test_polyphony() {
    auto e = entry(make_clip(1, kRate, 96000, dc20000));
    e.cut = pack::CutMode::Never;
    Stage s;
    s.add_sfx(kSampleAt, 4000, 64, e);
    for (int i = 0; i < 60; ++i) {
        s.play(i % 4, kSampleAt, 2000, 400, 64);
        s.run(30);
        s.stop(i % 4);
        s.run(30);
    }
    CHECK(s.pa.active_voices() <= 2 * audio::PackAudio::kMaxVoices);
    CHECK(s.pa.stats().stolen > 0);
    int clipped = 0;
    for (int16_t v : s.out)
        if (v == 32767 || v == -32768) ++clipped;
    CHECK(clipped > 0);  // many full-scale voices saturate rather than wrap
}

void test_faithful_switch() {
    Stage s;
    s.add_sfx(kSampleAt, 4000, 64, entry(make_clip(1, kRate, 96000, dc20000)));
    s.play(0, kSampleAt, 2000, 400, 64);
    s.run(1000);
    CHECK(s.l(900) == 10000 && s.p.channel_mix(0) == 0);
    pack::AssetPackSettings ps = pack::pack_settings();
    ps.faithful = true;
    pack::set_pack_settings(ps);
    s.run(1000);
    CHECK(s.p.channel_mix(0) == 256);
    CHECK(s.pa.active_voices() == 0);
    CHECK(s.l(1900) != 10000);  // the original is back
    ps.faithful = false;
    pack::set_pack_settings(ps);
    // audio switched off in the settings is the same as no pack
    ps.audio_upgrade = false;
    pack::set_pack_settings(ps);
    s.stop(0);
    s.run(200);
    s.play(0, kSampleAt, 2000, 400, 64);
    s.run(500);
    CHECK(s.pa.stats().replaced == 1);
}

void test_categories() {
    auto run = [](bool speech_cat, int sfx_pct, int speech_pct) {
        Stage s(pack::GameFamily::Cdtv, 0x100000);
        pack::AssetPackSettings ps = pack::pack_settings();
        ps.speech_volume = speech_pct;
        pack::set_pack_settings(ps);
        s.pa.set_sfx_volume(sfx_pct);
        const uint32_t at = speech_cat ? 0xB6500 : kSampleAt;
        auto e = entry(make_clip(1, kRate, 24000, dc20000));
        const std::string key = s.add_sfx(at, 4000, 64, e);
        if (speech_cat) {
            s.lib->speech_map[key] = s.lib->sfx_map[key];
            s.lib->sfx_map.clear();
        }
        s.play(0, at, 2000, 400, 64);
        s.run(500);
        return int(s.l(300));
    };
    CHECK(run(false, 100, 100) == 10000);
    CHECK(run(false, 50, 100) == 5000);
    CHECK(run(false, 100, 0) == 10000);  // the speech volume does not touch effects
    CHECK(run(true, 100, 100) == 10000);
    CHECK(run(true, 100, 25) == 2500);
    CHECK(run(true, 0, 100) == 10000);   // and the effect volume does not touch speech
}

void test_cdtv_speech_and_files() {
    Stage s(pack::GameFamily::Cdtv, 0x100000);
    auto e = entry(make_clip(1, kRate, 24000, dc20000));
    // A speech clip named by disc file: no length, no hash.
    s.lib->file_map["SPEECH63"] = e;
    s.pa.on_file_loaded(0xB645C, "CD0:SPEECH63", 0x1A518);
    for (size_t i = 0; i < 1000; ++i) s.ram[0xB6500 + i] = 33;
    s.play(0, 0xB6500, 500, 298, 64);
    s.run(500);
    CHECK(s.pa.stats().replaced == 1 && s.pa.stats().hashed == 0);
    CHECK(s.l(300) == 10000);
    s.stop(0);
    s.run(200);
    // The same buffer after another file was loaded over it: the name no longer matches.
    s.pa.on_file_loaded(0xB645C, "CD0:SPEECH12", 0x1A518);
    s.play(0, 0xB6500, 500, 298, 64);
    s.run(500);
    CHECK(s.pa.stats().replaced == 1);
    // Every start counts on CDTV, with no tracker: a sound outside the speech buffer is an effect.
    s.add_sfx(0x2000, 1000, 77, entry(make_clip(1, kRate, 24000, dc20000)));
    s.stop(0);
    s.run(200);
    s.play(0, 0x2000, 500, 298, 64);
    s.run(500);
    CHECK(s.pa.stats().replaced == 2);
    CHECK(s.lib->speech_calls == 0);  // (the speech lookup by key is only made for speech-region starts with a matching length)
}

// ---- determinism -----------------------------------------------------------------------------------------------------

uint64_t fnv(const std::vector<int16_t>& v) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int16_t s : v)
        for (int i = 0; i < 2; ++i) {
            h ^= uint8_t(uint16_t(s) >> (8 * i));
            h *= 0x100000001b3ull;
        }
    return h;
}

std::vector<int16_t> scenario() {
    Stage s;
    auto e = entry(make_clip(1, 22050, 20000, ramp4), 357);  // another rate: resampled when it is first used
    e.gain_db = -3.5f;
    s.add_sfx(kSampleAt, 4000, 64, e);
    auto e2 = entry(make_clip(2, 44100, 30000, stereo_lr), 300);
    e2.loop = true;
    e2.original = pack::OriginalMode::Mix;
    e2.original_mix_db = -9.0f;
    s.add_sfx(0x3000, 1000, 90, e2);
    for (size_t i = 0; i < 4000; ++i) s.ram[0x8000 + i] = uint8_t(int8_t((i * 53) % 220 - 110));
    s.play(0, kSampleAt, 2000, 357, 64);
    s.play(2, 0x3000, 500, 300, 50);
    s.run(2000);
    s.p.write_reg(0xA6, 250);
    s.p.write_reg(0xB8, 20);
    s.play(3, 0x8000, 1500, 200, 40);
    s.run(2000);
    s.stop(0);
    s.setup(2, 0x8000, 100, 300, 50);
    s.run(6000);
    return s.out;
}

void test_determinism() {
    const std::vector<int16_t> a = scenario(), b = scenario();
    CHECK(a == b);
    CHECK(!a.empty());
    // Recorded once; identical on every platform and build type because positions, pitch and gains are integer arithmetic.
    constexpr uint64_t kGolden = 0x47dce5e8c7303e96ull;
    const uint64_t h = fnv(a);
    std::printf("  scenario hash %016llx\n", (unsigned long long)h);
    CHECK(h == kGolden);
}

// ---- the dump --------------------------------------------------------------------------------------------------------

struct TempDir {
    std::string path;
    TempDir() {
        static int n = 0;
        path = (fs::temp_directory_path() / ("x2_pack_audio_test_" + std::to_string(std::rand()) + "_" + std::to_string(++n))).string();
        fs::remove_all(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_text(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::vector<uint8_t> read_bytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void write_text(const std::string& p, const std::string& t) {
    std::ofstream f(p, std::ios::binary);
    f << t;
}

void test_dump_rate_and_wav() {
    int rep = 0;
    // The rate is the colour clock over the period times the repeats, to better than 1e-5 (so a long loop does not drift), 44.1-192 kHz.
    for (int ref : {124, 150, 357, 600, 977, 2374, 10000, 65535}) {
        const int rate = audio::AudioDumper::file_rate(ref, &rep);
        CHECK(rep >= 1 && rate >= 44100 && rate <= 192000);
        CHECK_NEAR(double(rate) / rep, 3546895.0 / ref, 3546895.0 / ref * 1.2e-5 + 1e-9);
    }
    CHECK(audio::AudioDumper::file_rate(124, &rep) == 57208 && rep == 2);
    const int r600 = audio::AudioDumper::file_rate(600, &rep);
    const uint8_t bytes[4] = {0x01, 0xFF, 0x80, 0x7F};
    const std::vector<uint8_t> w = audio::AudioDumper::wav_bytes(bytes, 4, 600);
    CHECK(rep >= 2 && w.size() == size_t(44 + 4 * rep * 2));
    audio::DecodeLimits lim;
    audio::DecodedClip clip;
    std::string err;
    CHECK(audio::decode_all(audio::memory_source(w.data(), w.size()), lim, {}, clip, &err));
    CHECK(clip.channels == 1 && clip.sample_rate == r600 && clip.frames == uint64_t(4 * rep));
    const int16_t want[4] = {256, -256, -32768, 127 * 256};
    for (size_t i = 0; i < clip.pcm.size(); ++i) CHECK(clip.pcm[i] == want[i / size_t(rep)]);
}

void test_dump_matches_chip_ram_and_merges() {
    TempDir td;
    Stage s;
    audio::AudioDumper d;
    std::string why;
    CHECK(d.open(td.path, why));
    s.pa.set_dumper(&d);
    for (size_t i = 0; i < 2000; ++i) s.ram[0x2000 + i] = uint8_t(int8_t((i * 29) % 230 - 115));
    for (size_t i = 0; i < 640; ++i) s.ram[0x3000 + i] = uint8_t(int8_t(((i * 7) % 100) - 50));
    s.play(0, 0x2000, 1000, 357, 64);
    s.run(300);
    s.stop(0);
    s.run(300);
    s.play(0, 0x2000, 1000, 357, 64);  // the same sample again: one entry, two uses
    s.run(300);
    s.setup(1, 0x3000, 320, 600, 40);
    s.stop(0);
    s.run(300);
    s.play(1, 0x3000, 320, 600, 40);
    s.run(300);
    s.p.write_reg(0xB6, 700);  // a period change while it plays counts
    s.run(300);
    CHECK(d.sfx_count() == 2);
    CHECK(d.finish(why));
    const std::string key1 = pack::sample_key(&s.ram[0x2000], 2000), key2 = pack::sample_key(&s.ram[0x3000], 640);
    // each dumped clip equals the sample read independently from chip RAM
    {
        const std::vector<uint8_t> w = read_bytes(td.path + "/sfx/" + key1 + ".wav");
        audio::DecodedClip clip;
        std::string err;
        CHECK(audio::decode_all(audio::memory_source(w.data(), w.size()), audio::DecodeLimits{}, {}, clip, &err));
        int rep = 0;
        CHECK(clip.channels == 1 && clip.sample_rate == audio::AudioDumper::file_rate(357, &rep) && clip.frames == uint64_t(2000 * rep));
        bool same = clip.pcm.size() == 2000 * size_t(rep);
        for (size_t i = 0; same && i < clip.pcm.size(); ++i) same = clip.pcm[i] == int16_t(int(int8_t(s.ram[0x2000 + i / size_t(rep)])) * 256);
        CHECK(same);
    }
    {
        const std::vector<uint8_t> w = read_bytes(td.path + "/sfx/" + key2 + ".wav");
        audio::DecodedClip clip;
        std::string err;
        CHECK(audio::decode_all(audio::memory_source(w.data(), w.size()), audio::DecodeLimits{}, {}, clip, &err));
        int rep = 0;
        CHECK(clip.sample_rate == audio::AudioDumper::file_rate(600, &rep) && rep >= 2);  // held for its true duration
        bool same = clip.pcm.size() == 640 * size_t(rep);
        for (size_t i = 0; same && i < clip.pcm.size(); ++i) same = clip.pcm[i] == int16_t(int(int8_t(s.ram[0x3000 + i / size_t(rep)])) * 256);
        CHECK(same);
    }
    std::string m = read_text(td.path + "/manifest.json");
    CHECK(m.find("\"version\": 2") != std::string::npos);
    CHECK(m.find("\"" + key1 + "\"") != std::string::npos && m.find("\"" + key2 + "\"") != std::string::npos);
    CHECK(m.find("\"ref_period\": 357") != std::string::npos && m.find("\"uses\": 2") != std::string::npos);
    CHECK(m.find("\"periods\": [600, 700]") != std::string::npos);
    CHECK(m.find("\"source\": \"effect\"") != std::string::npos);

    // An author edits the manifest and a file, adds another section, and dumps again with one more sound.
    m.replace(m.find("\"ref_period\": 357"), 17, "\"ref_period\": 357, \"gain_db\": -4");
    m.insert(m.rfind('}'), ",\n  \"graphics\": {\"a-1x1\": {\"file\": \"g.png\"}}\n");
    write_text(td.path + "/manifest.json", m);
    write_text(td.path + "/sfx/" + key1 + ".wav", "edited");
    {
        Stage s2;
        audio::AudioDumper d2;
        CHECK(d2.open(td.path, why));
        s2.pa.set_dumper(&d2);
        for (size_t i = 0; i < 200; ++i) s2.ram[0x4000 + i] = uint8_t(int8_t(i % 90));
        for (size_t i = 0; i < 2000; ++i) s2.ram[0x2000 + i] = uint8_t(int8_t((i * 29) % 230 - 115));
        s2.play(0, 0x4000, 100, 400, 64);
        s2.run(300);
        s2.stop(0);
        s2.run(300);
        s2.play(0, 0x2000, 1000, 357, 64);
        s2.run(300);
        CHECK(d2.finish(why));
        const std::string key3 = pack::sample_key(&s2.ram[0x4000], 200);
        const std::string m2 = read_text(td.path + "/manifest.json");
        CHECK(m2.find("\"" + key3 + "\"") != std::string::npos);
        CHECK(m2.find("\"gain_db\": -4") != std::string::npos);                 // the edited entry is untouched
        CHECK(m2.find("\"graphics\": {\"a-1x1\": {\"file\": \"g.png\"}}") != std::string::npos);  // other sections too
        CHECK(read_text(td.path + "/sfx/" + key1 + ".wav") == "edited");        // and the edited file is not overwritten
        CHECK(audio::AudioDumper::existing_audio_member(td.path).rfind("\"audio\": {", 0) == 0);
    }
}

void test_dump_version1_manifest_and_nothing_to_dump() {
    TempDir td;
    fs::create_directories(td.path);
    write_text(td.path + "/manifest.json", "{\n  \"format\": \"x2-asset-pack\",\n  \"version\": 1,\n  \"name\": \"dump\"\n}\n");
    Stage s;
    audio::AudioDumper d;
    std::string why;
    CHECK(d.open(td.path, why));
    s.pa.set_dumper(&d);
    for (size_t i = 0; i < 400; ++i) s.ram[0x2000 + i] = uint8_t(int8_t(i % 100));
    s.play(0, 0x2000, 200, 300, 64);
    s.run(300);
    CHECK(d.finish(why));
    const std::string m = read_text(td.path + "/manifest.json");
    CHECK(m.find("\"version\": 2") != std::string::npos && m.find("\"version\": 1") == std::string::npos);
    CHECK(m.find("\"name\": \"dump\"") != std::string::npos);
    CHECK(audio::AudioDumper::existing_audio_member(td.path + "/nope").empty());
    // Silence is not a sound: an all-zero buffer (the game's idle loop) is not an entry.
    TempDir td2;
    Stage s2;
    audio::AudioDumper d2;
    CHECK(d2.open(td2.path, why));
    s2.pa.set_dumper(&d2);
    s2.play(0, 0x2000, 16, 124, 64);  // RAM there is zero
    s2.run(200);
    CHECK(d2.sfx_count() == 0);
    // A manifest that is not JSON is refused, not overwritten.
    TempDir td3;
    fs::create_directories(td3.path);
    write_text(td3.path + "/manifest.json", "not json");
    audio::AudioDumper d3;
    CHECK(d3.open(td3.path, why));
    CHECK(!d3.finish(why));
    CHECK(read_text(td3.path + "/manifest.json") == "not json");
}

void test_dump_speech_and_cd0() {
    TempDir td;
    Stage s(pack::GameFamily::Cdtv, 0x100000);
    audio::AudioDumper d;
    std::string why;
    CHECK(d.open(td.path, why));
    s.pa.set_dumper(&d);
    s.pa.on_file_loaded(0xB645C, "CD0:SPEECH63", 0x1A518);
    for (size_t i = 0; i < 800; ++i) s.ram[0xB6600 + i] = uint8_t(int8_t(i % 77));
    for (size_t i = 0; i < 400; ++i) s.ram[0x5000 + i] = uint8_t(int8_t(i % 33 + 1));
    s.play(0, 0xB6600, 400, 298, 64);
    s.run(300);
    s.stop(0);
    s.run(200);
    s.play(0, 0x5000, 200, 298, 64);
    s.run(300);
    CHECK(d.speech_count() == 1 && d.sfx_count() == 1);
    CHECK(d.finish(why));
    const std::string m = read_text(td.path + "/manifest.json");
    CHECK(m.find("\"speech\": {") != std::string::npos && m.find("\"cd0_file\": \"SPEECH63\"") != std::string::npos);
    CHECK(m.find("\"source\": \"speech\"") != std::string::npos);
    CHECK(fs::exists(td.path + "/speech/" + pack::sample_key(&s.ram[0xB6600], 800) + ".wav"));
}

} // namespace

int main() {
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"paula_identity_without_replacement", test_paula_identity_without_replacement},
        {"event_sink", test_event_sink},
        {"channel_gate", test_channel_gate},
        {"basic_replacement", test_basic_replacement},
        {"no_hash_without_matching_length", test_no_hash_without_matching_length},
        {"music_starts_never_replaced", test_music_starts_never_replaced},
        {"unmapped_build", test_unmapped_build},
        {"side_pan_gain_and_stereo", test_side_pan_gain_and_stereo},
        {"volume_follow", test_volume_follow},
        {"pitch_follow", test_pitch_follow},
        {"park_write_and_restart", test_park_write_and_restart},
        {"cut_and_never", test_cut_and_never},
        {"loop_and_reload", test_loop_and_reload},
        {"original_mix", test_original_mix},
        {"polyphony", test_polyphony},
        {"faithful_switch", test_faithful_switch},
        {"categories", test_categories},
        {"cdtv_speech_and_files", test_cdtv_speech_and_files},
        {"determinism", test_determinism},
        {"dump_rate_and_wav", test_dump_rate_and_wav},
        {"dump_matches_chip_ram_and_merges", test_dump_matches_chip_ram_and_merges},
        {"dump_version1_manifest_and_nothing_to_dump", test_dump_version1_manifest_and_nothing_to_dump},
        {"dump_speech_and_cd0", test_dump_speech_and_cd0},
    };
    for (const T& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
