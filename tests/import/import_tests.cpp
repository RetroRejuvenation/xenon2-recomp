// Importer unit tests with synthetic inputs built here (no game data): an OFS disc 1 holding a fake XENON-II
// packed with the game's cruncher format, a fake "X2B " disc 2, wrong/truncated/oversized images, ADZ, ZIP
// (stored, deflate, nested, corrupted), unsupported formats, folders, the asset writer and manifest checks,
// and truncated/garbage inputs that must never crash.
//
//   x2_import_tests WORKDIR
#include "import/importer.h"
#include "import/sha1.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
    Result r = run("DMS", {{"x.dms", with_magic("DMS!", 300000)}, {"d2.adf", g_disc2}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "DMS"));
    r = run("IPF", {{"x.ipf", with_magic("CAPS", 400000)}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "IPF"));
    r = run("extended ADF", {{"x.adf", with_magic("UAE-1ADF", 1000000)}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "extended ADF"));
    r = run("HD ADF", {{"x.adf", Bytes(2 * kAdf, 0)}});
    CHECK(r.status == Status::Unsupported && has_message(r, Message::Error, "HD disk image"));
    r = run("text file", {{"notes.txt", Bytes(100, 'a')}});
    CHECK(r.status == Status::BadInput && has_message(r, Message::Error, "not a disk image or archive"));
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
    write_file(src / "x.dms", Bytes{'D', 'M', 'S', '!', 0, 0, 0, 0});
    CHECK(import_cli({(src / "x.dms").string()}, (work / "x").string()) == 3);
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
    test_files_and_assets(work);
    test_robustness();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
