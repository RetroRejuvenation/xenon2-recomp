// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/retail.h"

#include <cstdio>
#include <cstring>

namespace x2::import {

namespace {

constexpr uint16_t kSyncWords[11] = {0x8A91, 0x8A44, 0x8A45, 0x8A51, 0x8912, 0x8911, 0x8914, 0x8915, 0x8944, 0x8945, 0x8951};
constexpr char kSignature[] = "Rob Northen Comp";
constexpr size_t kMaxHits = 256; // a track full of sync-like patterns is noise, not a key track

uint32_t lfsr_next(uint32_t x) { return ((x << 1) & 0x7FFFFF) | (((x >> 22) ^ x) & 1); }
uint8_t lfsr_byte(uint32_t x) { return uint8_t((x >> 15) & 0xFF); }

} // namespace

CopylockReport verify_copylock_track(const Track& t) {
    CopylockReport r;
    if (t.kind == Track::Kind::Decoded) { r.why = "cylinder 0 head 1 holds ordinary Amiga sectors, not a key track"; return r; }
    if (t.kind != Track::Kind::Raw || t.mfm.size() * 8 < t.bit_length || t.bit_length < 40000) {
        r.why = "cylinder 0 head 1 holds no key track data (the image does not carry it)";
        return r;
    }
    const uint32_t n = t.bit_length;
    auto bit = [&](uint64_t i) -> uint32_t {
        i %= n;
        return (t.mfm[size_t(i >> 3)] >> (7 - (i & 7))) & 1u;
    };
    struct Hit {
        uint32_t pos;
        int word;
    };
    std::vector<Hit> hits;
    uint32_t w = 0;
    for (int k = 0; k < 16; ++k) w = (w << 1) | bit(uint64_t(k));
    for (uint32_t i = 0; i < n && hits.size() < kMaxHits; ++i) {
        for (int s = 0; s < 11; ++s)
            if (w == kSyncWords[s]) hits.push_back({i, s});
        w = ((w << 1) | bit(uint64_t(i) + 16)) & 0xFFFFu;
    }
    // Plain (not odd/even split) MFM bytes after the sync word: the data bit is the second cell of each pair.
    auto body = [&](uint32_t pos, uint8_t* out, size_t count) {
        uint64_t p = uint64_t(pos) + 16;
        for (size_t k = 0; k < count; ++k) {
            uint32_t v = 0;
            for (int b = 0; b < 8; ++b) {
                v = (v << 1) | bit(p + 1);
                p += 2;
            }
            out[k] = uint8_t(v);
        }
    };
    // The order check uses the first hit of every sync word whose index byte is right: in a stream that is longer than one
    // revolution (flux decodes append the start of the next) those first hits are one revolution's worth, in track order,
    // possibly starting in the middle. The data checks (signature, LFSR) look at every hit of the word, so a copy that
    // runs into the seam of a stream does not hide an intact one.
    uint32_t pos[11] = {};
    bool have[11] = {};
    bool signature = false;
    uint32_t seed = 0;
    int best_lfsr = 0;
    for (const Hit& h : hits) {
        std::vector<uint8_t> b(513);
        body(h.pos, b.data(), b.size());
        if (b[0] != h.word) continue;
        if (!have[h.word]) { have[h.word] = true; pos[h.word] = h.pos; ++r.syncs_found; }
        if (h.word == 6 && std::memcmp(b.data() + 1, kSignature, 16) == 0) signature = true;
        if (h.word == 0 && !seed) {
            const uint8_t* d0 = b.data() + 1;
            uint32_t x = uint32_t(d0[0]) << 15;
            for (int k = 1; k < 16; ++k) x |= uint32_t(d0[k] & 1) << (15 - k);
            if (x == 0) continue;
            const uint32_t candidate = x;
            int ok = 0;
            for (int k = 0; k < 512; ++k) {
                if (d0[k] != lfsr_byte(x)) break;
                ++ok;
                x = lfsr_next(x);
            }
            if (ok > best_lfsr) best_lfsr = ok;
            if (ok == 512) seed = candidate;
        }
    }
    if (r.syncs_found < 11) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "only %d of the eleven Copylock sync words were found with the right index byte", r.syncs_found);
        r.why = buf;
        return r;
    }
    int descents = 0;
    for (int i = 0; i < 11; ++i)
        if (pos[(i + 1) % 11] < pos[i]) ++descents;
    if (descents != 1) { r.why = "the eleven sync words are not in the Copylock order around the track"; return r; }
    r.signature = signature;
    if (!signature) { r.why = "the 'Rob Northen Comp' signature is missing from sector 6"; return r; }
    r.lfsr_bytes = seed ? 512 : best_lfsr;
    if (!seed) { r.why = "the sector 0 data is not the Copylock LFSR sequence"; return r; }
    r.seed = seed;
    r.genuine = true;
    return r;
}

