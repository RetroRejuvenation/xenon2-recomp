// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Tests for the disk image layer (runtime/import/disk_image, ext_adf, dms) with synthetic inputs built by
// container_fixtures.h: no game data and no files. Covers DiskImage, container detection, the standard Amiga MFM
// sector decoder, extended ADF, and the DMS decoder (modes none, simple, quick, medium and heavy with a degenerate
// tree, keep-state, fake boot blocks, banner, damaged and truncated archives, a fuzz pass).
//
//   x2_disk_image_tests                 the synthetic tests
//   x2_disk_image_tests --dms-dir DIR   also decodes every .dms file found under DIR (exit 77 if DIR is empty or
//                                       missing): a DMS file that decodes has passed every per-track CRC and
//                                       checksum, so success is strong evidence. Used locally, never in CI.
#include "import/disk_image.h"
#include "import/dms.h"
#include "import/ext_adf.h"
#include "import/sha1.h"

#include "container_fixtures.h"

#include <algorithm>
#include <cctype>
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

constexpr size_t kAdf = 901120, kCyl = 11264;

// A synthetic disk: random sectors, long zero runs and repeated blocks (so every compression mode has work).
Bytes make_adf(uint32_t seed) {
    fx::Rng r{seed};
    Bytes adf(kAdf);
    for (size_t i = 0; i < kAdf; ++i) adf[i] = uint8_t(r.next());
    for (size_t c = 0; c < 80; ++c) {
        uint8_t* cyl = &adf[c * kCyl];
        std::memset(cyl + 1000, 0, 3000);                 // a zero run
        for (size_t i = 5000; i < 5400; ++i) cyl[i] = uint8_t("ABCDEFG"[i % 7]); // a repeating phrase
        std::memset(cyl + 9000, 0x90, 40);                // the RLE escape byte itself
        cyl[100] = 0x90;
    }
    for (size_t c = 4; c < 80; c += 5) std::memset(&adf[c * kCyl], int(c), kCyl); // heavy-degenerate cylinders
    return adf;
}

Bytes cylinder(const Bytes& adf, size_t c) { return Bytes(adf.begin() + long(c * kCyl), adf.begin() + long((c + 1) * kCyl)); }

// Cylinder c packed with a mode chosen by c % 5.
Bytes packed_cylinder(const Bytes& adf, size_t c, fx::LzState& quick_state, fx::LzState& medium_state) {
    Bytes un = cylinder(adf, c);
    fx::DmsTrack t{uint16_t(c), 0, 0, un, un};
    switch (c % 5) {
    case 0: break;
    case 1: t.mode = 1; t.packed = fx::rle_encode(un); break;
    case 2: {
        fx::LzState fresh;
        Bytes rle = fx::rle_encode(un); // QUICK, MEDIUM, DEEP and HEAVY(+flag 4) run an RLE pass after the LZ stage
        t.mode = 2;
        t.packed = fx::quick_encode(rle, fresh);
        t.intermediate = rle.size();
        break;
    }
    case 3: {
        fx::LzState fresh;
        Bytes rle = fx::rle_encode(un);
        t.mode = 3;
        t.packed = fx::medium_encode(rle, fresh);
        t.intermediate = rle.size();
        break;
    }
    default:
        t.mode = 6;
        t.flags = 2;
        t.packed = fx::heavy_single_symbol(un[0]);
        t.intermediate = un.size();
        break;
    }
    (void)quick_state;
    (void)medium_state;
    return fx::dms_track_bytes(t);
}

Bytes dms_of(const Bytes& adf, bool banner_and_fileid = true) {
    std::vector<Bytes> tracks;
    fx::LzState q, m;
    if (banner_and_fileid) {
        const char* text = "A banner";
        Bytes tb(text, text + std::strlen(text));
        tracks.push_back(fx::dms_track_bytes({0xFFFF, 0, 0, tb, tb}));
        Bytes fake(1024, 0x41); // a fake boot block ("track 0" of 1 KB): must be ignored
        tracks.push_back(fx::dms_track_bytes({0, 0, 0, fake, fake}));
    }
    for (size_t c = 0; c < 80; ++c) tracks.push_back(packed_cylinder(adf, c, q, m));
    if (banner_and_fileid) {
        const char* text = "FILE_ID text";
        Bytes tb(text, text + std::strlen(text));
        tracks.push_back(fx::dms_track_bytes({80, 0, 0, tb, tb}));
    }
    return fx::dms_archive(tracks, banner_and_fileid ? 0x108 : 0);
}

