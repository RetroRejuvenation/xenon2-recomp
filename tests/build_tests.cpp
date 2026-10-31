// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Game-build table tests (no game data): recognising a build from the image, the manifest's build= tag, the
// build-specific start of the CPU, the restart counter and the disk loader address, on synthetic images that hold only the
// bytes the table looks at.
//
//   x2_build_tests WORKDIR
#include "boot.h"
#include "disk_shim.h"
#include "machine.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace x2;
namespace fs = std::filesystem;

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

void put(std::vector<uint8_t>& img, uint32_t addr, std::initializer_list<uint8_t> bytes) {
    size_t at = addr - kGameBase;
    for (uint8_t b : bytes) img[at++] = b;
}

std::vector<uint8_t> blank() { return std::vector<uint8_t>(kGameImageSize, 0); }

std::vector<uint8_t> canonical_image() {
    auto g = blank();
    put(g, 0x400, {0x60, 0x00, 0x6C, 0x30});
    put(g, 0x7032, {0x46, 0xFC, 0x27, 0x00, 0x60, 0x00, 0x00, 0xBE});
    return g;
}

// The Power Pack's start-up at $762E: move.l #key,d0 ; moveq #1,d1 ; move.l d0,$24.w ; add.w d0,d1 ; lea $435FC.l,a7.
// What follows is `moveq #9,d5 ; bra.s *`, so a test can see that the CPU entered there.
std::vector<uint8_t> power_pack_image(uint8_t key_byte = 0x6B) {
    auto g = canonical_image();
    put(g, 0x7032, {0, 0, 0, 0, 0, 0, 0, 0}); // no reset stub there
    put(g, 0x762E, {0x20, 0x3C, 0x76, 0x70, 0xCF, key_byte, 0x72, 0x01, 0x21, 0xC0, 0x00, 0x24, 0xD2, 0x40, 0x4F, 0xF9,
                    0x00, 0x04, 0x35, 0xFC, 0x7A, 0x09, 0x60, 0xFE});
    return g;
}

std::vector<uint8_t> compilation_1762_image() {
    auto g = blank();
    put(g, 0x400, {0x60, 0x00, 0x6C, 0x3A});
    put(g, 0x703C, {0x46, 0xFC, 0x27, 0x00, 0x4F, 0xF9, 0x00, 0x04, 0x35, 0xFC});
    return g;
}

