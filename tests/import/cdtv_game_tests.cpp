// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The CDTV game importer (runtime/import/cdtv_game.*, hunk.*, iso9660.*) on synthetic inputs only: hunk files and ISO 9660
// volumes built here, damaged in every way the parsers have to survive, then imported, written, verified and re-imported.
// With --disc PATH it also checks the user's own CDTV disc image (exit code 77 when PATH is empty or missing).
//
//   x2_cdtv_game_tests WORKDIR [--disc PATH]
#include "cdtv/cdtv_disc.h"
#include "import/cdtv_game.h"
#include "import/hunk.h"
#include "import/importer.h"
#include "import/iso9660.h"
#include "import/sha1.h"
#include "import/variants.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace x2::import;
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

using Bytes = std::vector<uint8_t>;

void be32(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
void put_be32(Bytes& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24);
    b[at + 1] = uint8_t(v >> 16);
    b[at + 2] = uint8_t(v >> 8);
    b[at + 3] = uint8_t(v);
}
uint32_t get_be32(const Bytes& b, size_t at) { return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3]; }

// ---------------------------------------------------------------------------------------------------------
// A hunk file: hunk 0 = CODE of `code_longs` longwords with fix-ups, hunk 1 = BSS of `bss_longs` longwords. Both chip.
struct HunkSpec {
    uint32_t code_longs = 64, bss_longs = 32;
    uint32_t flags0 = 1, flags1 = 1; // memory type in the table's top bits
    // (offset in hunk 0, value stored there before relocation, target hunk)
    struct Fix { uint32_t off, value, target; };
    std::vector<Fix> fixes;
};

Bytes hunk_file(const HunkSpec& s) {
    Bytes b;
    be32(b, 0x3F3);
    be32(b, 0);
    be32(b, 2);
    be32(b, 0);
    be32(b, 1);
    be32(b, (s.flags0 << 30) | s.code_longs);
    be32(b, (s.flags1 << 30) | s.bss_longs);
    be32(b, 0x3E9);
    be32(b, s.code_longs);
    const size_t data_at = b.size();
    for (uint32_t i = 0; i < s.code_longs * 4; ++i) b.push_back(uint8_t(0x40 + (i * 7)));
    for (const auto& f : s.fixes)
        if (f.off + 4 <= s.code_longs * 4) put_be32(b, data_at + f.off, f.value);
    if (!s.fixes.empty()) {
        be32(b, 0x3EC);
        for (uint32_t target = 0; target < 2; ++target) {
            uint32_t n = 0;
            for (const auto& f : s.fixes) n += f.target == target;
            if (!n) continue;
            be32(b, n);
            be32(b, target);
            for (const auto& f : s.fixes)
                if (f.target == target) be32(b, f.off);
        }
        be32(b, 0);
    }
    be32(b, 0x3F2);
    be32(b, 0x3EB);
    be32(b, s.bss_longs);
    be32(b, 0x3F2);
    return b;
}

HunkSpec sample_spec() {
    HunkSpec s;
    s.fixes = {{0, 0x10, 0}, {4, 0x00, 1}, {8, 0xFF000020, 0}, {13 * 4, 0x100, 0}, {14 * 4, 0x80, 1}};
    // offsets 13*4/14*4 are word aligned; one odd offset is added by a dedicated test
    return s;
}

// ---------------------------------------------------------------------------------------------------------
// ISO 9660: raw MODE1/2352 or cooked 2048-byte blocks. Root holds the files, one subdirectory DEVS holds one.
struct IsoSpec {
    std::string volume = "XENONII";
    std::vector<std::pair<std::string, Bytes>> files; // root
    std::pair<std::string, Bytes> sub_file{"SYSTEM-CONFIGURATION", Bytes(232, 7)};
    bool corrupt_loop = false;        // the DEVS record points at the root
    bool corrupt_extent = false;      // the first file points past the image
    bool corrupt_overlap = false;     // the first file points into the root directory
    bool oversize_volume = false;     // the descriptor claims more blocks than the image holds
    bool zero_block_size = false;
};

void le_both32(Bytes& r, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) r[at + i] = uint8_t(v >> (8 * i));
    for (int i = 0; i < 4; ++i) r[at + 4 + i] = uint8_t(v >> (24 - 8 * i));
}

Bytes dir_record(const std::string& name, uint32_t lba, uint32_t size, bool dir) {
    size_t len = 33 + name.size();
    if (len % 2) ++len;
    Bytes r(len, 0);
    r[0] = uint8_t(len);
    le_both32(r, 2, lba);
    le_both32(r, 10, size);
    r[25] = dir ? 2 : 0;
    r[28] = 1;
    r[31] = 1;
    r[32] = uint8_t(name.size());
    for (size_t i = 0; i < name.size(); ++i) r[33 + i] = uint8_t(name[i]);
    return r;
}