// ------------------------------------------------------------------------------------------------------

void test_disk_image() {
    Bytes adf = make_adf(1);
    DiskImage img;
    CHECK(!DiskImage::from_adf(adf.data(), adf.size() - 1, img));
    CHECK(DiskImage::from_adf(adf.data(), adf.size(), img));
    Bytes back;
    std::string why;
    CHECK(img.to_adf(back, why) && back == adf);
    CHECK(img.nonstandard_tracks().empty());
    CHECK(img.sector(0) && std::memcmp(img.sector(0), adf.data(), 512) == 0);
    CHECK(img.sector(1759) && std::memcmp(img.sector(1759), &adf[1759 * 512], 512) == 0);
    CHECK(!img.sector(1760) && !img.sector(-1));
    CHECK(img.sector_sha1(3, 4) == sha1_hex(&adf[(3 * 11 + 4) * 512], 512));
    CHECK(img.track_sha1(7) == sha1_hex(&adf[7 * 5632], 5632));
    CHECK(img.sector_sha1(0, 11).empty() && img.track_sha1(168).empty());

    img.track(6) = Track();                       // missing
    img.track(9).kind = Track::Kind::Raw;         // raw
    img.track(12).status[3] = SectorStatus::BadData; // one bad sector
    CHECK(img.nonstandard_tracks() == (std::vector<int>{6, 9, 12}));
    CHECK(!img.to_adf(back, why));
    CHECK(why.find("track 6 (cylinder 3, side 0): missing") != std::string::npos);
    CHECK(why.find("track 9") != std::string::npos && why.find("raw MFM") != std::string::npos);
    CHECK(why.find("track 12") != std::string::npos && why.find("1 unreadable sector") != std::string::npos);
    CHECK(!img.sector(6 * 11) && !img.sector(12 * 11 + 3) && img.sector(12 * 11 + 4));
    // Zero fill covers only Empty tracks.
    img.track(9) = Track();
    img.track(12) = Track();
    CHECK(img.to_adf(back, why, true) && back.size() == kAdf);
    CHECK(std::all_of(back.begin() + 6 * 5632, back.begin() + 7 * 5632, [](uint8_t b) { return b == 0; }));
    CHECK(!img.to_adf(back, why, false));
    // Many bad tracks: the description is capped.
    for (int t = 0; t < 40; ++t) img.track(t) = Track();
    CHECK(img.describe_nonstandard().find("and ") != std::string::npos);
}

