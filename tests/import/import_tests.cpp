// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Importer unit tests with synthetic inputs built here (no game data): an OFS disc 1 holding a fake XENON-II
// packed with the game's cruncher format, a fake "X2B " disc 2, wrong/truncated/oversized images, ADZ, ZIP
// (stored, deflate, nested, corrupted), unsupported formats, folders, the asset writer and manifest checks,
// and truncated/garbage inputs that must never crash.
//
//   x2_import_tests WORKDIR
#include "import/importer.h"
#include "import/sha1.h"

#include "container_fixtures.h"
#include "ipf_fixtures.h"
#include "flux_fixtures.h"
#include "lha_fixtures.h"
#include "import/lha.h"
#include "import/whdload.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h> // GetACP, for the UTF-8 code page check in test_non_ascii_paths
#endif

using namespace x2::import;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                \
    } while (0)

constexpr size_t kAdf = 901120, kImage = 0x3FC00;

void put32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (24 - 8 * i));
}
uint32_t get32(const Bytes& b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}
void append_le(Bytes& b, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

struct Rng {
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
};

// --- the XENON-II cruncher format, encoder side (literal runs of up to 1023 bytes + 2-byte matches at offset 1)

struct BitWriter {
    std::vector<uint8_t> bits; // in the order the depacker consumes them
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bits.push_back(uint8_t((v >> i) & 1));
    }
};

// Returns the packed stream (lowest address first); `image` receives what the depacker must produce.
Bytes crunch(size_t size, uint32_t seed, Bytes& image) {
    image.assign(size, 0);
    BitWriter w;
    Rng rng{seed};
    size_t a2 = size;
    while (true) {
        // The last run is all literals (entry code at the bottom); keep at least 4 bytes for it.
        size_t n = a2 <= 1000 ? a2 : a2 - 1000 - 2 < 4 ? a2 - 6 : 1000;
        if (n < 7) w.put(uint32_t(n), 3);
        else if (n <= 22) { w.put(7, 3); w.put(0, 1); w.put(uint32_t(n - 7), 4); }
        else { w.put(7, 3); w.put(1, 1); w.put(uint32_t(n), 10); }
        for (size_t i = 0; i < n; ++i) {
            --a2;
            // The image must start with bra.w $7032 (60 00 6C 30); everything else is noise.
            static const uint8_t entry[4] = {0x60, 0x00, 0x6C, 0x30};
            uint8_t v = a2 < 4 ? entry[a2] : uint8_t(rng.next() >> 24);
            image[a2] = v;
            w.put(v, 8);
        }
        if (a2 == 0) break;
        w.put(0, 2); // match: length 2, offset 1
        w.put(1, 8);
        image[a2 - 1] = image[a2];
        image[a2 - 2] = image[a2];
        a2 -= 2;
    }
    // Longs in read order: size, first bit buffer (31 data bits + sentinel), then 32 bits each, LSB first.
    std::vector<uint32_t> longs = {uint32_t(size)};
    size_t p = 0;
    uint32_t first = 0;
    for (int i = 0; i < 31 && p < w.bits.size(); ++i, ++p) first |= uint32_t(w.bits[p]) << i;
    longs.push_back(first | 0x80000000u);
    while (p < w.bits.size()) {
        uint32_t v = 0;
        for (int i = 0; i < 32 && p < w.bits.size(); ++i, ++p) v |= uint32_t(w.bits[p]) << i;
        longs.push_back(v);
    }
    Bytes stream(longs.size() * 4);
    for (size_t i = 0; i < longs.size(); ++i) put32(stream, stream.size() - 4 * (i + 1), longs[i]);
    return stream;
}

// Hunk executable: one CODE hunk = stub (lea 8(pc),a0 ; adda.l #len,a0) + stream.
Bytes make_exe(const Bytes& stream) {
    Bytes code = {0x41, 0xFA, 0x00, 0x08, 0xD1, 0xFC, 0, 0, 0, 0};
    put32(code, 6, uint32_t(stream.size()));
    code.insert(code.end(), stream.begin(), stream.end());
    while (code.size() % 4) code.push_back(0);
    Bytes exe(32);
    put32(exe, 0, 0x3F3);
    put32(exe, 4, 0);
    put32(exe, 8, 1);
    put32(exe, 12, 0);
    put32(exe, 16, 0);
    put32(exe, 20, 0x40000000u | uint32_t(code.size() / 4));
    put32(exe, 24, 0x3E9);
    put32(exe, 28, uint32_t(code.size() / 4));
    exe.insert(exe.end(), code.begin(), code.end());
    Bytes tail(4);
    put32(tail, 0, 0x3F2);
    exe.insert(exe.end(), tail.begin(), tail.end());
    return exe;
}

// --- a minimal OFS writer

struct Ofs {
    Bytes d = Bytes(kAdf, 0);
    uint32_t next = 882;
    Ofs() {
        std::memcpy(d.data(), "DOS\0", 4);
        put32(d, 8, 880);
        size_t r = 880 * 512;
        put32(d, r, 2);
        put32(d, r + 12, 72);
        put32(d, r + 508, 1);
        name(880, "SYNTHETIC");
    }
    void name(uint32_t blk, const std::string& n) {
        d[blk * 512 + 432] = uint8_t(n.size());
        std::memcpy(&d[blk * 512 + 433], n.data(), n.size());
    }
    void checksum(uint32_t blk) {
        put32(d, blk * 512 + 20, 0);
        uint32_t sum = 0;
        for (int i = 0; i < 128; ++i) sum += get32(d, blk * 512 + size_t(i) * 4);
        put32(d, blk * 512 + 20, uint32_t(0) - sum);
    }
    uint32_t alloc() { return next++; }
    static uint32_t hash(const std::string& n) {
        uint32_t h = uint32_t(n.size());
        for (char c : n) h = (h * 13 + uint32_t(std::toupper(static_cast<unsigned char>(c)))) & 0x7FF;
        return h % 72;
    }
    void add_file(const std::string& fname, const Bytes& data) {
        uint32_t hdr = alloc();
        size_t slot = 880 * 512 + 24 + 4 * hash(fname);
        put32(d, hdr * 512 + 496, get32(d, slot)); // hash chain
        put32(d, slot, hdr);
        checksum(880);
        size_t nblocks = (data.size() + 487) / 488;
        std::vector<uint32_t> blocks;
        for (size_t i = 0; i < nblocks; ++i) blocks.push_back(alloc());
        for (size_t i = 0; i < nblocks; ++i) {
            size_t b = blocks[i] * 512, len = std::min<size_t>(488, data.size() - i * 488);
            put32(d, b, 8);
            put32(d, b + 4, hdr);
            put32(d, b + 8, uint32_t(i + 1));
            put32(d, b + 12, uint32_t(len));
            put32(d, b + 16, i + 1 < nblocks ? blocks[i + 1] : 0);
            std::memcpy(&d[b + 24], &data[i * 488], len);
            checksum(blocks[i]);
        }
        // Header (72 pointers) then extension blocks (T_LIST) with 72 more each.
        uint32_t tbl = hdr;
        for (size_t first = 0; first < nblocks || tbl == hdr; first += 72) {
            size_t t = tbl * 512, n = std::min<size_t>(72, nblocks - first);
            put32(d, t, tbl == hdr ? 2 : 16);
            put32(d, t + 4, tbl);
            put32(d, t + 8, uint32_t(n));
            if (n) put32(d, t + 16, blocks[first]);
            for (size_t i = 0; i < n; ++i) put32(d, t + 308 - 4 * i, blocks[first + i]);
            put32(d, t + 500, tbl == hdr ? 880 : hdr);
            put32(d, t + 508, 0xFFFFFFFD);
            uint32_t ext = first + 72 < nblocks ? alloc() : 0;
            put32(d, t + 504, ext);
            if (tbl == hdr) { put32(d, t + 324, uint32_t(data.size())); name(hdr, fname); }
            checksum(tbl);
            if (!ext) break;
            tbl = ext;
        }
    }
};

