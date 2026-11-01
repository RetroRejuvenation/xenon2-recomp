// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Compiled with floating-point contraction off (CMakeLists.txt, as decode.cpp): the gains are computed in doubles from exact
// integers and basic arithmetic only, so every platform gets the same integers.
#pragma STDC FP_CONTRACT OFF
#include "audio/pack_audio.h"

#include "audio/audio_dump.h"
#include "audio/decode.h"
#include "modern/layout.h"

#include <algorithm>
#include <cctype>

namespace x2::audio {

namespace {
constexpr int kMinPeriod = 124;       // Paula's DMA limit (paula.h)

// e^x for |x| < 20 from +,*,/ only: ten halvings, a Taylor series, ten squarings. Not libm: libm results differ between platforms
// in the last bit, and the gains it feeds are quantised to integers that decide golden audio hashes.
double det_exp(double x) {
    const double y = x / 1024.0;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k <= 9; ++k) {
        term = term * y / double(k);
        sum += term;
    }
    for (int i = 0; i < 10; ++i) sum *= sum;
    return sum;
}
double db_to_linear(double db) { return det_exp(db * 0.11512925464970229); }  // ln(10) / 20

// sin(x) for x in [0, pi/2].
double det_sin(double x) {
    const double x2 = x * x;
    double term = x, sum = x;
    for (int k = 1; k <= 11; ++k) {
        term = -term * x2 / (double(2 * k) * double(2 * k + 1));
        sum += term;
    }
    return sum;
}
constexpr double kHalfPi = 1.57079632679489661923;

int32_t q16(double v) { return int32_t(v * 65536.0 + 0.5); }
} // namespace

struct PackAudio::Prepared {
    std::vector<int16_t> pcm;  // interleaved, at the output rate
    int channels = 1;
    uint64_t frames = 0;
    uint64_t len_q32 = 0;      // the file's exact length in output frames (Q32.32): where a loop wraps, so loops do not drift
};

struct PackAudio::Voice {
    std::shared_ptr<const Prepared> clip;
    const pack::ClipEntry* entry = nullptr;
    int ch = 0;
    int cat = 0;                // 0 effect volume, 1 speech volume
    uint64_t start_frame = 0;
    uint64_t pos = 0;           // Q32.32 frames into the clip
    uint64_t step = 1ull << 32;
    uint32_t ref_period = 0, period = 0;
    int32_t base_l = 0, base_r = 0;     // Q16 gain of each side before volume, envelope and category
    int32_t vol_cur = 65536, vol_tgt = 65536;
    int32_t env = 65536, env_step = 0;
    bool fading = false, attached = true, loop = false, cut_channel = true, follow = true;
    int fade_ms = 5;
    uint32_t lc = 0;
    uint16_t len = 0;
    uint64_t serial = 0;
};

struct PackAudio::Queued {
    enum Kind : uint8_t { Start, Cut, Period, Volume, Reload } kind = Start;
    uint64_t frame = 0;
    int ch = 0;
    uint32_t period = 0, lc = 0;
    uint16_t len = 0;
    int32_t vol = 0;
    uint64_t serial = 0;  // Period, Volume, Reload: only the voice that followed the channel when the event happened
    Voice voice;
};

