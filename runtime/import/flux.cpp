// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The flux-to-bit-cell PLL and the per-track, multi-revolution sector recovery (flux.h, docs/re-notes/flux.md).
#include "import/flux.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x2::import {

namespace {

struct BitSink {
    std::vector<uint8_t> bytes;
    uint32_t n = 0;
    void put(int bit) {
        if ((n & 7) == 0) bytes.push_back(0);
        if (bit) bytes.back() = uint8_t(bytes.back() | (0x80 >> (n & 7)));
        ++n;
    }
};

const char* status_text(SectorStatus s) {
    switch (s) {
    case SectorStatus::Ok: return "intact";
    case SectorStatus::BadData: return "data checksum error";
    case SectorStatus::BadHeader: return "header checksum error";
    case SectorStatus::Missing: return "not found";
    }
    return "?";
}

int rank(SectorStatus s) { // higher is better evidence of a readable sector
    switch (s) {
    case SectorStatus::Ok: return 3;
    case SectorStatus::BadData: return 2;
    case SectorStatus::BadHeader: return 1;
    case SectorStatus::Missing: return 0;
    }
    return 0;
}

} // namespace

PllResult pll_decode(const std::vector<uint32_t>& intervals, size_t own_count, const PllParams& p) {
    PllResult r;
    BitSink sink;
    double T = p.nominal_ns, bnd = 0, t = 0, sum_T = 0;
    const double lo = p.nominal_ns * (1.0 - p.range), hi = p.nominal_ns * (1.0 + p.range);
    size_t i = 0;
    for (uint32_t d : intervals) {
        if (i++ == own_count) r.own_bits = sink.n;
        if (d == 0) continue;
        t += d;
        double x = (t - bnd) / T;
        int n = int(std::floor(x + 0.5));
        if (n < p.run_min) n = p.run_min;
        if (n > 12 && p.run_max >= 12) {
            // Silence far longer than any MFM gap: no flux to follow. Emit a bounded run of empty cells and restart
            // the loop on this transition.
            ++r.long_gaps;
            for (int k = 1; k < 12; ++k) sink.put(0);
            sink.put(1);
            bnd = t;
            continue;
        }
        if (n > p.run_max) n = p.run_max;
        double err = t - (bnd + n * T);
        for (int k = 1; k < n; ++k) sink.put(0);
        sink.put(1);
        bnd += n * T + p.phase_gain * err;
        T += p.freq_gain * err / n;
        T = std::min(hi, std::max(lo, T));
        sum_T += T;
    }
    if (own_count >= intervals.size()) r.own_bits = sink.n;
    r.bits = std::move(sink.bytes);
    r.bit_length = sink.n;
    r.mean_cell_ns = intervals.empty() ? 0 : sum_T / double(intervals.size());
    return r;
}

namespace {

// The bit cells of revolution `ri` (own revolution plus the start of the next one, see decode_flux_track).
PllResult revolution_bits(const std::vector<const FluxRev*>& revs, size_t ri, const PllParams& p, bool with_overlap) {
    const FluxRev& cur = *revs[ri];
    std::vector<uint32_t> ext = cur.ns;
    size_t own_count = ext.size();
    // The next revolution follows this one without a break (one capture, index pulse to index pulse), so the interval
    // that spans the pulse is this revolution's tail plus the next one's head, and a sector across the pulse reads whole.
    // The last revolution has nothing after it: its end is not continuous with its own start, so it is decoded as a
    // linear stream and a sector across its pulse is left to the other revolutions.
    if (with_overlap && ri + 1 < revs.size()) {
        const FluxRev& next = *revs[ri + 1];
        double acc = 0;
        bool first = true;
        for (uint32_t v : next.ns) {
            uint32_t w = v;
            if (first) { w = uint32_t(std::min<double>(double(v) + cur.tail_ns, 4e9)); first = false; }
            ext.push_back(w);
            acc += v;
            if (acc >= p.overlap_ns) break;
        }
    }
    return pll_decode(ext, own_count, p);
}

// The retry pass: a PLL that insists on valid MFM run lengths (2 to 4 cells) at a few loop gains. Phase tracking with that
// clamp read weak media that the plain PLL could not (docs/re-notes/flux.md).
constexpr struct { double phase, freq; } kRetry[] = {{0.1, 0.01}, {0.2, 0.0}, {0.3, 0.05}, {0.1, 0.0}};

PllParams retry_params(const PllParams& p, size_t i) {
    PllParams q = p;
    q.phase_gain = kRetry[i].phase;
    q.freq_gain = kRetry[i].freq;
    q.run_min = 2;
    q.run_max = 4;
    return q;
}

} // namespace

