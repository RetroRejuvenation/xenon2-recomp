// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Asset packs v2, workstream 7 (docs/re-notes/asset-packs-v2-design.md section 9): what the unit tests of the loader, the decoders and the
// audio stage do not cover on their own. Synthetic packs only, no game data.
//   - the version 1 documentation example loads, as a directory and as a ZIP, to the same library
//   - allocation bombs: atlases whose frames overlap, a picture that declares a huge size, entry and memory caps hold
//   - UTF-8 pack and file names, in a directory pack and in a ZIP
//   - determinism: loading twice, and as a ZIP, gives the same library; the same synthetic Paula run with a pack of 200 effects gives the same
//     audio hash every time, from a directory and from a ZIP, and equals the recorded hash
//   - performance gates: the cost of a pack of 200 effects and two streaming tracks on the audio stage, and of a library of 10,000 graphics
//     and 100 screens on the per-frame lookups, against the frame period. The gates have generous floors (CI machines are noisy); the
//     measured figures are printed. Sanitizer builds report them without enforcing.
//
//   x2_pack_integration_tests SOURCE_DIR SCRATCH_DIR
#include "audio/decode.h"
#include "audio/pack_audio.h"
#include "audio/paula.h"
#include "import/sha1.h"
#include "modern/assets.h"
#include "modern/layout.h"
#include "pack/pack_api.h"
#include "pack/pack_check.h"
#include "pack/pack_png.h"
#include "pack/pack_source.h"
#include "pack/pack_stack.h"
#include "support/synth.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace x2;
using namespace x2::pack;
using namespace x2::synth;

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

std::string g_src, g_tmp;

#if defined(X2_SANITIZED_BUILD)
constexpr bool kEnforceGates = false;
#else
constexpr bool kEnforceGates = true;
#endif
// Every gate is the design's budget times this factor: shared CI runners are slower and noisier than a developer machine.
constexpr double kSlack = 4.0;
constexpr double kFramePeriodMs = 20.0;  // 50 Hz

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Records a gate: prints the figure, and fails when it is over the limit (unless the build is sanitized).
void gate(const char* what, double measured, double limit, const char* unit) {
    std::printf("    gate %-52s %9.4f %s (limit %.4f)%s\n", what, measured, unit, limit, kEnforceGates ? "" : " [not enforced: sanitizer build]");
    if (kEnforceGates) CHECK(measured <= limit);
}

bool has_error(const PackReport& r, const std::string& what) {
    for (const PackMessage& m : r.messages)
        if (int(m.severity) == 2 && m.format().find(what) != std::string::npos) return true;
    return false;
}
size_t errors_in(const PackReport& r) {
    size_t n = 0;
    for (const PackMessage& m : r.messages) n += int(m.severity) == 2 ? 1 : 0;
    return n;
}

std::string hex16(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

PackContext disk() {
    PackContext c;
    c.game = GameFamily::Disk;
    c.build = "canonical";
    return c;
}

// A stable digest of everything a stack makes available: pictures (by key, in key order) and clips (by key).
std::string library_digest(const PackStack& s) {
    std::string acc;
    std::map<std::string, std::string> items;
    s.for_each_graphic([&](uint64_t hash, int w, int h, const GraphicEntry& e) {
        std::string d;
        if (e.pic) d.append(reinterpret_cast<const char*>(e.pic->px.data()), e.pic->px.size() * 4);
        items["g:" + hex16(hash) + "-" + std::to_string(w) + "x" + std::to_string(h)] =
            std::to_string(e.pic ? e.pic->w : 0) + "x" + std::to_string(e.pic ? e.pic->h : 0) + "@" + std::to_string(e.scale) + ":" + std::to_string(e.anchor.x) + "," + std::to_string(e.anchor.y) + ":" +
            import::sha1_hex(reinterpret_cast<const uint8_t*>(d.data()), d.size());
    });
    s.for_each_far([&](uint64_t line, const FarLineRef& r) {
        items["f:" + hex16(line)] = std::to_string(r.row) + "@" + std::to_string(r.scale);
    });
    for (const auto& kv : items) acc += kv.first + "=" + kv.second + "\n";
    acc += "counts " + std::to_string(s.graphic_count()) + " " + std::to_string(s.far_count()) + " " + std::to_string(s.sfx_count()) + " " + std::to_string(s.picture_bytes()) + "\n";
    return import::sha1_hex(reinterpret_cast<const uint8_t*>(acc.data()), acc.size());
}

void zip_directory(const std::string& dir, const std::string& zip_path, int method = 0) {
    std::vector<ZipFile> files;
    std::vector<std::string> names;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) names.push_back(fs::relative(e.path(), dir).generic_string());
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) files.push_back({n, slurp(dir + "/" + n), method});
    put_file(zip_path, make_zip(files));
}

