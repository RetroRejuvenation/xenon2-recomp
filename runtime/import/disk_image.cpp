// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "import/disk_image.h"

#include "import/sha1.h"

#include <algorithm>
#include <cstring>

namespace x2::import {

namespace {
constexpr size_t kAdfBytes = size_t(kStandardTracks) * kTrackBytes; // 901120

bool has_magic(const uint8_t* d, size_t n, const char* m, size_t at = 0) {
    size_t len = std::strlen(m);
    return n >= at + len && std::memcmp(d + at, m, len) == 0;
}
} // namespace

bool Track::all_sectors_ok() const {
    if (kind != Kind::Decoded || sectors.size() != kTrackBytes) return false;
    for (SectorStatus s : status)
        if (s != SectorStatus::Ok) return false;
    return true;
}

bool DiskImage::from_adf(const uint8_t* data, size_t size, DiskImage& out) {
    if (size != kAdfBytes) return false;
    out = DiskImage();
    for (int t = 0; t < kStandardTracks; ++t) {
        Track& tr = out.track(t);
        tr.kind = Track::Kind::Decoded;
        tr.sectors.assign(data + size_t(t) * kTrackBytes, data + size_t(t + 1) * kTrackBytes);
        tr.status.fill(SectorStatus::Ok);
    }
    return true;
}

const uint8_t* DiskImage::sector(int block) const {
    if (block < 0 || block >= kStandardTracks * kSectorsPerTrack) return nullptr;
    const Track& tr = tracks_[size_t(block / kSectorsPerTrack)];
    int s = block % kSectorsPerTrack;
    if (tr.kind != Track::Kind::Decoded || tr.sectors.size() != kTrackBytes || tr.status[size_t(s)] != SectorStatus::Ok)
        return nullptr;
    return tr.sectors.data() + size_t(s) * kSectorBytes;
}

std::vector<int> DiskImage::nonstandard_tracks() const {
    std::vector<int> v;
    for (int t = 0; t < kStandardTracks; ++t)
        if (!tracks_[size_t(t)].all_sectors_ok()) v.push_back(t);
    return v;
}

std::string DiskImage::describe_nonstandard() const {
    std::vector<int> bad = nonstandard_tracks();
    std::string s;
    size_t shown = 0;
    for (int t : bad) {
        if (shown++ == 6) break;
        const Track& tr = tracks_[size_t(t)];
        if (!s.empty()) s += "; ";
        s += "track " + std::to_string(t) + " (cylinder " + std::to_string(t / 2) + ", side " + std::to_string(t % 2) + "): ";
        switch (tr.kind) {
        case Track::Kind::Empty: s += "missing"; break;
        case Track::Kind::Raw: s += "raw MFM data that is not standard Amiga sectors"; break;
        case Track::Kind::Decoded: {
            int badsec = 0;
            for (SectorStatus st : tr.status) badsec += st != SectorStatus::Ok;
            s += std::to_string(badsec) + " unreadable sector" + (badsec == 1 ? "" : "s");
            break;
        }
        }
    }
    if (bad.size() > shown) s += "; and " + std::to_string(bad.size() - shown) + " more";
    return s;
}

bool DiskImage::to_adf(std::vector<uint8_t>& out, std::string& why, bool zero_fill_missing) const {
    out.clear();
    std::vector<int> bad;
    for (int t = 0; t < kStandardTracks; ++t) {
        const Track& tr = tracks_[size_t(t)];
        if (tr.all_sectors_ok()) continue;
        if (zero_fill_missing && tr.kind == Track::Kind::Empty) continue;
        bad.push_back(t);
    }
    if (!bad.empty()) {
        why = describe_nonstandard();
        return false;
    }
    out.assign(kAdfBytes, 0);
    for (int t = 0; t < kStandardTracks; ++t) {
        const Track& tr = tracks_[size_t(t)];
        if (tr.kind == Track::Kind::Decoded) std::memcpy(out.data() + size_t(t) * kTrackBytes, tr.sectors.data(), kTrackBytes);
    }
    return true;
}

std::string DiskImage::sector_sha1(int track, int sector) const {
    if (track < 0 || track >= kMaxTracks || sector < 0 || sector >= kSectorsPerTrack) return {};
    const Track& tr = tracks_[size_t(track)];
    if (tr.kind != Track::Kind::Decoded || tr.sectors.size() != kTrackBytes) return {};
    return sha1_hex(tr.sectors.data() + size_t(sector) * kSectorBytes, kSectorBytes);
}

std::string DiskImage::track_sha1(int track) const {
    if (track < 0 || track >= kMaxTracks) return {};
    const Track& tr = tracks_[size_t(track)];
    if (tr.kind != Track::Kind::Decoded || tr.sectors.size() != kTrackBytes) return {};
    return sha1_hex(tr.sectors.data(), kTrackBytes);
}

// ---------------------------------------------------------------------------------------------------------
// Standard Amiga MFM sectors.

namespace {

// `bits` as a circular bit string: the `n` (<= 32) bits starting at bit `pos` (wrapping), most significant first.
struct BitString {
    const uint8_t* d;
    uint32_t len;
    uint32_t get(uint32_t pos, int n) const {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            uint32_t p = (pos + uint32_t(i)) % len;
            v = (v << 1) | ((d[p >> 3] >> (7 - (p & 7))) & 1u);
        }
        return v;
    }
};

uint32_t odd_even(uint32_t odd, uint32_t even) { return ((odd & 0x55555555u) << 1) | (even & 0x55555555u); }

} // namespace

