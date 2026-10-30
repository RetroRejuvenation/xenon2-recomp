// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Tests for the LHA reader (runtime/import/lha) with synthetic archives built here: no game data.
//
//   x2_lha_tests                    the synthetic tests (stored members of every header level, hand-built -lh5-/-lh6-/-lh7-
//                                   streams, an -lh1- round trip through a model-driven encoder, damaged archives)
//   x2_lha_tests --lha-dir DIR      also reads every .lha/.lzh/.lzs file under DIR (exit 77 if there are none): every
//                                   member of a supported method must decode and match its CRC-16. Local only.
//   x2_lha_tests --fuzz N [SEED]    N mutation-fuzz iterations (default run: 600); meant for the sanitizer build.
#include "import/lha.h"

#include "lha_fixtures.h"

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
using namespace lhafx;
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

void test_stored_levels() {
    Bytes a = text(300, 1), b = text(17, 2), c;
    std::vector<Member> ms;
    for (int level = 0; level <= 3; ++level) {
        ms.clear();
        ms.push_back(stored("ONE.TXT", a, level));
        ms.push_back(stored("TWO", b, level, "dir/sub/"));
        ms.push_back(stored("EMPTY", c, level));
        Bytes arc = archive(ms);
        LhaArchive out;
        std::string why;
        bool ok = read_lha(arc, out, why);
        CHECK(ok);
        if (!ok) { std::fprintf(stderr, "  level %d: %s\n", level, why.c_str()); continue; }
        CHECK(out.entries.size() == 3);
        if (out.entries.size() != 3) continue;
        CHECK(out.entries[0].name == "ONE.TXT");
        CHECK(out.entries[0].data == a);
        CHECK(out.entries[0].problem.empty());
        CHECK(out.entries[0].header_level == level);
        CHECK(out.entries[1].name == "dir/sub/TWO");
        CHECK(out.entries[1].data == b);
        CHECK(out.entries[2].name == "EMPTY");
        CHECK(out.entries[2].data.empty() && out.entries[2].problem.empty());
        CHECK(out.note.empty());
    }
}

void test_directory_member() {
    Member d;
    d.method = "-lhd-";
    d.name = "folder";
    d.level = 1;
    Bytes arc = archive({d, stored("F", text(5, 3), 1, "folder/")});
    LhaArchive out;
    std::string why;
    CHECK(read_lha(arc, out, why));
    CHECK(out.entries.size() == 2);
    if (out.entries.size() == 2) {
        CHECK(out.entries[0].is_dir);
        CHECK(!out.entries[1].is_dir && out.entries[1].name == "folder/F");
    }
}

void test_damage() {
    Bytes a = text(200, 5), b = text(100, 6);
    Bytes arc = archive({stored("A", a, 0), stored("B", b, 1)});
    LhaArchive out;
    std::string why;
    // bad header checksum in the first member: not an LHA archive
    Bytes bad = arc;
    bad[1] ^= 1;
    CHECK(!read_lha(bad, out, why));
    // damaged second header: the first member is kept, the rest ignored with a note
    bad = arc;
    size_t second = build_member(stored("A", a, 0)).size();
    bad[second + 1] ^= 1;
    CHECK(read_lha(bad, out, why));
    CHECK(out.entries.size() == 1 && out.entries[0].problem.empty());
    CHECK(!out.note.empty());
    // CRC mismatch: the member stays, marked, without data
    Member m = stored("C", a, 0);
    m.bad_crc = true;
    CHECK(read_lha(archive({m}), out, why));
    CHECK(out.entries.size() == 1 && !out.entries[0].problem.empty() && out.entries[0].data.empty());
    // truncated data
    Bytes tr = arc;
    tr.resize(build_member(stored("A", a, 0)).size() - 50);
    CHECK(read_lha(tr, out, why));
    CHECK(out.entries.size() == 1 && !out.entries[0].problem.empty());
    // unsupported method
    Member u = stored("U", a, 0);
    u.method = "-lh2-";
    CHECK(read_lha(archive({u}), out, why));
    CHECK(out.entries.size() == 1 && out.entries[0].unsupported && !out.entries[0].problem.empty());
    // too large for max_member, and for max_total
    CHECK(read_lha(archive({stored("A", a, 0)}), out, why, 100));
    CHECK(out.entries.size() == 1 && out.entries[0].problem.find("too large") != std::string::npos);
    CHECK(!read_lha(archive({stored("A", a, 0), stored("B", a, 0)}), out, why, 1000, 300));
    // garbage and empty
    CHECK(!read_lha(Bytes{}, out, why));
    CHECK(!read_lha(Bytes{0}, out, why));
    CHECK(!read_lha(text(100, 7), out, why));
    // stored member whose sizes differ
    Member s = stored("S", a, 0);
    s.packed.resize(150);
    CHECK(read_lha(archive({s}), out, why));
    CHECK(out.entries.size() == 1 && !out.entries[0].problem.empty());
    // a member claiming a huge original size with tiny data is refused before allocation
    Member h = stored("H", a, 0);
    Bytes arc2 = build_member(h);
    // patch original size to 0x7FFFFFFF and fix the header checksum
    arc2[11] = 0xFF; arc2[12] = 0xFF; arc2[13] = 0xFF; arc2[14] = 0x7F;
    unsigned sum = 0;
    for (size_t i = 2; i < size_t(arc2[0]) + 2; ++i) sum += arc2[i];
    arc2[1] = uint8_t(sum);
    CHECK(read_lha(arc2, out, why));
    CHECK(out.entries.size() == 1 && !out.entries[0].problem.empty());
}

