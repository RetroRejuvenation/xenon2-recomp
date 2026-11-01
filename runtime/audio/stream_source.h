// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// What the music player (cdtv/cd_player.h) plays: a random-access stereo stream of float frames at the int16 scale, and the
// spec that says how to play one (rate, loop, crossfade, gain). A CD track, a decoded pack file and an intro followed by a
// loop body are all StreamSources behind the same voice engine, so loops, crossfades, volume ramps and determinism are shared
// (docs/re-notes/asset-packs-music.md).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace x2::audio {

class StreamSource {
public:
    virtual ~StreamSource() = default;
    // Interleaved stereo frames [first, first + n) into `out` (2n floats). The scale is that of 16-bit samples (32768 = full
    // scale). Frames outside the stream (negative, or past its length) read as silence. Called with ascending positions in the
    // common case; a random access (a loop wrap) may cost a seek.
    virtual void read(int64_t first, size_t n, float* out) = 0;
};

// What happens when the stream has played out.
enum class LoopRule : uint8_t {
    Off,      // play once, then silence (the game is told nothing: finished() stays false)
    Game,     // the original's rule: the game asks "is it done" every so often and starts the track again from its start
    Seamless, // the pack's own loop points [loop_start, loop_end), sample-exact (crossfaded when crossfade_ms > 0)
};

struct StreamSpec {
    std::function<std::unique_ptr<StreamSource>()> open; // a fresh, independent reader (every voice has its own)
    int rate = 44100;                                    // source sample rate
    uint64_t frames = 0;                                 // length of the whole stream in frames (intro included)
    LoopRule loop = LoopRule::Game;
    uint64_t loop_start = 0, loop_end = 0;               // Seamless only: loop_start < loop_end <= frames
    double gain = 1.0;                                   // linear, applied on top of the player's volume
    int crossfade_ms = -1;                               // -1: the player's own setting (the CD's --cd-crossfade)
    int fade_in_ms = 0, fade_out_ms = 0;
    std::string name;                                    // diagnostics
};

// Frames held in memory (interleaved stereo, 16-bit scale). Shared, read-only.
using FrameBuffer = std::shared_ptr<const std::vector<float>>;

class BufferStream final : public StreamSource {
public:
    explicit BufferStream(FrameBuffer frames) : frames_(std::move(frames)) {}
    void read(int64_t first, size_t n, float* out) override;
    uint64_t frames() const { return frames_ ? frames_->size() / 2 : 0; }

private:
    FrameBuffer frames_;
};

// `first` followed by `second`, sample-exact (both are at the same rate; `first_frames` is the length of the first).
class ConcatStream final : public StreamSource {
public:
    ConcatStream(std::unique_ptr<StreamSource> first, uint64_t first_frames, std::unique_ptr<StreamSource> second)
        : a_(std::move(first)), b_(std::move(second)), na_(first_frames) {}
    void read(int64_t first, size_t n, float* out) override;

private:
    std::unique_ptr<StreamSource> a_, b_;
    uint64_t na_;
};

} // namespace x2::audio
