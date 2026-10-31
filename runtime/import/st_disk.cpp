// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Atari ST disk image readers (st_disk.h). The Pasti layout follows tools/acq/stx.py and stx_fz.py, which were checked against
// four public dumps: the sector data of a record starts at (end of the descriptors) + (fuzzy-mask length) + (data offset).
#include "import/st_disk.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace x2::import {
namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct SizeGeometry {
    size_t bytes;
    int cylinders, sides, spt;
};
// The standard sizes of 3.5" disks as the ST formatted them (and one 81-cylinder cover-disk format).
constexpr SizeGeometry kSizeTable[] = {
    {368640, 80, 1, 9},  {409600, 80, 1, 10}, {450560, 80, 1, 11}, {737280, 80, 2, 9},
    {819200, 80, 2, 10}, {829440, 81, 2, 10}, {901120, 80, 2, 11},
};

} // namespace

uint16_t st_boot_checksum(const uint8_t* s) {
    uint32_t sum = 0;
    for (int i = 0; i < 256; ++i) sum += be16(s + 2 * i);
    return uint16_t(sum & 0xFFFF);
}

bool st_geometry(const uint8_t* d, size_t size, int& cylinders, int& sides, int& spt) {
    if (size < kStSectorBytes || size % kStSectorBytes != 0) return false;
    if (size >= 28) {
        const unsigned bps = le16(d + 11), bspt = le16(d + 24), bsides = le16(d + 26);
        if (bps == 512 && bspt >= 9 && bspt <= 11 && bsides >= 1 && bsides <= 2) {
            const size_t per = size_t(bspt) * bsides * kStSectorBytes;
            const size_t c = size / per;
            if (size % per == 0 && c >= 70 && c <= size_t(kStMaxCylinders)) {
                cylinders = int(c), sides = int(bsides), spt = int(bspt);
                return true;
            }
        }
    }
    for (const SizeGeometry& g : kSizeTable)
        if (g.bytes == size) {
            cylinders = g.cylinders, sides = g.sides, spt = g.spt;
            return true;
        }
    return false;
}

StContainer detect_st_container(const uint8_t* d, size_t size) {
    if (size >= 16 && std::memcmp(d, "RSY\0", 4) == 0) return StContainer::Stx;
    if (size >= 12 && be16(d) == 0x0E0F) {
        const unsigned spt = be16(d + 2), sides = be16(d + 4), first = be16(d + 6), last = be16(d + 8);
        if (spt >= 1 && spt <= kStMaxSectorsPerTrack && sides <= 1 && first <= last && last < unsigned(kStMaxCylinders))
            return StContainer::Msa;
    }
    int c, s, spt;
    if (size >= kStSectorBytes && st_geometry(d, size, c, s, spt) && (le16(d + 11) == 512 || st_boot_checksum(d) == 0x1234))
        return StContainer::St;
    return StContainer::None;
}

const char* st_container_name(StContainer c) {
    switch (c) {
    case StContainer::St: return "ST";
    case StContainer::Msa: return "MSA";
    case StContainer::Stx: return "STX (Pasti)";
    case StContainer::None: break;
    }
    return "unknown";
}

bool read_st_image(const uint8_t* d, size_t size, StDisk& out, std::string& why) {
    int cyl, sides, spt;
    if (!st_geometry(d, size, cyl, sides, spt)) {
        why = "not a plain ST disk image: " + std::to_string(size) + " bytes matches no standard geometry";
        return false;
    }
    if (size > kStMaxDiskBytes) { why = "ST image too large"; return false; }
    out = StDisk();
    out.format = StDisk::Format::St;
    out.cylinders = cyl;
    out.sides = sides;
    out.tracks.assign(size_t(cyl) * sides, StTrack());
    for (int c = 0; c < cyl; ++c)
        for (int s = 0; s < sides; ++s) {
            StTrack& t = out.track(c, s);
            t.sectors.resize(size_t(spt));
            for (int n = 0; n < spt; ++n) {
                StSector& sec = t.sectors[size_t(n)];
                sec.cylinder = uint8_t(c), sec.side = uint8_t(s), sec.number = uint8_t(n + 1), sec.size_code = 2;
                const size_t off = ((size_t(c) * sides + s) * spt + size_t(n)) * kStSectorBytes;
                sec.data.assign(d + off, d + off + kStSectorBytes);
            }
        }
    return true;
}

