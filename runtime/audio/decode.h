// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Audio decoding for asset packs (docs/re-notes/asset-packs-v2-design.md section 5.4, docs/re-notes/audio-decoders.md):
// WAV (our own parser), FLAC (vendor/audio/dr_flac.h) and Ogg Vorbis (vendor/audio/stb_vorbis.c) from a memory block or a
// caller-supplied byte stream, as interleaved float frames. Pack files are untrusted: every limit in DecodeLimits is checked
// before the memory it guards is allocated, and every malformed input returns an error message instead of crashing.
//
//   auto dec = AudioDecoder::open(memory_source(bytes, size), DecodeLimits{}, &err);   // streaming (music)
//   n = dec->read(out, 4096);                                                          // 0 = end (or failed(): see error())
//   decode_all(memory_source(bytes, size), limits, {2, 48000}, clip, &err);            // a whole clip, resampled (sfx, speech)
//
// Determinism: the same bytes, limits and host rate give the same samples on every platform (integer sample positions in the
// resampler, FMA contraction off in decode.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace x2::audio {

// A read-only byte stream with random access. A memory block is the common case; a ZIP member or a file can implement it.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Reads up to n bytes at the current position; fewer than n only at the end of the stream.
    virtual size_t read(void* dst, size_t n) = 0;
    // Moves to the absolute position `pos` (0..size()); false when pos > size() or the stream cannot seek.
    virtual bool seek(uint64_t pos) = 0;
    virtual uint64_t tell() const = 0;
    virtual uint64_t size() const = 0; // total length, always known
    // The whole stream as one contiguous block when it is one (a memory source), else null.
    virtual const uint8_t* data() const { return nullptr; }
};

// A view of `size` bytes at `data`: the caller keeps the memory alive for as long as the source (and any decoder made from it) is used.
std::unique_ptr<ByteSource> memory_source(const uint8_t* data, size_t size);
// The same, owning the bytes (a ZIP member that was just extracted, say): nothing to keep alive.
std::unique_ptr<ByteSource> memory_source(std::vector<uint8_t>&& data);

enum class Format { Wav, Flac, Vorbis };

// A loop, in frames of the file: play [start, end) and jump back to start (end is exclusive).
struct LoopPoints {
    uint64_t start = 0;
    uint64_t end = 0;
};

struct AudioInfo {
    Format format = Format::Wav;
    int channels = 0;
    int sample_rate = 0;
    int bits = 0;          // bits per sample in the file (WAV, FLAC); 0 for Vorbis
    int64_t frames = -1;   // length in frames, -1 when the file does not say
    std::optional<LoopPoints> loop; // from a WAV `smpl` chunk or the tags LOOPSTART / LOOPLENGTH / LOOPEND, validated
    std::string loop_note; // where `loop` came from ("smpl chunk", "tags"), or why a declared loop was ignored
};

struct DecodeLimits {
    size_t budget = size_t(64) << 20;      // bytes the decoder may allocate in total, and (decode_all) the decoded output as well
    uint64_t max_file_bytes = uint64_t(512) << 20;
    uint64_t max_frames = uint64_t(7200) * 192000; // at most two hours at the highest accepted rate
    int min_sample_rate = 8000;
    int max_sample_rate = 192000;
    int max_channels = 2;                  // 1 or 2; more is refused, not mixed down
    uint64_t max_calls = uint64_t(1) << 24; // decode_all: read calls before it gives up (a guard against decoders that crawl)
};

