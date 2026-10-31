// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/powerpack.h"

#include "import/crunch.h"

#include <algorithm>
#include <cstring>

namespace x2::import {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

constexpr uint32_t kM55 = 0x55555555u;
constexpr int kMainTrack = 48;
constexpr size_t kMainPacked = 0x2A314, kMainLen = 0x3F1F4;
constexpr uint32_t kMainBase = 0xE0C, kFileTable = 0x9512, kAddrTable = 0x94B2;
constexpr size_t kGameSize = 0x3FC00, kDiscSize = 1760 * 512;
constexpr int kMaxFiles = 32;

bool fail(std::string& why, std::string text) {
    why = std::move(text);
    return false;
}

} // namespace

bool speedlock_decode_track(const Track& t, uint8_t* out, std::string& why) {
    if (t.kind != Track::Kind::Raw || t.bit_length < 16 || t.mfm.size() * 8 < t.bit_length) return fail(why, "not a raw track");
    const uint32_t n = t.bit_length;
    auto bit = [&](uint64_t i) -> uint32_t {
        i %= n;
        return (t.mfm[size_t(i >> 3)] >> (7 - (i & 7))) & 1u;
    };
    // The first $4489 sync word, scanning without wrapping.
    uint32_t v = 0;
    for (uint32_t i = 0; i < 15; ++i) v = (v << 1) | bit(i);
    uint64_t sync_end = 0;
    for (uint32_t i = 15; i < n; ++i) {
        v = ((v << 1) | bit(i)) & 0xFFFFu;
        if (v == 0x4489u) { sync_end = uint64_t(i) + 1; break; }
    }
    if (!sync_end) return fail(why, "no sync word");
    auto longs = [&](uint64_t start, size_t count, uint32_t* dst) {
        for (size_t k = 0; k < count; ++k) {
            uint32_t w = 0;
            for (int j = 0; j < 32; ++j) w = (w << 1) | bit(start + 32 * k + uint64_t(j));
            dst[k] = w;
        }
    };
    const size_t nlongs = kPowerPackTrackBytes / 4;
    const uint64_t s = sync_end + 32; // the four raw bytes after the sync word are skipped
    std::vector<uint32_t> even(nlongs), odd(nlongs);
    uint32_t ck1, ck2;
    longs(s, nlongs, even.data());
    longs(s + 32 * nlongs, 1, &ck1);
    longs(s + 32 * nlongs + 32, nlongs, odd.data());
    longs(s + 64 * nlongs + 32, 1, &ck2);
    uint32_t sum = 0;
    for (size_t k = 0; k < nlongs; ++k) {
        uint32_t x = ((odd[k] & kM55) << 1) | (even[k] & kM55);
        sum += x;
        out[4 * k] = uint8_t(x >> 24);
        out[4 * k + 1] = uint8_t(x >> 16);
        out[4 * k + 2] = uint8_t(x >> 8);
        out[4 * k + 3] = uint8_t(x);
    }
    uint32_t ck = ((ck2 & kM55) << 1) | (ck1 & kM55);
    if (sum != ck) return fail(why, "checksum mismatch");
    return true;
}

bool powerpack_data_from_image(const DiskImage& img, std::vector<uint8_t>& data, std::string& why) {
    data.assign(kPowerPackDataSize, 0);
    for (int i = 0; i < kPowerPackDataTracks; ++i) {
        int tn = kPowerPackFirstDataTrack + i;
        std::string w;
        if (!speedlock_decode_track(img.track(tn), data.data() + size_t(i) * kPowerPackTrackBytes, w))
            return fail(why, "track " + std::to_string(tn) + ": " + w);
    }
    return true;
}