int decode_amiga_mfm_track(const uint8_t* bits, uint32_t bit_length, int track_number, Track& out, bool wrap) {
    out = Track();
    out.kind = Track::Kind::Decoded;
    out.sectors.assign(kTrackBytes, 0);
    out.status.fill(SectorStatus::Missing);
    if (!bits || bit_length < 64) return 0;
    BitString bs{bits, bit_length};
    int found = 0;
    uint32_t window = wrap ? bs.get(bit_length - 31, 31) : 0; // the 31 bits before bit 0, so the scan also finds a wrapped sync
    for (uint32_t pos = 0; pos < bit_length; ++pos) {
        window = (window << 1) | bs.get(pos, 1);
        if (window != 0x44894489u) continue;
        // `p` is the first bit after the two sync words. Header: info (odd, even), label (4 odd + 4 even longs),
        // header checksum (odd, even), data checksum (odd, even), then 128 odd and 128 even data longs.
        uint32_t p = pos + 1;
        if (!wrap && uint64_t(p) + 448 + 8192 > bit_length) continue; // the sector runs past the end of a linear stream
        uint32_t raw[10];
        uint32_t hchk = 0;
        for (int i = 0; i < 10; ++i) hchk ^= (raw[i] = bs.get(p + uint32_t(i) * 32, 32));
        hchk &= 0x55555555u;
        uint32_t info = odd_even(raw[0], raw[1]);
        uint32_t hchk_stored = odd_even(bs.get(p + 320, 32), bs.get(p + 352, 32));
        uint32_t dchk_stored = odd_even(bs.get(p + 384, 32), bs.get(p + 416, 32));
        int fmt = int(info >> 24), trk = int((info >> 16) & 0xFF), sec = int((info >> 8) & 0xFF);
        if (fmt != 0xFF || sec >= kSectorsPerTrack || trk != track_number) continue;
        SectorStatus& st = out.status[size_t(sec)];
        if (hchk != hchk_stored) {
            if (st == SectorStatus::Missing) st = SectorStatus::BadHeader;
            continue;
        }
        uint32_t base = p + 448;
        uint32_t dchk = 0;
        for (int i = 0; i < 256; ++i) dchk ^= bs.get(base + uint32_t(i) * 32, 32);
        dchk &= 0x55555555u;
        if (dchk != dchk_stored) {
            if (st != SectorStatus::Ok) st = SectorStatus::BadData;
            continue;
        }
        if (st == SectorStatus::Ok) continue; // a repeated good sector (other revolution or a long track)
        uint8_t* dst = out.sectors.data() + size_t(sec) * kSectorBytes;
        for (int i = 0; i < 128; ++i) {
            uint32_t v = odd_even(bs.get(base + uint32_t(i) * 32, 32), bs.get(base + uint32_t(i + 128) * 32, 32));
            dst[4 * i] = uint8_t(v >> 24);
            dst[4 * i + 1] = uint8_t(v >> 16);
            dst[4 * i + 2] = uint8_t(v >> 8);
            dst[4 * i + 3] = uint8_t(v);
        }
        st = SectorStatus::Ok;
        ++found;
    }
    return found;
}