Bytes g_disc1, g_disc2, g_image;

void build_discs() {
    Bytes stream = crunch(kImage + 0x1000, 1234, g_image);
    Ofs ofs;
    ofs.add_file("s", Bytes{'X', 'E', 'N', 'O', 'N', '-', 'I', 'I', '\n'});
    ofs.add_file("XENON-II", make_exe(stream));
    g_disc1 = ofs.d;
    g_image.resize(kImage);
    g_disc2.assign(kAdf, 0xE5);
    g_disc2[0] = g_disc2[1] = 0;
    std::memcpy(&g_disc2[2], "X2B ", 4);
    Rng r{99};
    for (size_t i = 11 * 512; i < 40 * 512; ++i) g_disc2[i] = uint8_t(r.next());
}

// --- container writers (stored deflate blocks; one real fixed/dynamic deflate member below)

Bytes deflate_stored(const Bytes& data) {
    Bytes out;
    size_t p = 0;
    do {
        size_t n = std::min<size_t>(65535, data.size() - p);
        out.push_back(p + n == data.size() ? 1 : 0);
        append_le(out, uint32_t(n), 2);
        append_le(out, uint32_t(~n & 0xFFFF), 2);
        out.insert(out.end(), data.begin() + long(p), data.begin() + long(p + n));
        p += n;
    } while (p < data.size());
    return out;
}

Bytes gzip(const Bytes& data) {
    Bytes out = {0x1F, 0x8B, 8, 8, 0, 0, 0, 0, 0, 3};
    for (char c : std::string("disc.adf")) out.push_back(uint8_t(c));
    out.push_back(0);
    Bytes body = deflate_stored(data);
    out.insert(out.end(), body.begin(), body.end());
    append_le(out, crc32(data.data(), data.size()), 4);
    append_le(out, uint32_t(data.size()), 4);
    return out;
}

// "Xenon 2 import test readme..." x3 (252 bytes), compressed by zlib (raw deflate, level 9).
const uint8_t kReadmeDeflate[] = {
    0x8b, 0x48, 0xcd, 0xcb, 0xcf, 0x53, 0x30, 0x52, 0xc8, 0xcc, 0x2d, 0xc8, 0x2f, 0x2a, 0x51, 0x28,
    0x49, 0x2d, 0x2e, 0x51, 0x28, 0x4a, 0x4d, 0x4c, 0xc9, 0x4d, 0xd5, 0x53, 0x88, 0xc0, 0x23, 0xe7,
    0x54, 0x94, 0x99, 0x97, 0xae, 0x50, 0x99, 0x5f, 0x5a, 0xa4, 0x90, 0x5f, 0x9e, 0xa7, 0x90, 0x92,
    0x59, 0x9c, 0x0d, 0x54, 0x97, 0x98, 0x9e, 0x5a, 0xac, 0xc8, 0x15, 0x31, 0x44, 0xcc, 0x04, 0x00};
const uint32_t kReadmeSize = 252, kReadmeCrc = 0xc4325589;

struct ZipMember {
    std::string name;
    Bytes data;          // uncompressed
    int method = 0;      // 0 stored, 8 deflate (stored blocks)
    Bytes raw;           // if non-empty: precompressed deflate data for method 8
    uint32_t size = 0, crc = 0;
};

Bytes zip(const std::vector<ZipMember>& members) {
    Bytes out, cd;
    for (const ZipMember& m : members) {
        Bytes body = !m.raw.empty() ? m.raw : m.method == 8 ? deflate_stored(m.data) : m.data;
        uint32_t crc = m.raw.empty() ? crc32(m.data.data(), m.data.size()) : m.crc;
        uint32_t usize = m.raw.empty() ? uint32_t(m.data.size()) : m.size;
        uint32_t off = uint32_t(out.size());
        append_le(out, 0x04034b50, 4);
        append_le(out, 20, 2);
        append_le(out, 0, 2);
        append_le(out, uint32_t(m.method), 2);
        append_le(out, 0, 4);
        append_le(out, crc, 4);
        append_le(out, uint32_t(body.size()), 4);
        append_le(out, usize, 4);
        append_le(out, uint32_t(m.name.size()), 2);
        append_le(out, 0, 2);
        out.insert(out.end(), m.name.begin(), m.name.end());
        out.insert(out.end(), body.begin(), body.end());
        append_le(cd, 0x02014b50, 4);
        append_le(cd, 20, 2);
        append_le(cd, 20, 2);
        append_le(cd, 0, 2);
        append_le(cd, uint32_t(m.method), 2);
        append_le(cd, 0, 4);
        append_le(cd, crc, 4);
        append_le(cd, uint32_t(body.size()), 4);
        append_le(cd, usize, 4);
        append_le(cd, uint32_t(m.name.size()), 2);
        append_le(cd, 0, 2);
        append_le(cd, 0, 2);
        append_le(cd, 0, 2);
        append_le(cd, 0, 2);
        append_le(cd, 0, 4);
        append_le(cd, off, 4);
        cd.insert(cd.end(), m.name.begin(), m.name.end());
    }
    uint32_t cd_off = uint32_t(out.size());
    out.insert(out.end(), cd.begin(), cd.end());
    append_le(out, 0x06054b50, 4);
    append_le(out, 0, 4);
    append_le(out, uint32_t(members.size()), 2);
    append_le(out, uint32_t(members.size()), 2);
    append_le(out, uint32_t(cd.size()), 4);
    append_le(out, cd_off, 4);
    append_le(out, 0, 2);
    return out;
}

