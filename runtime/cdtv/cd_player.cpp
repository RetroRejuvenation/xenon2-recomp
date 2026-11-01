// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "cdtv/cd_player.h"

// The resampler's float arithmetic must round the same way in every build (the audio hashes of the CD replays are exact): no
// fused multiply-add. GCC and Clang take the flag form per file (CMakeLists.txt) as well; this is the standard pragma.
#pragma STDC FP_CONTRACT OFF

#include <algorithm>
#include <cmath>
#include <string>

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

namespace {
// A CD track file as a stream: s16le stereo PCM at 44.1 kHz, widened to float exactly.
class CdTrackStream final : public audio::StreamSource {
public:
    explicit CdTrackStream(const TrackSource& t) { reader_.open(t); }
    void read(int64_t first, size_t n, float* out) override {
        buf_.resize(n * 2);
        reader_.read(first, n, buf_.data());
        for (size_t i = 0; i < n * 2; ++i) out[i] = float(buf_[i]);
    }

private:
    PcmReader reader_;
    std::vector<int16_t> buf_;
};
} // namespace

std::shared_ptr<const audio::StreamSpec> cd_stream_spec(const TrackSource& t) {
    auto s = std::make_shared<audio::StreamSpec>();
    const TrackSource copy = t;
    s->open = [copy]() { return std::make_unique<CdTrackStream>(copy); };
    s->rate = kSampleRate;
    s->frames = t.frames;
    s->loop = audio::LoopRule::Game;
    s->name = "cd track " + std::to_string(t.number);
    return s;
}

CdPlayer::CdPlayer() { configure(48000, 0); }

void CdPlayer::build_kernel(int src_rate, std::vector<float>& k) const {
    const double fc = 0.46 * std::min(1.0, double(rate_) / src_rate), half = kTaps / 2.0;
    k.assign(size_t(kPhases + 1) * kTaps, 0.0f);
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
        for (int j = 0; j < kTaps; ++j) k[size_t(ph) * kTaps + size_t(j)] = float(h[j] / sum);
    }
}

void CdPlayer::configure(int output_rate, int crossfade_ms) {
    rate_ = std::clamp(output_rate, 8000, 192000);
    crossfade_ms_ = std::clamp(crossfade_ms, 0, 500);
    xfade_frames_ = uint64_t(rate_) * uint64_t(crossfade_ms_) / 1000;
    for (auto& [src_rate, k] : kernels_) build_kernel(src_rate, k); // the output rate changed: same keys, new kernels
    if (rate_ != kSampleRate) kernel_for(kSampleRate);
    for (Voice& v : voices_) v.kernel = kernel_for(v.src_rate);
}

const float* CdPlayer::kernel_for(int src_rate) {
    if (src_rate == rate_) return nullptr;
    auto it = kernels_.find(src_rate);
    if (it == kernels_.end()) {
        it = kernels_.emplace(src_rate, std::vector<float>()).first;
        build_kernel(src_rate, it->second);
    }
    return it->second.data();
}

uint64_t CdPlayer::spec_xfade(const audio::StreamSpec& s) const {
    if (s.crossfade_ms < 0) return xfade_frames_;
    return uint64_t(rate_) * uint64_t(std::min(s.crossfade_ms, 5000)) / 1000;
}

void CdPlayer::set_volume(int percent) { volume_target_ = std::clamp(percent, 0, 100) / 100.0; }

void CdPlayer::fade_out_all(uint64_t frames) {
    for (Voice& v : voices_) {
        if (v.fading_out) continue;
        v.fading_out = true;
        v.fade_step = -1.0 / double(std::max<uint64_t>(1, frames));
    }
}

void CdPlayer::start_voice(const std::shared_ptr<const audio::StreamSpec>& s, uint64_t fade_in_frames, int64_t start) {
    Voice v;
    v.src = s->open();
    v.spec = s;
    v.src_rate = s->rate;
    v.kernel = kernel_for(s->rate);
    v.pos = start;
    const bool seamless = s->loop == audio::LoopRule::Seamless && s->loop_end > s->loop_start;
    const uint64_t xf = spec_xfade(*s);
    if (seamless) {
        const uint64_t body = (s->loop_end - s->loop_start) * uint64_t(rate_) / uint64_t(s->rate);
        v.xfade = std::min(xf, body / 2);
        v.wrap = v.xfade == 0;
        v.limit = v.wrap ? INT64_MAX : int64_t(s->loop_end);
    } else {
        v.xfade = s->loop == audio::LoopRule::Game ? xf : 0; // the CD's own crossfade loop
        v.limit = int64_t(s->frames);
    }
    if (fade_in_frames) {
        v.fade = 0.0;
        v.fade_step = 1.0 / double(fade_in_frames);
    }
    voices_.push_back(std::move(v));
}

void CdPlayer::play(const TrackSource* t, int number) {
    if (!t) { stop(); return; }
    play(cd_stream_spec(*t), number);
}

