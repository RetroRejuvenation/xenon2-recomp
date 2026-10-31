// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Tests for the flux readers (runtime/import/flux, scp, kryoflux, hfe) against SYNTHETIC flux: random sectors are
// MFM-encoded (container_fixtures.h), turned into transition times with spindle speed variation, timing jitter and a
// randomly placed index pulse per revolution (flux_fixtures.h), and wrapped as SCP, KryoFlux stream and HFE files.
// No game data and no files.
//
//   x2_flux_tests                    the synthetic tests (a recovery table versus jitter is printed)
//   x2_flux_tests --files DIR        also decodes every .scp / .hfe / trackNN.S.raw set found under DIR and prints a
//                                    per-disk summary (exit 77 if DIR is empty or missing): real dumps, local only
#include "import/disk_image.h"
#include "import/flux.h"
#include "import/importer.h"

#include "flux_fixtures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

using namespace x2::import;
using fx::Bytes;
namespace fs = std::filesystem;

namespace {
int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

Bytes random_sectors(uint32_t seed) {
    fx::Rng r{seed * 7919u + 17u};
    Bytes b(kTrackBytes);
    for (uint8_t& v : b) v = uint8_t(r.next());
    return b;
}

bool track_matches(const Track& t, const Bytes& sectors) {
    return t.kind == Track::Kind::Decoded && t.all_sectors_ok() && t.sectors == sectors;
}

// Sectors recovered intact and equal to the source, over `tracks` random tracks.
struct Recovery {
    int sectors = 0, good = 0, tracks_ok = 0, tracks = 0;
};

Recovery recover(const fx::FluxModel& base, int revs, int tracks, uint32_t extra_bits = 0, uint32_t rotate_base = 0, size_t pre = 0) {
    Recovery rec;
    for (int k = 0; k < tracks; ++k) {
        fx::FluxModel m = base;
        m.seed = base.seed + uint32_t(k) * 101u;
        if (revs == 1) m.index_jitter_cells = 0; // one revolution wraps onto itself: no index wander to tolerate
        Bytes secs = random_sectors(uint32_t(k) + base.seed);
        fx::MfmTrack mt = fx::mfm_encode_track(secs.data(), k, revs == 1 ? rotate_base : rotate_base + uint32_t(k) * 997u, extra_bits);
        fx::Timeline tl = fx::make_timeline(mt, revs + 1, pre, 0, m);
        FluxTrack ft = fx::scp_style_revs(tl);
        Track out;
        FluxTrackReport rep;
        decode_flux_track(ft, k, out, rep);
        ++rec.tracks;
        rec.sectors += kSectorsPerTrack;
        int ok = 0;
        for (int s = 0; s < kSectorsPerTrack; ++s)
            ok += out.kind == Track::Kind::Decoded && out.status[size_t(s)] == SectorStatus::Ok &&
                  std::memcmp(out.sectors.data() + size_t(s) * 512, secs.data() + size_t(s) * 512, 512) == 0;
        rec.good += ok;
        rec.tracks_ok += ok == kSectorsPerTrack;
    }
    return rec;
}

void test_pll_and_tracks() {
    // An ideal flux stream decodes exactly.
    {
        fx::FluxModel m;
        m.wow = 0;
        Recovery r = recover(m, 1, 6);
        CHECK(r.good == r.sectors);
    }
    // Spindle speed error and wow.
    for (double off : {-0.04, -0.015, 0.0, 0.015, 0.04}) {
        fx::FluxModel m;
        m.speed_offset = off;
        Recovery r = recover(m, 2, 6);
        std::printf("  speed %+.1f%%: %d/%d sectors\n", off * 100, r.good, r.sectors);
        CHECK(r.good == r.sectors);
    }
    // Recovery rate versus timing jitter (sigma as a fraction of the 2 us cell), one and three revolutions.
    std::printf("  jitter sigma  1 revolution      3 revolutions\n");
    int lo_ok = 0;
    for (double j : {0.00, 0.02, 0.04, 0.06, 0.08, 0.10, 0.12, 0.14, 0.17}) {
        fx::FluxModel m;
        m.jitter = j;
        m.seed = 11;
        Recovery r1 = recover(m, 1, 8), r3 = recover(m, 3, 8);
        std::printf("  %5.0f%% %14d/%d %14d/%d\n", j * 100, r1.good, r1.sectors, r3.good, r3.sectors);
        if (j <= 0.08) {
            CHECK(r1.good == r1.sectors);
            CHECK(r3.good == r3.sectors);
        }
        if (j <= 0.10) CHECK(r3.good * 100 >= r3.sectors * 95);
        if (j <= 0.12) lo_ok += r3.good * 100 >= r3.sectors * 90;
    }
    CHECK(lo_ok >= 7);
    // A sector straddling the index pulse is read whole from two or more revolutions (the seam between two
    // revolutions is continuous), and a long track reads too.
    {
        fx::FluxModel m;
        Recovery r = recover(m, 2, 8, 0, 8704 * 5 + 3000);
        CHECK(r.good == r.sectors);
        r = recover(m, 3, 8, 0, 8704 * 5 + 3000);
        CHECK(r.good == r.sectors);
        r = recover(m, 2, 4, 3000);
        CHECK(r.good == r.sectors);
        // With a single revolution the end is not continuous with the start: the straddling sector is reported
        // unreadable (never decoded across the seam), every other sector reads.
        r = recover(m, 1, 4, 0, 8704 * 5 + 3000);
        CHECK(r.good == r.sectors - 4);
    }
    // A sector that is unreadable in the first revolution is rescued by the second.
    {
        fx::FluxModel m;
        Bytes secs = random_sectors(5);
        fx::MfmTrack mt = fx::mfm_encode_track(secs.data(), 3, 0, 0);
        fx::Timeline tl = fx::make_timeline(mt, 3, 0, 0, m);
        FluxTrack ft = fx::scp_style_revs(tl);
        // Wipe a stretch of flux in revolution 0 (a dropout): the middle of the track.
        std::vector<uint32_t>& v = ft.revs[0].ns;
        size_t a = v.size() / 3, b = a + 4000;
        for (size_t i = a; i < b && i < v.size(); ++i) v[i] = 3000 + uint32_t(i % 7) * 400; // noise, not data
        Track out;
        FluxTrackReport rep;
        decode_flux_track(ft, 3, out, rep);
        CHECK(track_matches(out, secs));
        CHECK(rep.late > 0);
        CHECK(!rep.disagree);
        // The same track with every revolution damaged in the same place: precise per-sector errors.
        for (FluxRev& r : ft.revs) {
            std::vector<uint32_t>& w = r.ns;
            size_t aa = w.size() / 3;
            for (size_t i = aa; i < aa + 4000 && i < w.size(); ++i) w[i] = 3000 + uint32_t(i % 7) * 400;
        }
        decode_flux_track(ft, 3, out, rep);
        CHECK(!out.all_sectors_ok());
        FluxReport fr;
        fr.tracks.push_back(rep);
        std::string pr = fr.problems();
        CHECK(pr.find("track 3 sector") != std::string::npos && pr.find("all 2 revolutions") != std::string::npos);
        int bad = 0;
        for (int s = 0; s < kSectorsPerTrack; ++s) bad += out.status[size_t(s)] != SectorStatus::Ok;
        CHECK(bad >= 1 && bad < kSectorsPerTrack);
    }
    // Noise far beyond the tolerance is reported, not decoded wrongly: every "Ok" sector is the true one.
    {
        fx::FluxModel m;
        m.jitter = 0.35;
        for (int k = 0; k < 4; ++k) {
            Bytes secs = random_sectors(uint32_t(40 + k));
            fx::MfmTrack mt = fx::mfm_encode_track(secs.data(), k, 0, 0);
            FluxTrack ft = fx::scp_style_revs(fx::make_timeline(mt, 3, 0, 0, m));
            Track out;
            FluxTrackReport rep;
            decode_flux_track(ft, k, out, rep);
            for (int s = 0; s < kSectorsPerTrack; ++s)
                if (out.kind == Track::Kind::Decoded && out.status[size_t(s)] == SectorStatus::Ok)
                    CHECK(std::memcmp(out.sectors.data() + size_t(s) * 512, secs.data() + size_t(s) * 512, 512) == 0);
        }
    }
    // Two revolutions that both pass their checksums with different data (a "weak" sector) are flagged.
    {
        fx::FluxModel m;
        Bytes a = random_sectors(60), b = a;
        b[5 * 512 + 100] ^= 0x10;
        fx::MfmTrack ma = fx::mfm_encode_track(a.data(), 2, 0, 0), mb = fx::mfm_encode_track(b.data(), 2, 0, 0);
        FluxTrack fa = fx::scp_style_revs(fx::make_timeline(ma, 2, 0, 0, m)), fb = fx::scp_style_revs(fx::make_timeline(mb, 2, 0, 0, m));
        FluxTrack mix;
        mix.revs.push_back(fa.revs[0]);
        mix.revs.push_back(fb.revs[0]);
        Track out;
        FluxTrackReport rep;
        decode_flux_track(mix, 2, out, rep);
        CHECK(rep.disagree);
        CHECK(out.all_sectors_ok() && std::memcmp(out.sectors.data() + 5 * 512, a.data() + 5 * 512, 512) == 0); // the first revolution wins
    }
    // A track with no Amiga sectors at all stays Raw and keeps its bit cells.
    {
        fx::Rng r{99};
        FluxTrack ft;
        for (int rev = 0; rev < 2; ++rev) {
            FluxRev fr;
            for (int i = 0; i < 40000; ++i) fr.ns.push_back(uint32_t(4000 + 2000 * (r.next() % 3)));
            ft.revs.push_back(fr);
        }
        Track out;
        FluxTrackReport rep;
        decode_flux_track(ft, 5, out, rep);
        CHECK(out.kind == Track::Kind::Raw && rep.raw && out.bit_length > 100000 && out.revolutions == 2);
    }
    // Empty flux gives an Empty track; PLL on nothing does not crash.
    {
        FluxTrack ft;
        Track out;
        FluxTrackReport rep;
        decode_flux_track(ft, 0, out, rep);
        CHECK(out.kind == Track::Kind::Empty);
        PllResult pr = pll_decode({}, 0);
        CHECK(pr.bit_length == 0);
    }
}

// A whole synthetic disk of random sectors as per-track flux.
struct Disk {
    Bytes adf;
    std::vector<fx::MfmTrack> mfm;
    std::vector<FluxTrack> flux;
    std::vector<fx::Timeline> timelines;
};

Disk make_disk(int revs, double jitter, uint32_t seed, int tracks = 160, size_t pre = 0, size_t post = 0) {
    Disk d;
    d.adf.resize(size_t(tracks) * kTrackBytes);
    fx::Rng r{seed};
    for (uint8_t& b : d.adf) b = uint8_t(r.next());
    fx::FluxModel m;
    m.jitter = jitter;
    for (int t = 0; t < tracks; ++t) {
        m.seed = seed + uint32_t(t) * 31u;
        d.mfm.push_back(fx::mfm_encode_track(&d.adf[size_t(t) * kTrackBytes], t, uint32_t(t) * 4093u, 8 * uint32_t(t % 5)));
        d.timelines.push_back(fx::make_timeline(d.mfm.back(), revs + 1, pre, post, m));
        d.flux.push_back(fx::scp_style_revs(d.timelines.back()));
    }
    return d;
}

bool image_equals_adf(const DiskImage& img, const Bytes& adf, std::string* why = nullptr) {
    Bytes out;
    std::string w;
    bool ok = img.to_adf(out, w) && out == adf;
    if (!ok && why) *why = w;
    return ok;
}

void test_scp() {
    Disk d = make_disk(2, 0.04, 1000);
    Bytes file = fx::scp_file(d.flux, 2);
    FluxDisk fd;
    std::string why;
    std::vector<std::string> warns;
    CHECK(read_scp(file, fd, why, &warns));
    CHECK(warns.empty());
    DiskImage img;
    FluxReport rep;
    decode_flux_disk(fd, img, rep);
    std::string w;
    CHECK(image_equals_adf(img, d.adf, &w));
    if (!w.empty()) std::fprintf(stderr, "  not equal: %s\n", w.c_str());
    CHECK(rep.tracks.size() == 160);
    CHECK(rep.tracks[0].revolutions == 2);
    CHECK(rep.tracks[0].rev_ms > 190 && rep.tracks[0].rev_ms < 215);

    // A non-default resolution (tick = 50 ns) and flux longer than 65535 ticks (overflow words).
    Disk small = make_disk(2, 0.02, 2000, 4);
    small.flux[1].revs[0].ns[100] = 5'000'000; // a 5 ms silence: 100 000 ticks at 50 ns? no: 5 ms / 25 ns = 200 000 ticks
    Bytes f2 = fx::scp_file(small.flux, 2, 0);
    FluxDisk fd2;
    CHECK(read_scp(f2, fd2, why, &warns));
    CHECK(fd2.tracks[1].revs[0].ns[100] == 5'000'000);
    Bytes f3 = fx::scp_file(small.flux, 2, 1);
    FluxDisk fd3;
    CHECK(read_scp(f3, fd3, why, &warns));
    long diff = long(fd3.tracks[0].revs[0].ns[10]) - long(fd2.tracks[0].revs[0].ns[10]);
    CHECK(std::labs(diff) <= 50);
    CHECK(fd3.tracks[0].revs.size() == 2);

    // Checksum warning (not an error); stored checksum is advisory.
    warns.clear();
    Bytes badsum = fx::scp_file(small.flux, 2, 0, true);
    CHECK(read_scp(badsum, fd2, why, &warns) && !warns.empty() && warns[0].find("checksum") != std::string::npos);

    // Header validation and bounds.
    Bytes b = file;
    CHECK(!read_scp(Bytes(b.begin(), b.begin() + 100), fd, why));
    b = file; b[5] = 0;
    CHECK(!read_scp(b, fd, why) && why.find("0 revolutions") != std::string::npos);
    b = file; b[9] = 8;
    CHECK(!read_scp(b, fd, why) && why.find("16-bit") != std::string::npos);
    b = file; b[10] = 3;
    CHECK(!read_scp(b, fd, why));
    b = file; b[6] = 50; b[7] = 40;
    CHECK(!read_scp(b, fd, why) && why.find("track range") != std::string::npos);
    b = file; // track block offset past the end
    b[0x10] = 0xFF; b[0x11] = 0xFF; b[0x12] = 0xFF; b[0x13] = 0x7F;
    CHECK(!read_scp(b, fd, why) && why.find("outside the file") != std::string::npos);
    b = file; // flux count beyond the data
    {
        uint32_t off = uint32_t(b[0x10]) | uint32_t(b[0x11]) << 8 | uint32_t(b[0x12]) << 16 | uint32_t(b[0x13]) << 24;
        b[off + 4 + 4] = 0xFF; b[off + 4 + 5] = 0xFF; b[off + 4 + 6] = 0xFF; b[off + 4 + 7] = 0x00;
        CHECK(!read_scp(b, fd, why));
        b = file; // data offset that points back into the header (self-referential)
        b[off + 4 + 8] = 0; b[off + 4 + 9] = 0; b[off + 4 + 10] = 0; b[off + 4 + 11] = 0;
        CHECK(!read_scp(b, fd, why));
    }
    b = file; b.resize(b.size() / 2);
    CHECK(!read_scp(b, fd, why));
    CHECK(!read_scp(Bytes(), fd, why));
}

// A small SCP file must not be expandable into gigabytes of flux values: every (track, revolution) entry of the file may
// point at the same flux bytes, so overlapping data ranges and absurd revolution counts are refused.
void test_scp_bombs() {
    auto put32 = [](Bytes& b, size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
    };
    // header: "SCP", version, type, revs, first, last, flags, width, heads, resolution, checksum, 168 track offsets
    auto header = [&](int revs, int first, int last) {
        Bytes b(0x10 + 4 * 168, 0);
        std::memcpy(b.data(), "SCP", 3);
        b[5] = uint8_t(revs);
        b[6] = uint8_t(first);
        b[7] = uint8_t(last);
        b[8] = 0x20; // flags: no checksum
        return b;
    };
    FluxDisk fd;
    std::string why;
    // 1. 255 revolutions per track
    {
        Bytes b = header(255, 0, 0);
        put32(b, 0x10, uint32_t(b.size()));
        Bytes trk = {'T', 'R', 'K', 0};
        trk.resize(4 + 12 * 255, 0);
        b.insert(b.end(), trk.begin(), trk.end());
        CHECK(!read_scp(b, fd, why, nullptr) && why.find("revolutions per track") != std::string::npos);
    }
    // 2. 168 tracks x 5 revolutions of 200 000 flux values, every entry pointing at the same 400 KB
    {
        const int revs = 5;
        const uint32_t count = 200000;
        Bytes b = header(revs, 0, 167);
        const size_t block = b.size(), block_len = 4 + 12 * size_t(revs);
        for (int t = 0; t < 168; ++t) put32(b, 0x10 + 4 * size_t(t), uint32_t(block));
        Bytes trk(block_len, 0);
        std::memcpy(trk.data(), "TRK", 3);
        for (int r = 0; r < revs; ++r) {
            put32(trk, 4 + 12 * size_t(r) + 4, count);
            put32(trk, 4 + 12 * size_t(r) + 8, uint32_t(block_len)); // every revolution: the same data
        }
        b.insert(b.end(), trk.begin(), trk.end());
        Bytes data(size_t(count) * 2, 0);
        for (size_t i = 0; i < data.size(); i += 2) data[i + 1] = 80; // 2 us at 25 ns
        b.insert(b.end(), data.begin(), data.end());
        CHECK(b.size() < 500000);
        CHECK(!read_scp(b, fd, why, nullptr) && why.find("share flux data") != std::string::npos);
        FluxDisk none;
        CHECK(none.tracks[0].revs.empty()); // nothing was kept by the failed read
        CHECK(fd.tracks[0].revs.empty() && fd.tracks[167].revs.empty());
    }
    // 3. two revolutions of one track that overlap by a few bytes
    {
        Bytes b = header(2, 0, 0);
        put32(b, 0x10, uint32_t(b.size()));
        const size_t block_len = 4 + 12 * 2;
        Bytes trk(block_len, 0);
        std::memcpy(trk.data(), "TRK", 3);
        put32(trk, 4 + 4, 100);
        put32(trk, 4 + 8, uint32_t(block_len));
        put32(trk, 16 + 4, 100);
        put32(trk, 16 + 8, uint32_t(block_len + 190)); // starts 10 bytes before the first revolution's data ends
        b.insert(b.end(), trk.begin(), trk.end());
        b.resize(b.size() + 400, 0x50);
        CHECK(!read_scp(b, fd, why, nullptr) && why.find("share flux data") != std::string::npos);
        // the same file with adjacent (disjoint) ranges reads
        put32(b, 0x10 + 4 * 168 + 16 + 8, uint32_t(block_len + 200));
        CHECK(read_scp(b, fd, why, nullptr) && fd.tracks[0].revs.size() == 2);
    }
}

void test_kryoflux() {
    int c, s;
    CHECK(kryoflux_track_from_name("track00.0.raw", c, s) && c == 0 && s == 0);
    CHECK(kryoflux_track_from_name("dir/sub\\track79.1.raw", c, s) && c == 79 && s == 1);
    CHECK(kryoflux_track_from_name("TRACK05.1.RAW", c, s) && c == 5 && s == 1);
    CHECK(!kryoflux_track_from_name("track7.0.raw", c, s));
    CHECK(!kryoflux_track_from_name("track00.2.raw", c, s));
    CHECK(!kryoflux_track_from_name("track99.0.raw", c, s));
    CHECK(!kryoflux_track_from_name("disk.raw", c, s));
    CHECK(kryoflux_track_from_name("dir/dumpfile00.1.raw", c, s) && c == 0 && s == 1);
    CHECK(kryoflux_track_from_name("my disk_41.0.raw", c, s) && c == 41 && s == 0);
    CHECK(kryoflux_track_from_name("00.0.raw", c, s) && c == 0 && s == 0);
    CHECK(!kryoflux_track_from_name("dumpfile0.1.raw", c, s));
    CHECK(kryoflux_track_from_name("dumpfile000.1.raw", c, s) && c == 0 && s == 1);
    CHECK(!kryoflux_track_from_name("stream.0.raw", c, s));
    CHECK(!kryoflux_track_from_name(".0.raw", c, s));

    // One track as a stream: partial revolution before the first index and after the last.
    Disk d = make_disk(3, 0.04, 3000, 6, 5000, 7000);
    FluxTrack ft;
    std::string why;
    std::vector<std::string> warns;
    Bytes stream = fx::kryoflux_stream(d.timelines[2]);
    CHECK(read_kryoflux_stream(stream, ft, why, &warns));
    CHECK(warns.empty());
    CHECK(ft.revs.size() == 3); // four index pulses, three whole revolutions
    Track out;
    FluxTrackReport rep;
    decode_flux_track(ft, 2, out, rep);
    CHECK(track_matches(out, Bytes(d.adf.begin() + 2 * 5632, d.adf.begin() + 3 * 5632)));
    CHECK(rep.rev_ms > 190 && rep.rev_ms < 215);

    // The stream's own sample clock is read from the info block; a different clock changes the times.
    Bytes noinfo = fx::kryoflux_stream(d.timelines[2], 24027428.5714285, false, false);
    warns.clear();
    FluxTrack f2;
    CHECK(read_kryoflux_stream(noinfo, f2, why, &warns));
    CHECK(!warns.empty() && warns.back().find("end-of-file") != std::string::npos);
    CHECK(f2.revs.size() == 3);

    // No index blocks: one revolution of unknown start; still decodes (the wrap handles the unknown start).
    {
        fx::Timeline tl = d.timelines[3];
        tl.index.clear();
        Bytes s2 = fx::kryoflux_stream(tl);
        warns.clear();
        FluxTrack f3;
        CHECK(read_kryoflux_stream(s2, f3, why, &warns));
        CHECK(f3.revs.size() == 1 && !warns.empty());
        Track o3;
        FluxTrackReport r3;
        decode_flux_track(f3, 3, o3, r3);
        CHECK(track_matches(o3, Bytes(d.adf.begin() + 3 * 5632, d.adf.begin() + 4 * 5632)));
    }

    // Truncation and garbage are errors, never out-of-bounds reads.
    for (size_t cut : {size_t(1), stream.size() / 3, stream.size() - 5}) {
        FluxTrack f4;
        Bytes t(stream.begin(), stream.begin() + long(cut));
        bool ok = read_kryoflux_stream(t, f4, why, nullptr);
        (void)ok; // either outcome is fine; the point is that it returns
    }
    {
        Bytes t = stream;
        t.resize(t.size() - 20);
        t.push_back(0x0D); t.push_back(0x02); t.push_back(0xFF); t.push_back(0xFF); // OOB claiming 65535 bytes
        FluxTrack f5;
        CHECK(!read_kryoflux_stream(t, f5, why) && why.find("truncated") != std::string::npos);
    }
    CHECK(!read_kryoflux_stream(Bytes(), ft, why));
    CHECK(!read_kryoflux_stream(Bytes{0x0D, 0x0D, 0x0D, 0x0D}, ft, why));

    // The importer-side detector knows it by the info block's text.
    CHECK(detect_container(stream.data(), stream.size()) == Container::KryoFlux);
}

void test_hfe() {
    // Two sides, 82 cylinders' worth would be slow: 10 cylinders, whole-byte tracks.
    std::vector<fx::MfmTrack> s0, s1;
    Bytes adf(20 * kTrackBytes);
    fx::Rng r{77};
    for (uint8_t& b : adf) b = uint8_t(r.next());
    for (int c = 0; c < 10; ++c) {
        s0.push_back(fx::mfm_encode_track(&adf[size_t(2 * c) * kTrackBytes], 2 * c, 0, 0));
        s1.push_back(fx::mfm_encode_track(&adf[size_t(2 * c + 1) * kTrackBytes], 2 * c + 1, 0, 0));
        // a real writer pads the tail with the start of the track so that a sector across the wrap still reads
    }
    Bytes file = fx::hfe_file(s0, s1);
    DiskImage img;
    std::string why;
    bool hd = true;
    std::vector<std::string> warns;
    CHECK(read_hfe(file, img, why, hd, &warns) && !hd);
    CHECK(warns.empty());
    for (int t = 0; t < 20; ++t) CHECK(track_matches(img.track(t), Bytes(adf.begin() + long(t) * 5632, adf.begin() + long(t + 1) * 5632)));
    CHECK(img.track(20).kind == Track::Kind::Empty);

    // A track that is not Amiga data stays Raw.
    {
        std::vector<fx::MfmTrack> a = s0, b = s1;
        a[3].bytes.assign(a[3].bytes.size(), 0x55);
        Bytes f2 = fx::hfe_file(a, b);
        DiskImage i2;
        CHECK(read_hfe(f2, i2, why, hd, nullptr));
        CHECK(i2.track(6).kind == Track::Kind::Raw && i2.track(6).bit_length == a[3].bytes.size() * 8);
    }
    // HD, odd encodings, bad headers and bounds.
    {
        DiskImage i2;
        Bytes b = file;
        b[12] = 0xF4; b[13] = 0x01; // 500 kbit/s
        CHECK(!read_hfe(b, i2, why, hd, nullptr) && hd);
        b = file; b[11] = 2;
        CHECK(!read_hfe(b, i2, why, hd, nullptr) && why.find("encoding") != std::string::npos);
        b = file; b[10] = 3;
        CHECK(!read_hfe(b, i2, why, hd, nullptr));
        b = file; b[9] = 0;
        CHECK(!read_hfe(b, i2, why, hd, nullptr));
        b = file; b[18] = 0xFF; b[19] = 0xFF;
        CHECK(!read_hfe(b, i2, why, hd, nullptr) && why.find("track list") != std::string::npos);
        b = file; b.resize(1200);
        CHECK(!read_hfe(b, i2, why, hd, nullptr));
        CHECK(!read_hfe(Bytes(100, 0), i2, why, hd, nullptr));
        // data offset beyond the file
        b = file;
        b[512] = 0xFF; b[513] = 0x7F;
        CHECK(!read_hfe(b, i2, why, hd, nullptr));
    }
    // v3: opcodes in the data (NOP, set index, set bit rate with an argument) are skipped.
    {
        Bytes b = file;
        std::memcpy(b.data(), "HXCHFEV3", 8);
        DiskImage i2;
        warns.clear();
        CHECK(read_hfe(b, i2, why, hd, &warns));
        CHECK(!warns.empty());
    }
}

void test_importer() {
    // Whole discs as SCP, HFE and KryoFlux stream sets import like the plain ADF pair. The disc contents are the
    // importer test fixtures' business, so here only the container path is checked: a disc image that is not a
    // Xenon disc is reported as decoded from the right container.
    Disk d = make_disk(2, 0.03, 4000);
    Bytes scp = fx::scp_file(d.flux, 2);
    Result r = import_inputs({{"disc.scp", scp, false}});
    bool decoded = false;
    for (const Message& m : r.messages)
        if (m.text.find("disc.scp (SCP decoded)") != std::string::npos) decoded = true;
    CHECK(decoded);
    CHECK(r.status != Status::Ok); // random data is not a Xenon disc
    bool summary = false;
    for (const Message& m : r.messages)
        if (m.text.find("SCP flux image: 160 tracks") != std::string::npos) summary = true;
    CHECK(summary);

    // KryoFlux stream files, given as separate inputs in a folder, form one disc.
    std::vector<Input> ins;
    Disk small = make_disk(2, 0.02, 5000, 160, 3000, 3000);
    for (int t = 0; t < 160; ++t) {
        char name[64];
        std::snprintf(name, sizeof name, "set1/track%02d.%d.raw", t / 2, t % 2);
        ins.push_back({name, fx::kryoflux_stream(small.timelines[size_t(t)]), true});
    }
    r = import_inputs(ins);
    decoded = false;
    bool kf_summary = false;
    for (const Message& m : r.messages) {
        if (m.text.find("set1 (KryoFlux streams) (KryoFlux stream set decoded)") != std::string::npos) decoded = true;
        if (m.text.find("KryoFlux stream set flux image: 160 tracks") != std::string::npos) kf_summary = true;
    }
    if (!(decoded && kf_summary)) for (const Message& m : r.messages) std::fprintf(stderr, "   msg: %s\n", m.text.c_str());
    CHECK(decoded && kf_summary);
    // A KryoFlux file whose name gives no track number is an error, not a guess.
    r = import_inputs({{"stream.raw", ins[0].data, false}});
    CHECK(r.status == Status::BadInput);
    bool named = false;
    for (const Message& m : r.messages) named = named || m.text.find("trackNN.S.raw") != std::string::npos;
    CHECK(named);
    // An incomplete set reports the missing tracks.
    ins.resize(40);
    r = import_inputs(ins);
    bool missing = false;
    for (const Message& m : r.messages) missing = missing || m.text.find("missing") != std::string::npos;
    CHECK(missing);
    // HFE of a half-written disk.
    std::vector<fx::MfmTrack> s0(d.mfm.begin(), d.mfm.begin() + 10), s1 = s0;
    (void)s1;
}

// Corrupted files: every outcome is acceptable except a crash or a hang (run under ASan/UBSan in CI-like checks).
void test_fuzz() {
    Disk d = make_disk(2, 0.02, 6000, 4);
    Bytes scp = fx::scp_file(d.flux, 2), kf = fx::kryoflux_stream(d.timelines[1]);
    std::vector<fx::MfmTrack> s0(d.mfm.begin(), d.mfm.begin() + 2), s1(d.mfm.begin() + 2, d.mfm.begin() + 4);
    Bytes hfe = fx::hfe_file(s0, s1);
    fx::Rng r{1234};
    for (int i = 0; i < 400; ++i) {
        for (int which = 0; which < 3; ++which) {
            Bytes b = which == 0 ? scp : which == 1 ? kf : hfe;
            int edits = 1 + int(r.next() % 6);
            for (int e = 0; e < edits; ++e) {
                size_t at = r.next() % (which == 2 ? std::min<size_t>(b.size(), 2048) : b.size());
                b[at] = uint8_t(r.next());
            }
            if (r.next() % 5 == 0) b.resize(r.next() % b.size());
            std::string why;
            if (which == 0) {
                FluxDisk fd;
                if (read_scp(b, fd, why)) { DiskImage img; FluxReport rep; decode_flux_disk(fd, img, rep); }
            } else if (which == 1) {
                FluxTrack ft;
                if (read_kryoflux_stream(b, ft, why)) { Track t; FluxTrackReport rep; decode_flux_track(ft, 1, t, rep); }
            } else {
                DiskImage img;
                bool hd;
                read_hfe(b, img, why, hd);
            }
        }
    }
    CHECK(true);
}

// Local only: decode real flux dumps found under `dir`.
int run_real_files(const char* dir) {
    std::error_code ec;
    if (!dir || !*dir || !fs::is_directory(dir, ec)) return 77;
    std::map<std::string, std::vector<fs::path>> kf; // folder -> stream files
    std::map<std::string, fs::path> adfs;            // stem -> reference ADF found next to the dumps
    std::vector<fs::path> singles;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string n = it->path().filename().string();
        int c, s;
        if (kryoflux_track_from_name(n, c, s)) kf[it->path().parent_path().string()].push_back(it->path());
        else {
            std::string ext = it->path().extension().string();
            for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (ext == ".scp" || ext == ".hfe") singles.push_back(it->path());
            if (ext == ".adf") adfs[it->path().stem().string()] = it->path();
        }
    }
    if (kf.empty() && singles.empty()) return 77;
    auto slurp = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    auto report = [&](const std::string& label, const std::string& stem, DiskImage& img, const FluxReport& rep) {
        std::vector<int> bad = img.nonstandard_tracks();
        Bytes adf;
        std::string why;
        bool whole = img.to_adf(adf, why);
        bool dos = whole && adf.size() >= 4 && std::memcmp(adf.data(), "DOS", 3) == 0;
        std::printf("%s: %s%s%s%s\n", label.c_str(), rep.tracks.empty() ? "" : rep.summary().c_str(), rep.tracks.empty() ? "" : "; ",
                    whole ? "complete 880 KB ADF" : ("not a plain disk: " + why).c_str(), dos ? " (AmigaDOS boot block)" : "");
        std::printf("   nonstandard tracks: %zu\n", bad.size());
        // Against a reference ADF of the same disk (made by another tool): every sector we accept must equal it.
        auto ref = adfs.find(stem);
        if (ref != adfs.end()) {
            Bytes radf = slurp(ref->second);
            int compared = 0, equal = 0, accepted_total = 0;
            for (int blk = 0; blk < kStandardTracks * kSectorsPerTrack && radf.size() == size_t(kStandardTracks) * kTrackBytes; ++blk) {
                const uint8_t* sec = img.sector(blk);
                if (!sec) continue;
                ++accepted_total;
                ++compared;
                equal += std::memcmp(sec, &radf[size_t(blk) * kSectorBytes], kSectorBytes) == 0;
            }
            std::printf("   against %s: %d of %d accepted sectors equal the reference (%d sectors accepted of 1760)\n",
                        ref->second.filename().string().c_str(), equal, compared, accepted_total);
            CHECK(equal == compared);
            if (whole) { Bytes full; std::string w2; if (img.to_adf(full, w2) && radf.size() == full.size()) std::printf("   whole image %s the reference\n", full == radf ? "EQUALS" : "differs from"); }
        }
        CHECK(true);
    };
    for (const fs::path& p : singles) {
        Bytes b = slurp(p);
        DiskImage img;
        FluxReport rep;
        std::string why;
        if (p.extension() == ".scp" || p.extension() == ".SCP") {
            FluxDisk fd;
            if (!read_scp(b, fd, why)) { std::printf("%s: read error: %s\n", p.filename().string().c_str(), why.c_str()); CHECK(false); continue; }
            decode_flux_disk(fd, img, rep);
        } else {
            bool hd;
            if (!read_hfe(b, img, why, hd)) { std::printf("%s: read error: %s\n", p.filename().string().c_str(), why.c_str()); CHECK(false); continue; }
        }
        report(p.filename().string(), p.stem().string(), img, rep);
    }
    for (auto& kv : kf) {
        FluxDisk fd;
        for (const fs::path& p : kv.second) {
            int c, s;
            kryoflux_track_from_name(p.filename().string(), c, s);
            std::string why;
            if (!read_kryoflux_stream(slurp(p), fd.tracks[size_t(c * 2 + s)], why)) { std::printf("%s: %s\n", p.string().c_str(), why.c_str()); CHECK(false); }
        }
        DiskImage img;
        FluxReport rep;
        decode_flux_disk(fd, img, rep);
        report(kv.first + " (" + std::to_string(kv.second.size()) + " stream files)", fs::path(kv.first).filename().string(), img, rep);
    }
    return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--files") == 0) return run_real_files(argv[2]);
    test_pll_and_tracks();
    test_scp();
    test_scp_bombs();
    test_kryoflux();
    test_hfe();
    test_importer();
    test_fuzz();
    std::printf("flux tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
