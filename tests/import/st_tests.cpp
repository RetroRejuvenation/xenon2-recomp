// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Tests for the Atari ST importer (runtime/import/st_disk, st_fat12, st_game), all on SYNTHETIC data: random "disks" in the three
// containers (.st, .msa, Pasti .stx) built here, FAT12 volumes built here, RNC blocks packed by a test encoder. No game data.
//
//   x2_st_tests SCRATCH_DIR       the synthetic tests (SCRATCH_DIR holds the assets they write; it is removed first)
//   x2_st_tests SCRATCH_DIR --files DIR   also analyses every ST disk image under DIR (real dumps): the Kixx .st/.msa/.stx must give
//                                 the same logical image and import; exit 77 if DIR is empty or missing; local only, nothing is stored
#include "import/importer.h"
#include "import/rnc.h"
#include "import/sha1.h"
#include "import/st_disk.h"
#include "import/st_fat12.h"
#include "import/st_game.h"
#include "import/variants.h"

#include "container_fixtures.h"
#include "rnc_fixtures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
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

bool has_message(const StGame& g, const char* needle) {
    for (const Message& m : g.messages)
        if (m.text.find(needle) != std::string::npos) return true;
    return false;
}

void dump(const StGame& g) {
    for (const Message& m : g.messages) std::fprintf(stderr, "   msg: %s\n", m.text.c_str());
}

Bytes random_bytes(size_t n, uint32_t seed) {
    fx::Rng r{seed * 2654435761u + 12345u};
    Bytes b(n);
    for (uint8_t& x : b) x = uint8_t(r.next() >> 11);
    return b;
}

void le16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void le32(Bytes& b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); }

void set_boot(Bytes& img, int spt, int sides, bool executable) {
    img[11] = 0; img[12] = 2;  // bytes per sector
    img[24] = uint8_t(spt); img[25] = 0;
    img[26] = uint8_t(sides); img[27] = 0;
    if (executable) {
        img[510] = img[511] = 0;
        uint32_t sum = 0;
        for (int i = 0; i < 255; ++i) sum += uint32_t(img[size_t(2 * i)]) << 8 | img[size_t(2 * i + 1)];
        const uint32_t want = (0x1234u - sum) & 0xFFFF;
        img[510] = uint8_t(want >> 8);
        img[511] = uint8_t(want);
    }
}

// A plain .st: cylinders x sides x spt sectors of random data, boot sector set up.
Bytes make_st(int cyl, int sides, int spt, uint32_t seed, bool executable = true) {
    Bytes img = random_bytes(size_t(cyl) * sides * spt * 512, seed);
    set_boot(img, spt, sides, executable);
    return img;
}

// ---- MSA ----
Bytes msa_pack_track(const Bytes& t) {
    Bytes out;
    for (size_t i = 0; i < t.size();) {
        size_t n = 1;
        while (i + n < t.size() && t[i + n] == t[i] && n < 0xFFFF) ++n;
        if (n >= 4 || t[i] == 0xE5) {
            out.push_back(0xE5);
            out.push_back(t[i]);
            fx::put16(out, uint32_t(n));
            i += n;
        } else {
            out.push_back(t[i++]);
        }
    }
    return out.size() < t.size() ? out : t;
}

Bytes make_msa(const Bytes& st, int cyl, int sides, int spt) {
    Bytes out;
    fx::put16(out, 0x0E0F);
    fx::put16(out, uint32_t(spt));
    fx::put16(out, uint32_t(sides - 1));
    fx::put16(out, 0);
    fx::put16(out, uint32_t(cyl - 1));
    const size_t tb = size_t(spt) * 512;
    for (int c = 0; c < cyl; ++c)
        for (int s = 0; s < sides; ++s) {
            const size_t off = (size_t(c) * sides + s) * tb;
            Bytes t(st.begin() + long(off), st.begin() + long(off + tb));
            Bytes p = msa_pack_track(t);
            fx::put16(out, uint32_t(p.size()));
            out.insert(out.end(), p.begin(), p.end());
        }
    return out;
}

// ---- Pasti STX ----
struct StxSec {
    uint8_t cyl = 0, side = 0, num = 1, size_code = 2, fdc = 0;
    uint16_t read_time = 16400;
    Bytes data;
    bool omit_data = false;
};
struct StxTrack {
    int cyl = 0, side = 0;
    std::vector<StxSec> secs;
    Bytes fuzzy;
};

Bytes stx_record(const StxTrack& t) {
    Bytes desc, blob;
    uint16_t bitpos = 600;
    for (const StxSec& s : t.secs) {
        le32(desc, s.omit_data ? 0 : uint32_t(blob.size()));
        le16(desc, bitpos);
        le16(desc, s.read_time);
        desc.push_back(s.cyl); desc.push_back(s.side); desc.push_back(s.num); desc.push_back(s.size_code);
        le16(desc, 0);
        desc.push_back(s.fdc);
        desc.push_back(0);
        if (!s.omit_data) blob.insert(blob.end(), s.data.begin(), s.data.end());
        bitpos = uint16_t(bitpos + 4800);
    }
    Bytes rec;
    le32(rec, uint32_t(16 + desc.size() + t.fuzzy.size() + blob.size()));
    le32(rec, uint32_t(t.fuzzy.size()));
    le16(rec, uint32_t(t.secs.size()));
    le16(rec, 0x61);
    le16(rec, 6400);
    rec.push_back(uint8_t(t.cyl | (t.side << 7)));
    rec.push_back(0);
    rec.insert(rec.end(), desc.begin(), desc.end());
    rec.insert(rec.end(), t.fuzzy.begin(), t.fuzzy.end());
    rec.insert(rec.end(), blob.begin(), blob.end());
    return rec;
}

