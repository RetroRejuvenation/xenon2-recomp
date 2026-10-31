// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Tests for the Rob Northen PDos release's pieces (runtime/import/rnc, pdos, fluxengine and the importer's PDos route), all on
// SYNTHETIC data: random "game files" packed by a test encoder, a disk laid out the way the release is, PDos tracks encoded
// as MFM bit cells, and flux made from them. No game data and no files.
//
//   x2_pdos_tests                 the synthetic tests
//   x2_pdos_tests --files DIR     also imports every .scp / .flux file found under DIR as one disk (a dump of the release):
//                                 exit 77 if DIR is empty or missing; local only, nothing from DIR is stored
#include "import/disk_image.h"
#include "import/flux.h"
#include "import/importer.h"
#include "import/pdos.h"
#include "import/rnc.h"

#include "flux_fixtures.h"

#include <algorithm>
#include <chrono>
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

bool has_message(const Result& r, const char* needle) {
    for (const Message& m : r.messages)
        if (m.text.find(needle) != std::string::npos) return true;
    return false;
}

void dump_messages(const Result& r) {
    for (const Message& m : r.messages) std::fprintf(stderr, "   msg: %s\n", m.text.c_str());
}

// ---- an RNC method 1 encoder (the inverse of rnc_unpack; all three Huffman tables are the flat 31-leaf, 5-bit code) ----

struct RncWriter {
    Bytes out;
    size_t wpos = 0;
    int wbits = 16; // 16 = the current word is full: the next bit starts a word after whatever was appended meanwhile
    void bit(uint32_t b) {
        if (wbits == 16) {
            wpos = out.size();
            out.push_back(0);
            out.push_back(0);
            wbits = 0;
        }
        if (b) out[wpos + size_t(wbits >> 3)] = uint8_t(out[wpos + size_t(wbits >> 3)] | (1u << (wbits & 7)));
        ++wbits;
    }
    void bits(uint32_t v, int n) { for (int i = 0; i < n; ++i) bit((v >> i) & 1); }
    void code(uint32_t sym) { for (int i = 4; i >= 0; --i) bit((sym >> i) & 1); } // canonical code of leaf `sym`, MSB first
    void value(uint32_t v) {
        uint32_t s = v;
        if (v >= 2) { s = 0; for (uint32_t t = v; t; t >>= 1) ++s; }
        code(s);
        if (s >= 2) bits(v & ((1u << (s - 1)) - 1), int(s) - 1);
    }
    void table() {
        bits(31, 5);
        for (int i = 0; i < 31; ++i) bits(5, 4);
    }
};

struct Token { size_t lit_at = 0, lit_n = 0, dist = 0, len = 0; }; // literals, then a match (len 0: none)

Bytes rnc_pack(const Bytes& in, int chunks = 1) {
    RncWriter w;
    w.bits(0, 2);
    const size_t per = (in.size() + size_t(chunks) - 1) / size_t(chunks);
    for (int c = 0; c < chunks; ++c) {
        const size_t a = size_t(c) * per, b = std::min(in.size(), a + per);
        if (a >= b) break;
        std::vector<Token> toks;
        size_t i = a, lit = a;
        while (i < b) {
            size_t best = 0, best_d = 0;
            for (size_t d = 1; d <= std::min<size_t>(i, 300); ++d) {
                size_t n = 0;
                while (i + n < b && n < 700 && in[i + n] == in[i + n - d]) ++n;
                if (n > best) { best = n; best_d = d; }
            }
            if (best >= 3) {
                toks.push_back({lit, i - lit, best_d, best});
                i += best;
                lit = i;
            } else {
                ++i;
            }
        }
        toks.push_back({lit, b - lit, 0, 0});
        w.table();
        w.table();
        w.table();
        w.bits(uint32_t(toks.size()), 16);
        for (const Token& t : toks) {
            w.value(uint32_t(t.lit_n));
            w.out.insert(w.out.end(), in.begin() + long(t.lit_at), in.begin() + long(t.lit_at + t.lit_n));
            if (t.len) {
                w.value(uint32_t(t.dist - 1));
                w.value(uint32_t(t.len - 2));
            }
        }
    }
    Bytes r = {'R', 'N', 'C', 1};
    fx::put32(r, uint32_t(in.size()));
    fx::put32(r, uint32_t(w.out.size()));
    fx::put16(r, rnc_crc16(in.data(), in.size()));
    fx::put16(r, rnc_crc16(w.out.data(), w.out.size()));
    r.push_back(0);
    r.push_back(uint8_t(chunks));
    r.insert(r.end(), w.out.begin(), w.out.end());
    return r;
}

void repack_crc(Bytes& r) { // fix the packed CRC16 of a stream whose body was edited, so that the decoder proper is reached
    if (r.size() < kRncHeaderBytes) return;
    const uint32_t packed = uint32_t(r[8]) << 24 | uint32_t(r[9]) << 16 | uint32_t(r[10]) << 8 | r[11];
    if (packed > r.size() - kRncHeaderBytes) return;
    const uint16_t c = rnc_crc16(r.data() + kRncHeaderBytes, packed);
    r[14] = uint8_t(c >> 8);
    r[15] = uint8_t(c);
}