void test_detect() {
    auto d = [](const char* magic, size_t size) {
        Bytes b(size, 0);
        std::memcpy(b.data(), magic, std::strlen(magic));
        return detect_container(b.data(), b.size());
    };
    CHECK(d("\x1F\x8B\x08", 100) == Container::Adz);
    CHECK(d("PK\x03\x04", 100) == Container::Zip);
    CHECK(d("PK\x05\x06", 22) == Container::Zip);
    CHECK(d("DMS!", 100) == Container::Dms);
    CHECK(d("UAE-1ADF", 100) == Container::ExtAdf);
    CHECK(d("UAE--ADF", 100) == Container::ExtAdf1);
    CHECK(d("CAPS", 100) == Container::Ipf);
    CHECK(d("SCP", 1000) == Container::Scp);
    CHECK(d("SCP", 8) == Container::Unknown);
    CHECK(d("HXCPICFE", 100) == Container::Hfe);
    CHECK(d("HXCHFEV3", 100) == Container::Hfe);
    CHECK(d("", kAdf) == Container::Adf);
    CHECK(d("", 2 * kAdf) == Container::AdfHd);
    CHECK(d("", 1000) == Container::Unknown);
    Bytes lha = {0x1A, 0x00, '-', 'l', 'h', '5', '-', 0, 0, 0};
    CHECK(detect_container(lha.data(), lha.size()) == Container::Lha);
    Bytes kf = {0x0D, 0x04, 0x20, 0x00};
    const char* info = "name=KryoFlux DiskSystem, version=3.00s";
    kf.insert(kf.end(), info, info + std::strlen(info));
    kf.resize(64, 0);
    CHECK(detect_container(kf.data(), kf.size()) == Container::KryoFlux);
    // A real stream starts with a host-info block that does not name the device; the block structure is enough.
    Bytes kf2 = {0x0D, 0x04, 0x04, 0x00, 'h', 'o', 's', 't', 'x', 'x'};
    CHECK(detect_container(kf2.data(), kf2.size()) == Container::KryoFlux);
    Bytes notkf = {0x0D, 0x09, 0x04, 0x00, 'h', 'o', 's', 't', 'x', 'x'};
    CHECK(detect_container(notkf.data(), notkf.size()) == Container::Unknown);
    // A raw ADF whose first bytes look like another format is still an ADF, and detection is by content only.
    Bytes adf(kAdf, 0);
    std::memcpy(&adf[100], "KryoFlux", 8);
    std::memcpy(adf.data(), "DOS\0", 4);
    CHECK(detect_container(adf.data(), adf.size()) == Container::Adf);
    CHECK(std::strcmp(container_name(Container::Dms), "DMS") == 0 && std::strcmp(container_name(Container::Unknown), "unknown") == 0);
}

void test_mfm() {
    Bytes adf = make_adf(2);
    const uint8_t* sectors = &adf[5 * 5632];
    for (uint32_t rotate : {0u, 5u, 777u, 12345u}) {
        for (uint32_t extra : {0u, 3u, 100u}) {
            fx::MfmTrack mt = fx::mfm_encode_track(sectors, 5, rotate, extra);
            Track t;
            int n = decode_amiga_mfm_track(mt.bytes.data(), mt.bit_length, 5, t);
            CHECK(n == 11 && t.all_sectors_ok() && std::memcmp(t.sectors.data(), sectors, 5632) == 0);
        }
    }
    // Track number mismatch: nothing decodes.
    fx::MfmTrack mt = fx::mfm_encode_track(sectors, 5, 9, 0);
    Track t;
    CHECK(decode_amiga_mfm_track(mt.bytes.data(), mt.bit_length, 6, t) == 0 && !t.all_sectors_ok());
    // A damaged data area: that sector is BadData, the others are fine.
    mt = fx::mfm_encode_track(sectors, 5, 31, 0, /*corrupt_data_sector=*/3);
    CHECK(decode_amiga_mfm_track(mt.bytes.data(), mt.bit_length, 5, t) == 10);
    CHECK(t.status[3] == SectorStatus::BadData && t.status[2] == SectorStatus::Ok && !t.all_sectors_ok());
    // Damaged header checksum.
    mt = fx::mfm_encode_track(sectors, 5, 0, 0);
    Bytes bad = mt.bytes;
    bad[20] ^= 0x04;
    CHECK(decode_amiga_mfm_track(bad.data(), mt.bit_length, 5, t) <= 10 && !t.all_sectors_ok());
    // Garbage and tiny inputs never crash.
    fx::Rng r{77};
    for (int i = 0; i < 20; ++i) {
        Bytes g(size_t(r.next() % 2000));
        for (auto& b : g) b = uint8_t(r.next());
        decode_amiga_mfm_track(g.data(), uint32_t(g.size() * 8), 0, t);
        decode_amiga_mfm_track(g.data(), uint32_t(g.size() * 8 - (g.empty() ? 0 : 3)), 3, t);
    }
    CHECK(decode_amiga_mfm_track(nullptr, 0, 0, t) == 0);
}