bool read_msa(const uint8_t* d, size_t size, StDisk& out, std::string& why, size_t max_bytes) {
    if (size < 10 || be16(d) != 0x0E0F) { why = "not an MSA image"; return false; }
    const unsigned spt = be16(d + 2), sides_field = be16(d + 4), first = be16(d + 6), last = be16(d + 8);
    if (spt < 1 || spt > kStMaxSectorsPerTrack || sides_field > 1 || first > last || last >= unsigned(kStMaxCylinders)) {
        why = "MSA header out of range";
        return false;
    }
    const int sides = int(sides_field) + 1;
    const size_t track_bytes = size_t(spt) * kStSectorBytes;
    if ((size_t(last) + 1) * size_t(sides) * track_bytes > max_bytes) { why = "MSA image too large"; return false; }
    StDisk disk;
    disk.format = StDisk::Format::Msa;
    disk.cylinders = int(last) + 1;
    disk.sides = sides;
    disk.tracks.assign(size_t(disk.cylinders) * sides, StTrack());
    size_t pos = 10;
    for (unsigned c = first; c <= last; ++c)
        for (int s = 0; s < sides; ++s) {
            if (pos + 2 > size) { why = "MSA image is cut short (track " + std::to_string(c) + ")"; return false; }
            const size_t len = be16(d + pos);
            pos += 2;
            if (len > size - pos || len > track_bytes) { why = "MSA track " + std::to_string(c) + " has a bad length"; return false; }
            std::vector<uint8_t> raw;
            raw.reserve(track_bytes);
            if (len == track_bytes) {
                raw.assign(d + pos, d + pos + len);
            } else {
                size_t i = 0;
                while (i < len) {
                    if (d[pos + i] == 0xE5) {
                        if (i + 4 > len) { why = "MSA track " + std::to_string(c) + ": cut-short run"; return false; }
                        const uint8_t v = d[pos + i + 1];
                        const size_t n = be16(d + pos + i + 2);
                        if (raw.size() + n > track_bytes) { why = "MSA track " + std::to_string(c) + " unpacks to more than a track"; return false; }
                        raw.insert(raw.end(), n, v);
                        i += 4;
                    } else {
                        if (raw.size() + 1 > track_bytes) { why = "MSA track " + std::to_string(c) + " unpacks to more than a track"; return false; }
                        raw.push_back(d[pos + i]);
                        ++i;
                    }
                }
            }
            pos += len;
            if (raw.size() != track_bytes) { why = "MSA track " + std::to_string(c) + " unpacks to " + std::to_string(raw.size()) + " bytes, not a track"; return false; }
            StTrack& t = disk.track(int(c), s);
            t.sectors.resize(spt);
            for (unsigned n = 0; n < spt; ++n) {
                StSector& sec = t.sectors[n];
                sec.cylinder = uint8_t(c), sec.side = uint8_t(s), sec.number = uint8_t(n + 1), sec.size_code = 2;
                sec.data.assign(raw.begin() + ptrdiff_t(n * kStSectorBytes), raw.begin() + ptrdiff_t((n + 1) * kStSectorBytes));
            }
        }
    out = std::move(disk);
    return true;
}