Bytes random_bytes(size_t n, uint32_t seed, int run_percent = 0) {
    fx::Rng r{seed * 2654435761u + 99u};
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        if (run_percent && i > 8 && int(r.next() % 100) < run_percent) b[i] = b[i - 1 - r.next() % 8];
        else b[i] = uint8_t(r.next());
    }
    return b;
}

void test_rnc() {
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(rnc_crc16(digits, 9) == 0xBB3D);
    CHECK(rnc_crc16(digits, 0) == 0);

    struct Case { const char* name; Bytes data; int chunks; };
    std::vector<Case> cases;
    cases.push_back({"one byte", Bytes{0x42}, 1});
    cases.push_back({"random 1000", random_bytes(1000, 1), 1});
    cases.push_back({"random with runs", random_bytes(6000, 2, 55), 1});
    cases.push_back({"overlapping match", Bytes(900, 0xAB), 1});
    cases.push_back({"two chunks", random_bytes(5000, 3, 40), 2});
    cases.push_back({"five chunks", random_bytes(9000, 4, 60), 5});
    cases.push_back({"odd literal runs", random_bytes(777, 5, 5), 3});
    for (const Case& c : cases) {
        Bytes packed = rnc_pack(c.data, c.chunks);
        std::vector<uint8_t> out;
        RncHeader h;
        std::string why;
        const bool ok = rnc_unpack(packed.data(), packed.size(), 0, out, h, why);
        if (!ok) std::fprintf(stderr, "  case '%s': %s\n", c.name, why.c_str());
        CHECK(ok);
        CHECK(out == c.data);
        CHECK(h.unpacked == c.data.size());
        CHECK(h.chunks == c.chunks);
    }
    // At an offset inside a larger buffer, with other data on both sides, as on the disk.
    {
        Bytes data = random_bytes(3000, 8, 30), packed = rnc_pack(data);
        Bytes disk = random_bytes(777, 9);
        const size_t off = disk.size();
        disk.insert(disk.end(), packed.begin(), packed.end());
        Bytes tail = random_bytes(333, 10);
        disk.insert(disk.end(), tail.begin(), tail.end());
        std::vector<uint8_t> out;
        RncHeader h;
        std::string why;
        CHECK(rnc_unpack(disk.data(), disk.size(), off, out, h, why));
        CHECK(out == data);
        CHECK(!rnc_unpack(disk.data(), disk.size(), off - 1, out, h, why));
        CHECK(out.empty());
    }
    Bytes data = random_bytes(4000, 11, 45), good = rnc_pack(data, 2);
    std::vector<uint8_t> out;
    RncHeader h;
    std::string why;
    // Any single flipped byte of the packed data is caught by the packed CRC16 (a sector read wrongly is never accepted).
    for (size_t i = kRncHeaderBytes; i < good.size(); i += 37) {
        Bytes b = good;
        b[i] ^= 0x10;
        CHECK(!rnc_unpack(b.data(), b.size(), 0, out, h, why));
        CHECK(out.empty());
    }
    CHECK(why.find("CRC16 of the packed") != std::string::npos);
    // The unpacked CRC16 is checked as well.
    {
        Bytes b = good;
        b[12] ^= 1;
        CHECK(!rnc_unpack(b.data(), b.size(), 0, out, h, why));
        CHECK(why.find("unpacked") != std::string::npos);
    }
    // Truncation, a bad magic, a packed size past the end, and a huge unpacked size.
    CHECK(!rnc_unpack(good.data(), good.size() - 1, 0, out, h, why));
    CHECK(!rnc_unpack(good.data(), 10, 0, out, h, why));
    {
        Bytes b = good;
        b[3] = 2;
        CHECK(!rnc_unpack(b.data(), b.size(), 0, out, h, why));
        b = good;
        b[4] = 0x7F; // 2 GB unpacked
        CHECK(!rnc_unpack(b.data(), b.size(), 0, out, h, why));
        CHECK(why.find("unpacked bytes") != std::string::npos);
        b = good;
        b[8] = 0x7F;
        CHECK(!rnc_unpack(b.data(), b.size(), 0, out, h, why));
    }
    // Edited streams with a repaired packed CRC16 reach the decoder: any outcome but a crash or a wrong success is fine.
    fx::Rng rng{4242};
    int accepted_wrong = 0;
    for (int i = 0; i < 3000; ++i) {
        Bytes b = good;
        const int edits = 1 + int(rng.next() % 4);
        for (int e = 0; e < edits; ++e) b[kRncHeaderBytes + rng.next() % (b.size() - kRncHeaderBytes)] = uint8_t(rng.next());
        if (rng.next() % 7 == 0) b[16 + rng.next() % 2] = uint8_t(rng.next());
        repack_crc(b);
        if (rnc_unpack(b.data(), b.size(), 0, out, h, why) && out != data) ++accepted_wrong;
    }
    CHECK(accepted_wrong == 0);
}

// ---- PDos tracks: the inverse of pdos_decode_track on bit cells ----

constexpr uint32_t kM = 0x55555555u;
constexpr uint32_t kLead = 3000, kSectorCells = 8288;

