// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// FluxEngine's native ".flux" files (the format of FluxEngine and of tools that write it). A protobuf stream:
//   field 1 (varint)   magic 0x466C7578 ("Flux")        field 2 (varint)  version (2)
//   field 3 (bytes), repeated: a track { 1: cylinder (varint), 2: head (varint), 3: flux data (bytes) }
// The flux data is a byte stream: bit 7 = a transition, bit 6 = the index pulse, bits 0-5 = ticks of 1/12 us (83.33 ns);
// a byte of exactly 0x3F adds 63 ticks and continues the same interval. The index pulses cut the stream into revolutions;
// what lies before the first and after the last pulse is not a whole revolution and is dropped. Layout verified on a real
// dump (docs/re-notes/acquisitions-flux-acorn-st.md); only the fields above are read.
#include "import/flux.h"

#include <cstring>

namespace x2::import {

namespace {

constexpr uint64_t kMagic = 0x466C7578;
constexpr double kTickNs = 1000.0 / 12.0;
constexpr size_t kMaxTrackBlob = 4u << 20;      // a revolution is ~50,000 transitions; 3 revolutions fit in a few hundred KB
constexpr size_t kMaxTotalPulses = size_t(64) << 20;
constexpr int kMaxRevs = 32;

struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    bool varint(uint64_t& v) {
        v = 0;
        for (int s = 0; s < 70; s += 7) {
            if (i >= n) return false;
            uint8_t b = p[i++];
            v |= uint64_t(b & 0x7F) << s;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
};

// Skips a field of wire type `wt` (the value after the key).
bool skip(Reader& r, uint64_t wt) {
    uint64_t v;
    switch (wt) {
    case 0: return r.varint(v);
    case 1: if (r.n - r.i < 8) return false; r.i += 8; return true;
    case 2: if (!r.varint(v) || v > r.n - r.i) return false; r.i += size_t(v); return true;
    case 5: if (r.n - r.i < 4) return false; r.i += 4; return true;
    default: return false;
    }
}

} // namespace

bool is_fluxengine(const uint8_t* d, size_t n) {
    if (n < 8 || d[0] != 0x08) return false;
    Reader r{d, n, 1};
    uint64_t v;
    return r.varint(v) && v == kMagic;
}

bool read_fluxengine(const std::vector<uint8_t>& data, FluxDisk& out, std::string& why, std::vector<std::string>* warnings) {
    out = FluxDisk();
    out.source = "FluxEngine";
    if (!is_fluxengine(data.data(), data.size())) { why = "not a FluxEngine .flux file"; return false; }
    Reader r{data.data(), data.size(), 1};
    uint64_t v;
    r.varint(v); // the magic
    uint64_t version = 0;
    size_t total = 0;
    int tracks_read = 0;
    while (r.i < r.n) {
        uint64_t key;
        if (!r.varint(key)) { why = "FluxEngine file: truncated field key"; return false; }
        const uint64_t field = key >> 3, wt = key & 7;
        if (field == 2 && wt == 0) { if (!r.varint(version)) { why = "FluxEngine file: truncated version"; return false; } continue; }
        if (!(field == 3 && wt == 2)) {
            if (!skip(r, wt)) { why = "FluxEngine file: malformed field"; return false; }
            continue;
        }
        uint64_t len;
        if (!r.varint(len) || len > r.n - r.i) { why = "FluxEngine file: a track runs past the end of the file"; return false; }
        Reader t{r.p + r.i, size_t(len), 0};
        r.i += size_t(len);
        uint64_t cyl = ~0ull, head = ~0ull;
        const uint8_t* blob = nullptr;
        size_t blob_len = 0;
        while (t.i < t.n) {
            uint64_t k;
            if (!t.varint(k)) { why = "FluxEngine file: malformed track"; return false; }
            const uint64_t f = k >> 3, w = k & 7;
            if (w == 0 && (f == 1 || f == 2)) {
                uint64_t x;
                if (!t.varint(x)) { why = "FluxEngine file: malformed track"; return false; }
                (f == 1 ? cyl : head) = x;
            } else if (w == 2 && f == 3) {
                uint64_t l;
                if (!t.varint(l) || l > t.n - t.i) { why = "FluxEngine file: malformed flux data"; return false; }
                blob = t.p + t.i;
                blob_len = size_t(l);
                t.i += blob_len;
            } else if (!skip(t, w)) {
                why = "FluxEngine file: malformed track";
                return false;
            }
        }
        if (cyl == ~0ull || head == ~0ull || !blob) continue;
        if (cyl > 83 || head > 1) { if (warnings) warnings->push_back("FluxEngine file: a track beyond cylinder 83 or head 1 was ignored"); continue; }
        if (blob_len > kMaxTrackBlob) { why = "FluxEngine file: implausibly long flux data for one track"; return false; }
        std::vector<uint32_t> pulses;
        std::vector<size_t> index; // number of pulses before each index mark
        uint64_t acc = 0;
        for (size_t j = 0; j < blob_len; ++j) {
            uint8_t b = blob[j];
            if (b == 0x3F) { acc += 63; continue; }
            acc += b & 0x3F;
            if (b & 0x80) {
                double ns = double(acc) * kTickNs;
                pulses.push_back(ns > 4e9 ? 4000000000u : uint32_t(ns + 0.5));
                acc = 0;
            }
            if (b & 0x40) index.push_back(pulses.size());
        }
        total += pulses.size();
        if (total > kMaxTotalPulses) { why = "FluxEngine file: more than 64 million flux transitions in all"; return false; }
        FluxTrack& ft = out.tracks[size_t(cyl * 2 + head)];
        for (size_t k = 0; k + 1 < index.size() && int(ft.revs.size()) < kMaxRevs; ++k) {
            FluxRev rev;
            rev.ns.assign(pulses.begin() + long(index[k]), pulses.begin() + long(index[k + 1]));
            if (!rev.ns.empty()) ft.revs.push_back(std::move(rev));
        }
        if (!ft.revs.empty()) ++tracks_read;
    }
    if (version != 2 && warnings) warnings->push_back("FluxEngine file of version " + std::to_string(version) + " (version 2 is the one this reader was checked against)");
    if (tracks_read == 0) { why = "FluxEngine file holds no complete revolution"; return false; }
    return true;
}

} // namespace x2::import