Bytes uae_minus_adf(const Bytes& adf, int raw_track = -1) {
    Bytes b = {'U', 'A', 'E', '-', '-', 'A', 'D', 'F'};
    std::vector<Bytes> data;
    for (int t = 0; t < 160; ++t) {
        if (t == raw_track) {
            Bytes raw(300, 0xAA);
            fx::put16(b, 0x4489);
            fx::put16(b, uint32_t(raw.size()));
            data.push_back(raw);
        } else {
            fx::put16(b, 0);
            fx::put16(b, 5632);
            data.emplace_back(adf.begin() + long(t) * 5632, adf.begin() + long(t + 1) * 5632);
        }
    }
    for (const Bytes& d : data) b.insert(b.end(), d.begin(), d.end());
    return b;
}

void test_ext_adf() {
    Bytes adf = make_adf(3);
    DiskImage img;
    std::string why;
    bool hd = false;
    Bytes out;
    // All normal tracks.
    std::vector<fx::ExtTrack> tr;
    for (int t = 0; t < 160; ++t) tr.push_back({0, Bytes(adf.begin() + long(t) * 5632, adf.begin() + long(t + 1) * 5632)});
    CHECK(read_ext_adf(fx::ext_adf(tr), img, why, hd) && !hd);
    CHECK(img.to_adf(out, why) && out == adf);
    // Every track raw MFM (and rotated): decoded back to the same disk.
    tr.clear();
    for (int t = 0; t < 160; ++t) {
        fx::MfmTrack m = fx::mfm_encode_track(&adf[size_t(t) * 5632], t, uint32_t(t * 7), uint32_t(t % 5));
        tr.push_back({1, m.bytes, m.bit_length, uint8_t(1 + t % 2)});
    }
    CHECK(read_ext_adf(fx::ext_adf(tr), img, why, hd));
    CHECK(img.to_adf(out, why) && out == adf);
    // One raw track that is not standard sectors (a protection track): reported, not decoded.
    tr[10] = {1, Bytes(12500, 0x55), 100000, 1};
    CHECK(read_ext_adf(fx::ext_adf(tr), img, why, hd));
    CHECK(!img.to_adf(out, why) && why.find("track 10") != std::string::npos && why.find("raw MFM") != std::string::npos);
    CHECK(img.track(10).kind == Track::Kind::Raw && img.track(10).bit_length == 100000);
    CHECK(img.nonstandard_tracks() == std::vector<int>{10});
    // Older UAE--ADF: normal tracks, and one raw track.
    CHECK(read_ext_adf(uae_minus_adf(adf), img, why, hd) && img.to_adf(out, why) && out == adf);
    CHECK(read_ext_adf(uae_minus_adf(adf, 20), img, why, hd) && !img.to_adf(out, why) && img.track(20).kind == Track::Kind::Raw);
    // High-density marker; wrong sizes; truncation; bad counts.
    tr.assign(2, {0, Bytes(11264, 0)});
    CHECK(read_ext_adf(fx::ext_adf(tr), img, why, hd) && hd);
    tr.assign(2, {0, Bytes(100, 0)});
    CHECK(!read_ext_adf(fx::ext_adf(tr), img, why, hd) && why.find("5632") != std::string::npos);
    tr.assign(2, {7, Bytes(100, 0)});
    CHECK(!read_ext_adf(fx::ext_adf(tr), img, why, hd) && why.find("unknown type") != std::string::npos);
    Bytes good = fx::ext_adf({{0, Bytes(5632, 1)}, {0, Bytes(5632, 2)}});
    for (size_t cut : {size_t(0), size_t(7), size_t(11), size_t(20), good.size() - 1}) {
        Bytes c(good.begin(), good.begin() + long(cut));
        CHECK(!read_ext_adf(c, img, why, hd));
    }
    Bytes zero_tracks = {'U', 'A', 'E', '-', '1', 'A', 'D', 'F', 0, 0, 0, 0};
    CHECK(!read_ext_adf(zero_tracks, img, why, hd) && why == "no tracks");
    Bytes many = {'U', 'A', 'E', '-', '1', 'A', 'D', 'F', 0, 0, 0xFF, 0xFF};
    CHECK(!read_ext_adf(many, img, why, hd));
    CHECK(!read_ext_adf(Bytes(20, 'x'), img, why, hd));
}

