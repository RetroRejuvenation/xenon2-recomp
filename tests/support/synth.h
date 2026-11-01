// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Synthetic inputs shared by the asset-pack tests and the fuzz seeds (docs/re-notes/asset-packs-v2-design.md section 9): files, PNGs, ZIPs,
// WAVs and a deterministic mutator, all generated on the fly. Nothing here is game content.
#pragma once
#include "gfx/image_io.h"
#include "import/sha1.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace x2::synth {

namespace fs = std::filesystem;

inline void put_file(const std::string& path, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
}

inline std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// An RGBA PNG whose pixel (x, y) is f(x, y) as 0xAABBGGRR.
template <class F>
void put_png(const std::string& path, int w, int h, F f) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::vector<uint8_t> px;
    px.reserve(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 4; ++k) px.push_back(uint8_t(f(x, y) >> (8 * k)));
    write_png(path, px, w, h, true);
}
inline void put_flat(const std::string& path, int w, int h, uint32_t c = 0xFF0000FFu) {
    put_png(path, w, h, [c](int, int) { return c; });
}
// The PNG as bytes (no file).
template <class F>
std::string png_bytes(const std::string& scratch, int w, int h, F f) {
    put_png(scratch, w, h, f);
    return slurp(scratch);
}

inline uint32_t crc_of(const std::string& d) { return import::crc32(reinterpret_cast<const uint8_t*>(d.data()), d.size()); }

struct ZipFile {
    std::string name, data;
    int method = 0;       // 0 stored, 8 "deflate" (written as a deflate stored block)
    uint32_t usize = 0;   // 0: data.size() (a nonzero value declares a different unpacked size)
    uint16_t flags = 0;
    bool bad_crc = false;
};

// Raw deflate as one stored block (valid for inflate, not compressed).
inline std::string deflate_stored(const std::string& d) {
    std::string o;
    o += char(1);
    o += char(d.size() & 255); o += char(d.size() >> 8 & 255);
    o += char(~d.size() & 255); o += char((~d.size() >> 8) & 255);
    return o + d;
}

inline std::string make_zip(const std::vector<ZipFile>& files) {
    std::string out, cd;
    auto u16 = [](std::string& o, uint32_t v) { o += char(v & 255); o += char(v >> 8 & 255); };
    auto u32 = [&](std::string& o, uint32_t v) { u16(o, v & 0xFFFF); u16(o, v >> 16); };
    for (const ZipFile& f : files) {
        const std::string packed = f.method == 8 ? deflate_stored(f.data) : f.data;
        const uint32_t crc = crc_of(f.data) ^ (f.bad_crc ? 1u : 0u), off = uint32_t(out.size());
        const uint32_t us = f.usize ? f.usize : uint32_t(f.data.size());
        u32(out, 0x04034b50); u16(out, 20); u16(out, f.flags); u16(out, uint32_t(f.method)); u16(out, 0); u16(out, 0);
        u32(out, crc); u32(out, uint32_t(packed.size())); u32(out, us); u16(out, uint16_t(f.name.size())); u16(out, 0);
        out += f.name + packed;
        u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, f.flags); u16(cd, uint32_t(f.method)); u16(cd, 0); u16(cd, 0);
        u32(cd, crc); u32(cd, uint32_t(packed.size())); u32(cd, us); u16(cd, uint16_t(f.name.size()));
        u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, off);
        cd += f.name;
    }
    const uint32_t cd_off = uint32_t(out.size());
    out += cd;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, uint16_t(files.size())); u16(out, uint16_t(files.size()));
    u32(out, uint32_t(cd.size())); u32(out, cd_off); u16(out, 0);
    return out;
}

// A PCM16 WAV.
inline std::string make_wav(int rate, int channels, const std::vector<int16_t>& pcm) {
    std::string o = "RIFF";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) o += char(v >> (8 * i) & 255); };
    auto u16 = [&](uint32_t v) { o += char(v & 255); o += char(v >> 8 & 255); };
    u32(uint32_t(36 + pcm.size() * 2));
    o += "WAVEfmt ";
    u32(16); u16(1); u16(uint32_t(channels)); u32(uint32_t(rate)); u32(uint32_t(rate * channels * 2)); u16(uint32_t(channels * 2)); u16(16);
    o += "data";
    u32(uint32_t(pcm.size() * 2));
    for (int16_t s : pcm) u16(uint16_t(s));
    return o;
}
// A square wave.
inline std::vector<int16_t> tone(size_t frames, int channels = 1, int amp = 8000) {
    std::vector<int16_t> v;
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) v.push_back(int16_t(((i / 16) & 1) ? amp : -amp));
    return v;
}

// A small deterministic generator and byte-level mutator for fuzz-style tests and seeds.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    size_t below(size_t n) { return n ? size_t(next() % n) : 0; }
};

inline std::string mutate(const std::string& base, Rng& r, const std::vector<std::string>& tokens) {
    std::string s = base;
    const int n = 1 + int(r.below(4));
    for (int i = 0; i < n && !s.empty(); ++i) {
        const size_t at = r.below(s.size());
        switch (r.below(7)) {
        case 0: s[at] = char(r.next()); break;
        case 1: s.erase(at, 1 + r.below(std::min<size_t>(40, s.size() - at))); break;
        case 2: s.insert(at, s.substr(r.below(s.size()), 1 + r.below(60))); break;
        case 3: s.insert(at, tokens[r.below(tokens.size())]); break;
        case 4: s.resize(at); break;
        case 5: s[at] ^= char(1u << r.below(8)); break;
        default: {
            const size_t end = std::min(s.size(), at + 1 + r.below(30));
            s.replace(at, end - at, tokens[r.below(tokens.size())]);
        }
        }
    }
    return s;
}

} // namespace x2::synth
