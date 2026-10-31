// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/pdos.h"

#include "import/rnc.h"

#include <algorithm>
#include <cstring>

namespace x2::import {

namespace {

constexpr uint32_t kM55 = 0x55555555u;
constexpr uint32_t kSectorCells = 8288; // distance between the sectors of a PDos track

struct Cells {
    const uint8_t* bits;
    uint32_t n;
    uint32_t bit(uint64_t i) const {
        i %= n;
        return (bits[size_t(i >> 3)] >> (7 - (i & 7))) & 1u;
    }
    uint32_t get32(uint64_t p) const {
        uint32_t v = 0;
        for (int j = 0; j < 32; ++j) v = (v << 1) | bit(p + uint64_t(j));
        return v;
    }
};

// p = the first cell of the info longs, i.e. just after the 16-cell marker. Returns true and fills `out` when the sector
// number, track number and checksum all match.
bool decode_sector(const Cells& c, uint64_t p, int track, int sector, uint8_t* out) {
    const uint32_t info = (((c.get32(p) & kM55) << 1) | (c.get32(p + 32) & kM55)) & 0x7FFFFFFFu;
    if (int((info >> 16) & 0xFF) != track || int((info >> 24) & 0xFF) != sector) return false;
    const uint64_t q = p + 64;
    uint32_t x = 0, prev = 0;
    uint32_t words[128];
    for (int k = 0; k < 128; ++k) {
        const uint32_t a = c.get32(q + 32u * uint32_t(k)), o = c.get32(q + 32u * uint32_t(k) + 4096);
        x ^= a ^ o;
        const uint32_t dec = ((a & kM55) << 1) | (o & kM55);
        words[k] = prev ^ dec;
        prev = dec;
    }
    x &= kM55;
    const uint32_t folded = (x & 0xFFFFu) | (((x >> 16) << 1) & 0xFFFFu);
    if (folded != (info & 0xFFFFu)) return false;
    for (int k = 0; k < 128; ++k) {
        out[4 * k] = uint8_t(words[k] >> 24);
        out[4 * k + 1] = uint8_t(words[k] >> 16);
        out[4 * k + 2] = uint8_t(words[k] >> 8);
        out[4 * k + 3] = uint8_t(words[k]);
    }
    return true;
}

int decode_stream(const uint8_t* bits, uint32_t n, int track, std::array<std::array<uint8_t, 512>, kPdosSectors>& sectors,
                  std::array<bool, kPdosSectors>& have) {
    if (n < 32 || n < kSectorCells * 2) return 0;
    Cells c{bits, n};
    std::vector<uint64_t> hits;
    uint32_t w = 0;
    // The stream is a circle: a marker that straddles its seam is found by reading on past the end.
    for (uint32_t i = 0; i < n + 15; ++i) {
        w = ((w << 1) | c.bit(i)) & 0xFFFFu;
        if (i >= 15 && w == 0x4489u) hits.push_back(uint64_t(i) + 1);
    }
    int found = 0;
    for (uint64_t h : hits) {
        for (int dl = -6; dl <= 6; ++dl) {
            for (int s = 0; s < kPdosSectors; ++s) {
                if (have[size_t(s)]) continue;
                const int64_t p = int64_t(h) + dl;
                if (p < 0) continue;
                if (decode_sector(c, uint64_t(p), track, s, sectors[size_t(s)].data())) {
                    have[size_t(s)] = true;
                    ++found;
                }
            }
        }
    }
    return found;
}

} // namespace

int pdos_decode_track(const Track& t, int track_number, std::array<std::array<uint8_t, 512>, kPdosSectors>& sectors,
                      std::array<bool, kPdosSectors>& have) {
    int total = 0;
    for (bool b : have) total += b;
    if (t.kind != Track::Kind::Raw) return 0;
    auto done = [&] { return total >= kPdosSectors; };
    if (t.mfm.size() * 8 >= t.bit_length) total += decode_stream(t.mfm.data(), t.bit_length, track_number, sectors, have);
    for (const Track::Stream& s : t.alt) {
        if (done()) break;
        if (s.bits.size() * 8 < s.bit_length) continue;
        total += decode_stream(s.bits.data(), s.bit_length, track_number, sectors, have);
    }
    return total;
}

bool pdos_assemble(const DiskImage& img, PdosDisk& out, std::string& why) {
    out = PdosDisk();
    const Track& t0 = img.track(0);
    if (t0.kind != Track::Kind::Decoded || !t0.all_sectors_ok()) {
        why = "cylinder 0 head 0 is not a readable standard Amiga track";
        return false;
    }
    out.image.assign(kPdosDiskBytes, 0);
    std::memcpy(out.image.data(), t0.sectors.data(), kPdosTrack0Bytes);
    for (int t = kPdosFirstTrack; t <= kPdosLastTrack; ++t) {
        const Track& tr = img.track(t);
        if (tr.kind != Track::Kind::Raw) {
            why = "track " + std::to_string(t) + " is not a raw track (a PDos disk keeps its data tracks as raw MFM)";
            return false;
        }
        std::array<std::array<uint8_t, 512>, kPdosSectors> sectors{};
        std::array<bool, kPdosSectors> have{};
        pdos_decode_track(tr, t, sectors, have);
        for (int s = 0; s < kPdosSectors; ++s) {
            if (have[size_t(s)]) std::memcpy(out.image.data() + kPdosTrack0Bytes + size_t(t - 1) * kPdosTrackBytes + size_t(s) * 512, sectors[size_t(s)].data(), 512);
            else out.missing.emplace_back(t, s);
        }
    }
    return true;
}

bool pdos_is_v5_boot(const uint8_t* image, size_t size) {
    return size >= 1024 && std::memcmp(image, "DOS", 3) == 0 && rnc_crc16(image, 1024) == kPdosBootCrc;
}

namespace {
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
}

bool pdos_build_from_image(const std::vector<uint8_t>& image, const std::vector<std::pair<int, int>>& missing, PdosBuild& build,
                           std::string& why) {
    build = PdosBuild();
    if (image.size() != kPdosDiskBytes) { why = "a PDos disk image is " + std::to_string(kPdosDiskBytes) + " bytes"; return false; }
    if (!pdos_is_v5_boot(image.data(), image.size())) { why = "the boot block is not the PDos release's (CRC16 0xBDCE)"; return false; }
    // Which stream does an unreadable sector fall in? Image offset of (track, sector).
    auto sector_offset = [](int t, int s) { return kPdosTrack0Bytes + size_t(t - 1) * kPdosTrackBytes + size_t(s) * 512; };
    for (int i = 0; i < kPdosFileCount; ++i) {
        const uint8_t* e = image.data() + kPdosFileTableOffset + size_t(i) * 8;
        const uint32_t first = be16(e), count = be16(e + 2), off = be16(e + 4);
        PdosFile f;
        f.index = i;
        if (first < 12 || count == 0 || off >= 512 || size_t(first - 1) * 512 + off + kRncHeaderBytes > image.size()) {
            f.why = "its file table entry is invalid";
            build.files.push_back(f);
            continue;
        }
        f.image_offset = size_t(first - 1) * 512 + off; // the game's sector numbers are one above the image's
        std::vector<uint8_t> unpacked;
        RncHeader h;
        std::string w;
        const bool ok = rnc_unpack(image.data(), image.size(), f.image_offset, unpacked, h, w);
        f.packed = h.packed;
        f.unpacked = h.unpacked;
        f.ok = ok;
        f.why = ok ? "" : w;
        const size_t end = f.image_offset + kRncHeaderBytes + h.packed;
        for (const auto& m : missing) {
            size_t so = sector_offset(m.first, m.second);
            if (so < end && so + 512 > f.image_offset) f.damaged.push_back(m);
        }
        build.files_ok += ok;
        build.files.push_back(std::move(f));
    }
    std::vector<uint8_t> game;
    RncHeader gh;
    std::string gw;
    if (rnc_unpack(image.data(), image.size(), kPdosStage2Offset, game, gh, gw) && game.size() <= 0x3FC00 && game.size() > 0x7400 &&
        game[0] == 0x60 && game[1] == 0x00) {
        build.game_image = std::move(game);
        build.game_image.resize(0x3FC00, 0);
        build.game_ok = true;
    } else if (gw.empty()) {
        gw = "it does not look like the game";
    }
    build.disc = image;
    if (!build.game_ok) {
        why = "the game's packed stream does not verify: " + gw;
        return false;
    }
    if (build.files_ok != kPdosFileCount) {
        why = std::to_string(kPdosFileCount - build.files_ok) + " of the " + std::to_string(kPdosFileCount) + " packed game files do not verify";
        return false;
    }
    return true;
}

bool pdos_build(const PdosDisk& disk, PdosBuild& build, std::string& why) {
    return pdos_build_from_image(disk.image, disk.missing, build, why);
}

} // namespace x2::import
