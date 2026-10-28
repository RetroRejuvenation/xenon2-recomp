// SdlAudioOut smoke test on SDL's "dummy" audio driver (no sound device needed): the device thread must
// drain the ring buffer at about the real-time rate, and pacing pushes by queued_frames() must hold the
// latency target without overflow.
#include "audio/sdl_audio.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <string>
#include <vector>

int main() {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    x2::SdlAudioOut out;
    std::string err;
    if (!out.open(48000, 40, err)) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<int16_t> frame(959 * 2, 0); // one emulated frame of stereo samples
    uint64_t start = SDL_GetTicks();
    size_t pushed = 0;
    while (SDL_GetTicks() - start < 1000) {
        if (out.queued_frames() < out.target_frames()) {
            out.push(frame.data(), 959);
            pushed += 959;
        } else {
            SDL_Delay(1);
        }
    }
    size_t queued = out.queued_frames();
    std::printf("pushed %zu frames in 1 s, queued %zu (target %zu), underruns %llu, dropped %llu\n", pushed, queued,
                out.target_frames(), (unsigned long long)out.underruns(), (unsigned long long)out.dropped_frames());
    out.close();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    // Consumed ~1 s of audio (generous bounds: the dummy driver's timing is coarse on loaded CI hosts).
    bool ok = pushed > 48000 * 6 / 10 && pushed < 48000 * 14 / 10 && out.dropped_frames() == 0 && queued <= out.target_frames() + 959;
    std::printf("%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
