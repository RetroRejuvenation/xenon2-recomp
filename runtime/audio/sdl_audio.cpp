#include "sdl_audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace x2 {

bool SdlAudioOut::open(int rate, int latency_ms, std::string& error) {
    close();
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error = std::string("SDL audio init failed: ") + SDL_GetError();
        return false;
    }
    target_ = size_t(std::max(5, latency_ms)) * size_t(rate) / 1000;
    capacity_ = std::max<size_t>(target_ * 4, size_t(rate) / 4);
    ring_.assign(capacity_ * 2, 0);
    read_ = write_ = 0;
    underruns_ = 0;
    dropped_ = 0;
    started_ = false;
    ratio_ = 1.0f;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, rate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlAudioOut::callback, this);
    if (!stream_) {
        error = std::string("SDL_OpenAudioDeviceStream failed: ") + SDL_GetError();
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void SdlAudioOut::close() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_); // also closes the device and stops the callback
        stream_ = nullptr;
    }
}

void SdlAudioOut::push(const int16_t* frames, size_t n) {
    if (!stream_) return;
    size_t w = write_.load(std::memory_order_relaxed), r = read_.load(std::memory_order_acquire);
    size_t room = capacity_ - (w - r);
    if (n > room) {
        dropped_ += n - room;
        n = room;
    }
    for (size_t i = 0; i < n; ++i) {
        size_t slot = (w + i) % capacity_;
        if (volume_ == 1.0f) {
            ring_[slot * 2] = frames[i * 2];
            ring_[slot * 2 + 1] = frames[i * 2 + 1];
        } else {
            ring_[slot * 2] = int16_t(float(frames[i * 2]) * volume_);
            ring_[slot * 2 + 1] = int16_t(float(frames[i * 2 + 1]) * volume_);
        }
    }
    write_.store(w + n, std::memory_order_release);
    if (!started_ && w + n - r >= target_) started_ = true; // (re)start once primed to the target
    // Drift control: speed playback up slightly when the buffer is above target, slow it when below.
    double err = (double(w + n - r) - double(target_)) / double(std::max<size_t>(target_, 1));
    float ratio = float(1.0 + std::clamp(err * 0.005, -0.005, 0.005));
    if (std::abs(ratio - ratio_) > 0.0002f) {
        ratio_ = ratio;
        SDL_SetAudioStreamFrequencyRatio(stream_, ratio_);
    }
}

void SdlAudioOut::flush() {
    if (!stream_) return;
    // The device callback runs with the stream locked, so this cannot interleave with pull().
    SDL_LockAudioStream(stream_);
    started_ = false;
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    SDL_ClearAudioStream(stream_);
    SDL_UnlockAudioStream(stream_);
}

void SdlAudioOut::set_paused(bool paused) {
    if (!stream_) return;
    if (paused) {
        SDL_PauseAudioStreamDevice(stream_);
        flush();
    } else {
        flush();
        SDL_ResumeAudioStreamDevice(stream_);
    }
}

void SdlAudioOut::callback(void* user, SDL_AudioStream* stream, int additional, int /*total*/) {
    static_cast<SdlAudioOut*>(user)->pull(stream, additional);
}

void SdlAudioOut::pull(SDL_AudioStream* stream, int bytes) {
    size_t want = size_t(std::max(0, bytes)) / 4;
    if (!want) return;
    scratch_.assign(want * 2, 0);
    if (started_) {
        size_t r = read_.load(std::memory_order_relaxed), w = write_.load(std::memory_order_acquire);
        size_t n = std::min(want, w - r);
        for (size_t i = 0; i < n; ++i) {
            size_t slot = (r + i) % capacity_;
            scratch_[i * 2] = ring_[slot * 2];
            scratch_[i * 2 + 1] = ring_[slot * 2 + 1];
        }
        read_.store(r + n, std::memory_order_release);
        if (n < want) {
            ++underruns_;
            started_ = false; // re-prime to the latency target before playing again
        }
    }
    SDL_PutAudioStreamData(stream, scratch_.data(), int(want * 4));
}

} // namespace x2
