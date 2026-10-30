// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/ext_adf.h"

#include <cstring>

namespace x2::import {

namespace {
uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

void set_raw(Track& tr, int track_number, const uint8_t* bits, size_t bytes, uint32_t bit_length, uint8_t revolutions) {
    Track decoded;
    int ok = bit_length >= 64 && bit_length <= bytes * 8
                 ? decode_amiga_mfm_track(bits, bit_length, track_number, decoded)
                 : 0;
    if (ok == kSectorsPerTrack && decoded.all_sectors_ok()) {
        tr = std::move(decoded);
        return;
    }
    tr = Track();
    tr.kind = Track::Kind::Raw;
    tr.mfm.assign(bits, bits + bytes);
    tr.bit_length = bit_length <= bytes * 8 ? bit_length : uint32_t(bytes * 8);
    tr.revolutions = revolutions;
}

bool set_normal(Track& tr, const uint8_t* data, size_t bytes) {
    if (bytes != kTrackBytes) return false;
    tr = Track();
    tr.kind = Track::Kind::Decoded;
    tr.sectors.assign(data, data + bytes);
    tr.status.fill(SectorStatus::Ok);
    return true;
}
} // namespace

bool read_ext_adf(const std::vector<uint8_t>& d, DiskImage& out, std::string& why, bool& high_density) {
    high_density = false;
    out = DiskImage();
    if (d.size() >= 8 && std::memcmp(d.data(), "UAE--ADF", 8) == 0) {
        constexpr size_t kTracks = 160, kTable = 8 + kTracks * 4;
        if (d.size() < kTable) { why = "truncated track table"; return false; }
        size_t off = kTable;
        for (size_t t = 0; t < kTracks; ++t) {
            uint32_t sync = be16(&d[8 + t * 4]), len = be16(&d[10 + t * 4]);
            if (len > d.size() - off) { why = "track " + std::to_string(t) + " runs past the end of the file"; return false; }
            Track& tr = out.track(int(t));
            if (sync == 0) {
                if (!set_normal(tr, &d[off], len)) {
                    why = "track " + std::to_string(t) + " is a normal track of " + std::to_string(len) + " bytes (expected 5632)";
                    return false;
                }
            } else if (len != 0) {
                tr.kind = Track::Kind::Raw;
                tr.mfm.assign(d.begin() + long(off), d.begin() + long(off + len));
                tr.bit_length = len * 8;
            }
            off += len;
        }
        return true;
    }
    if (d.size() < 12 || std::memcmp(d.data(), "UAE-1ADF", 8) != 0) { why = "not an extended ADF"; return false; }
    uint32_t ntracks = be16(&d[10]);
    if (ntracks == 0 || ntracks > kMaxTracks) {
        why = ntracks == 0 ? "no tracks" : "unreasonable track count " + std::to_string(ntracks);
        return false;
    }
    size_t off = 12 + size_t(ntracks) * 12;
    if (off > d.size()) { why = "truncated track table"; return false; }
    for (uint32_t t = 0; t < ntracks; ++t) {
        const uint8_t* e = &d[12 + size_t(t) * 12];
        uint32_t type = e[3], revolutions = uint32_t(e[2]) + 1;
        uint32_t len = uint32_t(e[5]) << 16 | uint32_t(e[6]) << 8 | e[7];
        uint32_t bitlen = uint32_t(e[9]) << 16 | uint32_t(e[10]) << 8 | e[11];
        if (len > d.size() - off) { why = "track " + std::to_string(t) + " runs past the end of the file"; return false; }
        if (len > 20000) high_density = true;
        if (t < kMaxTracks && len != 0) {
            Track& tr = out.track(int(t));
            if (type == 0) {
                if (len == 2 * kTrackBytes) high_density = true;
                else if (!set_normal(tr, &d[off], len)) {
                    why = "track " + std::to_string(t) + " is a normal track of " + std::to_string(len) + " bytes (expected 5632)";
                    return false;
                }
            } else if (type == 1) {
                set_raw(tr, int(t), &d[off], len, bitlen, uint8_t(revolutions > 255 ? 255 : revolutions));
            } else {
                why = "track " + std::to_string(t) + " has the unknown type " + std::to_string(type);
                return false;
            }
        }
        off += len;
    }
    return true;
}

} // namespace x2::import
