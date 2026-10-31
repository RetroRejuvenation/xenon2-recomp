// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Tests of the retail Xenon 2 import and the variant layer with SYNTHETIC data only (no game bytes): a Copylock-shaped key
// track generated here, a synthetic "retail" disc 1 (random game image carrying only the entry code and the Copylock
// stub bytes, stored under the XOR chain), wrapped as an extended ADF, a plain ADF, SCP and HFE. Covers the key-track
// verification and its failure modes, the key-track import (receipt, manifest, verify/ensure), the refusal paths, the
// opt-in protection patch (disclaimer, confirmation, hash verification, patched=true) and the registry.
//
//   x2_retail_import_tests WORKDIR
#include "import/importer.h"
#include "import/retail.h"
#include "import/sha1.h"
#include "import/variants.h"

#include "container_fixtures.h"
#include "flux_fixtures.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
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

constexpr uint32_t kSeed = 0x36FA43; // the seed of the real key tracks; any 23-bit value works

bool has_message(const Result& r, Message::Level level, const std::string& needle) {
    for (const Message& m : r.messages)
        if (m.level == level && m.text.find(needle) != std::string::npos) return true;
    return false;
}

// --- synthetic Copylock key track ---------------------------------------------------------------------------------

constexpr uint16_t kSync[11] = {0x8A91, 0x8A44, 0x8A45, 0x8A51, 0x8912, 0x8911, 0x8914, 0x8915, 0x8944, 0x8945, 0x8951};

struct KeyOpts {
    uint32_t seed = kSeed;
    uint32_t rotate = 0;       // rotate the stream left by this many cells (the index pulse lands mid-track)
    bool bad_signature = false;
    int corrupt_lfsr_at = -1;  // sector 0 data byte to damage
    bool swap_sectors_3_4 = false;
    int wrong_index_in = -1;   // sector whose index byte is wrong
    int omit_sector = -1;
    bool spurious_sync = false; // an extra copy of a sync word with a wrong index byte, early in the track
    int duplicate = 1;          // the stream holds this many revolutions (a flux decode appends the start of the next)
};

fx::MfmTrack key_track(const KeyOpts& o = {}) {
    std::vector<uint8_t> cells;
    uint32_t prev = 0;
    auto put_byte = [&](uint8_t v) {
        for (int i = 7; i >= 0; --i) {
            uint32_t d = (v >> i) & 1, clock = (!prev && !d) ? 1u : 0u;
            cells.push_back(uint8_t(clock));
            cells.push_back(uint8_t(d));
            prev = d;
        }
    };
    auto put_sync = [&](uint16_t w) {
        for (int i = 15; i >= 0; --i) cells.push_back(uint8_t((w >> i) & 1));
        prev = w & 1;
    };
    auto lfsr_next = [](uint32_t x) { return ((x << 1) & 0x7FFFFF) | (((x >> 22) ^ x) & 1); };
    std::vector<int> order = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    if (o.swap_sectors_3_4) std::swap(order[3], order[4]);
    for (int i = 0; i < 64; ++i) cells.push_back(uint8_t(i & 1));
    for (int s : order) {
        if (s == o.omit_sector) continue;
        if (o.spurious_sync && s == 2) { // a stray sync-looking word with a wrong index byte
            put_sync(kSync[5]);
            put_byte(0x77);
            for (int i = 0; i < 40; ++i) cells.push_back(uint8_t(i & 1));
        }
        put_sync(kSync[s]);
        put_byte(uint8_t(s == o.wrong_index_in ? s + 1 : s));
        uint32_t x = o.seed;
        for (int i = 0; i < 512; ++i) {
            uint8_t v;
            if (s == 0) {
                v = uint8_t((x >> 15) & 0xFF);
                x = lfsr_next(x);
                if (i == o.corrupt_lfsr_at) v ^= 0x10;
            } else if (s == 6 && i < 16) {
                v = uint8_t("Rob Northen Comp"[i]);
                if (o.bad_signature && i == 3) v = 'X';
            } else {
                v = uint8_t(s * 31 + i);
            }
            put_byte(v);
        }
        for (int i = 0; i < 64; ++i) cells.push_back(uint8_t(i & 1));
    }
    const size_t rev = cells.size();
    for (int k = 1; k < o.duplicate; ++k)
        for (size_t i = 0; i < rev; ++i) cells.push_back(cells[i]);
    fx::MfmTrack out;
    out.bit_length = uint32_t(cells.size());
    out.bytes.assign((cells.size() + 7) / 8, 0);
    for (size_t i = 0; i < cells.size(); ++i) {
        size_t src = (i + o.rotate) % cells.size();
        if (cells[src]) out.bytes[i >> 3] = uint8_t(out.bytes[i >> 3] | (0x80 >> (i & 7)));
    }
    return out;
}

Track raw_track(const fx::MfmTrack& m) {
    Track t;
    t.kind = Track::Kind::Raw;
    t.mfm = m.bytes;
    t.bit_length = m.bit_length;
    return t;
}