Bytes stx_file(const std::vector<StxTrack>& tracks, unsigned declared = 0xFFFFFFFFu) {
    Bytes out = {'R', 'S', 'Y', 0};
    le16(out, 3);
    le16(out, 175);
    le16(out, 0);
    out.push_back(uint8_t(declared == 0xFFFFFFFFu ? tracks.size() : declared));
    out.push_back(2);
    for (int i = 0; i < 4; ++i) out.push_back(0);
    for (const StxTrack& t : tracks) {
        Bytes r = stx_record(t);
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

// The tracks of a plain image as an STX (sector order rotated per track like a real disk, plus empty trailing tracks).
std::vector<StxTrack> stx_tracks_of(const Bytes& st, int cyl, int sides, int spt, int empty_tail = 2) {
    std::vector<StxTrack> v;
    for (int c = 0; c < cyl; ++c)
        for (int s = 0; s < sides; ++s) {
            StxTrack t;
            t.cyl = c;
            t.side = s;
            for (int k = 0; k < spt; ++k) {
                const int n = (k + c) % spt + 1; // rotated numbering
                StxSec sec;
                sec.cyl = uint8_t(c), sec.side = uint8_t(s), sec.num = uint8_t(n);
                const size_t off = ((size_t(c) * sides + s) * spt + size_t(n - 1)) * 512;
                sec.data.assign(st.begin() + long(off), st.begin() + long(off + 512));
                t.secs.push_back(sec);
            }
            v.push_back(t);
        }
    for (int e = 0; e < empty_tail; ++e) {
        StxTrack t;
        t.cyl = cyl + e;
        v.push_back(t);
    }
    return v;
}

// ---- stored ZIP ----
uint32_t crc32(const Bytes& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) {
        c ^= b;
        for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
struct ZipMember { std::string name; Bytes data; };
Bytes zip_stored(const std::vector<ZipMember>& members) {
    Bytes out, cd;
    for (const ZipMember& m : members) {
        const uint32_t off = uint32_t(out.size()), crc = crc32(m.data);
        le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, 0); le32(out, 0); le32(out, crc);
        le32(out, uint32_t(m.data.size())); le32(out, uint32_t(m.data.size())); le16(out, uint32_t(m.name.size())); le16(out, 0);
        out.insert(out.end(), m.name.begin(), m.name.end());
        out.insert(out.end(), m.data.begin(), m.data.end());
        le32(cd, 0x02014b50); le16(cd, 20); le16(cd, 20); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, crc);
        le32(cd, uint32_t(m.data.size())); le32(cd, uint32_t(m.data.size())); le16(cd, uint32_t(m.name.size()));
        le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, off);
        cd.insert(cd.end(), m.name.begin(), m.name.end());
    }
    const uint32_t cd_off = uint32_t(out.size());
    out.insert(out.end(), cd.begin(), cd.end());
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, uint32_t(members.size())); le16(out, uint32_t(members.size()));
    le32(out, uint32_t(cd.size())); le32(out, cd_off); le16(out, 0);
    return out;
}

StRelease release_of(const Bytes& st_image, bool importable, const char* id = "st-test") {
    StRelease r;
    r.id = id;
    r.name = std::string("Synthetic test release ") + id;
    r.disk_sha1 = sha1_hex(st_image.data(), st_image.size());
    r.sides = 2;
    r.importable = importable;
    r.build_tag = importable ? "atari-st-test" : "";
    r.note = importable ? "a synthetic importable disk" : "a synthetic recognised disk: refused for testing";
    return r;
}

// ---- tests ----

void test_detect_and_geometry() {
    for (auto g : {std::array<int, 3>{80, 2, 10}, {80, 1, 9}, {80, 2, 9}, {81, 2, 10}, {80, 2, 11}, {80, 1, 10}}) {
        Bytes st = make_st(g[0], g[1], g[2], uint32_t(g[0] + g[1] * 7 + g[2]));
        int c = 0, s = 0, spt = 0;
        CHECK(st_geometry(st.data(), st.size(), c, s, spt) && c == g[0] && s == g[1] && spt == g[2]);
        CHECK(detect_st_container(st.data(), st.size()) == StContainer::St);
        StDisk d;
        std::string why;
        CHECK(read_st_image(st.data(), st.size(), d, why));
        CHECK(d.cylinders == g[0] && d.sides == g[1] && d.sectors_per_track() == g[2]);
        std::vector<uint8_t> logical;
        std::vector<std::string> problems;
        CHECK(d.logical_image(g[2], logical, problems) && problems.empty() && logical == st);
        CHECK(d.nonstandard_tracks(g[2], 4).empty());
    }
    {   // a BPB that is junk: the size table decides (an executable boot sector still marks it as ST)
        Bytes st = random_bytes(819200, 77);
        st[11] = 0x12; st[12] = 0x34; // bytes per sector nonsense
        set_boot(st, 10, 2, true);
        st[11] = 0x12; st[12] = 0x34;
        // set_boot rewrote the BPB; put the junk back and keep the checksum right
        st[510] = st[511] = 0;
        uint32_t sum = 0;
        for (int i = 0; i < 255; ++i) sum += uint32_t(st[size_t(2 * i)]) << 8 | st[size_t(2 * i + 1)];
        const uint32_t want = (0x1234u - sum) & 0xFFFF;
        st[510] = uint8_t(want >> 8); st[511] = uint8_t(want);
        int c = 0, s = 0, spt = 0;
        CHECK(st_geometry(st.data(), st.size(), c, s, spt) && c == 80 && s == 2 && spt == 10);
        CHECK(detect_st_container(st.data(), st.size()) == StContainer::St);
    }
    {   // things that are not ST disks
        Bytes zeros(819200, 0);
        CHECK(detect_st_container(zeros.data(), zeros.size()) == StContainer::None);
        Bytes junk = random_bytes(819200, 5);
        junk[11] = 0; junk[12] = 0; // not 512
        CHECK(detect_st_container(junk.data(), junk.size()) == StContainer::None);
        Bytes adf(901120, 0);
        adf[0] = 'D'; adf[1] = 'O'; adf[2] = 'S';
        CHECK(detect_st_container(adf.data(), adf.size()) == StContainer::None);
        Bytes odd(819201, 1);
        CHECK(detect_st_container(odd.data(), odd.size()) == StContainer::None);
        Bytes tiny(100, 0);
        CHECK(detect_st_container(tiny.data(), tiny.size()) == StContainer::None);
        CHECK(detect_st_container(nullptr, 0) == StContainer::None);
    }
    {
        Bytes st = make_st(80, 2, 10, 3);
        Bytes msa = make_msa(st, 80, 2, 10);
        CHECK(detect_st_container(msa.data(), msa.size()) == StContainer::Msa);
        Bytes stx = stx_file(stx_tracks_of(st, 80, 2, 10));
        CHECK(detect_st_container(stx.data(), stx.size()) == StContainer::Stx);
        CHECK(std::string(st_container_name(StContainer::Stx)).find("Pasti") != std::string::npos);
    }
}