void decode_flux_track(const FluxTrack& ft, int track_number, Track& out, FluxTrackReport& rep, const PllParams& p) {
    out = Track();
    rep = FluxTrackReport();
    rep.track = track_number;
    rep.best.fill(SectorStatus::Missing);
    std::vector<const FluxRev*> revs;
    for (const FluxRev& r : ft.revs)
        if (!r.ns.empty()) revs.push_back(&r);
    rep.revolutions = int(revs.size());
    if (revs.empty()) return; // Empty

    out.kind = Track::Kind::Decoded;
    out.sectors.assign(kTrackBytes, 0);
    out.status.fill(SectorStatus::Missing);
    out.revolutions = uint8_t(std::min<size_t>(revs.size(), 255));

    std::vector<Track::Stream> own_streams; // each revolution's own bit cells, for a Raw fallback
    double rev_ns_sum = 0, cell_sum = 0;
    std::array<bool, kSectorsPerTrack> have{}, ok_first{};
    for (size_t ri = 0; ri < revs.size(); ++ri) {
        const FluxRev& cur = *revs[ri];
        double own_ns = cur.tail_ns;
        for (uint32_t v : cur.ns) own_ns += v;
        rev_ns_sum += own_ns;
        PllResult pr = revolution_bits(revs, ri, p, true);
        cell_sum += pr.mean_cell_ns;
        if (own_streams.size() < size_t(kAltRevolutions) + 1) {
            Track::Stream st;
            st.bit_length = pr.own_bits;
            st.bits.assign(pr.bits.begin(), pr.bits.begin() + long((pr.own_bits + 7) / 8));
            own_streams.push_back(std::move(st));
        }
        Track tmp;
        decode_amiga_mfm_track(pr.bits.data(), pr.bit_length, track_number, tmp, false);
        for (int s = 0; s < kSectorsPerTrack; ++s) {
            size_t si = size_t(s);
            if (rank(tmp.status[si]) > rank(rep.best[si])) rep.best[si] = tmp.status[si];
            if (tmp.status[si] != SectorStatus::Ok) continue;
            ++rep.ok_revs[si];
            if (ri == 0) ok_first[si] = true;
            const uint8_t* src = tmp.sectors.data() + si * kSectorBytes;
            uint8_t* dst = out.sectors.data() + si * kSectorBytes;
            if (!have[si]) {
                std::memcpy(dst, src, kSectorBytes);
                have[si] = true;
            } else if (std::memcmp(dst, src, kSectorBytes) != 0) {
                rep.disagree = true;
            }
        }
    }
    auto count_ok = [&] {
        int n = 0;
        for (bool b : have) n += b;
        return n;
    };
    bool any_sector_seen = false;
    for (int s = 0; s < kSectorsPerTrack; ++s) any_sector_seen = any_sector_seen || rep.best[size_t(s)] != SectorStatus::Missing;

    // Retry pass. Only for a track that is Amiga formatted (some sector was seen) and only for the sectors that are still
    // not intact: a sector that passed its checksums is never touched, and a sector is only ever accepted on its own
    // header and data checksums (no repair). A track that stayed Raw is not retried here (its decoder gets the streams).
    if (any_sector_seen && count_ok() < kSectorsPerTrack) {
        for (size_t k = 0; k < sizeof kRetry / sizeof kRetry[0] && count_ok() < kSectorsPerTrack; ++k) {
            const PllParams q = retry_params(p, k);
            for (size_t ri = 0; ri < revs.size() && count_ok() < kSectorsPerTrack; ++ri) {
                PllResult pr = revolution_bits(revs, ri, q, true);
                Track tmp;
                decode_amiga_mfm_track(pr.bits.data(), pr.bit_length, track_number, tmp, false);
                for (int s = 0; s < kSectorsPerTrack; ++s) {
                    size_t si = size_t(s);
                    if (have[si] || tmp.status[si] != SectorStatus::Ok) continue;
                    std::memcpy(out.sectors.data() + si * kSectorBytes, tmp.sectors.data() + si * kSectorBytes, kSectorBytes);
                    have[si] = true;
                    rep.best[si] = SectorStatus::Ok;
                    ++rep.retried;
                }
            }
        }
    }

    int ok = 0;
    for (int s = 0; s < kSectorsPerTrack; ++s) {
        if (have[size_t(s)]) {
            out.status[size_t(s)] = SectorStatus::Ok;
            ++ok;
            if (!ok_first[size_t(s)]) ++rep.late;
        }
        else out.status[size_t(s)] = rep.best[size_t(s)];
    }
    rep.rev_ms = rev_ns_sum / double(revs.size()) / 1e6;
    rep.mean_cell_ns = cell_sum / double(revs.size());
    if (ok == 0 && !any_sector_seen) {
        // Not a standard Amiga track (or unreadable). Keep the bit cells so a protection check or a later decoder
        // can use them: the first revolution as the track, the others and clamped-PLL versions of all of them as
        // alternates.
        rep.raw = true;
        out = Track();
        out.kind = Track::Kind::Raw;
        out.mfm = std::move(own_streams[0].bits);
        out.bit_length = own_streams[0].bit_length;
        out.revolutions = uint8_t(std::min<size_t>(revs.size(), 255));
        for (size_t i = 1; i < own_streams.size() && out.alt.size() < kMaxAltStreams; ++i) out.alt.push_back(std::move(own_streams[i]));
        for (size_t k = 0; k < sizeof kRetry / sizeof kRetry[0]; ++k) {
            const PllParams q = retry_params(p, k);
            for (size_t ri = 0; ri < revs.size() && ri < size_t(kAltRevolutions) && out.alt.size() < kMaxAltStreams; ++ri) {
                PllResult pr = revolution_bits(revs, ri, q, false);
                Track::Stream st;
                st.bit_length = pr.own_bits;
                st.bits.assign(pr.bits.begin(), pr.bits.begin() + long((pr.own_bits + 7) / 8));
                out.alt.push_back(std::move(st));
            }
        }
    }
}