void write_file(const fs::path& p, const std::vector<uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

void write_text(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

std::vector<uint8_t> disc2() {
    std::vector<uint8_t> d(kDisc2Size, 0);
    std::memcpy(d.data() + 2, "X2B ", 4);
    return d;
}

void table() {
    CHECK(find_build("canonical") == &kBuildCanonical);
    CHECK(find_build("power-pack-2234") == &kBuildPowerPack);
    CHECK(find_build("compilation-1762") == &kBuild1762);
    CHECK(find_build("") == nullptr);
    // Every known build runs in a valid chip RAM size (512 KB for all disk builds; a CDTV build will ask for 1 MB).
    for (const BuildInfo* b : {&kBuildCanonical, &kBuildPowerPack, &kBuild1762})
        CHECK(b->chip_kb == 512 && valid_chip_size(b->chip_kb * 1024u));
    CHECK(find_build("power-pack") == nullptr);
    CHECK(find_build("CANONICAL") == nullptr);
    // Every build names itself the way the importer will write it, with sane addresses.
    for (const BuildInfo* b : {&kBuildCanonical, &kBuildPowerPack, &kBuild1762}) {
        CHECK(find_build(b->id) == b);
        CHECK(b->restart_pc >= kGameBase && b->restart_pc < kGameBase + kGameImageSize);
        CHECK(b->load_sectors_pc >= kGameBase && b->load_sectors_pc < kGameBase + kGameImageSize);
        CHECK(b->death_pc && b->vbl_pc && b->main_loop_pc && b->hiscore_addr && b->hiscore_saved_pc);
    }
    // The canonical build's values are what the code used before there was a table.
    CHECK(kBuildCanonical.entry_pc == 0 && kBuildCanonical.restart_pc == 0x7032 && kBuildCanonical.load_sectors_pc == 0x921A);
    CHECK(kBuildCanonical.vbl_pc == 0x20C2 && kBuildCanonical.main_loop_pc == 0x78CE && kBuildCanonical.discid_cmp_pc == 0x8FDE);
    CHECK(kBuildCanonical.death_pc == 0x5D00 && kBuildCanonical.hiscore_addr == 0x891E && kBuildCanonical.hiscore_saved_pc == 0x8B1A);
    CHECK(kBuildCanonical.canonical_layout && kBuildPowerPack.canonical_layout && !kBuild1762.canonical_layout);
    CHECK(kBuildPowerPack.entry_pc == 0x762E && kBuildPowerPack.discid_cmp_pc == 0);
}

void detection() {
    auto c = canonical_image();
    CHECK(detect_build(c.data(), c.size()) == &kBuildCanonical);
    auto p = power_pack_image();
    CHECK(detect_build(p.data(), p.size()) == &kBuildPowerPack);
    // The key immediate is not compared: another pressing of that release may carry another value.
    auto p2 = power_pack_image(0x11);
    CHECK(detect_build(p2.data(), p2.size()) == &kBuildPowerPack);
    auto n = compilation_1762_image();
    CHECK(detect_build(n.data(), n.size()) == &kBuild1762);

    // Anything else is no build: wrong size, unknown entry word, entry word right but start-up code wrong.
    CHECK(detect_build(nullptr, 0) == nullptr);
    std::vector<uint8_t> small(kGameImageSize - 1, 0);
    CHECK(detect_build(small.data(), small.size()) == nullptr);
    auto z = blank();
    CHECK(detect_build(z.data(), z.size()) == nullptr);
    auto e = canonical_image();
    e[0x7032 - kGameBase] ^= 0x01;
    put(e, 0x762E, {0});
    CHECK(detect_build(e.data(), e.size()) == nullptr);
    auto q = power_pack_image();
    q[0x762E - kGameBase + 7] = 0x02; // moveq #2,d1 instead of #1
    CHECK(detect_build(q.data(), q.size()) == nullptr);
    auto m = compilation_1762_image();
    m[0x703C - kGameBase + 4] = 0x4E;
    CHECK(detect_build(m.data(), m.size()) == nullptr);
    auto w = compilation_1762_image();
    w[3] = 0x30; // the canonical entry word over 1762 start-up code
    CHECK(detect_build(w.data(), w.size()) == nullptr);
}

void assets_loading(const fs::path& work) {
    fs::remove_all(work);
    fs::create_directories(work);
    GameAssets a;
    std::string err;
    auto dir = [&](const char* name) {
        fs::path d = work / name;
        fs::create_directories(d);
        write_file(d / "disc2.bin", disc2());
        return d;
    };

    { // no manifest: the build comes from the image alone
        fs::path d = dir("canonical");
        write_file(d / "game_0400.bin", canonical_image());
        CHECK(load_game_assets(d.string(), a, err) && a.build == &kBuildCanonical);
    }
    { // a manifest that names the same build, with other lines around it
        fs::path d = dir("pp_tagged");
        write_file(d / "game_0400.bin", power_pack_image());
        write_text(d / "manifest.txt", "# comment\nimporter_version=2\r\nbuild=power-pack-2234\r\nsource=x\n");
        CHECK(load_game_assets(d.string(), a, err) && a.build == &kBuildPowerPack);
    }
    { // a manifest without a build line (an importer that does not write one): the image decides
        fs::path d = dir("pp_untagged");
        write_file(d / "game_0400.bin", power_pack_image());
        write_text(d / "manifest.txt", "importer_version=1\nsource=x\n");
        CHECK(load_game_assets(d.string(), a, err) && a.build == &kBuildPowerPack);
    }
    { // the manifest and the image disagree: refused, naming both
        fs::path d = dir("mismatch");
        write_file(d / "game_0400.bin", compilation_1762_image());
        write_text(d / "manifest.txt", "build=power-pack-2234\n");
        CHECK(!load_game_assets(d.string(), a, err));
        CHECK(err.find("power-pack-2234") != std::string::npos && err.find("compilation-1762") != std::string::npos);
    }
    { // the manifest names a build this runtime does not know
        fs::path d = dir("unknown_tag");
        write_file(d / "game_0400.bin", canonical_image());
        write_text(d / "manifest.txt", "build=from-the-future\n");
        CHECK(!load_game_assets(d.string(), a, err) && err.find("from-the-future") != std::string::npos);
    }
    { // the CDTV release's assets (build=cdtv, no disc2.bin, an image of another size): refused by name, not by what is missing
        fs::path d = dir("cdtv_assets");
        write_file(d / "game_0400.bin", std::vector<uint8_t>(809564, 0));
        write_text(d / "manifest.txt", "importer_version=3\nbuild=cdtv\nchip_kb=1024\n");
        CHECK(!load_game_assets(d.string(), a, err));
        CHECK(err.find("CDTV build: runtime support not available yet") != std::string::npos);
        CHECK(err.find("disc2.bin") == std::string::npos && err.find("expected 0x3FC00") == std::string::npos);
        // the same folder without a manifest is just a bad disk-build folder (nothing says CDTV)
        fs::path e = dir("cdtv_no_manifest");
        write_file(e / "game_0400.bin", std::vector<uint8_t>(809564, 0));
        CHECK(!load_game_assets(e.string(), a, err) && err.find("CDTV build") == std::string::npos);
    }
    { // an image that is no known build
        fs::path d = dir("junk");
        write_file(d / "game_0400.bin", blank());
        CHECK(!load_game_assets(d.string(), a, err) && err.find("unexpected entry code") != std::string::npos);
    }
    { // wrong sizes are still refused
        fs::path d = dir("short");
        write_file(d / "game_0400.bin", std::vector<uint8_t>(100, 0));
        CHECK(!load_game_assets(d.string(), a, err));
    }
}

void boot_and_counters() {
    { // canonical: starts at $400 (bra.w $7032), and that first visit is the one the counter discounts
        GameAssets a;
        a.game_image = canonical_image();
        put(a.game_image, 0x7032 + 6, {0x7A, 0x07, 0x60, 0xFE}); // moveq #7,d5 ; bra.s * (after the stub's bra.w skips ahead)
        put(a.game_image, 0x7032, {0x7A, 0x07, 0x60, 0xFE});
        Machine m;
        boot_game(m, a);
        m.run_cycles(2000);
        CHECK(m.dreg(5) == 7);
        CHECK(m.restart_pc == 0x7032);
        CHECK(m.restarts == 1); // the first entry from $400
    }
    { // power pack: starts at $762E, and the counter already counts that entry as the discounted first one
        GameAssets a;
        a.game_image = power_pack_image();
        a.build = &kBuildPowerPack;
        Machine m;
        boot_game(m, a);
        m.run_cycles(2000);
        CHECK(m.dreg(5) == 9);
        CHECK(m.restart_pc == 0x7032);
        CHECK(m.restarts == 1);
        CHECK(m.dreg(0) == 0x7670CF6B && (m.dreg(1) & 0xFFFF) == 0xCF6C); // the start-up instructions really ran
        CHECK(m.read32(0x24) == 0x7670CF6B);
    }
    { // 1762: starts at $400 like the canonical build, but its restart vector target is $703C
        GameAssets a;
        a.game_image = compilation_1762_image();
        a.build = &kBuild1762;
        Machine m;
        boot_game(m, a);
        CHECK(m.restart_pc == 0x703C);
    }
}

void loader_address() {
    // The disk shim replaces whichever loader address the build says: the canonical $921A or 1762's $9012.
    std::vector<uint8_t> disc(kDisc2Size);
    for (size_t i = 0; i < disc.size(); ++i) disc[i] = uint8_t(i * 7 + (i >> 9));
    for (uint32_t loader : {uint32_t(0x921A), uint32_t(0x9012)}) {
        Machine m;
        DiskShim shim(disc, loader);
        shim.install(m);
        // lea $5000,a0 ; moveq #12,d0 ; moveq #3,d1 ; jsr loader ; move.w d0,$2004 ; bra.s *
        std::vector<uint16_t> code = {0x41F9, 0x0000, 0x5000, 0x700C, 0x7203, 0x4EB9, 0x0000, uint16_t(loader), 0x33C0, 0x0000, 0x2004, 0x60FE};
        uint32_t at = 0x1000;
        for (uint16_t w : code) { m.write16(at, w); at += 2; }
        m.write16(loader, 0x4AFC); // illegal: would fault if the trap did not intercept
        m.write16(0x2004, 0xBEEF);
        m.start_cpu(0x1000, 0x8000);
        m.run_cycles(2000);
        CHECK(shim.calls == 1 && m.read16(0x2004) == 0);
        CHECK(m.read8(0x5000) == disc[12 * 512] && m.read8(0x5000 + 3 * 512 - 1) == disc[15 * 512 - 1]);
    }
}
} // namespace

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_build_tests";
    table();
    detection();
    assets_loading(work);
    boot_and_counters();
    loader_address();
    fs::remove_all(work);
    std::printf("build tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
