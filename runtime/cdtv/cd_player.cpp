#include "cdtv/cd_player.h"

#include <algorithm>
#include <cmath>

namespace x2::cdtv {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kCacheFrames = 16384;
constexpr size_t kBlock = 64; // output frames per render step (loop and fade decisions happen between steps)

double bessel_i0(double x) {
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 50; ++k) {
        term *= q / (double(k) * k);
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}
} // namespace

bool PcmReader::open(const TrackSource& t) {
    in_.open(t.path, std::ios::binary);
    offset_ = t.offset;
    frames_ = t.frames;
    cache_.assign(kCacheFrames * 2, 0);
    cache_first_ = 0;
    cache_frames_ = 0;
    return bool(in_);
}

void PcmReader::read(int64_t first, size_t n, int16_t* out) {
    for (size_t i = 0; i < n;) {
        const int64_t f = first + int64_t(i);
        if (f < 0 || uint64_t(f) >= frames_) {
            out[i * 2] = out[i * 2 + 1] = 0;
            ++i;
            continue;
        }
        if (f < cache_first_ || f >= cache_first_ + int64_t(cache_frames_)) {
            cache_first_ = f;
            cache_frames_ = size_t(std::min<uint64_t>(kCacheFrames, frames_ - uint64_t(f)));
            std::vector<uint8_t> raw(cache_frames_ * 4);
            in_.clear();
            in_.seekg(std::streamoff(offset_ + uint64_t(f) * 4));
            in_.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()));
            const size_t got = size_t(std::max<std::streamsize>(0, in_.gcount()));
            for (size_t k = 0; k < cache_frames_ * 2; ++k) // little-endian on disk, whatever the host
                cache_[k] = k * 2 + 1 < got ? int16_t(uint16_t(raw[k * 2] | raw[k * 2 + 1] << 8)) : 0;
        }
        const size_t at = size_t(f - cache_first_);
        const size_t k = std::min(n - i, cache_frames_ - at);
        std::copy(&cache_[at * 2], &cache_[(at + k) * 2], out + i * 2);
        i += k;
    }
}

CdPlayer::CdPlayer() { configure(48000, 0); }

void CdPlayer::configure(int output_rate, int crossfade_ms) {
    rate_ = std::clamp(output_rate, 8000, 192000);
    crossfade_ms_ = std::clamp(crossfade_ms, 0, 500);
    xfade_frames_ = uint64_t(rate_) * uint64_t(crossfade_ms_) / 1000;
    kernel_.clear();
    if (rate_ == kSampleRate) return;
    const double fc = 0.46 * std::min(1.0, double(rate_) / kSampleRate), half = kTaps / 2.0;
    kernel_.assign(size_t(kPhases + 1) * kTaps, 0.0f);
    for (int ph = 0; ph <= kPhases; ++ph) {
        double sum = 0, h[kTaps];
        for (int j = 0; j < kTaps; ++j) {
            const double t = double(j - (kTaps / 2 - 1)) - double(ph) / kPhases;
            const double x = 2.0 * fc * t, sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double r = t / half;
            const double w = std::fabs(r) >= 1 ? 0.0 : bessel_i0(8.0 * std::sqrt(1.0 - r * r)) / bessel_i0(8.0);
            h[j] = 2.0 * fc * sinc * w;
            sum += h[j];
        }
        for (int j = 0; j < kTaps; ++j) kernel_[size_t(ph) * kTaps + size_t(j)] = float(h[j] / sum);
    }
}

void CdPlayer::set_volume(int percent) { volume_target_ = std::clamp(percent, 0, 100) / 100.0; }

void CdPlayer::fade_out_all(uint64_t frames) {
    for (Voice& v : voices_) {
        if (v.fading_out) continue;
        v.fading_out = true;
        v.fade_step = -1.0 / double(std::max<uint64_t>(1, frames));
    }
}

void CdPlayer::start_voice(const TrackSource* t, bool fade_in) {
    Voice v;
    v.src = std::make_unique<PcmReader>();
    v.src->open(*t);
    v.track = t;
    if (fade_in && xfade_frames_) {
        v.fade = 0.0;
        v.fade_step = 1.0 / double(xfade_frames_);
    }
    voices_.push_back(std::move(v));
}

void CdPlayer::play(const TrackSource* t, int number) {
    if (!t) { stop(); return; }
    bool playing = false; // fade in only to crossfade from another track, never from silence
    for (const Voice& v : voices_) playing = playing || !v.fading_out;
    fade_out_all(xfade_frames_ ? xfade_frames_ : uint64_t(rate_) / 200);
    start_voice(t, playing);
    current_ = t;
    track_ = number;
    ticks_ = 0;
}

void CdPlayer::stop() {
    fade_out_all(uint64_t(rate_) / 200);
    current_ = nullptr;
    track_ = 0;
}