void test_dms_round_trip() {
    Bytes adf = make_adf(4);
    Bytes dms = dms_of(adf);
    DiskImage img;
    DmsResult r = read_dms(dms, img);
    if (r.status != DmsStatus::Ok) std::fprintf(stderr, "  dms: %s\n", r.why.c_str());
    CHECK(r.status == DmsStatus::Ok);
    Bytes out;
    std::string why;
    CHECK(img.to_adf(out, why) && out == adf);
    CHECK(r.info.tracks_decoded == 80 && r.info.banner == "A banner" && r.info.file_id == "FILE_ID text");
    CHECK((r.info.modes_used & 0x4F) == 0x4F); // none, simple, quick, medium, heavy2
    CHECK(r.info.creator_version == 111 && r.info.date == 0x12345678 && r.info.first_track == 0 && r.info.last_track == 79);
    CHECK(!r.info.encrypted && !r.info.no_zero);
    // The same without the fake boot block, banner and FILEID.DIZ tracks.
    CHECK(read_dms(dms_of(adf, false), img).status == DmsStatus::Ok && img.to_adf(out, why) && out == adf);

    // Keep-state: QUICK and MEDIUM tracks continuing from the previous track's window.
    {
        fx::LzState q, m;
        Bytes a = cylinder(adf, 1), b = cylinder(adf, 2);
        Bytes ra = fx::rle_encode(a), rb = fx::rle_encode(b);
        std::vector<Bytes> tracks;
        Bytes pa = fx::quick_encode(ra, q), pb = fx::quick_encode(rb, q);
        tracks.push_back(fx::dms_track_bytes({0, 2, 1, pa, a, ra.size()})); // keep state
        tracks.push_back(fx::dms_track_bytes({1, 2, 0, pb, b, rb.size()}));
        Bytes ma = fx::medium_encode(ra, m), mb = fx::medium_encode(rb, m);
        tracks.push_back(fx::dms_track_bytes({2, 3, 1, ma, a, ra.size()}));
        tracks.push_back(fx::dms_track_bytes({3, 3, 0, mb, b, rb.size()}));
        r = read_dms(fx::dms_archive(tracks), img);
        if (r.status != DmsStatus::Ok) std::fprintf(stderr, "  dms: %s\n", r.why.c_str());
        CHECK(r.status == DmsStatus::Ok && r.info.tracks_decoded == 4);
        // Track numbers are whole cylinders: number n fills tracks 2n and 2n+1.
        CHECK(img.track(0).all_sectors_ok() && std::memcmp(img.track(0).sectors.data(), a.data(), 5632) == 0);
        CHECK(std::memcmp(img.track(1).sectors.data(), &a[5632], 5632) == 0);
        CHECK(std::memcmp(img.track(2).sectors.data(), b.data(), 5632) == 0 && std::memcmp(img.track(3).sectors.data(), &b[5632], 5632) == 0);
        CHECK(std::memcmp(img.track(4).sectors.data(), a.data(), 5632) == 0 && std::memcmp(img.track(7).sectors.data(), &b[5632], 5632) == 0);
    }

    // A track number that appears twice: the later one wins (placement is by track number).
    {
        Bytes a = cylinder(adf, 0), b = cylinder(adf, 1);
        r = read_dms(fx::dms_archive({fx::dms_track_bytes({0, 0, 0, a, a}), fx::dms_track_bytes({0, 0, 0, b, b})}), img);
        CHECK(r.status == DmsStatus::Ok && std::memcmp(img.track(0).sectors.data(), b.data(), 5632) == 0);
    }

    // Partial disks: cylinders that are absent are Empty; DMS says "no zero" when they are meant to be zero.
    {
        std::vector<Bytes> tracks;
        for (size_t c = 0; c < 10; ++c) tracks.push_back(fx::dms_track_bytes({uint16_t(c), 0, 0, cylinder(adf, c), cylinder(adf, c)}));
        r = read_dms(fx::dms_archive(tracks, 1, 2, 0, 9), img);
        CHECK(r.status == DmsStatus::Ok && r.info.no_zero && img.track(20).kind == Track::Kind::Empty);
        CHECK(!img.to_adf(out, why) && img.to_adf(out, why, true) && out.size() == kAdf);
        CHECK(std::memcmp(out.data(), adf.data(), 10 * kCyl) == 0 && out[10 * kCyl] == 0 && out[kAdf - 1] == 0);
    }
}