Bytes iso_blocks(const IsoSpec& s, uint32_t* total_out = nullptr) {
    std::vector<Bytes> blocks(16, Bytes(2048, 0)); // system area
    auto sectors_for = [](size_t n) { return uint32_t((n + 2047) / 2048); };
    // layout: 16 PVD, 17 terminator, 18 root dir, 19 DEVS dir, then file extents
    uint32_t next = 20;
    struct Placed { std::string name; uint32_t lba; uint32_t size; };
    std::vector<Placed> placed;
    for (const auto& f : s.files) {
        placed.push_back({f.first, next, uint32_t(f.second.size())});
        next += std::max<uint32_t>(1, sectors_for(f.second.size()));
    }
    const uint32_t sub_lba = next;
    next += 1;
    Bytes root;
    auto add = [&](const Bytes& rec) { root.insert(root.end(), rec.begin(), rec.end()); };
    add(dir_record(std::string(1, '\0'), 18, 2048, true));
    add(dir_record(std::string(1, '\1'), 18, 2048, true));
    for (const auto& p : placed) add(dir_record(p.name + ";1", p.lba, p.size, false));
    add(dir_record("DEVS", s.corrupt_loop ? 18 : 19, 2048, true));
    root.resize(2048, 0);
    Bytes devs;
    {
        Bytes rec;
        auto addd = [&](const Bytes& r) { devs.insert(devs.end(), r.begin(), r.end()); };
        addd(dir_record(std::string(1, '\0'), 19, 2048, true));
        addd(dir_record(std::string(1, '\1'), 18, 2048, true));
        addd(dir_record(s.sub_file.first + ";1", sub_lba, uint32_t(s.sub_file.second.size()), false));
    }
    devs.resize(2048, 0);
    if (s.corrupt_extent && !placed.empty()) {
        // the first file record: lba far past the end
        size_t off = dir_record(std::string(1, '\0'), 0, 0, true).size() * 2;
        le_both32(root, off + 2, 100000);
    }
    if (s.corrupt_overlap && !placed.empty()) {
        size_t off = dir_record(std::string(1, '\0'), 0, 0, true).size() * 2;
        le_both32(root, off + 2, 18);
    }
    Bytes pvd(2048, 0);
    pvd[0] = 1;
    std::memcpy(&pvd[1], "CD001", 5);
    pvd[6] = 1;
    std::memset(&pvd[8], ' ', 32);
    std::memset(&pvd[40], ' ', 32);
    std::memcpy(&pvd[40], s.volume.data(), std::min<size_t>(32, s.volume.size()));
    const uint32_t volume_blocks = s.oversize_volume ? next + 5000 : next;
    le_both32(pvd, 80, volume_blocks);
    pvd[128] = s.zero_block_size ? 0 : 0;
    pvd[129] = s.zero_block_size ? 0 : 8; // 2048 = 0x0800 little endian: 00 08
    pvd[130] = pvd[129];
    pvd[131] = pvd[128];
    Bytes rr = dir_record(std::string(1, '\0'), 18, 2048, true);
    std::memcpy(&pvd[156], rr.data(), rr.size());
    Bytes term(2048, 0);
    term[0] = 255;
    std::memcpy(&term[1], "CD001", 5);
    term[6] = 1;
    blocks.push_back(pvd);
    blocks.push_back(term);
    blocks.push_back(root);
    blocks.push_back(devs);
    for (const auto& f : s.files) {
        Bytes data = f.second;
        data.resize(std::max<uint32_t>(1, sectors_for(data.size())) * 2048, 0);
        for (size_t o = 0; o < data.size(); o += 2048) blocks.push_back(Bytes(data.begin() + long(o), data.begin() + long(o) + 2048));
    }
    {
        Bytes data = s.sub_file.second;
        data.resize(2048, 0);
        blocks.push_back(data);
    }
    Bytes out;
    for (const Bytes& b : blocks) out.insert(out.end(), b.begin(), b.end());
    if (total_out) *total_out = uint32_t(blocks.size());
    return out;
}

Bytes iso_raw(const Bytes& cooked) {
    Bytes out;
    for (size_t b = 0; b * 2048 < cooked.size(); ++b) {
        static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
        out.insert(out.end(), sync, sync + 12);
        uint32_t frame = uint32_t(b) + 150;
        out.push_back(uint8_t(frame / 4500));
        out.push_back(uint8_t(frame / 75 % 60));
        out.push_back(uint8_t(frame % 75));
        out.push_back(1);
        out.insert(out.end(), cooked.begin() + long(b * 2048), cooked.begin() + long(b * 2048 + 2048));
        out.insert(out.end(), 288, 0);
    }
    return out;
}

// The game's files for a synthetic disc, plus X2 and the CDTV's own software.
IsoSpec game_disc(const HunkSpec& hs = sample_spec()) {
    IsoSpec s;
    s.files.push_back({"CDTV.TM", Bytes(300, 1)});
    s.files.push_back({"CDTVPREFS", Bytes(200, 2)});
    s.files.push_back({"RMTM", Bytes(100, 3)});
    s.files.push_back({"X2", hunk_file(hs)});
    s.files.push_back({"INTRO", Bytes(5000, 4)});
    for (int i = 1; i <= 5; ++i) s.files.push_back({"LEVEL" + std::to_string(i), Bytes(3000 + size_t(i), uint8_t(10 + i))});
    s.files.push_back({"PICCODE", Bytes(700, 5)});
    s.files.push_back({"PICDATA1", Bytes(2100, 6)});
    s.files.push_back({"PICDATAB", Bytes(2049, 7)});
    s.files.push_back({"SOUNDS00", Bytes(4096, 8)});
    s.files.push_back({"SPEECH00", Bytes(1, 9)});
    s.files.push_back({"SPEECH66", Bytes(2048, 10)});
    return s;
}