// --- synthetic retail disc 1 --------------------------------------------------------------------------------------

Bytes chain_encode(const Bytes& plain) {
    Bytes out(plain.size());
    uint32_t prev = kRetailChainInit;
    for (size_t i = 0; i + 1 < plain.size(); i += 2) {
        uint32_t w = (uint32_t(plain[i]) << 8) | plain[i + 1];
        uint32_t e = w ^ prev;
        out[i] = uint8_t(e >> 8);
        out[i + 1] = uint8_t(e);
        prev = w;
    }
    return out;
}

// A random "game image" with the retail entry, the Copylock stub and the check constant in place.
Bytes synthetic_image(uint32_t seed) {
    fx::Rng r{seed};
    Bytes img(kRetailImageSize);
    for (uint8_t& b : img) b = uint8_t(r.next());
    const uint8_t entry[4] = {0x60, 0x00, 0x6C, 0x30};
    std::memcpy(img.data(), entry, 4);
    const Catalogue& real = builtin_catalogue();
    for (const PatchRange& p : real.patch) std::memcpy(&img[p.address - 0x400], p.original.data(), p.original.size());
    return img;
}

Bytes disc1_adf(const Bytes& image) {
    Bytes adf(901120, 0);
    std::memcpy(adf.data(), "DOS\0", 4);
    Bytes stored = chain_encode(image);
    std::memcpy(&adf[kRetailImageOffset], stored.data(), stored.size());
    return adf;
}

Bytes disc2_adf() {
    Bytes adf(901120, 0);
    fx::Rng r{77};
    for (uint8_t& b : adf) b = uint8_t(r.next());
    std::memcpy(&adf[2], "X2B ", 4);
    return adf;
}

// Disc 1 as an extended ADF whose track 1 is the key track (raw).
Bytes disc1_ext_adf(const Bytes& adf, const fx::MfmTrack& key) {
    std::vector<fx::ExtTrack> tracks;
    for (int t = 0; t < 160; ++t) {
        if (t == kKeyTrack) tracks.push_back({1, key.bytes, key.bit_length, 1});
        else tracks.push_back({0, Bytes(adf.begin() + long(t) * 5632, adf.begin() + long(t + 1) * 5632), 0, 1});
    }
    return fx::ext_adf(tracks);
}

Catalogue test_catalogue(const Bytes& image) {
    Catalogue c;
    c.retail.push_back({"retail-test", "synthetic retail pressing", sha1_hex(image.data(), image.size())});
    c.patch = builtin_catalogue().patch;
    return c;
}

Input input(const std::string& name, const Bytes& data) { return Input{name, data, false}; }