std::string copylock_receipt(uint32_t seed) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0x%06X\n", seed & 0x7FFFFF);
    return buf;
}

std::vector<uint8_t> chain_decode(const uint8_t* stored, size_t n, uint32_t init) {
    std::vector<uint8_t> out(n);
    uint32_t prev = init & 0xFFFF;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t wd = ((uint32_t(stored[i]) << 8) | stored[i + 1]) ^ prev;
        out[i] = uint8_t(wd >> 8);
        out[i + 1] = uint8_t(wd);
        prev = wd;
    }
    return out;
}

bool decode_retail_image(const DiskImage& disc, std::vector<uint8_t>& image, std::string& why) {
    constexpr size_t first = kRetailImageOffset / kSectorBytes, count = kRetailImageSize / kSectorBytes;
    std::vector<uint8_t> stored(kRetailImageSize);
    std::vector<int> bad_tracks;
    for (size_t b = first; b < first + count; ++b) {
        const uint8_t* s = disc.sector(int(b));
        if (!s) {
            int t = int(b / kSectorsPerTrack);
            if (bad_tracks.empty() || bad_tracks.back() != t) bad_tracks.push_back(t);
            continue;
        }
        std::memcpy(&stored[(b - first) * kSectorBytes], s, kSectorBytes);
    }
    if (!bad_tracks.empty()) {
        why = "the game image sectors are not all readable (track";
        why += bad_tracks.size() > 1 ? "s " : " ";
        for (size_t i = 0; i < bad_tracks.size() && i < 6; ++i) why += (i ? ", " : "") + std::to_string(bad_tracks[i]);
        if (bad_tracks.size() > 6) why += ", ...";
        why += ")";
        return false;
    }
    image = chain_decode(stored.data(), stored.size());
    return true;
}

bool decode_retail_image_adf(const uint8_t* adf, size_t size, std::vector<uint8_t>& image) {
    if (!adf || size != 1760 * kSectorBytes) return false;
    image = chain_decode(adf + kRetailImageOffset, kRetailImageSize);
    return true;
}

bool is_retail_image(const std::vector<uint8_t>& image) {
    static const uint8_t kEntry[4] = {0x60, 0x00, 0x6C, 0x30}; // bra.w $7032
    static const uint8_t kStub[kRetailStubLength] = {0x2F, 0x0E, 0x4D, 0xFA, 0, 0, 0x48, 0xD6, 0xFF, 0xFF, 0x4D, 0xEE, 0x00, 0x40};
    const size_t at = kRetailStubAddress - 0x400;
    if (image.size() != kRetailImageSize || at + kRetailStubLength > image.size()) return false;
    if (std::memcmp(image.data(), kEntry, 4) != 0) return false;
    for (size_t i = 0; i < kRetailStubLength; ++i)
        if (i != 4 && i != 5 && image[at + i] != kStub[i]) return false;
    return true;
}

} // namespace x2::import