void data_cells(std::vector<uint8_t>& cells, uint32_t value, uint32_t& prev) {
    for (int j = 0; j < 16; ++j) {
        const uint32_t d = (value >> (30 - 2 * j)) & 1;
        cells.push_back(uint8_t(!prev && !d));
        cells.push_back(uint8_t(d));
        prev = d;
    }
}

struct Cells {
    Bytes bytes;
    uint32_t bit_length = 0;
};

// `sectors`: 12 x 512 bytes. Cell index of sector s's marker: kLead + s * kSectorCells.
std::vector<uint8_t> encode_pdos_cells(const uint8_t* sectors, int track) {
    std::vector<uint8_t> cells;
    for (uint32_t i = 0; i < kLead; ++i) cells.push_back(uint8_t(i & 1));
    for (int s = 0; s < kPdosSectors; ++s) {
        uint32_t plain[128], key = 0;
        for (int k = 0; k < 128; ++k) {
            const uint8_t* p = sectors + size_t(s) * 512 + 4 * size_t(k);
            key ^= uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
            plain[k] = key;
        }
        uint32_t even[128], odd[128], x = 0;
        for (int k = 0; k < 128; ++k) {
            even[k] = (plain[k] >> 1) & kM;
            odd[k] = plain[k] & kM;
            x ^= even[k] ^ odd[k];
        }
        x &= kM;
        const uint32_t chk = (x & 0xFFFFu) | (((x >> 16) << 1) & 0xFFFFu);
        const uint32_t info = uint32_t(s) << 24 | uint32_t(track) << 16 | chk;
        for (int j = 0; j < 16; ++j) cells.push_back(uint8_t((0x4489 >> (15 - j)) & 1));
        uint32_t prev = 1;
        data_cells(cells, (info >> 1) & kM, prev);
        data_cells(cells, info & kM, prev);
        for (int k = 0; k < 128; ++k) data_cells(cells, even[k], prev);
        for (int k = 0; k < 128; ++k) data_cells(cells, odd[k], prev);
        for (uint32_t i = 0; i < (kSectorCells - 16 - 64 - 8192) / 2; ++i) { cells.push_back(0); cells.push_back(1); }
    }
    return cells;
}

Cells pack_cells(const std::vector<uint8_t>& cells, uint32_t rotate = 0) {
    Cells c;
    c.bit_length = uint32_t(cells.size());
    c.bytes.assign((cells.size() + 7) / 8, 0);
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[(i + rotate) % cells.size()]) c.bytes[i >> 3] = uint8_t(c.bytes[i >> 3] | (0x80 >> (i & 7)));
    return c;
}

Track raw_track(const Cells& c) {
    Track t;
    t.kind = Track::Kind::Raw;
    t.mfm = c.bytes;
    t.bit_length = c.bit_length;
    return t;
}

void flip_data_cell(std::vector<uint8_t>& cells, int sector) { // a data cell of the first even long of the sector: its checksum fails
    uint8_t& c = cells[kLead + size_t(sector) * kSectorCells + 16 + 64 + 3];
    c ^= 1;
}

void test_pdos_track() {
    Bytes secs = random_bytes(kPdosTrackBytes, 21);
    std::vector<uint8_t> cells = encode_pdos_cells(secs.data(), 17);
    CHECK(cells.size() == kLead + size_t(kPdosSectors) * kSectorCells);
    std::array<std::array<uint8_t, 512>, kPdosSectors> out{};
    std::array<bool, kPdosSectors> have{};
    // Every sector, for any starting point of the circular bit stream.
    for (uint32_t rot : {0u, 1u, 5u, 12345u, 80000u, 102455u}) {
        have = {};
        Track t = raw_track(pack_cells(cells, rot));
        CHECK(pdos_decode_track(t, 17, out, have) == kPdosSectors);
        bool same = true;
        for (int s = 0; s < kPdosSectors; ++s) same = same && have[size_t(s)] && std::memcmp(out[size_t(s)].data(), secs.data() + size_t(s) * 512, 512) == 0;
        CHECK(same);
    }
    // The track number is part of the sector header: another track's number reads nothing.
    {
        have = {};
        Track t = raw_track(pack_cells(cells));
        CHECK(pdos_decode_track(t, 18, out, have) == 0);
        CHECK(pdos_decode_track(t, 0, out, have) == 0);
    }
    // A standard (decoded) track, an empty track, or one too short reads nothing.
    {
        have = {};
        Track dec;
        dec.kind = Track::Kind::Decoded;
        CHECK(pdos_decode_track(dec, 17, out, have) == 0);
        Track e;
        CHECK(pdos_decode_track(e, 17, out, have) == 0);
        Track tiny = raw_track(pack_cells({1, 0, 1, 0, 1, 0, 0, 1}));
        CHECK(pdos_decode_track(tiny, 17, out, have) == 0);
    }
    // A data cell flipped in two sectors: those two are not read (a 16-bit checksum is never guessed around), the rest are.
    {
        std::vector<uint8_t> bad = cells;
        flip_data_cell(bad, 3);
        flip_data_cell(bad, 11);
        have = {};
        Track t = raw_track(pack_cells(bad));
        CHECK(pdos_decode_track(t, 17, out, have) == kPdosSectors - 2);
        CHECK(!have[3] && !have[11] && have[0] && have[4]);
        // An alternate stream with them intact supplies exactly those two, and never overwrites a sector already read.
        Track::Stream good;
        Cells g = pack_cells(cells, 777);
        good.bits = g.bytes;
        good.bit_length = g.bit_length;
        t.alt.push_back(good);
        const Bytes before(out[0].begin(), out[0].end());
        CHECK(pdos_decode_track(t, 17, out, have) == kPdosSectors); // the running total of sectors held
        bool all = true;
        for (bool h : have) all = all && h;
        CHECK(all);
        CHECK(std::memcmp(out[3].data(), secs.data() + 3 * 512, 512) == 0);
        CHECK(std::memcmp(out[0].data(), before.data(), 512) == 0);
    }
    // Noise never decodes (and never crashes).
    {
        fx::Rng r{77};
        Cells noise;
        noise.bit_length = 102456;
        noise.bytes.resize((noise.bit_length + 7) / 8);
        for (uint8_t& b : noise.bytes) b = uint8_t(r.next());
        have = {};
        CHECK(pdos_decode_track(raw_track(noise), 17, out, have) == 0);
    }
}

