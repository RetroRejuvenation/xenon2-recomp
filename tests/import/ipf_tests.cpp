// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Tests for the IPF reader (runtime/import/ipf) with synthetic IPF files built by ipf_fixtures.h: no game data.
// Covers a whole disk of standard Amiga tracks, protection-style tracks that stay Raw, noise tracks, every kind of
// damage the parser must reject (CRCs, record sizes, offsets, limits), and a mutation fuzz pass.
//
//   x2_ipf_tests                      the synthetic tests
//   x2_ipf_tests --ipf-dir DIR        also reads every .ipf under DIR (exit 77 if there are none): every record and
//                                     payload CRC must verify; the disk 2 images of the SPS 0297 and 2874 pressings
//                                     (known by the sha1 of the file) must decode to the canonical disc 2. Local only.
//   x2_ipf_tests --fuzz N [SEED]     N mutation-fuzz iterations (default run: 1200); meant for the sanitizer build.
//   x2_ipf_tests --adf-sha1 FILE.ipf  prints the sha1 of the plain ADF the IPF decodes to (when every track is standard).
//   x2_ipf_tests --report FILE.ipf    prints the header and one line per track in the format of tools/ipf.py, so the
//                                     two readers can be compared with diff.
#include "import/disk_image.h"
#include "import/ipf.h"
#include "import/sha1.h"

#include "ipf_fixtures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

constexpr size_t kAdf = 901120;

Bytes make_adf(uint32_t seed) {
    fx::Rng r{seed};
    Bytes adf(kAdf);
    for (size_t i = 0; i < kAdf; ++i) adf[i] = uint8_t(r.next() >> 3);
    return adf;
}

struct Parsed {
    bool ok = false;
    std::string why;
    DiskImage img;
    IpfDisk disk;
};