namespace {
// A pack file is offset based; the decoders want a stream with a position.
class PackStream final : public ByteSource {
public:
    explicit PackStream(pack::ByteSource& s) : s_(s) {}
    size_t read(void* dst, size_t n) override {
        const uint64_t left = pos_ < s_.size() ? s_.size() - pos_ : 0;
        const size_t got = n && left ? s_.read(pos_, dst, size_t(std::min<uint64_t>(n, left))) : 0;
        pos_ += got;
        return got;
    }
    bool seek(uint64_t pos) override {
        if (pos > s_.size()) return false;
        pos_ = pos;
        return true;
    }
    uint64_t tell() const override { return pos_; }
    uint64_t size() const override { return s_.size(); }

private:
    pack::ByteSource& s_;
    uint64_t pos_ = 0;
};

DecodeLimits decode_limits(const pack::AudioLimits& l) {
    DecodeLimits d;
    d.budget = l.max_decoded_bytes;
    d.max_file_bytes = l.max_file_bytes;
    d.max_frames = l.max_frames;
    d.min_sample_rate = l.min_rate;
    d.max_sample_rate = l.max_rate;
    d.max_channels = l.max_channels;
    return d;
}
void fill_info(const AudioInfo& a, pack::AudioInfo& out) {
    out.format = a.format == Format::Wav ? "wav" : a.format == Format::Flac ? "flac" : "vorbis";
    out.channels = a.channels;
    out.rate = a.sample_rate;
    out.frames = a.frames;
    out.has_loop = a.loop.has_value();
    if (a.loop) {
        out.loop_start = a.loop->start;
        out.loop_end = a.loop->end;
    }
}
bool codec_probe(pack::ByteSource& src, const pack::AudioLimits& limits, pack::AudioInfo& info, std::string& why) {
    auto dec = AudioDecoder::open(std::make_unique<PackStream>(src), decode_limits(limits), &why);
    if (!dec) return false;
    fill_info(dec->info(), info);
    return true;
}
bool codec_decode(pack::ByteSource& src, const pack::AudioLimits& limits, pack::DecodedClip& out, pack::AudioInfo& info, std::string& why) {
    FloatClip clip;
    if (!decode_all_f32(std::make_unique<PackStream>(src), decode_limits(limits), OutputFormat{}, clip, &why)) return false;
    fill_info(clip.source, info);
    info.peak = clip.peak;
    out.channels = clip.channels;
    out.rate = clip.sample_rate;
    out.pcm.resize(clip.pcm.size());
    for (size_t i = 0; i < clip.pcm.size(); ++i) out.pcm[i] = float_to_s16(clip.pcm[i]);
    return true;
}
} // namespace

const pack::AudioCodec& pack_codec() {
    static const pack::AudioCodec codec{codec_probe, codec_decode};
    return codec;
}

PackAudio::PackAudio() = default;

int PackAudio::active_voices() const { return int(voices_.size()); }

PackAudio::~PackAudio() {
    if (paula_) paula_->set_event_sink(nullptr);
}

void PackAudio::attach(Paula& paula, const uint8_t* chip, uint32_t chip_mask, pack::GameFamily game, const modern::AudioLayout& layout) {
    paula_ = &paula;
    chip_ = chip;
    chip_mask_ = chip_mask;
    game_ = game;
    speech_lo_ = layout.speech_lo;
    speech_hi_ = layout.speech_hi;
    effects_mapped_ = game != pack::GameFamily::Disk || layout.effect_lo != modern::kUnknownAddr;
    side_ = layout.side;
    rate_ = paula.config().output_rate;
    const double sep = paula.config().stereo_separation;
    side_same_ = (1.0 + sep) * 0.5;
    side_cross_ = (1.0 - sep) * 0.5;
    // A full-scale file plays as loud as one Paula channel at full volume (half of a side's full scale, at the output gain), which
    // is the level of the original sample, so a dump plays back at the original's level with gain_db 0.
    k_ = 0.5 * double(paula.config().output_gain);
    ramp_step_ = std::max(1, 65536 / std::max(1, rate_ / 500));  // a volume change takes 2 ms
    paula.set_event_sink([this](const SoundEvent& e) { on_event(e); });
}

void PackAudio::set_dumper(AudioDumper* d) {
    dumper_ = d;
    if (d) d->set_rate(rate_);
}

void PackAudio::set_sfx_volume(int percent) { cat_q16_[0] = int32_t(std::clamp(percent, 0, 100) * 65536 / 100); }

bool PackAudio::audio_on() {
    return pack::pack_settings().audio_active() && pack::packs().has_audio();
}

void PackAudio::on_file_loaded(uint32_t dest, const std::string& name, uint32_t size) {
    std::string n = name;
    if (n.size() > 4 && (n.compare(0, 4, "CD0:") == 0 || n.compare(0, 4, "cd0:") == 0)) n.erase(0, 4);
    for (char& c : n) c = char(std::toupper(uint8_t(c)));
    if (n.compare(0, 6, "SPEECH") != 0) return;
    loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(),
                                 [&](const LoadedFile& f) { return dest < f.dest + f.size && f.dest < dest + size; }),
                  loaded_.end());
    loaded_.push_back({dest, size, n});
    if (loaded_.size() > 8) loaded_.erase(loaded_.begin());
}

std::string PackAudio::cd0_name_for(uint32_t addr) const {
    for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it)
        if (addr >= it->dest && addr < it->dest + it->size) return it->name;
    return std::string();
}

bool PackAudio::classify(const SoundEvent& e, bool& speech) const {
    speech = false;
    switch (game_) {
    case pack::GameFamily::Disk:
        return effects_mapped_ && e.src == AudioSource::Effect;
    case pack::GameFamily::Cdtv:
        speech = speech_hi_ > speech_lo_ && e.lc >= speech_lo_ && e.lc < speech_hi_;
        return true;
    case pack::GameFamily::St:
        break;
    }
    return false;
}