bool powerpack_build(const std::vector<uint8_t>& data, PowerPackBuild& out, std::string& why) {
    if (data.size() != kPowerPackDataSize) return fail(why, "the data tracks are not 1,085,184 bytes");
    // The main image: track 48, an MSB-first crunch, that fills $E0C..$3FFFF.
    const size_t main_off = size_t(kMainTrack - kPowerPackFirstDataTrack) * kPowerPackTrackBytes;
    std::vector<uint8_t> main_image;
    bool consumed = false;
    std::string w;
    if (!powerpack_decrunch_msb(data.data() + main_off, kMainPacked, kMainLen, main_image, consumed, w))
        return fail(why, "main image: " + w);
    if (!consumed) return fail(why, "main image: the packed stream was not used exactly (damaged or a different disk)");
    out.game_image.assign(kGameSize, 0);
    static const uint8_t kStub[4] = {0x60, 0x00, 0x6C, 0x30}; // bra.w $7032: the entry every canonical-shaped image has at $400
    std::memcpy(out.game_image.data(), kStub, 4);
    const size_t base = kMainBase - 0x400;
    std::memcpy(out.game_image.data() + base, main_image.data(), main_image.size());
    out.disc2.assign(kDiscSize, 0);
    static const uint8_t kDiscId[6] = {0, 0, 'X', '2', 'B', ' '};
    std::memcpy(out.disc2.data(), kDiscId, sizeof kDiscId);
    out.files = 0;
    // The file table inside the main image: {id (= module start sector), start track, track count} and, in a second
    // table, the 16-byte crunch parameters of each file.
    for (int i = 0; i < kMaxFiles; ++i) {
        size_t t = size_t(kFileTable - kMainBase) + 4 * size_t(i), a = size_t(kAddrTable - kMainBase) + 16 * size_t(i);
        if (t + 4 > main_image.size() || a + 16 > main_image.size()) return fail(why, "file table runs off the main image");
        const uint8_t* e = main_image.data() + t;
        uint16_t fid = be16(e);
        uint8_t st = e[2], ct = e[3];
        if (fid == 0xFF00 || ct == 0) break;
        const uint8_t* ent = main_image.data() + a;
        const size_t end_packed = be32(ent), unp_len = be32(ent + 4), final_len = be32(ent + 12);
        const uint32_t rle_word = be32(ent + 8);
        if (st < kPowerPackFirstDataTrack || size_t(st - kPowerPackFirstDataTrack + ct) > size_t(kPowerPackDataTracks))
            return fail(why, "game file " + std::to_string(i) + " lies outside the data tracks");
        const size_t off = size_t(st - kPowerPackFirstDataTrack) * kPowerPackTrackBytes, avail = size_t(ct) * kPowerPackTrackBytes;
        if (end_packed > avail) return fail(why, "game file " + std::to_string(i) + ": its packed size exceeds its tracks");
        std::vector<uint8_t> stage1, body;
        bool ok = false;
        if (!powerpack_decrunch_lsb(data.data() + off, end_packed, unp_len, stage1, ok, w))
            return fail(why, "game file " + std::to_string(i) + ": " + w);
        const uint8_t marker = uint8_t(rle_word >> 24);
        const size_t rle_end = rle_word & 0xFFFFFFu;
        if (rle_end > stage1.size()) return fail(why, "game file " + std::to_string(i) + ": RLE length exceeds its data");
        size_t left = 0;
        if (!powerpack_unrle(stage1.data(), rle_end, marker, final_len, body, left, w))
            return fail(why, "game file " + std::to_string(i) + ": " + w);
        if (!ok || left != 0) return fail(why, "game file " + std::to_string(i) + " did not decrunch intact");
        if (size_t(fid) * 512 + body.size() > out.disc2.size()) return fail(why, "game file " + std::to_string(i) + " does not fit disc 2");
        std::memcpy(out.disc2.data() + size_t(fid) * 512, body.data(), body.size());
        ++out.files;
    }
    if (out.files != 6) return fail(why, "expected six game files, found " + std::to_string(out.files));
    return true;
}

} // namespace x2::import
