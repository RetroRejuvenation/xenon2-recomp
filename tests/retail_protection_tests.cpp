// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Retail-protection unit tests (no game data): the key-track receipt (copylock.txt), recognising a retail image,
// and the PC trap, run on a synthetic image that holds only the entry stub's opcodes and a few instructions.
//
//   x2_retail_protection_tests WORKDIR
#include "boot.h"
#include "machine.h"
#include "retail_protection.h"

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

const std::string kGood = "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0x36FA43\n";

void put(std::vector<uint8_t>& img, uint32_t addr, std::initializer_list<uint8_t> bytes) {
    size_t at = addr - kGameBase;
    for (uint8_t b : bytes) img[at++] = b;
}

// An image that is nothing like the game except where the test needs it: the boot entry `bra.w $7032`, the
// exception-vector target `move #$2700,sr; bra.w $70F6`, optionally the Copylock prologue at $70F6 (what follows it
// is `moveq #7,d5; bra.s *`), and at the routine's exit $763A `moveq #42,d5; bra.s *`.
GameAssets synthetic(bool retail) {
    GameAssets a;
    a.game_image.assign(kGameImageSize, 0);
    put(a.game_image, 0x400, {0x60, 0x00, 0x6C, 0x30});
    put(a.game_image, 0x7032, {0x46, 0xFC, 0x27, 0x00, 0x60, 0x00, 0x00, 0xBE});
    if (retail) put(a.game_image, 0x70F6, {0x2F, 0x0E, 0x4D, 0xFA, 0xFF, 0x86, 0x48, 0xD6, 0xFF, 0xFF, 0x4D, 0xEE, 0x00, 0x40});
    put(a.game_image, 0x7104, {0x7A, 0x07, 0x60, 0xFE}); // what runs if the stub is NOT skipped: moveq #7,d5; bra.s *
    put(a.game_image, 0x763A, {0x7A, 0x2A, 0x60, 0xFE});
    return a;
}

void datum_parsing() {
    CopylockDatum d;
    std::string why;
    CHECK(parse_copylock_datum(kGood, d, why));
    CHECK(d.present && d.seed == 0x36FA43);
    CHECK(parse_copylock_datum(format_copylock_datum(0x123456), d, why) && d.seed == 0x123456);
    CHECK(format_copylock_datum(0x36FA43) == kGood);
    CHECK(format_copylock_datum(0xFF123456) == format_copylock_datum(0x123456)); // only 23 bits are kept
    CHECK(parse_copylock_datum("x2recomp-copylock 1\r\nsignature rob-northen-comp\r\nsyncs ok\r\nseed 0x36fa43\r\n", d, why));

    // Each way a receipt can be wrong.
    const char* bad[] = {
        "",                                                                                                  // empty
        "x2recomp-copylock 2\nsignature rob-northen-comp\nsyncs ok\nseed 0x36FA43\n",                        // version
        "signature rob-northen-comp\nsyncs ok\nseed 0x36FA43\n",                                             // no version
        "x2recomp-copylock 1\nsignature missing\nsyncs ok\nseed 0x36FA43\n",                                  // signature
        "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs bad\nseed 0x36FA43\n",                        // syncs
        "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\n",                                        // no seed
        "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0\n",                                // zero seed
        "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0x800000\n",                         // 24 bits
        "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed zzz\n",                              // not a number
        "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0x36FA43\nextra 1\n",                // unknown line
    };
    for (const char* b : bad) {
        CopylockDatum bd;
        std::string bwhy;
        CHECK(!parse_copylock_datum(b, bd, bwhy));
        CHECK(!bd.present && !bwhy.empty());
    }
}

void datum_files(const fs::path& dir) {
    fs::create_directories(dir);
    CopylockDatum d;
    std::string why;
    CHECK(load_copylock_datum(dir.string(), d, why));   // absent: fine, not present
    CHECK(!d.present && why.empty());
    {
        std::ofstream(dir / kCopylockDatumFile) << kGood;
    }
    CHECK(load_copylock_datum(dir.string(), d, why) && d.present && d.seed == 0x36FA43);
    {
        std::ofstream(dir / kCopylockDatumFile, std::ios::trunc) << "garbage\n";
    }
    CHECK(!load_copylock_datum(dir.string(), d, why));  // present but invalid is an error that names the file
    CHECK(!d.present && why.find(kCopylockDatumFile) != std::string::npos);
}