// The queue is kept in time order; most events are appended, a deferred one is inserted behind the events that precede it.
void PackAudio::push(Queued&& q) {
    auto at = std::upper_bound(queue_.begin(), queue_.end(), q.frame, [](uint64_t f, const Queued& x) { return f < x.frame; });
    queue_.insert(at, std::move(q));
}

void PackAudio::on_event(const SoundEvent& e) {
    const size_t ch = e.ch & 3;
    switch (e.kind) {
    case SoundEvent::Start:
        on_start(e);
        break;
    case SoundEvent::Stop:
        if (dumper_) dumper_->on_channel_end(int(ch), e.frame);
        cur_len_[ch] = 0;
        if (owned_[ch]) {
            Queued q;
            q.kind = Queued::Cut;
            q.frame = e.frame + kLatencyFrames;
            q.ch = int(ch);
            push(std::move(q));
            owned_[ch] = false;
        }
        break;
    case SoundEvent::Period:
        if (dumper_) dumper_->on_period(int(ch), e.per, e.frame);
        if (owned_[ch]) {
            // A write of the minimum period is deferred by a few milliseconds: the effect code parks a channel (period 124, a one-word
            // loop) just before it turns the DMA off, which on the original is one sample at the park period and must not make a voice
            // that is fading out chirp. Every other period is a pitch and applies at once.
            Queued q;
            q.kind = Queued::Period;
            q.serial = owner_[ch];
            q.frame = e.frame + kLatencyFrames + (e.per && e.per <= kMinPeriod ? uint64_t(kPeriodDeferMs * rate_ / 1000) : 0);
            q.ch = int(ch);
            q.period = e.per ? e.per : 65536u;
            push(std::move(q));
        }
        break;
    case SoundEvent::Volume:
        if (owned_[ch]) {
            Queued q;
            q.kind = Queued::Volume;
            q.serial = owner_[ch];
            q.frame = e.frame + kLatencyFrames;
            q.ch = int(ch);
            q.vol = int32_t(e.vol) * 1024;
            push(std::move(q));
        }
        break;
    case SoundEvent::Reload:
        if (dumper_ && cur_len_[ch] && e.lc == cur_lc_[ch] && e.len == cur_len_[ch]) dumper_->on_loop(int(ch));
        if (owned_[ch]) {
            Queued q;
            q.kind = Queued::Reload;
            q.serial = owner_[ch];
            q.frame = e.frame + kLatencyFrames;
            q.ch = int(ch);
            q.lc = e.lc;
            q.len = e.len;
            push(std::move(q));
        }
        break;
    }
    if (queue_.size() > 8192) queue_.erase(queue_.begin(), queue_.begin() + 4096);
}

void PackAudio::on_start(const SoundEvent& e) {
    const size_t ch = e.ch & 3;
    ++stats_.starts;
    if (owned_[ch]) {  // the channel moves on: a voice that follows it ends (or rings out) now
        Queued q;
        q.kind = Queued::Cut;
        q.frame = e.frame + kLatencyFrames;
        q.ch = int(ch);
        push(std::move(q));
        owned_[ch] = false;
    }
    if (dumper_) dumper_->on_channel_end(int(ch), e.frame);
    cur_len_[ch] = 0;
    bool speech = false;
    if (!classify(e, speech)) return;
    ++stats_.candidates;
    const bool mixing = paula_->output_enabled() && audio_on();
    if (!mixing && !dumper_) return;

    const pack::PackLibrary& lib = pack::packs();
    const uint32_t bytes = (e.len ? uint32_t(e.len) : 65536u) * 2u;
    std::string key;
    auto ensure_key = [&]() {
        if (!key.empty()) return;
        scratch_.resize(bytes);
        for (uint32_t i = 0; i < bytes; ++i) scratch_[i] = chip_[(e.lc + i) & chip_mask_];
        key = pack::sample_key(scratch_.data(), bytes);
        ++stats_.hashed;
    };
    const std::string cd0 = speech ? cd0_name_for(e.lc) : std::string();
    const pack::ClipEntry* clip = nullptr;
    if (mixing) {
        if (speech && !cd0.empty()) clip = lib.speech_for_file(cd0);
        if (!clip && lib.has_clip_length(bytes)) {
            ensure_key();
            clip = speech ? lib.speech(key) : lib.sfx(key);
        }
    }
    if (dumper_) {
        ensure_key();
        SampleStart s;
        s.bytes = scratch_.data();
        s.size = bytes;
        s.key = key;
        s.speech = speech;
        s.cd0_file = cd0;
        s.channel = int(ch);
        s.period = e.per;
        s.volume = e.vol;
        s.frame = frame_source_ ? frame_source_() : e.frame;
        dumper_->on_sample(s);
        cur_lc_[ch] = e.lc;
        cur_len_[ch] = e.len ? e.len : 65536u;
    }
    if (clip && clip->clip) queue_start(e, clip, speech);
}

