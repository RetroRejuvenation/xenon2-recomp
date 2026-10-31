// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// CD-audio playback for the CDTV music layer: streams 44.1 kHz stereo tracks from disk (cdtv_disc.h), resamples
// them to the Paula output rate and adds them into Paula's output stream.
//
// Everything is driven by emulated time, never by the wall clock: the cue layer (cdtv_music.h) issues play/stop
// at the Paula output frame where the game called its music driver, and render() is given exactly the frames
// Paula produced. So --audio-out renders and audio hashes are exact and repeatable.
//
// Looping follows the CDTV game ($3A55C in its X2): every 50 VBL ticks it asks cdtv.device whether the track has
// finished and if so plays it again from the start, so there is a gap of up to one second at the loop point.
// With a crossfade (0-500 ms, default 0) the loop is seamless instead: the restart begins `crossfade` before
// the end and the two overlap with equal-power gains; track changes crossfade the same way. Without a crossfade a
// track change or stop fades the old track over 5 ms, only to avoid a click.
//
// Resampling: 32-tap Kaiser-windowed sinc (cut-off 0.46 x the lower rate), 256 phases with linear interpolation
// between them; the source position is an exact rational (frames + n / output_rate). At 44.1 kHz output the
// samples are copied unchanged.
#pragma once
#include "cdtv/cdtv_disc.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <vector>

namespace x2::cdtv {

enum class PauseMode { Keep, Duck, Stop }; // during the game's own pause (P): keep playing, play at 1/4, hold

// Buffered reader of one track's frames; frames outside the track read as silence.
class PcmReader {
public:
    bool open(const TrackSource& t);
    // Interleaved stereo frames [first, first + n) into out (2n samples).
    void read(int64_t first, size_t n, int16_t* out);
    uint64_t frames() const { return frames_; }

private:
    std::ifstream in_;
    uint64_t offset_ = 0, frames_ = 0;
    std::vector<int16_t> cache_;
    int64_t cache_first_ = 0;
    size_t cache_frames_ = 0;
};

class CdPlayer {
public:
    static constexpr int kLoopPollTicks = 50; // the CDTV game's CheckIO interval
    static constexpr int kTaps = 32, kPhases = 256;

    CdPlayer();
    void configure(int output_rate, int crossfade_ms);
    int output_rate() const { return rate_; }
    int crossfade_ms() const { return crossfade_ms_; }
    void set_volume(int percent);   // music volume 0..100, ramped over 10 ms
    void set_pause_mode(PauseMode m) { pause_mode_ = m; }
    void set_game_paused(bool p) { game_paused_ = p; }

    // Plays `t` (track number `number`) from the start; nullptr stops.
    void play(const TrackSource* t, int number);
    void stop();
    // One VBL of emulated time: the loop poll (every kLoopPollTicks ticks since play()). The CDTV release's own poll lives in
    // the game's code (docs/re-notes/cdtv-audio.md): it turns this off and calls finished() / restart() from there.
    void tick();
    void set_self_loop(bool on) { self_loop_ = on; }
    // True once the track has played out (nothing but fading voices is left): what cdtv.device's CheckIO reports as done.
    bool finished() const;
    // The game's reply to a finished request: the same track again from its start.
    void restart();
    // Adds the CD audio for the next n output frames into `stereo` (interleaved int16), saturating.
    void render(int16_t* stereo, size_t n);

    int track() const { return track_; } // 0 = stopped
    uint64_t loops() const { return loops_; }
    uint64_t clipped() const { return clipped_; }
    // Source position of the main voice (frames), for tests.
    int64_t position() const;

    // Output level of CD audio relative to int16 full scale before the volume; Paula's own output gain is
    // 0.75 (paula.h), so the CD's mastered full scale is brought to the same headroom.
    static constexpr double kCdGain = 0.75;

private:
    struct Voice {
        std::unique_ptr<PcmReader> src;
        const TrackSource* track = nullptr;
        int64_t pos = 0;      // source frame
        uint64_t frac = 0;    // + frac / rate_ frames
        double fade = 1.0, fade_step = 0.0; // per output frame; voice ends when fade reaches 0 while falling
        bool fading_out = false, finished = false, loop_started = false;
    };
    void start_voice(const TrackSource* t, bool fade_in);
    void fade_out_all(uint64_t frames);
    void render_voice(Voice& v, double* acc, size_t n);

    int rate_ = 48000, crossfade_ms_ = 0;
    uint64_t xfade_frames_ = 0; // crossfade in output frames
    std::vector<float> kernel_; // (kPhases + 1) x kTaps
    std::vector<Voice> voices_;
    std::vector<int16_t> scratch_;
    std::vector<double> acc_;
    int track_ = 0;
    const TrackSource* current_ = nullptr;
    int ticks_ = 0;
    bool self_loop_ = true;
    uint64_t loops_ = 0, clipped_ = 0;
    double volume_ = 1.0, volume_target_ = 1.0, pause_gain_ = 1.0;
    PauseMode pause_mode_ = PauseMode::Keep;
    bool game_paused_ = false;
};

} // namespace x2::cdtv