void test_dms_errors() {
    Bytes adf = make_adf(5);
    DiskImage img;
    Bytes a = cylinder(adf, 0);
    auto read = [&](const Bytes& d) { return read_dms(d, img); };
    Bytes good = fx::dms_archive({fx::dms_track_bytes({0, 0, 0, a, a})});
    CHECK(read(good).status == DmsStatus::Ok);

    CHECK(read(Bytes(10, 'x')).status == DmsStatus::NotDms);
    CHECK(read(Bytes{'D', 'M', 'S', '!', 0, 0}).status == DmsStatus::Truncated);
    Bytes b = good;
    b[20] ^= 1; // header CRC
    DmsResult r = read(b);
    CHECK(r.status == DmsStatus::Corrupt && r.why.find("header CRC") != std::string::npos);
    // Track header CRC, data CRC, checksum, truncation.
    b = good;
    b[56 + 3] ^= 1;
    CHECK(read(b).status == DmsStatus::Corrupt && read(b).why.find("track header CRC") != std::string::npos);
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 0, 0, a, a}, true)}));
    CHECK(r.status == DmsStatus::Corrupt && r.why.find("CRC mismatch in the data of track 0") != std::string::npos);
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 0, 0, a, a}, false, true)}));
    CHECK(r.status == DmsStatus::Corrupt && r.why.find("checksum mismatch") != std::string::npos);
    CHECK(read(Bytes(good.begin(), good.begin() + 56 + 10)).status == DmsStatus::Truncated);
    CHECK(read(Bytes(good.begin(), good.end() - 5)).status == DmsStatus::Truncated);
    CHECK(read(Bytes(good.begin(), good.begin() + 56)).status == DmsStatus::Corrupt); // no tracks at all
    // Junk after the last track is ignored.
    b = good;
    b.insert(b.end(), {'J', 'U', 'N', 'K'});
    CHECK(read(b).status == DmsStatus::Ok);
    // Features that are recognised but not supported.
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 0, 0, a, a})}, 2));
    CHECK(r.status == DmsStatus::Unsupported && r.why.find("encrypted") != std::string::npos);
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 0, 0, a, a})}, 0x10));
    CHECK(r.status == DmsStatus::Unsupported && r.why.find("high-density") != std::string::npos);
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 0, 0, a, a})}, 0, 7));
    CHECK(r.status == DmsStatus::Unsupported && r.why.find("FMS") != std::string::npos);
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 9, 0, a, a})}));
    CHECK(r.status == DmsStatus::Unsupported && r.why.find("unknown compression mode 9") != std::string::npos);
    Bytes half(a.begin(), a.begin() + 5632);
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 0, 0, half, half})}));
    CHECK(r.status == DmsStatus::Unsupported && r.why.find("unpacks to 5632") != std::string::npos);
    // Oversized declared lengths and data that does not decompress.
    Bytes big(40000, 0);
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 0, 0, big, big})}));
    CHECK(r.status == DmsStatus::Corrupt && r.why.find("oversized") != std::string::npos);
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 1, 0, Bytes{0x90, 0xFF, 0x00, 0xFF, 0xFF}, a})}));
    CHECK(r.status == DmsStatus::Corrupt && r.why.find("does not decompress") != std::string::npos); // run past the end
    r = read(fx::dms_archive({fx::dms_track_bytes({0, 2, 0, Bytes{0x00, 0x00}, a}), }));
    CHECK(r.status == DmsStatus::Corrupt);
    // Heavy with a character tree whose lengths over-subscribe the code space.
    {
        fx::BitWriter bw;
        bw.put(300, 9);
        for (int i = 0; i < 300; ++i) bw.put(1, 5);
        bw.put(0, 5);
        bw.put(0, 5);
        r = read(fx::dms_archive({fx::dms_track_bytes({0, 6, 2, bw.finish(), a, kCyl})}));
        CHECK(r.status == DmsStatus::Corrupt);
    }
    // DEEP and HEAVY on garbage cannot loop or read out of bounds.
    fx::Rng rng{5};
    for (int mode : {2, 3, 4, 5, 6}) {
        for (int round = 0; round < 6; ++round) {
            Bytes junk(size_t(100 + rng.next() % 3000));
            for (auto& x : junk) x = uint8_t(rng.next());
            for (uint8_t flags : {uint8_t(0), uint8_t(2), uint8_t(7), uint8_t(15)}) {
                r = read(fx::dms_archive({fx::dms_track_bytes({0, uint8_t(mode), flags, junk, a, kCyl})}));
                CHECK(r.status == DmsStatus::Corrupt || r.status == DmsStatus::Ok);
            }
        }
    }
}