void PackAudio::queue_start(const SoundEvent& e, const pack::ClipEntry* entry, bool speech) {
    std::shared_ptr<const Prepared> prepared = prepare(*entry);
    if (!prepared || prepared->frames == 0) return;
    Queued q;
    q.kind = Queued::Start;
    q.frame = e.frame + kLatencyFrames;
    q.ch = e.ch & 3;
    Voice& v = q.voice;
    v.clip = std::move(prepared);
    v.entry = entry;
    v.ch = q.ch;
    v.cat = speech ? 1 : 0;
    v.start_frame = q.frame;
    v.serial = ++serial_;
    v.period = e.per ? e.per : 65536u;
    v.ref_period = entry->ref_period > 0 ? uint32_t(entry->ref_period) : v.period;
    v.follow = entry->pitch == pack::PitchMode::Follow;
    v.loop = entry->loop;
    v.cut_channel = entry->cut == pack::CutMode::Channel;
    v.fade_ms = std::clamp(entry->fade_out_ms, 0, 2000);
    v.lc = e.lc;
    v.len = e.len;
    v.vol_cur = v.vol_tgt = int32_t(e.vol) * 1024;
    const uint32_t pe = std::max<uint32_t>(v.period, kMinPeriod);
    v.step = v.follow ? (uint64_t(v.ref_period) << 32) / pe : (1ull << 32);
    // Paula's output stage delays the original by 15.625 frames: the decimating FIR has a group delay of 63 samples at 4x (15.75 frames) and
    // each of those samples is the area average of the interval that ends at it (half a sample, 0.125 frames, earlier). The voice starts on
    // frame 16, so it begins 3/8 of a frame into the clip.
    v.pos = (v.step >> 2) + (v.step >> 3);

    const double db = db_to_linear(double(std::clamp(entry->gain_db, -60.0f, 24.0f)));
    double gl = k_ * db, gr = k_ * db;
    if (v.clip->channels == 1) {
        if (entry->pan == pack::PanMode::Position) {
            const double p = std::clamp(double(entry->pan_pos), -1.0, 1.0);
            const double a = (p + 1.0) * (kHalfPi * 0.5);
            gl = k_ * db * det_sin(kHalfPi - a);
            gr = k_ * db * det_sin(a);
        } else if (side_[size_t(v.ch)] == 0) {
            gl = k_ * db * side_same_;
            gr = k_ * db * side_cross_;
        } else if (side_[size_t(v.ch)] == 1) {
            gl = k_ * db * side_cross_;
            gr = k_ * db * side_same_;
        } else {
            gl = gr = k_ * db * 0.70710678118654752;
        }
    }
    v.base_l = q16(gl);
    v.base_r = q16(gr);

    // The original voice leaves the mix (or sinks under the replacement) from the channel's start to its next start or stop.
    if (entry->original == pack::OriginalMode::Mute) paula_->set_channel_mix(v.ch, 0);
    else paula_->set_channel_mix(v.ch, int(std::clamp(db_to_linear(double(entry->original_mix_db)) * 256.0 + 0.5, 0.0, 256.0)));
    owned_[size_t(v.ch)] = true;
    owner_[size_t(v.ch)] = v.serial;
    // The game often starts one sample on two channels at once (a pair: both sides, or two channels of one side). A mono file then
    // gets a voice per start, as the original has a channel per start. A stereo file already carries both sides, so the starts that
    // follow the first within a few milliseconds add nothing: they only gate their own original channel.
    if (v.clip->channels == 2) {
        const uint64_t window = uint64_t(kPeriodDeferMs * rate_ / 1000);
        auto near = [&](uint64_t f) { return f + window >= q.frame && f <= q.frame + window; };
        for (const Voice& o : voices_)
            if (o.entry == entry && near(o.start_frame)) return;
        for (const Queued& o : queue_)
            if (o.kind == Queued::Start && o.voice.entry == entry && near(o.frame)) return;
    }
    ++stats_.replaced;
    push(std::move(q));
}

