// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Fuzz target: the sound decoders (runtime/audio/decode.*, vendor/audio: dr_flac and stb_vorbis). X2_FUZZ_AUDIO_KIND selects the
// container whose magic is forced onto the input so that the mutations land in that decoder: 1 WAV, 2 FLAC, 3 Ogg Vorbis.
#include "audio/decode.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef X2_FUZZ_AUDIO_KIND
#define X2_FUZZ_AUDIO_KIND 1
#endif

using namespace x2::audio;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 12 || size > (2u << 20)) return 0;
    std::vector<uint8_t> in(data, data + size);
    static const char* const kMagic[] = {"", "RIFF", "fLaC", "OggS"};
    std::memcpy(in.data(), kMagic[X2_FUZZ_AUDIO_KIND], 4);
    if (X2_FUZZ_AUDIO_KIND == 1) std::memcpy(in.data() + 8, "WAVE", 4);

    DecodeLimits lim;
    lim.budget = size_t(8) << 20;
    lim.max_frames = uint64_t(1) << 22;
    lim.max_calls = uint64_t(1) << 16;
    std::string err;
    // streaming read with seeks
    if (auto dec = AudioDecoder::open(memory_source(std::vector<uint8_t>(in)), lim, &err)) {
        const int ch = dec->info().channels;
        if (ch < 1 || ch > 2) std::abort();
        std::vector<float> buf(4096 * size_t(ch));
        uint64_t total = 0;
        for (int i = 0; i < 64; ++i) {
            const size_t n = dec->read(buf.data(), 4096);
            if (n > 4096) std::abort();
            for (size_t k = 0; k < n * size_t(ch); ++k)
                if (!(buf[k] > -1e6f && buf[k] < 1e6f)) std::abort();  // finite and sane (NaN fails both)
            total += n;
            if (n == 0) break;
        }
        const int64_t frames = dec->info().frames;
        dec->seek(frames > 0 ? uint64_t(frames) / 2 : 0);
        dec->read(buf.data(), 512);
        dec->seek(0);
        dec->read(buf.data(), 512);
        dec->seek(~uint64_t(0));
        (void)total;
    }
    // whole-file decode with conversion
    DecodedClip clip;
    OutputFormat want;
    want.channels = 2;
    want.sample_rate = 22050;
    if (decode_all(memory_source(std::move(in)), lim, want, clip, &err)) {
        if (clip.channels != 2 || clip.pcm.size() != size_t(clip.frames) * 2) std::abort();
    }
    return 0;
}