// ---- the version 1 documentation example -------------------------------------------------------------------------------------

void test_v1_example() {
    std::printf("the version 1 documentation example\n");
    const std::string dir = g_tmp + "/v1doc";
    fs::remove_all(dir);
    put_file(dir + "/manifest.json", slurp(g_src + "/docs/schema/examples/v1-graphics.example.json"));
    put_flat(dir + "/sprites/ship.png", 16 * 4, 12 * 4);
    put_flat(dir + "/tiles/wall_03.png", 16 * 2, 15 * 2);
    put_flat(dir + "/far/level1.png", 320 * 4, 3 * 4);
    PackStack a(disk());
    PackReport ra;
    CHECK(a.add(dir, ra) && errors_in(ra) == 0);
    CHECK(a.graphic_count() == 2 && a.far_count() == 2 && a.palette_count() == 1);
    // as a ZIP, deflated and stored, and again: the same library
    zip_directory(dir, g_tmp + "/v1doc_stored.zip", 0);
    zip_directory(dir, g_tmp + "/v1doc_deflate.zip", 8);
    for (const char* z : {"/v1doc_stored.zip", "/v1doc_deflate.zip"}) {
        PackStack b(disk());
        PackReport rb;
        CHECK(b.add(g_tmp + z, rb) && errors_in(rb) == 0);
        CHECK(library_digest(a) == library_digest(b));
    }
    // the version 1 view reads the same library
    modern::AssetLibrary lib;
    std::vector<std::string> msgs;
    CHECK(lib.load(dir, msgs) && lib.graphics() == 2 && lib.far_lines() == 2 && lib.palettes() == 1);
    const modern::Replacement* r = lib.graphic(0x0048848ccf64d966ull, 16, 12);
    CHECK(r && r->img && r->img->w == 64 && r->scale == 4);
    const modern::Replacement* t = lib.graphic(0x01a6be94d61de0e7ull, 16, 15);
    CHECK(t && t->scale == 2 && t->ax == 2 && t->ay == 0);
}

// ---- allocation bombs ---------------------------------------------------------------------------------------------------------