// ---- a synthetic PDos disk ----

constexpr size_t kTrack0Sectors = 11;

struct Synth {
    Bytes image;                        // kPdosDiskBytes
    Bytes game;                         // the unpacked game (what stage 2 holds)
    std::vector<size_t> file_at;        // image offset of each packed file's header
    std::vector<size_t> file_len;       // header + packed bytes
    size_t stage2_len = 0;
};

uint16_t crc_step(uint16_t crc, uint8_t b) {
    crc = uint16_t(crc ^ b);
    for (int k = 0; k < 8; ++k) crc = uint16_t((crc & 1) ? (crc >> 1) ^ 0xA001u : crc >> 1);
    return crc;
}

Synth make_synth(uint32_t seed) {
    Synth s;
    s.image = random_bytes(kPdosDiskBytes, seed);
    // The boot block: "DOS", with its last two bytes chosen so that the CRC16 is the one the release's boot block has.
    std::memcpy(s.image.data(), "DOS\0", 4);
    const uint16_t prefix = rnc_crc16(s.image.data(), 1022);
    bool found = false;
    for (uint32_t ab = 0; ab < 65536 && !found; ++ab) {
        if (crc_step(crc_step(prefix, uint8_t(ab >> 8)), uint8_t(ab)) == kPdosBootCrc) {
            s.image[1022] = uint8_t(ab >> 8);
            s.image[1023] = uint8_t(ab);
            found = true;
        }
    }
    CHECK(found);
    // Eleven packed files from sector 16 on, each starting a little way into its first sector; the file table lists them.
    size_t base = 0x2000;
    for (int i = 0; i < kPdosFileCount; ++i) {
        const size_t off = 90 + size_t(i) * 11;
        Bytes packed = rnc_pack(random_bytes(2500 + size_t(i) * 700, seed * 31u + uint32_t(i), 50), 1 + i % 3);
        const size_t at = base + off;
        std::memcpy(s.image.data() + at, packed.data(), packed.size());
        s.file_at.push_back(at);
        s.file_len.push_back(packed.size());
        const size_t sectors = (off + packed.size() + 511) / 512;
        uint8_t* e = s.image.data() + kPdosFileTableOffset + size_t(i) * 8;
        const uint32_t first = uint32_t(base / 512) + 1; // the game counts sectors from one above the image's
        e[0] = uint8_t(first >> 8); e[1] = uint8_t(first);
        e[2] = uint8_t(sectors >> 8); e[3] = uint8_t(sectors);
        e[4] = uint8_t(off >> 8); e[5] = uint8_t(off);
        e[6] = e[7] = 0;
        base += (sectors + 1) * 512;
    }
    // The game: it starts with a `bra.w`, and is longer than the loader code.
    s.game = random_bytes(0x9000, seed + 5, 60);
    s.game[0] = 0x60;
    s.game[1] = 0x00;
    Bytes packed = rnc_pack(s.game, 2);
    s.stage2_len = packed.size();
    std::memcpy(s.image.data() + kPdosStage2Offset, packed.data(), packed.size());
    CHECK(base < kPdosStage2Offset);
    return s;
}

// (track, sector) of the sector holding image offset `at`.
std::pair<int, int> sector_of(size_t at) {
    const size_t idx = at - kPdosTrack0Bytes;
    return {int(idx / kPdosTrackBytes) + 1, int((idx % kPdosTrackBytes) / 512)};
}

void damage(Bytes& image, std::pair<int, int> ts) {
    std::fill_n(image.begin() + long(kPdosTrack0Bytes + size_t(ts.first - 1) * kPdosTrackBytes + size_t(ts.second) * 512), 512, uint8_t(0));
}

