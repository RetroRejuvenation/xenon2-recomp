// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Writes the seed corpora of the fuzz targets, from synthetic data and the tone fixtures of tests/audio/fixtures; nothing is game content.
//   x2_fuzz_seeds SOURCE_DIR OUT_DIR   (OUT_DIR/manifest, zip, png, wav, flac, vorbis)
#include "support/synth.h"

#include <cstdio>
#include <string>

using namespace x2::synth;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: x2_fuzz_seeds SOURCE_DIR OUT_DIR\n");
        return 2;
    }
    const std::string src = argv[1], out = argv[2];
    std::error_code ec;
    fs::remove_all(out, ec);
    int n = 0;
    auto put = [&](const std::string& kind, const std::string& name, const std::string& bytes) {
        put_file(out + "/" + kind + "/" + name, bytes);
        ++n;
    };

    // manifests: the byte before the text selects validator mode
    const std::string v2 = slurp(src + "/docs/schema/examples/v2-full.example.json");
    const std::string v1 = slurp(src + "/docs/schema/examples/v1-graphics.example.json");
    if (v2.empty() || v1.empty()) {
        std::fprintf(stderr, "x2_fuzz_seeds: the example manifests are missing under %s\n", src.c_str());
        return 1;
    }
    for (int mode = 0; mode < 2; ++mode) {
        const std::string m(1, char(mode));
        put("manifest", "v2_full_" + std::to_string(mode) + ".bin", m + v2);
        put("manifest", "v1_graphics_" + std::to_string(mode) + ".bin", m + v1);
    }
    put("manifest", "minimal.bin", std::string(1, '\1') + "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\"}");
    put("manifest", "atlas.bin", std::string(1, '\1') +
        "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"scale\": 2, \"atlases\": [{\"png\": \"a.png\", \"grid\": {\"cell\": [16, 16], \"keys\": [\"0123456789abcdef-16x16\"]}}],"
        " \"audio\": {\"sfx\": {\"0123456789abcdef-100\": {\"file\": \"s.wav\", \"ref_period\": 428}}}}");

    // regressions found by the fuzzer: each stays in the corpus so that the plain ctest replay guards it
    put("manifest", "regress_version_overflow.bin", std::string(1, '\0') + "{\"format\": \"x2-asset-pack\", \"version\": 4444444444444444444444444444444444444444444444444444444444444}");

    // ZIPs
    const std::string scratch = (fs::temp_directory_path() / "x2_fuzz_seed.png").string();
    const std::string png16 = png_bytes(scratch, 16, 16, [](int x, int y) { return 0xFF000000u | uint32_t(x * 16) | uint32_t(y * 16) << 8; });
    const std::string manifest = "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"graphics\": {\"0123456789abcdef-8x8\": {\"png\": \"sprites/a.png\", \"scale\": 2}}}";
    put("zip", "stored.zip", make_zip({{"manifest.json", manifest, 0}, {"sprites/a.png", png16, 0}}));
    put("zip", "deflated.zip", make_zip({{"manifest.json", manifest, 8}, {"sprites/a.png", png16, 8}, {"audio/s.wav", make_wav(22050, 1, tone(512)), 8}}));
    put("zip", "backslash.zip", make_zip({{"manifest.json", manifest, 0}, {"sprites\\a.png", png16, 0}}));
    put("zip", "bomb_declared.zip", make_zip({{"manifest.json", manifest, 0}, {"bomb.bin", std::string(50, 'z'), 8, 3u << 20}}));
    put("zip", "bad_crc.zip", [&] { ZipFile f{"x.bin", "payload", 0}; f.bad_crc = true; return make_zip({{"manifest.json", "{}", 0}, f}); }());
    put("zip", "unsafe_names.zip", make_zip({{"../evil.png", "x", 0}, {"/abs.png", "y", 0}, {"a/../../b", "z", 0}, {"manifest.json", "{}", 0}}));

    // PNGs
    put("png", "gradient16.png", png16);
    put("png", "flat1.png", png_bytes(scratch, 1, 1, [](int, int) { return 0xFF0000FFu; }));
    put("png", "alpha8x4.png", png_bytes(scratch, 8, 4, [](int x, int) { return (x & 1) ? 0x80FFFFFFu : 0u; }));
    put("png", "wide64x2.png", png_bytes(scratch, 64, 2, [](int x, int y) { return 0xFF000000u | uint32_t(x * 4 + y); }));
    fs::remove(scratch, ec);

    // WAV: PCM16 mono and stereo, a tiny one, and one with a loop (smpl) chunk
    put("wav", "mono22050.wav", make_wav(22050, 1, tone(600)));
    put("wav", "stereo44100.wav", make_wav(44100, 2, tone(300, 2)));
    put("wav", "tiny8000.wav", make_wav(8000, 1, tone(8)));
    {
        std::string w = make_wav(22050, 1, tone(400));
        std::string smpl = "smpl";
        auto u32 = [&](std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += char(v >> (8 * i) & 255); };
        u32(smpl, 60);
        u32(smpl, 0); u32(smpl, 0); u32(smpl, 0); u32(smpl, 60); u32(smpl, 0); u32(smpl, 0); u32(smpl, 0); u32(smpl, 1); u32(smpl, 0);
        u32(smpl, 0); u32(smpl, 0); u32(smpl, 10); u32(smpl, 300); u32(smpl, 0); u32(smpl, 0);
        w += smpl;
        const uint32_t riff = uint32_t(w.size() - 8);
        for (int i = 0; i < 4; ++i) w[size_t(4 + i)] = char(riff >> (8 * i) & 255);
        put("wav", "loop.wav", w);
    }

    // FLAC and Vorbis: the synthesised tones
    for (const char* f : {"tone_mono_22050.flac", "tone_stereo_44100_24bit.flac", "tone_loop_22050.flac"})
        put("flac", f, slurp(src + "/tests/audio/fixtures/" + f));
    for (const char* f : {"tone_mono_22050.ogg", "tone_stereo_44100.ogg", "tone_loop_22050.ogg"})
        put("vorbis", f, slurp(src + "/tests/audio/fixtures/" + f));
    std::printf("fuzz seeds: %d files in %s\n", n, out.c_str());
    return 0;
}