void recognition() {
    GameAssets retail = synthetic(true), plain = synthetic(false);
    CHECK(is_retail_copylock_image(retail.game_image.data(), retail.game_image.size()));
    CHECK(!is_retail_copylock_image(plain.game_image.data(), plain.game_image.size()));
    CHECK(!is_retail_copylock_image(nullptr, 0));
    CHECK(!is_retail_copylock_image(retail.game_image.data(), 0x6000)); // too short to hold $70F6
    // The lea displacement may differ between pressings; any other byte of the prologue may not.
    retail.game_image[0x70F6 - kGameBase + 4] = 0x12;
    retail.game_image[0x70F6 - kGameBase + 5] = 0x34;
    CHECK(is_retail_copylock_image(retail.game_image.data(), retail.game_image.size()));
    for (size_t i : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(6), size_t(7), size_t(8), size_t(9), size_t(10),
                     size_t(11), size_t(12), size_t(13)}) {
        GameAssets r = synthetic(true);
        r.game_image[0x70F6 - kGameBase + i] ^= 0x01;
        CHECK(!is_retail_copylock_image(r.game_image.data(), r.game_image.size()));
    }
    // The canonical image's replacement stub (a different instruction sequence) is not a retail image.
    GameAssets cracked = synthetic(false);
    put(cracked.game_image, 0x70F6, {0x20, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x21, 0xC0, 0x00, 0x24, 0x60, 0x00, 0x05, 0x38});
    CHECK(!is_retail_copylock_image(cracked.game_image.data(), cracked.game_image.size()));
}

void trap_behaviour() {
    CopylockDatum key;
    key.present = true;
    key.seed = 0x36FA43;
    CopylockDatum none;

    { // retail image with a receipt: the stub is skipped and the CPU lands on the routine's exit
        GameAssets a = synthetic(true);
        Machine m;
        boot_game(m, a);
        CHECK(install_retail_protection(m, a, key) == RetailProtection::Installed);
        m.run_cycles(4000);
        CHECK(m.dreg(5) == 42);
        CHECK((m.pc() & 0xFFFFFF) == 0x763C);
        CHECK(m.dreg(0) == 0 && m.dreg(1) == 0); // the registers the routine returns are left as they were
    }
    { // retail image without a receipt: refused, nothing installed
        GameAssets a = synthetic(true);
        Machine m;
        boot_game(m, a);
        CHECK(install_retail_protection(m, a, none) == RetailProtection::MissingKey);
        m.run_cycles(4000);
        CHECK(m.dreg(5) == 7);
    }
    { // an image without the stub is never touched, receipt or not
        GameAssets a = synthetic(false);
        Machine m;
        boot_game(m, a);
        CHECK(install_retail_protection(m, a, key) == RetailProtection::NotRetail);
        CHECK(install_retail_protection(m, a, none) == RetailProtection::NotRetail);
        m.run_cycles(4000);
        CHECK(m.dreg(5) == 7);
    }
    { // the trap is on the stub address only: an exception restart through $7032 reaches it and is answered too
        GameAssets a = synthetic(true);
        Machine m;
        boot_game(m, a);
        CHECK(install_retail_protection(m, a, key) == RetailProtection::Installed);
        m.run_cycles(4000);
        CHECK(m.dreg(5) == 42);
        m.set_dreg(5, 0);
        m.set_pc(0x7032);
        m.run_cycles(4000);
        CHECK(m.dreg(5) == 42);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: x2_retail_protection_tests WORKDIR\n");
        return 2;
    }
    fs::path dir = argv[1];
    std::error_code ec;
    fs::remove_all(dir, ec);
    datum_parsing();
    datum_files(dir);
    recognition();
    trap_behaviour();
    std::printf("retail_protection: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
