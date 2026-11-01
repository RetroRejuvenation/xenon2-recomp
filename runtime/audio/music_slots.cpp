// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "audio/music_slots.h"

// The gain and the 16-bit scaling must round the same way in every build (pack music has recorded audio hashes): no fused multiply-add.
#pragma STDC FP_CONTRACT OFF

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace x2::audio {

namespace {

constexpr size_t kCacheFrames = 8192;

// A pack file (pack::ByteSource: size and read at an offset) as the decoders' byte stream.
class PackBytes final : public ByteSource {
public:
    explicit PackBytes(std::unique_ptr<pack::ByteSource> in) : in_(std::move(in)) {}
    size_t read(void* dst, size_t n) override {
        const uint64_t left = pos_ < in_->size() ? in_->size() - pos_ : 0;
        const size_t got = in_->read(pos_, dst, size_t(std::min<uint64_t>(n, left)));
        pos_ += got;
        return got;
    }
    bool seek(uint64_t pos) override {
        if (pos > in_->size()) return false;
        pos_ = pos;
        return true;
    }
    uint64_t tell() const override { return pos_; }
    uint64_t size() const override { return in_->size(); }

private:
    std::unique_ptr<pack::ByteSource> in_;
    uint64_t pos_ = 0;
};

std::unique_ptr<AudioDecoder> open_decoder(const pack::PackLibrary& lib, const pack::FileRef& ref, const DecodeLimits& limits,
                                           std::string& err) {
    std::string why;
    std::unique_ptr<pack::ByteSource> in = lib.open_file(ref, why);
    if (!in) {
        err = why.empty() ? "cannot open " + ref.path : why;
        return nullptr;
    }
    return AudioDecoder::open(std::make_unique<PackBytes>(std::move(in)), limits, &err);
}

// Interleaved channels of the decoder to stereo at the 16-bit scale.
void to_stereo(const float* in, size_t frames, int channels, float* out) {
    for (size_t i = 0; i < frames; ++i) {
        const float l = in[i * size_t(channels)] * 32768.0f;
        out[i * 2] = l;
        out[i * 2 + 1] = channels == 2 ? in[i * 2 + 1] * 32768.0f : l;
    }
}

// A decoder over a pack file as a stream: the first buffer comes from memory, the rest is decoded on demand into a small cache.
class DecoderStream final : public StreamSource {
public:
    DecoderStream(const pack::PackLibrary* lib, pack::FileRef ref, DecodeLimits limits, uint64_t frames, int channels, FrameBuffer head,
                  std::string name)
        : lib_(lib), ref_(std::move(ref)), limits_(limits), frames_(frames), channels_(channels), head_(std::move(head)),
          name_(std::move(name)) {}

    void read(int64_t first, size_t n, float* out) override {
        const int64_t head_frames = head_ ? int64_t(head_->size() / 2) : 0;
        for (size_t i = 0; i < n;) {
            const int64_t f = first + int64_t(i);
            if (f < 0 || uint64_t(f) >= frames_) {
                const size_t run = f < 0 ? size_t(std::min<int64_t>(int64_t(n - i), -f)) : n - i;
                std::fill(out + i * 2, out + (i + run) * 2, 0.0f);
                i += run;
                continue;
            }
            if (f < head_frames) {
                const size_t run = size_t(std::min<int64_t>(int64_t(n - i), head_frames - f));
                std::copy(head_->begin() + f * 2, head_->begin() + (f + int64_t(run)) * 2, out + i * 2);
                i += run;
                continue;
            }
            if (f < cache_first_ || f >= cache_first_ + int64_t(cache_frames_)) fill(f);
            const size_t at = size_t(f - cache_first_);
            const size_t run = std::min(n - i, cache_frames_ - at);
            std::copy(cache_.begin() + at * 2, cache_.begin() + (at + run) * 2, out + i * 2);
            i += run;
        }
    }

private:
    void fail(const std::string& why) {
        if (!failed_) std::fprintf(stderr, "x2recomp: music %s: %s; the rest of the track is silent\n", name_.c_str(), why.c_str());
        failed_ = true;
    }
    void fill(int64_t f) {
        cache_first_ = f;
        cache_frames_ = size_t(std::min<uint64_t>(kCacheFrames, frames_ - uint64_t(f)));
        cache_.assign(cache_frames_ * 2, 0.0f);
        if (failed_) return;
        std::string err;
        if (!dec_) {
            dec_ = open_decoder(*lib_, ref_, limits_, err);
            if (!dec_) { fail(err); return; }
            dec_pos_ = 0;
        }
        if (dec_pos_ != uint64_t(f)) {
            if (!dec_->seek(uint64_t(f))) { fail("cannot seek"); return; }
            dec_pos_ = uint64_t(f);
        }
        std::vector<float> raw(cache_frames_ * size_t(channels_));
        size_t got = 0;
        while (got < cache_frames_) {
            const size_t k = dec_->read(&raw[got * size_t(channels_)], cache_frames_ - got);
            if (k == 0) break;
            got += k;
        }
        if (dec_->failed()) fail(dec_->error());
        to_stereo(raw.data(), got, channels_, cache_.data());
        dec_pos_ += got;
    }

    const pack::PackLibrary* lib_;
    pack::FileRef ref_;
    DecodeLimits limits_;
    uint64_t frames_;
    int channels_;
    FrameBuffer head_;
    std::string name_;
    std::unique_ptr<AudioDecoder> dec_;
    uint64_t dec_pos_ = 0;
    std::vector<float> cache_;
    int64_t cache_first_ = 0;
    size_t cache_frames_ = 0;
    bool failed_ = false;
};

// What opening one file told us.
struct Probe {
    AudioInfo info;
    uint64_t frames = 0;
    FrameBuffer head;
};

bool probe(const pack::PackLibrary& lib, const pack::FileRef& ref, const DecodeLimits& limits, Probe& out, std::string& err) {
    std::unique_ptr<AudioDecoder> dec = open_decoder(lib, ref, limits, err);
    if (!dec) return false;
    out.info = dec->info();
    const int channels = out.info.channels;
    if (channels < 1 || channels > 2) {
        err = "only mono and stereo files are supported";
        return false;
    }
    const uint64_t known = out.info.frames >= 0 ? uint64_t(out.info.frames) : 0;
    const size_t want_head = size_t(out.info.frames >= 0 ? std::min<uint64_t>(MusicSlots::kHeadFrames, known) : MusicSlots::kHeadFrames);
    auto head = std::make_shared<std::vector<float>>(want_head * 2, 0.0f);
    std::vector<float> raw(4096 * size_t(channels));
    uint64_t total = 0;
    for (;;) {
        const size_t room = total < want_head ? std::min<size_t>(4096, want_head - size_t(total)) : 4096;
        const size_t got = dec->read(raw.data(), room);
        if (got == 0) break;
        if (total < want_head) to_stereo(raw.data(), std::min<size_t>(got, want_head - size_t(total)), channels, &(*head)[size_t(total) * 2]);
        total += got;
        if (out.info.frames >= 0 && total >= want_head) break; // the length is known: only the head is wanted
        if (total > limits.max_frames) {
            err = "longer than the decode limit";
            return false;
        }
    }
    if (dec->failed()) {
        err = dec->error();
        return false;
    }
    out.frames = out.info.frames >= 0 ? known : total;
    if (out.frames == 0) {
        err = "the file has no audio";
        return false;
    }
    head->resize(std::min<size_t>(want_head, size_t(out.frames)) * 2);
    out.head = std::move(head);
    return true;
}

uint64_t seconds_to_frames(double s, int rate) { return s <= 0.0 ? 0 : uint64_t(std::llround(s * double(rate))); }

} // namespace

std::vector<std::string> disk_slot_names(int slot) {
    if (slot <= 0) return {"title"};
    return {"level" + std::to_string(slot), "level"};
}

std::vector<std::string> cdtv_slot_names(CdtvRole role, int level, int track) {
    std::vector<std::string> names;
    if (track >= 2 && track <= 13) names.push_back("cd" + std::to_string(track));
    if (role == CdtvRole::Title) {
        names.push_back("title");
    } else if (role == CdtvRole::Level) {
        if (level >= 1 && level <= 5) names.push_back("level" + std::to_string(level));
        names.push_back("level");
    }
    return names;
}

MusicSlots::MusicSlots(const pack::PackLibrary& lib, DecodeLimits limits) : lib_(lib), limits_(limits) {}

pack::PackReport MusicSlots::take_report() {
    pack::PackReport r = std::move(report_);
    report_ = pack::PackReport();
    return r;
}

std::string MusicSlots::pack_name(const pack::FileRef& f) const {
    if (f.pack < 0 || size_t(f.pack) >= lib_.packs().size()) return "";
    const pack::PackInfo& p = lib_.packs()[size_t(f.pack)];
    return p.id.empty() ? p.name : p.id;
}

void MusicSlots::warn(const pack::MusicSlot& slot, const std::string& text) {
    report_.add(pack::Severity::Warning, pack_name(slot.file.valid() ? slot.file : slot.intro), "music", slot.slot, text);
}

const MusicSlots::Entry& MusicSlots::entry(const std::string& name, const pack::MusicSlot& slot) {
    auto it = entries_.find(name);
    if (it != entries_.end()) return it->second;
    Entry e;
    e.retrigger = slot.retrigger.value_or(lib_.music_defaults().retrigger);
    e.usable = build(slot, e);
    return entries_.emplace(name, std::move(e)).first->second;
}

bool MusicSlots::prepare(const std::string& name) {
    const pack::MusicSlot* slot = lib_.music_slot(name);
    return !slot || entry(name, *slot).usable;
}

MusicChoice MusicSlots::resolve(const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        const pack::MusicSlot* slot = lib_.music_slot(name);
        if (!slot) continue;
        const Entry& e = entry(name, *slot);
        if (!e.usable) continue;
        MusicChoice c;
        c.slot = name;
        c.retrigger = e.retrigger;
        c.kind = e.mute ? MusicChoice::Silent : MusicChoice::Track;
        c.spec = e.spec;
        return c;
    }
    return {};
}