void test_dms_fuzz() {
    Bytes adf = make_adf(6);
    std::vector<Bytes> tracks;
    fx::LzState q, m;
    for (size_t c = 0; c < 15; ++c) tracks.push_back(packed_cylinder(adf, c, q, m));
    Bytes base = fx::dms_archive(tracks);
    fx::Rng r{123};
    int ok = 0, runs = 0;
    DiskImage img;
    for (int i = 0; i < 300; ++i) {
        Bytes b = base;
        if (i % 3 == 0) b.resize(r.next() % b.size());
        else
            for (uint32_t k = 0, n = 1 + r.next() % 8; k < n; ++k) b[r.next() % b.size()] ^= uint8_t(1u << (r.next() % 8));
        DmsResult res = read_dms(b, img);
        ok += res.status == DmsStatus::Ok;
        ++runs;
    }
    std::printf("[dms fuzz] %d damaged archives, no crash (%d still decoded)\n", runs, ok);
    CHECK(runs == 300);
}

// Local only: every .dms below `dir`.
int test_dms_dir(const char* dir) {
    std::error_code ec;
    if (!dir || !*dir || !fs::is_directory(dir, ec)) return 77;
    int files = 0, ok = 0, failed = 0;
    int by_mode[7] = {};
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator() && files < 500; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string(), ext = it->path().extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".dms" || name.rfind("._", 0) == 0) continue; // skip AppleDouble sidecars
        std::ifstream f(it->path(), std::ios::binary);
        Bytes d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (d.size() > (64u << 20)) continue;
        ++files;
        DiskImage img;
        DmsResult r = read_dms(d, img);
        if (r.status == DmsStatus::Ok) {
            ++ok;
            for (int m = 0; m < 7; ++m) by_mode[m] += (r.info.modes_used >> m) & 1;
            Bytes out;
            std::string why;
            // Every decoded track passed its CRC and checksum; the whole disk is complete unless the archive is a
            // partial one.
            if (!img.to_adf(out, why, r.info.no_zero)) std::printf("  note: %s decodes but is a partial disk (%s)\n", name.c_str(), why.c_str());
        } else {
            ++failed;
            std::printf("  %s: %s\n", name.c_str(), r.why.c_str());
        }
    }
    std::printf("[dms files] %d files: %d decoded with every CRC and checksum verified, %d rejected; archives using mode: "
                "none %d, simple %d, quick %d, medium %d, deep %d, heavy1 %d, heavy2 %d\n",
                files, ok, failed, by_mode[0], by_mode[1], by_mode[2], by_mode[3], by_mode[4], by_mode[5], by_mode[6]);
    if (files == 0) return 77;
    CHECK(ok > 0);
    return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--dms-dir") == 0) return test_dms_dir(argv[2]);
    test_disk_image();
    test_detect();
    test_mfm();
    test_ext_adf();
    test_dms_round_trip();
    test_dms_errors();
    test_dms_fuzz();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