// A whole text file, closed before returning: on Windows a directory cannot be removed while a file in it is open
// (the throwing fs::remove_all then ends the test program in std::terminate).
std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void remove_tree(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// --- tests --------------------------------------------------------------------------------------------------------

void test_key_track() {
    {
        CopylockReport r = verify_copylock_track(raw_track(key_track()));
        CHECK(r.genuine && r.seed == kSeed && r.syncs_found == 11 && r.signature && r.lfsr_bytes == 512);
    }
    for (uint32_t seed : {0x000001u, 0x7FFFFFu, 0x123456u}) {
        KeyOpts o;
        o.seed = seed;
        CopylockReport r = verify_copylock_track(raw_track(key_track(o)));
        CHECK(r.genuine && r.seed == seed);
    }
    // The index pulse can fall anywhere: the order is circular.
    for (uint32_t rot : {1u, 12345u, 40000u, 90000u}) {
        KeyOpts o;
        o.rotate = rot;
        CopylockReport r = verify_copylock_track(raw_track(key_track(o)));
        CHECK(r.genuine && r.seed == kSeed);
    }
    // A stream of two revolutions (a flux decode appends the start of the next), rotated into the middle of a sector.
    {
        KeyOpts o;
        o.duplicate = 2;
        o.rotate = 31337;
        CHECK(verify_copylock_track(raw_track(key_track(o))).genuine);
    }
    {
        KeyOpts o;
        o.spurious_sync = true; // extra sync-like words do not hide the real ones
        CHECK(verify_copylock_track(raw_track(key_track(o))).genuine);
    }
    {
        KeyOpts o;
        o.bad_signature = true;
        CopylockReport r = verify_copylock_track(raw_track(key_track(o)));
        CHECK(!r.genuine && !r.signature && r.why.find("signature") != std::string::npos);
    }
    {
        KeyOpts o;
        o.corrupt_lfsr_at = 300;
        CopylockReport r = verify_copylock_track(raw_track(key_track(o)));
        CHECK(!r.genuine && r.why.find("LFSR") != std::string::npos && r.lfsr_bytes == 300);
    }
    {
        KeyOpts o;
        o.swap_sectors_3_4 = true;
        CopylockReport r = verify_copylock_track(raw_track(key_track(o)));
        CHECK(!r.genuine && r.why.find("order") != std::string::npos);
    }
    {
        KeyOpts o;
        o.wrong_index_in = 2;
        CopylockReport r = verify_copylock_track(raw_track(key_track(o)));
        CHECK(!r.genuine && r.syncs_found == 10);
    }
    {
        KeyOpts o;
        o.omit_sector = 8;
        CopylockReport r = verify_copylock_track(raw_track(key_track(o)));
        CHECK(!r.genuine && r.why.find("only 10 of the eleven") != std::string::npos);
    }
    {
        Track t; // empty
        CHECK(!verify_copylock_track(t).genuine);
        Track d;
        d.kind = Track::Kind::Decoded;
        CopylockReport r = verify_copylock_track(d);
        CHECK(!r.genuine && r.why.find("ordinary Amiga sectors") != std::string::npos);
        Track tiny;
        tiny.kind = Track::Kind::Raw;
        tiny.mfm = Bytes(100, 0xAA);
        tiny.bit_length = 800;
        CHECK(!verify_copylock_track(tiny).genuine);
        Track lying = raw_track(key_track());
        lying.bit_length += 1000; // claims more bits than the buffer holds
        CHECK(!verify_copylock_track(lying).genuine);
        Track noise;
        noise.kind = Track::Kind::Raw;
        fx::Rng r2{5};
        noise.mfm.resize(20000);
        for (uint8_t& b : noise.mfm) b = uint8_t(r2.next());
        noise.bit_length = 160000;
        CHECK(!verify_copylock_track(noise).genuine);
    }
}

void test_image_helpers() {
    Bytes img = synthetic_image(1);
    CHECK(is_retail_image(img));
    CHECK(chain_decode(chain_encode(img).data(), img.size()) == img);
    Bytes bad = img;
    bad[kRetailStubAddress - 0x400 + 6] ^= 1;
    CHECK(!is_retail_image(bad));
    bad = img;
    bad[1] ^= 1;
    CHECK(!is_retail_image(bad));
    Bytes shorter(img.begin(), img.end() - 2);
    CHECK(!is_retail_image(shorter));
    // The stub's lea displacement is not compared.
    bad = img;
    bad[kRetailStubAddress - 0x400 + 4] ^= 0x55;
    CHECK(is_retail_image(bad));
    // decode from an ADF and from a DiskImage agree
    Bytes adf = disc1_adf(img);
    Bytes a;
    CHECK(decode_retail_image_adf(adf.data(), adf.size(), a) && a == img);
    DiskImage di;
    CHECK(DiskImage::from_adf(adf.data(), adf.size(), di));
    Bytes b;
    std::string why;
    CHECK(decode_retail_image(di, b, why) && b == img);
    // an unreadable sector in the image range names its track
    di.track(90).status[3] = SectorStatus::BadData;
    CHECK(!decode_retail_image(di, b, why) && why.find("track 90") != std::string::npos);
    CHECK(!decode_retail_image_adf(adf.data(), adf.size() - 1, a));
    CHECK(copylock_receipt(kSeed, "0123456789abcdef0123456789abcdef01234567") ==
          "x2recomp-copylock 2\nsignature rob-northen-comp\nsyncs ok\nseed 0x36FA43\nimage-sha1 0123456789abcdef0123456789abcdef01234567\n");
}

void test_registry_and_patch_table() {
    std::set<std::string> ids;
    for (const VariantInfo& v : all_variants()) {
        CHECK(!v.id.empty() && ids.insert(v.id).second);
        CHECK(!v.name.empty() && !v.identified_by.empty());
        if (v.supported && (v.recipe == Recipe::Bs1OfsExecutable || v.recipe == Recipe::RetailKeyTrack)) CHECK(v.game_sha1.size() == 40);
        if (v.supported) CHECK(v.disc2_sha1.size() == 40);
        if (!v.supported) CHECK(v.recipe == Recipe::Recognised);
    }
    CHECK(ids.count("bs1-t3") && ids.count("retail-2874") && ids.count("retail-0297") && ids.count("retail-patched"));
    CHECK(ids.count("bs1-timecircle") && ids.count("others4") && ids.count("others4-a"));
    CHECK(ids.count("crack-pna") && ids.count("crack-qtx") && ids.count("crack-flashtro-16"));
    CHECK(ids.count("crack-black-monks") && ids.count("crack-predators-9") && ids.count("crack-flashtro-16-a") && ids.count("others4-a-d2"));
    for (const VariantInfo& v : all_variants()) {
        if (v.recipe == Recipe::CrackedRetail) CHECK(v.supported && !v.canonical && v.game_sha1.empty());
        if (v.recipe == Recipe::Bs1OfsExecutable) CHECK(v.canonical && v.game_sha1 == canonical_game_image_sha1());
    }
    // every catalogued disc has a 40-digit lower-case SHA-1 and a registry id of its own
    {
        auto sha_ok = [](const std::string& h) {
            if (h.size() != 40) return false;
            for (char ch : h)
                if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
            return true;
        };
        const Catalogue& cat = builtin_catalogue();
        for (const CrackedRetail& c : cat.cracked) CHECK(sha_ok(c.sha1) && !c.id.empty() && !c.name.empty());
        for (const RefusedDisc& r : cat.refused_disc2) CHECK(sha_ok(r.sha1) && r.why[0] != 0);
        CHECK(find_bs1_disc1("cb366028a1d0904601b16da745f2557ff08debfb") && std::string(find_bs1_disc1("cb366028a1d0904601b16da745f2557ff08debfb")->id) == "bs1-timecircle");
        CHECK(find_bs1_disc2("5e5d7865508da11ccc41d79edf0c7da1b51277d6")->quality == 2);
        CHECK(find_refused_disc1("2405da96182c63fc6a7e48b7f19f2904f0a44ed9") && find_refused_disc1("1b4855f98f7df8d5052e5578d387dbf486068f8f"));
        CHECK(find_refused_disc2("31997fddbf4174ea175ba25375864a2a75a1c445") && !find_refused_disc2("a748afc7a142335a40cc46fb7785dea820770241"));
        // the canonical disc 2's six level modules: inside the disc, in order, not overlapping
        const std::vector<Disc2Module>& mods = canonical_disc2_modules();
        CHECK(mods.size() == 6);
        int end = 0;
        for (const Disc2Module& m : mods) {
            CHECK(m.first_sector >= end && m.first_sector + m.sectors <= 1760 && sha_ok(m.sha1));
            end = m.first_sector + m.sectors;
        }
    }
    const Catalogue& c = builtin_catalogue();
    CHECK(c.retail.size() == 2 && c.find_retail("fc84cc6f1ed47e3da0d5dc913632c705140f1e0a")->id == "retail-2874");
    CHECK(c.find_retail("8d02d25be543d21d3545b321ec774b41afc9d7e3")->id == "retail-0297");
    CHECK(c.find_retail("0000000000000000000000000000000000000000") == nullptr);
    // the patch table: 18 bytes in two ranges, same-sized, inside the image, not overlapping
    size_t total = 0;
    for (size_t i = 0; i < c.patch.size(); ++i) {
        const PatchRange& p = c.patch[i];
        CHECK(p.original.size() == p.patched.size() && !p.original.empty());
        CHECK(p.address >= 0x400 && p.address - 0x400 + p.original.size() <= kRetailImageSize);
        CHECK(p.original != p.patched);
        total += p.patched.size();
        for (size_t j = i + 1; j < c.patch.size(); ++j) {
            const PatchRange& q = c.patch[j];
            CHECK(p.address + p.original.size() <= q.address || q.address + q.original.size() <= p.address);
        }
    }
    CHECK(total == 18 && c.patch.size() == 2);
    CHECK(c.patch[0].address == kRetailStubAddress && c.patch[0].original.size() == kRetailStubLength);
    // applying it to a synthetic image gives exactly the patched bytes and leaves everything else alone
    Bytes img = synthetic_image(2), before = img;
    std::string why;
    std::vector<std::string> applied;
    CHECK(apply_patch_ranges(img, c.patch, why, &applied));
    CHECK(applied.size() == 2 && applied[0] == "$70F6+14" && applied[1] == "$76AE+4");
    size_t changed = 0;
    for (size_t i = 0; i < img.size(); ++i) changed += img[i] != before[i];
    CHECK(changed <= 18 && changed >= 10);
    for (const PatchRange& p : c.patch)
        CHECK(Bytes(img.begin() + long(p.address - 0x400), img.begin() + long(p.address - 0x400 + p.patched.size())) == p.patched);
    CHECK(!is_retail_image(img)); // the stub is gone
    // a second application is refused (the originals are no longer there) and changes nothing
    Bytes again = img;
    CHECK(!apply_patch_ranges(again, c.patch, why) && again == img && why.find("expected retail bytes") != std::string::npos);
    // a wrong or short image is refused untouched
    Bytes wrong = before;
    wrong[0x76AE - 0x400 + 1] ^= 1;
    Bytes wrong_copy = wrong;
    CHECK(!apply_patch_ranges(wrong, c.patch, why) && wrong == wrong_copy);
    Bytes shorter(100);
    CHECK(!apply_patch_ranges(shorter, c.patch, why) && why.find("too short") != std::string::npos);
    // the disclaimer says what it must
    std::string all;
    for (const std::string& l : protection_patch_disclaimer()) all += l + "\n";
    CHECK(all.find("YOUR retail disc 1") != std::string::npos && all.find("18 bytes") != std::string::npos);
    CHECK(all.find("own") != std::string::npos && all.find("law") != std::string::npos && all.find("preferred") != std::string::npos);
}

void test_key_track_import(const fs::path& work) {
    Bytes image = synthetic_image(10);
    Bytes adf1 = disc1_adf(image), adf2 = disc2_adf();
    fx::MfmTrack key = key_track();
    Bytes ext = disc1_ext_adf(adf1, key);
    Catalogue cat = test_catalogue(image);
    ImportOptions known;
    known.catalogue = &cat;

    // the extended ADF carries the key track: imported, receipt written, recognised through the catalogue
    Result r = import_inputs({input("d1.adf.ext", ext), input("d2.adf", adf2)}, known);
    CHECK(r.status == Status::Ok);
    CHECK(r.game_image == image && r.disc2 == adf2);
    CHECK(!r.canonical && !r.patched && r.variant_id == "retail-test" && r.pressing_id == "retail-test" && r.container == "extended ADF");
    CHECK(r.copylock_receipt == copylock_receipt(kSeed, sha1_hex(image.data(), image.size())));
    CHECK(has_message(r, Message::Info, "key track verified") && !has_message(r, Message::Warn, "not a catalogued"));
    CHECK(has_message(r, Message::Info, "not the canonical"));

    // not in the built-in catalogue: imported with a warning
    Result u = import_inputs({input("d1.adf.ext", ext), input("d2.adf", adf2)});
    CHECK(u.status == Status::Ok && u.variant_id == "retail-unrecognised" && !u.copylock_receipt.empty());
    CHECK(has_message(u, Message::Warn, "not a catalogued retail pressing"));

    // a damaged key track: refused, naming the reason and the options
    KeyOpts bad;
    bad.corrupt_lfsr_at = 7;
    Result b = import_inputs({input("d1.adf.ext", disc1_ext_adf(adf1, key_track(bad))), input("d2.adf", adf2)}, known);
    CHECK(b.status == Status::Unsupported && has_message(b, Message::Error, "key track (cylinder 0, head 1) could not be verified"));
    CHECK(has_message(b, Message::Error, "--apply-protection-patch"));
    CHECK(has_message(b, Message::Error, "IPF or a flux dump"));

    // a key track on a disc whose game image is not the retail image: not imported as retail
    Bytes damaged_image = image;
    damaged_image[kRetailStubAddress - 0x400] ^= 0xFF;
    Result n = import_inputs({input("d1.adf.ext", disc1_ext_adf(disc1_adf(damaged_image), key)), input("d2.adf", adf2)}, known);
    CHECK(n.status != Status::Ok);

    // the plain ADF of the same disc cannot carry the key track
    Result p = import_inputs({input("d1.adf", adf1), input("d2.adf", adf2)}, known);
    CHECK(p.status == Status::Unsupported && has_message(p, Message::Error, "cannot carry the Copylock key track"));
    CHECK(has_message(p, Message::Error, "--i-confirm-i-own-the-original"));

    // write, verify, and the receipt belongs to the manifest
    fs::path dir = work / "assets";
    remove_tree(dir);
    std::string err, why;
    CHECK(write_assets(dir.string(), r, err));
    CHECK(fs::exists(dir / "copylock.txt") && fs::exists(dir / "manifest.txt"));
    {
        std::string text = read_text(dir / "copylock.txt");
        CHECK(text == r.copylock_receipt);
        std::string mt = read_text(dir / "manifest.txt");
        CHECK(mt.find("importer_version=" + std::to_string(kImporterVersion) + "\n") != std::string::npos);
        CHECK(mt.find("variant=retail-test\n") != std::string::npos && mt.find("canonical=false\n") != std::string::npos);
        CHECK(mt.find("patched=false\n") != std::string::npos && mt.find("copylock=present\n") != std::string::npos);
        CHECK(mt.find("container=extended ADF\n") != std::string::npos && mt.find("copylock.txt_sha1=") != std::string::npos);
    }
    CHECK(verify_assets(dir.string(), why));
    {
        std::ofstream f(dir / "copylock.txt", std::ios::binary | std::ios::trunc);
        f << "x2recomp-copylock 2\nsignature rob-northen-comp\nsyncs ok\nseed 0x000001\nimage-sha1 0123456789abcdef0123456789abcdef01234567\n";
    }
    CHECK(!verify_assets(dir.string(), why) && why.find("copylock.txt") != std::string::npos);
    fs::remove(dir / "copylock.txt");
    CHECK(!verify_assets(dir.string(), why) && why.find("copylock.txt") != std::string::npos);
    // an import without a receipt over the same folder removes a stale receipt and says copylock=absent
    {
        std::ofstream f(dir / "copylock.txt", std::ios::binary | std::ios::trunc);
        f << "stale";
    }
    Result canon;
    canon.status = Status::Ok;
    canon.game_image = Bytes(kRetailImageSize, 7);
    canon.disc2 = adf2;
    canon.variant_id = "bs1-t3";
    CHECK(write_assets(dir.string(), canon, err));
    CHECK(!fs::exists(dir / "copylock.txt") && verify_assets(dir.string(), why));
    // a receipt dropped next to an import whose manifest records none is not accepted (it would arm the runtime's trap)
    {
        std::ofstream f(dir / "copylock.txt", std::ios::binary | std::ios::trunc);
        f << copylock_receipt(kSeed, sha1_hex(canon.game_image.data(), canon.game_image.size()));
    }
    CHECK(!verify_assets(dir.string(), why) && why.find("copylock.txt") != std::string::npos && why.find("records no") != std::string::npos);
    fs::remove(dir / "copylock.txt");
    CHECK(verify_assets(dir.string(), why));
}

void test_patch_option(const fs::path& work) {
    Bytes image = synthetic_image(20);
    Bytes adf1 = disc1_adf(image), adf2 = disc2_adf();
    Catalogue cat = test_catalogue(image);
    Bytes ext = disc1_ext_adf(adf1, key_track());
    Bytes expected = image;
    std::string why;
    std::vector<std::string> ranges;
    CHECK(apply_patch_ranges(expected, cat.patch, why, &ranges));

    ImportOptions opt;
    opt.catalogue = &cat;
    opt.apply_protection_patch = true;

    // no confirmation: stops, with the disclaimer, and imports nothing
    Result n = import_inputs({input("d1.adf", adf1), input("d2.adf", adf2)}, opt);
    CHECK(n.status == Status::ConfirmationRequired && n.game_image.empty());
    CHECK(has_message(n, Message::Info, "YOUR retail disc 1") && has_message(n, Message::Error, "--i-confirm-i-own-the-original"));

    opt.confirmed_ownership = true;
    // a plain ADF (no key track), catalogued pressing: patched
    Result p = import_inputs({input("d1.adf", adf1), input("d2.adf", adf2)}, opt);
    CHECK(p.status == Status::Ok && p.patched && !p.canonical && p.variant_id == "retail-patched");
    CHECK(p.game_image == expected && p.game_image != image);
    CHECK(p.patched_ranges == ranges && p.copylock_receipt.empty() && p.pressing_id == "retail-test");
    CHECK(has_message(p, Message::Info, "protection patch applied to $70F6+14, $76AE+4"));
    CHECK(has_message(p, Message::Info, "YOUR retail disc 1")); // the disclaimer is printed whenever the option is used
    CHECK(!is_retail_image(p.game_image));
    // from a source that does carry the key track, the explicit option still means "patch", without a receipt
    Result k = import_inputs({input("d1.adf.ext", ext), input("d2.adf", adf2)}, opt);
    CHECK(k.status == Status::Ok && k.patched && k.game_image == expected && k.copylock_receipt.empty() && k.container == "extended ADF");
    CHECK(has_message(k, Message::Info, "key track verified, so the default import") && !has_message(p, Message::Info, "key track verified, so the default import"));
    // not a catalogued pressing: refused (nothing patched), even with the confirmation
    ImportOptions builtin;
    builtin.apply_protection_patch = builtin.confirmed_ownership = true;
    Result u = import_inputs({input("d1.adf", adf1), input("d2.adf", adf2)}, builtin);
    CHECK(u.status == Status::Unsupported && u.game_image.empty() && has_message(u, Message::Error, "catalogued retail pressing"));
    // the image does not hold the expected retail bytes at a patch range: refused
    Bytes odd = image;
    odd[0x76AE - 0x400] ^= 0x01;
    Catalogue odd_cat = test_catalogue(odd);
    ImportOptions oo = opt;
    oo.catalogue = &odd_cat;
    Result o = import_inputs({input("d1.adf", disc1_adf(odd)), input("d2.adf", adf2)}, oo);
    CHECK(o.status == Status::Unsupported && has_message(o, Message::Error, "expected retail bytes"));
    // the manifest records it, and a stale receipt is removed
    fs::path dir = work / "patched";
    remove_tree(dir);
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "copylock.txt", std::ios::binary);
        f << "x2recomp-copylock 2\nsignature rob-northen-comp\nsyncs ok\nseed 0x36FA43\nimage-sha1 0123456789abcdef0123456789abcdef01234567\n";
    }
    std::string err;
    CHECK(write_assets(dir.string(), p, err) && !fs::exists(dir / "copylock.txt"));
    std::string mt = read_text(dir / "manifest.txt");
    CHECK(mt.find("patched=true\n") != std::string::npos && mt.find("patched_ranges=$70F6+14,$76AE+4\n") != std::string::npos);
    CHECK(mt.find("copylock=absent\n") != std::string::npos && mt.find("variant=retail-patched\n") != std::string::npos);
    CHECK(verify_assets(dir.string(), why));
    // an outdated patched import is never re-done silently
    {
        std::string old = mt;
        const std::string line = "importer_version=" + std::to_string(kImporterVersion);
        old.replace(old.find(line), line.size(), "importer_version=0");
        std::ofstream f(dir / "manifest.txt", std::ios::binary | std::ios::trunc);
        f << old;
    }
    CHECK(!ensure_imported(dir.string(), why) && why.find("confirmed again") != std::string::npos);
}