bool read_stx(const uint8_t* d, size_t size, StDisk& out, std::string& why, size_t max_bytes) {
    if (size < 16 || std::memcmp(d, "RSY\0", 4) != 0) { why = "not a Pasti (STX) image"; return false; }
    const int version = le16(d + 4);
    const unsigned track_count = d[10];
    struct Rec {
        int cyl, side;
        StTrack track;
    };
    std::vector<Rec> recs;
    size_t off = 16, held = 0;
    std::map<int, bool> seen;
    for (unsigned r = 0; r < track_count; ++r) {
        if (off + 16 > size) { why = "STX image is cut short (track record " + std::to_string(r) + ")"; return false; }
        const size_t rs = le32(d + off), fuzzy = le32(d + off + 4);
        const unsigned sc = le16(d + off + 8), flags = le16(d + off + 10), tn = d[off + 14];
        if (rs < 16 || rs > size - off) { why = "STX track record " + std::to_string(r) + " has a bad size"; return false; }
        if (sc > kStMaxSectorsPerTrack) { why = "STX track record " + std::to_string(r) + " has too many sectors"; return false; }
        const size_t desc_end = off + 16 + size_t(sc) * 16;
        if (desc_end > off + rs) { why = "STX track record " + std::to_string(r) + ": sector descriptors run past the record"; return false; }
        if (fuzzy > off + rs - desc_end) { why = "STX track record " + std::to_string(r) + ": fuzzy mask runs past the record"; return false; }
        Rec rec;
        rec.cyl = int(tn & 0x7F), rec.side = int(tn >> 7);
        rec.track.flags = uint16_t(flags);
        rec.track.fuzzy_bytes = uint32_t(fuzzy);
        const size_t data_base = desc_end + fuzzy;
        for (unsigned i = 0; i < sc; ++i) {
            const uint8_t* p = d + off + 16 + size_t(i) * 16;
            StSector s;
            const size_t data_off = le32(p);
            s.bit_position = le16(p + 4);
            s.read_time = le16(p + 6);
            s.cylinder = p[8], s.side = p[9], s.number = p[10], s.size_code = p[11];
            s.crc = le16(p + 12);
            s.fdc = p[14];
            if (s.size_code > 3) { why = "STX track record " + std::to_string(r) + ": sector size code " + std::to_string(s.size_code); return false; }
            const size_t n = s.size();
            if (!s.not_found()) {
                // The data must lie inside the file (and a sector of a well-formed record inside the record).
                if (data_off > size || data_base > size || data_base + data_off > size || n > size - (data_base + data_off)) {
                    why = "STX track record " + std::to_string(r) + ": sector data runs past the end of the file";
                    return false;
                }
                if (held + n > max_bytes) { why = "STX image holds more than " + std::to_string(max_bytes >> 20) + " MB of sector data"; return false; }
                held += n;
                s.data.assign(d + data_base + data_off, d + data_base + data_off + n);
            }
            rec.track.sectors.push_back(std::move(s));
        }
        if (sc > 0) {
            if (rec.cyl >= kStMaxCylinders) { why = "STX track record " + std::to_string(r) + ": cylinder " + std::to_string(rec.cyl) + " is out of range"; return false; }
            const int key = rec.cyl * 2 + rec.side;
            if (seen[key]) { why = "STX image has two records for cylinder " + std::to_string(rec.cyl) + " side " + std::to_string(rec.side); return false; }
            seen[key] = true;
            recs.push_back(std::move(rec));
        }
        off += rs;
    }
    if (recs.empty()) { why = "STX image holds no sectors"; return false; }
    int cylinders = 0, sides = 1;
    for (const Rec& r : recs) {
        cylinders = std::max(cylinders, r.cyl + 1);
        sides = std::max(sides, r.side + 1);
    }
    StDisk disk;
    disk.format = StDisk::Format::Stx;
    disk.stx_version = version;
    disk.cylinders = cylinders;
    disk.sides = sides;
    disk.tracks.assign(size_t(cylinders) * sides, StTrack());
    for (Rec& r : recs) disk.track(r.cyl, r.side) = std::move(r.track);
    out = std::move(disk);
    return true;
}