// ---------------------------------------------------------------------------------------------------------
// Container detection

Container detect_container(const uint8_t* d, size_t n) {
    // Definitive magics first.
    if (n >= 2 && d[0] == 0x1F && d[1] == 0x8B) return Container::Adz;
    if (has_magic(d, n, "PK\x03\x04") || has_magic(d, n, "PK\x05\x06")) return Container::Zip;
    if (has_magic(d, n, "DMS!")) return Container::Dms;
    if (has_magic(d, n, "UAE-1ADF")) return Container::ExtAdf;
    if (has_magic(d, n, "UAE--ADF")) return Container::ExtAdf1;
    if (has_magic(d, n, "CAPS")) return Container::Ipf;
    if (has_magic(d, n, "HXCPICFE") || has_magic(d, n, "HXCHFEV3")) return Container::Hfe;
    if (n >= 8 && d[0] == 0x08 && d[1] == 0xF8 && d[2] == 0xEA && d[3] == 0xB1 && d[4] == 0xB3 && d[5] == 0x04) return Container::FluxEngine;
    // A raw image has no magic: its size says what it is, and beats the weaker content guesses below.
    if (n == kAdfBytes) return Container::Adf;
    if (n == 2 * kAdfBytes) return Container::AdfHd;
    if (has_magic(d, n, "SCP") && n >= 16) return Container::Scp;
    // LHA/LZH level 0-2 header: size/checksum bytes, then "-lh5-" style method id at offset 2.
    if (n >= 7 && d[2] == '-' && d[3] == 'l' && (d[4] == 'h' || d[4] == 'z') && d[6] == '-') return Container::Lha;
    // KryoFlux stream files start with out-of-band blocks (0x0D, type 1-4, 16-bit size): the host info text, then a
    // block whose text names the device.
    if (n >= 8 && d[0] == 0x0D && d[1] >= 1 && d[1] <= 4 && size_t(d[2] | d[3] << 8) + 4 <= n) return Container::KryoFlux;
    static const char kKryo[] = "KryoFlux";
    for (size_t i = 0; n >= sizeof kKryo - 1 && i + sizeof kKryo - 1 <= std::min<size_t>(n, 512); ++i)
        if (std::memcmp(d + i, kKryo, sizeof kKryo - 1) == 0) return Container::KryoFlux;
    return Container::Unknown;
}

const char* container_name(Container c) {
    switch (c) {
    case Container::Adf: return "ADF";
    case Container::AdfHd: return "HD ADF";
    case Container::Adz: return "ADZ (gzip)";
    case Container::Zip: return "ZIP";
    case Container::Dms: return "DMS";
    case Container::ExtAdf: return "extended ADF (UAE-1ADF)";
    case Container::ExtAdf1: return "extended ADF (UAE--ADF)";
    case Container::Ipf: return "IPF";
    case Container::Scp: return "SCP";
    case Container::KryoFlux: return "KryoFlux stream";
    case Container::Hfe: return "HFE";
    case Container::FluxEngine: return "FluxEngine";
    case Container::Lha: return "LHA";
    case Container::Unknown: break;
    }
    return "unknown";
}

} // namespace x2::import