void test_new_methods() {
    // "abcabcabcXabcabc" style data: literals and distance-3 matches of length 3
    struct Case { const char* method; int offset_bits; int obits; };
    for (Case c : {Case{"-lh5-", 4, 3}, Case{"-lh4-", 4, 3}, Case{"-lh6-", 5, 3}, Case{"-lh7-", 5, 4}, Case{"-lh5-", 4, 1}, Case{"-lh5-", 4, 0}}) {
        std::vector<std::pair<int, int>> tokens;
        Bytes expect;
        auto lit = [&](int v) { tokens.push_back({v, 0}); expect.push_back(uint8_t(v)); };
        auto match = [&](int dist) {
            tokens.push_back({-1, dist});
            for (int i = 0; i < 3; ++i) expect.push_back(expect[expect.size() - size_t(dist)]);
        };
        lit('a'); lit('b'); lit('c');
        // the single offset symbol fixes the distance: obits 0 -> 1, 1 -> 2, 3 -> 5..8 (we use 5), 4 -> 9..16 (we use 9)
        int dist = c.obits == 0 ? 1 : c.obits == 1 ? 2 : c.obits == 3 ? 5 : 9;
        for (int i = 0; i < 20; ++i) lit('a' + (i % 3) + 3 * (i % 2));
        if (c.obits == 0) { lit('z'); match(1); match(1); }
        else if (c.obits == 1) { match(2); match(2); }
        else { for (int i = 0; i < 10; ++i) lit('k' + i); match(dist); match(dist); }
        Bytes packed = lh_new_stream(tokens, c.offset_bits, c.obits);
        std::vector<uint8_t> out;
        std::string why;
        bool ok = lha_decode(c.method, packed.data(), packed.size(), expect.size(), out, why);
        CHECK(ok);
        if (!ok) std::fprintf(stderr, "  %s obits %d: %s\n", c.method, c.obits, why.c_str());
        CHECK(out == expect);
        // inside an archive, with the CRC
        Member m;
        m.method = c.method;
        m.name = "M";
        m.level = 1;
        m.original = expect;
        m.packed = packed;
        LhaArchive arc;
        CHECK(read_lha(archive({m}), arc, why));
        CHECK(arc.entries.size() == 1 && arc.entries[0].problem.empty() && arc.entries[0].data == expect);
        // truncation is an error, not a crash
        for (size_t cut = 0; cut < packed.size(); cut += 3) {
            std::vector<uint8_t> o2;
            lha_decode(c.method, packed.data(), cut, expect.size(), o2, why);
        }
    }
}


void test_lh1() {
    // Round trips through the adaptive model. The 40 KB case has more than 32768 coded symbols, so the tree is rebuilt.
    std::vector<Bytes> inputs;
    inputs.push_back(Bytes{});
    inputs.push_back(Bytes{'x'});
    inputs.push_back(text(5000, 21));
    inputs.push_back(Bytes(3000, 'z'));
    {
        Bytes mixed = text(2000, 22);
        Bytes rnd(40000);
        fx::Rng r{23};
        for (auto& v : rnd) v = uint8_t(r.next() >> 7);
        mixed.insert(mixed.end(), rnd.begin(), rnd.end());
        mixed.insert(mixed.end(), mixed.begin(), mixed.begin() + 3000);
        inputs.push_back(mixed);
    }
    for (const Bytes& in : inputs) {
        Bytes packed;
        CHECK(lha_lh1_encode(in.data(), in.size(), packed));
        std::vector<uint8_t> out;
        std::string why;
        bool ok = lha_decode("-lh1-", packed.data(), packed.size(), in.size(), out, why);
        CHECK(ok);
        if (!ok) std::fprintf(stderr, "  lh1 round trip of %zu bytes: %s\n", in.size(), why.c_str());
        CHECK(out == in);
        if (in.size() > 100) {
            for (size_t cut : {size_t(0), packed.size() / 2, packed.size() - 1}) {
                std::vector<uint8_t> o2;
                CHECK(!lha_decode("-lh1-", packed.data(), cut, in.size(), o2, why));
            }
        }
    }
    Member m;
    m.method = "-lh1-";
    m.name = "T";
    m.level = 1;
    m.original = text(3000, 24);
    lha_lh1_encode(m.original.data(), m.original.size(), m.packed);
    LhaArchive arc;
    std::string why;
    CHECK(read_lha(archive({m}), arc, why));
    CHECK(arc.entries.size() == 1 && arc.entries[0].problem.empty() && arc.entries[0].data == m.original);
}