void test_msa() {
    Bytes st = make_st(80, 2, 10, 11);
    // make some tracks compressible and include $E5 bytes
    for (size_t i = 0; i < 6 * 5120; ++i) st[5120 * 20 + i] = (i / 700) % 3 == 0 ? 0xE5 : uint8_t(i / 700);
    set_boot(st, 10, 2, true);
    Bytes msa = make_msa(st, 80, 2, 10);
    CHECK(msa.size() < st.size());
    StDisk d;
    std::string why;
    CHECK(read_msa(msa.data(), msa.size(), d, why));
    std::vector<uint8_t> logical;
    std::vector<std::string> problems;
    CHECK(d.logical_image(10, logical, problems) && logical == st && d.format == StDisk::Format::Msa);
    // malformed
    {
        Bytes cut(msa.begin(), msa.begin() + long(msa.size() / 2));
        CHECK(!read_msa(cut.data(), cut.size(), d, why) &&
              (why.find("cut short") != std::string::npos || why.find("bad length") != std::string::npos));
        Bytes cut2(msa.begin(), msa.begin() + long(msa.size() - 1));
        CHECK(!read_msa(cut2.data(), cut2.size(), d, why));
        Bytes bad = msa;
        bad[2] = 0; bad[3] = 0; // zero sectors per track
        CHECK(!read_msa(bad.data(), bad.size(), d, why));
        bad = msa;
        bad[6] = 0; bad[7] = 9; bad[8] = 0; bad[9] = 3; // first track 9 > last 3
        CHECK(!read_msa(bad.data(), bad.size(), d, why));
        bad = msa;
        bad[4] = 0; bad[5] = 7; // sides field 7
        CHECK(!read_msa(bad.data(), bad.size(), d, why));
        bad = msa;
        bad[10] = 0xFF; bad[11] = 0xFF; // first track length beyond the file / track
        CHECK(!read_msa(bad.data(), bad.size(), d, why) && why.find("bad length") != std::string::npos);
        // a run that unpacks to more than a track
        Bytes over = {0x0E, 0x0F, 0, 1, 0, 0, 0, 0, 0, 0};
        Bytes run = {0xE5, 0x41, 0xFF, 0xFF};
        fx::put16(over, uint32_t(run.size()));
        over.insert(over.end(), run.begin(), run.end());
        CHECK(!read_msa(over.data(), over.size(), d, why) && why.find("more than a track") != std::string::npos);
        // a short track: unpacks to fewer bytes than a track
        Bytes few = {0x0E, 0x0F, 0, 1, 0, 0, 0, 0, 0, 0};
        Bytes lit = {1, 2, 3};
        fx::put16(few, uint32_t(lit.size()));
        few.insert(few.end(), lit.begin(), lit.end());
        CHECK(!read_msa(few.data(), few.size(), d, why));
        // a cut-short run marker at the end of a packed track
        Bytes tail = {0x0E, 0x0F, 0, 1, 0, 0, 0, 0, 0, 0};
        Bytes half = {0xE5, 1};
        fx::put16(tail, uint32_t(half.size()));
        tail.insert(tail.end(), half.begin(), half.end());
        CHECK(!read_msa(tail.data(), tail.size(), d, why));
        // size cap
        CHECK(!read_msa(msa.data(), msa.size(), d, why, 1000));
    }
}