void test_atlas_bombs() {
    std::printf("allocation bombs: overlapping atlas frames, a huge declared picture, entry and memory caps\n");
    // 4000 frames, all the same 64x64 rectangle of one 256x256 sheet, under 4000 distinct keys.
    const std::string dir = g_tmp + "/bomb_atlas";
    fs::remove_all(dir);
    std::string man = "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"atlases\": [{\"png\": \"sheet.png\", \"frames\": [";
    for (int i = 0; i < 4000; ++i) {
        char key[64];
        std::snprintf(key, sizeof key, "%016x-16x16", unsigned(0x1000 + i));
        man += std::string(i ? "," : "") + "{\"key\": \"" + key + "\", \"rect\": [0, 0, 64, 64]}";
    }
    man += "]}]}";
    put_file(dir + "/manifest.json", man);
    put_flat(dir + "/sheet.png", 256, 256);
    {
        // memory cap: 1 MB for all pictures (the sheet is 256 KB, each frame 16 KB)
        StackOptions so;
        so.limits.max_total = 1u << 20;
        PackStack s(disk(), so);
        PackReport rep;
        CHECK(s.add(dir, rep));
        CHECK(s.picture_bytes() <= (1u << 20));
        CHECK(s.graphic_count() >= 40 && s.graphic_count() < 100);
        CHECK(has_error(rep, "over the memory limit"));
    }
    {
        // entry cap: 100 entries of one pack
        StackOptions so;
        so.limits.max_entries = 100;
        PackStack s(disk(), so);
        PackReport rep;
        CHECK(s.add(dir, rep));
        CHECK(s.graphic_count() <= 100);
        CHECK(has_error(rep, "more than 100 entries"));
        CHECK(s.picture_bytes() < 100u * 64 * 64 * 4 + (1u << 20));
    }
    {
        // the defaults hold too: all 4000 frames fit (64 MB at most), and nothing depends on them being the same picture
        PackStack s(disk());
        PackReport rep;
        CHECK(s.add(dir, rep) && s.graphic_count() == 4000 && errors_in(rep) == 0);
    }
    // A PNG whose header declares a gigantic picture but carries almost no data: refused by the size checks before any allocation.
    {
        const std::string d2 = g_tmp + "/bomb_png";
        fs::remove_all(d2);
        std::string png = slurp(g_tmp + "/bomb_atlas/sheet.png");
        // patch IHDR width and height to 4096x4096 (inside the per-side limit, over the pixel budget of a small limit)
        const size_t ihdr = png.find("IHDR");
        png.replace(ihdr + 4, 4, std::string("\x00\x00\x10\x00", 4));
        png.replace(ihdr + 8, 4, std::string("\x00\x00\x10\x00", 4));
        put_file(d2 + "/big.png", png);
        put_file(d2 + "/manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"graphics\": {\"0123456789abcdef-16x16\": {\"png\": \"big.png\"}}}");
        StackOptions so;
        so.limits.max_pixels = 1u << 20;
        PackStack s(disk(), so);
        PackReport rep;
        s.add(d2, rep);
        CHECK(s.graphic_count() == 0 && s.picture_bytes() == 0 && errors_in(rep) >= 1);
    }
    // A manifest with a huge scale and a huge ring is refused or clamped, never allocated.
    {
        const std::string d3 = g_tmp + "/bomb_scale";
        fs::remove_all(d3);
        put_file(d3 + "/manifest.json",
                 "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"scale\": 1000000, \"graphics\": {\"0123456789abcdef-16x16\": {\"png\": \"a.png\", \"scale\": 4294967296}},"
                 "\"backgrounds\": [{\"level\": 1, \"png\": \"a.png\", \"ring_lines\": 2147483647}]}");
        put_flat(d3 + "/a.png", 16, 16);
        PackStack s(disk());
        PackReport rep;
        s.add(d3, rep);
        CHECK(s.picture_bytes() <= 1024);
    }
}

// ---- UTF-8 names --------------------------------------------------------------------------------------------------------------

void test_utf8_names() {
    std::printf("UTF-8 pack and file names\n");
    const std::string dir = g_tmp + "/pack-\xC3\xBCn\xC3\xAF-\xE6\xA8\xA1\xE5\xBC\x8F";  // pack-ünï-模式
    fs::remove_all(fs::path(dir));
    const std::string key = "0123456789abcdef-16x16";
    const std::string rel = "sprites/\xC3\x9F-\xC3\xBC.png";                                  // ß-ü
    put_file(dir + "/manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"graphics\": {\"" + key + "\": {\"png\": \"" + rel + "\"}}}");
    put_flat(dir + "/" + rel, 16, 16);
    PackStack a(disk());
    PackReport ra;
    CHECK(a.add(dir, ra) && errors_in(ra) == 0 && a.graphic_count() == 1);
    // the same names written as JSON escapes in the manifest
    put_file(dir + "/manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"graphics\": {\"" + key + "\": {\"png\": \"sprites/\\u00df-\\u00fc.png\"}}}");
    PackStack b(disk());
    PackReport rb;
    CHECK(b.add(dir, rb) && errors_in(rb) == 0 && b.graphic_count() == 1);
    // in a ZIP, and the ZIP itself under a UTF-8 name
    const std::string zip = g_tmp + "/z-\xC3\xA9t\xC3\xA9.zip";
    put_file(zip, make_zip({{"manifest.json", slurp(dir + "/manifest.json"), 0}, {rel, slurp(dir + "/" + rel), 8}}));
    PackStack c(disk());
    PackReport rc;
    CHECK(c.add(zip, rc) && errors_in(rc) == 0 && c.graphic_count() == 1);
    CHECK(library_digest(a) == library_digest(c));
    std::string why;
    auto src = open_pack_source(dir, PackLimits(), why);
    CHECK(src && src->exists(rel));
}

// ---- audio: a real Paula, the real stage, a pack of 200 effects ----------------------------------------------------------------

constexpr int kSfx = 200;
constexpr uint32_t kSampleBase = 0x1000, kSampleStride = 0x400, kSampleWords = 256;  // 512-byte effects
constexpr int kOutRate = 48000;

