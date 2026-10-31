// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Atari ST machine on the user's own Kixx dump (public identity: sha1 323d6859635b6b82c86363db048b26e0f3df994c, the
// 819,200-byte ".st"): the real boot sector runs on the emulated WD1772, reads the front-end and the resident engine through
// the controller and its DMA, and the front-end runs to its title screen on the MFP, VBL and YM. Skipped (exit 77) without it.
//
//   x2_st_kixx_tests [DIR]      DIR (or $X2_ST_DEV_DIR) holds the dump; every .st file there is checked against the sha1
#include "import/sha1.h"
#include "st/st_machine.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace x2::st;
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

constexpr const char* kSha1 = "323d6859635b6b82c86363db048b26e0f3df994c";

bool find_disk(const std::string& dir, std::vector<uint8_t>& out) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return false;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || it->path().extension() != ".st") continue;
        std::vector<uint8_t> bytes;
        {
            std::ifstream f(it->path(), std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        if (bytes.size() == 819200 && x2::import::sha1_hex(bytes.data(), bytes.size()) == kSha1) {
            out = std::move(bytes);
            return true;
        }
    }
    return false;
}

struct Run {
    uint64_t hash_800 = 0, hash_1300 = 0, hash_end = 0;
    uint64_t vbls = 0, mfp = 0, unmapped = 0, fatal = 0;
    size_t sectors = 0;
    std::vector<FdcDma::ReadRecord> log;
    std::map<uint32_t, IoStat> io;
    int ym_writes = 0;
};

Run run(const std::vector<uint8_t>& image, uint32_t frames) {
    Run r;
    StDisk disk;
    std::string why;
    std::vector<uint8_t> copy = image;
    if (!make_st_disk(std::move(copy), disk, why)) { std::fprintf(stderr, "  disk: %s\n", why.c_str()); ++g_failures; return r; }
    StMachine m;
    m.insert_disk(std::move(disk));
    m.enable_io_summary();
    m.fdc().log_reads(true);
    if (!m.boot(why)) { std::fprintf(stderr, "  boot: %s\n", why.c_str()); ++g_failures; return r; }
    for (uint32_t f = 0; f < frames; ++f) {
        m.run_frame();
        if (f == 800) r.hash_800 = m.picture_hash();
        if (f == 1300) r.hash_1300 = m.picture_hash();
        if (m.fatal_vector_hits()) { std::fprintf(stderr, "  faulted at frame %u: %s\n", f, m.fault_report().c_str()); break; }
    }
    r.hash_end = m.picture_hash();
    r.vbls = m.vbl_taken();
    r.mfp = m.mfp_taken();
    r.unmapped = m.unmapped_accesses();
    r.fatal = m.fatal_vector_hits();
    r.log = m.fdc().read_log();
    r.sectors = r.log.size();
    r.io = m.io_summary();
    return r;
}
} // namespace

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "";
    if (const char* e = std::getenv("X2_ST_DEV_DIR"); e && dir.empty()) dir = e;
    std::vector<uint8_t> image;
    if (!find_disk(dir, image)) {
        std::printf("st kixx tests: the Kixx dump (sha1 %s) is not available: skipped\n", kSha1);
        return 77;
    }
    constexpr uint32_t kFrames = 3000;
    const Run a = run(image, kFrames);
    // The boot sector's loads: 250 sectors of tracks 1-25 (side 0) to $10000, then 460 of tracks 34-79 to $400, by single
    // sector commands in order.
    CHECK(a.sectors == 710);
    if (a.sectors == 710) {
        CHECK(a.log[0].track == 1 && a.log[0].side == 0 && a.log[0].sector == 1 && a.log[0].addr == 0x10000 && a.log[0].stored);
        CHECK(a.log[249].track == 25 && a.log[249].sector == 10 && a.log[249].addr == 0x10000 + 249 * 512);
        CHECK(a.log[250].track == 34 && a.log[250].side == 0 && a.log[250].sector == 1 && a.log[250].addr == 0x400);
        CHECK(a.log[709].track == 79 && a.log[709].sector == 10 && a.log[709].addr == 0x400 + 459 * 512);
        bool in_order = true;
        for (size_t i = 1; i < 250; ++i) in_order = in_order && a.log[i].cycle > a.log[i - 1].cycle && a.log[i].addr == a.log[i - 1].addr + 512;
        CHECK(in_order);
        // A real drive: a sector takes about 20 ms (one frame is 20 ms at 50 Hz), so the first load takes about 250 frames.
        CHECK(a.log[249].cycle > 240ull * kCyclesPerFrame && a.log[249].cycle < 320ull * kCyclesPerFrame);
    }
    // The program ran the whole time: a vertical blank every frame, the MFP's Timer A at about 7,979 Hz after the front-end
    // starts it, nothing unmapped and no CPU exception.
    CHECK(a.vbls == kFrames);
    CHECK(a.mfp > 100000);
    CHECK(a.unmapped == 0 && a.fatal == 0);
    // The registers the front-end and engine use: the controller and DMA, the MFP, the keyboard ACIA, the Shifter and the YM.
    for (uint32_t addr : {0xFF8604u, 0xFF8606u, 0xFF8609u, 0xFF860Bu, 0xFF860Du, 0xFFFA01u, 0xFFFA07u, 0xFFFA13u, 0xFFFA19u, 0xFFFA1Fu,
                          0xFFFC02u, 0xFF8800u, 0xFF8802u, 0xFF8201u, 0xFF8203u, 0xFF8207u, 0xFF8240u})
        CHECK(a.io.count(addr) == 1);
    CHECK(a.io.count(0xFF8800) && a.io.at(0xFF8802).writes > 100000); // the music player is writing the YM
    // Golden pictures after frames 800 (the Bitmap Brothers caption being typed), 1300 (the credits screen) and 2999 (the title); the frame numbers of
    // `x2recomp --game st --hash-at`.
    CHECK(a.hash_800 == 0xedd545c2abf264caull);
    CHECK(a.hash_1300 == 0x39982ed38e4c5404ull);
    CHECK(a.hash_end == 0xa79eb6e9a7773366ull);
    // The machine is deterministic: a second run is identical.
    const Run b = run(image, kFrames);
    CHECK(b.hash_1300 == a.hash_1300 && b.hash_800 == a.hash_800 && b.hash_end == a.hash_end && b.mfp == a.mfp && b.sectors == a.sectors);
    std::printf("st kixx tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