void test_stx() {
    Bytes st = make_st(80, 2, 10, 21);
    Bytes stx = stx_file(stx_tracks_of(st, 80, 2, 10));
    StDisk d;
    std::string why;
    CHECK(read_stx(stx.data(), stx.size(), d, why));
    CHECK(d.format == StDisk::Format::Stx && d.cylinders == 80 && d.sides == 2 && d.stx_version == 3);
    CHECK(d.sectors_per_track() == 10);
    std::vector<uint8_t> logical;
    std::vector<std::string> problems;
    CHECK(d.logical_image(10, logical, problems) && logical == st); // rotated sector order, still the same logical disk
    CHECK(d.nonstandard_tracks(10, 4).empty());

    // single-sided disk
    {
        Bytes one = make_st(80, 1, 10, 22);
        Bytes f = stx_file(stx_tracks_of(one, 80, 1, 10));
        StDisk d1;
        CHECK(read_stx(f.data(), f.size(), d1, why) && d1.sides == 1 && d1.cylinders == 80);
        CHECK(d1.logical_image(10, logical, problems) && logical == one);
    }
    // the fuzzy-mask rule: sector data offsets are relative to the end of the mask
    {
        std::vector<StxTrack> t = stx_tracks_of(st, 80, 2, 10);
        t[0].fuzzy = Bytes(512, 0xAA);
        Bytes f = stx_file(t);
        StDisk df;
        CHECK(read_stx(f.data(), f.size(), df, why));
        CHECK(df.track(0, 0).fuzzy_bytes == 512);
        CHECK(df.logical_image(10, logical, problems) && logical == st);
        CHECK(df.nonstandard_tracks(10, 4).size() == 1 && df.nonstandard_tracks(10, 4)[0].find("weak-bit") != std::string::npos);
    }
    // a CRC error keeps its data but is reported; a record not found has no data; both make the disk imperfect
    {
        std::vector<StxTrack> t = stx_tracks_of(st, 80, 2, 10);
        for (StxSec& s : t[0].secs)
            if (s.num == 6) s.fdc = 0x08;
        for (StxSec& s : t[2].secs)
            if (s.num == 3) { s.fdc = 0x10; s.omit_data = true; s.data.clear(); }
        Bytes f = stx_file(t);
        StDisk dc;
        CHECK(read_stx(f.data(), f.size(), dc, why));
        CHECK(!dc.logical_image(10, logical, problems));
        bool crc = false, rnf = false;
        for (const std::string& p : problems) {
            crc = crc || p.find("sector 6 CRC error") != std::string::npos;
            rnf = rnf || p.find("sector 3 record not found") != std::string::npos;
        }
        CHECK(crc && rnf);
        // the CRC-error sector's data is still in the image, the missing one is zero
        CHECK(std::equal(st.begin() + long(((0 * 2 + 0) * 10 + 5) * 512), st.begin() + long(((0 * 2 + 0) * 10 + 6) * 512),
                         logical.begin() + long(((0 * 2 + 0) * 10 + 5) * 512)));
        CHECK(std::all_of(logical.begin() + long(((1 * 2 + 0) * 10 + 2) * 512), logical.begin() + long(((1 * 2 + 0) * 10 + 3) * 512),
                          [](uint8_t b) { return b == 0; }));
        CHECK(dc.nonstandard_tracks(10, 8).size() == 2);
    }
    // odd sectors: an 11th sector, a 1024-byte sector, a duplicated number
    {
        std::vector<StxTrack> t = stx_tracks_of(st, 80, 2, 10);
        StxSec extra;
        extra.cyl = 0, extra.side = 0, extra.num = 11;
        extra.data = random_bytes(512, 1);
        t[0].secs.push_back(extra);
        StxSec dup = t[1].secs[0];
        t[1].secs.push_back(dup);
        StxSec big;
        big.num = 12, big.size_code = 3, big.data = random_bytes(1024, 2);
        t[3].secs.push_back(big);
        Bytes f = stx_file(t);
        StDisk dd;
        CHECK(read_stx(f.data(), f.size(), dd, why));
        CHECK(!dd.logical_image(10, logical, problems));
        CHECK(dd.nonstandard_tracks(10, 8).size() == 3);
    }
    // malformed files
    {
        Bytes bad = stx;
        CHECK(!read_stx(bad.data(), 10, d, why));
        for (size_t cut : {size_t(16), size_t(40), stx.size() / 2, stx.size() - 1}) {
            CHECK(!read_stx(stx.data(), cut, d, why));
        }
        bad = stx;
        bad[16] = 4; bad[17] = bad[18] = bad[19] = 0; // record size 4 < 16
        CHECK(!read_stx(bad.data(), bad.size(), d, why) && why.find("bad size") != std::string::npos);
        bad = stx;
        bad[16 + 8] = 200; // 200 sectors in a record
        CHECK(!read_stx(bad.data(), bad.size(), d, why));
        std::vector<StxTrack> t = stx_tracks_of(st, 80, 2, 10);
        t[4].secs[2].size_code = 5;
        Bytes f = stx_file(t);
        CHECK(!read_stx(f.data(), f.size(), d, why) && why.find("size code") != std::string::npos);
        t = stx_tracks_of(st, 80, 2, 10);
        t[5].cyl = 99; // cylinder out of range with sectors
        f = stx_file(t);
        CHECK(!read_stx(f.data(), f.size(), d, why) && why.find("out of range") != std::string::npos);
        t = stx_tracks_of(st, 80, 2, 10);
        t[6] = t[4]; // two records for one track
        f = stx_file(t);
        CHECK(!read_stx(f.data(), f.size(), d, why) && why.find("two records") != std::string::npos);
        t = stx_tracks_of(st, 80, 2, 10);
        f = stx_file(t, 250); // declares more records than the file holds
        CHECK(!read_stx(f.data(), f.size(), d, why));
        std::vector<StxTrack> empty;
        StxTrack none;
        empty.push_back(none);
        f = stx_file(empty);
        CHECK(!read_stx(f.data(), f.size(), d, why) && why.find("no sectors") != std::string::npos);
    }
    // sector data that runs past the file: a descriptor with a huge data offset
    {
        Bytes bad = stx;
        // first record: descriptor 0 data offset
        bad[16 + 16 + 0] = 0xFF; bad[16 + 16 + 1] = 0xFF; bad[16 + 16 + 2] = 0xFF; bad[16 + 16 + 3] = 0x7F;
        CHECK(!read_stx(bad.data(), bad.size(), d, why) && why.find("past the end") != std::string::npos);
    }
    // an aliasing bomb: every track's sectors point at the same 1 KB (size code 3), 64 sectors a track, 168 tracks, in a small file
    {
        std::vector<StxTrack> t;
        const Bytes blob = random_bytes(1024, 9);
        for (int c = 0; c < 84; ++c)
            for (int s = 0; s < 2; ++s) {
                StxTrack tr;
                tr.cyl = c, tr.side = s;
                for (int k = 0; k < 64; ++k) {
                    StxSec sec;
                    sec.num = uint8_t(k + 1), sec.size_code = 3;
                    sec.data = blob;
                    tr.secs.push_back(sec);
                }
                t.push_back(tr);
            }
        // build it with every descriptor's data offset 0: patch after encoding
        Bytes f;
        {
            Bytes hdr = {'R', 'S', 'Y', 0};
            le16(hdr, 3); le16(hdr, 175); le16(hdr, 0); hdr.push_back(uint8_t(t.size())); hdr.push_back(2);
            for (int i = 0; i < 4; ++i) hdr.push_back(0);
            f = hdr;
            for (const StxTrack& tr : t) {
                Bytes desc;
                for (size_t k = 0; k < tr.secs.size(); ++k) {
                    le32(desc, 0); le16(desc, 600); le16(desc, 16400);
                    desc.push_back(uint8_t(tr.cyl)); desc.push_back(uint8_t(tr.side)); desc.push_back(uint8_t(k + 1)); desc.push_back(3);
                    le16(desc, 0); desc.push_back(0); desc.push_back(0);
                }
                Bytes rec;
                le32(rec, uint32_t(16 + desc.size() + 1024));
                le32(rec, 0);
                le16(rec, uint32_t(tr.secs.size()));
                le16(rec, 0x61); le16(rec, 6400);
                rec.push_back(uint8_t(tr.cyl | (tr.side << 7))); rec.push_back(0);
                rec.insert(rec.end(), desc.begin(), desc.end());
                rec.insert(rec.end(), blob.begin(), blob.end());
                f.insert(f.end(), rec.begin(), rec.end());
            }
        }
        CHECK(f.size() < size_t(400) * 1024);
        const size_t rss_before = 0; (void)rss_before;
        CHECK(!read_stx(f.data(), f.size(), d, why) && why.find("MB of sector data") != std::string::npos);
        CHECK(!read_stx(f.data(), f.size(), d, why, 64 * 1024));
    }
}

