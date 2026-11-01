// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "audio/stream_source.h"

#include <algorithm>

namespace x2::audio {

void BufferStream::read(int64_t first, size_t n, float* out) {
    const int64_t total = int64_t(frames());
    for (size_t i = 0; i < n; ++i) {
        const int64_t f = first + int64_t(i);
        if (f < 0 || f >= total) {
            out[i * 2] = out[i * 2 + 1] = 0.0f;
        } else {
            out[i * 2] = (*frames_)[size_t(f) * 2];
            out[i * 2 + 1] = (*frames_)[size_t(f) * 2 + 1];
        }
    }
}

void ConcatStream::read(int64_t first, size_t n, float* out) {
    const int64_t split = int64_t(na_);
    size_t i = 0;
    if (first < split) { // the part (or all) that lies in the first stream, which is silent beyond its end
        const size_t k = size_t(std::min<int64_t>(int64_t(n), split - first));
        a_->read(first, k, out);
        i = k;
    }
    if (i < n) b_->read(first + int64_t(i) - split, n - i, out + i * 2);
}

} // namespace x2::audio