void merge_flux_disks(FluxDisk& fd, const FluxDisk& other) {
    for (size_t t = 0; t < fd.tracks.size() && t < other.tracks.size(); ++t) {
        FluxTrack& dst = fd.tracks[t];
        for (const FluxRev& r : other.tracks[t].revs)
            if (dst.revs.size() < 32) dst.revs.push_back(r);
    }
    fd.source += " + " + other.source;
}

void decode_flux_disk(const FluxDisk& fd, DiskImage& out, FluxReport& rep, const PllParams& p) {
    out = DiskImage();
    rep = FluxReport();
    for (int t = 0; t < kMaxTracks; ++t) {
        const FluxTrack& ft = fd.tracks[size_t(t)];
        bool any = false;
        for (const FluxRev& r : ft.revs) any = any || !r.ns.empty();
        if (!any) continue;
        FluxTrackReport tr;
        decode_flux_track(ft, t, out.track(t), tr, p);
        if (tr.disagree) ++rep.disagreements;
        rep.recovered_late += tr.late;
        rep.recovered_retry += tr.retried;
        rep.tracks.push_back(tr);
    }
}

std::string FluxReport::problems(size_t max_items) const {
    std::string s;
    size_t shown = 0, total = 0;
    for (const FluxTrackReport& t : tracks) {
        if (t.track >= kStandardTracks || t.raw) continue;
        for (int sec = 0; sec < kSectorsPerTrack; ++sec) {
            if (t.best[size_t(sec)] == SectorStatus::Ok) continue;
            ++total;
            if (shown >= max_items) continue;
            if (!s.empty()) s += "; ";
            s += "track " + std::to_string(t.track) + " sector " + std::to_string(sec) + ": " + status_text(t.best[size_t(sec)]) +
                 " in " + (t.revolutions == 1 ? "the only revolution" : "all " + std::to_string(t.revolutions) + " revolutions");
            ++shown;
        }
    }
    if (total > shown) s += "; and " + std::to_string(total - shown) + " more";
    return s;
}

std::string FluxReport::summary() const {
    int raw = 0, max_revs = 0;
    for (const FluxTrackReport& t : tracks) {
        raw += t.raw;
        max_revs = std::max(max_revs, t.revolutions);
    }
    std::string s = std::to_string(tracks.size()) + " tracks, " + std::to_string(max_revs) +
                    (max_revs == 1 ? " revolution" : " revolutions") + " each";
    if (raw) s += ", " + std::to_string(raw) + " not standard Amiga tracks (kept raw)";
    if (recovered_late) s += ", " + std::to_string(recovered_late) + " sectors rescued by a later revolution";
    if (recovered_retry) s += ", " + std::to_string(recovered_retry) + " rescued by a second PLL pass limited to valid MFM runs";
    if (disagreements) s += ", " + std::to_string(disagreements) + " tracks where revolutions disagree";
    return s;
}

} // namespace x2::import