bool MusicSlots::build(const pack::MusicSlot& slot, Entry& e) {
    if (slot.mute) { // silence wins over a file the author left in
        e.mute = true;
        return true;
    }
    if (!slot.file.valid()) {
        warn(slot, "no file and not muted: the slot is ignored");
        return false;
    }
    const pack::MusicDefaults& def = lib_.music_defaults();
    std::string err;

    Probe body;
    if (!probe(lib_, slot.file, limits_, body, err)) {
        warn(slot, "\"" + slot.file.path + "\": " + err + ": the slot is ignored");
        return false;
    }
    const int rate = body.info.sample_rate;

    // Optional intro: played once before the loop body. Same rate as the body (resampled here when it differs).
    uint64_t intro_frames = 0;
    std::function<std::unique_ptr<StreamSource>()> make_intro;
    if (slot.intro.valid()) {
        Probe in;
        if (!probe(lib_, slot.intro, limits_, in, err)) {
            warn(slot, "intro \"" + slot.intro.path + "\": " + err + ": the slot is ignored");
            return false;
        }
        if (in.info.sample_rate == rate) {
            intro_frames = in.frames;
            const pack::PackLibrary* lib = &lib_;
            const pack::FileRef ref = slot.intro;
            const DecodeLimits lim = limits_;
            const int ch = in.info.channels;
            const FrameBuffer head = in.head;
            const uint64_t n = in.frames;
            make_intro = [=]() { return std::make_unique<DecoderStream>(lib, ref, lim, n, ch, head, ref.path); };
        } else {
            std::string why;
            std::unique_ptr<pack::ByteSource> src = lib_.open_file(slot.intro, why);
            FloatClip clip;
            if (!src || !decode_all_f32(std::make_unique<PackBytes>(std::move(src)), limits_, OutputFormat{2, rate}, clip, &err)) {
                warn(slot, "intro \"" + slot.intro.path + "\": " + (src ? err : why) + ": the slot is ignored");
                return false;
            }
            auto buf = std::make_shared<std::vector<float>>(clip.frames * 2);
            for (size_t i = 0; i < clip.frames * 2; ++i) (*buf)[i] = clip.pcm[i] * 32768.0f;
            intro_frames = clip.frames;
            make_intro = [buf]() { return std::make_unique<BufferStream>(buf); };
            e.note += "intro resampled from " + std::to_string(in.info.sample_rate) + " Hz; ";
        }
    }

    auto spec = std::make_shared<StreamSpec>();
    spec->rate = rate;
    spec->frames = intro_frames + body.frames;
    spec->name = slot.slot;
    {
        const pack::PackLibrary* lib = &lib_;
        const pack::FileRef ref = slot.file;
        const DecodeLimits lim = limits_;
        const int ch = body.info.channels;
        const FrameBuffer head = body.head;
        const uint64_t n = body.frames;
        auto make_body = [=]() { return std::make_unique<DecoderStream>(lib, ref, lim, n, ch, head, ref.path); };
        if (make_intro) {
            auto mi = make_intro;
            const uint64_t ni = intro_frames;
            spec->open = [mi, make_body, ni]() -> std::unique_ptr<StreamSource> {
                return std::make_unique<ConcatStream>(mi(), ni, make_body());
            };
        } else {
            spec->open = [make_body]() -> std::unique_ptr<StreamSource> { return make_body(); };
        }
    }

    // The loop: the manifest says, else the file does, else the game's own rule (the original always repeats its tune).
    const pack::LoopMode mode = slot.loop_mode.value_or(def.loop_mode);
    bool seamless = false;
    uint64_t ls = 0, le = body.frames;
    switch (slot.loop.kind) {
    case pack::LoopKind::Off:
        spec->loop = LoopRule::Off;
        e.note += "no loop";
        break;
    case pack::LoopKind::Whole:
        seamless = true;
        e.note += "whole file";
        break;
    case pack::LoopKind::Points: {
        ls = seconds_to_frames(slot.loop.start_s, rate);
        le = slot.loop.end_s < 0 ? body.frames : std::min<uint64_t>(body.frames, seconds_to_frames(slot.loop.end_s, rate));
        if (slot.loop.end_s >= 0 && seconds_to_frames(slot.loop.end_s, rate) > body.frames)
            warn(slot, "loop end is past the end of the file: the loop ends at the file's end");
        if (ls >= le) {
            warn(slot, "loop start is not before the loop end: looping the whole file");
            ls = 0;
            le = body.frames;
        }
        seamless = true;
        e.note += "manifest points";
        break;
    }
    case pack::LoopKind::Default:
        if (body.info.loop) {
            ls = body.info.loop->start;
            le = body.info.loop->end;
            seamless = true;
            e.note += "file loop (" + body.info.loop_note + ")";
        } else {
            if (!body.info.loop_note.empty()) warn(slot, "file loop ignored: " + body.info.loop_note);
            e.note += "none declared: the game's restart rule";
        }
        break;
    }
    if (slot.loop.kind != pack::LoopKind::Off) {
        if (seamless && mode == pack::LoopMode::Pack) {
            spec->loop = LoopRule::Seamless;
            spec->loop_start = intro_frames + ls;
            spec->loop_end = intro_frames + le;
        } else {
            spec->loop = LoopRule::Game;
        }
    }

    spec->gain = std::pow(10.0, double(slot.gain_db) / 20.0);
    spec->crossfade_ms = std::clamp(slot.crossfade_ms.value_or(def.crossfade_ms), 0, 5000);
    spec->fade_in_ms = std::clamp(slot.fade_in_ms.value_or(def.fade_in_ms), 0, 10000);
    spec->fade_out_ms = std::clamp(slot.fade_out_ms.value_or(def.fade_out_ms), 0, 10000);
    e.spec = std::move(spec);
    return true;
}

} // namespace x2::audio
