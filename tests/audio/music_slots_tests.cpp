// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Asset-pack music (runtime/audio/{stream_source,music_slots}.*, runtime/cdtv/{cd_player,cdtv_music}.*): the stream player with
// per-voice rates, wrapped and crossfaded loops, fades and gains; slot resolution and loop decisions on an in-memory pack library of
// synthetic WAV files (and the FLAC / Ogg Vorbis tones in tests/audio/fixtures, argument 1); and the cue layer on the CDTV
// and disk builds (Machine with synthetic code, no game data). No game audio anywhere.
//
//   x2_music_slots_tests FIXTURE_DIR
#include "audio/music_slots.h"
#include "audio/stream_source.h"
#include "cdtv/cd_player.h"
#include "cdtv/cdtv_music.h"
#include "machine.h"
#include "pack/pack_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace x2;
using namespace x2::audio;
using x2::cdtv::CdPlayer;
using Bytes = std::vector<uint8_t>;

namespace {
int g_failures = 0;
int g_checks = 0;
std::string g_fixtures;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

constexpr double kPi = 3.14159265358979323846;

// ---- signals and files ---------------------------------------------------------------------------------------------

double noise(uint64_t i, int c) {
    uint32_t x = uint32_t(i * 2 + uint64_t(c)) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return double(int(x % 40001) - 20000);
}

FrameBuffer noise_frames(uint64_t n, uint64_t offset = 0) {
    auto v = std::make_shared<std::vector<float>>(n * 2);
    for (uint64_t i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c) (*v)[i * 2 + size_t(c)] = float(noise(offset + i, c));
    return v;
}

FrameBuffer sine_frames(uint64_t n, int rate, double hz, double amp) {
    auto v = std::make_shared<std::vector<float>>(n * 2);
    for (uint64_t i = 0; i < n; ++i) (*v)[i * 2] = (*v)[i * 2 + 1] = float(amp * std::sin(2 * kPi * hz * double(i) / rate));
    return v;
}

void put16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(Bytes& b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
void put_tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

// A 16-bit PCM WAV of interleaved samples; `loop` adds a smpl chunk (end inclusive, as the format has it).
Bytes make_wav(int channels, int rate, const std::vector<int16_t>& pcm, int loop_start = -1, int loop_end = -1) {
    Bytes b;
    put_tag(b, "RIFF");
    put32(b, 0);
    put_tag(b, "WAVE");
    put_tag(b, "fmt ");
    put32(b, 16);
    put16(b, 1);
    put16(b, unsigned(channels));
    put32(b, unsigned(rate));
    put32(b, unsigned(rate * channels * 2));
    put16(b, unsigned(channels * 2));
    put16(b, 16);
    put_tag(b, "data");
    put32(b, uint32_t(pcm.size() * 2));
    for (int16_t s : pcm) put16(b, uint16_t(s));
    if (loop_start >= 0) {
        put_tag(b, "smpl");
        put32(b, 36 + 24);
        for (int i = 0; i < 7; ++i) put32(b, 0);
        put32(b, 1);
        put32(b, 0);
        put32(b, 1);
        put32(b, 0);
        put32(b, uint32_t(loop_start));
        put32(b, uint32_t(loop_end));
        put32(b, 0);
        put32(b, 0);
    }
    const uint32_t riff = uint32_t(b.size() - 8);
    b[4] = uint8_t(riff); b[5] = uint8_t(riff >> 8); b[6] = uint8_t(riff >> 16); b[7] = uint8_t(riff >> 24);
    return b;
}

std::vector<int16_t> sine_pcm(uint64_t frames, int channels, int rate, double hz, double amp) {
    std::vector<int16_t> v(frames * size_t(channels));
    for (uint64_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) v[i * size_t(channels) + size_t(c)] = int16_t(std::lround(amp * std::sin(2 * kPi * hz * double(i) / rate)));
    return v;
}

Bytes load_fixture(const std::string& name) {
    std::ifstream f(g_fixtures + "/" + name, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- an in-memory pack library -------------------------------------------------------------------------------------

class MemFile final : public pack::ByteSource {
public:
    explicit MemFile(const Bytes* b) : b_(b) {}
    uint64_t size() const override { return b_->size(); }
    size_t read(uint64_t offset, void* dst, size_t n) override {
        if (offset >= b_->size()) return 0;
        n = size_t(std::min<uint64_t>(n, b_->size() - offset));
        std::copy(b_->begin() + std::ptrdiff_t(offset), b_->begin() + std::ptrdiff_t(offset + n), static_cast<uint8_t*>(dst));
        return n;
    }

private:
    const Bytes* b_;
};

class TestLibrary final : public pack::PackLibrary {
public:
    TestLibrary() {
        pack::PackInfo p;
        p.id = "testpack";
        p.name = "Test pack";
        packs_.push_back(p);
    }
    void add_file(const std::string& path, Bytes b) { files_[path] = std::move(b); }
    pack::MusicSlot& slot(const std::string& name, const std::string& file = "") {
        pack::MusicSlot& s = slots_[name];
        s.slot = name;
        if (!file.empty()) s.file = pack::FileRef{0, file};
        return s;
    }
    pack::MusicDefaults defaults;

    const pack::PackContext& context() const override { return n().context(); }
    const std::vector<pack::PackInfo>& packs() const override { return packs_; }
    bool has_graphics() const override { return false; }
    bool has_audio() const override { return !slots_.empty(); }
    const pack::GraphicEntry* graphic(uint64_t h, int w, int hh) const override { return n().graphic(h, w, hh); }
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
    const pack::MusicDefaults& music_defaults() const override { return defaults; }
    const pack::MusicSlot* music_slot(const std::string& s) const override {
        auto it = slots_.find(s);
        return it == slots_.end() ? nullptr : &it->second;
    }
    bool read_file(const pack::FileRef& f, std::vector<uint8_t>& out, std::string& why) const override {
        auto it = files_.find(f.path);
        if (it == files_.end()) { why = "no such file in the pack: " + f.path; return false; }
        out = it->second;
        return true;
    }
    std::unique_ptr<pack::ByteSource> open_file(const pack::FileRef& f, std::string& why) const override {
        auto it = files_.find(f.path);
        if (it == files_.end()) { why = "no such file in the pack: " + f.path; return nullptr; }
        return std::make_unique<MemFile>(&it->second);
    }

private:
    static const pack::PackLibrary& n() { return pack::null_library(); }
    std::vector<pack::PackInfo> packs_;
    std::map<std::string, pack::MusicSlot> slots_;
    std::map<std::string, Bytes> files_;
};

std::shared_ptr<const StreamSpec> buffer_spec(FrameBuffer b, int rate, LoopRule loop = LoopRule::Game, uint64_t ls = 0, uint64_t le = 0) {
    auto s = std::make_shared<StreamSpec>();
    s->open = [b]() { return std::make_unique<BufferStream>(b); };
    s->rate = rate;
    s->frames = b->size() / 2;
    s->loop = loop;
    s->loop_start = ls;
    s->loop_end = le;
    s->crossfade_ms = 0;
    return s;
}

std::vector<int16_t> render(CdPlayer& p, size_t frames) {
    std::vector<int16_t> out(frames * 2, 0);
    p.render(out.data(), frames);
    return out;
}

int16_t expect(double v, double gain = 1.0) { return int16_t(std::lround(v * gain * CdPlayer::kCdGain)); }

double snr_db(const std::vector<int16_t>& out, size_t from, size_t to, int rate, double hz, double amp) {
    double err = 0, sig = 0;
    for (size_t i = from; i < to; ++i) {
        const double want = amp * CdPlayer::kCdGain * std::sin(2 * kPi * hz * double(i) / rate);
        err += (out[i * 2] - want) * (out[i * 2] - want);
        sig += want * want;
    }
    return 10 * std::log10(sig / std::max(err, 1e-9));
}

double rms(const std::vector<int16_t>& v, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i) s += double(v[i * 2]) * v[i * 2];
    return std::sqrt(s / double(std::max<size_t>(1, to - from)));
}

// ---- streams -------------------------------------------------------------------------------------------------------

void test_streams() {
    FrameBuffer a = noise_frames(10), b = noise_frames(10, 100);
    BufferStream sa(a);
    std::vector<float> out(8 * 2, -1.0f);
    sa.read(-2, 8, out.data()); // two silent frames, then frames 0..5
    CHECK(out[0] == 0 && out[3] == 0 && out[4] == (*a)[0] && out[15] == (*a)[11]);
    sa.read(8, 4, out.data()); // the last two frames, then silence
    CHECK(out[0] == (*a)[16] && out[3] == (*a)[19] && out[4] == 0 && out[7] == 0);

    ConcatStream cs(std::make_unique<BufferStream>(a), 10, std::make_unique<BufferStream>(b));
    std::vector<float> c(30 * 2);
    cs.read(0, 25, c.data());
    bool ok = true;
    for (size_t i = 0; i < 10; ++i) ok = ok && c[i * 2] == (*a)[i * 2];
    for (size_t i = 0; i < 10; ++i) ok = ok && c[(10 + i) * 2] == (*b)[i * 2];
    for (size_t i = 20; i < 25; ++i) ok = ok && c[i * 2] == 0 && c[i * 2 + 1] == 0;
    CHECK(ok);
    cs.read(7, 6, c.data()); // across the join
    CHECK(c[0] == (*a)[14] && c[2 * 2] == (*a)[18] && c[3 * 2] == (*b)[0] && c[5 * 2 + 1] == (*b)[2 * 2 + 1]);
}

// ---- the player ----------------------------------------------------------------------------------------------------

void test_player_basics() {
    FrameBuffer n = noise_frames(5000);
    // A stream at the output rate is copied at the CD gain, times the spec's gain.
    CdPlayer p;
    p.configure(44100, 0);
    auto spec = std::make_shared<StreamSpec>(*buffer_spec(n, 44100));
    spec->gain = 0.5;
    p.play(spec, 7);
    CHECK(p.track() == 7 && p.playing() && !p.idle());
    std::vector<int16_t> o = render(p, 1000);
    bool exact = true;
    for (size_t i = 0; i < 1000; ++i)
        for (int c = 0; c < 2; ++c) exact = exact && o[i * 2 + size_t(c)] == expect(noise(i, c), 0.5);
    CHECK(exact);

    // Fade in (from silence: only because the spec asks) and a long fade out on stop.
    auto fi = std::make_shared<StreamSpec>(*buffer_spec(sine_frames(20000, 44100, 440, 10000), 44100));
    fi->fade_in_ms = 20;
    fi->fade_out_ms = 40;
    CdPlayer q;
    q.configure(44100, 0);
    q.play(fi, 1);
    std::vector<int16_t> qo = render(q, 2000);
    CHECK(qo[0] == 0 && rms(qo, 0, 100) < rms(qo, 800, 900) * 0.6);
    q.stop();
    CHECK(q.track() == 0 && !q.playing());
    std::vector<int16_t> so = render(q, 4000);
    CHECK(rms(so, 1000, 1100) > 100 && rms(so, 3000, 3100) < 1); // still sounding after 5 ms, gone after 40 ms
    CHECK(q.idle());
    // A stream played once: silence afterwards, and the game is never told it finished.
    CdPlayer o1;
    o1.configure(44100, 0);
    o1.play(buffer_spec(n, 44100, LoopRule::Off), 1);
    render(o1, 6000);
    for (int t = 0; t < 120; ++t) { o1.tick(); }
    CHECK(!o1.finished() && o1.loops() == 0);
    // The game's rule: finished at the end, started again at the next 50-tick poll.
    CdPlayer g;
    g.configure(44100, 0);
    g.play(buffer_spec(n, 44100, LoopRule::Game), 1);
    std::vector<int16_t> all;
    for (int t = 0; t < 120; ++t) {
        std::vector<int16_t> f = render(g, 882);
        all.insert(all.end(), f.begin(), f.end());
        g.tick();
    }
    CHECK(g.loops() == 2 && all[50 * 882 * 2] == expect(noise(0, 0)) && all[100 * 882 * 2 + 1] == expect(noise(0, 1)));
    bool quiet = true; // between the end of the track and the poll
    for (size_t i = 6000; i < 49 * 882; ++i) quiet = quiet && all[i * 2] == 0;
    CHECK(quiet);
}

void test_wrapped_loop() {
    FrameBuffer n = noise_frames(1000);
    CdPlayer p;
    p.configure(44100, 0);
    p.play(buffer_spec(n, 44100, LoopRule::Seamless, 100, 400), 1);
    std::vector<int16_t> o = render(p, 3000);
    bool exact = true;
    for (size_t i = 0; i < 3000; ++i) {
        const uint64_t src = i < 400 ? i : 100 + (i - 100) % 300;
        exact = exact && o[i * 2] == expect(noise(src, 0)) && o[i * 2 + 1] == expect(noise(src, 1));
    }
    CHECK(exact);
    CHECK(p.loops() == 0 && !p.finished()); // the player never reports a seamless loop as finished

    // Resampled (22.05 kHz to 48 kHz) the taps run across the wrap: a sine with whole cycles in the loop stays a clean sine.
    const uint64_t n_loop = 441; // 20 cycles of 1 kHz at 22.05 kHz
    FrameBuffer s = sine_frames(2000, 22050, 1000, 16000);
    CdPlayer r;
    r.configure(48000, 0);
    r.play(buffer_spec(s, 22050, LoopRule::Seamless, 200, 200 + n_loop), 1);
    std::vector<int16_t> ro = render(r, 30000);
    CHECK(snr_db(ro, 800, 29000, 48000, 1000, 16000) > 55);
    // The same calls give the same samples whatever the render block sizes.
    CdPlayer a, b;
    for (CdPlayer* pl : {&a, &b}) { pl->configure(48000, 0); pl->play(buffer_spec(s, 22050, LoopRule::Seamless, 200, 200 + n_loop), 1); }
    std::vector<int16_t> one = render(a, 20000), parts;
    for (size_t k : {size_t(1), size_t(999), size_t(7000), size_t(12000)}) {
        std::vector<int16_t> f = render(b, k);
        parts.insert(parts.end(), f.begin(), f.end());
    }
    CHECK(one == parts);
}

void test_crossfaded_loop() {
    FrameBuffer n = noise_frames(20000);
    auto spec = std::make_shared<StreamSpec>(*buffer_spec(n, 44100, LoopRule::Seamless, 2000, 18000));
    spec->crossfade_ms = 100; // 4410 frames
    CdPlayer p;
    p.configure(44100, 0);
    p.play(spec, 1);
    std::vector<int16_t> o = render(p, 50000);
    double min_rms = 1e9;
    for (size_t w = 0; w + 441 <= 50000; w += 441) min_rms = std::min(min_rms, rms(o, w, w + 441));
    CHECK(min_rms > 5000); // never a silent window, never a dip to nothing
    CHECK(p.loops() >= 2);
    // The next pass starts at the loop start (frame 2000) and overlaps the end by the crossfade: the loop's period is shorter by it.
    CHECK(p.position() >= 2000 && p.position() < 18000);
    // At full level between the fades: the output at a point well inside the loop is the source (single voice).
    CHECK(o[3000 * 2] == expect(noise(3000, 0)));
}

void test_intro_loop_spec() {
    FrameBuffer intro = noise_frames(500);       // frames 0..499
    FrameBuffer body = noise_frames(300, 10000); // frames 500..799
    auto spec = std::make_shared<StreamSpec>();
    spec->open = [intro, body]() -> std::unique_ptr<StreamSource> {
        return std::make_unique<ConcatStream>(std::make_unique<BufferStream>(intro), 500, std::make_unique<BufferStream>(body));
    };
    spec->rate = 44100;
    spec->frames = 800;
    spec->loop = LoopRule::Seamless;
    spec->loop_start = 500;
    spec->loop_end = 800;
    spec->crossfade_ms = 0;
    CdPlayer p;
    p.configure(44100, 0);
    p.play(spec, 1);
    std::vector<int16_t> o = render(p, 2000);
    bool exact = true;
    for (size_t i = 0; i < 2000; ++i) {
        const double want = i < 500 ? noise(i, 0) : noise(10000 + (i - 500) % 300, 0); // the intro once, then only the body
        exact = exact && o[i * 2] == expect(want);
    }
    CHECK(exact);
}

// ---- slots on a library --------------------------------------------------------------------------------------------

void test_slot_names() {
    CHECK((disk_slot_names(0) == std::vector<std::string>{"title"}));
    CHECK((disk_slot_names(3) == std::vector<std::string>{"level3", "level"}));
    CHECK((cdtv_slot_names(CdtvRole::Title, 0, 2) == std::vector<std::string>{"cd2", "title"}));
    CHECK((cdtv_slot_names(CdtvRole::Level, 4, 3) == std::vector<std::string>{"cd3", "level4", "level"}));
    CHECK((cdtv_slot_names(CdtvRole::Level, 9, 3) == std::vector<std::string>{"cd3", "level"}));
    CHECK((cdtv_slot_names(CdtvRole::Other, 4, 9) == std::vector<std::string>{"cd9"}));
    CHECK(cdtv::cdtv_caller_role(0x03C822) == CdtvRole::Title && cdtv::cdtv_caller_role(0x0003ADB6) == CdtvRole::Level &&
          cdtv::cdtv_caller_role(0x03B45A) == CdtvRole::Level && cdtv::cdtv_caller_role(0x5047E) == CdtvRole::Other);
}

void test_resolution() {
    TestLibrary lib;
    lib.add_file("a.wav", make_wav(1, 22050, sine_pcm(3000, 1, 22050, 440, 8000)));
    lib.add_file("b.wav", make_wav(2, 44100, sine_pcm(6000, 2, 44100, 660, 8000)));
    lib.slot("level", "a.wav");
    lib.slot("level2", "b.wav");
    lib.slot("title").mute = true;
    lib.slot("cd5", "missing.wav");
    {
        pack::MusicSlot& s = lib.slot("level5", "a.wav");
        s.mute = true; // mute with a file still named: silence
    }
    lib.slot("cd6", "a.wav");
    MusicSlots slots(lib);

    MusicChoice c = slots.resolve(disk_slot_names(2));
    CHECK(c.kind == MusicChoice::Track && c.slot == "level2" && c.spec && c.spec->rate == 44100 && c.spec->frames == 6000);
    c = slots.resolve(disk_slot_names(4)); // no level4: the `level` fallback
    CHECK(c.kind == MusicChoice::Track && c.slot == "level" && c.spec->rate == 22050 && c.spec->frames == 3000);
    c = slots.resolve(disk_slot_names(0));
    CHECK(c.kind == MusicChoice::Silent && !c.spec);
    CHECK(slots.resolve({"nothing", "else"}).kind == MusicChoice::Original);
    CHECK(slots.resolve(disk_slot_names(5)).kind == MusicChoice::Silent);
    CHECK(slots.take_report().messages.empty());

    // A slot whose file is missing is skipped (once), with the next candidate or the original taking over.
    c = slots.resolve(cdtv_slot_names(CdtvRole::Level, 1, 5)); // cd5 is broken: level1 absent, `level` plays
    CHECK(c.kind == MusicChoice::Track && c.slot == "level");
    pack::PackReport rep = slots.take_report();
    CHECK(rep.messages.size() == 1 && rep.messages[0].severity == pack::Severity::Warning && rep.messages[0].section == "music" &&
          rep.messages[0].key == "cd5" && rep.messages[0].format().find("missing.wav") != std::string::npos);
    CHECK(slots.resolve({"cd5"}).kind == MusicChoice::Original && slots.take_report().messages.empty()); // reported once only
    CHECK(!slots.prepare("cd5") && slots.prepare("cd6") && slots.prepare("not-a-slot"));

    // Exact beats alias beats `level`.
    c = slots.resolve(cdtv_slot_names(CdtvRole::Level, 2, 6));
    CHECK(c.slot == "cd6");
    c = slots.resolve(cdtv_slot_names(CdtvRole::Level, 2, 3));
    CHECK(c.slot == "level2");
}

void test_loop_decisions() {
    TestLibrary lib;
    const int rate = 22050;
    lib.add_file("plain.wav", make_wav(1, rate, sine_pcm(4410, 1, rate, 440, 8000)));
    lib.add_file("smpl.wav", make_wav(1, rate, sine_pcm(4410, 1, rate, 440, 8000), 1000, 3999));
    lib.slot("s_default", "plain.wav");
    lib.slot("s_smpl", "smpl.wav");
    lib.slot("s_off", "smpl.wav").loop.kind = pack::LoopKind::Off;
    lib.slot("s_whole", "plain.wav").loop.kind = pack::LoopKind::Whole;
    {
        pack::MusicSlot& s = lib.slot("s_points", "smpl.wav");
        s.loop.kind = pack::LoopKind::Points;
        s.loop.start_s = 0.1;
        s.loop.end_s = 0.15;
    }
    {
        pack::MusicSlot& s = lib.slot("s_pastend", "plain.wav");
        s.loop.kind = pack::LoopKind::Points;
        s.loop.start_s = 0.05;
        s.loop.end_s = 99.0;
    }
    {
        pack::MusicSlot& s = lib.slot("s_backwards", "plain.wav");
        s.loop.kind = pack::LoopKind::Points;
        s.loop.start_s = 0.15;
        s.loop.end_s = 0.1;
    }
    lib.slot("s_game", "smpl.wav").loop_mode = pack::LoopMode::Game;
    {
        pack::MusicSlot& s = lib.slot("s_opts", "plain.wav");
        s.gain_db = -6.0f;
        s.crossfade_ms = 250;
        s.fade_in_ms = 30;
        s.fade_out_ms = 60;
        s.retrigger = pack::Retrigger::Continue;
    }
    lib.defaults.crossfade_ms = 120;
    lib.defaults.fade_out_ms = 10;
    MusicSlots slots(lib);
    auto spec = [&](const char* n) { return slots.resolve({n}); };

    // Nothing declared and nothing in the file: the original's rule (always repeat, the game restarts it).
    CHECK(spec("s_default").spec->loop == LoopRule::Game);
    // The file's own loop (smpl chunk, end inclusive) when the manifest says nothing; the manifest wins over it.
    MusicChoice c = spec("s_smpl");
    CHECK(c.spec->loop == LoopRule::Seamless && c.spec->loop_start == 1000 && c.spec->loop_end == 4000);
    CHECK(spec("s_off").spec->loop == LoopRule::Off);
    c = spec("s_whole");
    CHECK(c.spec->loop == LoopRule::Seamless && c.spec->loop_start == 0 && c.spec->loop_end == 4410);
    c = spec("s_points");
    CHECK(c.spec->loop == LoopRule::Seamless && c.spec->loop_start == 2205 && c.spec->loop_end == 3308);
    CHECK(slots.take_report().messages.empty());
    c = spec("s_pastend");
    CHECK(c.spec->loop_start == 1103 && c.spec->loop_end == 4410);
    pack::PackReport rep = slots.take_report();
    CHECK(rep.messages.size() == 1 && rep.messages[0].text.find("past the end") != std::string::npos);
    c = spec("s_backwards");
    CHECK(c.spec->loop == LoopRule::Seamless && c.spec->loop_start == 0 && c.spec->loop_end == 4410);
    CHECK(slots.take_report().messages.size() == 1);
    // loop_mode "game" keeps the original's rule whatever the points are.
    CHECK(spec("s_game").spec->loop == LoopRule::Game);
    // Options and defaults.
    c = spec("s_opts");
    CHECK(std::fabs(c.spec->gain - 0.5011872) < 1e-6 && c.spec->crossfade_ms == 250 && c.spec->fade_in_ms == 30 && c.spec->fade_out_ms == 60 &&
          c.retrigger == pack::Retrigger::Continue);
    c = spec("s_default");
    CHECK(c.spec->crossfade_ms == 120 && c.spec->fade_out_ms == 10 && c.spec->fade_in_ms == 0 && c.spec->gain == 1.0 &&
          c.retrigger == pack::Retrigger::Restart);
}

void test_intro_slot() {
    TestLibrary lib;
    const int rate = 22050;
    // intro: 500 frames of one pitch; loop file: 2205 frames of 441 Hz (whole cycles: 10 cycles in 1000 frames at 22.05 kHz is 220.5 Hz)
    lib.add_file("intro.wav", make_wav(1, rate, std::vector<int16_t>(500, 3000)));
    lib.add_file("loop.wav", make_wav(1, rate, std::vector<int16_t>(1000, -3000)));
    lib.add_file("intro48.wav", make_wav(1, 48000, std::vector<int16_t>(960, 3000)));
    {
        pack::MusicSlot& s = lib.slot("a", "loop.wav");
        s.intro = pack::FileRef{0, "intro.wav"};
        s.loop.kind = pack::LoopKind::Whole;
    }
    {
        pack::MusicSlot& s = lib.slot("b", "loop.wav");
        s.intro = pack::FileRef{0, "intro48.wav"}; // another rate: resampled to the loop file's
        s.loop.kind = pack::LoopKind::Whole;
    }
    lib.slot("c", "loop.wav").intro = pack::FileRef{0, "no-intro.wav"};
    MusicSlots slots(lib);

    MusicChoice c = slots.resolve({"a"});
    CHECK(c.kind == MusicChoice::Track && c.spec->frames == 1500 && c.spec->loop_start == 500 && c.spec->loop_end == 1500);
    CdPlayer p;
    p.configure(22050, 0);
    p.play(c.spec, 1);
    std::vector<int16_t> o = render(p, 4000);
    bool ok = true;
    for (size_t i = 0; i < 4000; ++i) ok = ok && o[i * 2] == int16_t(std::lround((i < 500 ? 3000 : -3000) * CdPlayer::kCdGain));
    CHECK(ok); // the intro once, then the body forever
    c = slots.resolve({"b"});
    CHECK(c.kind == MusicChoice::Track && c.spec->frames == 441 + 1000 && c.spec->loop_start == 441); // 960 frames at 48 kHz are 441 at 22.05 kHz
    CHECK(slots.resolve({"c"}).kind == MusicChoice::Original); // a broken intro drops the slot
    CHECK(!slots.take_report().messages.empty());
}

// A longer file than the pre-decoded head and the cache: streams from the decoder with seeks at the loop.
void test_streaming_file() {
    TestLibrary lib;
    const int rate = 44100;
    std::vector<int16_t> pcm(90000 * 2);
    for (size_t i = 0; i < 90000; ++i) { pcm[i * 2] = int16_t(noise(i, 0)); pcm[i * 2 + 1] = int16_t(noise(i, 1)); }
    lib.add_file("long.wav", make_wav(2, rate, pcm));
    pack::MusicSlot& s = lib.slot("long", "long.wav");
    s.loop.kind = pack::LoopKind::Points;
    s.loop.start_s = 20000.0 / rate;
    s.loop.end_s = 70000.0 / rate;
    MusicSlots slots(lib);
    MusicChoice c = slots.resolve({"long"});
    CHECK(c.kind == MusicChoice::Track && c.spec->frames == 90000 && c.spec->loop_start == 20000 && c.spec->loop_end == 70000);
    CdPlayer p;
    p.configure(44100, 0);
    p.play(c.spec, 1);
    std::vector<int16_t> o = render(p, 160000);
    bool exact = true;
    for (size_t i = 0; i < 160000 && exact; ++i) {
        const uint64_t src = i < 70000 ? i : 20000 + (i - 20000) % 50000;
        exact = o[i * 2] == expect(noise(src, 0)) && o[i * 2 + 1] == expect(noise(src, 1));
    }
    CHECK(exact);
    // Two voices of the same slot have their own readers (a crossfaded restart plays the head again while the tail finishes).
    auto x = std::make_shared<StreamSpec>(*c.spec);
    x->crossfade_ms = 50;
    CdPlayer q;
    q.configure(44100, 0);
    q.play(x, 1);
    std::vector<int16_t> qo = render(q, 160000);
    CHECK(q.loops() >= 2 && qo[1000 * 2] == expect(noise(1000, 0)));
}

void test_damaged_files() {
    TestLibrary lib;
    lib.add_file("junk.wav", Bytes(200, 0x5A));
    lib.add_file("empty.wav", make_wav(1, 22050, {}));
    lib.add_file("three.wav", make_wav(1, 22050, sine_pcm(100, 1, 22050, 440, 5000)));
    Bytes cut = make_wav(1, 22050, sine_pcm(40000, 1, 22050, 440, 5000));
    cut.resize(cut.size() / 2); // a download that stopped half way: the header still promises the whole file
    lib.add_file("cut.wav", cut);
    lib.slot("junk", "junk.wav");
    lib.slot("empty", "empty.wav");
    lib.slot("nofile");
    lib.slot("cut", "cut.wav");
    MusicSlots slots(lib);
    CHECK(slots.resolve({"junk"}).kind == MusicChoice::Original);
    CHECK(slots.resolve({"empty"}).kind == MusicChoice::Original);
    CHECK(slots.resolve({"nofile"}).kind == MusicChoice::Original);
    CHECK(slots.take_report().messages.size() == 3);
    // A damaged tail must not crash the player: whatever cannot be read is silence.
    MusicChoice c = slots.resolve({"cut"});
    if (c.kind == MusicChoice::Track) {
        CdPlayer p;
        p.configure(22050, 0);
        p.play(c.spec, 1);
        std::vector<int16_t> o = render(p, 45000);
        CHECK(o.size() == 90000);
    } else {
        CHECK(!slots.take_report().messages.empty());
    }
}

void test_fixture_formats() {
    if (g_fixtures.empty()) return;
    TestLibrary lib;
    struct F { const char* file; int rate; double hz; };
    const F files[] = {{"tone_mono_22050.flac", 22050, 440}, {"tone_stereo_44100_24bit.flac", 44100, 880},
                       {"tone_mono_22050.ogg", 22050, 440}, {"tone_stereo_44100.ogg", 44100, 880}};
    for (const F& f : files) {
        Bytes b = load_fixture(f.file);
        CHECK(!b.empty());
        lib.add_file(f.file, std::move(b));
        pack::MusicSlot& s = lib.slot(f.file, f.file);
        s.loop.kind = pack::LoopKind::Whole;
    }
    MusicSlots slots(lib);
    for (const F& f : files) {
        MusicChoice c = slots.resolve({f.file});
        CHECK(c.kind == MusicChoice::Track && c.spec->rate == f.rate);
        if (c.kind != MusicChoice::Track) continue;
        CdPlayer p;
        p.configure(48000, 0);
        p.play(c.spec, 1);
        std::vector<int16_t> o = render(p, 20000); // several loops
        // The tones are about -6 dB (0.5 x 32768 or so): a loud, clean sine at the right pitch right through the loop wraps.
        double peak = 0;
        for (size_t i = 2000; i < 18000; ++i) peak = std::max(peak, std::fabs(double(o[i * 2])));
        std::printf("  %s: peak %.0f\n", f.file, peak);
        CHECK(peak > 1500);
        // Count rising zero crossings in a window: 440 Hz or 880 Hz.
        int up = 0;
        for (size_t i = 3001; i < 3000 + 24000 / 2; ++i) up += o[(i - 1) * 2] < 0 && o[i * 2] >= 0;
        const double seconds = (24000.0 / 2 - 1) / 48000.0;
        CHECK(std::fabs(up / seconds - f.hz) < f.hz * 0.12);
    }
    CHECK(slots.take_report().messages.empty());
}

// ---- the cue layer ---------------------------------------------------------------------------------------------------

struct PackSetup {
    PackSetup(std::shared_ptr<TestLibrary> l, bool audio = true) : lib(std::move(l)) {
        pack::AssetPackSettings s;
        s.audio_upgrade = audio;
        pack::set_pack_settings(s);
        pack::set_packs(lib);
    }
    ~PackSetup() {
        pack::set_packs(nullptr);
        pack::set_pack_settings(pack::AssetPackSettings());
    }
    std::shared_ptr<TestLibrary> lib;
};

double frame_rms(const std::vector<int16_t>& v) {
    double s = 0;
    for (size_t i = 0; i < v.size() / 2; ++i) s += double(v[i * 2]) * v[i * 2];
    return v.empty() ? 0.0 : std::sqrt(s / double(v.size() / 2));
}

void run_frames(Machine& m, cdtv::CdtvMusic& music, int n, std::vector<int16_t>* out = nullptr) {
    for (int i = 0; i < n; ++i) {
        m.run_frame();
        music.after_frame(m);
        if (out) out->insert(out->end(), m.paula.output().begin(), m.paula.output().end());
        m.paula.output().clear(); // the consumer's job in the player
    }
}

void test_cdtv_cues() {
    auto lib = std::make_shared<TestLibrary>();
    lib->add_file("title.wav", make_wav(2, 48000, sine_pcm(480000, 2, 48000, 500, 12000)));
    lib->add_file("l3.wav", make_wav(2, 48000, sine_pcm(480000, 2, 48000, 700, 12000)));
    lib->add_file("short.wav", make_wav(2, 48000, sine_pcm(24000, 2, 48000, 900, 12000))); // 0.5 s
    lib->slot("title", "title.wav").loop.kind = pack::LoopKind::Whole;
    lib->slot("level3", "l3.wav").loop.kind = pack::LoopKind::Whole;
    lib->slot("cd9", "l3.wav").loop.kind = pack::LoopKind::Whole;
    lib->slot("cd12").mute = true;
    {
        pack::MusicSlot& s = lib->slot("cd13", "short.wav");
        s.loop_mode = pack::LoopMode::Game;
    }
    {
        pack::MusicSlot& s = lib->slot("cd10", "l3.wav");
        s.loop.kind = pack::LoopKind::Whole;
        s.retrigger = pack::Retrigger::Continue;
    }
    PackSetup setup(lib);

    Machine m(1024 * 1024);
    m.paula.set_output_enabled(true);
    m.start_cpu(0x1000, 0x2000, 0x2000);
    m.write16(0x1000, 0x60FE);
    cdtv::CdtvMusic music;
    music.attach_game(m);
    music.player().configure(48000, 0);
    // No CD at all: the pack's music still plays, with nothing but the pack to say so.
    CHECK(music.effective_source() == cdtv::MusicSource::Amiga);

    // Title screen caller, track 2: `title`.
    music.cue_play(m, 2, 0x03C822);
    std::vector<int16_t> o;
    run_frames(m, music, 3, &o);
    CHECK(music.pack_owns_music() && music.pack_slot() == "title" && music.player().track() == 2 && frame_rms(o) > 3000);

    // A level start with the level word 3 and track 3: `cd3` absent, `level3` plays; the game stops before it plays.
    m.write16(cdtv::kCdtvLevelWord, 3);
    music.cue_stop(m, 0x03ADB6);
    music.cue_play(m, 3, 0x03ADB6);
    run_frames(m, music, 3);
    CHECK(music.pack_owns_music() && music.pack_slot() == "level3");

    // A CD-only screen's track with no slot: the original (CD, here none), the pack stops.
    music.cue_stop(m, 0x5047E);
    music.cue_play(m, 8, 0x5047E);
    run_frames(m, music, 2); // the old track's 5 ms fade
    o.clear();
    run_frames(m, music, 3, &o);
    CHECK(!music.pack_owns_music() && music.pack_slot().empty() && music.player().idle() && frame_rms(o) == 0.0);

    // The exact `cdN` slot beats every alias.
    m.write16(cdtv::kCdtvLevelWord, 3);
    music.cue_stop(m, 0x03ADB6);
    music.cue_play(m, 9, 0x03ADB6);
    run_frames(m, music, 2);
    CHECK(music.pack_slot() == "cd9");

    // `mute`: silence, the original not played either.
    music.cue_stop(m, 0x5047E);
    music.cue_play(m, 12, 0x5047E);
    run_frames(m, music, 2);
    o.clear();
    run_frames(m, music, 3, &o);
    CHECK(music.pack_owns_music() && music.pack_slot() == "cd12" && music.player().idle() && frame_rms(o) == 0.0);

    // Retrigger: a restart cue (stop, then the same slot) plays on with `continue`, starts over otherwise.
    music.cue_play(m, 10, 0x5047E);
    run_frames(m, music, 5);
    const int64_t before = music.player().position();
    CHECK(before > 4000);
    music.cue_stop(m, 0x5047E);
    music.cue_play(m, 10, 0x5047E);
    run_frames(m, music, 1);
    CHECK(music.player().position() > before); // kept playing
    music.cue_play(m, 9, 0x5047E); // restart (default): from the start
    run_frames(m, music, 1);
    const int64_t first = music.player().position();
    run_frames(m, music, 5);
    music.cue_stop(m, 0x5047E);
    music.cue_play(m, 9, 0x5047E);
    run_frames(m, music, 1);
    CHECK(music.player().position() < first + 1500 && music.player().position() > 0);

    // The game's own loop rule on a pack track: finished after 0.5 s, started again by the game's poll.
    music.cue_stop(m, 0x5047E);
    music.cue_play(m, 13, 0x5047E);
    run_frames(m, music, 1);
    CHECK(!music.cue_poll(m, 13));
    run_frames(m, music, 35);
    CHECK(music.player().finished());
    CHECK(music.cue_poll(m, 13) && music.restarts() == 1);
    run_frames(m, music, 1);
    CHECK(!music.player().finished() && music.player().position() < 3000);

    // F1 (faithful) and the audio toggle: the pack steps aside at once, and comes back.
    music.cue_stop(m, 0x5047E);
    music.cue_play(m, 9, 0x5047E);
    run_frames(m, music, 3);
    CHECK(music.pack_owns_music());
    pack::AssetPackSettings off = pack::pack_settings();
    off.faithful = true;
    pack::set_pack_settings(off);
    run_frames(m, music, 2);
    o.clear();
    run_frames(m, music, 3, &o);
    CHECK(!music.pack_owns_music() && music.player().idle() && frame_rms(o) == 0.0);
    off.faithful = false;
    pack::set_pack_settings(off);
    run_frames(m, music, 3);
    CHECK(music.pack_owns_music() && music.pack_slot() == "cd9");
    off.audio_upgrade = false;
    pack::set_pack_settings(off);
    run_frames(m, music, 2);
    CHECK(!music.pack_owns_music());

    // reset(): the game starts over.
    off.audio_upgrade = true;
    pack::set_pack_settings(off);
    music.reset();
    CHECK(!music.pack_owns_music() && music.player().track() == 0);
}

void test_disk_cues() {
    // Disk build: the tracker driver's entry point is a trap. A synthetic program calls it, and one Paula channel plays a loud
    // "tracker" tone; with a pack slot the tone must be gone from the mix from the first frame, and the pack track must play.
    auto make = [](std::shared_ptr<TestLibrary> lib, bool title, std::vector<int16_t>& out, std::string* slot, bool* owns) {
        PackSetup setup(lib);
        Machine m(1024 * 1024);
        m.paula.set_output_enabled(true);
        for (size_t i = 0; i < 0x2000; ++i) m.write8(0x4000 + uint32_t(i), uint8_t(int8_t(std::lround(110 * std::sin(double(i) * 0.12)))));
        m.write16(0x19F9E, 0x4E75); // the driver's music_init: RTS
        m.write32(cdtv::kMusicPtr, cdtv::kMusicTick); // the resident tick is installed: writes count as music
        const uint32_t at = title ? 0x8230 : 0x1000;
        m.write16(at, 0x4EB9);
        m.write32(at + 2, cdtv::kMusicInit); // JSR music_init
        m.write16(at + 6, 0x60FE);
        m.write16(cdtv::kLevelWord, 2);
        m.start_cpu(at, 0x2000, 0x2000);
        cdtv::CdtvMusic music;
        music.install(m);
        music.player().configure(48000, 0);
        m.paula.write_reg(0xA0, 0);
        m.paula.write_reg(0xA2, 0x4000);
        m.paula.write_reg(0xA4, 0x1000);
        m.paula.write_reg(0xA6, 200);
        m.paula.write_reg(0xA8, 64);
        m.paula.set_dmacon(0x8201);
        for (int i = 0; i < 6; ++i) {
            m.run_frame();
            music.after_frame(m);
            out.insert(out.end(), m.paula.output().begin(), m.paula.output().end());
            m.paula.output().clear();
        }
        if (slot) *slot = music.pack_slot();
        if (owns) *owns = music.pack_owns_music();
    };
    std::vector<int16_t> plain, muted, packed, title;
    std::string slot;
    bool owns = false;
    make(std::make_shared<TestLibrary>(), false, plain, &slot, &owns);
    CHECK(!owns && frame_rms(plain) > 1000); // the tracker is audible without a pack

    auto lib = std::make_shared<TestLibrary>();
    lib->slot("level2").mute = true;
    make(lib, false, muted, &slot, &owns);
    CHECK(owns && slot == "level2" && frame_rms(muted) == 0.0); // gone from the first sample, nothing else plays

    lib = std::make_shared<TestLibrary>();
    lib->add_file("l.wav", make_wav(2, 48000, sine_pcm(96000, 2, 48000, 440, 9000)));
    lib->slot("level", "l.wav").loop.kind = pack::LoopKind::Whole;
    make(lib, false, packed, &slot, &owns);
    CHECK(owns && slot == "level" && frame_rms(packed) > 3000 && frame_rms(packed) < 9000);
    // The pack track is the whole mix: the tone is a clean 440 Hz sine, no tracker underneath.
    std::vector<int16_t> tail(packed.end() - 4000, packed.end());
    int up = 0;
    for (size_t i = 1; i < tail.size() / 2; ++i) up += tail[(i - 1) * 2] < 0 && tail[i * 2] >= 0;
    CHECK(std::abs(up - int(440.0 * (double(tail.size() / 2)) / 48000.0)) <= 1);

    // The title cue (called from $8230) looks for `title`, not `level`.
    make(lib, true, title, &slot, &owns);
    CHECK(!owns && frame_rms(title) > 1000); // only `level` exists: the tracker plays the title
    lib->slot("title").mute = true;
    std::vector<int16_t> title2;
    make(lib, true, title2, &slot, &owns);
    CHECK(owns && slot == "title" && frame_rms(title2) == 0.0);
}

// `--write-pack DIR GAME`: the synthetic pack of the data tests (tests/audio/music_pack_check.cmake). Integer-only waveforms, so the
// files (and the recorded audio hashes of the replays that play them) are the same on every platform.
int write_pack(const std::string& dir, const std::string& game) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(dir) / "music", ec);
    auto wave = [](uint64_t frames, int channels, int rate, int period, int amp, bool square) {
        std::vector<int16_t> v(frames * size_t(channels));
        for (uint64_t i = 0; i < frames; ++i) {
            const int ph = int(i % uint64_t(period));
            const int tri = ph < period / 2 ? -amp + 4 * amp * ph / period : 3 * amp - 4 * amp * ph / period;
            for (int c = 0; c < channels; ++c) v[i * size_t(channels) + size_t(c)] = int16_t(square ? (ph < period / 2 ? amp : -amp) : tri);
        }
        return make_wav(channels, rate, v);
    };
    auto write = [&](const char* name, const Bytes& b) {
        std::ofstream f(fs::path(dir) / "music" / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        return bool(f);
    };
    bool ok = write("title.wav", wave(132300, 2, 44100, 134, 9000, false));  // 3 s, ~329 Hz triangle, stereo, the output's own rate family
    ok = write("level.wav", wave(88200, 1, 22050, 40, 7000, true)) && ok;     // 4 s, ~551 Hz square, mono, resampled
    ok = write("intro.wav", wave(11025, 1, 22050, 25, 7000, false)) && ok;    // 0.5 s intro
    std::ofstream m(fs::path(dir) / "manifest.json");
    m << "{\n \"format\": \"x2-asset-pack\", \"version\": 2, \"id\": \"test.music\", \"pack_version\": \"1.0.0\", \"name\": \"Synthetic music\",\n"
         " \"license\": \"CC0-1.0\", \"provides\": [\"audio\"], \"games\": [\"" << game << "\"],\n"
         " \"audio\": {\"music\": {\"defaults\": {\"crossfade_ms\": 0}, \"slots\": {\n"
         "  \"title\": {\"file\": \"music/title.wav\", \"loop\": true},\n"
         "  \"level1\": {\"intro\": \"music/intro.wav\", \"file\": \"music/level.wav\", \"loop\": true, \"crossfade_ms\": 100},\n"
         "  \"level\": {\"intro\": \"music/intro.wav\", \"file\": \"music/level.wav\", \"loop\": {\"start_s\": 0.25}}\n"
         " }}}\n}\n";
    return ok && bool(m) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 4 && std::string(argv[2]) == "--write-pack") return write_pack(argv[3], argv[4]);
    if (argc > 1) g_fixtures = argv[1];
    test_streams();
    test_player_basics();
    test_wrapped_loop();
    test_crossfaded_loop();
    test_intro_loop_spec();
    test_slot_names();
    test_resolution();
    test_loop_decisions();
    test_intro_slot();
    test_streaming_file();
    test_damaged_files();
    test_fixture_formats();
    test_cdtv_cues();
    test_disk_cues();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
