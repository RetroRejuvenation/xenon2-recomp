// SDL3 audio output for Paula's stereo int16 stream.
//
// The emulation thread push()es the frames Paula produced for each emulated frame into a lock-free
// single-producer/single-consumer ring buffer; SDL's device thread pulls from it in the stream callback.
// Latency is held near the target (default 40 ms) by nudging the stream's frequency ratio by at most +-0.5%
// in proportion to the fill error, which absorbs the drift between the emulated 50.08 Hz frame rate and the
// host's frame pacing without audible pitch change. On underrun the callback outputs silence (counted) and
// waits until the buffer is primed to the target again;
// on overflow (host loop running far ahead) new frames are dropped (counted). A frame loop that wants to
// pace itself by audio can wait while queued_frames() is above target_frames().
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace x2 {

class SdlAudioOut {
public:
    ~SdlAudioOut() { close(); }
    // Opens the default playback device for `rate` Hz stereo int16 input. Initialises the SDL audio
    // subsystem if needed.
    bool open(int rate, int latency_ms, std::string& error);
    void close();
    bool is_open() const { return stream_ != nullptr; }

    void push(const int16_t* frames, size_t n_frames);
    size_t queued_frames() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }
    size_t target_frames() const { return target_; }
    uint64_t underruns() const { return underruns_.load(); }
    uint64_t dropped_frames() const { return dropped_; }

private:
    static void callback(void* user, SDL_AudioStream* stream, int additional, int total);
    void pull(SDL_AudioStream* stream, int bytes);

    SDL_AudioStream* stream_ = nullptr;
    std::vector<int16_t> ring_; // capacity_ frames, interleaved stereo
    size_t capacity_ = 0, target_ = 0;
    std::atomic<size_t> read_{0}, write_{0}; // monotonically increasing frame counters
    std::atomic<uint64_t> underruns_{0};
    std::atomic<bool> started_{false};
    uint64_t dropped_ = 0;
    std::vector<int16_t> scratch_; // callback-thread buffer
    float ratio_ = 1.0f;
};

} // namespace x2