// ---- FAT12 ----
struct FatDisk {
    Bytes img;
    void set_fat(unsigned n, unsigned v) {
        const size_t off = 512 + size_t(n) * 3 / 2;
        if (n & 1) { img[off] = uint8_t((img[off] & 0x0F) | ((v & 0x0F) << 4)); img[off + 1] = uint8_t(v >> 4); }
        else { img[off] = uint8_t(v); img[off + 1] = uint8_t((img[off + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
        for (int f = 1; f < 2; ++f) {
            const size_t o2 = 512 + 3 * 512 + size_t(n) * 3 / 2;
            if (n & 1) { img[o2] = uint8_t((img[o2] & 0x0F) | ((v & 0x0F) << 4)); img[o2 + 1] = uint8_t(v >> 4); }
            else { img[o2] = uint8_t(v); img[o2 + 1] = uint8_t((img[o2 + 1] & 0xF0) | ((v >> 8) & 0x0F)); }
        }
    }
    uint8_t* cluster(unsigned n) { return img.data() + (14 + size_t(n - 2) * 2) * 512; }
    static void entry(uint8_t* e, const char* name83, uint8_t attr, unsigned cluster, uint32_t size) {
        std::memset(e, ' ', 11);
        std::memcpy(e, name83, std::min<size_t>(11, std::strlen(name83)));
        e[11] = attr;
        std::memset(e + 12, 0, 20);
        e[26] = uint8_t(cluster); e[27] = uint8_t(cluster >> 8);
        e[28] = uint8_t(size); e[29] = uint8_t(size >> 8); e[30] = uint8_t(size >> 16); e[31] = uint8_t(size >> 24);
    }
};

FatDisk make_fat() {
    FatDisk d;
    d.img.assign(1440 * 512, 0);
    uint8_t* b = d.img.data();
    b[11] = 0; b[12] = 2; b[13] = 2; b[14] = 1; b[15] = 0; b[16] = 2; b[17] = 112; b[18] = 0; b[19] = 0xA0; b[20] = 0x05; b[21] = 0xF9; b[22] = 3; b[23] = 0;
    b[24] = 9; b[25] = 0; b[26] = 2; b[27] = 0;
    std::memcpy(b + 3, "TESTFAT", 7);
    d.img[512] = d.img[512 + 3 * 512] = 0xF9;
    d.img[513] = d.img[514] = d.img[513 + 3 * 512] = d.img[514 + 3 * 512] = 0xFF;
    uint8_t* root = d.img.data() + 7 * 512;
    FatDisk::entry(root + 0, "TESTDISK   ", 0x08, 0, 0);
    FatDisk::entry(root + 32, "HELLO   TXT", 0x20, 2, 700);
    FatDisk::entry(root + 64, "BIG     BIN", 0x20, 3, 3000);
    FatDisk::entry(root + 96, "SUB        ", 0x10, 6, 0);
    FatDisk::entry(root + 128, "LOOP    BIN", 0x20, 8, 3000);
    root[160] = 0xE5; // deleted
    d.set_fat(2, 0xFFF);
    d.set_fat(3, 4); d.set_fat(4, 5); d.set_fat(5, 0xFFF);
    d.set_fat(6, 0xFFF);
    d.set_fat(7, 0xFFF);
    d.set_fat(8, 9); d.set_fat(9, 8); // a loop
    for (int i = 0; i < 700; ++i) d.cluster(2)[i] = uint8_t('A' + i % 26);
    for (int i = 0; i < 3000; ++i) d.cluster(3)[i] = uint8_t(i * 7);
    FatDisk::entry(d.cluster(6) + 0, ".          ", 0x10, 6, 0);
    FatDisk::entry(d.cluster(6) + 32, "..         ", 0x10, 0, 0);
    d.cluster(6)[11 + 32 - 32 + 0] = 0x10;
    FatDisk::entry(d.cluster(6) + 64, "INNER   BIN", 0x20, 7, 100);
    for (int i = 0; i < 100; ++i) d.cluster(7)[i] = uint8_t(255 - i);
    return d;
}

void test_fat12() {
    FatDisk d = make_fat();
    FatInfo info;
    std::vector<FatEntry> entries;
    std::vector<std::string> warnings;
    std::string why;
    CHECK(fat12_info(d.img.data(), d.img.size(), info, why));
    CHECK(info.bytes_per_sector == 512 && info.sectors_per_cluster == 2 && info.media == 0xF9 && info.clusters == 713);
    CHECK(fat12_list(d.img.data(), d.img.size(), info, entries, warnings, why));
    CHECK(info.label == "TESTDISK");
    std::set<std::string> names;
    for (const FatEntry& e : entries) names.insert(e.path);
    CHECK(names.count("HELLO.TXT") && names.count("BIG.BIN") && names.count("SUB") && names.count("SUB/INNER.BIN") && names.count("LOOP.BIN"));
    CHECK(!names.count(".") && !names.count("SUB/.") && !names.count("SUB/.."));
    CHECK(entries.size() == 5);
    for (const FatEntry& e : entries) {
        std::vector<uint8_t> out;
        if (e.path == "HELLO.TXT") {
            CHECK(fat12_read(d.img.data(), d.img.size(), e, out, why) && out.size() == 700 && out[0] == 'A' && out[27] == 'B');
        } else if (e.path == "BIG.BIN") {
            CHECK(fat12_read(d.img.data(), d.img.size(), e, out, why) && out.size() == 3000 && out[1] == 7);
        } else if (e.path == "SUB/INNER.BIN") {
            CHECK(fat12_read(d.img.data(), d.img.size(), e, out, why) && out.size() == 100 && out[0] == 255);
        } else if (e.path == "LOOP.BIN") {
            CHECK(!fat12_read(d.img.data(), d.img.size(), e, out, why) && why.find("loop") != std::string::npos);
        } else if (e.directory) {
            CHECK(!fat12_read(d.img.data(), d.img.size(), e, out, why));
        }
    }
    {   // too large for the limit
        FatEntry big;
        big.size = 100;
        big.first_cluster = 2;
        std::vector<uint8_t> out;
        CHECK(!fat12_read(d.img.data(), d.img.size(), big, out, why, 50));
    }
    {   // a stale BPB without a file system behind it (Kixx's boot sector has one): refused
        FatDisk raw = d;
        std::memset(raw.img.data() + 512, 0x5A, 3 * 512);
        CHECK(!fat12_info(raw.img.data(), raw.img.size(), info, why) && why.find("media descriptor") != std::string::npos);
        CHECK(!fat12_list(raw.img.data(), raw.img.size(), info, entries, warnings, why));
    }
    {   // BPB damage: bytes per sector, cluster size, FAT count, tiny image
        FatDisk bad = d;
        bad.img[11] = 0; bad.img[12] = 4;
        CHECK(!fat12_info(bad.img.data(), bad.img.size(), info, why));
        bad = d; bad.img[13] = 3;
        CHECK(!fat12_info(bad.img.data(), bad.img.size(), info, why));
        bad = d; bad.img[16] = 0;
        CHECK(!fat12_info(bad.img.data(), bad.img.size(), info, why));
        bad = d; bad.img[22] = 0; bad.img[23] = 0;
        CHECK(!fat12_info(bad.img.data(), bad.img.size(), info, why));
        CHECK(!fat12_info(d.img.data(), 300, info, why));
        CHECK(!fat12_info(d.img.data(), 600, info, why));
    }
    {   // a directory that loops back onto itself is bounded by the depth limit
        FatDisk lp = d;
        FatDisk::entry(lp.cluster(6) + 64, "SELF       ", 0x10, 6, 0);
        CHECK(fat12_list(lp.img.data(), lp.img.size(), info, entries, warnings, why));
        CHECK(entries.size() <= 1024);
        bool deep = false;
        for (const std::string& w : warnings) deep = deep || w.find("nested too deeply") != std::string::npos;
        CHECK(deep);
    }
    {   // a cluster number outside the data area, in a file entry
        FatDisk oob = d;
        FatDisk::entry(oob.img.data() + 7 * 512 + 32, "HELLO   TXT", 0x20, 4000, 700);
        CHECK(fat12_list(oob.img.data(), oob.img.size(), info, entries, warnings, why));
        for (const FatEntry& e : entries)
            if (e.path == "HELLO.TXT") {
                std::vector<uint8_t> out;
                CHECK(!fat12_read(oob.img.data(), oob.img.size(), e, out, why));
            }
    }
}

// ---- the import ----

// A synthetic "release": a double-sided disk with an executable boot sector and two verified RNC blocks on side 1.
struct Synth {
    Bytes st;
    Bytes rnc1, rnc2;
    size_t off1 = 0, off2 = 0; // offsets of the blocks in side 1's stream
};

Synth make_synth(uint32_t seed) {
    Synth s;
    s.st = make_st(80, 2, 10, seed);
    Bytes a = random_bytes(3000, seed + 1), b(5000, 0x42);
    for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t((i / 50) ^ (i % 7));
    s.rnc1 = fx::rnc_pack(a);
    s.rnc2 = fx::rnc_pack(b, 2);
    // side 1 stream: cylinder-major, 5120 bytes a cylinder; the blocks at side offsets 5120+100 and 12 cylinders in
    s.off1 = 5120 + 100;
    s.off2 = 5120 * 12 + 3000;
    auto put = [&](size_t side_off, const Bytes& blk) {
        size_t o = side_off;
        for (size_t i = 0; i < blk.size(); ++i, ++o) {
            const size_t cyl = o / 5120, within = o % 5120;
            s.st[((cyl * 2 + 1) * 10) * 512 + within] = blk[i];
        }
    };
    put(s.off1, s.rnc1);
    put(s.off2, s.rnc2);
    return s;
}

std::string scratch_dir;

void test_import_flow() {
    Synth s = make_synth(31);
    const StRelease rel_ok = release_of(s.st, true);
    const std::vector<StRelease> cat = {rel_ok, release_of(random_bytes(100, 1), false, "st-other")};
    auto run = [&](std::vector<StInput> in, const std::vector<StRelease>* c = nullptr) { return import_st_inputs(in, c ? c : &cat); };

    Bytes msa = make_msa(s.st, 80, 2, 10);
    Bytes stx = stx_file(stx_tracks_of(s.st, 80, 2, 10));
    for (auto& [name, data] : std::vector<std::pair<std::string, Bytes>>{{"disk.st", s.st}, {"disk.msa", msa}, {"disk.stx", stx}}) {
        StGame g = run({{name, data, false}});
        if (g.status != Status::Ok) dump(g);
        CHECK(g.status == Status::Ok && g.image == s.st && g.image_sha1 == rel_ok.disk_sha1);
        CHECK(g.disk.release && g.disk.release->id == "st-test" && g.disk.executable_boot && g.disk.sectors_per_track == 10);
        CHECK(g.source_sha1 == sha1_hex(data.data(), data.size()));
        // the two RNC blocks are indexed with their geometry
        CHECK(g.disk.rnc.size() == 2);
        if (g.disk.rnc.size() == 2) {
            CHECK(g.disk.rnc[0].side == 1 && g.disk.rnc[0].side_offset == s.off1 && g.disk.rnc[0].cylinder == int(s.off1 / 5120) &&
                  g.disk.rnc[0].track_offset == s.off1 % 5120 && g.disk.rnc[0].unpacked == 3000);
            CHECK(g.disk.rnc[1].side_offset == s.off2 && g.disk.rnc[1].unpacked == 5000);
        }
        CHECK(has_message(g, "unprotected release"));
    }
    // all three containers together: the first is used, the others are the same disk
    {
        StGame g = run({{"a.st", s.st, false}, {"b.msa", msa, false}, {"c.stx", stx, false}});
        CHECK(g.status == Status::Ok && has_message(g, "the same disk as a.st, not used"));
    }
    // from a zip, and from a folder (in_folder inputs: unrelated files are only noted)
    {
        Bytes z = zip_stored({{"readme.txt", {'h', 'i'}}, {"x/disk.msa", msa}});
        StGame g = run({{"pack.zip", z, false}});
        CHECK(g.status == Status::Ok && g.image == s.st && has_message(g, "readme.txt: not an Atari ST disk image, skipped"));
        StGame f = run({{"dir/notes.txt", {'x'}, true}, {"dir/disk.stx", stx, true}});
        CHECK(f.status == Status::Ok);
        StGame nested = run({{"outer.zip", zip_stored({{"inner.zip", z}}), false}});
        CHECK(nested.status == Status::Ok);
        Bytes deep = z;
        for (int i = 0; i < 4; ++i) deep = zip_stored({{"in.zip", deep}});
        StGame too = run({{"deep.zip", deep, false}});
        CHECK(too.status != Status::Ok && has_message(too, "nested too deeply"));
    }
    // refusals: nothing found, a recognised disk that cannot be imported, an unknown disk, a damaged container
    {
        StGame none = run({{"notes.txt", {'a', 'b'}, false}});
        CHECK(none.status == Status::BadInput && has_message(none, "not an Atari ST disk image"));
        StGame empty = run({});
        CHECK(empty.status == Status::BadInput && has_message(empty, "no Atari ST disk image found"));
        Bytes other = make_st(80, 2, 10, 99);
        StGame unk = run({{"other.st", other, false}});
        CHECK(unk.status == Status::Unsupported && has_message(unk, "not a Xenon 2 Megablast ST disk the importer knows") && has_message(unk, "sha1"));
        const std::vector<StRelease> refused = {release_of(other, false, "st-refused")};
        StGame ref = run({{"other.st", other, false}}, &refused);
        CHECK(ref.status == Status::Unsupported && has_message(ref, "refused for testing") && ref.image.empty());
        Bytes trunc(stx.begin(), stx.begin() + long(stx.size() / 3));
        StGame bad = run({{"cut.stx", trunc, false}});
        CHECK(bad.status != Status::Ok && has_message(bad, "STX (Pasti) image:"));
        // an importable disk next to a refused one: the importable one wins, the other is not used
        const std::vector<StRelease> both = {rel_ok, release_of(other, false, "st-refused")};
        StGame mix = run({{"other.st", other, false}, {"disk.st", s.st, false}}, &both);
        CHECK(mix.status == Status::Ok && mix.image == s.st && has_message(mix, "other.st: not used"));
        // a non-executable boot sector warns
        Synth ne = make_synth(32);
        set_boot(ne.st, 10, 2, false);
        ne.st[510] = 0; ne.st[511] = 1;
        const std::vector<StRelease> cne = {release_of(ne.st, true, "st-ne")};
        StGame w = run({{"ne.st", ne.st, false}}, &cne);
        CHECK(w.status == Status::Ok && has_message(w, "boot sector is not executable"));
    }
    // a corrupted RNC block is not indexed
    {
        Synth c = make_synth(33);
        const size_t o = c.off1 + 40;
        const size_t cyl = o / 5120, within = o % 5120;
        c.st[((cyl * 2 + 1) * 10) * 512 + within] ^= 0x55;
        const std::vector<StRelease> cc = {release_of(c.st, true, "st-c")};
        StGame g = run({{"c.st", c.st, false}}, &cc);
        CHECK(g.status == Status::Ok && g.disk.rnc.size() == 1 && g.disk.rnc[0].side_offset == c.off2);
    }
    // writing, verifying, tampering
    {
        const std::string dir = scratch_dir + "/assets-st";
        StGame g = run({{"disk.stx", stx, false}});
        std::string err;
        CHECK(write_st_assets(dir, g, err));
        std::string reason;
        CHECK(verify_assets(dir, reason));
        std::ifstream mf(fs::path(dir) / "manifest.txt");
        std::string text((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
        CHECK(manifest_is_st(text));
        for (const char* key : {"build=atari-st-test\n", "game=st\n", "container=STX (Pasti)\n", "cylinders=80\n", "sides=2\n", "sectors_per_track=10\n",
                                "sector_bytes=512\n", "bootable=true\n", "boot_checksum=0x1234\n", "disk.bin_size=819200\n", "rnc_streams=2\n"})
            CHECK(text.find(key) != std::string::npos);
        CHECK(text.find("disk.bin_sha1=" + rel_ok.disk_sha1 + "\n") != std::string::npos);
        CHECK(text.find("rnc.0=side=1,") != std::string::npos && text.find("rnc.1=side=1,") != std::string::npos);
        CHECK(!fs::exists(fs::path(dir) / "game_0400.bin") && !fs::exists(fs::path(dir) / "disc2.bin"));
        // tamper with disk.bin: verification fails
        {
            std::fstream f(fs::path(dir) / "disk.bin", std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(1000);
            f.put('\x7E');
        }
        CHECK(!verify_assets(dir, reason) && reason.find("does not match the import manifest") != std::string::npos);
        // a truncated disk.bin, and a missing one
        {
            std::ofstream f(fs::path(dir) / "disk.bin", std::ios::binary | std::ios::trunc);
            f.write("short", 5);
        }
        CHECK(!verify_assets(dir, reason));
        std::error_code ec;
        fs::remove(fs::path(dir) / "disk.bin", ec);
        CHECK(!verify_assets(dir, reason));
        // an earlier import of another build in the folder is cleared by the write
        { std::ofstream f(fs::path(dir) / "game_0400.bin", std::ios::binary); f << "old"; }
        { std::ofstream f(fs::path(dir) / "copylock.txt", std::ios::binary); f << "old"; }
        CHECK(write_st_assets(dir, g, err) && !fs::exists(fs::path(dir) / "game_0400.bin") && !fs::exists(fs::path(dir) / "copylock.txt"));
        CHECK(verify_assets(dir, reason));
        // nothing to write for a refused import
        StGame refusedg = run({{"o.st", make_st(80, 2, 10, 5), false}});
        CHECK(!write_st_assets(scratch_dir + "/nothing", refusedg, err));
        CHECK(!fs::exists(scratch_dir + "/nothing"));
    }
}

void test_diagnose_findings() {
    // a synthetic Copylock-ST-like Pasti disk: the key sector begins "Rob Northen Comp", is written with a CRC error, is slow and
    // sits on a track with a weak-bit mask
    Bytes st = make_st(80, 1, 10, 41);
    std::memcpy(st.data() + 5 * 512, "Rob Northen Comp", 16);
    std::vector<StxTrack> t = stx_tracks_of(st, 80, 1, 10);
    // stx_tracks_of rotates sector numbers per track, track 0 has them in order
    for (StxSec& sec : t[0].secs)
        if (sec.num == 6) { sec.fdc = 0x08; sec.read_time = 17200; }
    t[0].fuzzy = Bytes(512, 0x00);
    Bytes f = stx_file(t);
    std::vector<Message> msgs;
    std::vector<StInput> in = {{"locked.stx", f, false}};
    std::vector<StDiskReport> reports = diagnose_st_inputs(in, msgs);
    CHECK(reports.size() == 1);
    if (reports.size() == 1) {
        const StDiskReport& r = reports[0];
        bool key = false, weak = false, slow = false;
        for (const std::string& s : r.findings) {
            key = key || (s.find("Copylock ST key sector") != std::string::npos && s.find("written with a CRC error") != std::string::npos);
            weak = weak || s.find("weak-bit") != std::string::npos;
            slow = slow || s.find("reads 4.9% longer") != std::string::npos || s.find("longer than the other sectors") != std::string::npos;
        }
        CHECK(key && weak && slow);
        CHECK(r.nonstandard_total == 1 && !r.problems.empty() && r.release == nullptr);
        std::string joined;
        for (const std::string& l : format_st_report(r)) joined += l + "\n";
        CHECK(joined.find("copy protection findings:") != std::string::npos && joined.find("recognised as: not a catalogued") != std::string::npos);
        CHECK(joined.find("logical image sha1: ") != std::string::npos && joined.find("geometry: 80 cylinders x 1 side x 10 sectors") != std::string::npos);
    }
    // a clean disk: no findings, "none found"
    Bytes clean = make_st(80, 2, 10, 42);
    std::vector<StInput> in2 = {{"clean.st", clean, false}};
    std::vector<StDiskReport> r2 = diagnose_st_inputs(in2, msgs);
    CHECK(r2.size() == 1 && r2[0].findings.empty() && r2[0].nonstandard_total == 0);
    std::string joined;
    for (const std::string& l : format_st_report(r2[0])) joined += l + "\n";
    CHECK(joined.find("copy protection: none found") != std::string::npos && joined.find("RNC ProPack blocks: none that verify") != std::string::npos);
    // a FAT12 disk lists its files in the report
    FatDisk fat = make_fat();
    set_boot(fat.img, 9, 2, false);
    // set_boot overwrote the BPB fields it owns only (bytes per sector, spt, sides): the FAT layout stays valid
    std::vector<StInput> in3 = {{"fat.st", fat.img, false}};
    std::vector<StDiskReport> r3 = diagnose_st_inputs(in3, msgs);
    CHECK(r3.size() == 1);
    if (r3.size() == 1) {
        bool hello = false;
        for (const std::string& s : r3[0].files) hello = hello || s.find("HELLO.TXT (700 bytes)") != std::string::npos;
        CHECK(hello && r3[0].fat_note.find("TESTDISK") != std::string::npos);
    }
}

void test_registry() {
    const std::vector<StRelease>& v = st_releases();
    CHECK(v.size() == 5);
    std::set<std::string> ids, hashes;
    int importable = 0;
    for (const StRelease& r : v) {
        ids.insert(r.id);
        hashes.insert(r.disk_sha1);
        importable += r.importable ? 1 : 0;
        CHECK(r.disk_sha1.size() == 40 && !r.note.empty() && !r.name.empty());
        CHECK(r.importable == !r.build_tag.empty());
        CHECK(r.build_tag.empty() || r.build_tag.rfind(kStBuildPrefix, 0) == 0);
        CHECK(find_st_release(r.disk_sha1) == &r);
    }
    CHECK(ids.size() == v.size() && hashes.size() == v.size() && importable == 1);
    const StRelease* kixx = find_st_release("323d6859635b6b82c86363db048b26e0f3df994c");
    CHECK(kixx && kixx->importable && kixx->build_tag == "atari-st-kixx" && kixx->sides == 2);
    CHECK(find_st_release("0000000000000000000000000000000000000000") == nullptr);
    int rows = 0;
    for (const VariantInfo& i : all_variants())
        if (i.recipe == Recipe::StDisk) {
            ++rows;
            CHECK(i.disk_sha1.size() == 40 && i.supported == (i.build == "atari-st-kixx") && !i.identified_by.empty());
        }
    CHECK(rows == 5);
}

// A bounded random-mutation pass over all three containers: nothing may crash or read out of bounds (run it under ASan/UBSan).
void test_fuzz() {
    Synth s = make_synth(51);
    const Bytes inputs[] = {s.st, make_msa(s.st, 80, 2, 10), stx_file(stx_tracks_of(s.st, 80, 2, 10))};
    fx::Rng rng{12345};
    int parsed = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        Bytes b = inputs[iter % 3];
        const int edits = 1 + int(rng.next() % 6);
        for (int e = 0; e < edits; ++e) {
            const size_t at = (rng.next() % 3 == 0) ? rng.next() % std::min<size_t>(b.size(), 400) : rng.next() % b.size();
            switch (rng.next() % 4) {
            case 0: b[at] = uint8_t(rng.next()); break;
            case 1: b[at] ^= uint8_t(1u << (rng.next() % 8)); break;
            case 2: b.resize(std::max<size_t>(1, at)); break;
            default: if (b.size() > 1000) b.erase(b.begin() + long(at), b.begin() + long(std::min(b.size(), at + 1 + rng.next() % 64))); break;
            }
            if (b.empty()) b.push_back(0);
        }
        const StContainer c = detect_st_container(b.data(), b.size());
        StDisk d;
        std::string why;
        bool ok = false;
        if (c == StContainer::St) ok = read_st_image(b.data(), b.size(), d, why);
        else if (c == StContainer::Msa) ok = read_msa(b.data(), b.size(), d, why);
        else if (c == StContainer::Stx) ok = read_stx(b.data(), b.size(), d, why);
        if (ok) {
            ++parsed;
            std::vector<uint8_t> logical;
            std::vector<std::string> problems;
            const int spt = d.sectors_per_track();
            if (spt >= 1) d.logical_image(spt, logical, problems);
            d.nonstandard_tracks(std::max(spt, 1), 4);
        }
        // the whole import path too, on a small share of the inputs
        if (iter % 10 == 0) {
            std::vector<StInput> in = {{"fuzz", b, false}};
            std::vector<Message> m;
            diagnose_st_inputs(in, m);
        }
        // FAT12 on the mutated image
        FatInfo info;
        std::vector<FatEntry> entries;
        std::vector<std::string> warnings;
        fat12_list(b.data(), b.size(), info, entries, warnings, why);
    }
    CHECK(parsed > 100);
    // FAT fuzz on a valid volume
    FatDisk fd = make_fat();
    for (int iter = 0; iter < 2000; ++iter) {
        Bytes b = fd.img;
        for (int e = 0; e < 8; ++e) b[rng.next() % (14 * 512 + 4096)] = uint8_t(rng.next());
        FatInfo info;
        std::vector<FatEntry> entries;
        std::vector<std::string> warnings;
        std::string why;
        if (fat12_list(b.data(), b.size(), info, entries, warnings, why))
            for (const FatEntry& en : entries) {
                std::vector<uint8_t> out;
                fat12_read(b.data(), b.size(), en, out, why);
            }
    }
}

// ---- local data: real dumps ----
int test_files(const std::string& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return 77;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    int seen = 0;
    std::string kixx_sha;
    for (const fs::path& p : files) {
        std::ifstream f(p, std::ios::binary);
        Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (detect_st_container(data.data(), data.size()) == StContainer::None) continue;
        ++seen;
        std::vector<StInput> in = {{p.string(), data, false}};
        std::vector<Message> msgs;
        std::vector<StDiskReport> r = diagnose_st_inputs(in, msgs);
        CHECK(r.size() == 1);
        if (r.size() != 1) continue;
        std::printf("%s: %s, %dx%dx%d, sha1 %s, %s\n", p.filename().string().c_str(), st_container_name(r[0].container), r[0].cylinders, r[0].sides,
                    r[0].sectors_per_track, r[0].logical_sha1.c_str(), r[0].release ? r[0].release->id.c_str() : "unknown");
        if (r[0].release && r[0].release->id == "st-kixx") {
            CHECK(r[0].executable_boot && r[0].nonstandard_total == 0 && r[0].findings.empty() && r[0].rnc.size() == 11);
            CHECK(r[0].logical_sha1 == "323d6859635b6b82c86363db048b26e0f3df994c");
            kixx_sha = r[0].logical_sha1;
            StGame g = import_st_inputs(in);
            CHECK(g.status == Status::Ok && g.image.size() == 819200);
            if (g.status == Status::Ok) {
                const std::string dir2 = scratch_dir + "/assets-st-real";
                std::string err, reason;
                CHECK(write_st_assets(dir2, g, err) && verify_assets(dir2, reason));
            }
        }
        if (r[0].release && !r[0].release->importable) {
            StGame g = import_st_inputs(in);
            CHECK(g.status == Status::Unsupported && g.image.empty());
        }
    }
    return seen ? 0 : 77;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: x2_st_tests SCRATCH_DIR [--files DIR]\n"); return 2; }
    scratch_dir = argv[1];
    std::error_code ec;
    fs::remove_all(scratch_dir, ec);
    fs::create_directories(scratch_dir, ec);
    std::string files_dir;
    bool files_mode = false;
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--files") { files_mode = true; files_dir = i + 1 < argc ? argv[++i] : ""; }
    if (files_mode) {
        const int rc = test_files(files_dir);
        if (rc == 77) { std::printf("SKIPPED: no ST disk images under '%s'\n", files_dir.c_str()); return 77; }
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    test_detect_and_geometry();
    test_msa();
    test_stx();
    test_fat12();
    test_import_flow();
    test_diagnose_findings();
    test_registry();
    test_fuzz();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