void write_file(const fs::path& p, const Bytes& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
void write_text(const fs::path& p, const std::string& t) { write_file(p, Bytes(t.begin(), t.end())); }

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool has_message(const CdtvGame& g, const std::string& needle, Message::Level level) {
    for (const Message& m : g.messages)
        if (m.level == level && m.text.find(needle) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------
void test_hunk_link() {
    HunkFile f;
    std::string why;
    Bytes b = hunk_file(sample_spec());
    CHECK(parse_hunk_file(b.data(), b.size(), f, why));
    CHECK(f.hunks.size() == 2 && f.reloc_total == 5);
    CHECK(f.hunks[0].kind == Hunk::Kind::Code && f.hunks[1].kind == Hunk::Kind::Bss);
    CHECK(f.hunks[0].mem_bytes == 256 && f.hunks[1].mem_bytes == 128 && f.hunks[0].mem_flags == 1 && f.hunks[1].mem_flags == 1);
    CHECK(f.hunks[0].data.size() == 256 && f.hunks[1].data.empty());
    LinkedImage L;
    CHECK(link_hunk_file(f, 0x400, 0x100000, L, why));
    CHECK(L.base == 0x400 && L.hunk_base.size() == 2 && L.hunk_base[0] == 0x400 && L.hunk_base[1] == 0x400 + 256);
    CHECK(L.image.size() == 256 + 128 && L.reloc_total == 5 && L.reloc_count[0] == 5 && L.reloc_count[1] == 0);
    CHECK(L.target_count[0] == 3 && L.target_count[1] == 2);
    // each fix-up adds its target's base: value + 0x400 for hunk 0, + 0x500 for hunk 1
    CHECK(get_be32(L.image, 0) == 0x10 + 0x400);
    CHECK(get_be32(L.image, 4) == 0x00 + 0x500);
    CHECK(get_be32(L.image, 8) == 0xFF000020u + 0x400); // the upper byte is kept
    CHECK(L.high_byte_values == 1);
    CHECK(get_be32(L.image, 13 * 4) == 0x100 + 0x400 && get_be32(L.image, 14 * 4) == 0x80 + 0x500);
    CHECK(L.pointers_to_end == 2); // $100 = the size of hunk 0, $80 = the size of hunk 1
    // bytes that are not fix-up sites are untouched, and the BSS is zero
    for (size_t i = 0; i < 256; ++i) {
        bool site = (i >= 0 && i < 12) || (i >= 52 && i < 60);
        if (!site) CHECK(L.image[i] == f.hunks[0].data[i]);
    }
    for (size_t i = 256; i < L.image.size(); ++i) CHECK(L.image[i] == 0);
    // another base
    LinkedImage M;
    CHECK(link_hunk_file(f, 0x1000, 0x100000, M, why) && get_be32(M.image, 0) == 0x10 + 0x1000);
    // an odd fix-up offset is counted, not refused (data may hold unaligned pointers)
    HunkSpec odd = sample_spec();
    odd.fixes.push_back({21, 0x20, 0});
    Bytes ob = hunk_file(odd);
    HunkFile of;
    LinkedImage OL;
    CHECK(parse_hunk_file(ob.data(), ob.size(), of, why) && link_hunk_file(of, 0x400, 0x100000, OL, why) && OL.odd_offsets == 1);
}

void test_hunk_damage() {
    HunkFile f;
    std::string why;
    const Bytes good = hunk_file(sample_spec());
    // every proper prefix of a good file is refused, and nothing crashes
    for (size_t n = 0; n < good.size(); ++n) CHECK(!parse_hunk_file(good.data(), n, f, why));
    CHECK(!parse_hunk_file(nullptr, 0, f, why));
    { // wrong magic
        Bytes b = good;
        b[3] = 0xE9;
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why) && why.find("HUNK_HEADER") != std::string::npos);
    }
    { // resident libraries
        Bytes b = good;
        put_be32(b, 4, 1);
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why));
    }
    { // table size zero and too large
        Bytes b = good;
        put_be32(b, 8, 0);
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why));
        put_be32(b, 8, 1000000);
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why));
    }
    { // a partial table (overlays)
        Bytes b = good;
        put_be32(b, 12, 1);
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why) && why.find("overlays") != std::string::npos);
    }
    { // a hunk that claims 16 GB, and three hunks that together exceed the total limit
        Bytes b = good;
        put_be32(b, 20, 0x40000000u | 0x3FFFFFFFu);
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why));
        Bytes three;
        be32(three, 0x3F3); be32(three, 0); be32(three, 3); be32(three, 0); be32(three, 2);
        for (int i = 0; i < 3; ++i) be32(three, 0x40000000u | (kHunkMaxBytes / 4));
        CHECK(!parse_hunk_file(three.data(), three.size(), f, why) && why.find("together") != std::string::npos);
    }
    { // extended memory flags
        Bytes b = good;
        put_be32(b, 20, 0xC0000000u | 64);
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why) && why.find("extended") != std::string::npos);
    }
    { // a code block longer than its allocation
        Bytes b = good;
        put_be32(b, 32, 65);
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why));
    }
    { // an unsupported block type
        Bytes b = good;
        put_be32(b, 28, 0x3F5); // HUNK_OVERLAY
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why) && why.find("unsupported hunk block") != std::string::npos);
    }
    { // a relocation outside its hunk, to a missing hunk, and a count larger than the hunk
        HunkSpec s = sample_spec();
        s.fixes.push_back({252, 0, 0}); // offset+4 == 256: still inside
        Bytes ok = hunk_file(s);
        CHECK(parse_hunk_file(ok.data(), ok.size(), f, why));
        s.fixes.back().off = 253;
        Bytes bad = hunk_file(s);
        CHECK(!parse_hunk_file(bad.data(), bad.size(), f, why) && why.find("outside hunk") != std::string::npos);
        Bytes target = hunk_file(sample_spec());
        size_t at = 0;
        for (size_t i = 0; i + 8 < target.size(); i += 4)
            if (get_be32(target, i) == 0x3EC) { at = i; break; }
        // first block: count, target
        put_be32(target, at + 8, 7);
        CHECK(!parse_hunk_file(target.data(), target.size(), f, why) && why.find("does not exist") != std::string::npos);
        Bytes count = hunk_file(sample_spec());
        put_be32(count, at + 4, 100000);
        CHECK(!parse_hunk_file(count.data(), count.size(), f, why));
    }
    { // a relocated value beyond the end of its target hunk: refused at link time
        HunkSpec s = sample_spec();
        s.fixes = {{0, 0x101, 0}};
        Bytes b = hunk_file(s);
        LinkedImage L;
        CHECK(parse_hunk_file(b.data(), b.size(), f, why));
        CHECK(!link_hunk_file(f, 0x400, 0x100000, L, why) && why.find("lies beyond") != std::string::npos);
        // low 24 bits decide, not the upper byte
        s.fixes = {{0, 0xFF000101u, 0}};
        b = hunk_file(s);
        CHECK(parse_hunk_file(b.data(), b.size(), f, why) && !link_hunk_file(f, 0x400, 0x100000, L, why));
    }
    { // not fitting below the limit, and a misaligned base
        Bytes b = good;
        HunkFile g;
        LinkedImage L;
        CHECK(parse_hunk_file(b.data(), b.size(), g, why));
        CHECK(!link_hunk_file(g, 0x400, 0x400 + 383, L, why));
        CHECK(link_hunk_file(g, 0x400, 0x400 + 384, L, why));
        CHECK(!link_hunk_file(g, 0x402, 0x100000, L, why));
        CHECK(!link_hunk_file(g, 0xFFFFFFF0u, 0xFFFFFFFFu, L, why));
    }
    { // relocations inside BSS, END outside a hunk, a hunk without END
        Bytes b;
        be32(b, 0x3F3); be32(b, 0); be32(b, 1); be32(b, 0); be32(b, 0); be32(b, 0x40000000u | 4);
        be32(b, 0x3EB); be32(b, 4); be32(b, 0x3EC); be32(b, 1); be32(b, 0); be32(b, 0); be32(b, 0); be32(b, 0x3F2);
        CHECK(!parse_hunk_file(b.data(), b.size(), f, why) && why.find("BSS") != std::string::npos);
        Bytes e;
        be32(e, 0x3F3); be32(e, 0); be32(e, 1); be32(e, 0); be32(e, 0); be32(e, 4); be32(e, 0x3F2);
        CHECK(!parse_hunk_file(e.data(), e.size(), f, why));
        Bytes c;
        be32(c, 0x3F3); be32(c, 0); be32(c, 1); be32(c, 0); be32(c, 0); be32(c, 4);
        be32(c, 0x3E9); be32(c, 1); be32(c, 0); be32(c, 0x3E9); be32(c, 1); be32(c, 0);
        CHECK(!parse_hunk_file(c.data(), c.size(), f, why));
    }
    { // name, debug and symbol blocks are skipped
        Bytes b;
        be32(b, 0x3F3); be32(b, 0); be32(b, 1); be32(b, 0); be32(b, 0); be32(b, 0x40000000u | 2);
        be32(b, 0x3E8); be32(b, 1); be32(b, 0x41424344);
        be32(b, 0x3E9); be32(b, 2); be32(b, 0x11223344); be32(b, 0x55667788);
        be32(b, 0x3F0); be32(b, 1); be32(b, 0x61626364); be32(b, 0x10); be32(b, 0);
        be32(b, 0x3F1); be32(b, 1); be32(b, 0);
        be32(b, 0x3F2);
        CHECK(parse_hunk_file(b.data(), b.size(), f, why) && f.hunks.size() == 1 && f.hunks[0].data.size() == 8);
    }
}