// A streaming decoder. Not thread safe. After an error read() returns 0 and failed() is true.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    // Identifies the format by its first bytes, reads the header and checks it against `limits`. Null with *error set on any problem
    // (unknown format, unsupported variant, a limit exceeded, a damaged header).
    static std::unique_ptr<AudioDecoder> open(std::unique_ptr<ByteSource> source, const DecodeLimits& limits, std::string* error);

    const AudioInfo& info() const { return info_; }
    // Up to max_frames interleaved float frames (info().channels per frame, nominally -1..1) into `out`. Returns the number
    // produced; 0 at the end of the stream or after a failure.
    virtual size_t read(float* out, size_t max_frames) = 0;
    // Moves to `frame` (0..frames); false when the format or the stream cannot. After a false return the position is undefined: seek again or reopen.
    virtual bool seek(uint64_t frame) = 0;
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }
    uint64_t position() const { return position_; } // frames delivered by read() since the last seek

protected:
    AudioInfo info_;
    std::string error_;
    uint64_t position_ = 0;
};

struct OutputFormat {
    int channels = 0;    // 0 keep the file's, else 1 or 2 (mono is duplicated to stereo; stereo is averaged to mono)
    int sample_rate = 0; // 0 keep the file's, else resample to this rate (8000..192000)
};

struct DecodedClip {
    int channels = 0;
    int sample_rate = 0;
    uint64_t frames = 0;
    std::vector<int16_t> pcm; // interleaved, frames * channels
    float peak = 0.0f;        // the largest absolute sample of the decoded signal before it was clamped to 16 bits (1.0 = full scale)
    AudioInfo source;         // what the file said
};

struct FloatClip {
    int channels = 0;
    int sample_rate = 0;
    uint64_t frames = 0;
    std::vector<float> pcm;
    float peak = 0.0f;
    AudioInfo source;
};

// Decodes a whole file. The decoder's memory, the decoded source samples and the converted output all count against
// limits.budget (4 bytes per float sample); max_frames bounds the frames of the file. false with *error set on failure.
bool decode_all_f32(std::unique_ptr<ByteSource> source, const DecodeLimits& limits, const OutputFormat& want, FloatClip& out,
                    std::string* error);
bool decode_all(std::unique_ptr<ByteSource> source, const DecodeLimits& limits, const OutputFormat& want, DecodedClip& out,
                std::string* error);

// Float to 16 bit as decode_all does it: round to nearest, saturate.
int16_t float_to_s16(float v);

// Interleaved channel conversion (1 <-> 2); other counts are not supported (the output is left empty).
void convert_channels(const float* in, size_t frames, int in_channels, int out_channels, std::vector<float>& out);

// Sample-rate conversion by a 64-tap Kaiser-windowed sinc (beta 8, cut-off 0.46 x the lower rate, 256 phases with linear
// interpolation between them), the same family as the CD player's. The position of output frame k is k * in_rate / out_rate
// computed in integers, so the result does not depend on how the input is chunked. Equal rates copy the input exactly.
class StreamResampler {
public:
    StreamResampler(int channels, int in_rate, int out_rate);
    void push(const float* in, size_t frames); // append input frames (interleaved)
    void finish();                             // no more input: the tail is flushed by pull(). Call once.
    // Writes up to max_frames output frames and returns the number written; fewer than asked means it needs more input (or, after
    // finish(), that the stream is done: done()).
    size_t pull(float* out, size_t max_frames);
    bool done() const { return finished_ && next_out_ >= total_out_; }
    // The number of output frames for `in_frames` input frames: ceil(in_frames * out_rate / in_rate).
    static uint64_t output_frames(uint64_t in_frames, int in_rate, int out_rate);

private:
    static constexpr int kTaps = 64;
    static constexpr int kPhases = 256;
    int channels_, in_rate_, out_rate_;
    bool identity_;
    std::vector<float> kernel_;    // (kPhases + 1) * kTaps
    std::vector<float> buf_;       // interleaved input still needed
    int64_t base_ = 0;             // absolute input frame index of buf_[0]
    uint64_t pushed_ = 0;          // input frames pushed so far
    uint64_t next_out_ = 0;        // index of the next output frame
    uint64_t total_out_ = 0;       // set by finish()
    bool finished_ = false;
};

} // namespace x2::audio