void CdPlayer::tick() {
    if (!current_ || ++ticks_ % kLoopPollTicks) return;
    for (const Voice& v : voices_)
        if (!v.fading_out) return; // still playing
    start_voice(current_, false);  // finished: the CDTV game plays the track again
    ++loops_;
}

int64_t CdPlayer::position() const {
    for (const Voice& v : voices_)
        if (!v.fading_out) return v.pos;
    return -1;
}

void CdPlayer::render_voice(Voice& v, double* acc, size_t n) {
    const int64_t frames = int64_t(v.track->frames);
    const bool direct = kernel_.empty();
    // Source frames this block needs: kTaps/2 - 1 before the position, kTaps/2 after the last one.
    const size_t advance = direct ? n : size_t((v.frac + uint64_t(n) * kSampleRate) / uint64_t(rate_)) + 1;
    const int64_t first = direct ? v.pos : v.pos - (kTaps / 2 - 1);
    const size_t count = direct ? n : advance + kTaps;
    scratch_.resize(count * 2);
    v.src->read(first, count, scratch_.data());
    for (size_t i = 0; i < n; ++i) {
        if (v.pos >= frames) { v.finished = true; break; }
        double l, r;
        if (direct) {
            l = scratch_[size_t(v.pos - first) * 2];
            r = scratch_[size_t(v.pos - first) * 2 + 1];
            ++v.pos;
        } else {
            const double p = double(v.frac) * kPhases / rate_;
            const int ip = std::min(int(p), kPhases - 1);
            const float t = float(p - ip);
            const float* k0 = &kernel_[size_t(ip) * kTaps];
            const float* k1 = k0 + kTaps;
            const int16_t* x = &scratch_[size_t(v.pos - (kTaps / 2 - 1) - first) * 2];
            float sl = 0, sr = 0;
            for (int j = 0; j < kTaps; ++j) {
                const float k = k0[j] + t * (k1[j] - k0[j]);
                sl += k * x[j * 2];
                sr += k * x[j * 2 + 1];
            }
            l = sl;
            r = sr;
            v.frac += kSampleRate;
            while (v.frac >= uint64_t(rate_)) { v.frac -= uint64_t(rate_); ++v.pos; }
        }
        const double g = v.fade >= 1.0 ? 1.0 : std::sin(v.fade * kPi / 2); // equal-power fades
        acc[i * 2] += l * g;
        acc[i * 2 + 1] += r * g;
        if (v.fade_step != 0.0) {
            v.fade = std::clamp(v.fade + v.fade_step, 0.0, 1.0);
            if (v.fade >= 1.0 && v.fade_step > 0) v.fade_step = 0.0;
            if (v.fade <= 0.0 && v.fading_out) { v.finished = true; break; }
        }
    }
}

void CdPlayer::render(int16_t* stereo, size_t n) {
    if (game_paused_ && pause_mode_ == PauseMode::Stop) return; // held: no audio, no progress
    const double ramp = 1.0 / (0.01 * rate_);
    const double pause_target = game_paused_ && pause_mode_ == PauseMode::Duck ? 0.25 : 1.0;
    for (size_t done = 0; done < n;) {
        const size_t k = std::min(kBlock, n - done);
        // Seamless loop: start the next pass `crossfade` before the end.
        if (xfade_frames_ && current_) {
            for (size_t i = 0; i < voices_.size(); ++i) {
                Voice& v = voices_[i];
                if (v.fading_out || v.loop_started) continue;
                const uint64_t left = v.pos < int64_t(v.track->frames) ? (v.track->frames - uint64_t(v.pos)) * uint64_t(rate_) / kSampleRate : 0;
                if (left > xfade_frames_) continue;
                v.loop_started = v.fading_out = true;
                v.fade_step = -1.0 / double(std::max<uint64_t>(1, left));
                start_voice(current_, true);
                ++loops_;
                break;
            }
        }
        acc_.assign(k * 2, 0.0);
        for (Voice& v : voices_)
            if (!v.finished) render_voice(v, acc_.data(), k);
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.finished; }), voices_.end());
        int16_t* out = stereo + done * 2;
        for (size_t i = 0; i < k; ++i) {
            if (volume_ != volume_target_)
                volume_ = volume_ < volume_target_ ? std::min(volume_target_, volume_ + ramp) : std::max(volume_target_, volume_ - ramp);
            if (pause_gain_ != pause_target)
                pause_gain_ = pause_gain_ < pause_target ? std::min(pause_target, pause_gain_ + ramp) : std::max(pause_target, pause_gain_ - ramp);
            const double g = kCdGain * volume_ * pause_gain_;
            for (int c = 0; c < 2; ++c) {
                const double s = std::round(double(out[i * 2 + c]) + acc_[i * 2 + size_t(c)] * g);
                if (s > 32767.0 || s < -32768.0) ++clipped_;
                out[i * 2 + c] = int16_t(std::clamp(s, -32768.0, 32767.0));
            }
        }
        done += k;
    }
}

} // namespace x2::cdtv