int StDisk::sectors_per_track() const {
    std::map<int, int> hist;
    for (const StTrack& t : tracks) {
        int n = 0;
        for (const StSector& s : t.sectors)
            if (s.size_code == 2) ++n;
        if (n > 0) ++hist[n];
    }
    int best = 0, best_count = 0;
    for (const auto& [n, count] : hist)
        if (count > best_count || (count == best_count && n > best)) best = n, best_count = count;
    return best;
}

bool StDisk::logical_image(int spt, std::vector<uint8_t>& out, std::vector<std::string>& problems) const {
    out.assign(size_t(cylinders) * sides * size_t(spt) * kStSectorBytes, 0);
    problems.clear();
    size_t total = 0;
    auto note = [&](int c, int s, const std::string& what) {
        ++total;
        if (problems.size() < 32) problems.push_back("cylinder " + std::to_string(c) + " side " + std::to_string(s) + ": " + what);
    };
    for (int c = 0; c < cylinders; ++c)
        for (int s = 0; s < sides; ++s) {
            const StTrack& t = track(c, s);
            std::vector<bool> have(size_t(spt) + 1, false);
            for (const StSector& sec : t.sectors) {
                if (sec.size_code != 2 || sec.number < 1 || sec.number > spt) {
                    note(c, s, "extra or odd sector " + std::to_string(sec.number) + " (" + std::to_string(sec.size()) + " bytes)");
                    continue;
                }
                if (have[sec.number]) { note(c, s, "sector " + std::to_string(sec.number) + " appears twice"); continue; }
                have[sec.number] = true;
                if (sec.data.size() == kStSectorBytes) {
                    const size_t off = ((size_t(c) * sides + s) * spt + size_t(sec.number - 1)) * kStSectorBytes;
                    std::memcpy(out.data() + off, sec.data.data(), kStSectorBytes);
                }
                if (sec.not_found()) note(c, s, "sector " + std::to_string(sec.number) + " record not found");
                else if (sec.crc_error()) note(c, s, "sector " + std::to_string(sec.number) + " CRC error");
                else if (sec.data.size() != kStSectorBytes) note(c, s, "sector " + std::to_string(sec.number) + " has no data");
            }
            for (int n = 1; n <= spt; ++n)
                if (!have[size_t(n)]) note(c, s, "sector " + std::to_string(n) + " missing");
        }
    if (total > problems.size()) problems.push_back("... and " + std::to_string(total - problems.size()) + " more");
    return total == 0;
}

std::vector<std::string> StDisk::nonstandard_tracks(int spt, size_t limit, size_t* total) const {
    std::vector<std::string> lines;
    size_t count = 0;
    for (int c = 0; c < cylinders; ++c)
        for (int s = 0; s < sides; ++s) {
            const StTrack& t = track(c, s);
            std::string what;
            std::vector<bool> have(size_t(spt) + 1, false);
            size_t clean = 0;
            for (const StSector& sec : t.sectors) {
                if (sec.size_code != 2 || sec.number < 1 || sec.number > spt || have[sec.number]) {
                    what += (what.empty() ? "" : ", ") + std::string("odd sector ") + std::to_string(sec.number);
                    continue;
                }
                have[sec.number] = true;
                if (sec.crc_error()) what += (what.empty() ? "" : ", ") + std::string("sector ") + std::to_string(sec.number) + " CRC error";
                else if (sec.not_found()) what += (what.empty() ? "" : ", ") + std::string("sector ") + std::to_string(sec.number) + " not found";
                else ++clean;
            }
            if (t.sectors.empty()) what = "no sectors";
            else if (what.empty() && clean != size_t(spt)) what = std::to_string(clean) + " of " + std::to_string(spt) + " sectors";
            if (t.fuzzy_bytes) what += (what.empty() ? "" : ", ") + std::string("weak-bit mask (") + std::to_string(t.fuzzy_bytes) + " bytes)";
            if (what.empty()) continue;
            ++count;
            if (lines.size() < limit) lines.push_back("cylinder " + std::to_string(c) + " side " + std::to_string(s) + ": " + what);
        }
    if (total) *total = count;
    return lines;
}

} // namespace x2::import