Parsed parse(const Bytes& b) {
    Parsed p;
    p.ok = read_ipf(b, p.img, p.disk, p.why);
    return p;
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// File offsets of the records of a synthetic IPF, in order.
struct Rec {
    size_t pos, size, payload;
    std::string tag;
};
std::vector<Rec> records(const Bytes& b) {
    std::vector<Rec> out;
    size_t pos = 0;
    while (b.size() - pos >= 12) {
        uint32_t tsize = uint32_t(b[pos + 4]) << 24 | uint32_t(b[pos + 5]) << 16 | uint32_t(b[pos + 6]) << 8 | b[pos + 7];
        if (tsize < 12 || b.size() - pos < tsize) break;
        size_t payload = 0;
        if (std::memcmp(&b[pos], "DATA", 4) == 0 && tsize == 28)
            payload = uint32_t(b[pos + 12]) << 24 | uint32_t(b[pos + 13]) << 16 | uint32_t(b[pos + 14]) << 8 | b[pos + 15];
        out.push_back({pos, tsize, payload, std::string(reinterpret_cast<const char*>(&b[pos]), 4)});
        pos += tsize + payload;
    }
    return out;
}

const Rec& nth(const std::vector<Rec>& r, const char* tag, int n) {
    int seen = 0;
    for (const Rec& x : r)
        if (x.tag == tag && seen++ == n) return x;
    std::fprintf(stderr, "no %s record %d\n", tag, n);
    std::abort();
}

// --- tests ---------------------------------------------------------------------------------------------------

void test_standard_disk() {
    Bytes adf = make_adf(0x1234567);
    Bytes ipf = fx::ipf_of_adf(adf);
    Parsed p = parse(ipf);
    CHECK(p.ok);
    CHECK(p.why.empty());
    CHECK(p.disk.info.release == 297 && p.disk.info.platform[0] == 1 && p.disk.info.encoder_type == 1);
    CHECK(p.disk.info.max_cylinder == 83 && p.disk.info.max_head == 1);
    CHECK(p.disk.tracks.size() == 160);
    bool all = p.disk.tracks.size() == 160;
    for (size_t i = 0; all && i < p.disk.tracks.size(); ++i) {
        const IpfTrackInfo& t = p.disk.tracks[i];
        all = all && t.number() == int(i) && t.standard && t.cells == fx::kIpfStandardTrackCells && t.blocks == 11 &&
              t.type == 2 && t.weak_cells == 0 && t.index_cells == 2300 + i && t.timing_ns == std::vector<uint16_t>{2000};
    }
    CHECK(all);
    bool decoded = true;
    for (int t = 0; t < kStandardTracks; ++t) decoded = decoded && p.img.track(t).kind == Track::Kind::Decoded && p.img.track(t).all_sectors_ok();
    CHECK(decoded);
    Bytes back;
    std::string why;
    CHECK(p.img.to_adf(back, why));
    CHECK(back == adf);
    CHECK(p.img.nonstandard_tracks().empty());
    CHECK(p.img.sector_sha1(7, 3) == sha1_hex(&adf[(7 * 11 + 3) * 512], 512));
    // the index position moves where the sectors start in the stream, not what they contain
    fx::IpfBuild other;
    other.standard_index_base = 50;
    Parsed q = parse(fx::ipf_of_adf(adf, other));
    Bytes back2;
    CHECK(q.ok && q.img.to_adf(back2, why) && back2 == adf);
    // only some tracks present: the rest are Empty and to_adf says so
    fx::IpfBuild few;
    few.tracks = 6;
    Parsed f = parse(fx::ipf_of_adf(adf, few));
    CHECK(f.ok && f.disk.tracks.size() == 6);
    CHECK(!f.img.to_adf(back2, why) && contains(why, "track 6"));
    CHECK(f.img.to_adf(back2, why, true) && back2.size() == kAdf && std::equal(adf.begin(), adf.begin() + 6 * 5632, back2.begin()));
}

void test_protected_tracks() {
    Bytes adf = make_adf(0xBEEF);
    fx::IpfBuild cfg;
    cfg.raw_track = 3;
    cfg.noise_track = 5;
    Parsed p = parse(fx::ipf_of_adf(adf, cfg));
    CHECK(p.ok);
    CHECK(p.img.nonstandard_tracks() == (std::vector<int>{3, 5}));
    CHECK(p.img.track(3).kind == Track::Kind::Raw && p.img.track(3).bit_length == 100000);
    CHECK(p.img.track(3).mfm.size() == 12500);
    CHECK(p.img.track(5).kind == Track::Kind::Raw && p.img.track(5).bit_length == 100000);
    CHECK(p.disk.tracks[5].type == 1 && p.disk.tracks[5].weak_cells == 100000 && !p.disk.tracks[5].standard);
    CHECK(p.disk.tracks[3].cells == 100000 && !p.disk.tracks[3].standard && p.disk.tracks[3].index_cells == 3000);
    Bytes out;
    std::string why;
    CHECK(!p.img.to_adf(out, why) && contains(why, "track 3"));
    CHECK(p.img.track(4).kind == Track::Kind::Decoded && p.img.track(6).kind == Track::Kind::Decoded);
    // the raw track keeps its cells: original cell j sits at index (j + index_cells) of the rotated stream
    const Track& r = p.img.track(3);
    int match = 0;
    for (int i = 0; i < 64; ++i) {
        size_t k = (size_t(i) + 3000) % 100000;
        int expect = ((0x5A ^ uint8_t((i >> 3) * 37)) >> (7 - (i & 7))) & 1;
        match += ((r.mfm[k >> 3] >> (7 - (k & 7))) & 1) == expect ? 1 : 0;
    }
    CHECK(match == 64);
    // a retail-style key track is just another raw track as far as the reader goes: three such tracks
    fx::IpfBuild many;
    many.raw_track = 1;
    Parsed m = parse(fx::ipf_of_adf(adf, many));
    CHECK(m.ok && m.img.nonstandard_tracks() == std::vector<int>{1});
}

void test_errors() {
    Bytes adf = make_adf(77);
    fx::IpfBuild two;
    two.tracks = 2;
    Bytes good = fx::ipf_of_adf(adf, two);
    CHECK(parse(good).ok);
    auto recs = records(good);

    // empty, tiny, truncated
    CHECK(!parse(Bytes()).ok && contains(parse(Bytes()).why, "INFO"));
    CHECK(contains(parse(Bytes{'C', 'A', 'P', 'S', 0}).why, "truncated"));
    CHECK(!parse(Bytes(good.begin(), good.begin() + long(good.size() / 2))).ok);
    CHECK(!parse(Bytes(good.begin(), good.end() - 1)).ok);
    CHECK(contains(parse(Bytes(good.begin(), good.end() - 1)).why, "payload truncated"));
    for (size_t cut : {size_t(11), size_t(12), size_t(23), size_t(24), size_t(107)})
        CHECK(!parse(Bytes(good.begin(), good.begin() + long(cut))).ok);

    // CRC damage: a record body, a payload byte
    Bytes b = good;
    b[nth(recs, "INFO", 0).pos + 20] ^= 1;
    Parsed p = parse(b);
    CHECK(!p.ok && contains(p.why, "CRC error in the INFO record"));
    b = good;
    b[nth(recs, "DATA", 0).pos + 28 + 100] ^= 0x10;
    p = parse(b);
    CHECK(!p.ok && contains(p.why, "payload CRC"));
    b = good;
    b[nth(recs, "DATA", 1).pos + 5] ^= 0x01; // the record size field
    CHECK(!parse(b).ok);

    // record sizes
    b = good;
    fx::set32(b, nth(recs, "IMGE", 0).pos + 4, 8);
    p = parse(b);
    CHECK(!p.ok && contains(p.why, "bad record size"));
    b = good;
    fx::set32(b, nth(recs, "IMGE", 0).pos + 4, uint32_t(good.size() + 100));
    CHECK(contains(parse(b).why, "bad record size"));
    b = good;
    fx::set32(b, nth(recs, "INFO", 0).pos + 4, 100); // a size that is not the INFO size
    fx::ipf_fix_crcs(b);
    CHECK(!parse(b).ok);

    // semantic damage in IMGE/INFO, with CRCs repaired so the parser reaches the field checks
    auto with = [&](const char* tag, int n, size_t field, uint32_t v) {
        Bytes c = good;
        fx::set32(c, nth(recs, tag, n).pos + field, v);
        fx::ipf_fix_crcs(c);
        return parse(c);
    };
    CHECK(contains(with("INFO", 0, 12, 2).why, "unsupported INFO"));
    CHECK(contains(with("INFO", 0, 16, 3).why, "unsupported INFO"));
    CHECK(contains(with("INFO", 0, 20, 2).why, "unsupported INFO"));
    CHECK(contains(with("IMGE", 0, 12, 90).why, "geometry"));        // cylinder outside INFO's range
    CHECK(contains(with("IMGE", 0, 16, 2).why, "geometry"));         // head outside INFO's range
    CHECK(contains(with("IMGE", 0, 24, 2).why, "signal type"));
    CHECK(contains(with("IMGE", 0, 56, 1).why, "encoder process"));
    CHECK(contains(with("IMGE", 0, 64, 1001).why, "out of range"));
    CHECK(contains(with("DATA", 0, 24, 5000).why, "out of range"));
    CHECK(contains(with("IMGE", 0, 64, 7).why, "mismatch"));         // IMGE for index 7, DATA for index 0
    CHECK(contains(with("IMGE", 0, 48, 0x7FFFFFFFu).why, "could not rebuild")); // size_cells over the limit
    CHECK(contains(with("IMGE", 0, 36, fx::kIpfStandardTrackCells).why, "could not rebuild"));
    CHECK(contains(with("IMGE", 0, 52, 0xFFFFFFFFu).why, "could not rebuild")); // more blocks than the data holds
    CHECK(contains(with("IMGE", 0, 52, 0).why, "could not rebuild"));
    CHECK(contains(with("IMGE", 0, 20, 12).why, "could not rebuild")); // unknown track type
    // geometry past the disk model: INFO allows it, the DiskImage has no place for it
    {
        Bytes c = good;
        fx::set32(c, nth(recs, "INFO", 0).pos + 40, 200);
        fx::set32(c, nth(recs, "IMGE", 0).pos + 12, 100);
        fx::ipf_fix_crcs(c);
        CHECK(contains(parse(c).why, "84 cylinders"));
    }
    // IMGE before INFO, a file with no INFO, a data track without its IMGE
    {
        Bytes c = fx::ipf_caps();
        Bytes im = fx::ipf_imge({});
        c.insert(c.end(), im.begin(), im.end());
        CHECK(contains(parse(c).why, "before the INFO"));
        Bytes only_caps = fx::ipf_caps();
        CHECK(contains(parse(only_caps).why, "no INFO"));
        Bytes d = fx::ipf_caps(), in = fx::ipf_info(), da = fx::ipf_data(Bytes(64, 0), 0);
        d.insert(d.end(), in.begin(), in.end());
        d.insert(d.end(), da.begin(), da.end());
        CHECK(contains(parse(d).why, "mismatch"));
    }

    // damage in the DATA payload (the block descriptors and the element streams)
    size_t payload0 = nth(recs, "DATA", 0).pos + 28;
    auto patch_payload = [&](size_t at, uint32_t v, bool word) {
        Bytes c = good;
        if (word) fx::set32(c, payload0 + at, v);
        else c[payload0 + at] = uint8_t(v);
        fx::ipf_fix_crcs(c);
        return parse(c);
    };
    CHECK(contains(patch_payload(16, 7, true).why, "could not rebuild"));      // block encoder 7
    CHECK(contains(patch_payload(0, 9000, true).why, "could not rebuild"));    // data cells that do not fit the elements
    CHECK(contains(patch_payload(28, 0, true).why, "could not rebuild"));      // data stream offset pointing at the descriptors
    CHECK(contains(patch_payload(28, 0xFFFFFFF0u, true).why, "could not rebuild")); // ... outside the payload
    CHECK(contains(patch_payload(4, 0xFFFFFFF0u, true).why, "could not rebuild")); // a gap larger than the track
    CHECK(patch_payload(20, 3, true).ok);                                         // flags are ignored for encoder type 1
    {
        // the first element of block 0: op 1 (sync) with a size byte count of 7 and nothing behind it
        Parsed q = patch_payload(32 * 11, 0xE1, false);
        CHECK(!q.ok && contains(q.why, "could not rebuild"));
        q = patch_payload(32 * 11, 0x1F, false); // an unknown op
        CHECK(!q.ok);
        q = patch_payload(32 * 11 + 1, 0xFF, false); // a sync size larger than the data left
        CHECK(!q.ok);
    }
    // gap types 1, 2 and 3 pointing at garbage descriptions
    for (uint32_t gap_type : {1u, 2u, 3u}) {
        Bytes c = good;
        fx::set32(c, payload0 + 20 + 0, gap_type);
        fx::set32(c, payload0 + 8, 0xFFFFFF00u);
        fx::ipf_fix_crcs(c);
        // encoder type 1 ignores the flags word, so build the INFO with encoder type 2 to make it count
        fx::set32(c, nth(recs, "INFO", 0).pos + 16, 2);
        fx::ipf_fix_crcs(c);
        Parsed q = parse(c);
        CHECK(!q.ok);
        fx::set32(c, payload0 + 8, 32 * 11); // description inside the payload: still not a valid description
        fx::ipf_fix_crcs(c);
        CHECK(!parse(c).ok);
    }
}

// Mutates a small IPF at random places, repairing the CRCs so the damage reaches the track generator. The reader
// must neither crash nor hang (run under ASan/UBSan in the sanitizer build), and a success must be self-consistent.
void test_fuzz(int iterations = 1200, uint32_t seed = 0x5EEDu) {
    Bytes adf = make_adf(2024);
    fx::IpfBuild one;
    one.tracks = 1;
    Bytes base = fx::ipf_of_adf(adf, one);
    fx::Rng r{seed};
    int ok = 0, bad = 0;
    for (int iter = 0; iter < iterations; ++iter) {
        Bytes c = base;
        int n = 1 + int(r.next() % 4);
        for (int k = 0; k < n; ++k) {
            size_t at = r.next() % c.size();
            if (r.next() % 3 == 0) at = r.next() % 400; // headers and the block descriptors
            c[at] = uint8_t(r.next());
        }
        if (iter % 7 == 0) c.resize(r.next() % c.size());
        if (iter % 5 != 0) fx::ipf_fix_crcs(c);
        Parsed p = parse(c);
        if (p.ok) {
            ++ok;
            CHECK(p.disk.tracks.size() <= 1);
            for (const IpfTrackInfo& t : p.disk.tracks) CHECK(t.cells <= (1u << 20) && (t.cells == 0 || t.index_cells < t.cells));
        } else {
            ++bad;
            CHECK(!p.why.empty());
        }
    }
    std::printf("fuzz: %d accepted, %d rejected\n", ok, bad);
    CHECK(bad > 0);
}

// --- local, data-dependent -------------------------------------------------------------------------------------

std::string format_report(const Bytes& data, bool& ok) {
    Parsed p = parse(data);
    ok = p.ok;
    if (!p.ok) return "error: " + p.why + "\n";
    const IpfInfo& i = p.disk.info;
    char line[512];
    std::string out;
    std::snprintf(line, sizeof line, "IPF release %u revision %u, encoder %u, cyl %u-%u, heads %u-%u, platform [%u, %u, %u, %u]\n",
                  i.release, i.revision, i.encoder_type, i.min_cylinder, i.max_cylinder, i.min_head, i.max_head, i.platform[0],
                  i.platform[1], i.platform[2], i.platform[3]);
    out += line;
    for (const IpfTrackInfo& t : p.disk.tracks) {
        std::string timing;
        for (size_t k = 0; k < t.timing_ns.size(); ++k) timing += (k ? "/" : "") + std::to_string(t.timing_ns[k]);
        std::snprintf(line, sizeof line, "cyl %2u head %u  type %u  cells %6u  index %6u  blocks %2u  weak %5u  timing %s\n",
                      t.cylinder, t.head, t.type, t.cells, t.index_cells, t.blocks, t.weak_cells, timing.c_str());
        out += line;
    }
    return out;
}

bool read_file(const fs::path& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

int test_ipf_dir(const char* dir) {
    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_directory(dir, ec))
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (it->is_regular_file(ec) && ext == ".ipf") files.push_back(it->path());
        }
    if (files.empty()) {
        std::printf("no .ipf files under %s: skipped\n", dir);
        return 77;
    }
    std::sort(files.begin(), files.end());
    // The decoded disk 2 of the two retail pressings is the canonical disc 2 (sha1 of the file -> expected image).
    static const char* const kDisk2Files[] = {"571b9c323dc251d9c00ee5dbf5b6a2603e4de683", "5032c1bc0680000c67393bbca6ee01adeeaf34c3"};
    static const char* const kDisc2 = "a748afc7a142335a40cc46fb7785dea820770241";
    int parsed = 0, disk2 = 0;
    for (const fs::path& f : files) {
        Bytes data;
        CHECK(read_file(f, data));
        std::string sha = sha1_hex(data.data(), data.size());
        Parsed p = parse(data);
        CHECK(p.ok);
        if (!p.ok) {
            std::printf("%s: %s\n", sha.c_str(), p.why.c_str());
            continue;
        }
        ++parsed;
        int standard = 0, raw = 0, empty = 0;
        for (const IpfTrackInfo& t : p.disk.tracks) (t.cells == 0 ? empty : t.standard ? standard : raw)++;
        std::printf("%s: release %u, %zu tracks: %d standard, %d other, %d empty\n", sha.c_str(), p.disk.info.release,
                    p.disk.tracks.size(), standard, raw, empty);
        for (const char* d2 : kDisk2Files) {
            if (sha != d2) continue;
            Bytes adf;
            std::string why;
            bool conv = p.img.to_adf(adf, why);
            CHECK(conv);
            if (conv) CHECK(sha1_hex(adf.data(), adf.size()) == kDisc2);
            ++disk2;
        }
    }
    std::printf("%d IPF files read, %d retail disk 2 images checked, %d/%d checks passed\n", parsed, disk2, g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--fuzz") == 0) { // a longer mutation run, e.g. under the sanitizers
        test_fuzz(std::atoi(argv[2]), argc >= 4 ? uint32_t(std::strtoul(argv[3], nullptr, 0)) : 1u);
        std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
        return g_failures ? 1 : 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--ipf-dir") == 0) return test_ipf_dir(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--adf-sha1") == 0) { // sha1 of the plain ADF an IPF decodes to
        Bytes d;
        if (!read_file(argv[2], d)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
        Parsed p = parse(d);
        Bytes adf;
        std::string why;
        if (!p.ok || !p.img.to_adf(adf, why)) { std::printf("not a plain disk: %s\n", p.ok ? why.c_str() : p.why.c_str()); return 1; }
        std::printf("adf sha1 %s\n", sha1_hex(adf.data(), adf.size()).c_str());
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--report") == 0) {
        Bytes d;
        if (!read_file(argv[2], d)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
        bool ok = false;
        std::string rep = format_report(d, ok);
        std::fputs(rep.c_str(), stdout);
        return ok ? 0 : 1;
    }
    test_standard_disk();
    test_protected_tracks();
    test_errors();
    test_fuzz();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