void test_iso() {
    IsoSpec spec = game_disc();
    Bytes cooked = iso_blocks(spec);
    Bytes raw = iso_raw(cooked);
    std::string why;
    for (int layout = 0; layout < 2; ++layout) {
        const Bytes& img = layout ? raw : cooked;
        IsoVolume v;
        CHECK(v.open(img.data(), img.size(), why));
        CHECK(v.raw() == (layout == 1) && v.volume_id() == "XENONII");
        std::vector<IsoEntry> list;
        CHECK(v.list(list, why));
        CHECK(list.size() == spec.files.size() + 1);
        std::map<std::string, IsoEntry> by;
        for (const IsoEntry& e : list) by[e.path] = e;
        CHECK(by.count("X2") && by.count("DEVS/SYSTEM-CONFIGURATION") && by.count("LEVEL3") && by.count("SPEECH66"));
        Bytes data;
        CHECK(v.read(by["LEVEL3"], data, why) && data.size() == 3003 && data[0] == 13 && data.back() == 13);
        CHECK(v.read(by["PICDATAB"], data, why) && data.size() == 2049 && data.back() == 7); // spans a block boundary
        CHECK(v.read(by["DEVS/SYSTEM-CONFIGURATION"], data, why) && data.size() == 232);
        IsoEntry bad = by["X2"];
        bad.size = 0x7FFFFFFF;
        CHECK(!v.read(bad, data, why));
    }
    { // too short, no descriptor, a descriptor of another volume type
        IsoVolume v;
        Bytes tiny(1000, 0);
        CHECK(!v.open(tiny.data(), tiny.size(), why));
        CHECK(!v.open(nullptr, 0, why));
        Bytes noise(40 * 2048, 0x55);
        CHECK(!v.open(noise.data(), noise.size(), why));
        Bytes b = cooked;
        b[16 * 2048] = 2;
        CHECK(!v.open(b.data(), b.size(), why));
        b = cooked;
        b[16 * 2048 + 128] = 0; b[16 * 2048 + 129] = 4; // block size 1024
        CHECK(!v.open(b.data(), b.size(), why) && why.find("block size") != std::string::npos);
    }
    { // truncated images: the volume is larger than the image
        IsoVolume v;
        Bytes t(cooked.begin(), cooked.begin() + long(cooked.size() - 4096));
        CHECK(!v.open(t.data(), t.size(), why) && why.find("larger than the image") != std::string::npos);
    }
    { // oversize volume claim, directory loop, extent past the image, a file overlapping a directory
        IsoSpec s = game_disc();
        s.oversize_volume = true;
        Bytes c = iso_blocks(s);
        IsoVolume v;
        CHECK(!v.open(c.data(), c.size(), why));
        s = game_disc();
        s.corrupt_loop = true;
        c = iso_blocks(s);
        std::vector<IsoEntry> list;
        CHECK(v.open(c.data(), c.size(), why) && !v.list(list, why) && why.find("loop") != std::string::npos);
        s = game_disc();
        s.corrupt_extent = true;
        c = iso_blocks(s);
        CHECK(v.open(c.data(), c.size(), why) && !v.list(list, why) && why.find("outside the volume") != std::string::npos);
        s = game_disc();
        s.corrupt_overlap = true;
        c = iso_blocks(s);
        CHECK(v.open(c.data(), c.size(), why) && !v.list(list, why) && why.find("overlaps a directory") != std::string::npos);
    }
    { // a record shorter than a record, and records that run past the directory's own size
        Bytes c = cooked;
        const size_t root = 18 * 2048;
        IsoVolume v;
        std::vector<IsoEntry> list;
        c[root + 34 + 34] = 3; // the third record (the first file) claims 3 bytes
        CHECK(v.open(c.data(), c.size(), why) && !v.list(list, why));
        Bytes d = cooked;
        d[16 * 2048 + 156 + 10] = 100; // the root directory is declared 100 bytes long, but its records are longer
        d[16 * 2048 + 156 + 11] = 0;
        CHECK(v.open(d.data(), d.size(), why) && !v.list(list, why) && why.find("past the end") != std::string::npos);
    }
    { // a name with a path separator is refused
        IsoSpec s = game_disc();
        s.files[0].first = "A/B";
        Bytes c = iso_blocks(s);
        IsoVolume v;
        std::vector<IsoEntry> list;
        CHECK(v.open(c.data(), c.size(), why) && !v.list(list, why));
    }
}

