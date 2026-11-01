// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Audio decoding for asset packs: see decode.h. WAV is parsed here; FLAC and Ogg Vorbis go through the vendored dr_flac and
// stb_vorbis (vendor/audio/README.md), behind byte-source callbacks and allocation budgets so that a hostile file cannot make
// them allocate more than the caller allowed.
//
// The resampler's float arithmetic must round the same way in every build (pack audio hashes are exact): no fused multiply-add.
// CMake also passes -ffp-contract=off (GCC, Clang) or /fp:strict (MSVC) for this file; the pragma covers Clang on its own.
#ifndef _MSC_VER
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma STDC FP_CONTRACT OFF
#endif

#include "audio/decode.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "dr_flac.h"
extern "C" {
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "stb_vorbis.c"
}

namespace x2::audio {

namespace {

constexpr size_t kBlockFrames = 4096;

class MemorySource final : public ByteSource {
public:
    MemorySource(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    explicit MemorySource(std::vector<uint8_t>&& v) : owned_(std::move(v)), d_(owned_.data()), n_(owned_.size()) {}
    size_t read(void* dst, size_t n) override {
        const size_t k = std::min(n, n_ - pos_);
        if (k) std::memcpy(dst, d_ + pos_, k);
        pos_ += k;
        return k;
    }
    bool seek(uint64_t pos) override {
        if (pos > n_) return false;
        pos_ = size_t(pos);
        return true;
    }
    uint64_t tell() const override { return pos_; }
    uint64_t size() const override { return n_; }
    const uint8_t* data() const override { return d_; }

private:
    std::vector<uint8_t> owned_; // empty for a view; declared first: d_ and n_ are initialised from it
    const uint8_t* d_;
    size_t n_;
    size_t pos_ = 0;
};

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// Loop description gathered from tags; turned into LoopPoints once the length is known.
struct LoopTags {
    std::optional<uint64_t> start, length, end;
    bool any() const { return start || length || end; }
};

bool iequals_prefix(const char* s, size_t len, const char* name, size_t name_len) {
    if (len <= name_len || s[name_len] != '=') return false;
    for (size_t i = 0; i < name_len; ++i)
        if (std::toupper(static_cast<unsigned char>(s[i])) != name[i]) return false;
    return true;
}

bool parse_u64(const char* s, size_t len, uint64_t& v) {
    if (len == 0 || len > 18) return false;
    uint64_t r = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        r = r * 10 + uint64_t(s[i] - '0');
    }
    v = r;
    return true;
}

void note_tag(LoopTags& t, const char* s, size_t len) {
    uint64_t v;
    if (iequals_prefix(s, len, "LOOPSTART", 9)) { if (parse_u64(s + 10, len - 10, v)) t.start = v; }
    else if (iequals_prefix(s, len, "LOOPLENGTH", 10)) { if (parse_u64(s + 11, len - 11, v)) t.length = v; }
    else if (iequals_prefix(s, len, "LOOPEND", 7)) { if (parse_u64(s + 8, len - 8, v)) t.end = v; }
}

// LOOPLENGTH wins over LOOPEND (end exclusive); a missing start is 0 and a missing end is the end of the file.
void apply_tags(AudioInfo& info, const LoopTags& t) {
    if (!t.any()) return;
    const uint64_t start = t.start.value_or(0);
    uint64_t end = 0;
    if (t.length) end = start + *t.length;
    else if (t.end) end = *t.end;
    else if (info.frames > 0) end = uint64_t(info.frames);
    if (end == 0 || end <= start || (info.frames >= 0 && end > uint64_t(info.frames))) {
        info.loop_note = "loop tags ignored: the loop is empty or outside the file";
        return;
    }
    info.loop = LoopPoints{start, end};
    info.loop_note = "tags";
}

// Counts allocations of the vendored decoders against a byte budget.
struct CountingAllocator {
    size_t limit = 0;
    size_t used = 0;
    bool exceeded = false;
    static constexpr size_t kHeader = 16;