void CdPlayer::play(std::shared_ptr<const audio::StreamSpec> spec, int number) {
    if (!spec) { stop(); return; }
    bool playing = false; // fade in only to crossfade from another track, never from silence
    for (const Voice& v : voices_) playing = playing || !v.fading_out;
    const uint64_t xf = spec_xfade(*spec);
    const uint64_t old_out = current_ ? uint64_t(rate_) * uint64_t(current_->fade_out_ms) / 1000 : 0;
    fade_out_all(xf ? xf : std::max<uint64_t>(old_out, uint64_t(rate_) / 200));
    const uint64_t fade_in = playing && xf ? xf : uint64_t(rate_) * uint64_t(spec->fade_in_ms) / 1000;
    start_voice(spec, fade_in, 0);
    current_ = std::move(spec);
    track_ = number;
    ticks_ = 0;
}

void CdPlayer::stop() {
    const uint64_t out = current_ ? uint64_t(rate_) * uint64_t(current_->fade_out_ms) / 1000 : 0;
    fade_out_all(std::max<uint64_t>(out, uint64_t(rate_) / 200));
    current_.reset();
    track_ = 0;
}

bool CdPlayer::finished() const {
    if (!current_ || current_->loop == audio::LoopRule::Off) return false;
    for (const Voice& v : voices_)
        if (!v.fading_out) return false; // still playing
    return true;
}

void CdPlayer::restart() {
    if (!current_) return;
    start_voice(current_, uint64_t(rate_) * uint64_t(current_->fade_in_ms) / 1000, 0); // finished: the game plays the track again
    ++loops_;
}

void CdPlayer::tick() {
    if (!self_loop_ || !current_ || ++ticks_ % kLoopPollTicks) return;
    if (finished()) restart();
}

int64_t CdPlayer::position() const {
    for (const Voice& v : voices_)
        if (!v.fading_out) return v.pos;
    return -1;
}

void CdPlayer::fetch(Voice& v, int64_t first, size_t count, float* dst) {
    if (!v.wrap) { v.src->read(first, count, dst); return; }
    // Positions past the loop end map back into [loop_start, loop_end): the taps see the loop continue.
    const int64_t s = int64_t(v.spec->loop_start), e = int64_t(v.spec->loop_end);
    for (size_t i = 0; i < count;) {
        const int64_t f = first + int64_t(i);
        const int64_t m = f >= e ? s + (f - s) % (e - s) : f;
        const size_t run = f >= e ? size_t(e - m) : size_t(std::min<int64_t>(int64_t(count - i), e - f));
        v.src->read(m, std::min(run, count - i), dst + i * 2);
        i += std::min(run, count - i);
    }
}

void CdPlayer::render_voice(Voice& v, double* acc, size_t n) {
    const bool direct = v.kernel == nullptr;
    // Source frames this block needs: kTaps/2 - 1 before the position, kTaps/2 after the last one.
    const size_t advance = direct ? n : size_t((v.frac + uint64_t(n) * uint64_t(v.src_rate)) / uint64_t(rate_)) + 1;
    const int64_t first = direct ? v.pos : v.pos - (kTaps / 2 - 1);
    const size_t count = direct ? n : advance + kTaps;
    scratch_.resize(count * 2);
    fetch(v, first, count, scratch_.data());
    const double vgain = v.spec->gain;
    for (size_t i = 0; i < n; ++i) {
        if (v.pos >= v.limit) { v.finished = true; break; }
        double l, r;
        if (direct) {
            l = scratch_[size_t(v.pos - first) * 2];
            r = scratch_[size_t(v.pos - first) * 2 + 1];
            ++v.pos;
        } else {
            const double p = double(v.frac) * kPhases / rate_;
            const int ip = std::min(int(p), kPhases - 1);
            const float t = float(p - ip);
            const float* k0 = &v.kernel[size_t(ip) * kTaps];
            const float* k1 = k0 + kTaps;
            const float* x = &scratch_[size_t(v.pos - (kTaps / 2 - 1) - first) * 2];
            float sl = 0, sr = 0;
            for (int j = 0; j < kTaps; ++j) {
                const float k = k0[j] + t * (k1[j] - k0[j]);
                sl += k * x[j * 2];
                sr += k * x[j * 2 + 1];
            }
            l = sl;
            r = sr;
            v.frac += uint64_t(v.src_rate);
            while (v.frac >= uint64_t(rate_)) { v.frac -= uint64_t(rate_); ++v.pos; }
        }
        const double g = v.fade >= 1.0 ? 1.0 : std::sin(v.fade * kPi / 2); // equal-power fades
        acc[i * 2] += l * g * vgain;
        acc[i * 2 + 1] += r * g * vgain;
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
        // Seamless loop with a crossfade: start the next pass `crossfade` before the end of the loop.
        if (current_) {
            for (size_t i = 0; i < voices_.size(); ++i) {
                Voice& v = voices_[i];
                if (v.fading_out || v.loop_started || v.xfade == 0) continue;
                const uint64_t left = v.pos < v.limit ? uint64_t(v.limit - v.pos) * uint64_t(rate_) / uint64_t(v.src_rate) : 0;
                if (left > v.xfade) continue;
                v.loop_started = v.fading_out = true;
                v.fade_step = -1.0 / double(std::max<uint64_t>(1, left));
                const int64_t restart = v.spec->loop == audio::LoopRule::Seamless ? int64_t(v.spec->loop_start) : 0;
                const std::shared_ptr<const audio::StreamSpec> spec = v.spec; // start_voice may reallocate voices_
                const uint64_t xf = v.xfade;
                start_voice(spec, xf, restart);
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