void test_import(const fs::path& work) {
    const fs::path dir = work / "disc";
    std::error_code ec;
    fs::remove_all(dir, ec);
    IsoSpec spec = game_disc();
    Bytes cooked = iso_blocks(spec), raw = iso_raw(cooked);
    write_file(dir / "track.bin", raw);
    write_file(dir / "disc.iso", cooked);
    write_text(dir / "disc.cue", "FILE \"track.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    CHECK(looks_like_cdtv_game_image((dir / "track.bin").string()));
    CHECK(looks_like_cdtv_game_image((dir / "disc.iso").string()));
    CHECK(looks_like_cdtv_game_image((dir / "disc.cue").string()));
    CHECK(!looks_like_cdtv_game_image((dir / "missing.bin").string()));
    write_text(dir / "notes.txt", "hello");
    CHECK(!looks_like_cdtv_game_image((dir / "notes.txt").string()));
    for (const char* name : {"track.bin", "disc.iso", "disc.cue"}) {
        CdtvGame g = import_cdtv_game((dir / name).string());
        CHECK(g.status == Status::Ok);
        CHECK(g.volume_id == "XENONII" && g.iso_files == spec.files.size() + 1);
        CHECK(g.image.size() == 256 + 128 && g.link.hunk_base[1] == 0x500);
        CHECK(g.files.size() == 12); // INTRO, LEVEL1-5, PICCODE, PICDATA1, PICDATAB, SOUNDS00, SPEECH00, SPEECH66
        bool no_os_files = true;
        for (const CdtvFile& f : g.files)
            if (f.name == "CDTV.TM" || f.name == "CDTVPREFS" || f.name == "RMTM" || f.name == "X2") no_os_files = false;
        CHECK(no_os_files);
        CHECK(!g.recognised && has_message(g, "differs from the catalogued CDTV disc", Message::Warn));
        CHECK(g.variant_id == "cdtv-unrecognised" && g.variant_name.find("uncatalogued") != std::string::npos);
        CHECK(g.languages.empty());
        CHECK(g.has_cue == (std::string(name) == "disc.cue"));
        CHECK(has_message(g, "imported and verified (play it with", Message::Info));
        CHECK(g.container.find(std::string(name) == "disc.cue" ? "CDTV disc image" : "data track") != std::string::npos);
    }
    // write, verify, tamper, re-import from the recorded source
    CdtvGame g = import_cdtv_game((dir / "track.bin").string());
    const fs::path out = work / "assets-cdtv";
    fs::remove_all(out, ec);
    std::string err;
    CHECK(write_cdtv_assets(out.string(), g, err));
    CHECK(fs::exists(out / "game_0400.bin") && fs::exists(out / "cd0" / "LEVEL3") && fs::exists(out / "manifest.txt"));
    CHECK(!fs::exists(out / "disc2.bin") && !fs::exists(out / "cd0" / "X2") && !fs::exists(out / "cd0" / "CDTV.TM"));
    const std::string manifest = read_text(out / "manifest.txt");
    CHECK(manifest.find("build=cdtv\n") != std::string::npos && manifest.find("chip_kb=1024\n") != std::string::npos);
    CHECK(manifest.find("importer_version=" + std::to_string(kImporterVersion) + "\n") != std::string::npos);
    CHECK(manifest.find("file_count=12\n") != std::string::npos && manifest.find("file.0=INTRO,5000,") != std::string::npos);
    CHECK(manifest.find("image_base=$400\n") != std::string::npos && manifest.find("hunk.1=bss,128,chip,base=$500,") != std::string::npos);
    CHECK(manifest_is_cdtv(manifest) && !manifest_is_cdtv("build=canonical\n") && !manifest_is_cdtv(""));
    std::string reason;
    CHECK(verify_assets(out.string(), reason));
    {
        Bytes image;
        std::ifstream f(out / "game_0400.bin", std::ios::binary);
        image.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        CHECK(image == g.image);
    }
    // a changed byte in a stored file, a missing file, an extra manifest row naming a path
    {
        Bytes lv = g.files[1].data;
        lv[0] ^= 0xFF;
        write_file(out / "cd0" / g.files[1].name, lv);
        CHECK(!verify_assets(out.string(), reason) && reason.find(g.files[1].name) != std::string::npos);
        fs::remove(out / "cd0" / g.files[1].name, ec);
        CHECK(!verify_assets(out.string(), reason));
        std::string m = manifest;
        auto at = m.find("file.0=INTRO");
        m.replace(at, 12, "file.0=../INT");
        write_text(out / "manifest.txt", m);
        CHECK(!verify_assets(out.string(), reason) && reason.find("not one of the game's") != std::string::npos);
    }
    // ensure_imported re-imports from the recorded source (the .bin) and repairs the folder
    write_text(out / "manifest.txt", manifest);
    CHECK(!verify_assets(out.string(), reason));
    CHECK(ensure_imported(out.string(), reason));
    CHECK(verify_assets(out.string(), reason) && fs::exists(out / "cd0" / g.files[1].name));
    // an importer version from another build is not accepted and, with the source gone, not repaired
    {
        std::string old = manifest;
        auto at = old.find("importer_version=");
        old.replace(at, old.find('\n', at) - at, "importer_version=1");
        write_text(out / "manifest.txt", old);
        CHECK(!verify_assets(out.string(), reason) && reason.find("importer version") != std::string::npos);
        fs::rename(dir / "track.bin", dir / "track.moved", ec);
        CHECK(!ensure_imported(out.string(), reason) && reason.find("gone") != std::string::npos);
        fs::rename(dir / "track.moved", dir / "track.bin", ec);
    }
    // stale files of an earlier import (disc2.bin, copylock.txt, other cd0 files) are removed by a new write
    write_file(out / "disc2.bin", Bytes(10, 1));
    write_file(out / "copylock.txt", Bytes(10, 1));
    write_file(out / "cd0" / "STALE", Bytes(10, 1));
    CHECK(write_cdtv_assets(out.string(), g, err));
    CHECK(!fs::exists(out / "disc2.bin") && !fs::exists(out / "copylock.txt") && !fs::exists(out / "cd0" / "STALE"));
    CHECK(verify_assets(out.string(), reason));
    // the CLI: writes and returns 0; a bare track has no audio and says so; the cue has none either (no audio tracks listed)
    CHECK(import_cdtv_game_cli((dir / "disc.iso").string(), (work / "assets-cli").string()) == 0);
    CHECK(fs::exists(work / "assets-cli" / "manifest.txt"));
    CHECK(import_cdtv_cli((dir / "disc.iso").string(), (work / "assets-cli2").string(), (work / "audio-cli2").string()) == 0);
    CHECK(import_cdtv_cli((dir / "disc.cue").string(), (work / "assets-cli3").string(), (work / "audio-cli3").string()) == 0); // no audio tracks: not an error
    CHECK(!fs::exists(work / "audio-cli3" / "manifest.txt"));
    CHECK(import_cdtv_game_cli((dir / "missing.iso").string(), (work / "assets-cli4").string()) == 2);
    CHECK(!fs::exists(work / "assets-cli4"));
    // diagnose
    std::vector<std::string> lines;
    CHECK(diagnose_cdtv((dir / "disc.iso").string(), lines) && lines.size() > 4);
    CHECK(!diagnose_cdtv((dir / "notes.txt").string(), lines) && lines.empty());
}

void test_not_this_disc(const fs::path& work) {
    const fs::path dir = work / "other";
    std::error_code ec;
    fs::remove_all(dir, ec);
    auto run = [&](const IsoSpec& s, const char* name) {
        write_file(dir / name, iso_blocks(s));
        return import_cdtv_game((dir / name).string());
    };
    { // another volume
        IsoSpec s = game_disc();
        s.volume = "OTHERDISC";
        CdtvGame g = run(s, "a.iso");
        CHECK(g.status == Status::Unsupported && has_message(g, "not XENONII", Message::Error));
        CHECK(!looks_like_cdtv_game_image((dir / "a.iso").string())); // the quick check names the volume too
    }
    { // no X2
        IsoSpec s = game_disc();
        s.files.erase(s.files.begin() + 3);
        CdtvGame g = run(s, "b.iso");
        CHECK(g.status == Status::Unsupported && has_message(g, "no file X2", Message::Error));
    }
    { // a level overlay missing
        IsoSpec s = game_disc();
        for (size_t i = 0; i < s.files.size(); ++i)
            if (s.files[i].first == "LEVEL4") s.files.erase(s.files.begin() + long(i));
        CdtvGame g = run(s, "c.iso");
        CHECK(g.status == Status::Unsupported && has_message(g, "LEVEL4", Message::Error));
        CHECK(g.image.empty() || g.status != Status::Ok);
    }
    { // X2 that is not a hunk file
        IsoSpec s = game_disc();
        s.files[3].second = Bytes(5000, 0x41);
        CdtvGame g = run(s, "d.iso");
        CHECK(g.status == Status::BadInput && has_message(g, "X2:", Message::Error));
    }
    { // X2 wanting fast RAM
        HunkSpec hs = sample_spec();
        hs.flags1 = 2;
        CdtvGame g = run(game_disc(hs), "e.iso");
        CHECK(g.status == Status::Unsupported && has_message(g, "fast RAM", Message::Error));
    }
    { // X2 too large for the chip RAM below its base
        HunkSpec hs = sample_spec();
        hs.code_longs = 300000;
        hs.fixes.clear();
        CdtvGame g = run(game_disc(hs), "f.iso");
        CHECK(g.status == Status::BadInput && has_message(g, "do not fit", Message::Error));
    }
    { // X2 with its first hunk not code
        HunkSpec hs = sample_spec();
        Bytes x = hunk_file(hs);
        put_be32(x, 28, 0x3EA); // HUNK_DATA
        IsoSpec s = game_disc();
        s.files[3].second = x;
        CdtvGame g = run(s, "g.iso");
        CHECK(g.status == Status::BadInput && has_message(g, "not code", Message::Error));
        Bytes y = x;
        put_be32(y, 28, 0x3EB);
        s.files[3].second = y;
        g = run(s, "h.iso");
        CHECK(g.status == Status::BadInput);
    }
    { // damaged volumes
        IsoSpec s = game_disc();
        s.corrupt_loop = true;
        CdtvGame g = run(s, "i.iso");
        CHECK(g.status == Status::BadInput && has_message(g, "damaged ISO 9660 volume", Message::Error));
    }
    { // not a disc at all
        write_file(dir / "j.bin", Bytes(100000, 0x77));
        CdtvGame g = import_cdtv_game((dir / "j.bin").string());
        CHECK(g.status == Status::Unsupported);
        g = import_cdtv_game((dir / "nothing").string());
        CHECK(g.status == Status::BadInput);
    }
    { // the game's file names: lower case and wrong digit counts are not game files; PICDATA<letter> and SPEECH99 are
        IsoSpec s = game_disc();
        s.files.push_back({"SPEECH99", Bytes(10, 1)});
        s.files.push_back({"speech01", Bytes(10, 1)});
        s.files.push_back({"SPEECH1", Bytes(10, 1)});
        s.files.push_back({"LEVEL6", Bytes(10, 1)});
        s.files.push_back({"PICDATAb", Bytes(10, 1)});
        s.files.push_back({"README", Bytes(10, 1)});
        CdtvGame g = run(s, "k.iso");
        CHECK(g.status == Status::Ok && g.files.size() == 13);
        bool has99 = false, odd = false;
        for (const CdtvFile& f : g.files) {
            if (f.name == "SPEECH99") has99 = true;
            if (f.name == "speech01" || f.name == "SPEECH1" || f.name == "LEVEL6" || f.name == "PICDATAb" || f.name == "README") odd = true;
        }
        CHECK(has99 && !odd);
    }
}

// Random damage to a good hunk file and a good ISO image: whatever the result, no crash, no out-of-bounds access (the sanitizer
// builds are the real judge), and an accepted hunk file always links within its limits.
void test_fuzz() {
    uint32_t state = 0x12345678u;
    auto rnd = [&]() { state = state * 1664525u + 1013904223u; return state >> 8; };
    const Bytes good = hunk_file(sample_spec());
    for (int iter = 0; iter < 6000; ++iter) {
        Bytes b = good;
        const int edits = 1 + int(rnd() % 4);
        for (int e = 0; e < edits; ++e) {
            const size_t at = rnd() % b.size();
            switch (rnd() % 3) {
            case 0: b[at] = uint8_t(rnd()); break;
            case 1: b[at] ^= uint8_t(1u << (rnd() % 8)); break;
            default: if (at + 4 <= b.size()) put_be32(b, at, rnd() % 7 == 0 ? 0xFFFFFFFFu : rnd() % 600); break;
            }
        }
        if (rnd() % 5 == 0) b.resize(rnd() % (b.size() + 1));
        HunkFile f;
        std::string why;
        if (parse_hunk_file(b.data(), b.size(), f, why)) {
            LinkedImage L;
            if (link_hunk_file(f, 0x400, 0x100000, L, why)) CHECK(L.image.size() <= 0x100000 - 0x400);
        }
    }
    const IsoSpec spec = game_disc();
    const Bytes cooked = iso_blocks(spec), raw = iso_raw(cooked);
    for (int iter = 0; iter < 3000; ++iter) {
        Bytes b = (iter % 2) ? raw : cooked;
        const int edits = 1 + int(rnd() % 6);
        for (int e = 0; e < edits; ++e) {
            // aim at the descriptor and the directories as often as anywhere else
            size_t at = (rnd() % 2) ? size_t(16 + rnd() % 4) * ((iter % 2) ? 2352 : 2048) + rnd() % 200 : rnd() % b.size();
            if (at >= b.size()) at = rnd() % b.size();
            b[at] = uint8_t(rnd());
        }
        if (rnd() % 7 == 0) b.resize(rnd() % (b.size() + 1));
        IsoVolume v;
        std::string why;
        std::vector<IsoEntry> list;
        if (v.open(b.data(), b.size(), why) && v.list(list, why))
            for (const IsoEntry& e : list) {
                Bytes d;
                if (v.read(e, d, why)) CHECK(d.size() == e.size);
            }
    }
}

void test_registry() {
    bool found = false;
    for (const VariantInfo& v : all_variants())
        if (v.id == "cdtv-xenon2-1991") {
            found = true;
            CHECK(v.recipe == Recipe::CdtvDisc && v.build == "cdtv" && v.supported && !v.canonical);
            CHECK(v.game_sha1 == builtin_cdtv_disc().game_sha1 && v.game_sha1.size() == 40);
        }
    CHECK(found);
    CHECK(builtin_cdtv_disc().game_files == 95 && builtin_cdtv_disc().languages == "EN,FR,DE,IT");
}

// ---------------------------------------------------------------------------------------------------------
// The user's own disc (--disc PATH): the evidence for the relocation.
int test_real_disc(const std::string& path, const fs::path& work) {
    if (path.empty() || !looks_like_cdtv_game_image(path)) {
        std::printf("SKIPPED: no CDTV disc image given (--disc PATH)\n");
        return 77;
    }
    const CdtvGame g = import_cdtv_game(path);
    CHECK(g.status == Status::Ok);
    if (g.status != Status::Ok) {
        for (const Message& m : g.messages) std::fprintf(stderr, "  %s\n", format_message(m).c_str());
        return 1;
    }
    const CdtvDisc& known = builtin_cdtv_disc();
    CHECK(g.recognised && g.x2_sha1 == known.x2_sha1 && g.image_sha1 == known.game_sha1);
    CHECK(g.volume_id == "XENONII" && g.iso_files == 101 && g.files.size() == 95 && g.languages == "EN,FR,DE,IT");
    // the load layout of the WHDLoad CDTV slave (docs/re-notes/cdtv-slave.md): hunk 0 at $400, hunk 1 at $6B09C, the end at $C5E5C
    CHECK(g.link.hunk_base.size() == 2 && g.link.hunk_base[0] == 0x400 && g.link.hunk_base[1] == 0x6B09C);
    CHECK(g.link.hunk_bytes[0] == 437404 && g.link.hunk_bytes[1] == 372160);
    CHECK(g.link.hunk_base[1] + g.link.hunk_bytes[1] == 0xC5E5C);
    CHECK(g.image.size() == 809564 && g.link.image.empty());
    CHECK(g.hunk_flags[0] == 1 && g.hunk_flags[1] == 1); // both chip
    CHECK(g.link.reloc_total == 4452 && g.link.reloc_count[0] == 4452 && g.link.reloc_count[1] == 0);
    CHECK(g.link.target_count[0] == 4367 && g.link.target_count[1] == 85);
    CHECK(g.link.high_byte_values == 10 && g.link.odd_offsets == 0);
    // independent check of the relocation: re-read X2, compare the unrelocated hunk bytes with the linked image
    Bytes raw;
    std::string where, why;
    x2::cdtv::Status cs;
    if (x2::cdtv::looks_like_disc_image(path)) {
        CHECK(x2::cdtv::read_data_track(path, raw, where, why, cs));
    } else {
        std::ifstream f(path, std::ios::binary);
        raw.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    IsoVolume vol;
    CHECK(vol.open(raw.data(), raw.size(), why));
    std::vector<IsoEntry> list;
    CHECK(vol.list(list, why));
    Bytes x2;
    for (const IsoEntry& e : list)
        if (e.path == "X2") CHECK(vol.read(e, x2, why));
    HunkFile hf;
    CHECK(parse_hunk_file(x2.data(), x2.size(), hf, why) && hf.hunks.size() == 2 && hf.reloc_total == 4452);
    std::set<uint32_t> sites;
    std::map<uint32_t, uint32_t> target;
    for (const HunkReloc& r : hf.hunks[0].relocs)
        for (uint32_t off : r.offsets) {
            CHECK(sites.insert(off).second);               // no site listed twice
            CHECK(off % 2 == 0 && off + 4 <= 437404);       // word aligned, inside the hunk
            target[off] = r.target;
        }
    CHECK(sites.size() == 4452);
    // The same arithmetic redone here: the unrelocated hunk bytes, each fix-up's long plus its target's base, the BSS zero. The linked
    // image must equal it byte for byte, which also shows that nothing else was touched.
    Bytes expected = hf.hunks[0].data;
    expected.resize(437404 + 372160, 0);
    for (const HunkReloc& r : hf.hunks[0].relocs)
        for (uint32_t off : r.offsets) {
            const uint32_t before = get_be32(expected, off);
            CHECK((before & 0xFFFFFF) <= g.link.hunk_bytes[r.target]);       // inside the target hunk
            const uint32_t after = before + g.link.hunk_base[r.target];
            CHECK((after & 0xFFFFFF) >= 0x400 && (after & 0xFFFFFF) <= 0xC5E5C); // relocated longs fall in [$400, end]
            put_be32(expected, off, after);
        }
    CHECK(expected == g.image);
    size_t differing_bytes = 0;
    for (size_t i = 0; i < 437404; ++i)
        if (g.image[i] != hf.hunks[0].data[i]) ++differing_bytes;
    CHECK(differing_bytes > 0 && differing_bytes <= 4452 * 4); // changes only at the fix-up sites
    std::printf("  real disc: %zu fix-up sites, %zu bytes differ from the unrelocated hunk\n", sites.size(), differing_bytes);
    bool bss_zero = true;
    for (size_t i = 437404; i < g.image.size(); ++i)
        if (g.image[i]) bss_zero = false;
    CHECK(bss_zero);
    // write, verify, tamper, repair
    const fs::path out = work / "real-assets-cdtv";
    std::error_code ec;
    fs::remove_all(out, ec);
    std::string err, reason;
    CHECK(write_cdtv_assets(out.string(), g, err));
    CHECK(verify_assets(out.string(), reason));
    CHECK(fs::exists(out / "cd0" / "LEVEL1") && fs::exists(out / "cd0" / "SPEECH66") && !fs::exists(out / "disc2.bin"));
    // cd0 files equal the ISO files
    for (const CdtvFile& f : g.files)
        for (const IsoEntry& e : list)
            if (e.path == f.name) {
                Bytes d;
                CHECK(vol.read(e, d, why) && d == f.data);
            }
    fs::remove(out / "cd0" / "LEVEL2", ec);
    CHECK(!verify_assets(out.string(), reason));
    CHECK(ensure_imported(out.string(), reason) && verify_assets(out.string(), reason));
    std::vector<std::string> lines;
    CHECK(diagnose_cdtv(path, lines) && lines.size() > 8);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_cdtv_game_tests";
    std::string disc;
    bool real = false;
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--disc") { real = true; if (i + 1 < argc) disc = argv[++i]; }
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    int rc = 0;
    if (real) {
        rc = test_real_disc(disc, work);
    } else {
        test_hunk_link();
        test_hunk_damage();
        test_iso();
        test_import(work);
        test_not_this_disc(work);
        test_fuzz();
        test_registry();
    }
    fs::remove_all(work, ec);
    std::printf("cdtv game tests: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) return 1;
    return rc;
}