    static void* on_malloc(size_t sz, void* u) {
        auto* a = static_cast<CountingAllocator*>(u);
        if (sz > a->limit || a->used > a->limit - sz) { a->exceeded = true; return nullptr; }
        auto* p = static_cast<uint8_t*>(std::malloc(sz + kHeader));
        if (!p) return nullptr;
        std::memcpy(p, &sz, sizeof sz);
        a->used += sz;
        return p + kHeader;
    }
    static void on_free(void* p, void* u) {
        if (!p) return;
        auto* a = static_cast<CountingAllocator*>(u);
        auto* raw = static_cast<uint8_t*>(p) - kHeader;
        size_t sz;
        std::memcpy(&sz, raw, sizeof sz);
        a->used -= std::min(a->used, sz);
        std::free(raw);
    }
    static void* on_realloc(void* p, size_t sz, void* u) {
        if (!p) return on_malloc(sz, u);
        auto* a = static_cast<CountingAllocator*>(u);
        size_t old;
        std::memcpy(&old, static_cast<uint8_t*>(p) - kHeader, sizeof old);
        void* q = on_malloc(sz, u);
        if (!q) return nullptr;
        std::memcpy(q, p, std::min(old, sz));
        on_free(p, a);
        return q;
    }
};

bool check_stream(AudioInfo& info, const DecodeLimits& lim, std::string& err) {
    if (info.channels < 1) { err = "the file has no audio channels"; return false; }
    if (info.channels > lim.max_channels) {
        err = std::to_string(info.channels) + " channels: only mono and stereo are supported";
        return false;
    }
    if (info.sample_rate < lim.min_sample_rate || info.sample_rate > lim.max_sample_rate) {
        err = "sample rate " + std::to_string(info.sample_rate) + " Hz is outside " + std::to_string(lim.min_sample_rate) + "-" +
              std::to_string(lim.max_sample_rate);
        return false;
    }
    if (info.frames >= 0 && uint64_t(info.frames) > lim.max_frames) {
        err = "the file is longer than the limit of " + std::to_string(lim.max_frames) + " frames";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// WAV

class WavDecoder final : public AudioDecoder {
public:
    static std::unique_ptr<AudioDecoder> open(std::unique_ptr<ByteSource> src, const DecodeLimits& lim, std::string* error) {
        auto d = std::unique_ptr<WavDecoder>(new WavDecoder());
        d->src_ = std::move(src);
        std::string err;
        if (!d->parse(lim, err)) {
            if (error) *error = "WAV: " + err;
            return nullptr;
        }
        return d;
    }

    size_t read(float* out, size_t max_frames) override {
        if (failed() || remaining_ == 0) return 0;
        size_t done = 0;
        uint8_t raw[kBlockFrames * 2 * 8];
        const size_t per = size_t(block_align_);
        while (done < max_frames && remaining_ > 0) {
            const size_t n = size_t(std::min<uint64_t>({max_frames - done, remaining_, sizeof raw / per}));
            if (src_->read(raw, n * per) != n * per) {
                error_ = "WAV: the data chunk is shorter than its header says";
                remaining_ = 0;
                break;
            }
            convert(raw, n * size_t(info_.channels), out + done * size_t(info_.channels));
            done += n;
            remaining_ -= n;
        }
        position_ += done;
        return done;
    }

    bool seek(uint64_t frame) override {
        if (failed()) return false;
        if (frame > uint64_t(info_.frames) || !src_->seek(data_pos_ + frame * block_align_)) return false;
        remaining_ = uint64_t(info_.frames) - frame;
        position_ = 0;
        return true;
    }

private:
    WavDecoder() = default;

    bool parse(const DecodeLimits& lim, std::string& err) {
        const uint64_t file = src_->size();
        uint8_t h[12];
        if (!src_->seek(0) || src_->read(h, 12) != 12) { err = "too short"; return false; }
        if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) { err = "not a RIFF/WAVE file"; return false; }
        bool have_fmt = false, have_data = false;
        int tag = 0, bits = 0;
        uint64_t pos = 12, data_size = 0;
        LoopPoints smpl{};
        bool have_smpl = false;
        for (int chunks = 0; pos + 8 <= file && chunks < 1024; ++chunks) {
            uint8_t ch[8];
            if (!src_->seek(pos) || src_->read(ch, 8) != 8) break;
            const uint64_t size = le32(ch + 4);
            const uint64_t body = pos + 8;
            const bool is_data = std::memcmp(ch, "data", 4) == 0;
            // A streamed file may carry 0 or 0xFFFFFFFF for the data size: it then runs to the end of the file.
            const uint64_t eff = is_data && (size == 0xFFFFFFFFu || size == 0) ? file - body : size;
            if (body + eff > file) {
                if (is_data) { err = "the data chunk is truncated (declares " + std::to_string(size) + " bytes, " + std::to_string(file - body) + " present)"; return false; }
                if (have_fmt && have_data) break; // trailing damage after the audio is ignored
                err = "a chunk extends past the end of the file";
                return false;
            }
            if (std::memcmp(ch, "fmt ", 4) == 0) {
                if (have_fmt) { err = "two fmt chunks"; return false; }
                if (size < 16 || size > 4096) { err = "bad fmt chunk size"; return false; }
                uint8_t f[4096];
                if (src_->read(f, size_t(size)) != size) { err = "truncated fmt chunk"; return false; }
                tag = le16(f);
                info_.channels = le16(f + 2);
                info_.sample_rate = int(std::min<uint32_t>(le32(f + 4), 0x7FFFFFFFu));
                block_align_ = le16(f + 12);
                bits = le16(f + 14);
                if (tag == 0xFFFE) {
                    static const uint8_t kGuid[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                    if (size < 40 || le16(f + 16) < 22) { err = "bad WAVE_FORMAT_EXTENSIBLE chunk"; return false; }
                    if (std::memcmp(f + 26, kGuid, 14) != 0) { err = "unsupported extensible sub-format"; return false; }
                    tag = le16(f + 24);
                }
                if (tag != 1 && tag != 3) { err = "format tag " + std::to_string(tag) + " is not PCM or IEEE float"; return false; }
                have_fmt = true;
            } else if (is_data) {
                if (!have_data) {
                    have_data = true;
                    data_pos_ = body;
                    data_size = eff;
                }
            } else if (std::memcmp(ch, "smpl", 4) == 0 && size >= 36 && size <= 65536 && !have_smpl) {
                uint8_t s[65536];
                if (src_->read(s, size_t(size)) == size) {
                    const uint32_t loops = le32(s + 28);
                    if (loops >= 1 && size >= 36 + 24 && le32(s + 36 + 4) == 0) { // the first loop, forward type
                        smpl.start = le32(s + 36 + 8);
                        smpl.end = uint64_t(le32(s + 36 + 12)) + 1; // the chunk's end is the last frame played
                        have_smpl = true;
                    }
                }
            }
            if (body + eff > UINT64_MAX - 1) break;
            pos = body + eff + (eff & 1);
        }
        if (!have_fmt) { err = "no fmt chunk"; return false; }
        if (!have_data) { err = "no data chunk"; return false; }
        if (info_.channels < 1 || info_.channels > 2) {
            if (info_.channels > 2) err = std::to_string(info_.channels) + " channels: only mono and stereo are supported";
            else err = "no audio channels";
            return false;
        }
        const bool is_float = tag == 3;
        if (is_float ? (bits != 32 && bits != 64) : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
            err = std::to_string(bits) + "-bit " + (is_float ? "float" : "PCM") + " is not supported";
            return false;
        }
        bytes_ = bits / 8;
        if (block_align_ != info_.channels * bytes_) { err = "block alignment does not match the channels and bits"; return false; }
        info_.format = Format::Wav;
        info_.bits = bits;
        float_ = is_float;
        info_.frames = int64_t(data_size / block_align_); // a partial last frame is dropped
        if (!check_stream(info_, lim, err)) return false;
        if (info_.frames == 0) { err = "the data chunk holds no audio"; return false; }
        if (have_smpl) {
            if (smpl.end > smpl.start && smpl.end <= uint64_t(info_.frames)) {
                info_.loop = smpl;
                info_.loop_note = "smpl chunk";
            } else {
                info_.loop_note = "smpl loop ignored: it is empty or outside the file";
            }
        }
        if (!src_->seek(data_pos_)) { err = "cannot seek to the audio"; return false; }
        remaining_ = uint64_t(info_.frames);
        return true;
    }

    void convert(const uint8_t* in, size_t samples, float* out) const {
        for (size_t i = 0; i < samples; ++i, in += bytes_) {
            float v;
            if (float_) {
                if (bytes_ == 4) { uint32_t u = le32(in); float f; std::memcpy(&f, &u, 4); v = f; }
                else { uint64_t u = uint64_t(le32(in)) | uint64_t(le32(in + 4)) << 32; double f; std::memcpy(&f, &u, 8); v = float(f); }
                // NaN and infinity are silence; a huge finite value is capped so the resampler's sums cannot overflow to infinity
                if (!std::isfinite(v)) v = 0.0f;
                else v = std::min(1e30f, std::max(-1e30f, v));
            } else if (bytes_ == 1) {
                v = float(int(in[0]) - 128) * (1.0f / 128.0f);
            } else if (bytes_ == 2) {
                v = float(int16_t(le16(in))) * (1.0f / 32768.0f);
            } else if (bytes_ == 3) {
                int32_t s = int32_t((uint32_t(in[0]) << 8 | uint32_t(in[1]) << 16 | uint32_t(in[2]) << 24)) >> 8;
                v = float(s) * (1.0f / 8388608.0f);
            } else {
                v = float(int32_t(le32(in))) * (1.0f / 2147483648.0f);
            }
            out[i] = v;
        }
    }

    std::unique_ptr<ByteSource> src_;
    uint64_t data_pos_ = 0, remaining_ = 0;
    int block_align_ = 0, bytes_ = 0;
    bool float_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------
// FLAC (dr_flac)

class FlacDecoder final : public AudioDecoder {
public:
    static std::unique_ptr<AudioDecoder> open(std::unique_ptr<ByteSource> src, const DecodeLimits& lim, std::string* error) {
        auto d = std::unique_ptr<FlacDecoder>(new FlacDecoder());
        d->src_ = std::move(src);
        d->alloc_.limit = lim.budget;
        std::string err;
        if (!d->start(lim, err)) {
            if (error) *error = "FLAC: " + err;
            return nullptr;
        }
        return d;
    }
    ~FlacDecoder() override {
        if (f_) drflac_close(f_);
    }

    size_t read(float* out, size_t max_frames) override {
        if (failed() || !f_) return 0;
        if (info_.frames >= 0) {
            if (position_ >= uint64_t(info_.frames)) return 0;
            max_frames = size_t(std::min<uint64_t>(max_frames, uint64_t(info_.frames) - position_));
        }
        const size_t n = size_t(drflac_read_pcm_frames_f32(f_, max_frames, out));
        position_ += n;
        return n;
    }
    bool seek(uint64_t frame) override {
        if (failed() || !f_) return false;
        if (info_.frames >= 0 && frame > uint64_t(info_.frames)) return false;
        if (!drflac_seek_to_pcm_frame(f_, frame)) return false;
        position_ = 0;
        return true;
    }

private:
    FlacDecoder() = default;

    static size_t on_read(void* u, void* buf, size_t n) { return static_cast<FlacDecoder*>(u)->src_->read(buf, n); }
    static drflac_bool32 on_seek(void* u, int offset, drflac_seek_origin origin) {
        ByteSource& s = *static_cast<FlacDecoder*>(u)->src_;
        int64_t target;
        switch (origin) {
            case DRFLAC_SEEK_SET: target = offset; break;
            case DRFLAC_SEEK_CUR: target = int64_t(s.tell()) + offset; break;
            default: target = int64_t(s.size()) - offset; break;
        }
        if (target < 0 || uint64_t(target) > s.size()) return DRFLAC_FALSE;
        return s.seek(uint64_t(target)) ? DRFLAC_TRUE : DRFLAC_FALSE;
    }
    static drflac_bool32 on_tell(void* u, drflac_int64* cursor) {
        *cursor = drflac_int64(static_cast<FlacDecoder*>(u)->src_->tell());
        return DRFLAC_TRUE;
    }
    static void on_meta(void* u, drflac_metadata* m) {
        auto* d = static_cast<FlacDecoder*>(u);
        if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
        drflac_vorbis_comment_iterator it;
        drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
        drflac_uint32 len = 0;
        for (const char* c = drflac_next_vorbis_comment(&it, &len); c; c = drflac_next_vorbis_comment(&it, &len)) note_tag(d->tags_, c, len);
    }

    bool start(const DecodeLimits& lim, std::string& err) {
        if (src_->size() > lim.max_file_bytes) { err = "the file is larger than the limit"; return false; }
        if (!src_->seek(0)) { err = "cannot read"; return false; }
        drflac_allocation_callbacks cb;
        cb.pUserData = &alloc_;
        cb.onMalloc = &CountingAllocator::on_malloc;
        cb.onRealloc = &CountingAllocator::on_realloc;
        cb.onFree = &CountingAllocator::on_free;
        f_ = drflac_open_with_metadata(&on_read, &on_seek, &on_tell, &on_meta, this, &cb);
        if (!f_) {
            err = alloc_.exceeded ? "the decoder needs more memory than the limit allows" : "damaged or unsupported file";
            return false;
        }
        if (f_->container != drflac_container_native) { err = "Ogg-wrapped FLAC is not supported"; return false; }
        info_.format = Format::Flac;
        info_.channels = f_->channels;
        info_.sample_rate = int(std::min<drflac_uint32>(f_->sampleRate, 0x7FFFFFFFu));
        info_.bits = f_->bitsPerSample;
        info_.frames = f_->totalPCMFrameCount ? int64_t(std::min<drflac_uint64>(f_->totalPCMFrameCount, INT64_MAX)) : -1;
        if (!check_stream(info_, lim, err)) return false;
        apply_tags(info_, tags_);
        return true;
    }

    std::unique_ptr<ByteSource> src_;
    CountingAllocator alloc_;
    drflac* f_ = nullptr;
    LoopTags tags_;
};

// ---------------------------------------------------------------------------------------------------------------------
// Ogg Vorbis (stb_vorbis). The pull API needs the whole file in memory: a memory source is used in place, any other source is
// read into a buffer first (counted against the budget). All of stb_vorbis's own allocations come from one caller-sized block.

const char* vorbis_error_text(int e) {
    switch (e) {
        case VORBIS_need_more_data: return "the stream ends early";
        case VORBIS_invalid_api_mixing: return "internal error";
        case VORBIS_outofmem: return "the decoder needs more memory than the limit allows";
        case VORBIS_feature_not_supported: return "an unsupported Vorbis feature";
        case VORBIS_too_many_channels: return "too many channels";
        case VORBIS_file_open_failure: return "cannot read";
        case VORBIS_seek_without_length: return "cannot seek";
        case VORBIS_unexpected_eof: return "the file ends early";
        case VORBIS_seek_invalid: return "bad seek";
        case VORBIS_invalid_setup: return "bad Vorbis setup header";
        case VORBIS_invalid_stream: return "damaged stream";
        case VORBIS_missing_capture_pattern:
        case VORBIS_invalid_stream_structure_version:
        case VORBIS_continued_packet_flag_invalid:
        case VORBIS_incorrect_stream_serial_number:
        case VORBIS_invalid_first_page:
        case VORBIS_bad_packet_type:
        case VORBIS_cant_find_last_page:
        case VORBIS_seek_failed: return "damaged Ogg stream";
        default: return "damaged or unsupported file";
    }
}

class VorbisDecoder final : public AudioDecoder {
public:
    static std::unique_ptr<AudioDecoder> open(std::unique_ptr<ByteSource> src, const DecodeLimits& lim, std::string* error) {
        auto d = std::unique_ptr<VorbisDecoder>(new VorbisDecoder());
        d->src_ = std::move(src);
        std::string err;
        if (!d->start(lim, err)) {
            if (error) *error = "Ogg Vorbis: " + err;
            return nullptr;
        }
        return d;
    }
    ~VorbisDecoder() override {
        if (v_) stb_vorbis_close(v_);
    }

    size_t read(float* out, size_t max_frames) override {
        if (failed() || !v_) return 0;
        if (info_.frames >= 0) {
            if (position_ >= uint64_t(info_.frames)) return 0;
            max_frames = size_t(std::min<uint64_t>(max_frames, uint64_t(info_.frames) - position_));
        }
        const int ch = info_.channels;
        max_frames = std::min<size_t>(max_frames, size_t(INT_MAX / 2) / size_t(ch));
        const int n = stb_vorbis_get_samples_float_interleaved(v_, ch, out, int(max_frames) * ch);
        if (n <= 0) {
            const int e = stb_vorbis_get_error(v_);
            if (e != VORBIS__no_error && e != VORBIS_need_more_data && e != VORBIS_unexpected_eof)
                error_ = std::string("Ogg Vorbis: ") + vorbis_error_text(e);
            return 0;
        }
        // A damaged codebook can yield NaN or enormous values: the same policy as float WAV (silence, or capped)
        for (size_t i = 0, total = size_t(n) * size_t(ch); i < total; ++i) {
            const float v = out[i];
            out[i] = std::isfinite(v) ? std::min(1e30f, std::max(-1e30f, v)) : 0.0f;
        }
        position_ += size_t(n);
        return size_t(n);
    }
    bool seek(uint64_t frame) override {
        if (failed() || !v_) return false;
        if (info_.frames >= 0 && frame > uint64_t(info_.frames)) return false;
        if (frame > UINT_MAX || !stb_vorbis_seek(v_, unsigned(frame))) return false;
        position_ = 0;
        return true;
    }

private:
    VorbisDecoder() = default;

    bool start(const DecodeLimits& lim, std::string& err) {
        const uint64_t size = src_->size();
        if (size > lim.max_file_bytes || size > uint64_t(INT_MAX)) { err = "the file is larger than the limit"; return false; }
        if (size < 64) { err = "too short"; return false; }
        const uint8_t* mem = src_->data();
        size_t budget = lim.budget;
        if (!mem) {
            if (size > budget) { err = "the file is larger than the memory limit"; return false; }
            owned_.resize(size_t(size));
            if (!src_->seek(0) || src_->read(owned_.data(), owned_.size()) != owned_.size()) { err = "cannot read"; return false; }
            mem = owned_.data();
            budget -= owned_.size();
        }
        // stb_vorbis's setup memory (codebooks, floors, windows) is a few hundred KB to a few MB: try small first.
        static const size_t kTries[] = {size_t(2) << 20, size_t(8) << 20, size_t(32) << 20};
        int e = 0;
        for (size_t want : kTries) {
            const size_t sz = std::min(want, budget);
            if (sz < 64 * 1024) break;
            arena_.assign(sz, 0);
            stb_vorbis_alloc a;
            a.alloc_buffer = reinterpret_cast<char*>(arena_.data());
            a.alloc_buffer_length_in_bytes = int(std::min<size_t>(sz, size_t(INT_MAX)));
            e = 0;
            v_ = stb_vorbis_open_memory(mem, int(size), &e, &a);
            if (v_ || e != VORBIS_outofmem || sz == budget) break;
        }
        if (!v_) {
            err = e == 0 ? "the decoder needs more memory than the limit allows" : vorbis_error_text(e);
            return false;
        }
        const stb_vorbis_info vi = stb_vorbis_get_info(v_);
        info_.format = Format::Vorbis;
        info_.channels = vi.channels;
        info_.sample_rate = int(std::min<unsigned>(vi.sample_rate, 0x7FFFFFFFu));
        info_.bits = 0;
        const unsigned len = stb_vorbis_stream_length_in_samples(v_);
        info_.frames = len ? int64_t(len) : -1;
        if (!check_stream(info_, lim, err)) return false;
        // stream_length scans the last page and restores the position; make sure decoding starts at the beginning.
        if (!stb_vorbis_seek_start(v_)) { err = "cannot rewind"; return false; }
        const stb_vorbis_comment c = stb_vorbis_get_comment(v_);
        LoopTags tags;
        for (int i = 0; i < c.comment_list_length; ++i)
            if (c.comment_list[i]) note_tag(tags, c.comment_list[i], std::strlen(c.comment_list[i]));
        apply_tags(info_, tags);
        return true;
    }

    std::unique_ptr<ByteSource> src_;
    std::vector<uint8_t> owned_;
    std::vector<uint8_t> arena_;
    stb_vorbis* v_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------------------------

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64 && term > 1e-14 * sum; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
    }
    return sum;
}

} // namespace

std::unique_ptr<ByteSource> memory_source(const uint8_t* data, size_t size) { return std::make_unique<MemorySource>(data, size); }
std::unique_ptr<ByteSource> memory_source(std::vector<uint8_t>&& data) { return std::make_unique<MemorySource>(std::move(data)); }

std::unique_ptr<AudioDecoder> AudioDecoder::open(std::unique_ptr<ByteSource> source, const DecodeLimits& limits, std::string* error) {
    auto fail = [&](const std::string& m) -> std::unique_ptr<AudioDecoder> {
        if (error) *error = m;
        return nullptr;
    };
    if (!source) return fail("no data");
    if (limits.max_channels < 1 || limits.max_channels > 2) return fail("max_channels must be 1 or 2");
    const uint64_t size = source->size();
    if (size > limits.max_file_bytes) return fail("the file is larger than the limit of " + std::to_string(limits.max_file_bytes) + " bytes");
    uint8_t m[12] = {};
    if (!source->seek(0)) return fail("cannot read");
    const size_t got = source->read(m, sizeof m);
    if (got < 4) return fail("the file is too short to be audio");
    if (!source->seek(0)) return fail("cannot read");
    if (got >= 12 && std::memcmp(m, "RIFF", 4) == 0 && std::memcmp(m + 8, "WAVE", 4) == 0) return WavDecoder::open(std::move(source), limits, error);
    if (std::memcmp(m, "RF64", 4) == 0 || std::memcmp(m, "BW64", 4) == 0) return fail("RF64 WAV files are not supported");
    if (std::memcmp(m, "fLaC", 4) == 0) return FlacDecoder::open(std::move(source), limits, error);
    if (std::memcmp(m, "OggS", 4) == 0) return VorbisDecoder::open(std::move(source), limits, error);
    if (std::memcmp(m, "ID3", 3) == 0) return fail("a file with an ID3 tag (MP3?) is not supported: use WAV, FLAC or Ogg Vorbis");
    return fail("unknown audio format: use WAV, FLAC or Ogg Vorbis");
}

int16_t float_to_s16(float v) {
    if (!(v == v)) return 0; // NaN
    const float s = v * 32768.0f;
    if (s >= 32767.0f) return 32767;
    if (s <= -32768.0f) return -32768;
    return int16_t(std::floor(s + 0.5f));
}

void convert_channels(const float* in, size_t frames, int in_channels, int out_channels, std::vector<float>& out) {
    out.clear();
    if (in_channels == out_channels && (in_channels == 1 || in_channels == 2)) {
        out.assign(in, in + frames * size_t(in_channels));
    } else if (in_channels == 1 && out_channels == 2) {
        out.resize(frames * 2);
        for (size_t i = 0; i < frames; ++i) out[2 * i] = out[2 * i + 1] = in[i];
    } else if (in_channels == 2 && out_channels == 1) {
        out.resize(frames);
        for (size_t i = 0; i < frames; ++i) out[i] = (in[2 * i] + in[2 * i + 1]) * 0.5f;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Resampler

uint64_t StreamResampler::output_frames(uint64_t in_frames, int in_rate, int out_rate) {
    if (in_rate <= 0 || out_rate <= 0) return 0;
    if (in_frames > (UINT64_MAX - uint64_t(in_rate)) / uint64_t(out_rate)) return UINT64_MAX; // not reachable within the frame limit
    return (in_frames * uint64_t(out_rate) + uint64_t(in_rate) - 1) / uint64_t(in_rate);
}

StreamResampler::StreamResampler(int channels, int in_rate, int out_rate)
    : channels_(std::clamp(channels, 1, 2)), in_rate_(std::max(in_rate, 1)), out_rate_(std::max(out_rate, 1)), identity_(in_rate == out_rate) {
    if (identity_) return;
    constexpr double kPi = 3.14159265358979323846;
    const double fc = 0.46 * std::min(1.0, double(out_rate_) / double(in_rate_)), half = kTaps / 2.0;
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
    base_ = -int64_t(kTaps / 2 - 1);
    buf_.assign(size_t(kTaps / 2 - 1) * size_t(channels_), 0.0f); // silence before the start
}

void StreamResampler::push(const float* in, size_t frames) {
    if (finished_ || frames == 0) return;
    buf_.insert(buf_.end(), in, in + frames * size_t(channels_));
    pushed_ += frames;
}

void StreamResampler::finish() {
    if (finished_) return;
    finished_ = true;
    total_out_ = identity_ ? pushed_ : output_frames(pushed_, in_rate_, out_rate_);
}

size_t StreamResampler::pull(float* out, size_t max_frames) {
    const size_t ch = size_t(channels_);
    if (identity_) {
        const size_t n = std::min(max_frames, buf_.size() / ch);
        std::copy(buf_.begin(), buf_.begin() + std::ptrdiff_t(n * ch), out);
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(n * ch));
        next_out_ += n;
        return n;
    }
    size_t produced = 0;
    const int64_t nbuf = int64_t(buf_.size() / ch);
    while (produced < max_frames) {
        if (finished_ && next_out_ >= total_out_) break;
        const uint64_t num = next_out_ * uint64_t(in_rate_);
        const int64_t i0 = int64_t(num / uint64_t(out_rate_));
        const uint64_t rem = num % uint64_t(out_rate_);
        if (!finished_ && i0 + kTaps / 2 >= base_ + nbuf) break; // needs input that has not arrived
        const uint64_t x = rem * uint64_t(kPhases);
        const size_t ph = size_t(x / uint64_t(out_rate_));
        const float w = float(x % uint64_t(out_rate_)) / float(out_rate_);
        const float* k0 = &kernel_[ph * kTaps];
        const float* k1 = &kernel_[(ph + 1) * kTaps];
        double acc[2] = {0.0, 0.0};
        for (int j = 0; j < kTaps; ++j) {
            const int64_t idx = i0 + j - (kTaps / 2 - 1) - base_;
            if (idx < 0 || idx >= nbuf) continue; // before the start or after the end: silence
            const float c = k0[j] + (k1[j] - k0[j]) * w;
            for (size_t cc = 0; cc < ch; ++cc) acc[cc] += double(buf_[size_t(idx) * ch + cc]) * double(c);
        }
        for (size_t cc = 0; cc < ch; ++cc) out[produced * ch + cc] = float(acc[cc]);
        ++produced;
        ++next_out_;
    }
    // Drop input that no later output frame can reach.
    const int64_t keep_from = int64_t((next_out_ * uint64_t(in_rate_)) / uint64_t(out_rate_)) - (kTaps / 2 - 1);
    if (keep_from - base_ >= 8192) {
        const int64_t drop = keep_from - base_;
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(drop * int64_t(ch)));
        base_ += drop;
    }
    return produced;
}

// ---------------------------------------------------------------------------------------------------------------------
// Whole-file decoding

bool decode_all_f32(std::unique_ptr<ByteSource> source, const DecodeLimits& limits, const OutputFormat& want, FloatClip& out,
                    std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    out = FloatClip{};
    if ((want.channels != 0 && want.channels != 1 && want.channels != 2) ||
        (want.sample_rate != 0 && (want.sample_rate < 8000 || want.sample_rate > 192000)))
        return fail("bad output format");
    std::string err;
    auto dec = AudioDecoder::open(std::move(source), limits, &err);
    if (!dec) return fail(err);
    const AudioInfo info = dec->info();
    const size_t ch = size_t(info.channels);
    size_t left = limits.budget;
    // Decode at the file's own rate and channels first (the decoder's own memory is not counted again: it was bounded by the same budget).
    std::vector<float> src;
    std::vector<float> block(kBlockFrames * ch);
    uint64_t frames = 0, calls = 0;
    for (;;) {
        if (++calls > limits.max_calls) return fail("decoding gave up: too many read calls");
        const size_t n = dec->read(block.data(), kBlockFrames);
        if (n == 0) break;
        if (frames + n > limits.max_frames) return fail("the file is longer than the limit of " + std::to_string(limits.max_frames) + " frames");
        const size_t bytes = n * ch * sizeof(float);
        if (bytes > left) return fail("the decoded audio exceeds the memory limit of " + std::to_string(limits.budget) + " bytes");
        left -= bytes;
        src.insert(src.end(), block.begin(), block.begin() + std::ptrdiff_t(n * ch));
        frames += n;
    }
    if (dec->failed()) return fail(dec->error());
    if (frames == 0) return fail("the file holds no audio");
    if (info.frames >= 0 && frames < uint64_t(info.frames)) { // damaged or cut short (Vorbis lengths are a page granule: a little slack)
        const uint64_t slack = info.format == Format::Vorbis ? std::max<uint64_t>(2048, uint64_t(info.frames) / 200) : 0;
        if (uint64_t(info.frames) - frames > slack)
            return fail("the file ends early: " + std::to_string(frames) + " of " + std::to_string(info.frames) + " frames decoded");
    }
    dec.reset();

    const int out_ch = want.channels ? want.channels : info.channels;
    const int out_rate = want.sample_rate ? want.sample_rate : info.sample_rate;
    std::vector<float> chan;
    const float* cur = src.data();
    if (out_ch != info.channels) {
        const size_t bytes = size_t(frames) * size_t(out_ch) * sizeof(float);
        if (bytes > left) return fail("the converted audio exceeds the memory limit");
        left -= bytes;
        convert_channels(src.data(), size_t(frames), info.channels, out_ch, chan);
        src.clear();
        src.shrink_to_fit();
        cur = chan.data();
    }
    std::vector<float>& base = out_ch != info.channels ? chan : src;
    FloatClip res;
    res.channels = out_ch;
    res.sample_rate = out_rate;
    res.source = info;
    if (out_rate != info.sample_rate) {
        const uint64_t total = StreamResampler::output_frames(frames, info.sample_rate, out_rate);
        if (total > limits.max_frames) return fail("the resampled audio is longer than the limit");
        const uint64_t bytes = total * uint64_t(out_ch) * sizeof(float);
        if (bytes > left) return fail("the resampled audio exceeds the memory limit");
        left -= size_t(bytes);
        res.pcm.resize(size_t(total) * size_t(out_ch));
        StreamResampler rs(out_ch, info.sample_rate, out_rate);
        rs.push(cur, size_t(frames));
        rs.finish();
        size_t at = 0;
        while (at < total) {
            const size_t n = rs.pull(res.pcm.data() + at * size_t(out_ch), size_t(std::min<uint64_t>(total - at, kBlockFrames * 4)));
            if (n == 0) break;
            at += n;
        }
        res.pcm.resize(at * size_t(out_ch));
        res.frames = at;
    } else {
        res.pcm = std::move(base);
        res.frames = frames;
    }
    float peak = 0.0f;
    for (float v : res.pcm) peak = std::max(peak, std::fabs(v));
    res.peak = peak;
    out = std::move(res);
    return true;
}

bool decode_all(std::unique_ptr<ByteSource> source, const DecodeLimits& limits, const OutputFormat& want, DecodedClip& out,
                std::string* error) {
    out = DecodedClip{};
    FloatClip f;
    if (!decode_all_f32(std::move(source), limits, want, f, error)) return false;
    DecodedClip c;
    c.channels = f.channels;
    c.sample_rate = f.sample_rate;
    c.frames = f.frames;
    c.peak = f.peak;
    c.source = f.source;
    c.pcm.resize(f.pcm.size());
    for (size_t i = 0; i < f.pcm.size(); ++i) c.pcm[i] = float_to_s16(f.pcm[i]);
    out = std::move(c);
    return true;
}

} // namespace x2::audio
