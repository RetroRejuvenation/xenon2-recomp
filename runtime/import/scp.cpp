// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// SuperCard Pro (SCP) flux images. Layout (SuperCard Pro image format specification, cbmstuff.com):
//
//   0x00  "SCP"                                 0x08  flags      bit 0 index-aligned, bit 1 96 TPI, bit 2 360 RPM,
//   0x03  version                                                 bit 3 normalised, bit 4 read/write, bit 5 footer
//   0x04  disk type                             0x09  cell width (0 = 16-bit values)
//   0x05  revolutions per track                 0x0A  heads (0 both, 1 side 0 only, 2 side 1 only)
//   0x06  first track                           0x0B  resolution (tick = 25 ns * (value + 1))
//   0x07  last track                            0x0C  checksum, 32-bit sum of every byte from 0x10 to the end
//   0x10  168 x 32-bit little-endian offsets of the track blocks (0 = no data)
//
// A track block is "TRK", the track number, then one 12-byte entry per revolution (index time, number of flux
// values, offset of the values from the start of the block; all little-endian 32-bit), and the values: big-endian
// 16-bit tick counts, where 0 means "add 65536 and carry on with the next value". Verified against real dumps
// (docs/re-notes/flux.md): the first value of each revolution after the first spans the index pulse.
#include "import/flux.h"

#include <algorithm>
#include <cstring>

namespace x2::import {

namespace {
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
constexpr size_t kMaxFluxPerRev = 8u << 20; // far beyond any real track (a DD revolution has ~50,000 transitions)
} // namespace

bool read_scp(const std::vector<uint8_t>& d, FluxDisk& out, std::string& why, std::vector<std::string>* warnings) {
    out = FluxDisk();
    out.source = "SCP";
    if (d.size() < 0x10 + 4 * 168 || std::memcmp(d.data(), "SCP", 3) != 0) {
        why = "not an SCP image (too short or no SCP header)";
        return false;
    }
    int revs = d[5], first = d[6], last = d[7], flags = d[8], width = d[9], heads = d[10], resolution = d[11];
    if (revs < 1) { why = "SCP header says 0 revolutions per track"; return false; }
    if (width != 0) { why = "SCP image with " + std::to_string(width) + "-bit flux values (only the standard 16-bit form is read)"; return false; }
    if (heads > 2) { why = "SCP header names an unknown head layout (" + std::to_string(heads) + ")"; return false; }
    if (first > last || last >= 168) { why = "SCP header track range " + std::to_string(first) + ".." + std::to_string(last) + " is invalid"; return false; }
    const double tick_ns = 25.0 * double(resolution + 1);
    if (warnings) {
        uint32_t stored = rd32(&d[12]), sum = 0;
        for (size_t i = 0x10; i < d.size(); ++i) sum += d[i];
        if (stored != sum && !(flags & 0x20)) warnings->push_back("SCP checksum does not match the file contents (the image may be damaged)");
        if (flags & 0x04) warnings->push_back("SCP image of a 360 RPM drive (a 5.25 inch high-density disk?)");
    }
    int tracks_read = 0;
    for (int t = first; t <= last; ++t) {
        uint32_t off = rd32(&d[0x10 + 4 * size_t(t)]);
        if (off == 0) continue;
        if (size_t(off) + 4 + 12 * size_t(revs) > d.size() || std::memcmp(&d[off], "TRK", 3) != 0) {
            why = "SCP track " + std::to_string(t) + ": block offset " + std::to_string(off) + " is outside the file or has no TRK header";
            return false;
        }
        int track_number;
        if (heads == 0) track_number = t;               // cylinder * 2 + side, as the format numbers a two-sided disk
        else if (heads == 1) track_number = t * 2;      // single-sided image: track number is the cylinder
        else track_number = t * 2 + 1;
        if (track_number >= kMaxTracks) { why = "SCP track " + std::to_string(t) + " is beyond cylinder 83"; return false; }
        FluxTrack& ft = out.tracks[size_t(track_number)];
        for (int r = 0; r < revs; ++r) {
            const uint8_t* e = &d[size_t(off) + 4 + 12 * size_t(r)];
            uint32_t count = rd32(e + 4), doff = rd32(e + 8); // (e + 0: the index time, a revolution time only)
            if (count > kMaxFluxPerRev) { why = "SCP track " + std::to_string(t) + ": implausible flux count " + std::to_string(count); return false; }
            size_t start = size_t(off) + doff, bytes = size_t(count) * 2;
            if (start < size_t(off) + 4 + 12 * size_t(revs) || start > d.size() || bytes > d.size() - start) {
                why = "SCP track " + std::to_string(t) + " revolution " + std::to_string(r) + ": flux data runs outside the file";
                return false;
            }
            FluxRev rev;
            rev.ns.reserve(count);
            double carry = 0;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t v = uint32_t(d[start + 2 * i]) << 8 | d[start + 2 * i + 1];
                if (v == 0) { carry += 65536.0; continue; }
                double ns = (carry + v) * tick_ns;
                carry = 0;
                if (ns > 4e9) ns = 4e9;
                rev.ns.push_back(uint32_t(ns + 0.5));
            }
            // The first value of every revolution after the first already holds the whole interval that spans the index
            // pulse (the time since the last transition before it): real captures show first values that are already
            // plausible MFM intervals (<= 8 us) where "index time minus the sum of the values" would push them past 12 us.
            // So there is no separate tail to add (tail_ns stays 0).
            ft.revs.push_back(std::move(rev));
        }
        ++tracks_read;
    }
    if (tracks_read == 0) { why = "SCP image holds no track data"; return false; }
    return true;
}

} // namespace x2::import
