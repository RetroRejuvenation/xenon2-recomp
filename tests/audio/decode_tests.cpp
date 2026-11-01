// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Audio decoder tests (runtime/audio/decode.*): WAV files built in memory, the synthesised FLAC and Ogg Vorbis tones in
// tests/audio/fixtures (argument 1), limits, loop metadata, the resampler, determinism and a mutation fuzz. No game audio.
// X2_AUDIO_FUZZ_ITERATIONS sets the number of fuzz mutations per file kind (default 3000).
#include "audio/decode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace x2::audio;

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

using Bytes = std::vector<uint8_t>;

std::string g_dir;

Bytes load(const std::string& name) {
    std::ifstream f(g_dir + "/" + name, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A view of a named buffer, or (for a temporary) a source that owns its bytes.
std::unique_ptr<ByteSource> src(const Bytes& b) { return memory_source(b.data(), b.size()); }
std::unique_ptr<ByteSource> src(Bytes&& b) { return memory_source(std::move(b)); }

uint64_t fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// ---- WAV builder ---------------------------------------------------------------------------------------------------

void put16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(Bytes& b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
void put_tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

struct Chunk { const char* id; Bytes body; };

struct Wav {
    int channels = 1, rate = 22050, bits = 16;
    int tag = 1; // 1 PCM, 3 float
    bool extensible = false;
    Bytes data;
    std::vector<Chunk> before, after; // extra chunks around the data chunk
    int64_t data_size_field = -1;       // -1 = the real size
    int block_align_delta = 0;
    bool riff64 = false;
};

Bytes fmt_body(const Wav& w) {
    Bytes f;
    put16(f, w.extensible ? 0xFFFE : unsigned(w.tag));
    put16(f, unsigned(w.channels));
    put32(f, unsigned(w.rate));
    const unsigned align = unsigned(w.channels * w.bits / 8);
    put32(f, unsigned(w.rate) * align);
    put16(f, unsigned(int(align) + w.block_align_delta));
    put16(f, unsigned(w.bits));
    if (w.extensible) {
        put16(f, 22);
        put16(f, unsigned(w.bits));
        put32(f, w.channels == 1 ? 4u : 3u);
        put16(f, unsigned(w.tag));
        static const uint8_t g[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        f.insert(f.end(), g, g + 14); // the 14 bytes after the format tag (rest of the KSDATAFORMAT sub-format GUID)
    }
    return f;
}

Bytes build(const Wav& w) {
    Bytes b;
    put_tag(b, w.riff64 ? "RF64" : "RIFF");
    put32(b, 0);
    put_tag(b, "WAVE");
    auto chunk = [&](const char* id, const Bytes& body, int64_t size_field = -1) {
        put_tag(b, id);
        put32(b, size_field >= 0 ? uint32_t(size_field) : uint32_t(body.size()));
        b.insert(b.end(), body.begin(), body.end());
        if (body.size() & 1) b.push_back(0);
    };
    chunk("fmt ", fmt_body(w));
    for (const Chunk& c : w.before) chunk(c.id, c.body);
    chunk("data", w.data, w.data_size_field);
    for (const Chunk& c : w.after) chunk(c.id, c.body);
    put32(b, 0); // keep the size field harmless: parsers must not rely on it
    b.resize(b.size() - 4);
    const uint32_t riff = uint32_t(b.size() - 8);
    b[4] = uint8_t(riff); b[5] = uint8_t(riff >> 8); b[6] = uint8_t(riff >> 16); b[7] = uint8_t(riff >> 24);
    return b;
}

Bytes smpl_chunk(uint32_t loops, uint32_t type, uint32_t start, uint32_t end) {
    Bytes s;
    for (int i = 0; i < 7; ++i) put32(s, 0);       // manufacturer .. SMPTE offset
    put32(s, loops);                               // number of loops
    put32(s, 0);                                   // sampler data
    if (loops) {
        put32(s, 1); put32(s, type); put32(s, start); put32(s, end); put32(s, 0); put32(s, 0);
    }
    return s;
}

std::vector<float> sine(size_t frames, int rate, double freq, double amp) {
    std::vector<float> v(frames);
    for (size_t i = 0; i < frames; ++i) v[i] = float(amp * std::sin(2.0 * 3.14159265358979323846 * freq * double(i) / double(rate)));
    return v;
}

Bytes pcm16(const std::vector<float>& m, int channels = 1) {
    Bytes d;
    for (float f : m)
        for (int c = 0; c < channels; ++c) put16(d, uint16_t(float_to_s16(c ? -f : f)));
    return d;
}

// Zero crossings per second of a mono signal.
double frequency(const float* x, size_t n, int rate) {
    size_t z = 0;
    for (size_t i = 1; i < n; ++i)
        if ((x[i - 1] < 0) != (x[i] < 0)) ++z;
    return double(z) * 0.5 * double(rate) / double(n);
}

double rms(const float* x, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += double(x[i]) * double(x[i]);
    return std::sqrt(s / double(n ? n : 1));
}

bool decode_wav(const Bytes& b, FloatClip& out, std::string* err, const DecodeLimits& lim = DecodeLimits{}) {
    return decode_all_f32(src(b), lim, {}, out, err);
}

// ---- tests ---------------------------------------------------------------------------------------------------------

void test_memory_source() {
    const Bytes d = {1, 2, 3, 4, 5};
    auto s = memory_source(d.data(), d.size());
    uint8_t b[8];
    CHECK(s->size() == 5 && s->tell() == 0 && s->data() == d.data());
    CHECK(s->read(b, 3) == 3 && b[2] == 3 && s->tell() == 3);
    CHECK(s->read(b, 8) == 2 && b[1] == 5 && s->tell() == 5);
    CHECK(s->read(b, 1) == 0);
    CHECK(s->seek(1) && s->read(b, 1) == 1 && b[0] == 2);
    CHECK(s->seek(5) && !s->seek(6));
    CHECK(memory_source(nullptr, 0)->size() == 0);
    auto o = memory_source(Bytes{9, 8, 7});
    CHECK(o->size() == 3 && o->read(b, 8) == 3 && b[0] == 9 && b[2] == 7 && o->data() != nullptr);
}

void test_wav_formats() {
    // 16-bit mono: exact values
    {
        Wav w;
        w.data = {0x00, 0x00, 0x00, 0x40, 0x00, 0xC0, 0xFF, 0x7F, 0x00, 0x80};
        std::string err;
        auto d = AudioDecoder::open(src(build(w)), DecodeLimits{}, &err);
        CHECK(d && err.empty());
        if (d) {
            CHECK(d->info().format == Format::Wav && d->info().channels == 1 && d->info().sample_rate == 22050);
            CHECK(d->info().bits == 16 && d->info().frames == 5 && !d->info().loop);
            float o[8];
            CHECK(d->read(o, 8) == 5);
            CHECK(o[0] == 0.0f && o[1] == 0.5f && o[2] == -0.5f && o[3] == 32767.0f / 32768.0f && o[4] == -1.0f);
            CHECK(d->read(o, 8) == 0 && !d->failed() && d->position() == 5);
        }
    }
    // 8-bit unsigned, 24-bit, 32-bit, float 32 and 64
    {
        Wav w; w.bits = 8; w.data = {128, 255, 0, 192};
        FloatClip c; std::string err;
        CHECK(decode_wav(build(w), c, &err) && c.frames == 4);
        CHECK(c.pcm[0] == 0.0f && std::fabs(c.pcm[1] - 127.0f / 128.0f) < 1e-7f && c.pcm[2] == -1.0f && c.pcm[3] == 0.5f);
    }
    {
        Wav w; w.bits = 24;
        w.data = {0x00, 0x00, 0x40, /* 0.5 */ 0x00, 0x00, 0xC0, /* -0.5 */ 0xFF, 0xFF, 0x7F, /* ~1 */ 0x00, 0x00, 0x80 /* -1 */};
        FloatClip c; std::string err;
        CHECK(decode_wav(build(w), c, &err) && c.frames == 4);
        CHECK(c.pcm[0] == 0.5f && c.pcm[1] == -0.5f && c.pcm[2] == 8388607.0f / 8388608.0f && c.pcm[3] == -1.0f);
    }
    {
        Wav w; w.bits = 32;
        w.data = {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x80};
        FloatClip c; std::string err;
        CHECK(decode_wav(build(w), c, &err) && c.frames == 2 && c.pcm[0] == 0.5f && c.pcm[1] == -1.0f);
    }
    {
        Wav w; w.bits = 32; w.tag = 3;
        const float v[3] = {0.25f, -2.0f, NAN};
        w.data.resize(sizeof v);
        std::memcpy(w.data.data(), v, sizeof v);
        FloatClip c; std::string err;
        CHECK(decode_wav(build(w), c, &err) && c.frames == 3 && c.pcm[0] == 0.25f && c.pcm[1] == -2.0f && c.pcm[2] == 0.0f);
        CHECK(c.peak == 2.0f);
        DecodedClip c16;
        CHECK(decode_all(src(build(w)), DecodeLimits{}, {}, c16, &err) && c16.pcm[1] == -32768 && c16.pcm[0] == 8192 && c16.peak == 2.0f);
    }
    {
        Wav w; w.bits = 64; w.tag = 3;
        const double v[2] = {0.5, -0.125};
        w.data.resize(sizeof v);
        std::memcpy(w.data.data(), v, sizeof v);
        FloatClip c; std::string err;
        CHECK(decode_wav(build(w), c, &err) && c.frames == 2 && c.pcm[0] == 0.5f && c.pcm[1] == -0.125f);
    }
    // stereo, extensible, extra chunks, odd-sized chunk padding, partial last frame, data size field 0xFFFFFFFF and 0
    {
        Wav w; w.channels = 2; w.extensible = true;
        w.data = {0x00, 0x40, 0x00, 0xC0, 0x00, 0x20, 0x00, 0xE0, 0x7F}; // 2 stereo frames and one stray byte
        w.before.push_back({"LIST", Bytes{1, 2, 3}});                    // odd size: padded
        w.before.push_back({"junk", Bytes(10, 7)});
        w.after.push_back({"id3 ", Bytes(5, 1)});
        FloatClip c; std::string err;
        CHECK(decode_wav(build(w), c, &err) && c.channels == 2 && c.frames == 2);
        CHECK(c.pcm[0] == 0.5f && c.pcm[1] == -0.5f && c.pcm[2] == 0.25f && c.pcm[3] == -0.25f);
    }
    for (const int64_t field : {int64_t(0xFFFFFFFFu), int64_t(0)}) {
        Wav w; w.data = pcm16(sine(100, 22050, 440, 0.5)); w.data_size_field = field;
        FloatClip c; std::string err;
        CHECK(decode_wav(build(w), c, &err) && c.frames == 100);
    }
    // conversion to stereo and a new rate
    {
        Wav w; w.data = pcm16(sine(2205, 22050, 440, 0.5));
        DecodedClip c; std::string err;
        CHECK(decode_all(src(build(w)), DecodeLimits{}, {2, 44100}, c, &err));
        CHECK(c.channels == 2 && c.sample_rate == 44100 && c.frames == 4410 && c.pcm.size() == 8820);
        CHECK(c.pcm[2 * 1000] == c.pcm[2 * 1000 + 1]);
        CHECK(c.source.sample_rate == 22050 && c.source.frames == 2205);
    }
}

void test_wav_errors() {
    auto expect_error = [&](const Bytes& b, const char* needle, const DecodeLimits& lim = DecodeLimits{}) {
        std::string err;
        FloatClip c;
        const bool ok = decode_wav(b, c, &err, lim);
        CHECK(!ok);
        CHECK(err.find(needle) != std::string::npos);
        if (err.find(needle) == std::string::npos) std::fprintf(stderr, "    got: %s (wanted '%s')\n", err.c_str(), needle);
    };
    Wav base; base.data = pcm16(sine(100, 22050, 440, 0.5));
    { Wav w = base; w.channels = 3; w.data = Bytes(60, 0); expect_error(build(w), "3 channels"); }
    { Wav w = base; w.channels = 0; expect_error(build(w), "no audio channels"); }
    { Wav w = base; w.bits = 12; expect_error(build(w), "12-bit"); }
    { Wav w = base; w.tag = 3; w.bits = 16; expect_error(build(w), "16-bit float"); }
    { Wav w = base; w.tag = 2; expect_error(build(w), "format tag 2"); }
    { Wav w = base; w.rate = 100000000; expect_error(build(w), "sample rate"); }
    { Wav w = base; w.rate = 4000; expect_error(build(w), "sample rate"); }
    { Wav w = base; w.block_align_delta = 1; expect_error(build(w), "block alignment"); }
    { Wav w = base; w.data_size_field = 100000; expect_error(build(w), "truncated"); }
    { Wav w = base; w.riff64 = true; expect_error(build(w), "RF64"); }
    { Wav w = base; w.data.clear(); expect_error(build(w), "no audio"); }
    { Wav w = base; w.extensible = true; w.channels = 2; w.data = Bytes(8, 0); Bytes b = build(w); b[12 + 8 + 26] ^= 0x55; expect_error(b, "sub-format"); }
    {   // no data chunk
        Bytes b; put_tag(b, "RIFF"); put32(b, 4); put_tag(b, "WAVE");
        put_tag(b, "fmt "); put32(b, 16); Wav w; Bytes f = fmt_body(w); b.insert(b.end(), f.begin(), f.end());
        expect_error(b, "no data chunk");
    }
    {   // no fmt chunk
        Bytes b; put_tag(b, "RIFF"); put32(b, 4); put_tag(b, "WAVE");
        put_tag(b, "data"); put32(b, 4); put32(b, 0);
        expect_error(b, "no fmt chunk");
    }
    {   // a chunk past the end before the audio
        Wav w = base; w.before.push_back({"LIST", Bytes(8, 1)});
        Bytes b = build(w);
        b[12 + 8 + 16 + 4] = 0xFF; b[12 + 8 + 16 + 5] = 0xFF; // the LIST chunk's size
        expect_error(b, "extends past");
    }
    { Bytes b = build(base); b.resize(30); expect_error(b, "WAV"); }
    expect_error(Bytes{1, 2}, "too short");
    expect_error(Bytes(64, 0x41), "unknown audio format");
    expect_error(Bytes{'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0, 0, 0}, "ID3");
    {   // limits
        DecodeLimits l; l.max_channels = 1;
        Wav w = base; w.channels = 2; w.data = Bytes(400, 0);
        expect_error(build(w), "2 channels", l);
        DecodeLimits f; f.max_frames = 50;
        expect_error(build(base), "longer than the limit", f);
        DecodeLimits big; big.max_file_bytes = 100;
        expect_error(build(base), "larger than the limit", big);
        DecodeLimits mem; mem.budget = 100; // the decoded audio (100 frames * 4 bytes) does not fit
        expect_error(build(base), "memory limit", mem);
    }
    {   // an empty and a null source
        std::string err;
        CHECK(!AudioDecoder::open(nullptr, DecodeLimits{}, &err) && !err.empty());
        CHECK(!AudioDecoder::open(memory_source(nullptr, 0), DecodeLimits{}, &err));
    }
}

void test_wav_loop_and_seek() {
    Wav w; w.data = pcm16(sine(1000, 22050, 440, 0.5));
    w.after.push_back({"smpl", smpl_chunk(1, 0, 100, 899)});
    std::string err;
    auto d = AudioDecoder::open(src(build(w)), DecodeLimits{}, &err);
    CHECK(d && d->info().loop && d->info().loop->start == 100 && d->info().loop->end == 900 && d->info().loop_note == "smpl chunk");
    { // loop outside the file, a ping-pong loop and no loops are ignored
        Wav a = w; a.after = {{"smpl", smpl_chunk(1, 0, 100, 5000)}};
        auto e = AudioDecoder::open(src(build(a)), DecodeLimits{}, &err);
        CHECK(e && !e->info().loop && !e->info().loop_note.empty());
        Wav b = w; b.after = {{"smpl", smpl_chunk(1, 1, 100, 500)}};
        auto f = AudioDecoder::open(src(build(b)), DecodeLimits{}, &err);
        CHECK(f && !f->info().loop);
        Wav c = w; c.after = {{"smpl", smpl_chunk(0, 0, 0, 0)}};
        auto g = AudioDecoder::open(src(build(c)), DecodeLimits{}, &err);
        CHECK(g && !g->info().loop);
    }
    { // a smpl chunk before the data is found too
        Wav a = w; a.after.clear(); a.before = {{"smpl", smpl_chunk(1, 0, 10, 19)}};
        auto e = AudioDecoder::open(src(build(a)), DecodeLimits{}, &err);
        CHECK(e && e->info().loop && e->info().loop->start == 10 && e->info().loop->end == 20);
    }
    // seek
    if (d) {
        std::vector<float> all(1000);
        CHECK(d->read(all.data(), 1000) == 1000);
        CHECK(d->seek(500) && d->position() == 0);
        float o[10];
        CHECK(d->read(o, 10) == 10 && std::memcmp(o, &all[500], sizeof o) == 0 && d->position() == 10);
        CHECK(d->seek(1000) && d->read(o, 10) == 0);
        CHECK(!d->seek(1001));
        CHECK(d->seek(0) && d->read(o, 10) == 10 && std::memcmp(o, all.data(), sizeof o) == 0);
    }
}

void test_streaming_matches_decode_all(const Bytes& file, const char* what) {
    std::string err;
    FloatClip all;
    const bool ok = decode_all_f32(src(file), DecodeLimits{}, {}, all, &err);
    CHECK(ok);
    if (!ok) { std::fprintf(stderr, "    %s: %s\n", what, err.c_str()); return; }
    for (size_t chunk : {size_t(1), size_t(7), size_t(1000), size_t(4096)}) {
        auto d = AudioDecoder::open(src(file), DecodeLimits{}, &err);
        CHECK(d != nullptr);
        if (!d) return;
        std::vector<float> got;
        std::vector<float> buf(chunk * size_t(d->info().channels));
        for (;;) {
            const size_t n = d->read(buf.data(), chunk);
            if (!n) break;
            got.insert(got.end(), buf.begin(), buf.begin() + std::ptrdiff_t(n * size_t(d->info().channels)));
            if (chunk == 1 && got.size() > 20000) break; // keep the one-frame case short
        }
        CHECK(!d->failed());
        const size_t n = std::min(got.size(), all.pcm.size());
        CHECK(n > 0 && std::memcmp(got.data(), all.pcm.data(), n * sizeof(float)) == 0);
    }
}

// ---- FLAC and Vorbis fixtures ---------------------------------------------------------------------------------------

void check_tone(const char* name, Format fmt, int channels, int rate, int64_t frames, int bits, double freq, double peak_lo, double peak_hi) {
    const Bytes b = load(name);
    CHECK(!b.empty());
    if (b.empty()) { std::fprintf(stderr, "    missing fixture %s\n", name); return; }
    std::string err;
    FloatClip c;
    const bool ok = decode_all_f32(src(b), DecodeLimits{}, {}, c, &err);
    CHECK(ok);
    if (!ok) { std::fprintf(stderr, "    %s: %s\n", name, err.c_str()); return; }
    CHECK(c.source.format == fmt && c.channels == channels && c.sample_rate == rate && c.frames == uint64_t(frames));
    CHECK(c.source.bits == bits && c.source.frames == frames);
    CHECK(c.peak >= peak_lo && c.peak <= peak_hi);
    if (!(c.peak >= peak_lo && c.peak <= peak_hi)) std::fprintf(stderr, "    %s: peak %.4f (wanted %.3f..%.3f)\n", name, double(c.peak), peak_lo, peak_hi);
    std::vector<float> ch0(c.frames);
    for (uint64_t i = 0; i < c.frames; ++i) ch0[size_t(i)] = c.pcm[size_t(i) * size_t(channels)];
    const double f = frequency(ch0.data(), ch0.size(), rate);
    CHECK(std::fabs(f - freq) < freq * 0.03);
    if (std::fabs(f - freq) >= freq * 0.03) std::fprintf(stderr, "    %s: frequency %.1f (wanted %.1f)\n", name, f, freq);
    if (channels == 2) { // the fixture is the same tone in both channels
        double d = 0;
        for (uint64_t i = 0; i < c.frames; ++i) d = std::max(d, double(std::fabs(c.pcm[2 * size_t(i)] - c.pcm[2 * size_t(i) + 1])));
        CHECK(d < 0.02);
    }
}

void test_fixtures() {
    // The ffmpeg `sine` source has an amplitude of 1/8; the stereo fixtures went through a pan filter that scales it by 1/sqrt(2).
    check_tone("tone_mono_22050.flac", Format::Flac, 1, 22050, 2205, 16, 440, 0.12, 0.13);
    check_tone("tone_stereo_44100_24bit.flac", Format::Flac, 2, 44100, 4410, 24, 880, 0.085, 0.092);
    check_tone("tone_mono_22050.ogg", Format::Vorbis, 1, 22050, 2205, 0, 440, 0.10, 0.15);
    check_tone("tone_stereo_44100.ogg", Format::Vorbis, 2, 44100, 4410, 0, 880, 0.08, 0.11);
    for (const char* n : {"tone_mono_22050.flac", "tone_stereo_44100_24bit.flac", "tone_mono_22050.ogg", "tone_stereo_44100.ogg"})
        test_streaming_matches_decode_all(load(n), n);

    std::string err;
    {   // loop tags: LOOPSTART + LOOPLENGTH (FLAC) and LOOPSTART + LOOPEND (Vorbis)
        auto f = AudioDecoder::open(src(load("tone_loop_22050.flac")), DecodeLimits{}, &err);
        CHECK(f && f->info().loop && f->info().loop->start == 100 && f->info().loop->end == 1100 && f->info().loop_note == "tags");
        auto o = AudioDecoder::open(src(load("tone_loop_22050.ogg")), DecodeLimits{}, &err);
        CHECK(o && o->info().loop && o->info().loop->start == 100 && o->info().loop->end == 1500);
        auto n = AudioDecoder::open(src(load("tone_mono_22050.flac")), DecodeLimits{}, &err);
        CHECK(n && !n->info().loop && n->info().loop_note.empty());
    }
    {   // seek: FLAC is exact; Vorbis lands on the same samples
        for (const char* name : {"tone_mono_22050.flac", "tone_mono_22050.ogg"}) {
            auto d = AudioDecoder::open(src(load(name)), DecodeLimits{}, &err);
            CHECK(d != nullptr);
            if (!d) continue;
            std::vector<float> all(2205), part(100);
            CHECK(d->read(all.data(), 2205) == 2205);
            CHECK(d->seek(1000));
            CHECK(d->read(part.data(), 100) == 100);
            double worst = 0;
            for (int i = 0; i < 100; ++i) worst = std::max(worst, double(std::fabs(part[size_t(i)] - all[1000 + size_t(i)])));
            CHECK(worst < 1e-5);
            CHECK(d->seek(0) && d->read(part.data(), 100) == 100 && std::fabs(part[50] - all[50]) < 1e-5f);
            CHECK(!d->seek(2206));
        }
    }
    {   // limits reach the vendored decoders
        const Bytes flac = load("tone_mono_22050.flac"), ogg = load("tone_mono_22050.ogg");
        DecodeLimits tiny; tiny.budget = 1000;
        CHECK(!AudioDecoder::open(src(flac), tiny, &err) && !err.empty());
        CHECK(!AudioDecoder::open(src(ogg), tiny, &err) && !err.empty());
        DecodeLimits ch1; ch1.max_channels = 1;
        CHECK(!AudioDecoder::open(src(load("tone_stereo_44100.ogg")), ch1, &err) && err.find("2 channels") != std::string::npos);
        CHECK(!AudioDecoder::open(src(load("tone_stereo_44100_24bit.flac")), ch1, &err) && err.find("2 channels") != std::string::npos);
        DecodeLimits rate; rate.max_sample_rate = 22000;
        CHECK(!AudioDecoder::open(src(flac), rate, &err) && err.find("sample rate") != std::string::npos);
        CHECK(!AudioDecoder::open(src(ogg), rate, &err) && err.find("sample rate") != std::string::npos);
        DecodeLimits frames; frames.max_frames = 1000;
        CHECK(!AudioDecoder::open(src(flac), frames, &err) && err.find("longer than") != std::string::npos);
        CHECK(!AudioDecoder::open(src(ogg), frames, &err) && err.find("longer than") != std::string::npos);
        DecodeLimits file; file.max_file_bytes = 500;
        CHECK(!AudioDecoder::open(src(flac), file, &err) && !AudioDecoder::open(src(ogg), file, &err));
        DecodeLimits out_budget; out_budget.budget = 8 << 10; // decoder memory fits, the decoded 2205 mono floats (8.8 KB) do not
        FloatClip c;
        CHECK(!decode_all_f32(src(flac), out_budget, {}, c, &err));
    }
    {   // truncated and damaged files are errors, not partial successes
        Bytes flac = load("tone_mono_22050.flac");
        flac.resize(flac.size() - 400);
        FloatClip c;
        CHECK(!decode_all_f32(src(flac), DecodeLimits{}, {}, c, &err));
        Bytes ogg = load("tone_mono_22050.ogg");
        ogg.resize(ogg.size() / 2);
        CHECK(!decode_all_f32(src(ogg), DecodeLimits{}, {}, c, &err));
        Bytes bad = load("tone_mono_22050.flac");
        bad[0] = 'X';
        CHECK(!AudioDecoder::open(src(bad), DecodeLimits{}, &err));
        Bytes badogg = load("tone_mono_22050.ogg");
        badogg[1] = 'X';
        CHECK(!AudioDecoder::open(src(badogg), DecodeLimits{}, &err));
    }
    {   // a source with no contiguous block (Vorbis reads it into memory first)
        struct Piecewise final : ByteSource {
            const Bytes& b; size_t pos = 0;
            explicit Piecewise(const Bytes& x) : b(x) {}
            size_t read(void* d, size_t n) override { n = std::min(n, b.size() - pos); std::memcpy(d, b.data() + pos, n); pos += n; return n; }
            bool seek(uint64_t p) override { if (p > b.size()) return false; pos = size_t(p); return true; }
            uint64_t tell() const override { return pos; }
            uint64_t size() const override { return b.size(); }
        };
        const Bytes ogg = load("tone_mono_22050.ogg"), flac = load("tone_mono_22050.flac");
        FloatClip a, b2, c, d;
        CHECK(decode_all_f32(src(ogg), DecodeLimits{}, {}, a, &err));
        CHECK(decode_all_f32(std::make_unique<Piecewise>(ogg), DecodeLimits{}, {}, b2, &err));
        CHECK(a.pcm == b2.pcm && a.frames == 2205);
        CHECK(decode_all_f32(src(flac), DecodeLimits{}, {}, c, &err));
        CHECK(decode_all_f32(std::make_unique<Piecewise>(flac), DecodeLimits{}, {}, d, &err));
        CHECK(c.pcm == d.pcm);
        DecodeLimits small; small.budget = 1000;
        CHECK(!AudioDecoder::open(std::make_unique<Piecewise>(ogg), small, &err));
    }
}

// ---- resampler -----------------------------------------------------------------------------------------------------

std::vector<float> resample_all(const std::vector<float>& in, int channels, int in_rate, int out_rate, size_t push_chunk = 0, size_t pull_chunk = 4096) {
    StreamResampler r(channels, in_rate, out_rate);
    std::vector<float> out;
    std::vector<float> tmp(pull_chunk * size_t(channels));
    const size_t frames = in.size() / size_t(channels);
    size_t at = 0;
    const size_t step = push_chunk ? push_chunk : frames;
    while (at < frames) {
        const size_t n = std::min(step, frames - at);
        r.push(in.data() + at * size_t(channels), n);
        at += n;
        for (size_t k; (k = r.pull(tmp.data(), pull_chunk)) > 0;) out.insert(out.end(), tmp.begin(), tmp.begin() + std::ptrdiff_t(k * size_t(channels)));
    }
    r.finish();
    for (size_t k; (k = r.pull(tmp.data(), pull_chunk)) > 0;) out.insert(out.end(), tmp.begin(), tmp.begin() + std::ptrdiff_t(k * size_t(channels)));
    CHECK(r.done());
    return out;
}

void test_resampler() {
    // frame counts
    CHECK(StreamResampler::output_frames(2205, 22050, 48000) == 4800);
    CHECK(StreamResampler::output_frames(1000, 44100, 48000) == 1089);
    CHECK(StreamResampler::output_frames(1, 48000, 8000) == 1);
    CHECK(StreamResampler::output_frames(0, 48000, 8000) == 0);
    // identity copies exactly
    {
        const auto x = sine(1000, 48000, 440, 0.7);
        const auto y = resample_all(x, 1, 48000, 48000, 77, 33);
        CHECK(y == x);
    }
    // a sine keeps its frequency and level, up and down
    for (auto [in_rate, out_rate] : {std::pair{22050, 48000}, std::pair{44100, 48000}, std::pair{48000, 44100}, std::pair{48000, 22050}, std::pair{8000, 48000}, std::pair{192000, 48000}}) {
        const double freq = 440.0;
        const size_t frames = size_t(in_rate) / 2;
        const auto x = sine(frames, in_rate, freq, 0.5);
        const auto y = resample_all(x, 1, in_rate, out_rate);
        CHECK(y.size() == StreamResampler::output_frames(frames, in_rate, out_rate));
        // the middle half, away from the edges
        const size_t a = y.size() / 4, n = y.size() / 2;
        const double f = frequency(&y[a], n, out_rate);
        CHECK(std::fabs(f - freq) < freq * 0.02);
        const double r = rms(&y[a], n), want = 0.5 / std::sqrt(2.0);
        CHECK(std::fabs(r - want) < want * 0.01);
        if (std::fabs(r - want) >= want * 0.01 || std::fabs(f - freq) >= freq * 0.02)
            std::fprintf(stderr, "    %d -> %d: frequency %.1f rms %.4f (wanted %.4f)\n", in_rate, out_rate, f, r, want);
    }
    // DC passes unchanged (away from the edges)
    {
        const std::vector<float> x(4000, 0.5f);
        const auto y = resample_all(x, 1, 22050, 48000);
        double worst = 0;
        for (size_t i = 200; i + 200 < y.size(); ++i) worst = std::max(worst, double(std::fabs(y[i] - 0.5f)));
        CHECK(worst < 1e-4);
    }
    // content above the new Nyquist frequency is removed
    {
        const auto x = sine(48000, 48000, 15000, 0.5);
        const auto y = resample_all(x, 1, 48000, 22050);
        CHECK(rms(&y[2000], y.size() - 4000) < 0.5 / std::sqrt(2.0) * 0.02);
    }
    // the result does not depend on how the input is chunked or the output pulled
    {
        const auto x = sine(5000, 22050, 700, 0.6);
        const auto whole = resample_all(x, 1, 22050, 48000);
        for (size_t push : {size_t(1), size_t(13), size_t(999), size_t(4096)})
            for (size_t pull : {size_t(1), size_t(100), size_t(4096)})
                CHECK(resample_all(x, 1, 22050, 48000, push, pull) == whole);
    }
    // stereo channels are independent
    {
        const auto l = sine(2000, 22050, 440, 0.5), r = sine(2000, 22050, 1000, 0.25);
        std::vector<float> st(4000);
        for (size_t i = 0; i < 2000; ++i) { st[2 * i] = l[i]; st[2 * i + 1] = r[i]; }
        const auto y = resample_all(st, 2, 22050, 44100);
        const auto yl = resample_all(l, 1, 22050, 44100), yr = resample_all(r, 1, 22050, 44100);
        CHECK(y.size() == yl.size() * 2);
        bool same = true;
        for (size_t i = 0; i < yl.size(); ++i) same = same && y[2 * i] == yl[i] && y[2 * i + 1] == yr[i];
        CHECK(same);
    }
    // an empty input produces nothing; finishing twice and pushing after finish are harmless
    {
        StreamResampler r(1, 22050, 48000);
        r.finish(); r.finish();
        float o[4];
        CHECK(r.pull(o, 4) == 0 && r.done());
        const float x = 1.0f;
        r.push(&x, 1);
        CHECK(r.pull(o, 4) == 0);
    }
}

void test_conversions() {
    std::vector<float> out;
    const float mono[3] = {0.5f, -1.0f, 0.25f};
    convert_channels(mono, 3, 1, 2, out);
    CHECK(out.size() == 6 && out[0] == 0.5f && out[1] == 0.5f && out[2] == -1.0f && out[5] == 0.25f);
    const float st[4] = {1.0f, 0.0f, 0.5f, -0.5f};
    convert_channels(st, 2, 2, 1, out);
    CHECK(out.size() == 2 && out[0] == 0.5f && out[1] == 0.0f);
    convert_channels(st, 2, 2, 2, out);
    CHECK(out.size() == 4 && out[3] == -0.5f);
    convert_channels(st, 2, 3, 2, out);
    CHECK(out.empty());
    CHECK(float_to_s16(0.0f) == 0 && float_to_s16(1.0f) == 32767 && float_to_s16(-1.0f) == -32768 && float_to_s16(2.0f) == 32767 &&
          float_to_s16(-2.0f) == -32768 && float_to_s16(0.5f) == 16384 && float_to_s16(NAN) == 0 && float_to_s16(1.0f / 65536.0f) == 1 &&
          float_to_s16(-1.0f / 65536.0f) == 0 && float_to_s16(INFINITY) == 32767);
}

// ---- determinism ---------------------------------------------------------------------------------------------------

// Recorded with X2_AUDIO_PRINT_GOLDENS=1 (the values are printed and the checks are skipped in that mode).
constexpr uint64_t kGoldenFlac = 0xfb79cc5e86b2788eull;
constexpr uint64_t kGoldenResampled = 0x11613341b93aae95ull;
bool printing_goldens() { return std::getenv("X2_AUDIO_PRINT_GOLDENS") != nullptr; }
void print_golden(const char* what, uint64_t h) {
    if (printing_goldens()) std::printf("  golden %s = 0x%016llxull\n", what, (unsigned long long)h);
}

void test_determinism() {
    std::string err;
    // Integer paths are exact everywhere: golden hashes of a decoded FLAC and of a resampled WAV (the kernel is built with double
    // math and rounded to float, so only the platform's libm could move it; if a platform differs, these checks show it).
    {
        DecodedClip c;
        CHECK(decode_all(src(load("tone_mono_22050.flac")), DecodeLimits{}, {}, c, &err));
        const uint64_t h = fnv(c.pcm.data(), c.pcm.size() * 2);
        print_golden("flac", h);
        CHECK(printing_goldens() || h == kGoldenFlac);
    }
    {
        Wav w; w.data = pcm16(sine(3000, 22050, 523.25, 0.4));
        DecodedClip c;
        CHECK(decode_all(src(build(w)), DecodeLimits{}, {2, 48000}, c, &err));
        CHECK(c.frames == 6531 && c.channels == 2);
        const uint64_t h = fnv(c.pcm.data(), c.pcm.size() * 2);
        print_golden("resampled", h);
        CHECK(printing_goldens() || h == kGoldenResampled);
    }
    // Vorbis uses float transforms: it must repeat exactly within one process (and the golden float values are checked by tolerance above).
    {
        DecodedClip a, b;
        CHECK(decode_all(src(load("tone_stereo_44100.ogg")), DecodeLimits{}, {2, 48000}, a, &err));
        CHECK(decode_all(src(load("tone_stereo_44100.ogg")), DecodeLimits{}, {2, 48000}, b, &err));
        CHECK(a.pcm == b.pcm && !a.pcm.empty());
    }
}

// ---- mutation fuzz -------------------------------------------------------------------------------------------------

struct Rng {
    uint64_t s;
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    size_t below(size_t n) { return n ? size_t(next() % n) : 0; }
};

Bytes mutate(const Bytes& in, Rng& r) {
    Bytes b = in;
    const int kind = int(r.below(8));
    switch (kind) {
        case 0: // flip a few bits
            for (int i = 0, n = 1 + int(r.below(4)); i < n && !b.empty(); ++i) b[r.below(b.size())] ^= uint8_t(1u << r.below(8));
            break;
        case 1: // random bytes
            for (int i = 0, n = 1 + int(r.below(8)); i < n && !b.empty(); ++i) b[r.below(b.size())] = uint8_t(r.next());
            break;
        case 2: // truncate
            b.resize(r.below(b.size() + 1));
            break;
        case 3: // overwrite a header-sized region with 0xFF or 0x00
            if (!b.empty()) {
                const size_t at = r.below(std::min<size_t>(b.size(), 128)), n = 1 + r.below(8);
                for (size_t i = at; i < std::min(b.size(), at + n); ++i) b[i] = (r.next() & 1) ? 0xFF : 0x00;
            }
            break;
        case 4: // insert bytes
            b.insert(b.begin() + std::ptrdiff_t(r.below(b.size() + 1)), 1 + r.below(16), uint8_t(r.next()));
            break;
        case 5: // delete a run
            if (!b.empty()) {
                const size_t at = r.below(b.size());
                b.erase(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(std::min(b.size(), at + 1 + r.below(32))));
            }
            break;
        case 6: // splice a copy of an earlier run
            if (b.size() > 8) {
                const size_t from = r.below(b.size() - 4), n = 1 + r.below(std::min<size_t>(64, b.size() - from));
                const Bytes run(b.begin() + std::ptrdiff_t(from), b.begin() + std::ptrdiff_t(from + n));
                b.insert(b.begin() + std::ptrdiff_t(r.below(b.size())), run.begin(), run.end());
            }
            break;
        default: // a 32-bit field set to an extreme value
            if (b.size() > 8) {
                static const uint32_t ex[] = {0, 1, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFF, 0x10000};
                const size_t at = r.below(std::min<size_t>(b.size() - 4, 256));
                const uint32_t v = ex[r.below(sizeof ex / sizeof ex[0])];
                for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
            }
    }
    return b;
}

void fuzz_one(const Bytes& b, Rng& r, uint64_t& ok_count, uint64_t& err_count) {
    DecodeLimits lim;
    lim.budget = size_t(8) << 20;
    lim.max_frames = 400000;
    lim.max_file_bytes = 4 << 20;
    lim.max_calls = 4096;
    std::string err;
    FloatClip c;
    OutputFormat want;
    if (r.below(3) == 0) { want.channels = 1 + int(r.below(2)); want.sample_rate = 8000 + int(r.below(40000)); }
    if (decode_all_f32(src(b), lim, want, c, &err)) {
        ++ok_count;
        CHECK(c.pcm.size() == size_t(c.frames) * size_t(c.channels));
        CHECK(c.channels >= 1 && c.channels <= 2 && c.frames > 0);
        bool finite = true;
        for (float v : c.pcm) finite = finite && std::isfinite(v);
        CHECK(finite);
        if (!finite) std::fprintf(stderr, "    non-finite output\n");
    } else {
        ++err_count;
        CHECK(!err.empty());
    }
    // streaming with random chunk sizes, seeks and reads after the end
    auto d = AudioDecoder::open(src(b), lim, &err);
    if (d) {
        std::vector<float> buf(2048 * 2);
        for (int i = 0; i < 40; ++i) {
            if (r.below(5) == 0) d->seek(r.below(5000));
            const size_t n = d->read(buf.data(), 1 + r.below(2048));
            if (n == 0 && r.below(2) == 0) break;
        }
    } else {
        CHECK(!err.empty());
    }
}

void test_fuzz() {
    uint64_t iters = 3000;
    if (const char* e = std::getenv("X2_AUDIO_FUZZ_ITERATIONS")) iters = std::strtoull(e, nullptr, 10);
    Wav w1; w1.data = pcm16(sine(600, 22050, 440, 0.5));
    Wav w2; w2.channels = 2; w2.extensible = true; w2.bits = 16; w2.data = pcm16(sine(300, 44100, 880, 0.5), 2);
    w2.before.push_back({"LIST", Bytes(9, 3)});
    w2.after.push_back({"smpl", smpl_chunk(1, 0, 10, 99)});
    Wav w3; w3.bits = 24; w3.data = Bytes(300, 0x11);
    Wav w4; w4.bits = 32; w4.tag = 3; w4.data = Bytes(400, 0x22);
    const std::vector<std::pair<const char*, Bytes>> seeds = {
        {"wav16", build(w1)}, {"wav-stereo-ext", build(w2)}, {"wav24", build(w3)}, {"wavfloat", build(w4)},
        {"flac16", load("tone_mono_22050.flac")}, {"flac24", load("tone_stereo_44100_24bit.flac")},
        {"flac-loop", load("tone_loop_22050.flac")}, {"ogg-mono", load("tone_mono_22050.ogg")},
        {"ogg-stereo", load("tone_stereo_44100.ogg")}, {"ogg-loop", load("tone_loop_22050.ogg")}};
    uint64_t total = 0;
    for (const auto& [name, seed] : seeds) {
        if (seed.empty()) { CHECK(!seed.empty()); std::fprintf(stderr, "    missing seed %s\n", name); continue; }
        Rng r{0x9E3779B97F4A7C15ull ^ fnv(name, std::strlen(name))};
        uint64_t ok = 0, bad = 0;
        const uint64_t n = (std::strncmp(name, "wav", 3) == 0 ? iters * 2 : iters);
        for (uint64_t i = 0; i < n; ++i) {
            Bytes m = mutate(seed, r);
            if (r.below(4) == 0) m = mutate(m, r); // two mutations
            fuzz_one(m, r, ok, bad);
        }
        total += n;
        std::printf("  fuzz %-15s %6llu mutations: %6llu decoded, %6llu refused\n", name, (unsigned long long)n, (unsigned long long)ok, (unsigned long long)bad);
    }
    std::printf("  fuzz total: %llu mutations\n", (unsigned long long)total);
}

} // namespace

int main(int argc, char** argv) {
    g_dir = argc > 1 ? argv[1] : "tests/audio/fixtures";
    const struct { const char* name; void (*fn)(); } tests[] = {
        {"memory source", test_memory_source}, {"wav formats", test_wav_formats}, {"wav errors", test_wav_errors},
        {"wav loop and seek", test_wav_loop_and_seek}, {"fixtures (FLAC, Ogg Vorbis)", test_fixtures}, {"resampler", test_resampler},
        {"conversions", test_conversions}, {"determinism", test_determinism}, {"mutation fuzz", test_fuzz}};
    for (const auto& t : tests) {
        const int before = g_failures;
        std::printf("%s\n", t.name);
        t.fn();
        std::printf("  %s\n", g_failures == before ? "ok" : "FAILED");
    }
    std::printf("audio decode tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