std::shared_ptr<const PackAudio::Prepared> PackAudio::prepare(const pack::ClipEntry& e) {
    const pack::DecodedClip* c = e.clip.get();
    if (!c || c->channels < 1 || c->channels > 2 || c->rate <= 0 || c->frames() == 0) return nullptr;
    auto it = prepared_.find(c);
    if (it != prepared_.end()) return it->second;
    auto p = std::make_shared<Prepared>();
    p->channels = c->channels;
    {
        const uint64_t num = c->frames() * uint64_t(rate_), den = uint64_t(c->rate);  // fits: frames <= 1.4e9, rate <= 192000
        p->len_q32 = ((num / den) << 32) + (((num % den) << 32) / den);
    }
    if (c->rate == rate_) {
        p->pcm = c->pcm;
        p->frames = c->frames();
    } else {
        std::vector<float> in(c->pcm.size());
        for (size_t i = 0; i < in.size(); ++i) in[i] = float(c->pcm[i]) * (1.0f / 32768.0f);
        StreamResampler rs(c->channels, c->rate, rate_);
        rs.push(in.data(), size_t(c->frames()));
        rs.finish();
        const uint64_t n = StreamResampler::output_frames(c->frames(), c->rate, rate_);
        std::vector<float> outf(size_t(n) * size_t(c->channels));
        const size_t got = rs.pull(outf.data(), size_t(n));
        p->frames = got;
        p->pcm.resize(got * size_t(c->channels));
        for (size_t i = 0; i < p->pcm.size(); ++i) p->pcm[i] = float_to_s16(outf[i]);
    }
    if (prepared_.size() < 4096) prepared_[c] = p;
    return p;
}

void PackAudio::fade_voice(Voice& v) {
    if (v.fading) return;
    v.fading = true;
    if (v.fade_ms <= 0) {
        v.env = 0;
        return;
    }
    const int frames = std::max(1, v.fade_ms * rate_ / 1000);
    v.env_step = std::max(1, int(int64_t(v.env) / frames));
}

void PackAudio::apply(const Queued& q) {
    switch (q.kind) {
    case Queued::Start: {
        for (Voice& v : voices_) {
            if (v.ch != q.ch || !v.attached) continue;
            v.attached = false;
            if (v.cut_channel) fade_voice(v);
        }
        int live = 0;
        for (const Voice& v : voices_)
            if (!v.fading) ++live;
        if (live >= kMaxVoices) {
            Voice* oldest = nullptr;
            for (Voice& v : voices_)
                if (!v.fading && (!oldest || v.serial < oldest->serial)) oldest = &v;
            if (oldest) {
                oldest->fade_ms = std::min(oldest->fade_ms, 5);
                fade_voice(*oldest);
                oldest->attached = false;
                ++stats_.stolen;
            }
        }
        if (voices_.size() >= 2 * size_t(kMaxVoices)) {  // faders still running: the oldest is dropped at once
            auto old = std::min_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) { return a.serial < b.serial; });
            voices_.erase(old);
        }
        voices_.push_back(q.voice);
        break;
    }
    case Queued::Cut:
        for (Voice& v : voices_) {
            if (v.ch != q.ch || !v.attached) continue;
            v.attached = false;
            if (v.cut_channel) fade_voice(v);
        }
        break;
    case Queued::Period:
        for (Voice& v : voices_)
            if (v.ch == q.ch && v.attached && v.serial == q.serial) {
                v.period = q.period;
                if (v.follow) v.step = (uint64_t(v.ref_period) << 32) / std::max<uint32_t>(q.period, kMinPeriod);
            }
        break;
    case Queued::Volume:
        for (Voice& v : voices_)
            if (v.ch == q.ch && v.attached && v.serial == q.serial) v.vol_tgt = q.vol;
        break;
    case Queued::Reload:
        for (Voice& v : voices_)
            if (v.ch == q.ch && v.attached && v.serial == q.serial && v.loop && (v.lc != q.lc || v.len != q.len)) {
                v.attached = false;
                fade_voice(v);
            }
        break;
    }
}