struct Rig {
    std::vector<uint8_t> ram;
    Paula p;
    audio::PackAudio pa;
    std::shared_ptr<PackStack> lib;
    std::vector<int16_t> out;
    uint16_t dma = 0;

    explicit Rig(std::shared_ptr<PackStack> l) : ram(0x80000, 0), lib(std::move(l)) {
        p.attach(ram.data(), uint32_t(ram.size() - 1), [](uint16_t) {});
        PaulaConfig c;
        c.filter = AudioFilterModel::None;
        c.stereo_separation = 1.0f;
        c.output_gain = 1.0f;
        c.output_rate = kOutRate;
        p.configure(c);
        p.set_output_enabled(true);
        p.set_writer_query([]() { return AudioSource::Effect; });
        for (int i = 0; i < kSfx; ++i)
            for (uint32_t j = 0; j < kSampleWords * 2; ++j) ram[kSampleBase + uint32_t(i) * kSampleStride + j] = uint8_t(i * 31 + int(j) * 7);
        AssetPackSettings s;
        s.upgrade = true;
        s.audio_upgrade = true;
        set_pack_settings(s);
        set_packs(lib);  // nullptr: no pack
        pa.attach(p, ram.data(), uint32_t(ram.size() - 1), GameFamily::Disk, modern::audio_layout_for(GameFamily::Disk));
    }
    ~Rig() {
        set_packs(nullptr);
        set_pack_settings(AssetPackSettings{});
    }
    std::string key_of(int i) const { return sample_key(&ram[kSampleBase + uint32_t(i) * kSampleStride], kSampleWords * 2); }
    void play(int ch, uint32_t lc, uint16_t per, uint16_t vol) {
        const uint16_t base = uint16_t(0xA0 + 16 * ch);
        p.write_reg(uint16_t(base + 0), uint16_t(lc >> 16));
        p.write_reg(uint16_t(base + 2), uint16_t(lc));
        p.write_reg(uint16_t(base + 4), uint16_t(kSampleWords));
        p.write_reg(uint16_t(base + 6), per);
        p.write_reg(uint16_t(base + 8), vol);
        dma = uint16_t(dma | (1 << ch));
        p.set_dmacon(uint16_t(0x0200 | dma), uint16_t(0x8000 | (1 << ch)));
    }
    void stop(int ch) {
        dma = uint16_t(dma & ~(1 << ch));
        p.set_dmacon(uint16_t(0x0200 | dma), uint16_t(1 << ch));
    }
    void run_frame() {
        p.run_until(p.now() + 3697);  // about 50 output frames
        pa.after_frame(p);
        out.insert(out.end(), p.output().begin(), p.output().end());
        p.output().clear();
    }
};

// A pack with one WAV per effect; the keys are those of the rig's samples.
std::string make_sfx_pack(const std::string& name) {
    Rig keys(nullptr);
    const std::string dir = g_tmp + "/" + name;
    fs::remove_all(dir);
    std::string man = "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"audio\": {\"sfx\": {";
    for (int i = 0; i < kSfx; ++i) {
        char file[32];
        std::snprintf(file, sizeof file, "sfx/%03d.wav", i);
        man += std::string(i ? ", " : "") + "\"" + keys.key_of(i) + "\": {\"file\": \"" + file + "\", \"ref_period\": 428}";
        std::vector<int16_t> pcm = tone(1500 + size_t(i % 7) * 100, 1, 4000 + (i % 13) * 1500);
        put_file(dir + "/" + file, make_wav(22050, 1, pcm));
    }
    man += "}}}";
    put_file(dir + "/manifest.json", man);
    return dir;
}

std::shared_ptr<PackStack> load_stack(const std::string& path, PackReport* rep = nullptr) {
    auto s = std::make_shared<PackStack>(disk());
    PackReport r;
    s->add(path, r);
    if (rep) *rep = r;
    return s;
}

// The synthetic scenario: 200 effects started on the four channels, one every 6 frames, with period and volume changes.
std::vector<int16_t> run_scenario(Rig& r, double* wall_ms = nullptr) {
    const double t0 = now_ms();
    for (int i = 0; i < kSfx; ++i) {
        const int ch = i % 4;
        r.stop(ch);  // a DMA start needs the channel off first
        r.run_frame();
        r.play(ch, kSampleBase + uint32_t(i) * kSampleStride, uint16_t(380 + (i % 5) * 20), uint16_t(40 + i % 24));
        for (int f = 0; f < 5; ++f) {
            r.run_frame();
            if (f == 2) r.p.write_reg(uint16_t(0xA0 + 16 * ch + 6), uint16_t(400 + (i % 7) * 15));
        }
    }
    for (int f = 0; f < 60; ++f) r.run_frame();
    if (wall_ms) *wall_ms = now_ms() - t0;
    return r.out;
}