void test_fuzz(int iterations, uint32_t seed) {
    std::vector<Bytes> seeds;
    for (int level = 0; level <= 3; ++level)
        seeds.push_back(archive({stored("A", text(120, 11), level), stored("B", text(40, 12), level, "d/")}));
    {
        std::vector<std::pair<int, int>> tokens;
        for (int i = 0; i < 40; ++i) tokens.push_back({'a' + i % 7, 0});
        tokens.push_back({-1, 5});
        Bytes expect;
        for (auto& t : tokens) {
            if (t.first >= 0) expect.push_back(uint8_t(t.first));
            else for (int i = 0; i < 3; ++i) expect.push_back(expect[expect.size() - size_t(t.second)]);
        }
        Member m;
        m.method = "-lh5-";
        m.name = "M";
        m.level = 2;
        m.original = expect;
        m.packed = lh_new_stream(tokens, 4, 3);
        seeds.push_back(archive({m}));
    }
    fx::Rng r{seed};
    for (int it = 0; it < iterations; ++it) {
        Bytes a = seeds[r.next() % seeds.size()];
        int edits = 1 + int(r.next() % 4);
        for (int e = 0; e < edits; ++e) {
            switch (r.next() % 4) {
            case 0: a[r.next() % a.size()] ^= uint8_t(1u << (r.next() % 8)); break;
            case 1: a[r.next() % a.size()] = uint8_t(r.next()); break;
            case 2: a.resize(r.next() % (a.size() + 1)); if (a.empty()) a.push_back(0); break;
            default: a.insert(a.begin() + long(r.next() % a.size()), uint8_t(r.next())); break;
            }
        }
        LhaArchive out;
        std::string why;
        read_lha(a, out, why, size_t(1) << 20, size_t(4) << 20);
        for (const LhaEntry& e : out.entries) CHECK(e.problem.empty() || e.data.empty());
    }
}

} // namespace

int main(int argc, char** argv) {
    int fuzz = 600;
    uint32_t seed = 1;
    bool dir_mode = false;
    std::string dir;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--lha-dir") && i + 1 < argc) { dir = argv[++i]; dir_mode = true; }
        else if (!std::strcmp(argv[i], "--fuzz") && i + 1 < argc) {
            fuzz = std::atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') seed = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        }
    }
    if (dir_mode && dir.empty()) {
        std::printf("lha_files: no folder given (set -DX2_LHA_TEST_DIR=...): skipped\n");
        return 77;
    }
    test_stored_levels();
    test_directory_member();
    test_damage();
    test_new_methods();
    test_lh1();
    test_fuzz(fuzz, seed);
    if (dir_mode) {
        std::error_code ec;
        size_t files = 0, members = 0, ok_members = 0, unsupported = 0, bad = 0, expected = 0;
        std::map<std::string, int> methods;
        std::vector<fs::path> paths;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (it->is_regular_file(ec) && (ext == ".lha" || ext == ".lzh" || ext == ".lzs")) paths.push_back(it->path());
        }
        std::sort(paths.begin(), paths.end());
        for (const fs::path& p : paths) {
            std::ifstream f(p, std::ios::binary);
            Bytes d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            LhaArchive arc;
            std::string why;
            ++files;
            if (!read_lha(d, arc, why)) {
                std::printf("%s: not read (%s)\n", p.filename().string().c_str(), why.c_str());
                continue;
            }
            for (const LhaEntry& e : arc.entries) {
                ++members;
                ++methods[e.method];
                if (e.problem.empty()) ++ok_members;
                else if (e.unsupported) ++unsupported;
                else if (e.problem.find("too large") != std::string::npos || e.problem.find("truncated") != std::string::npos) ++expected;
                else {
                    ++bad;
                    std::printf("%s: %s (%s): %s\n", p.filename().string().c_str(), e.name.c_str(), e.method.c_str(), e.problem.c_str());
                }
            }
        }
        std::printf("lha-dir: %zu files, %zu members, %zu decoded with a matching CRC-16, %zu of unsupported methods, %zu too large or truncated, %zu failed\n",
                    files, members, ok_members, unsupported, expected, bad);
        for (auto& kv : methods) std::printf("  %s: %d\n", kv.first.c_str(), kv.second);
        if (files == 0) {
            std::printf("no LHA files under %s: skipped\n", dir.c_str());
            return 77;
        }
        if (bad) ++g_failures;
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