void PackAudio::render(int16_t* out, uint64_t first, uint64_t from, uint64_t to) {
    if (to <= from || voices_.empty()) return;
    const size_t n = size_t(to - from);
    acc_.assign(n * 2, 0);
    for (Voice& v : voices_) {
        if (v.start_frame >= to) continue;
        const Prepared& c = *v.clip;
        const uint64_t begin = std::max(from, v.start_frame);
        const int32_t cat = cat_q16_[v.cat];
        for (uint64_t f = begin; f < to; ++f) {
            if (v.pos >= c.len_q32) {
                if (!v.loop) {
                    v.env = 0;
                    v.fading = true;
                    break;
                }
                v.pos -= c.len_q32;
            }
            uint64_t idx = v.pos >> 32;
            if (idx >= c.frames) idx = c.frames - 1;
            const int64_t frac = int64_t((v.pos >> 16) & 0xFFFF);
            const bool last = idx + 1 >= c.frames;
            const size_t i0 = size_t(idx) * size_t(c.channels), i1 = last ? (v.loop ? 0 : i0) : i0 + size_t(c.channels);
            const bool zero_next = last && !v.loop;
            int64_t sl, sr;
            {
                const int64_t a = c.pcm[i0], b = zero_next ? 0 : c.pcm[i1];
                sl = a + (((b - a) * frac) >> 16);
            }
            if (c.channels == 2) {
                const int64_t a = c.pcm[i0 + 1], b = zero_next ? 0 : c.pcm[i1 + 1];
                sr = a + (((b - a) * frac) >> 16);
            } else {
                sr = sl;
            }
            if (v.vol_cur != v.vol_tgt) {
                v.vol_cur = v.vol_cur < v.vol_tgt ? std::min(v.vol_tgt, v.vol_cur + ramp_step_) : std::max(v.vol_tgt, v.vol_cur - ramp_step_);
            }
            if (v.fading) {
                v.env = std::max(0, v.env - v.env_step);
            }
            if (v.fading && v.env == 0) break;  // finished: removed below
            const int64_t g = (int64_t(v.vol_cur) * v.env) >> 16;
            const int64_t gl = (((int64_t(v.base_l) * g) >> 16) * cat) >> 16, gr = (((int64_t(v.base_r) * g) >> 16) * cat) >> 16;
            const size_t o = size_t(f - from) * 2;
            acc_[o] += int32_t((sl * gl) >> 16);
            acc_[o + 1] += int32_t((sr * gr) >> 16);
            v.pos += v.step;
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const Voice& v) { return v.fading && v.env == 0; }),
                  voices_.end());
    for (size_t i = 0; i < n * 2; ++i) {
        const int32_t s = int32_t(out[size_t(from - first) * 2 + i]) + acc_[i];
        out[size_t(from - first) * 2 + i] = int16_t(std::clamp(s, -32768, 32767));
    }
}

void PackAudio::release_all_gates() {
    if (!paula_) return;
    for (int c = 0; c < 4; ++c) paula_->set_channel_mix(c, 256);
}

void PackAudio::after_frame(Paula& paula) {
    cat_q16_[1] = int32_t(std::clamp(pack::pack_settings().speech_volume, 0, 100) * 65536 / 100);
    const bool on = audio_on();
    if (was_on_ && !on) {  // the Faithful switch (or the audio toggle) went off: the original comes back, the voices fade away
        release_all_gates();
        for (Voice& v : voices_) {
            v.attached = false;
            v.fade_ms = std::min(v.fade_ms, 5);
            fade_voice(v);
        }
        queue_.clear();
        owned_.fill(false);
    }
    was_on_ = on;
    if (queue_.empty() && voices_.empty()) {
        mixed_ = paula.frames_out();
        return;
    }
    std::vector<int16_t>& out = paula.output();
    const uint64_t end = paula.frames_out();
    const uint64_t buffered = std::min<uint64_t>(end, out.size() / 2);
    const uint64_t buf_first = end - buffered;
    const uint64_t begin = std::max<uint64_t>(mixed_, buf_first);
    uint64_t at = begin;
    size_t used = 0;
    for (; used < queue_.size(); ++used) {
        const Queued& q = queue_[used];
        if (q.frame >= end) break;
        const uint64_t t = std::clamp<uint64_t>(q.frame, at, end);
        if (buffered) render(out.data(), buf_first, at, t);
        at = t;
        apply(q);
    }
    queue_.erase(queue_.begin(), queue_.begin() + std::ptrdiff_t(used));
    if (buffered) render(out.data(), buf_first, at, end);
    mixed_ = end;
}

void PackAudio::reset(Paula& paula) {
    queue_.clear();
    voices_.clear();
    owned_.fill(false);
    release_all_gates();
    mixed_ = paula.frames_out();
    for (auto& l : loaded_) l = LoadedFile{};
    loaded_.clear();
}

} // namespace x2::audio