void test_cli(const fs::path& work) {
    Bytes image = synthetic_image(30);
    Bytes adf1 = disc1_adf(image), adf2 = disc2_adf();
    fs::path src = work / "cli_src";
    remove_tree(src);
    fs::create_directories(src);
    auto write = [&](const char* name, const Bytes& b) {
        std::ofstream f(src / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    };
    write("d1.adf", adf1);
    write("d1.ext", disc1_ext_adf(adf1, key_track()));
    write("d2.adf", adf2);
    fs::path out = work / "cli_out";
    remove_tree(out);
    std::vector<std::string> plain = {(src / "d1.adf").string(), (src / "d2.adf").string()};
    std::vector<std::string> ext = {(src / "d1.ext").string(), (src / "d2.adf").string()};
    CHECK(import_cli(plain, out.string()) == 3 && !fs::exists(out / "manifest.txt"));
    ImportOptions want;
    want.apply_protection_patch = true;
    CHECK(import_cli(plain, out.string(), want) == 4 && !fs::exists(out / "manifest.txt"));
    want.confirmed_ownership = true;
    CHECK(import_cli(plain, out.string(), want) == 3); // not a catalogued pressing
    CHECK(import_cli(ext, out.string()) == 0 && fs::exists(out / "copylock.txt") && fs::exists(out / "manifest.txt"));
    std::string why;
    CHECK(ensure_imported(out.string(), why));
    CHECK(import_cli({(src / "nothing").string()}, out.string()) == 2);
    CHECK(list_variants_cli() == 0);
    CHECK(diagnose_cli({(src / "d1.ext").string()}) == 0);
    CHECK(diagnose_cli({(src / "missing").string()}) == 2);
    // diagnostics report the key track and the image without printing it
    std::vector<std::string> lines;
    ImportOptions d;
    d.diagnostics = &lines;
    Result r = import_paths({(src / "d1.ext").string(), (src / "d2.adf").string()}, d);
    std::string all;
    for (const std::string& l : lines) all += l + "\n";
    CHECK(r.status == Status::Ok);
    CHECK(all.find("Rob Northen Copylock verified, seed 0x36FA43") != std::string::npos);
    CHECK(all.find("159 standard tracks, 1 raw") != std::string::npos);
    CHECK(all.find("retail entry and Copylock stub present") != std::string::npos && all.find("unrecognised retail pressing") != std::string::npos);
}

// A cracked copy of the retail disk 1: the game image stored plain at $70C00, recognised by the SHA-1 of the whole disc.
void test_cracked_retail() {
    Bytes image = synthetic_image(50);
    // a cracked image has no Copylock stub: the patch bytes are in place already
    {
        std::string why;
        Catalogue real;
        real.patch = builtin_catalogue().patch;
        CHECK(apply_patch_ranges(image, real.patch, why));
    }
    Bytes adf1(901120, 0);
    std::memcpy(adf1.data(), "DOS\0", 4);
    std::memcpy(&adf1[kRetailImageOffset], image.data(), image.size());
    Bytes adf2 = disc2_adf();
    const std::string disc_sha = sha1_hex(adf1.data(), adf1.size());
    Catalogue cat;
    cat.patch = builtin_catalogue().patch;
    cat.cracked.push_back({disc_sha, "crack-test", "synthetic crack of the retail disk 1", false, "synthetic note"});

    ImportOptions opt;
    opt.catalogue = &cat;
    Result r = import_inputs({input("crack.adf", adf1), input("d2.adf", adf2)}, opt);
    CHECK(r.status == Status::Ok && r.game_image == image && r.disc2 == adf2);
    CHECK(!r.canonical && !r.patched && r.variant_id == "crack-test" && r.copylock_receipt.empty() && r.container == "ADF");
    CHECK(has_message(r, Message::Info, "cracked copy of the retail disk 1") && has_message(r, Message::Info, "not the canonical"));
    CHECK(!has_message(r, Message::Warn, "trainer"));
    // a trainer build is flagged
    cat.cracked[0].trainer = true;
    Result t = import_inputs({input("crack.adf", adf1), input("d2.adf", adf2)}, opt);
    CHECK(t.status == Status::Ok && has_message(t, Message::Warn, "carries a trainer"));
    cat.cracked[0].trainer = false;
    // not catalogued: a disc with a custom loader and no readable volume stays unrecognised
    Result u = import_inputs({input("crack.adf", adf1), input("d2.adf", adf2)});
    CHECK(u.status == Status::BadInput && has_message(u, Message::Error, "no readable file system"));
    // the entry code damaged: refused, naming the problem
    Bytes bad = adf1;
    bad[kRetailImageOffset + 1] ^= 0x10;
    Catalogue bad_cat = cat;
    bad_cat.cracked[0].sha1 = sha1_hex(bad.data(), bad.size());
    ImportOptions bo;
    bo.catalogue = &bad_cat;
    Result b = import_inputs({input("crack.adf", bad), input("d2.adf", adf2)}, bo);
    CHECK(b.status == Status::BadInput && has_message(b, Message::Error, "does not start with bra.w $7032"));
    // written like any import: manifest names the variant, not canonical, no receipt
    fs::path dir = fs::temp_directory_path() / "x2_retail_import_crack_assets";
    remove_tree(dir);
    std::string err, why;
    CHECK(write_assets(dir.string(), r, err) && verify_assets(dir.string(), why) && !fs::exists(dir / "copylock.txt"));
    std::string mt = read_text(dir / "manifest.txt");
    CHECK(mt.find("variant=crack-test\n") != std::string::npos && mt.find("canonical=false\n") != std::string::npos);
    remove_tree(dir);
    // the patch option never touches a cracked disc (it has no stub): it is not a retail image, so it is imported as it is
    ImportOptions po = opt;
    po.apply_protection_patch = po.confirmed_ownership = true;
    Result p = import_inputs({input("crack.adf", adf1), input("d2.adf", adf2)}, po);
    CHECK(p.status == Status::Ok && !p.patched && p.game_image == image);
}

// Disc 2 handling: equivalent, refused (damaged or another build) and unknown with module differences.
void test_disc2_rules() {
    Bytes image = synthetic_image(60);
    Bytes adf1 = disc1_adf(image);
    Bytes adf2 = disc2_adf();
    Catalogue cat = test_catalogue(image);
    ImportOptions opt;
    opt.catalogue = &cat;
    opt.apply_protection_patch = opt.confirmed_ownership = true; // a plain retail disc 1 imports through the patch route
    // an unknown disc 2: warned about, with the level modules that differ named (a synthetic disc 2 differs in all six)
    Result r = import_inputs({input("d1.adf", adf1), input("d2.adf", adf2)}, opt);
    CHECK(r.status == Status::Ok && has_message(r, Message::Warn, "level module(s) 1, 2, 3, 4, 5, 6 of 6 differ"));
    // a disc 2 whose catalogued SHA-1 is refused (a damaged copy, another build): not imported
    cat.refused_disc2.push_back({"", "d2-test", "synthetic damaged disc 2", "a damaged copy: synthetic"});
    static std::string held;
    held = sha1_hex(adf2.data(), adf2.size());
    cat.refused_disc2.back().sha1 = held.c_str();
    Result d = import_inputs({input("d1.adf", adf1), input("d2.adf", adf2)}, opt);
    CHECK(d.status == Status::Unsupported && has_message(d, Message::Error, "a damaged copy: synthetic"));
    CHECK(has_message(d, Message::Error, "disc 2 not found"));
}

// The key track as flux: a drive's transition times, wrapped as SCP and HFE, with only the tracks the import reads.
void test_flux_sources() {
    Bytes image = synthetic_image(40);
    Bytes adf1 = disc1_adf(image), adf2 = disc2_adf();
    fx::MfmTrack key = key_track();
    fx::FluxModel model;
    model.jitter = 0.02;

    // SCP: tracks 0, the key track 1 and the image tracks 82..128
    {
        std::vector<FluxTrack> flux(160);
        for (int t = 0; t < 160; ++t) {
            bool needed = t == 0 || t == kKeyTrack || (t >= 82 && t <= 128);
            if (!needed) continue;
            model.seed = 100u + uint32_t(t);
            fx::MfmTrack m = t == kKeyTrack ? key : fx::mfm_encode_track(&adf1[size_t(t) * 5632], t, uint32_t(t) * 977u, 8);
            fx::Timeline tl = fx::make_timeline(m, 3, 0, 0, model);
            flux[size_t(t)] = fx::scp_style_revs(tl);
        }
        Bytes scp = fx::scp_file(flux, 2);
        Result r = import_inputs({input("d1.scp", scp), input("d2.adf", adf2)});
        CHECK(r.status == Status::Ok && r.container == "SCP" && !r.copylock_receipt.empty() && r.game_image == image);
        CHECK(r.disc2 == adf2);
    }
    // HFE: the key track's bit cells go through as they are
    {
        std::vector<fx::MfmTrack> side0(80), side1(80);
        for (int t = 0; t < 160; ++t) {
            fx::MfmTrack m = t == kKeyTrack ? key : fx::mfm_encode_track(&adf1[size_t(t) * 5632], t, 0, 0);
            (t % 2 ? side1 : side0)[size_t(t / 2)] = m;
        }
        Bytes hfe = fx::hfe_file(side0, side1);
        Result r = import_inputs({input("d1.hfe", hfe), input("d2.adf", adf2)});
        CHECK(r.status == Status::Ok && r.container == "HFE" && !r.copylock_receipt.empty() && r.game_image == image);
    }
}

} // namespace

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_retail_import_test";
    fs::create_directories(work);
    test_key_track();
    test_image_helpers();
    test_registry_and_patch_table();
    test_key_track_import(work);
    test_patch_option(work);
    test_cracked_retail();
    test_disc2_rules();
    test_cli(work);
    test_flux_sources();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