void test_pdos_build() {
    const Synth s = make_synth(1);
    CHECK(pdos_is_v5_boot(s.image.data(), s.image.size()));
    {
        Bytes wrong = s.image;
        wrong[500] ^= 1;
        CHECK(!pdos_is_v5_boot(wrong.data(), wrong.size()));
        CHECK(!pdos_is_v5_boot(s.image.data(), 100));
    }
    PdosBuild pb;
    std::string why;
    CHECK(pdos_build_from_image(s.image, {}, pb, why));
    CHECK(pb.game_ok);
    CHECK(pb.files_ok == kPdosFileCount);
    CHECK(pb.game_image.size() == 0x3FC00);
    CHECK(std::equal(s.game.begin(), s.game.end(), pb.game_image.begin()));
    CHECK(pb.disc == s.image);
    for (const PdosFile& f : pb.files) CHECK(f.ok && f.damaged.empty());

    // A sector of one packed file unreadable: the build is refused, naming that file and sector, the other ten still verify.
    for (int k : {0, 4, 10}) {
        Bytes img = s.image;
        const auto ts = sector_of(s.file_at[size_t(k)] + 600);
        damage(img, ts);
        PdosBuild b;
        std::string w;
        CHECK(!pdos_build_from_image(img, {ts}, b, w));
        CHECK(b.game_ok);
        CHECK(b.files_ok == kPdosFileCount - 1);
        CHECK(!b.files[size_t(k)].ok);
        CHECK(b.files[size_t(k)].damaged.size() == 1 && b.files[size_t(k)].damaged[0] == ts);
        CHECK(w.find("1 of the 11 packed game files") != std::string::npos);
        CHECK(!b.files[size_t(k)].why.empty());
    }
    // A sector of the game itself.
    {
        Bytes img = s.image;
        const auto ts = sector_of(kPdosStage2Offset + 2000);
        damage(img, ts);
        PdosBuild b;
        std::string w;
        CHECK(!pdos_build_from_image(img, {ts}, b, w));
        CHECK(!b.game_ok);
        CHECK(w.find("game") != std::string::npos);
    }
    // A damaged sector the missing list does not know of (a CRC failure alone) is refused too.
    {
        Bytes img = s.image;
        img[s.file_at[2] + 100] ^= 0x40;
        PdosBuild b;
        std::string w;
        CHECK(!pdos_build_from_image(img, {}, b, w));
        CHECK(b.files_ok == kPdosFileCount - 1);
    }
    // Sizes, a boot block of another disk, a file table pointing nowhere.
    {
        PdosBuild b;
        std::string w;
        Bytes shorter(s.image.begin(), s.image.end() - 512);
        CHECK(!pdos_build_from_image(shorter, {}, b, w));
        Bytes other = s.image;
        other[100] ^= 0xFF;
        CHECK(!pdos_build_from_image(other, {}, b, w));
        Bytes table = s.image;
        for (size_t i = 0; i < 88; ++i) table[kPdosFileTableOffset + i] = 0xFF;
        CHECK(!pdos_build_from_image(table, {}, b, w));
        CHECK(b.game_ok && b.files_ok == 0);
    }
    // Random edits anywhere in the file area never crash the build, and a build that succeeds returned the same game.
    fx::Rng rng{9};
    for (int i = 0; i < 40; ++i) {
        Bytes img = s.image;
        for (int e = 0; e < 3; ++e) img[0x2000 + rng.next() % 0x20000] = uint8_t(rng.next());
        if (rng.next() % 4 == 0) img[kPdosFileTableOffset + rng.next() % 88] = uint8_t(rng.next());
        PdosBuild b;
        std::string w;
        if (pdos_build_from_image(img, {}, b, w)) CHECK(std::equal(s.game.begin(), s.game.end(), b.game_image.begin()));
    }
}

// ---- the importer: a disk as an extended ADF, a WHDLoad Disk.1, the damaged cases ----

std::vector<fx::ExtTrack> ext_tracks(const Synth& s, const std::vector<std::pair<int, int>>& bad_cells = {}) {
    std::vector<fx::ExtTrack> tracks;
    Bytes t0(s.image.begin(), s.image.begin() + long(kPdosTrack0Bytes));
    // Track 0 is a standard Amiga track.
    fx::ExtTrack n0;
    n0.type = 0;
    n0.data = t0;
    tracks.push_back(n0);
    for (int t = 1; t <= kPdosLastTrack; ++t) {
        std::vector<uint8_t> cells = encode_pdos_cells(s.image.data() + kPdosTrack0Bytes + size_t(t - 1) * kPdosTrackBytes, t);
        for (const auto& b : bad_cells)
            if (b.first == t) flip_data_cell(cells, b.second);
        Cells c = pack_cells(cells, uint32_t(t) * 131u);
        fx::ExtTrack et;
        et.type = 1;
        et.data = c.bytes;
        et.bits = c.bit_length;
        tracks.push_back(et);
    }
    return tracks;
}