std::string hash_of(const std::vector<int16_t>& v) { return import::sha1_hex(reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(int16_t)); }

// Recorded with this scenario; the audio stage computes in integers and the pack's resampler is run with contraction off, so this is the
// same on every platform. If the stage's maths changes on purpose, record the new hash here.
const char* kGoldenAudio = "04d504f61d6e48f27ad743cd5ea6f0eb2ea96d67";

void test_audio_determinism_and_perf() {
    std::printf("audio: a pack of %d effects on the real stage; determinism and the cost gates\n", kSfx);
    set_audio_codec(&audio::pack_codec());
    const std::string dir = make_sfx_pack("sfx200");
    zip_directory(dir, g_tmp + "/sfx200_stored.zip", 0);
    zip_directory(dir, g_tmp + "/sfx200_deflate.zip", 8);

    PackReport rep;
    auto stack = load_stack(dir, &rep);
    CHECK(stack->sfx_count() == size_t(kSfx) && errors_in(rep) == 0);

    double wall_pack = 0;
    std::string h1;
    {
        Rig r(stack);
        const std::vector<int16_t> out = run_scenario(r, &wall_pack);
        h1 = hash_of(out);
        std::printf("    stage: %llu starts, %llu candidates, %llu replaced, %llu stolen, %llu hashed\n", (unsigned long long)r.pa.stats().starts, (unsigned long long)r.pa.stats().candidates,
                    (unsigned long long)r.pa.stats().replaced, (unsigned long long)r.pa.stats().stolen, (unsigned long long)r.pa.stats().hashed);
        CHECK(r.pa.stats().replaced == uint64_t(kSfx));
        CHECK(!out.empty());
    }
    std::printf("    scenario hash %s\n", h1.c_str());
    {
        Rig r(load_stack(dir));
        CHECK(hash_of(run_scenario(r)) == h1);                                  // a second load and run
    }
    for (const char* z : {"/sfx200_stored.zip", "/sfx200_deflate.zip"}) {
        Rig r(load_stack(g_tmp + z));
        CHECK(hash_of(run_scenario(r)) == h1);                                  // the ZIP pack, stored or deflated
    }
    if (kGoldenAudio[0]) CHECK(h1 == kGoldenAudio);
    // without the pack: the stage attached, nothing replaced, and the mix is the original's
    double wall_plain = 0;
    std::string hp;
    {
        Rig r(nullptr);
        hp = hash_of(run_scenario(r, &wall_plain));
        CHECK(r.pa.stats().replaced == 0);
    }
    CHECK(hp != h1);

    // The cost: frames emulated in the scenario, and the extra time per frame with the pack (the voices, the resampled clips are made
    // once and cached by the stage) against the 20 ms frame period.
    const double frames = double(kSfx) * 6 + 60;
    const double extra_ms = std::max(0.0, wall_pack - wall_plain) / frames;
    gate("pack audio stage, 200 effects: extra ms per frame", extra_ms, 0.03 * kFramePeriodMs * kSlack, "ms");

    // Two streaming tracks: a stereo 44.1 kHz WAV each, decoded and resampled to the output rate, one 20 ms slice per frame.
    const std::string track = make_wav(44100, 2, tone(44100 * 4, 2, 6000));
    std::vector<std::unique_ptr<audio::AudioDecoder>> dec;
    std::vector<audio::StreamResampler> rs;
    audio::DecodeLimits lim;
    std::string err;
    for (int i = 0; i < 2; ++i) {
        dec.push_back(audio::AudioDecoder::open(audio::memory_source(reinterpret_cast<const uint8_t*>(track.data()), track.size()), lim, &err));
        CHECK(dec.back() != nullptr);
        rs.emplace_back(2, 44100, kOutRate);
    }
    if (dec[0] && dec[1]) {
        std::vector<float> in(2 * 882), outb(2 * 960);
        const int slices = 150;
        double sink = 0, per_frame = 1e9;
        // The best of three passes: one preempted pass on a shared runner (3.6 ms against a 2.4 ms limit once) says nothing about the
        // code, while a real slowdown makes every pass slow. The limit is 10x the design budget for the same reason.
        for (int pass = 0; pass < 3; ++pass) {
            const double t0 = now_ms();
            for (int f = 0; f < slices; ++f)
                for (int i = 0; i < 2; ++i) {
                    size_t n = dec[size_t(i)]->read(in.data(), 882);
                    if (n < 882) { dec[size_t(i)]->seek(0); n += dec[size_t(i)]->read(in.data() + n * 2, 882 - n); }
                    rs[size_t(i)].push(in.data(), n);
                    const size_t m = rs[size_t(i)].pull(outb.data(), 960);
                    for (size_t k = 0; k < m * 2; k += 97) sink += outb[k];
                }
            per_frame = std::min(per_frame, (now_ms() - t0) / slices);
        }
        if (sink == 12345.678) std::printf(" ");  // keep the loop observable
        gate("two streaming tracks, decode and resample: ms per frame", per_frame, 0.03 * kFramePeriodMs * 10.0, "ms");
    }
}