bool has_message(const Result& r, Message::Level level, const std::string& needle) {
    for (const Message& m : r.messages)
        if (m.level == level && m.text.find(needle) != std::string::npos) return true;
    return false;
}

void dump(const char* what, const Result& r) {
    std::printf("[%s] -> %s\n", what, status_name(r.status));
    for (const Message& m : r.messages) std::printf("    %s\n", format_message(m).c_str());
}

Result run(const char* what, std::vector<Input> in) {
    Result r = import_inputs(in);
    dump(what, r);
    return r;
}

void write_file(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void test_sha1_crc() {
    const uint8_t* abc = reinterpret_cast<const uint8_t*>("abc");
    CHECK(sha1_hex(abc, 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1_hex(nullptr, 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    std::string s56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha1_hex(reinterpret_cast<const uint8_t*>(s56.data()), s56.size()) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926);
}

void test_raw_pair() {
    Result r = run("raw disc 1 + disc 2", {{"d1.adf", g_disc1}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Ok);
    CHECK(r.game_image == g_image);
    CHECK(r.disc2 == g_disc2);
    CHECK(has_message(r, Message::Warn, "disc 1 is not a recognised image"));
    CHECK(has_message(r, Message::Warn, "XENON-II executable differs"));
    CHECK(has_message(r, Message::Warn, "disc 2 differs"));
    CHECK(!has_message(r, Message::Info, "trainer loader")); // only said for the recognised BS1 disks
    // Order does not matter, discs are found by content.
    Result r2 = import_inputs({{"b", g_disc2}, {"a", g_disc1}});
    CHECK(r2.status == Status::Ok && r2.game_image == g_image);
}

void test_missing_and_wrong() {
    Result r = run("disc 1 only", {{"d1.adf", g_disc1}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "disc 2 not found"));
    r = run("disc 2 only", {{"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "disc 1 not found"));
    Bytes junk(kAdf);
    Rng rng{7};
    for (auto& b : junk) b = uint8_t(rng.next());
    r = run("880 KB of noise", {{"noise.adf", junk}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "neither an AmigaDOS disc"));
    // An AmigaDOS disc without the game: a plain file system with an unrelated file.
    Ofs other;
    other.add_file("README", Bytes(5000, 'x'));
    r = run("other AmigaDOS disc", {{"other.adf", other.d}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "without the XENON-II executable"));
    // A differently packed Xenon 2 executable (e.g. the VF one-disk crunch): unsupported.
    Ofs vf;
    vf.add_file("XENON_II", Bytes(20000, 0x4E));
    r = run("differently packed XENON_II", {{"vf.adf", vf.d}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Unsupported);
    CHECK(has_message(r, Message::Error, "packed differently"));
    // DOS boot block without a file system (custom trackloader disc).
    Bytes boot(kAdf, 0);
    std::memcpy(boot.data(), "DOS\0", 4);
    r = run("trackloader disc", {{"tl.adf", boot}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "no readable file system"));
}

void test_truncated_and_corrupt() {
    Bytes shortd(g_disc1.begin(), g_disc1.begin() + 500000);
    Result r = run("truncated disc 1", {{"short.adf", shortd}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "truncated disk image (500000 bytes"));
    Bytes longd = g_disc2;
    longd.resize(kAdf + 512);
    r = run("oversized disc 2", {{"d1.adf", g_disc1}, {"long.adf", longd}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "oversized"));
    // A data block of XENON-II damaged: OFS checksum catches it.
    Bytes bad = g_disc1;
    bad[900 * 512 + 100] ^= 0xFF;
    r = run("damaged data block", {{"bad.adf", bad}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "corrupted"));
    // Packed stream damaged with valid checksums: the depacker must reject it, not crash.
    Rng rng{3};
    int rejected = 0;
    for (int i = 0; i < 20; ++i) {
        Bytes stream = crunch(kImage + 64, 77 + uint32_t(i), g_image /*unused copy*/);
        for (int k = 0; k < 8; ++k) stream[rng.next() % stream.size()] ^= uint8_t(1 + rng.next() % 255);
        Ofs o;
        o.add_file("XENON-II", make_exe(stream));
        Result rr = import_inputs({{"x", o.d}, {"y", g_disc2}});
        if (rr.status != Status::Ok) ++rejected;
    }
    std::printf("[damaged packed streams] %d/20 rejected (others decode to a different image)\n", rejected);
    build_discs(); // restore g_image
}

void test_adz_zip() {
    Result r = run("ADZ disc 1 + raw disc 2", {{"d1.adz", gzip(g_disc1)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Ok && r.game_image == g_image);
    ZipMember readme{"readme.txt", {}, 8, Bytes(std::begin(kReadmeDeflate), std::end(kReadmeDeflate)), kReadmeSize, kReadmeCrc};
    Bytes z = zip({{"Disk 2.adf", g_disc2, 8}, {"Disk 1.adf", g_disc1, 0}, readme, {"dir/", {}, 0}});
    r = run("zip with both discs + readme", {{"pair.zip", z}});
    CHECK(r.status == Status::Ok && r.game_image == g_image && r.disc2 == g_disc2);
    CHECK(has_message(r, Message::Info, "readme.txt: ignored"));
    CHECK(!has_message(r, Message::Error, "readme"));
    Bytes nested = zip({{"inner1.zip", zip({{"a.adz", gzip(g_disc1), 0}}), 0}, {"inner2.zip", zip({{"b.adf", g_disc2, 8}}), 8}});
    r = run("nested zips (adz inside zip inside zip)", {{"outer.zip", nested}});
    CHECK(r.status == Status::Ok && r.game_image == g_image);
    // Corrupted member (CRC) and one disc per path.
    Bytes zc = zip({{"d1.adf", g_disc1, 0}});
    zc[200000] ^= 0x55;
    r = run("zip with a corrupted member", {{"bad.zip", zc}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput);
    CHECK(has_message(r, Message::Error, "CRC mismatch"));
    Bytes enc = zip({{"d1.adf", g_disc1, 0}});
    enc[6] |= 1; // encrypted flag in the local header only: the central directory still says plain
    size_t cd = enc.size() - 22 - 46 - 6;
    enc[cd + 8] |= 1;
    r = run("encrypted zip member", {{"enc.zip", enc}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Unsupported);
    CHECK(has_message(r, Message::Error, "encrypted"));
    r = run("empty zip", {{"empty.zip", zip({})}});
    CHECK(r.status == Status::BadInput);
}

void test_unsupported_formats() {
    auto with_magic = [](const char* m, size_t size) {
        Bytes b(size, 0);
        std::memcpy(b.data(), m, std::strlen(m));
        return b;
    };
    // DMS and extended ADF are read now: a header-only file is a damaged one, not an unsupported format.
    Result r = run("DMS, empty", {{"x.dms", with_magic("DMS!", 300000)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "DMS archive"));
    r = run("IPF, empty", {{"x.ipf", with_magic("CAPS", 400000)}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "IPF image"));
    r = run("extended ADF, empty", {{"x.adf", with_magic("UAE-1ADF", 1000000)}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "extended ADF"));
    r = run("HD ADF", {{"x.adf", Bytes(2 * kAdf, 0)}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "HD disk image"));
    r = run("text file", {{"notes.txt", Bytes(100, 'a')}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "not a disk image or archive"));
    // Flux images are read now: a header-only file is a damaged one.
    r = run("SCP", {{"x.scp", with_magic("SCP", 5000)}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "SCP image"));
    r = run("HFE", {{"x.hfe", with_magic("HXCPICFE", 5000)}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "HFE image"));
    Bytes kf = {0x0D, 0x04, 0x20, 0x00};
    const char* info = "name=KryoFlux DiskSystem";
    kf.insert(kf.end(), info, info + std::strlen(info));
    r = run("KryoFlux", {{"track00.0.raw", kf}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "truncated"));
    // LHA archives are read now: a header fragment is not an archive.
    r = run("LHA, fragment", {{"x.lha", Bytes{0x1A, 0, '-', 'l', 'h', '5', '-', 0, 0, 0}}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "LHA archive"));
}

// A boot block whose CRC-16/ARC is `target`: random bytes, the last two chosen to reach it.
Bytes forged_boot(uint16_t target, uint32_t seed) {
    Rng r{seed};
    Bytes b(1024);
    for (auto& v : b) v = uint8_t(r.next());
    for (uint32_t tail = 0; tail < 65536; ++tail) {
        b[1022] = uint8_t(tail >> 8);
        b[1023] = uint8_t(tail);
        if (lha_crc16(b.data(), b.size()) == target) return b;
    }
    return b;
}

Bytes retail_disk1(const char* version_crc_name, uint16_t crc, uint32_t seed) {
    (void)version_crc_name;
    Rng r{seed};
    Bytes d(kAdf);
    for (auto& v : d) v = uint8_t(r.next());
    Bytes boot = forged_boot(crc, seed + 1);
    std::memcpy(d.data(), boot.data(), boot.size());
    return d;
}

// A WHDLoad slave: HUNK_HEADER, one HUNK_CODE with `moveq #-1,d0 ; rts`, "WHDLOADS" and the structure up to ws_info.
Bytes make_slave(uint16_t version, const std::string& name, const std::string& copy, const std::string& info) {
    auto be16 = [](Bytes& b, uint32_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); };
    auto be32 = [&](Bytes& b, uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); };
    Bytes code = {0x70, 0xFF, 0x4E, 0x75};
    code.insert(code.end(), {'W', 'H', 'D', 'L', 'O', 'A', 'D', 'S'});
    be16(code, version);
    be16(code, 0x0002);
    be32(code, 0xC6000); // base memory
    be32(code, 0);       // exec install
    be16(code, 0x94);    // game loader
    be16(code, 0x2A);    // current dir
    be16(code, 0);       // don't cache
    code.push_back('Y');
    code.push_back(0);
    be32(code, 0); // expansion memory
    size_t at = code.size() + 6;
    be16(code, uint32_t(at));
    be16(code, uint32_t(at + name.size() + 1));
    be16(code, uint32_t(at + name.size() + copy.size() + 2));
    for (const std::string* st : {&name, &copy, &info}) {
        code.insert(code.end(), st->begin(), st->end());
        code.push_back(0);
    }
    while (code.size() % 4) code.push_back(0);
    Bytes b;
    be32(b, 0x3F3);
    be32(b, 0);
    be32(b, 1);
    be32(b, 0);
    be32(b, 0);
    be32(b, uint32_t(code.size() / 4));
    be32(b, 0x3E9);
    be32(b, uint32_t(code.size() / 4));
    b.insert(b.end(), code.begin(), code.end());
    be32(b, 0x3F2);
    return b;
}

void test_lha_and_whdload() {
    using namespace lhafx;
    // The disc pair inside LHA archives of every header level, stored and -lh5- compressed (a hand-built stream
    // cannot hold 880 KB, so the compressed case carries a small file next to the stored discs).
    for (int level = 0; level <= 3; ++level) {
        Member a = stored("Disk.1", g_disc1, level, "Xenon2/"), b = stored("Disk.2", g_disc2, level, "Xenon2/");
        Bytes arc = archive({stored("ReadMe", Bytes{'h', 'i'}, level, "Xenon2/"), a, b});
        Result r = run("pair in an LHA archive", {{"pair.lha", arc}});
        CHECK(r.status == Status::Ok && r.game_image == g_image && r.disc2 == g_disc2);
        // inside a zip, inside a folder-like list of inputs
        Result rz = run("LHA in a zip", {{"x.zip", zip({{"inner/pair.lha", arc, 8}})}});
        CHECK(rz.status == Status::Ok && rz.game_image == g_image);
    }
    // A damaged member and an unsupported method are reported with the member's name.
    {
        Member bad = stored("Disk.1", g_disc1, 1);
        bad.bad_crc = true;
        Result r = run("LHA, CRC mismatch", {{"x.lha", archive({bad, stored("Disk.2", g_disc2, 1)})}});
        CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "CRC mismatch"));
        Member u = stored("Disk.1", Bytes(100, 1), 0);
        u.method = "-lh2-";
        r = run("LHA, unsupported method", {{"x.lha", archive({u})}});
        CHECK(has_message(r, Message::Error, "-lh2-") && has_message(r, Message::Error, "not supported"));
        Bytes full = archive({stored("Disk.2", g_disc2, 0)});
        r = run("LHA, truncated", {{"x.lha", Bytes(full.begin(), full.begin() + 5000)}});
        CHECK(has_message(r, Message::Error, "truncated"));
    }
    // Retail disk 1 images (WHDLoad Disk.1 or plain ADFs) are recognised by their boot block.
    struct Retail { uint16_t crc; const char* version; bool two_disk; };
    for (Retail rt : {Retail{0x7f8f, "v1", true}, Retail{0xc0fb, "v2", true}, Retail{0x7ea7, "v3", true}, Retail{0x7e53, "v4", false},
                      Retail{0xbdce, "v5", false}, Retail{0x6827, "v6", false}}) {
        Bytes d1 = retail_disk1("", rt.crc, rt.crc);
        const RetailDisk1* id = identify_retail_disk1(d1.data(), d1.size());
        CHECK(id && std::string(id->version) == rt.version && id->two_disk == rt.two_disk);
        // with only disc 2 (as in a WHDLoad install): unsupported, said precisely
        Result r = run("retail disk 1 + canonical disc 2", {{"Disk.1", d1}, {"Disk.2", g_disc2}});
        CHECK(r.status == Status::Unsupported);
        CHECK(has_message(r, Message::Info, std::string("floppy version ") + rt.version));
        CHECK(has_message(r, Message::Error, rt.two_disk ? "retail disk 1 recognised" : "one-disk release recognised"));
        CHECK(r.game_image.empty());
        // with a supported disc 1 as well: imported, the retail disk is noted as not used
        Result r2 = run("retail disk 1 beside a supported disc 1", {{"Disk.1", d1}, {"d1.adf", g_disc1}, {"Disk.2", g_disc2}});
        CHECK(r2.status == Status::Ok && r2.game_image == g_image);
        CHECK(has_message(r2, Message::Info, "not used: a supported disc 1 was given"));
    }
    // The one-disk custom `Disk.1` of the Power Pack has its own size; an unknown boot block of that size is not retail.
    {
        Bytes pp(1085184, 7);
        Bytes boot = forged_boot(0x6827, 5);
        std::memcpy(pp.data(), boot.data(), boot.size());
        CHECK(identify_retail_disk1(pp.data(), pp.size()) != nullptr);
        Bytes other(1085184, 7);
        CHECK(identify_retail_disk1(other.data(), other.size()) == nullptr);
        Bytes plain(kAdf, 7);
        CHECK(identify_retail_disk1(plain.data(), plain.size()) == nullptr);
        CHECK(identify_retail_disk1(g_disc1.data(), g_disc1.size()) == nullptr);
        CHECK(identify_retail_disk1(g_disc1.data(), 100) == nullptr);
    }
    // WHDLoad slaves.
    {
        WhdSlave sl;
        Bytes floppy = make_slave(10, "Xenon 2 - Megablast", "1988 The Bitmap Brothers", "installed & fixed by Wepl\nVersion 1.9 Floppy (11.05.2005)");
        CHECK(parse_whdload_slave(floppy.data(), floppy.size(), sl));
        CHECK(sl.version == 10 && sl.name == "Xenon 2 - Megablast" && sl.copyright == "1988 The Bitmap Brothers");
        CHECK(sl.info.find("Version 1.9 Floppy") != std::string::npos);
        Result r = run("WHDLoad floppy slave", {{"X.Slave", floppy}, {"d1.adf", g_disc1}, {"d2.adf", g_disc2}});
        CHECK(r.status == Status::Ok && has_message(r, Message::Info, "WHDLoad install of the floppy release"));
        CHECK(has_message(r, Message::Info, "Version 1.9 Floppy (11.05.2005)"));
        Bytes cdtv = make_slave(10, "Xenon 2 - Megablast", "1991 The Bitmap Brothers", "installed & fixed by Wepl\nVersion 1.6 CDTV (11.05.2005)");
        r = run("WHDLoad CDTV slave", {{"X.Slave", cdtv}});
        CHECK(r.status == Status::Unsupported || r.status == Status::BadInput);
        CHECK(has_message(r, Message::Error, "CDTV release has a different game executable"));
        Bytes demo = make_slave(10, "Xenon II Demo", "1989 The Bitmap Brothers", "adapted & fixed by JOTD\nVersion 1.0");
        r = run("WHDLoad demo slave", {{"D.Slave", demo}});
        CHECK(has_message(r, Message::Error, "demo is a different, smaller program"));
        // not slaves: truncated, wrong magic, damaged strings
        CHECK(!parse_whdload_slave(floppy.data(), 30, sl));
        Bytes broken = floppy;
        broken[38] ^= 0xFF; // inside "WHDLOADS" (24 bytes of hunk header, 8 of HUNK_CODE, 4 of code)
        CHECK(!parse_whdload_slave(broken.data(), broken.size(), sl));
        Bytes oob = make_slave(10, "n", "c", "i");
        oob[24 + 8 + 36] = 0xFF; // high byte of the ws_name offset: past the end, so no name, but still a slave
        CHECK(parse_whdload_slave(oob.data(), oob.size(), sl) && sl.name.empty());
        for (size_t cut = 0; cut < floppy.size(); ++cut) parse_whdload_slave(floppy.data(), cut, sl);
    }
}

// A DMS archive (modes none and simple alternating) of an 880 KB ADF.
Bytes dms_of_adf(const Bytes& adf, uint16_t geninfo = 0, size_t cylinders = 80) {
    std::vector<Bytes> tracks;
    for (size_t c = 0; c < cylinders; ++c) {
        Bytes un(adf.begin() + long(c * 11264), adf.begin() + long((c + 1) * 11264));
        fx::DmsTrack t{uint16_t(c), 0, 0, un, un};
        if (c % 2) { t.mode = 1; t.packed = fx::rle_encode(un); }
        tracks.push_back(fx::dms_track_bytes(t));
    }
    return fx::dms_archive(tracks, geninfo);
}

Bytes ext_adf_of(const Bytes& adf, bool raw) {
    std::vector<fx::ExtTrack> tr;
    for (int t = 0; t < 160; ++t) {
        const uint8_t* src = &adf[size_t(t) * 5632];
        if (raw) {
            fx::MfmTrack m = fx::mfm_encode_track(src, t, uint32_t(t * 13), 7);
            tr.push_back({1, m.bytes, m.bit_length, 1});
        } else {
            tr.push_back({0, Bytes(src, src + 5632)});
        }
    }
    return fx::ext_adf(tr);
}

void test_dms_and_extended_adf() {
    Result raw = import_inputs({{"d1.adf", g_disc1}, {"d2.adf", g_disc2}});
    CHECK(raw.status == Status::Ok);

    // DMS: both discs, then nested in a zip, then mixed with a raw ADF.
    Bytes d1 = dms_of_adf(g_disc1), d2 = dms_of_adf(g_disc2);
    Result r = run("DMS pair", {{"d1.dms", d1}, {"d2.dms", d2}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    CHECK(r.disc1_sha1 == raw.disc1_sha1 && r.disc2_sha1 == raw.disc2_sha1 && r.exe_sha1 == raw.exe_sha1);
    CHECK(has_message(r, Message::Info, "d1.dms (DMS unpacked)"));
    r = run("zip of DMS + raw ADF", {{"set.zip", zip({{"d1.dms", d1, 8}})}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    // DMS inside an ADZ-free folder-like context still works when the DMS holds a non-Xenon disc: it is reported as such.
    Bytes other(kAdf, 0);
    std::memcpy(other.data(), "DOS", 3);
    r = run("DMS of an unrelated disc", {{"o.dms", dms_of_adf(other)}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "o.dms (DMS unpacked)"));

    // Damaged, unsupported and partial DMS.
    Bytes bad = d1;
    bad[100] ^= 0x40;
    r = run("DMS, damaged", {{"d1.dms", bad}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "CRC mismatch"));
    r = run("DMS, encrypted", {{"d1.dms", dms_of_adf(g_disc1, 2)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "encrypted DMS archives are not supported"));
    r = run("DMS, partial", {{"d1.dms", dms_of_adf(g_disc1, 0, 40)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "does not hold a complete 880 KB disk"));
    r = run("DMS, truncated", {{"d1.dms", Bytes(d1.begin(), d1.begin() + long(d1.size() / 2))}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "truncated"));

    // Extended ADF with normal tracks, with every track raw MFM, and with a protection track.
    r = run("extended ADF, normal", {{"d1.adf", ext_adf_of(g_disc1, false)}, {"d2.adf", ext_adf_of(g_disc2, false)}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    CHECK(has_message(r, Message::Info, "d1.adf (extended ADF)"));
    r = run("extended ADF, raw MFM", {{"d1.adf", ext_adf_of(g_disc1, true)}, {"d2.adf", ext_adf_of(g_disc2, true)}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    {
        std::vector<fx::ExtTrack> tr;
        for (int t = 0; t < 160; ++t) tr.push_back({0, Bytes(&g_disc1[size_t(t) * 5632], &g_disc1[size_t(t) * 5632] + 5632)});
        tr[0] = {1, Bytes(12500, 0x55), 100000, 1};
        r = run("extended ADF, protection track", {{"d1.adf", fx::ext_adf(tr)}, {"d2.adf", g_disc2}});
        CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "track 0 (cylinder 0, side 0): raw MFM"));
        CHECK(has_message(r, Message::Error, "protected disks cannot be imported yet"));
    }
    Bytes shortext = ext_adf_of(g_disc1, false);
    shortext.resize(shortext.size() - 100);
    r = run("extended ADF, truncated", {{"d1.adf", shortext}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "extended ADF"));
}

// IPF images of the same synthetic discs import exactly like the raw ADFs; a disc with a protection track is
// recognised and refused; damaged IPFs are bad input.
void test_ipf() {
    Result raw = import_inputs({{"d1.adf", g_disc1}, {"d2.adf", g_disc2}});
    CHECK(raw.status == Status::Ok);
    Bytes i1 = fx::ipf_of_adf(g_disc1), i2 = fx::ipf_of_adf(g_disc2);
    Result r = run("IPF pair", {{"d1.ipf", i1}, {"d2.ipf", i2}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    CHECK(r.disc1_sha1 == raw.disc1_sha1 && r.disc2_sha1 == raw.disc2_sha1 && r.exe_sha1 == raw.exe_sha1);
    CHECK(has_message(r, Message::Info, "d1.ipf (IPF decoded)"));
    r = run("IPF + raw ADF", {{"d1.ipf", i1}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    r = run("zip of IPF", {{"set.zip", zip({{"d1.ipf", i1, 8}, {"d2.ipf", i2, 8}})}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);

    fx::IpfBuild prot;
    prot.raw_track = 1;
    r = run("IPF, protection track", {{"d1.ipf", fx::ipf_of_adf(g_disc1, prot)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "copy-protected or non-standard tracks"));
    CHECK(has_message(r, Message::Error, "track 1 (cylinder 0, side 1)"));
    CHECK(has_message(r, Message::Error, "import of protected disks not available yet"));
    fx::IpfBuild noise;
    noise.noise_track = 100;
    r = run("IPF, unformatted track", {{"d1.ipf", fx::ipf_of_adf(g_disc1, noise)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "track 100"));

    Bytes bad = i1;
    bad[200] ^= 0x20;
    r = run("IPF, damaged", {{"d1.ipf", bad}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "IPF image: CRC error"));
    r = run("IPF, truncated", {{"d1.ipf", Bytes(i1.begin(), i1.begin() + long(i1.size() / 2))}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "IPF image"));
    // an IPF of an unrelated disc is reported like any other
    Bytes other(kAdf, 0);
    std::memcpy(other.data(), "DOS", 3);
    r = run("IPF of an unrelated disc", {{"o.ipf", fx::ipf_of_adf(other)}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "o.ipf (IPF decoded)"));
}

// Whole discs as flux images: SCP (one file per disc), a KryoFlux stream set (one folder per disc) and HFE import
// like the plain ADF pair. Random-free: the discs are the fixtures' own, MFM-encoded and turned into flux.
std::vector<FluxTrack> flux_of_adf(const Bytes& adf, uint32_t seed, double jitter, int revs, std::vector<fx::Timeline>* timelines = nullptr) {
    std::vector<FluxTrack> out;
    fx::FluxModel m;
    m.jitter = jitter;
    for (int t = 0; t < 160; ++t) {
        m.seed = seed + uint32_t(t) * 7u;
        fx::MfmTrack mt = fx::mfm_encode_track(&adf[size_t(t) * 5632], t, uint32_t(t) * 1237u, 8 * uint32_t(t % 3));
        fx::Timeline tl = fx::make_timeline(mt, revs + 1, 4000, 4000, m);
        out.push_back(fx::scp_style_revs(tl));
        if (timelines) timelines->push_back(std::move(tl));
    }
    return out;
}

void test_flux_images() {
    Result raw = import_inputs({{"d1.adf", g_disc1}, {"d2.adf", g_disc2}});
    CHECK(raw.status == Status::Ok);

    // SCP.
    Bytes s1 = fx::scp_file(flux_of_adf(g_disc1, 1, 0.04, 2), 2), s2 = fx::scp_file(flux_of_adf(g_disc2, 2, 0.04, 2), 2);
    Result r = run("SCP pair", {{"d1.scp", s1}, {"d2.scp", s2}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    CHECK(r.disc1_sha1 == raw.disc1_sha1 && r.disc2_sha1 == raw.disc2_sha1);
    CHECK(has_message(r, Message::Info, "d1.scp: SCP flux image: 160 tracks, 2 revolutions each"));
    r = run("zip of SCP + raw ADF", {{"set.zip", zip({{"d1.scp", s1, 8}})}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);

    // A KryoFlux stream set per disc, in two folders of one input list (as a folder import delivers them).
    std::vector<Input> kf;
    for (int disc = 0; disc < 2; ++disc) {
        std::vector<fx::Timeline> tls;
        flux_of_adf(disc ? g_disc2 : g_disc1, 10 + uint32_t(disc), 0.04, 2, &tls);
        for (int t = 0; t < 160; ++t) {
            char name[64];
            std::snprintf(name, sizeof name, "Xenon2/disk%d/track%02d.%d.raw", disc + 1, t / 2, t % 2);
            kf.push_back({name, fx::kryoflux_stream(tls[size_t(t)]), true});
        }
    }
    r = import_inputs(kf);
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    CHECK(has_message(r, Message::Info, "Xenon2/disk1 (KryoFlux streams)"));
    // One folder missing a track is an error naming the track; the other disc is not affected.
    std::vector<Input> partial = kf;
    partial.erase(partial.begin() + 7);
    r = import_inputs(partial);
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "track 7"));

    // HFE (bit cells, no flux): every track is whole bytes here.
    auto hfe_of = [&](const Bytes& adf) {
        std::vector<fx::MfmTrack> s0, s1;
        for (int c = 0; c < 80; ++c) {
            s0.push_back(fx::mfm_encode_track(&adf[size_t(2 * c) * 5632], 2 * c, 0, 0));
            s1.push_back(fx::mfm_encode_track(&adf[size_t(2 * c + 1) * 5632], 2 * c + 1, 0, 0));
        }
        return fx::hfe_file(s0, s1);
    };
    r = run("HFE pair", {{"d1.hfe", hfe_of(g_disc1)}, {"d2.hfe", hfe_of(g_disc2)}});
    CHECK(r.status == Status::Ok && r.game_image == raw.game_image && r.disc2 == raw.disc2);
    CHECK(has_message(r, Message::Info, "d1.hfe (HFE decoded)"));

    // A protected (raw, non-Amiga) track stays visible: reported as protected, not silently zeroed.
    std::vector<FluxTrack> prot = flux_of_adf(g_disc1, 1, 0.02, 2);
    {
        fx::Rng rr{5};
        FluxTrack junk;
        for (int rev = 0; rev < 2; ++rev) {
            FluxRev fr;
            for (int i = 0; i < 30000; ++i) fr.ns.push_back(uint32_t(4000 + 2000 * (rr.next() % 3)));
            junk.revs.push_back(fr);
        }
        prot[1] = junk;
    }
    r = run("SCP, protection track", {{"d1.scp", fx::scp_file(prot, 2)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "track 1"));
    CHECK(has_message(r, Message::Error, "copy-protected tracks"));
    // A damaged track: the error names the sector.
    std::vector<FluxTrack> dmg = flux_of_adf(g_disc1, 1, 0.02, 2);
    for (FluxRev& rev : dmg[2].revs) for (size_t i = rev.ns.size() / 3; i < rev.ns.size() / 3 + 3000; ++i) rev.ns[i] = 3000 + uint32_t(i % 5) * 700;
    r = run("SCP, damaged track", {{"d1.scp", fx::scp_file(dmg, 2)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "track 2 sector"));
    // Truncated or corrupt containers.
    r = run("SCP, truncated", {{"d1.scp", Bytes(s1.begin(), s1.begin() + long(s1.size() / 2))}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "SCP image"));
    Bytes hfe_bad = hfe_of(g_disc1);
    hfe_bad[12] = 0xF4; hfe_bad[13] = 0x01;
    r = run("HFE, high density", {{"d1.hfe", hfe_bad}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "high-density"));
    r = run("KryoFlux, unnamed", {{"stream.raw", kf[0].data}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "trackNN.S.raw"));
}

// Reading disc images from, and writing assets into, a directory whose name is not ASCII. Paths are UTF-8
// std::strings everywhere, so on Windows this works only while the process runs with the UTF-8 code page: with
// the legacy ANSI code page the bytes below are already '?' by the time main() sees argv. docs/PORTING.md.
void test_non_ascii_paths(const fs::path& work) {
#ifdef _WIN32
    CHECK(GetACP() == 65001); // packaging/win-utf8.manifest is embedded in this executable
#endif
    const std::string name = "\xc3\x9cml\xc3\xa4ut-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"; // "Ümläut-日本語"
    fs::path dir = work / "nonascii" / fs::path(name);
    fs::remove_all(work / "nonascii");
    fs::create_directories(dir);
    write_file(dir / "disc1.adz", gzip(g_disc1));
    write_file(dir / "disc2.adf", g_disc2);
    Result r = import_paths({(dir / "disc1.adz").string(), (dir / "disc2.adf").string()});
    CHECK(r.status == Status::Ok && r.game_image == g_image);

    std::string assets = (dir / "assets").string(), err, why;
    CHECK(write_assets(assets, r, err));
    CHECK(verify_assets(assets, why));
    CHECK(read_file(dir / "assets" / "game_0400.bin") == g_image);
}

void test_files_and_assets(const fs::path& work) {
    fs::path src = work / "src", sub = src / "sub", assets = work / "data" / "assets";
    fs::remove_all(work);
    fs::create_directories(sub);
    write_file(src / "disc1.adz", gzip(g_disc1));
    write_file(sub / "disc2.adf", g_disc2);
    write_file(src / "info.nfo", Bytes(300, 'n'));
    Result r = import_paths({src.string()});
    dump("folder (disc 2 in a subfolder)", r);
    CHECK(r.status == Status::Ok && r.game_image == g_image);
    r = import_paths({(work / "nope").string()});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "no such file"));

    std::string err, why;
    CHECK(!verify_assets(assets.string(), why));
    CHECK(write_assets(assets.string(), import_paths({(src / "disc1.adz").string(), (sub / "disc2.adf").string()}), err));
    CHECK(read_file(assets / "game_0400.bin") == g_image);
    CHECK(read_file(assets / "disc2.bin") == g_disc2);
    CHECK(!fs::exists(assets / "game_0400.bin.tmp"));
    std::vector<std::string> sources;
    CHECK(verify_assets(assets.string(), why, &sources));
    CHECK(sources.size() == 2);
    // A changed file no longer matches its manifest; ensure_imported re-imports from the recorded sources.
    Bytes g = g_image;
    g[1000] ^= 1;
    write_file(assets / "game_0400.bin", g);
    CHECK(!verify_assets(assets.string(), why));
    CHECK(why.find("does not match") != std::string::npos);
    CHECK(ensure_imported(assets.string(), why));
    CHECK(read_file(assets / "game_0400.bin") == g_image);
    // Another importer version: re-import too. Without sources: no valid import.
    std::string m;
    {
        std::ifstream f(assets / "manifest.txt");
        m.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::string old = m;
    old.replace(old.find("importer_version=1"), 18, "importer_version=0");
    write_file(assets / "manifest.txt", Bytes(old.begin(), old.end()));
    CHECK(!verify_assets(assets.string(), why) && why.find("version") != std::string::npos);
    CHECK(ensure_imported(assets.string(), why));
    fs::remove(src / "disc1.adz");
    write_file(assets / "manifest.txt", Bytes(old.begin(), old.end()));
    CHECK(!ensure_imported(assets.string(), why));
    CHECK(why.find("is gone") != std::string::npos);
    // CLI wrapper exit codes.
    write_file(src / "disc1.adf", g_disc1);
    CHECK(import_cli({(src / "disc1.adf").string(), (sub / "disc2.adf").string()}, assets.string()) == 0);
    CHECK(import_cli({(src / "disc1.adf").string()}, (work / "x").string()) == 2);
    write_file(src / "x.ipf", Bytes{'C', 'A', 'P', 'S', 0, 0, 0, 0}); // damaged IPF: bad input
    CHECK(import_cli({(src / "x.ipf").string()}, (work / "x").string()) == 2);
    fx::IpfBuild protected_disc;
    protected_disc.raw_track = 1;
    write_file(src / "p.ipf", fx::ipf_of_adf(g_disc1, protected_disc)); // protected disc: unsupported
    CHECK(import_cli({(src / "p.ipf").string(), (sub / "disc2.adf").string()}, (work / "x").string()) == 3);
    fs::remove(src / "p.ipf");
    write_file(src / "x.dms", Bytes{'D', 'M', 'S', '!', 0, 0, 0, 0});
    CHECK(import_cli({(src / "x.dms").string()}, (work / "x").string()) == 2);
    CHECK(!fs::exists(work / "x"));
    // A failed import never touches an existing good set.
    CHECK(verify_assets(assets.string(), why));
    CHECK(!write_assets(assets.string(), import_inputs({{"d1", g_disc1}}), err));
    CHECK(verify_assets(assets.string(), why));
}

// Truncated and corrupted inputs of every kind: any status is fine, crashing or hanging is not.
void test_robustness() {
    Bytes z = zip({{"d1.adf", g_disc1, 8}, {"d2.adf", g_disc2, 0}});
    Bytes gz = gzip(g_disc1);
    std::vector<Bytes> bases = {z, gz, g_disc1, g_disc2};
    Rng rng{2024};
    int runs = 0, ok = 0;
    for (const Bytes& base : bases) {
        for (size_t cut : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), size_t(10), size_t(21), size_t(22),
                           size_t(30), size_t(100), base.size() / 3, base.size() / 2, base.size() - 23, base.size() - 22,
                           base.size() - 8, base.size() - 1}) {
            if (cut > base.size()) continue;
            Bytes b(base.begin(), base.begin() + long(cut));
            ok += import_inputs({{"cut", b}}).status == Status::Ok;
            ++runs;
        }
        for (int i = 0; i < 40; ++i) {
            Bytes b = base;
            int flips = 1 + int(rng.next() % 16);
            for (int k = 0; k < flips; ++k) b[rng.next() % b.size()] = uint8_t(rng.next());
            // Damage the archive structures specifically (end of central directory, local headers).
            if (i % 4 == 0) b[b.size() - 1 - rng.next() % std::min<size_t>(b.size(), 100)] = uint8_t(rng.next());
            ok += import_inputs({{"flip", b}, {"d2", g_disc2}}).status == Status::Ok;
            ++runs;
        }
    }
    for (int i = 0; i < 50; ++i) {
        Bytes b(rng.next() % 4096);
        for (auto& v : b) v = uint8_t(rng.next());
        static const char* const magics[] = {"PK\x03\x04", "PK\x05\x06", "\x1F\x8B\x08", "DOS", "DMS!"};
        const char* m = magics[i % 5];
        for (size_t k = 0; k < std::strlen(m) && k < b.size(); ++k) b[k] = uint8_t(m[k]);
        import_inputs({{"garbage", b}});
        ++runs;
    }
    std::printf("[robustness] %d truncated/corrupted/garbage inputs, no crash (%d still imported)\n", runs, ok);
    CHECK(runs > 200);
}

} // namespace

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_import_tests";
    build_discs();
    test_sha1_crc();
    test_raw_pair();
    test_missing_and_wrong();
    test_truncated_and_corrupt();
    test_adz_zip();
    test_unsupported_formats();
    test_lha_and_whdload();
    test_dms_and_extended_adf();
    test_ipf();
    test_flux_images();
    test_files_and_assets(work);
    test_non_ascii_paths(work);
    test_robustness();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