// A standard track 0 needs to be an eleven-sector Amiga track. The extended ADF's normal tracks are ordinary sector data,
// which the reader checksums as good, so that is all a "decoded" track 0 needs.
void test_importer_adf() {
    const Synth s = make_synth(2);
    // The WHDLoad Disk.1 form: the assembled disk as it is.
    {
        Result r = import_inputs({{"Disk.1", s.image, false}});
        if (r.status != Status::Ok) dump_messages(r);
        CHECK(r.status == Status::Ok);
        CHECK(r.build_tag == "pdos-v5");
        CHECK(r.variant_id == "pdos-v5");
        CHECK(r.disc2 == s.image);
        CHECK(r.game_image.size() == 0x3FC00);
        CHECK(std::equal(s.game.begin(), s.game.end(), r.game_image.begin()));
        CHECK(has_message(r, "all 11 packed files verify"));
        // And the assets written from it verify again.
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / "x2_pdos_assets_test";
        fs::remove_all(dir, ec);
        std::string err;
        CHECK(write_assets(dir.string(), r, err));
        std::string reason;
        CHECK(verify_assets(dir.string(), reason));
        fs::remove_all(dir, ec);
    }
    // A damaged Disk.1 (a sector that reads as zeros) does not verify: refused, with the reason.
    {
        Bytes img = s.image;
        damage(img, sector_of(s.file_at[1] + 700));
        Result r = import_inputs({{"Disk.1", img, false}});
        CHECK(r.status != Status::Ok);
        CHECK(has_message(r, "does not verify"));
    }
    // Extended ADF with a standard track 0 and raw PDos tracks.
    {
        Result r = import_inputs({{"pdos.adf", fx::ext_adf(ext_tracks(s)), false}});
        if (r.status != Status::Ok) dump_messages(r);
        CHECK(r.status == Status::Ok);
        CHECK(r.build_tag == "pdos-v5");
        CHECK(r.disc2 == s.image);
        CHECK(std::equal(s.game.begin(), s.game.end(), r.game_image.begin()));
    }
    // A sector of a track unreadable (a flipped data cell): refused, naming the track and sector, never imported.
    {
        const auto ts = sector_of(s.file_at[3] + 800);
        Result r = import_inputs({{"pdos.adf", fx::ext_adf(ext_tracks(s, {ts})), false}});
        CHECK(r.status != Status::Ok);
        CHECK(r.game_image.empty());
        CHECK(has_message(r, "dump is incomplete"));
        const std::string needle = " " + std::to_string(ts.first) + ":" + std::to_string(ts.second);
        CHECK(has_message(r, needle.c_str()));
        CHECK(has_message(r, "4 (")); // file 4 of the table is the damaged one
    }
    // A sector that no file uses and the game does not load, unreadable: the import goes ahead with a note (the sector is stored as zeros).
    {
        const auto ts = sector_of(kPdosTrack0Bytes + 140 * kPdosTrackBytes + 6 * 512);
        Result r = import_inputs({{"pdos.adf", fx::ext_adf(ext_tracks(s, {ts})), false}});
        CHECK(r.status == Status::Ok);
        CHECK(has_message(r, "outside the game and its eleven packed files"));
        CHECK(r.disc2.size() == s.image.size());
        size_t differ = 0;
        for (size_t i = 0; i < s.image.size(); ++i) differ += r.disc2[i] != s.image[i];
        CHECK(differ > 0 && differ <= 512);
    }
}

// ---- FluxEngine .flux files ----

fx::MfmTrack standard_track(int t, uint32_t seed) {
    Bytes secs = random_bytes(kTrackBytes, seed + uint32_t(t));
    return fx::mfm_encode_track(secs.data(), t, uint32_t(t) * 997u);
}