// ---- performance: the lookups of a large library ---------------------------------------------------------------------------------

void test_lookup_perf() {
    std::printf("performance: a library of 10000 graphics and 100 screens, the per-frame lookups\n");
    const std::string dir = g_tmp + "/big_lib";
    fs::remove_all(dir);
    constexpr int kGraphics = 10000, kScreens = 100;
    std::string man = "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"scale\": 2, \"graphics\": {";
    for (int i = 0; i < kGraphics; ++i) {
        char key[48];
        std::snprintf(key, sizeof key, "%016llx-16x16", (unsigned long long)(0x9E3779B97F4A7C15ull * uint64_t(i + 1)));
        man += std::string(i ? "," : "") + "\"" + key + "\": {\"png\": \"a.png\"}";
    }
    man += "}, \"screens\": {";
    for (int i = 0; i < kScreens; ++i) {
        char s[320];
        std::snprintf(s, sizeof s, "\"s%03d\": {\"png\": \"screen.png\", \"ref_palette\": [\"#000\", \"#fff\"], \"signature\": {\"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"%016llx\"}]}}", i,
                      (unsigned long long)(0xC2B2AE3D27D4EB4Full * uint64_t(i + 1)));
        man += std::string(i ? "," : "") + s;
    }
    man += "}}";
    put_file(dir + "/manifest.json", man);
    put_flat(dir + "/a.png", 32, 32);
    put_flat(dir + "/screen.png", 640, 400);
    const double t0 = now_ms();
    PackStack s(disk());
    PackReport rep;
    CHECK(s.add(dir, rep) && s.graphic_count() == size_t(kGraphics) && s.screen_count() == size_t(kScreens));
    const double load_ms = now_ms() - t0;
    std::printf("    loaded in %.0f ms (%zu graphics, %zu screens, %.1f MB of pictures)\n", load_ms, s.graphic_count(), s.screen_count(), double(s.picture_bytes()) / (1 << 20));
    gate("loading 10000 graphics and 100 screens", load_ms / 1000.0, 20.0, "s");
    // one frame: 3000 sprite lookups (a third of them hits), 40 block lookups for the screen matcher
    constexpr int kFrames = 400;
    size_t hits = 0;
    const double t1 = now_ms();
    for (int f = 0; f < kFrames; ++f) {
        for (int k = 0; k < 3000; ++k) {
            const int i = (f * 7 + k * 3) % (kGraphics * 3);
            hits += s.graphic(0x9E3779B97F4A7C15ull * uint64_t(i + 1), 16, 16) != nullptr;
        }
        for (int k = 0; k < 40; ++k) hits += s.screens_for_block(0xC2B2AE3D27D4EB4Full * uint64_t((f + k) % (kScreens * 2) + 1)).size();
    }
    const double per_frame = (now_ms() - t1) / kFrames;
    CHECK(hits > 0);
    gate("library lookups, 3000 graphics + 40 blocks: ms per frame", per_frame, 0.05 * kFramePeriodMs * kSlack, "ms");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: x2_pack_integration_tests SOURCE_DIR SCRATCH_DIR\n");
        return 2;
    }
    g_src = argv[1];
    g_tmp = argv[2];
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    fs::create_directories(g_tmp, ec);
    test_v1_example();
    test_atlas_bombs();
    test_utf8_names();
    test_audio_determinism_and_perf();
    test_lookup_perf();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
