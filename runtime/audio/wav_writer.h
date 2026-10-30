// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Minimal streaming WAV writer (16-bit PCM, interleaved). The RIFF sizes are patched on close().
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace x2 {

class WavWriter {
public:
    ~WavWriter() { close(); }
    bool open(const std::string& path, int rate, int channels);
    void write(const int16_t* samples, size_t count); // count = samples (not frames)
    void close();
    bool is_open() const { return f_ != nullptr; }

private:
    std::FILE* f_ = nullptr;
    uint32_t data_bytes_ = 0;
};

} // namespace x2