void test_fluxengine() {
    fx::FluxModel m;
    m.speed_offset = 0.01;
    m.jitter = 0.03;
    std::vector<Bytes> blobs(4);
    std::vector<Bytes> sectors(4);
    for (int t = 0; t < 4; ++t) {
        sectors[size_t(t)] = random_bytes(kTrackBytes, 70u + uint32_t(t));
        fx::MfmTrack mt = fx::mfm_encode_track(sectors[size_t(t)].data(), t, uint32_t(t) * 997u);
        m.seed = 5u + uint32_t(t);
        blobs[size_t(t)] = fx::fluxengine_track_bytes(fx::make_timeline(mt, 4, 0, 0, m));
    }
    Bytes file = fx::fluxengine_file(blobs);
    CHECK(is_fluxengine(file.data(), file.size()));
    FluxDisk fd;
    std::string why;
    std::vector<std::string> warns;
    CHECK(read_fluxengine(file, fd, why, &warns));
    CHECK(warns.empty());
    CHECK(fd.source == "FluxEngine");
    for (int t = 0; t < 4; ++t) {
        CHECK(fd.tracks[size_t(t)].revs.size() == 3); // four index pulses: three whole revolutions
        Track out;
        FluxTrackReport rep;
        decode_flux_track(fd.tracks[size_t(t)], t, out, rep);
        CHECK(out.kind == Track::Kind::Decoded && out.all_sectors_ok() && out.sectors == sectors[size_t(t)]);
    }
    CHECK(fd.tracks[4].revs.empty());
    // The file detects as a FluxEngine container, and the importer reads it as a disk image of that kind.
    CHECK(detect_container(file.data(), file.size()) == Container::FluxEngine);
    CHECK(std::strcmp(container_name(Container::FluxEngine), "FluxEngine") == 0);
    {
        Result r = import_inputs({{"capture.flux", file, false}});
        CHECK(r.status != Status::Ok); // four random tracks are not a Xenon disk
        CHECK(has_message(r, "FluxEngine flux image"));
    }
    // Other versions are read with a warning; unknown fields and tracks beyond the drive's range are skipped.
    {
        std::vector<std::string> w;
        FluxDisk f2;
        CHECK(read_fluxengine(fx::fluxengine_file(blobs, 3), f2, why, &w));
        CHECK(w.size() == 1 && w[0].find("version 3") != std::string::npos);
        Bytes extra = file;
        extra.push_back(0x28); // field 5, varint
        extra.push_back(0x07);
        extra.push_back(0x32); // field 6, bytes
        extra.push_back(0x02);
        extra.push_back(0x01);
        extra.push_back(0x02);
        CHECK(read_fluxengine(extra, f2, why, &w));
        std::vector<Bytes> far(200);
        far[170] = blobs[0]; // cylinder 85
        far[0] = blobs[0];
        w.clear();
        CHECK(read_fluxengine(fx::fluxengine_file(far), f2, why, &w));
        CHECK(!w.empty());
    }
    // Rejections.
    {
        FluxDisk f2;
        Bytes bad = file;
        bad[1] ^= 1;
        CHECK(!is_fluxengine(bad.data(), bad.size()));
        CHECK(!read_fluxengine(bad, f2, why));
        CHECK(!read_fluxengine({}, f2, why));
        CHECK(!read_fluxengine(Bytes(file.begin(), file.begin() + 8), f2, why));
        // Flux with no index pulse has no whole revolution.
        Bytes blob = blobs[0];
        for (uint8_t& b : blob) b = uint8_t(b & ~0x40);
        CHECK(!read_fluxengine(fx::fluxengine_file({blob}), f2, why));
        CHECK(why.find("no complete revolution") != std::string::npos);
        // A varint that never ends.
        Bytes endless = {0x08, 0xF8, 0xEA, 0xB1, 0xB3, 0x04, 0x1A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(!read_fluxengine(endless, f2, why));
        // A track longer than the file.
        Bytes cut = file;
        cut.resize(cut.size() - 7);
        CHECK(!read_fluxengine(cut, f2, why));
    }
    // Random damage: any outcome but a crash or a hang.
    fx::Rng r{31};
    for (int i = 0; i < 400; ++i) {
        Bytes b = file;
        const int edits = 1 + int(r.next() % 5);
        for (int e = 0; e < edits; ++e) b[r.next() % std::min<size_t>(b.size(), i % 2 ? 80 : b.size())] = uint8_t(r.next());
        if (r.next() % 6 == 0) b.resize(r.next() % b.size());
        FluxDisk f2;
        std::string w;
        if (read_fluxengine(b, f2, w)) {
            DiskImage img;
            FluxReport rep;
            // Keep the decode small: only the first track of a damaged read.
            Track t;
            FluxTrackReport tr;
            decode_flux_track(f2.tracks[0], 0, t, tr);
        }
    }
    CHECK(true);
}

// ---- two captures of one disk ----

void test_merge() {
    // A track of the second capture rescues a sector the first one cannot read; a sector one reads is never given up.
    fx::FluxModel m;
    Bytes secs = random_bytes(kTrackBytes, 90);
    fx::MfmTrack good = fx::mfm_encode_track(secs.data(), 0, 0);
    fx::MfmTrack bad_a = fx::mfm_encode_track(secs.data(), 0, 0, 0, 4);
    fx::MfmTrack bad_b = fx::mfm_encode_track(secs.data(), 0, 0, 0, 9);
    m.seed = 1;
    FluxDisk a, b;
    a.tracks[0] = fx::scp_style_revs(fx::make_timeline(bad_a, 2, 0, 0, m));
    m.seed = 2;
    b.tracks[0] = fx::scp_style_revs(fx::make_timeline(bad_b, 2, 0, 0, m));
    a.source = "SCP";
    b.source = "FluxEngine";
    auto decode = [](const FluxDisk& d) {
        Track t;
        FluxTrackReport rep;
        decode_flux_track(d.tracks[0], 0, t, rep);
        return t;
    };
    Track ta = decode(a), tb = decode(b);
    CHECK(ta.status[4] != SectorStatus::Ok && ta.status[9] == SectorStatus::Ok);
    CHECK(tb.status[9] != SectorStatus::Ok && tb.status[4] == SectorStatus::Ok);
    merge_flux_disks(a, b);
    CHECK(a.source == "SCP + FluxEngine");
    CHECK(a.tracks[0].revs.size() == 2); // one revolution from each capture
    Track tm = decode(a);
    CHECK(tm.kind == Track::Kind::Decoded && tm.all_sectors_ok() && tm.sectors == secs);
    // A capture of a different disk merged in cannot make a sector read wrongly: wrong data never passes the checksum.
    (void)good;
}

// A whole disk's flux: one revolution per track. `bad`: (track, sector) of a sector to ruin (a flipped data cell).
struct FullFlux {
    std::vector<FluxTrack> scp;
    std::vector<Bytes> fluxengine;
};

FullFlux full_flux(const Synth& s, const std::vector<std::pair<int, int>>& bad, bool as_scp, uint32_t seed) {
    FullFlux f;
    f.scp.resize(160);
    f.fluxengine.resize(160);
    fx::FluxModel m;
    m.speed_offset = as_scp ? 0.004 : -0.006;
    m.jitter = 0.02;
    m.index_jitter_cells = 2;
    for (int t = 0; t < 160; ++t) {
        m.seed = seed + uint32_t(t);
        fx::Timeline tl;
        if (t == 0) {
            fx::MfmTrack mt = fx::mfm_encode_track(s.image.data(), 0, 0);
            tl = fx::make_timeline(mt, 3, 0, 0, m);
        } else {
            std::vector<uint8_t> cells = encode_pdos_cells(s.image.data() + kPdosTrack0Bytes + size_t(t - 1) * kPdosTrackBytes, t);
            for (const auto& b : bad)
                if (b.first == t) flip_data_cell(cells, b.second);
            Cells c = pack_cells(cells);
            fx::MfmTrack mt;
            mt.bytes = c.bytes;
            mt.bit_length = c.bit_length;
            tl = fx::make_timeline(mt, 2, 0, 0, m);
        }
        if (as_scp) f.scp[size_t(t)] = fx::scp_style_revs(tl);
        else f.fluxengine[size_t(t)] = fx::fluxengine_track_bytes(tl);
    }
    return f;
}

void test_two_captures_import() {
    const auto t0 = std::chrono::steady_clock::now();
    const Synth s = make_synth(3);
    const auto bad_a = sector_of(s.file_at[2] + 900), bad_b = sector_of(s.file_at[7] + 1200);
    FullFlux fa = full_flux(s, {bad_a}, true, 100), fb = full_flux(s, {bad_b}, false, 200);
    Bytes scp = fx::scp_file(fa.scp, 1);
    Bytes flux = fx::fluxengine_file(fb.fluxengine);
    // Each capture alone has a sector the other has: refused, naming the sector.
    {
        Result r = import_inputs({{"first.scp", scp, false}});
        CHECK(r.status != Status::Ok);
        const std::string needle = " " + std::to_string(bad_a.first) + ":" + std::to_string(bad_a.second);
        CHECK(has_message(r, "dump is incomplete"));
        CHECK(has_message(r, needle.c_str()));
        CHECK(r.game_image.empty());
    }
    // Together they cover the whole disk: the import succeeds, and what was imported is the original disk.
    {
        Result r = import_inputs({{"first.scp", scp, false}, {"second.flux", flux, false}});
        if (r.status != Status::Ok) dump_messages(r);
        CHECK(r.status == Status::Ok);
        CHECK(r.build_tag == "pdos-v5");
        CHECK(r.disc2 == s.image);
        CHECK(has_message(r, "its revolutions are merged with it"));
    }
    // A capture of a different disk is not merged (its track 0 differs): the complete one is imported on its own, and the
    // first one's unreadable sector is not "covered" by it.
    {
        const Synth other = make_synth(4);
        FullFlux fo = full_flux(other, {}, false, 300);
        Result r = import_inputs({{"first.scp", scp, false}, {"other.flux", fx::fluxengine_file(fo.fluxengine), false}});
        CHECK(r.status == Status::Ok);
        CHECK(r.disc2 == other.image);
        CHECK(!has_message(r, "its revolutions are merged with it"));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  two captures of a synthetic PDos disk: %.1f s\n", secs);
}

// Local only: import the SCP / FluxEngine files found under `dir` together as one disk.
int run_real_files(const char* dir) {
    std::error_code ec;
    if (!dir || !*dir || !fs::is_directory(dir, ec)) return 77;
    std::vector<Input> ins;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".scp" && ext != ".flux") continue;
        std::ifstream f(it->path(), std::ios::binary);
        Input in;
        in.name = it->path().filename().string();
        in.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        ins.push_back(std::move(in));
    }
    if (ins.empty()) return 77;
    std::vector<std::string> diag;
    ImportOptions opt;
    opt.diagnostics = &diag;
    Result r = import_inputs(ins, opt);
    for (const std::string& d : diag) std::printf("%s\n", d.c_str());
    for (const Message& m : r.messages) std::printf("  %s\n", m.text.c_str());
    // A dump of the release is either complete (imported as the PDos build) or refused with its damaged sectors listed.
    if (r.status == Status::Ok) {
        std::printf("pdos files: imported as %s\n", r.build_tag.c_str());
        return r.build_tag == "pdos-v5" ? 0 : 1;
    }
    bool listed = false;
    for (const Message& m : r.messages) listed = listed || m.text.find("dump is incomplete") != std::string::npos;
    std::printf("pdos files: not imported (%s)\n", listed ? "incomplete dump, refused" : "other reason");
    return listed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--files") == 0) return run_real_files(argv[2]);
    test_rnc();
    test_pdos_track();
    test_pdos_build();
    test_importer_adf();
    test_fluxengine();
    test_merge();
    test_two_captures_import();
    std::printf("pdos tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
