// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Fixed-timestep pacing of emulated frames against a host clock, independent of the host refresh rate.
// Frame n is due at origin + n * frame_ns (integer nanoseconds), so there is no accumulated rounding drift.
// If the host falls more than max_catchup frames behind (debugger stop, window drag) the schedule is rebased
// instead of running a burst of frames.
#pragma once
#include <cstdint>

namespace x2 {

class FramePacer {
public:
    explicit FramePacer(uint64_t frame_ns = 20'000'000, uint32_t max_catchup = 4)
        : frame_ns_(frame_ns), max_catchup_(max_catchup) {}

    void reset(uint64_t now_ns) {
        origin_ = now_ns;
        frames_ = 0;
    }
    // Number of emulated frames to run now; the caller must run exactly that many.
    uint32_t frames_due(uint64_t now_ns) {
        uint64_t target = now_ns >= origin_ ? (now_ns - origin_) / frame_ns_ : 0;
        if (target <= frames_) return 0;
        uint64_t n = target - frames_;
        if (n > max_catchup_) {
            ++rebases_;
            origin_ = now_ns - frame_ns_; // run one frame now, the next one frame_ns later
            frames_ = 0;
            n = 1;
        }
        frames_ += n;
        total_ += n;
        return uint32_t(n);
    }
    // Host time at which the next frame becomes due.
    uint64_t next_due_ns() const { return origin_ + (frames_ + 1) * frame_ns_; }
    uint64_t frames_run() const { return total_; }
    uint32_t rebases() const { return rebases_; }

private:
    uint64_t frame_ns_;
    uint32_t max_catchup_;
    uint64_t origin_ = 0;
    uint64_t frames_ = 0; // frames run since origin_
    uint64_t total_ = 0;
    uint32_t rebases_ = 0;
};

} // namespace x2
