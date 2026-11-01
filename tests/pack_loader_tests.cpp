// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Asset packs v2, workstream 1b (runtime/pack): the JSON reader, path rules, ZIP reader, manifest parser, layered loader, discovery
// and the validator, on synthetic packs written to a scratch directory. No game data. The 20,000-iteration manifest fuzz and the ZIP and
// PNG mutation runs are part of this program, and the CI sanitizer jobs run it under ASan and UBSan.
//
//   x2_pack_loader_tests SOURCE_DIR SCRATCH_DIR [iterations]
#include "gfx/image_io.h"
#include "import/sha1.h"
#include "pack/pack_api.h"
#include "pack/pack_check.h"
#include "pack/pack_discovery.h"
#include "pack/pack_json.h"
#include "pack/pack_manifest.h"
#include "pack/pack_paths.h"
#include "pack/pack_png.h"
#include "pack/pack_source.h"
#include "pack/pack_stack.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace x2;
using namespace x2::pack;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

std::string g_src, g_tmp;

// ---- helpers: files, PNGs, ZIPs, WAVs ---------------------------------------------------------------------------------------

void put_file(const std::string& path, const std::string& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

template <class F>
void put_png(const std::string& path, int w, int h, F f) {
    fs::create_directories(fs::path(path).parent_path());
    std::vector<uint8_t> px;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 4; ++k) px.push_back(uint8_t(f(x, y) >> (8 * k)));
    write_png(path, px, w, h, true);
}
void put_flat(const std::string& path, int w, int h, uint32_t c = 0xFF0000FFu) {
    put_png(path, w, h, [c](int, int) { return c; });
}

uint32_t crc_of(const std::string& d) { return import::crc32(reinterpret_cast<const uint8_t*>(d.data()), d.size()); }

struct ZipFile {
    std::string name, data;
    int method = 0;          // 0 stored, 8 "deflate" (written as a deflate stored block)
    uint32_t usize = 0;      // 0: data.size() (a nonzero value declares a different unpacked size)
    uint16_t flags = 0;
    bool bad_crc = false;
};

// Raw deflate as one stored block (valid for inflate, not compressed).
std::string deflate_stored(const std::string& d) {
    std::string o;
    o += char(1);
    o += char(d.size() & 255); o += char(d.size() >> 8 & 255);
    o += char(~d.size() & 255); o += char((~d.size() >> 8) & 255);
    return o + d;
}

std::string make_zip(const std::vector<ZipFile>& files) {
    std::string out, cd;
    auto u16 = [](std::string& o, uint32_t v) { o += char(v & 255); o += char(v >> 8 & 255); };
    auto u32 = [&](std::string& o, uint32_t v) { u16(o, v & 0xFFFF); u16(o, v >> 16); };
    for (const ZipFile& f : files) {
        const std::string packed = f.method == 8 ? deflate_stored(f.data) : f.data;
        const uint32_t crc = crc_of(f.data) ^ (f.bad_crc ? 1u : 0u), off = uint32_t(out.size());
        const uint32_t us = f.usize ? f.usize : uint32_t(f.data.size());
        u32(out, 0x04034b50); u16(out, 20); u16(out, f.flags); u16(out, f.method); u16(out, 0); u16(out, 0);
        u32(out, crc); u32(out, uint32_t(packed.size())); u32(out, us); u16(out, uint16_t(f.name.size())); u16(out, 0);
        out += f.name + packed;
        u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, f.flags); u16(cd, f.method); u16(cd, 0); u16(cd, 0);
        u32(cd, crc); u32(cd, uint32_t(packed.size())); u32(cd, us); u16(cd, uint16_t(f.name.size()));
        u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, off);
        cd += f.name;
    }
    const uint32_t cd_off = uint32_t(out.size());
    out += cd;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, uint16_t(files.size())); u16(out, uint16_t(files.size()));
    u32(out, uint32_t(cd.size())); u32(out, cd_off); u16(out, 0);
    return out;
}

// A PCM16 WAV.
std::string make_wav(int rate, int channels, const std::vector<int16_t>& pcm) {
    std::string o = "RIFF";
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) o += char(v >> (8 * i) & 255); };
    auto u16 = [&](uint32_t v) { o += char(v & 255); o += char(v >> 8 & 255); };
    u32(uint32_t(36 + pcm.size() * 2));
    o += "WAVEfmt ";
    u32(16); u16(1); u16(uint32_t(channels)); u32(uint32_t(rate)); u32(uint32_t(rate * channels * 2)); u16(uint32_t(channels * 2)); u16(16);
    o += "data";
    u32(uint32_t(pcm.size() * 2));
    for (int16_t s : pcm) u16(uint16_t(s));
    return o;
}
std::vector<int16_t> tone(size_t frames, int channels = 1, int amp = 8000) {
    std::vector<int16_t> v;
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) v.push_back(int16_t(((i / 16) & 1) ? amp : -amp));
    return v;
}

// A test codec for WAV, standing in for the decoders of WS3a.
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
bool read_all(ByteSource& s, std::vector<uint8_t>& out, uint64_t limit) {
    if (s.size() > limit) return false;
    out.resize(size_t(s.size()));
    return out.empty() || s.read(0, out.data(), out.size()) == out.size();
}
bool wav_probe_impl(const std::vector<uint8_t>& b, const AudioLimits& lim, AudioInfo& info, size_t& data_at, size_t& data_len, std::string& why) {
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) { why = "not a WAV file"; return false; }
    const int ch = b[22] | b[23] << 8;
    const int rate = int(le32(&b[24]));
    if (ch < 1 || ch > lim.max_channels) { why = "more than two channels"; return false; }
    if (rate < lim.min_rate || rate > lim.max_rate) { why = "sample rate " + std::to_string(rate) + " is outside " + std::to_string(lim.min_rate) + "-" + std::to_string(lim.max_rate); return false; }
    data_at = 44;
    data_len = std::min<size_t>(le32(&b[40]), b.size() - 44);
    info.format = "wav";
    info.channels = ch;
    info.rate = rate;
    info.frames = int64_t(data_len / 2 / size_t(ch));
    if (uint64_t(info.frames) > lim.max_frames) { why = "too long"; return false; }
    return true;
}
bool wav_probe(ByteSource& s, const AudioLimits& lim, AudioInfo& info, std::string& why) {
    std::vector<uint8_t> b;
    if (!read_all(s, b, lim.max_file_bytes)) { why = "cannot read"; return false; }
    size_t at = 0, len = 0;
    return wav_probe_impl(b, lim, info, at, len, why);
}
bool wav_decode(ByteSource& s, const AudioLimits& lim, DecodedClip& out, AudioInfo& info, std::string& why) {
    std::vector<uint8_t> b;
    if (!read_all(s, b, lim.max_file_bytes)) { why = "cannot read"; return false; }
    size_t at = 0, len = 0;
    if (!wav_probe_impl(b, lim, info, at, len, why)) return false;
    out.channels = info.channels;
    out.rate = info.rate;
    float peak = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        const int16_t v = int16_t(b[at + i] | b[at + i + 1] << 8);
        out.pcm.push_back(v);
        peak = std::max(peak, std::abs(float(v)) / 32768.0f);
    }
    info.peak = info.rate == 11111 ? 2.0f : peak;  // 11111 Hz stands for a file that was clipped when it was made
    (void)lim;
    return true;
}
const AudioCodec kWavCodec{wav_probe, wav_decode};

// ---- helpers: messages ------------------------------------------------------------------------------------------------------

bool said(const PackReport& r, const std::string& what, int severity = -1) {
    for (const PackMessage& m : r.messages)
        if (m.format().find(what) != std::string::npos && (severity < 0 || int(m.severity) == severity)) return true;
    return false;
}
std::string dump_report(const PackReport& r) {
    std::string o;
    for (const PackMessage& m : r.messages) o += "    " + m.format() + "\n";
    return o;
}

const char* kEmptyMap = "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\"";

// Writes a pack directory: manifest text and the listed pictures (name -> w, h).
std::string make_pack(const std::string& name, const std::string& manifest, const std::map<std::string, std::pair<int, int>>& pics = {}) {
    const std::string dir = g_tmp + "/" + name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    put_file(dir + "/manifest.json", manifest);
    for (const auto& [p, wh] : pics) put_flat(dir + "/" + p, wh.first, wh.second);
    return dir;
}

std::string key16(const char* hex, int w, int h) { return std::string(hex) + "-" + std::to_string(w) + "x" + std::to_string(h); }
const char* kH1 = "0123456789abcdef";
const char* kH2 = "1123456789abcdef";
const char* kH3 = "2123456789abcdef";

// ---- JSON ----------------------------------------------------------------------------------------------------------------------

void test_json() {
    std::printf("json: grammar, limits\n");
    Json j;
    std::string why;
    CHECK(parse_json("{\"a\": [1, 2.5, -3e2, true, false, null, \"x\\u00e9\\n\"], \"b\": {}}", j, why) && j.is_obj() && j.get("a")->a.size() == 7);
    CHECK(j.get("a")->a[2].n == -300.0 && j.get("a")->a[6].s == "x\xC3\xA9\n");
    CHECK(parse_json("\xEF\xBB\xBF{}", j, why));                       // BOM
    CHECK(parse_json("\"\\ud83d\\ude00\"", j, why) && j.s == "\xF0\x9F\x98\x80");  // a surrogate pair
    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "[01]", "[1.]", "[.5]", "[+1]", "[0x10]", "[NaN]", "[Infinity]", "[1e999]", "\"\\q\"",
                            "\"a\nb\"", "{} x", "'a'", "[1 2]", "{\"a\":}", "tru", "\"\\u12\""})
        CHECK(!parse_json(bad, j, why) && !why.empty());
    // Duplicate keys: the last wins.
    CHECK(parse_json("{\"a\": 1, \"a\": 2}", j, why) && j.get("a")->n == 2 && j.o.size() == 2);
    // Depth and node limits.
    std::string deep(100, '[');
    CHECK(!parse_json(deep, j, why) && why.find("too deeply") != std::string::npos);
    std::string flat = "[";
    for (int i = 0; i < 200; ++i) flat += "1,";
    flat += "1]";
    CHECK(parse_json(flat, j, why, 1000) && !parse_json(flat, j, why, 100) && why.find("too many values") != std::string::npos);
    int v = 0;
    Json num;
    num.t = Json::Num;
    num.n = 7;
    CHECK(num.as_int(1, 8, v) && v == 7 && !num.as_int(8, 9, v));
    num.n = 7.5;
    CHECK(!num.as_int(1, 8, v));
}

// ---- paths ---------------------------------------------------------------------------------------------------------------------

void test_paths() {
    std::printf("paths: rules, folding, normalising\n");
    for (const char* good : {"a.png", "sprites/a b.png", "music/track-01.ogg", "x/y/z/w.json", "n.u.l.png", "COM10.png", "con-sole.png"}) CHECK(safe_pack_path(good));
    for (const char* bad : {"", "/a.png", "../a.png", "a/../b.png", "a/./b.png", "a//b.png", "a/", "a\\b.png", "c:x.png", "a:b", "CON", "con.png", "dir/NUL.txt", "aux", "Com1.x",
                            "lpt9", "a.png.", "a ", "dir /a.png", "a\tb.png", "a\x01z.png"})
        CHECK(!safe_pack_path(bad));
    CHECK(!safe_pack_path(std::string(241, 'a')) && safe_pack_path(std::string(240, 'a')));
    CHECK(path_problem("../x").find("..") != std::string::npos && path_problem("a.png").empty());
    CHECK(fold_path("Sprites/ABC.PNG") == "sprites/abc.png");
    bool changed = false;
    CHECK(normalise_member_name("a\\b\\c.png", changed) == "a/b/c.png" && changed);
    CHECK(normalise_member_name("a/b.png", changed) == "a/b.png" && !changed);
}

// ---- versions ------------------------------------------------------------------------------------------------------------------

void test_versions() {
    std::printf("versions: semver, constraints, ids\n");
    Semver s;
    CHECK(parse_semver("1.2.3", s) && s.major == 1 && s.minor == 2 && s.patch == 3 && !s.pre);
    CHECK(parse_semver("1.2.3-rc.1", s) && s.pre && parse_semver("1.2.3+build-5", s) && !s.pre);
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "1.2.x", "v1.2.3", "1.2.3-", "1.2.3_x", "-1.2.3", "1..3", "1.2.3-a+b"}) CHECK(!parse_semver(bad, s));
    CHECK(version_satisfies("1.2.3", ">=1.2") && version_satisfies("1.2.3", "1.2") && version_satisfies("1.2.3", ">=1.2.3") && !version_satisfies("1.2.2", ">=1.2.3"));
    CHECK(version_satisfies("2.0.0", ">1.9") && !version_satisfies("1.9.0", ">1.9.0") && version_satisfies("1.9.1", ">1.9.0"));
    CHECK(version_satisfies("1.2.9", "=1.2") && !version_satisfies("1.3.0", "=1.2") && version_satisfies("1.2.3", "=1.2.3") && !version_satisfies("1.2.4", "=1.2.3"));
    CHECK(!version_satisfies("1.0.0-rc1", ">=1.0") && version_satisfies("1.0.1-rc1", ">=1.0") && !version_satisfies("garbage", ">=1.0") && !version_satisfies("1.0.0", "garbage"));
    CHECK(valid_constraint(">=0.5") && valid_constraint("=1.0.2") && valid_constraint("2.1") && !valid_constraint(">=1") && !valid_constraint("<1.0") && !valid_constraint(""));
    CHECK(valid_pack_id("a1") && valid_pack_id("my.pack_v2-x") && !valid_pack_id("a") && !valid_pack_id("-ab") && !valid_pack_id("Ab") && !valid_pack_id(std::string(65, 'a')));
    CHECK(slug_of("My Great Pack!") == "my-great-pack" && slug_of("  ") == "" && slug_of("!!") == "" && slug_of("ÄÖ") == "");
}

// ---- PNG -----------------------------------------------------------------------------------------------------------------------

void test_png() {
    std::printf("png: header probe, limits, expansion guard\n");
    const std::string p = g_tmp + "/png/a.png";
    put_flat(p, 5, 3);
    const std::string f = slurp(p);
    const std::vector<uint8_t> bytes(f.begin(), f.end());
    int w = 0, h = 0;
    std::string why;
    CHECK(png_size(bytes.data(), bytes.size(), w, h, why) && w == 5 && h == 3);
    Picture pic;
    CHECK(decode_png(bytes.data(), bytes.size(), pic, why) && pic.w == 5 && pic.px.size() == 15 && pic.px[0] == 0xFF0000FFu);
    CHECK(!decode_png(bytes.data(), bytes.size(), pic, why, 4) && why.find("too large or empty (5x3") != std::string::npos);
    CHECK(!decode_png(bytes.data(), bytes.size(), pic, why, 4096, 10));
    CHECK(!png_size(bytes.data(), 7, w, h, why) && !decode_png(bytes.data(), 7, pic, why));
    std::vector<uint8_t> bad = bytes;
    bad[20] ^= 0xFF;  // IHDR width: the chunk CRC no longer matches
    CHECK(!png_size(bad.data(), bad.size(), w, h, why) && why.find("CRC") != std::string::npos);
    // A header that claims a 16384 x 2048 16-bit RGBA picture with a few bytes of image data: refused before the output is allocated.
    std::string ihdr = std::string("IHDR") + std::string("\x00\x00\x40\x00\x00\x00\x08\x00\x10\x06\x00\x00\x00", 13);
    auto chunk = [](const std::string& type_and_data) {
        const uint32_t crc = crc_of(type_and_data), len = uint32_t(type_and_data.size() - 4);
        std::string c;
        for (int i = 3; i >= 0; --i) c += char(len >> (8 * i) & 255);
        c += type_and_data;
        for (int i = 3; i >= 0; --i) c += char(crc >> (8 * i) & 255);
        return c;
    };
    const std::string zl = std::string("\x78\x01", 2) + std::string("\x03\x00", 2) + std::string("\x00\x00\x00\x01", 4);
    const std::string bomb = std::string("\x89PNG\r\n\x1a\n", 8) + chunk(ihdr) + chunk("IDAT" + zl) + chunk("IEND");
    const std::vector<uint8_t> bb(bomb.begin(), bomb.end());
    CHECK(!decode_png(bb.data(), bb.size(), pic, why, 16384, 32u << 20) && why.find("corrupted image data") != std::string::npos);
}

// ---- ZIP -----------------------------------------------------------------------------------------------------------------------

std::vector<ZipFile> zip_of(std::initializer_list<std::pair<const char*, std::string>> l) {
    std::vector<ZipFile> v;
    for (const auto& [n, d] : l) v.push_back({n, d});
    return v;
}

void test_zip() {
    std::printf("zip: lazy members, folders, collisions, refusals\n");
    const PackLimits lim;
    std::string why;
    auto open = [&](const std::string& name, const std::string& bytes, const PackLimits& l = PackLimits()) {
        const std::string path = g_tmp + "/zip/" + name;
        put_file(path, bytes);
        return open_pack_source(path, l, why);
    };
    // root manifest, stored and "deflated" members, a nested folder
    std::vector<ZipFile> files = {{"manifest.json", "{}"}, {"a.png", "AAAA"}, {"d/b.bin", "BBBBBBBB", 8}, {"__MACOSX/x", "junk"}};
    auto src = open("ok.zip", make_zip(files));
    CHECK(src && src->is_zip() && src->notes().empty());
    if (src) {
        std::vector<uint8_t> out;
        CHECK(src->read("a.png", 100, out, why) && std::string(out.begin(), out.end()) == "AAAA");
        CHECK(src->read("d/b.bin", 100, out, why) && std::string(out.begin(), out.end()) == "BBBBBBBB");
        CHECK(!src->read("a.png", 3, out, why) && why.find("too large (4 bytes, limit 3)") != std::string::npos);
        CHECK(!src->read("zzz", 100, out, why) && why == "missing file 'zzz'");
        CHECK(!src->read("../a.png", 100, out, why) && why.find("unsafe path '../a.png'") != std::string::npos);
        CHECK(!src->exists("__MACOSX/x") && src->exists("a.png") && src->streamable("a.png") && !src->streamable("d/b.bin"));
        uint64_t size = 0;
        CHECK(src->file_size("d/b.bin", size) && size == 8 && !src->file_size("nope", size));
        auto stream = src->open("a.png", 100, why);
        CHECK(stream && stream->size() == 4);
        if (stream) {
            char buf[8] = {};
            CHECK(stream->read(1, buf, 8) == 3 && std::string(buf, 3) == "AAA" && stream->read(4, buf, 8) == 0 && stream->read(0, buf, 2) == 2);
        }
        auto dstream = src->open("d/b.bin", 100, why);
        CHECK(dstream && dstream->size() == 8);
        const std::vector<std::string> names = src->names();
        CHECK(names.size() == 3);
    }
    // one top-level folder holds the pack
    src = open("folder.zip", make_zip({{"mypack/manifest.json", "{}"}, {"mypack/a.png", "AAAA"}, {"other/x.png", "zz"}}));
    CHECK(src && src->exists("a.png") && !src->exists("x.png") && !src->exists("other/x.png"));
    // two manifests in two folders, none at all
    CHECK(!open("two.zip", make_zip({{"a/manifest.json", "{}"}, {"b/manifest.json", "{}"}})) && why == "no manifest.json in the ZIP file");
    CHECK(!open("none.zip", make_zip({{"a.png", "x"}})) && why == "no manifest.json in the ZIP file");
    // backslash names (old Windows tools) are read as '/', with a note
    src = open("bs.zip", make_zip({{"manifest.json", "{}"}, {"sprites\\a.png", "AAAA"}}));
    CHECK(src && src->exists("sprites/a.png") && src->notes().size() == 1 && src->notes()[0].find("backslashes") != std::string::npos);
    // an unsafe member is ignored with a note; the pack still opens
    src = open("evil.zip", make_zip({{"manifest.json", "{}"}, {"../evil.png", "x"}, {"con.png", "x"}, {"ok.png", "x"}}));
    CHECK(src && !src->exists("../evil.png") && src->exists("ok.png") && !src->exists("con.png") && src->notes().size() == 2);
    // names that collide when case is folded: an error
    CHECK(!open("case.zip", make_zip({{"manifest.json", "{}"}, {"A.png", "x"}, {"a.PNG", "y"}})) && why.find("collide") != std::string::npos);
    CHECK(!open("dup.zip", make_zip({{"manifest.json", "{}"}, {"a.png", "x"}, {"a.png", "y"}})) && why.find("collide") != std::string::npos);
    // member problems are per member and found when it is read
    {
        ZipFile enc{"enc.png", "x"};
        enc.flags = 1;
        ZipFile crc{"crc.png", "payload"};
        crc.bad_crc = true;
        ZipFile bomb{"bomb.bin", std::string(50, 'z'), 8, 3u << 20};
        ZipFile big{"big.bin", "tiny", 0, 0};
        ZipFile huge{"huge.bin", "x", 0, 600u << 20};
        ZipFile method{"m.bin", "x"};
        method.method = 12;
        src = open("problems.zip", make_zip({{"manifest.json", "{}"}, enc, crc, bomb, huge, method}));
        std::vector<uint8_t> out;
        CHECK(src != nullptr);
        if (src) {
            CHECK(!src->read("enc.png", 1000, out, why) && why.find("encrypted") != std::string::npos);
            CHECK(!src->read("crc.png", 1000, out, why) && why.find("CRC mismatch") != std::string::npos);
            CHECK(!src->read("bomb.bin", 100u << 20, out, why) && why.find("compression ratio above 1000:1") != std::string::npos);
            CHECK(!src->read("huge.bin", 1u << 30, out, why) && why.find("too large") != std::string::npos);
            CHECK(!src->read("m.bin", 1000, out, why) && why.find("method 12") != std::string::npos);
            CHECK(src->read("manifest.json", 100, out, why));  // the others do not affect it
        }
    }
    // not a ZIP, truncated, ZIP64 marker, too many members, too large
    CHECK(!open("garbage.zip", std::string(500, 'x')) && why.find("not a usable ZIP file") != std::string::npos);
    CHECK(!open("tiny.zip", "PK") && why.find("not a usable ZIP file") != std::string::npos);
    std::string z64 = make_zip({{"manifest.json", "{}"}});
    z64.replace(z64.size() - 6, 4, std::string("\xFF\xFF\xFF\xFF", 4));  // central directory offset
    CHECK(!open("z64.zip", z64) && why.find("zip64") != std::string::npos);
    PackLimits few;
    few.max_zip_members = 2;
    CHECK(!open("few.zip", make_zip({{"manifest.json", "{}"}, {"a", "1"}, {"b", "2"}}), few) && why.find("more than 2 members") != std::string::npos);
    PackLimits small;
    small.max_zip = 100;
    CHECK(!open("small.zip", make_zip({{"manifest.json", std::string(300, 'x')}}), small) && why.find("too large") != std::string::npos);
    // not there at all
    CHECK(!open_pack_source(g_tmp + "/zip/missing.zip", lim, why) && why == "no such asset pack (a directory or a .zip file)");
    // laziness: a corrupt member that nothing reads does not stop the pack
    std::string zl = make_zip({{"manifest.json", "{}"}, {"a.png", "AAAA"}, {"b.png", "BBBB"}});
    const size_t at = zl.find("BBBB");
    zl[at] = 'X';
    src = open("lazy.zip", zl);
    std::vector<uint8_t> out;
    CHECK(src && src->read("a.png", 100, out, why) && !src->read("b.png", 100, out, why) && why.find("CRC") != std::string::npos);
    // directories: containment and names
    const std::string dir = make_pack("dirsrc", "{}");
    put_file(dir + "/sub/x.bin", "xyz");
    put_file(g_tmp + "/outside.bin", "secret");
    src = open_pack_source(dir, lim, why);
    CHECK(src && !src->is_zip() && src->exists("sub/x.bin") && !src->exists("../outside.bin") && !src->exists("sub"));
    if (src) {
        CHECK(!src->read("sub", 100, out, why) && why == "missing file 'sub'");
        const std::vector<std::string> names = src->names();
        CHECK(names.size() == 2 && names[0] == "manifest.json" && names[1] == "sub/x.bin");
        auto st = src->open("sub/x.bin", 100, why);
        char buf[4] = {};
        CHECK(st && st->size() == 3 && st->read(1, buf, 4) == 2 && std::string(buf, 2) == "yz");
        CHECK(!src->open("sub/x.bin", 2, why) && why.find("too large") != std::string::npos);
    }
    std::error_code ec;
    fs::create_directory_symlink(g_tmp + "/zip", dir + "/link", ec);
    if (!ec) {
        src = open_pack_source(dir, lim, why);
        CHECK(src && !src->exists("link/ok.zip"));  // a symlink out of the pack is not followed
        CHECK(src && !src->read("link/ok.zip", 100, out, why) && why.find("leaves the pack directory") != std::string::npos);
    }
}

// ---- manifest parsing ----------------------------------------------------------------------------------------------------------

struct Parsed {
    Manifest m;
    PackReport rep;
    std::string fatal;
    bool ok = false;
};
Parsed parse(const std::string& text, bool validate = false) {
    Parsed p;
    Json j;
    std::string why;
    if (!parse_json(text, j, why)) {
        p.fatal = "json: " + why;
        return p;
    }
    ParseOptions po;
    po.validate = validate;
    p.ok = parse_manifest(j, "t", po, p.m, p.rep, p.fatal);
    return p;
}

void test_manifest() {
    std::printf("manifest: v1 and v2 sections, ranges, refusals\n");
    // The two example manifests of the schema directory parse without a message.
    for (const char* ex : {"v1-graphics.example.json", "v2-full.example.json"}) {
        Parsed p = parse(slurp(g_src + "/docs/schema/examples/" + ex), true);
        CHECK(p.ok && p.rep.count(Severity::Error) == 0);
        if (p.rep.count(Severity::Error)) std::fprintf(stderr, "%s\n%s", ex, dump_report(p.rep).c_str());
    }
    Parsed full = parse(slurp(g_src + "/docs/schema/examples/v2-full.example.json"), true);
    CHECK(full.m.version == 2 && full.m.id == "example.hd" && full.m.scale == 4 && full.m.scope.games == 0x3 && full.m.requires_x2recomp == ">=0.5.0");
    CHECK(full.m.requires_packs.size() == 1 && full.m.requires_packs[0].id == "base.pack" && full.m.requires_packs[0].version == ">=1.0");
    CHECK(full.m.graphics.size() == 1 && full.m.graphics[0].keys.size() == 2 && full.m.graphics[0].flash_png == "sprites/ship_f.png" && full.m.graphics[0].scale == 4);
    CHECK(full.m.backgrounds.size() == 1 && full.m.backgrounds[0].ring_lines == 1024 && full.m.atlases.size() == 2 && full.m.atlases[1].has_grid && full.m.atlases[1].grid_keys.size() == 2);
    CHECK(full.m.stars && full.m.stars->shape == StarsEntry::Shape::Round && full.m.stars->size == 1.5);
    CHECK(full.m.screens.size() == 1 && full.m.screens[0].signature.blocks.size() == 1 && full.m.screens[0].ref_palette.size() == 2 && full.m.screens[0].ref_palette[1] == 0xFFFFFF);
    CHECK(full.m.panel && full.m.panel->cells.size() == 1 && full.m.panel->background_png == "panel/bg.png");
    CHECK(full.m.clips.size() == 2 && !full.m.clips[0].speech && full.m.clips[1].speech && full.m.clips[1].original == OriginalMode::Mix && full.m.clips[1].original_mix_db == -12.0f);
    CHECK(full.m.clips[0].ref_period == 214 && full.m.clips[0].gain_db == -3.0f && full.m.clips[1].cd0_file == "SPEECH63");
    CHECK(full.m.slots.size() == 3 && full.m.music_defaults.crossfade_ms && *full.m.music_defaults.crossfade_ms == 250);
    CHECK(full.m.slots[0].slot.loop.kind == LoopKind::Points && full.m.slots[0].slot.loop.start_s == 4.0 && full.m.slots[0].slot.loop.end_s == 80.5);
    CHECK(full.m.slots[1].file_null && full.m.slots[1].slot.mute && full.m.slots[2].slot.intro.path == "music/c5i.flac" && full.m.slots[2].slot.loop.kind == LoopKind::Whole);

    // Refusals of the whole pack.
    auto fatal = [&](const std::string& text, const char* what) {
        Parsed p = parse(text);
        const bool good = !p.ok && p.fatal.find(what) != std::string::npos;
        if (!good) std::fprintf(stderr, "  expected '%s', got ok=%d '%s'\n", what, p.ok, p.fatal.c_str());
        return good;
    };
    CHECK(fatal("[]", "must be a JSON object"));
    CHECK(fatal("{\"version\": 1}", "\"format\" must be"));
    CHECK(fatal("{\"format\": \"x\", \"version\": 1}", "\"format\" must be"));
    CHECK(fatal("{\"format\": \"x2-asset-pack\"}", "unsupported \"version\""));
    CHECK(fatal("{\"format\": \"x2-asset-pack\", \"version\": 0}", "unsupported \"version\""));
    CHECK(fatal("{\"format\": \"x2-asset-pack\", \"version\": 1.5}", "unsupported \"version\""));
    CHECK(fatal("{\"format\": \"x2-asset-pack\", \"version\": 3}", "needs a newer x2recomp"));
    CHECK(fatal("{\"format\": \"x2-asset-pack\", \"version\": 1, \"scale\": 9}", "\"scale\" must be 1-8"));
    CHECK(fatal("{\"format\": \"x2-asset-pack\", \"version\": 1, \"games\": [\"amiga\"]}", "\"games\""));
    CHECK(fatal("{\"format\": \"x2-asset-pack\", \"version\": 1, \"games\": []}", "\"games\""));
    CHECK(fatal("{\"format\": \"x2-asset-pack\", \"version\": 1, \"builds\": [\"A_B\"]}", "\"builds\""));
    CHECK(fatal("{\"format\": \"x2-asset-pack\", \"version\": 1, \"builds\": [\"a\", \"a\"]}", "twice"));

    // Version 1 manifests may not use the v2 sections (and `requires`).
    Parsed v1 = parse("{\"format\": \"x2-asset-pack\", \"version\": 1, \"screens\": {}, \"audio\": {}, \"panel\": {}, \"backgrounds\": [], \"atlases\": [], \"groups\": {}, \"stars\": {}, "
                      "\"requires\": {\"x2recomp\": \">=1.0\"}, \"graphics\": {}}");
    CHECK(v1.ok && v1.rep.count(Severity::Error) == 8 && said(v1.rep, "screens: needs \"version\": 2") && v1.m.requires_x2recomp.empty());

    // Entry errors skip the entry, the rest stays.
    const std::string good = std::string("\"") + kH1 + "-16x8\"";
    Parsed e = parse("{\"format\": \"x2-asset-pack\", \"version\": 2, \"scale\": 2, \"graphics\": {" + good + ": {\"png\": \"a.png\"}, \"bad\": {\"png\": \"a.png\"}, " +
                     "\"" + kH2 + "-16x8\": {\"png\": 5}, \"" + kH3 + "-16x8\": {\"png\": \"a.png\", \"scale\": 9}, \"3123456789abcdef-16x8\": {\"png\": \"a.png\", \"anchor\": [1]}, " +
                     "\"4123456789abcdef-16x8\": {\"png\": \"a.png\", \"keys\": [\"zz\"]}, \"5123456789abcdef-16x8\": {\"png\": \"a.png\", \"games\": [\"x\"]}, \"6123456789abcdef-16x8\": 3, " +
                     "\"7123456789abcdef-16x8\": {}, \"8123456789abcdef-16x8\": {\"png\": \"\"}, \"f:9123456789abcdef-4x4\": {\"png\": \"f.png\"}}}");
    CHECK(e.ok && e.m.graphics.size() == 2 && e.m.graphics[0].scale == 2 && e.m.graphics[1].keys[0] == "f:9123456789abcdef-4x4" && e.m.skipped == 9);
    CHECK(said(e.rep, "graphics[\"bad\"]: the key must be") && said(e.rep, "\"png\" must be a string") && said(e.rep, "\"scale\" must be 1-8") && said(e.rep, "\"anchor\" must be [x, y]") &&
          said(e.rep, "is not a graphic key") && said(e.rep, "\"games\" must be a non-empty list") && said(e.rep, "must be an object") && said(e.rep, "\"png\" missing") && said(e.rep, "\"png\" is empty"));

    // Far strips, palettes and the layer names of v1.
    Parsed f = parse("{\"format\": \"x2-asset-pack\", \"version\": 1, \"far\": [{\"png\": \"f.png\", \"lines\": [\"" + std::string(kH1) + "\", \"-\", \"\", \"zz\"]}, 5, {\"png\": \"g.png\"}, {\"png\": \"h.png\", \"lines\": [], \"scale\": 0}], "
                     "\"palettes\": [{\"match\": [\"#000\", \"#111\", \"#222\", \"#333\", \"#444\", \"#555\", \"#666\", \"#777\", \"#888\", \"#999\", \"#aaa\", \"#bbb\", \"#ccc\", \"#ddd\", \"#eee\", \"#fff\"], "
                     "\"colors\": [\"#000000\", \"#010101\", \"#020202\", \"#030303\", \"#040404\", \"#050505\", \"#060606\", \"#070707\", \"#080808\", \"#090909\", \"#0a0a0a\", \"#0b0b0b\", \"#0c0c0c\", \"#0d0d0d\", \"#0e0e0e\", \"#102030\"], "
                     "\"layers\": [\"far\", \"objects\"]}, {\"match\": [], \"colors\": []}, {\"match\": [\"#000\"], \"colors\": [\"#000000\"], \"layers\": [\"sky\"]}]}");
    CHECK(f.ok && f.m.far.size() == 1 && f.m.far[0].lines.size() == 4 && f.m.far[0].used[0] && !f.m.far[0].used[1] && !f.m.far[0].used[2] && !f.m.far[0].used[3]);
    CHECK(f.m.far[0].lines[0] == 0x0123456789abcdefull && said(f.rep, "far[0]: line 3: not a 16-digit hex key") && said(f.rep, "far[1]: needs \"png\" and \"lines\"") && said(f.rep, "far[3]: \"scale\" must be 1-8"));
    CHECK(f.m.palettes.size() == 1 && f.m.palettes[0].po.layers == ((1 << 1) | (1 << 4)) && f.m.palettes[0].po.match[15] == 0x0FFF && f.m.palettes[0].po.rgba[15] == 0xFF302010u);
    CHECK(said(f.rep, "palettes[1]: needs \"match\"") && said(f.rep, "palettes[2]: needs \"match\"") == true);

    // v2: backgrounds, atlases, stars, screens, panel, audio errors.
    Parsed v2 = parse(std::string("{\"format\": \"x2-asset-pack\", \"version\": 2, "
                                  "\"backgrounds\": [{\"level\": 6, \"png\": \"a.png\"}, {\"level\": 2}, {\"level\": 2, \"png\": \"b.png\", \"ring_lines\": 5000}, {\"level\": 3, \"png\": \"b.png\", \"scale\": 2}], "
                                  "\"atlases\": [{\"png\": \"s.png\"}, {\"png\": \"s.png\", \"frames\": [{\"key\": \"nope\", \"rect\": [0, 0, 1, 1]}, {\"key\": \"") +
                     kH1 + "-4x4\", \"rect\": [0, 0, 0, 4]}, {\"key\": \"" + kH2 + "-4x4\", \"rect\": [1, 2, 8, 8], \"anchor\": [3, 4], \"flash_rect\": [0, 0, 8, 8]}]}, "
                     "{\"png\": \"s.png\", \"grid\": {\"cell\": [8, 8], \"keys\": [\"" + kH3 + "-4x4\", null, \"bad\"]}}, {\"png\": \"s.png\", \"grid\": {\"cell\": [8], \"keys\": []}}], "
                     "\"stars\": {\"shape\": \"square\"}}");
    CHECK(v2.ok && v2.m.backgrounds.size() == 1 && v2.m.backgrounds[0].level == 3 && v2.m.atlases.size() == 2);
    CHECK(said(v2.rep, "\"level\" must be 1-5") && said(v2.rep, "\"png\" missing") && said(v2.rep, "\"ring_lines\" must be 1-4096") && said(v2.rep, "needs \"frames\" or \"grid\"") &&
          said(v2.rep, "\"key\" must be a graphic key") && said(v2.rep, "\"rect\" must be [x, y, w, h]") && said(v2.rep, "grid: key 2 must be a graphic key or null") &&
          said(v2.rep, "grid: \"cell\" must be [width, height]") && said(v2.rep, "\"shape\" must be pixel or round"));
    CHECK(v2.m.atlases[0].frames.size() == 1 && v2.m.atlases[0].frames[0].has_flash && v2.m.atlases[0].frames[0].anchor.x == 3 && v2.m.atlases[1].grid_keys.size() == 3 && v2.m.atlases[1].grid_keys[1].empty());

    Parsed sc = parse("{\"format\": \"x2-asset-pack\", \"version\": 2, \"screens\": {\"Bad Name\": {}, \"nosig\": {\"png\": \"a.png\"}, "
                      "\"minbig\": {\"png\": \"a.png\", \"signature\": {\"min\": 3, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"" + std::string(kH1) + "\"}]}}, "
                      "\"ok\": {\"png\": \"a.png\", \"fade\": \"none\", \"bg_index\": 3, \"wide_png\": \"w.png\", \"wide_scale\": 2, \"dynamic\": [{\"x\": 1, \"y\": 2, \"w\": 3, \"h\": 4}], "
                      "\"signature\": {\"block\": 8, \"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"" + std::string(kH1) + "\"}]}}, "
                      "\"badblock\": {\"png\": \"a.png\", \"signature\": {\"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"xyz\"}]}}, "
                      "\"badblk\": {\"png\": \"a.png\", \"signature\": {\"block\": 12, \"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"" + std::string(kH1) + "\"}]}}}}");
    CHECK(sc.ok && sc.m.screens.size() == 2 && sc.m.screens[0].key == "minbig" && sc.m.screens[1].key == "ok" && sc.m.screens[1].fade == FadeMode::None && sc.m.screens[1].signature.block == 8 &&
          sc.m.screens[1].wide_scale == 2 && sc.m.screens[1].bg_index == 3);
    CHECK(said(sc.rep, "the name must be lowercase") && said(sc.rep, "\"signature\" needs \"blocks\"") && said(sc.rep, "\"min\" is larger than the number of blocks", 1) &&
          said(sc.rep, "signature block 0 needs") && said(sc.rep, "signature \"block\" must be 8, 16 or 32"));

    Parsed au = parse(std::string("{\"format\": \"x2-asset-pack\", \"version\": 2, \"audio\": {\"sfx\": {\"zz\": {\"file\": \"a.wav\"}, \"0123456789abcdef-100\": {\"file\": \"a.wav\", \"ref_period\": 50}, "
                                  "\"1123456789abcdef-100\": {\"file\": \"a.wav\", \"gain_db\": 99}, \"2123456789abcdef-100\": {\"file\": \"a.wav\", \"pan\": 3}, \"3123456789abcdef-100\": {\"file\": \"a.wav\", \"original\": \"mix:abc\"}, "
                                  "\"4123456789abcdef-100\": {\"file\": \"a.wav\", \"original\": \"mix:-6.5\", \"pan\": -0.5, \"pitch\": \"fixed\", \"cut\": \"never\", \"loop\": true, \"fade_out_ms\": 9}, "
                                  "\"5123456789abcdef-100\": {\"file\": \"a.wav\", \"cd0_file\": \"lower\"}, \"6123456789abcdef-100\": {\"file\": \"a.wav\", \"cd0_file\": \"SPEECH1\"}, \"7123456789abcdef-100\": {}}, "
                                  "\"music\": {\"slots\": {\"level9\": {\"file\": \"a.ogg\"}, \"cd14\": {\"file\": \"a.ogg\"}, \"cd13\": {\"file\": \"a.ogg\"}, \"title\": {}, \"level1\": {\"file\": null}, "
                                  "\"level2\": {\"file\": \"a.ogg\", \"loop\": {\"start_s\": 5, \"end_s\": 4}}, \"level3\": {\"file\": \"a.ogg\", \"loop\": \"x\"}, \"level4\": {\"mute\": true}, "
                                  "\"cd2\": {\"file\": \"a.ogg\", \"crossfade_ms\": 6000}, \"cd3\": {\"file\": \"a.ogg\", \"retrigger\": \"maybe\"}}, \"defaults\": {\"crossfade_ms\": 100, \"gain_db\": -2, \"loop_mode\": \"game\"}}}}"));
    CHECK(au.ok && au.m.clips.size() == 2 && au.m.clips[0].key == "4123456789abcdef-100" && au.m.clips[0].original_mix_db == -6.5f && au.m.clips[0].pan == PanMode::Position && au.m.clips[0].pan_pos == -0.5f);
    CHECK(au.m.clips[0].pitch == PitchMode::Fixed && au.m.clips[0].cut == CutMode::Never && au.m.clips[0].loop && au.m.clips[0].fade_out_ms == 9 && au.m.clips[1].cd0_file == "SPEECH1");
    CHECK(said(au.rep, "the key must be \"<16 hex digits>-<sample bytes>\"") && said(au.rep, "\"ref_period\" must be 124-65535") && said(au.rep, "\"gain_db\" must be -60 to 24") &&
          said(au.rep, "\"pan\" must be") && said(au.rep, "\"original\" must be") && said(au.rep, "\"cd0_file\" must be an upper-case") && said(au.rep, "\"file\" missing"));
    CHECK(said(au.rep, "audio.music.slots[\"level9\"]: the slot must be") && said(au.rep, "[\"cd14\"]: the slot must be") && said(au.rep, "[\"title\"]: needs a \"file\"") &&
          said(au.rep, "\"file\": null needs \"mute\": true") && said(au.rep, "\"loop\" {start_s, end_s}") && said(au.rep, "\"loop\" must be true, false or") && said(au.rep, "\"crossfade_ms\" must be 0-5000") &&
          said(au.rep, "\"retrigger\" must be restart or continue"));
    CHECK(au.m.slots.size() == 2 && au.m.slots[0].key == "cd13" && au.m.slots[1].key == "level4");
    CHECK(au.m.music_defaults.crossfade_ms && *au.m.music_defaults.crossfade_ms == 100 && au.m.music_defaults.gain_db && *au.m.music_defaults.gain_db == -2.0f && au.m.music_defaults.loop_mode &&
          *au.m.music_defaults.loop_mode == LoopMode::Game);

    // Panel.
    Parsed pn = parse("{\"format\": \"x2-asset-pack\", \"version\": 2, \"scale\": 3, \"panel\": {\"cell_w\": 16, \"max_span\": 4, \"background\": {\"png\": \"b.png\", \"anchor\": [1, 2]}, \"cells\": {"
                      "\"0123456789abcdef-2\": {\"png\": \"c.png\", \"keys\": [\"1123456789abcdef-1\"]}, \"0123456789abcdef-9\": {\"png\": \"c.png\"}, \"bad\": {\"png\": \"c.png\"}, "
                      "\"2123456789abcdef-1\": {\"png\": \"c.png\", \"scale\": 5}}}}");
    CHECK(pn.ok && pn.m.panel && pn.m.panel->scale == 3 && pn.m.panel->cell_w == 16 && pn.m.panel->max_span == 4 && pn.m.panel->cells.size() == 2 && pn.m.panel->cells[0].keys.size() == 2 && pn.m.panel->cells[1].scale == 5);
    CHECK(pn.m.panel->background_anchor.x == 1 && said(pn.rep, "the key must be \"<16 hex digits>-<cells 1-8>\"") == true);
    CHECK(parse("{\"format\": \"x2-asset-pack\", \"version\": 2, \"panel\": {\"cell_w\": 12}}").rep.count(Severity::Error) == 1);

    // Metadata problems are errors but not fatal; the validator mode reports unknown and duplicate fields and a missing license.
    Parsed md = parse("{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": 5, \"id\": \"A\", \"pack_version\": \"1.0\", \"credits\": [{}, {\"name\": \"n\", \"x\": 1}], \"provides\": [\"video\"], "
                      "\"requires\": {\"x2recomp\": \"x\", \"packs\": [{\"id\": \"a\"}, {\"id\": \"bb\", \"version\": \"x\"}]}, \"preview\": \"../p.png\", \"zzz\": 1, \"license\": \"MIT\", \"license\": \"MIT\"}", true);
    CHECK(md.ok && md.rep.count(Severity::Error) >= 8 && said(md.rep, "unknown field \"zzz\"", 1) && said(md.rep, "duplicate field \"license\"") && md.m.credits.size() == 1 && md.m.id.empty());
    Parsed nl = parse("{\"format\": \"x2-asset-pack\", \"version\": 2}", true);
    CHECK(said(nl.rep, "license: missing", 1));
    Parsed quiet = parse("{\"format\": \"x2-asset-pack\", \"version\": 2, \"zzz\": 1}", false);
    CHECK(quiet.rep.messages.empty());

    // The entry cap counts every kind.
    PackReport rep;
    Json j;
    std::string why, fatal2;
    std::string many = "{\"format\": \"x2-asset-pack\", \"version\": 1, \"graphics\": {";
    for (int i = 0; i < 20; ++i) {
        char key[40];
        std::snprintf(key, sizeof key, "%016x-4x4", unsigned(i + 1));
        many += std::string(i ? "," : "") + "\"" + key + "\": {\"png\": \"a.png\"}";
    }
    many += "}}";
    CHECK(parse_json(many, j, why));
    ParseOptions po;
    po.limits.max_entries = 5;
    Manifest m;
    CHECK(parse_manifest(j, "t", po, m, rep, fatal2) && m.graphics.size() == 5 && m.entries == 5 && said(rep, "more than 5 entries: the rest are ignored"));
}


// ---- the loader ----------------------------------------------------------------------------------------------------------------

struct Loaded {
    std::unique_ptr<PackStack> s;
    PackReport rep;
    std::vector<bool> ok;
};
Loaded load(const std::vector<std::string>& paths, PackContext ctx = {}, StackOptions so = {}) {
    Loaded l;
    l.s = std::make_unique<PackStack>(ctx, so);
    for (const std::string& p : paths) l.ok.push_back(l.s->add(p, l.rep));
    return l;
}
PackContext ctx_of(GameFamily g, const std::string& build = "") {
    PackContext c;
    c.game = g;
    c.build = build;
    return c;
}
uint64_t hex_of(const char* h) { return std::strtoull(h, nullptr, 16); }

// Pixel (x, y) of the atlas: 0xFF000000 | y << 8 | x.
uint32_t atlas_px(int x, int y) { return 0xFF000000u | uint32_t(y) << 8 | uint32_t(x); }

void test_loader_graphics() {
    std::printf("loader: graphics, far, palettes, backgrounds, atlases, stars, screens, panel\n");
    const std::string a = key16(kH1, 16, 8), alias = key16(kH2, 16, 8), fk = std::string("f:") + key16(kH3, 4, 4);
    const std::string frame = key16("3123456789abcdef", 4, 4), g1 = key16("4123456789abcdef", 8, 8), g2 = key16("5123456789abcdef", 8, 8);
    std::string man = std::string("{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": \"Gfx\", \"id\": \"gfx\", \"license\": \"CC0-1.0\", \"scale\": 2,\n");
    man += "\"graphics\": {\"" + a + "\": {\"png\": \"a.png\", \"keys\": [\"" + alias + "\"], \"flash_png\": \"f.png\", \"anchor\": [3, -2]},\n";
    man += " \"" + fk + "\": {\"png\": \"f.png\"}, \"" + key16("6123456789abcdef", 4, 4) + "\": {\"png\": \"missing.png\"}},\n";
    man += "\"far\": [{\"png\": \"far.png\", \"lines\": [\"aaaaaaaaaaaaaaaa\", \"-\", \"bbbbbbbbbbbbbbbb\"]}],\n";
    man += "\"palettes\": [{\"match\": [\"#000\", \"#111\", \"#222\", \"#333\", \"#444\", \"#555\", \"#666\", \"#777\", \"#888\", \"#999\", \"#aaa\", \"#bbb\", \"#ccc\", \"#ddd\", \"#eee\", \"#fff\"], "
           "\"colors\": [\"#000000\", \"#010101\", \"#020202\", \"#030303\", \"#040404\", \"#050505\", \"#060606\", \"#070707\", \"#080808\", \"#090909\", \"#0a0a0a\", \"#0b0b0b\", \"#0c0c0c\", \"#0d0d0d\", \"#0e0e0e\", \"#0f0f0f\"]}],\n";
    man += "\"backgrounds\": [{\"level\": 1, \"png\": \"bg.png\", \"scale\": 1, \"ring_lines\": 4}, {\"level\": 2, \"png\": \"bg_bad.png\", \"scale\": 1}, {\"level\": 3, \"png\": \"bg.png\", \"scale\": 1, \"ring_lines\": 9}],\n";
    man += "\"atlases\": [{\"png\": \"atlas.png\", \"frames\": [{\"key\": \"" + frame + "\", \"rect\": [8, 4, 8, 8], \"anchor\": [1, 1], \"flash_rect\": [16, 8, 8, 8]}, {\"key\": \"" + g1 +
           "\", \"rect\": [60, 0, 8, 8]}], \"grid\": {\"cell\": [16, 16], \"keys\": [\"" + g1 + "\", null, \"" + g2 + "\"]}}],\n";
    man += "\"stars\": {\"shape\": \"round\", \"size\": 1.5, \"softness\": 0.25},\n";
    man += "\"screens\": {\"title\": {\"png\": \"screen.png\", \"wide_png\": \"wide.png\", \"ref_palette\": [\"#000\", \"#fff\"], \"signature\": {\"min\": 1, \"blocks\": [{\"x\": 16, \"y\": 32, \"hash\": \"" +
           std::string(kH1) + "\"}, {\"x\": 0, \"y\": 0, \"hash\": \"" + kH2 + "\"}]}, \"dynamic\": [{\"x\": 1, \"y\": 1, \"w\": 5, \"h\": 5}]}},\n";
    man += "\"panel\": {\"scale\": 2, \"background\": {\"png\": \"pbg.png\", \"anchor\": [0, 1]}, \"cells\": {\"" + std::string(kH1) + "-1\": {\"png\": \"cell.png\", \"keys\": [\"" + kH2 + "-2\"], \"anchor\": [0, -2]}}}}";
    const std::string dir = make_pack("gfx1", man, {{"a.png", {32, 16}}, {"f.png", {32, 16}}, {"far.png", {640, 6}}, {"bg.png", {320, 4}}, {"bg_bad.png", {100, 4}}, {"screen.png", {640, 400}},
                                                    {"wide.png", {800, 400}}, {"pbg.png", {640, 16}}, {"cell.png", {16, 16}}});
    put_png(dir + "/atlas.png", 64, 32, [](int x, int y) { return atlas_px(x, y); });
    Loaded l = load({dir}, ctx_of(GameFamily::Disk, "canonical"));
    const PackStack& s = *l.s;
    if (!l.ok[0]) std::fprintf(stderr, "%s", dump_report(l.rep).c_str());
    CHECK(l.ok[0] && s.packs().size() == 1 && s.packs()[0].usable && s.packs()[0].id == "gfx" && s.packs()[0].name == "Gfx" && s.packs()[0].provides_graphics && s.packs()[0].provides_screens && !s.packs()[0].provides_audio);
    CHECK(s.has_graphics() && !s.has_audio() && !s.empty());
    // sprites and tiles, the alias, the flash picture and the explicit flash key
    const GraphicEntry* e = s.graphic(hex_of(kH1), 16, 8);
    CHECK(e && e->pic && e->pic->w == 32 && e->pic->h == 16 && e->scale == 2 && e->anchor.x == 3 && e->anchor.y == -2 && e->flash && e->flash->w == 32 && e->flash_scale == 2);
    const GraphicEntry* ea = s.graphic(hex_of(kH2), 16, 8);
    CHECK(ea && ea->pic == e->pic && ea->scale == 2);
    CHECK(!s.graphic(hex_of(kH1), 16, 9) && !s.graphic(hex_of(kH1), 8, 8) && !s.graphic(hex_of(kH1) + 1, 16, 8));
    const GraphicEntry* fe = s.flash(hex_of(kH3), 4, 4);
    CHECK(fe && fe->pic && fe->pic->w == 32 && !s.flash(hex_of(kH1), 16, 8) && !s.graphic(hex_of(kH3), 4, 4));
    CHECK(said(l.rep, "graphics[\"" + key16("6123456789abcdef", 4, 4) + "\"]: missing file 'missing.png'", 2));
    // far lines: used rows only
    const FarLineRef* fa = s.far_line(hex_of("aaaaaaaaaaaaaaaa"));
    const FarLineRef* fb = s.far_line(hex_of("bbbbbbbbbbbbbbbb"));
    CHECK(fa && fb && fa->strip == fb->strip && fa->row == 0 && fb->row == 2 && fa->scale == 2 && fa->strip->w == 640);
    CHECK(!s.far_line(hex_of("cccccccccccccccc")) && s.far_count() == 2);
    // palette override: matched on the 12-bit colours
    uint16_t cols[16];
    for (int i = 0; i < 16; ++i) cols[i] = uint16_t(i * 0x111);
    const PaletteOverride* po = s.palette(cols);
    CHECK(po && po->rgba[15] == 0xFF0F0F0Fu && po->layers == 0x1F);
    cols[3] ^= 1;
    CHECK(!s.palette(cols));
    cols[3] ^= 1;
    for (int i = 0; i < 16; ++i) cols[i] |= 0xF000;  // the top nibble is ignored
    CHECK(s.palette(cols) != nullptr);
    // backgrounds: sizes are checked
    const BackgroundEntry* bg = s.background(1);
    CHECK(bg && bg->level == 1 && bg->pic->w == 320 && bg->ring_lines == 4 && !s.background(2) && !s.background(3) && !s.background(0) && !s.background(6));
    CHECK(said(l.rep, "backgrounds[1][\"level2\"]: the picture must be 320 pixels wide") && said(l.rep, "backgrounds[2][\"level3\"]: the picture must be 9 pixels tall"));
    // atlases: frames cropped from the sheet
    const GraphicEntry* fr = s.graphic(hex_of("3123456789abcdef"), 4, 4);
    CHECK(fr && fr->pic->w == 8 && fr->pic->h == 8 && fr->pic->px[0] == atlas_px(8, 4) && fr->pic->px[7 * 8 + 7] == atlas_px(15, 11) && fr->anchor.x == 1 && fr->scale == 2);
    CHECK(fr && fr->flash && fr->flash->px[0] == atlas_px(16, 8));
    // the frame with a rect outside the atlas is refused, then the grid supplies g1 (cell 0) and g2 (cell 2 = x 32)
    const GraphicEntry* gg1 = s.graphic(hex_of("4123456789abcdef"), 8, 8);
    const GraphicEntry* gg2 = s.graphic(hex_of("5123456789abcdef"), 8, 8);
    CHECK(said(l.rep, "rect [60, 0, 8, 8] lies outside the 64x32 atlas"));
    CHECK(gg1 && gg1->pic->w == 16 && gg1->pic->px[0] == atlas_px(0, 0) && gg2 && gg2->pic->px[0] == atlas_px(32, 0) && gg2->pic->px[15 * 16 + 15] == atlas_px(47, 15));
    // stars
    const StarsEntry* st = s.stars();
    CHECK(st && st->shape == StarsEntry::Shape::Round && st->size == 2 && st->size_px == 1.5f && st->softness == 0.25f);
    // screens: indexed by the first block's hash
    CHECK(s.screens().size() == 1 && s.screens_for_block(hex_of(kH1)).size() == 1 && s.screens_for_block(hex_of(kH2)).empty() && s.screens_for_block(7).empty());
    const ScreenEntry* sc = s.screens()[0];
    CHECK(sc->name == "title" && sc->pic->w == 640 && sc->scale == 2 && sc->wide_pic && sc->wide_pic->w == 800 && sc->wide_scale == 2 && sc->ref_palette.size() == 2 && sc->ref_palette[1] == 0xFFFFFF &&
          sc->signature.blocks.size() == 2 && sc->signature.min == 1 && sc->signature.blocks[0].x == 16 && sc->dynamic.size() == 1 && sc->fade == FadeMode::Scale);
    // panel
    const PanelSet* pn = s.panel();
    CHECK(pn && pn->scale == 2 && pn->background && pn->background->w == 640 && pn->background_anchor.y == 1 && pn->cells.size() == 2 && pn->find(panel_key(hex_of(kH1), 1)) &&
          pn->find(std::string(kH2) + "-2")->pic == pn->find(std::string(kH1) + "-1")->pic && pn->find(std::string(kH1) + "-1")->anchor.y == -2 && !pn->find(std::string(kH1) + "-3") && !pn->empty());
    CHECK(s.picture_bytes() > 0 && said(l.rep, "loaded: 5 graphics, 2 far lines, 1 palettes, 1 backgrounds, 1 screens, 1 panel cells", 0));
    // the null library for comparison
    const PackLibrary& n = null_library();
    CHECK(n.empty() && !n.graphic(hex_of(kH1), 16, 8) && !n.panel() && !n.stars() && n.screens().empty());
}

void test_scope() {
    std::printf("loader: games and builds scoping\n");
    const std::string k1 = key16(kH1, 4, 4), k2 = key16(kH2, 4, 4), k3 = key16(kH3, 4, 4), k4 = key16("3123456789abcdef", 4, 4);
    auto graphics = [&](const std::string& pack_scope, const std::string& e1, const std::string& e2) {
        return "{\"format\": \"x2-asset-pack\", \"version\": 2, " + pack_scope + "\"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\"" + e1 + "}, \"" + k2 + "\": {\"png\": \"a.png\"" + e2 + "}, \"" + k3 +
               "\": {\"png\": \"a.png\"}}}";
    };
    const std::map<std::string, std::pair<int, int>> pics = {{"a.png", {4, 4}}};
    // pack for the CDTV only: nothing loads on the disk build and the pack says so, it is not an error
    std::string dir = make_pack("scope1", graphics("\"games\": [\"cdtv\"], ", "", ""), pics);
    Loaded l = load({dir}, ctx_of(GameFamily::Disk, "canonical"));
    CHECK(l.ok[0] && l.s->graphic_count() == 0 && l.s->packs()[0].usable && said(l.rep, "none of its entries apply to this game (disk, build canonical)", 0) && l.rep.count(Severity::Error) == 0);
    l = load({dir}, ctx_of(GameFamily::Cdtv, "cdtv"));
    CHECK(l.s->graphic_count() == 3);
    // entry scope inside a pack for disk and CDTV: the first entry is CDTV only, the second names two builds
    dir = make_pack("scope2", graphics("\"games\": [\"disk\", \"cdtv\"], ", ", \"games\": [\"cdtv\"]", ", \"builds\": [\"power-pack-2234\", \"canonical\"]"), pics);
    l = load({dir}, ctx_of(GameFamily::Disk, "canonical"));
    CHECK(!l.s->graphic(hex_of(kH1), 4, 4) && l.s->graphic(hex_of(kH2), 4, 4) && l.s->graphic(hex_of(kH3), 4, 4));
    l = load({dir}, ctx_of(GameFamily::Disk, "power-pack-2234"));
    CHECK(!l.s->graphic(hex_of(kH1), 4, 4) && l.s->graphic(hex_of(kH2), 4, 4));
    l = load({dir}, ctx_of(GameFamily::Disk, "compilation-1762"));
    CHECK(!l.s->graphic(hex_of(kH1), 4, 4) && !l.s->graphic(hex_of(kH2), 4, 4) && l.s->graphic(hex_of(kH3), 4, 4));
    l = load({dir}, ctx_of(GameFamily::Cdtv, "cdtv"));
    CHECK(l.s->graphic(hex_of(kH1), 4, 4) && !l.s->graphic(hex_of(kH2), 4, 4));  // the second entry's builds do not include cdtv
    l = load({dir}, ctx_of(GameFamily::St, "atari-st-kixx"));
    CHECK(l.s->graphic_count() == 0 && said(l.rep, "none of its entries apply", 0));
    // an entry cannot widen its pack: pack disk only, entry cdtv: nowhere
    dir = make_pack("scope3", graphics("\"games\": [\"disk\"], ", ", \"games\": [\"cdtv\", \"disk\"]", ", \"games\": [\"cdtv\"]"), pics);
    l = load({dir}, ctx_of(GameFamily::Cdtv, "cdtv"));
    CHECK(l.s->graphic_count() == 0);
    l = load({dir}, ctx_of(GameFamily::Disk, "canonical"));
    CHECK(l.s->graphic(hex_of(kH1), 4, 4) && !l.s->graphic(hex_of(kH2), 4, 4) && l.s->graphic(hex_of(kH3), 4, 4));
    // pack builds and entry builds intersect
    dir = make_pack("scope4", graphics("\"builds\": [\"power-pack-2234\", \"pdos-v5\"], ", ", \"builds\": [\"pdos-v5\"]", ", \"builds\": [\"cdtv\"]"), pics);
    l = load({dir}, ctx_of(GameFamily::Disk, "pdos-v5"));
    CHECK(l.s->graphic(hex_of(kH1), 4, 4) && !l.s->graphic(hex_of(kH2), 4, 4) && l.s->graphic(hex_of(kH3), 4, 4));
    l = load({dir}, ctx_of(GameFamily::Disk, "power-pack-2234"));
    CHECK(!l.s->graphic(hex_of(kH1), 4, 4) && l.s->graphic(hex_of(kH3), 4, 4));
    // in validate mode no scoping applies: everything loads
    StackOptions vo;
    vo.validate = true;
    l = load({dir}, ctx_of(GameFamily::St, "atari-st-kixx"), vo);
    CHECK(l.s->graphic_count() == 3);
    // GameScope itself
    GameScope a, b;
    a.games = 0x3;
    b.games = 0x6;
    CHECK(GameScope::intersect(a, b).games == 0x2 && a.applies(ctx_of(GameFamily::Cdtv)) && !a.applies(ctx_of(GameFamily::St)));
    a.builds = {"x"};
    b.builds = {"y"};
    CHECK(GameScope::intersect(a, b).games == 0);
    (void)k4;
}

void test_layering() {
    std::printf("loader: layering, overrides, requires, ids, limits\n");
    const std::string k1 = key16(kH1, 4, 4), k2 = key16(kH2, 4, 4);
    auto pack = [&](const std::string& name, const std::string& extra, const std::string& id, const std::string& version = "", const std::string& req = "") {
        std::string m = "{\"format\": \"x2-asset-pack\", \"version\": 2, \"id\": \"" + id + "\", " + (version.empty() ? "" : "\"pack_version\": \"" + version + "\", ") + req + extra + "}";
        return make_pack(name, m, {{"a.png", {4, 4}}, {"b.png", {8, 8}}, {"far.png", {320, 2}}});
    };
    const std::string pa = pack("lay_a", "\"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\"}, \"" + k2 + "\": {\"png\": \"a.png\"}}, \"far\": [{\"png\": \"far.png\", \"lines\": [\"aaaaaaaaaaaaaaaa\", \"bbbbbbbbbbbbbbbb\"]}], "
                                      "\"stars\": {\"size\": 1}, \"screens\": {\"s1\": {\"png\": \"a.png\", \"signature\": {\"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"" + std::string(kH1) + "\"}]}}}, "
                                      "\"backgrounds\": [{\"level\": 1, \"png\": \"far.png\", \"scale\": 1}]",
                                "pack.a", "1.2.0");
    const std::string pb = pack("lay_b", "\"graphics\": {\"" + k1 + "\": {\"png\": \"b.png\", \"scale\": 2}}, \"far\": [{\"png\": \"far.png\", \"lines\": [\"-\", \"bbbbbbbbbbbbbbbb\"]}], "
                                      "\"stars\": {\"size\": 3}, \"screens\": {\"s1\": {\"png\": \"b.png\", \"signature\": {\"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"" + std::string(kH2) + "\"}]}}}, "
                                      "\"backgrounds\": [{\"level\": 1, \"png\": \"far.png\", \"scale\": 1, \"ring_lines\": 2}]",
                                "pack.b", "2.0.0", "\"requires\": {\"packs\": [{\"id\": \"pack.a\", \"version\": \">=1.0\"}]}, ");
    Loaded l = load({pa, pb});
    CHECK(l.ok[0] && l.ok[1] && l.s->packs().size() == 2);
    const GraphicEntry* g1 = l.s->graphic(hex_of(kH1), 4, 4);
    CHECK(g1 && g1->pic->w == 8 && g1->scale == 2);                    // pack b wins the key both define
    CHECK(l.s->graphic(hex_of(kH2), 4, 4)->pic->w == 4);                // pack a keeps the one only it defines
    CHECK(said(l.rep, "graphics[\"" + k1 + "\"]: overrides the entry of 'lay_a'", 0) && !said(l.rep, "graphics[\"" + k2 + "\"]: overrides"));
    CHECK(l.s->far_line(hex_of("aaaaaaaaaaaaaaaa")) != nullptr && l.s->far_line(hex_of("bbbbbbbbbbbbbbbb"))->row == 1 && l.s->far_count() == 2);
    CHECK(l.s->stars() && l.s->stars()->size == 3);
    CHECK(l.s->screens().size() == 1 && l.s->screens()[0]->pic->w == 8 && l.s->screens_for_block(hex_of(kH2)).size() == 1 && l.s->screens_for_block(hex_of(kH1)).empty());
    CHECK(l.s->background(1)->ring_lines == 2);
    // the order is the user's: b before a refuses b
    l = load({pb, pa});
    CHECK(!l.ok[0] && l.ok[1] && l.s->graphic_count() == 2 && l.s->graphic(hex_of(kH1), 4, 4)->pic->w == 4);
    CHECK(said(l.rep, "requires the pack 'pack.a' >=1.0, which must be loaded before it (list it earlier)", 2) && !l.s->packs()[0].usable && l.s->packs()[0].problem.find("pack.a") != std::string::npos);
    // version constraints
    const std::string pc = pack("lay_c", "\"graphics\": {}", "pack.c", "", "\"requires\": {\"packs\": [{\"id\": \"pack.a\", \"version\": \">=1.3\"}]}, ");
    l = load({pa, pc});
    CHECK(l.ok[0] && !l.ok[1] && said(l.rep, "requires the pack 'pack.a' >=1.3, but version 1.2.0 is loaded"));
    const std::string pd = pack("lay_d", "\"graphics\": {}", "pack.d", "", "\"requires\": {\"packs\": [{\"id\": \"pack.zzz\"}]}, ");
    l = load({pa, pd});
    CHECK(!l.ok[1] && said(l.rep, "requires the pack 'pack.zzz', which must be loaded before it"));
    // requires.x2recomp against the runtime's version
    const std::string pe = pack("lay_e", "\"graphics\": {}", "pack.e", "", "\"requires\": {\"x2recomp\": \">=0.5\"}, ");
    StackOptions so;
    so.runtime_version = "0.3.0";
    l = load({pe}, {}, so);
    CHECK(!l.ok[0] && said(l.rep, "needs x2recomp >=0.5 (this is 0.3.0)"));
    so.runtime_version = "0.5.1";
    l = load({pe}, {}, so);
    CHECK(l.ok[0]);
    so.runtime_version = "0.6";  // a version without a patch number
    l = load({pe}, {}, so);
    CHECK(l.ok[0]);
    // two packs with one id: a warning, both load
    l = load({pa, pa});
    CHECK(l.ok[0] && l.ok[1] && said(l.rep, "has the same id 'pack.a'", 1));
    // refused packs are reported, the others still load
    l = load({g_tmp + "/no-such-pack", pa});
    CHECK(!l.ok[0] && l.ok[1] && l.s->packs().size() == 2 && !l.s->packs()[0].usable && l.s->packs()[0].problem == "no such asset pack (a directory or a .zip file)" && l.s->graphic_count() == 2);
    // more than 20 overridden keys: the first 20 are listed and the rest counted
    std::string many_a = "\"graphics\": {", many_b = many_a;
    for (int i = 0; i < 25; ++i) {
        char key[40];
        std::snprintf(key, sizeof key, "%016x-4x4", unsigned(i + 1));
        many_a += std::string(i ? ", " : "") + "\"" + key + "\": {\"png\": \"a.png\"}";
        many_b += std::string(i ? ", " : "") + "\"" + key + "\": {\"png\": \"b.png\", \"scale\": 2}";
    }
    l = load({pack("lay_m1", many_a + "}", "m.one"), pack("lay_m2", many_b + "}", "m.two")});
    CHECK(l.rep.count(Severity::Info) == 1 + 20 + 1 + 1 && said(l.rep, "and 5 more entries override those of earlier packs"));
    // the memory limit for all pictures, the picture limits
    StackOptions mo;
    mo.limits.max_total = 4 * 4 * 4 + 10;  // room for one 4x4 picture only
    l = load({pack("lay_mem", "\"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\"}, \"" + k2 + "\": {\"png\": \"b.png\"}}", "mem.a")}, {}, mo);
    CHECK(l.s->graphic_count() == 1 && said(l.rep, "'b.png': over the memory limit for all packs (0 MB)"));
    StackOptions po;
    po.limits.max_side = 5;
    l = load({pack("lay_side", "\"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\"}, \"" + k2 + "\": {\"png\": \"b.png\"}}", "side.a")}, {}, po);
    CHECK(l.s->graphic_count() == 1 && said(l.rep, "too large or empty (8x8, limit 5 per side"));
    // backgrounds may be taller than other pictures
    const std::string tall = make_pack("lay_tall", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"backgrounds\": [{\"level\": 2, \"png\": \"bg.png\", \"scale\": 1}], \"graphics\": {\"" + k1 + "\": {\"png\": \"bg.png\"}}}",
                                       {{"bg.png", {320, 5000}}});
    l = load({tall});
    CHECK(l.s->background(2) && l.s->background(2)->pic->h == 5000 && !l.s->graphic(hex_of(kH1), 4, 4) && said(l.rep, "too large or empty (320x5000"));
    // entry cap in the loader
    StackOptions co;
    co.limits.max_entries = 1;
    l = load({pack("lay_cap", "\"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\"}, \"" + k2 + "\": {\"png\": \"a.png\"}}", "cap.a")}, {}, co);
    CHECK(l.s->graphic_count() == 1 && said(l.rep, "more than 1 entries: the rest are ignored"));
    // a pack that is a ZIP behaves like the folder
    put_file(g_tmp + "/lay_a.zip", make_zip({{"pa/manifest.json", slurp(pa + "/manifest.json"), 0}, {"pa/a.png", slurp(pa + "/a.png"), 0}, {"pa/far.png", slurp(pa + "/far.png"), 8}}));
    l = load({g_tmp + "/lay_a.zip"});
    CHECK(l.ok[0] && l.s->graphic_count() == 2 && l.s->far_count() == 2 && l.s->packs()[0].id == "pack.a");
    // names without a name or id: the id is the folder's slug
    const std::string anon = make_pack("Anonymous Pack", "{\"format\": \"x2-asset-pack\", \"version\": 1}");
    l = load({anon});
    CHECK(l.ok[0] && l.s->packs()[0].id == "anonymous-pack" && l.s->packs()[0].name == "Anonymous Pack");
}

// ---- audio ---------------------------------------------------------------------------------------------------------------------

void test_audio() {
    std::printf("loader: sound effects, speech, music slots, the codec hook\n");
    set_audio_codec(&kWavCodec);
    const std::string key640 = "0123456789abcdef-640", key9000 = "1123456789abcdef-9000", alias = "2123456789abcdef-641";
    std::string man = "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"audio\": {\n";
    man += "\"sfx\": {\"" + key640 + "\": {\"file\": \"sfx/fire.wav\", \"ref_period\": 214, \"gain_db\": -3, \"keys\": [\"" + alias + "\"]}, \"3123456789abcdef-100\": {\"file\": \"sfx/long.wav\"}, "
           "\"4123456789abcdef-100\": {\"file\": \"sfx/slow.wav\"}, \"5123456789abcdef-100\": {\"file\": \"sfx/none.wav\"}, \"6123456789abcdef-100\": {\"file\": \"sfx/junk.wav\"}},\n";
    man += "\"speech\": {\"" + key9000 + "\": {\"file\": \"sfx/fire.wav\", \"cd0_file\": \"SPEECH63\", \"original\": \"mix:-12\"}},\n";
    man += "\"music\": {\"defaults\": {\"crossfade_ms\": 250, \"gain_db\": -4, \"loop_mode\": \"game\"}, \"slots\": {\"title\": {\"file\": \"music/t.wav\", \"loop\": {\"start_s\": 0.25}}, "
           "\"level1\": {\"intro\": \"music/i.wav\", \"file\": \"music/t.wav\", \"loop\": true, \"gain_db\": 2, \"crossfade_ms\": 10}, \"level2\": {\"file\": null, \"mute\": true}, \"level3\": {\"file\": \"music/missing.wav\"}, "
           "\"cd5\": {\"file\": \"music/junk.wav\"}, \"level4\": {\"file\": \"music/t.wav\", \"loop\": false, \"retrigger\": \"continue\"}}}}}";
    const std::string dir = make_pack("audio1", man);
    put_file(dir + "/sfx/fire.wav", make_wav(22050, 1, tone(320)));      // 640 bytes
    put_file(dir + "/sfx/long.wav", make_wav(8000, 1, tone(8000 * 121)));  // 121 s
    put_file(dir + "/sfx/slow.wav", make_wav(4000, 1, tone(100)));        // rate below 8000
    put_file(dir + "/sfx/junk.wav", "not a wav");
    put_file(dir + "/music/t.wav", make_wav(44100, 2, tone(44100, 2)));
    put_file(dir + "/music/i.wav", make_wav(44100, 2, tone(4410, 2)));
    put_file(dir + "/music/junk.wav", "junk");
    Loaded l = load({dir});
    const PackStack& s = *l.s;
    if (!l.ok[0]) std::fprintf(stderr, "%s", dump_report(l.rep).c_str());
    CHECK(l.ok[0] && s.has_audio() && !s.has_graphics() && s.packs()[0].provides_audio);
    const ClipEntry* c = s.sfx(key640);
    CHECK(c && c->clip && c->clip->pcm.size() == 320 && c->clip->rate == 22050 && c->clip->channels == 1 && c->ref_period == 214 && c->gain_db == -3.0f && c->key == key640 && c->file == "sfx/fire.wav");
    const ClipEntry* ca = s.sfx(alias);
    CHECK(ca && ca->clip == c->clip && ca->key == alias && ca->ref_period == 214);
    CHECK(s.has_clip_length(640) && s.has_clip_length(641) && s.has_clip_length(9000) && !s.has_clip_length(642) && !s.has_clip_length(0));
    CHECK(!s.sfx("3123456789abcdef-100") && !s.sfx("4123456789abcdef-100") && !s.sfx("5123456789abcdef-100") && !s.sfx("6123456789abcdef-100"));
    CHECK(said(l.rep, "audio.sfx[\"3123456789abcdef-100\"]: 'sfx/long.wav': too long") || said(l.rep, "'sfx/long.wav': longer than 120 s"));
    CHECK(said(l.rep, "'sfx/slow.wav': sample rate 4000 is outside 8000-192000") && said(l.rep, "missing file 'sfx/none.wav'") && said(l.rep, "'sfx/junk.wav': not a WAV file"));
    const ClipEntry* sp = s.speech(key9000);
    CHECK(sp && sp->original == OriginalMode::Mix && sp->original_mix_db == -12.0f && sp->cd0_file == "SPEECH63" && s.speech_for_file("SPEECH63") == sp && !s.speech_for_file("SPEECH64") && !s.sfx(key9000));
    // music
    const MusicSlot* t = s.music_slot("title");
    CHECK(t && t->file.pack == 0 && t->file.path == "music/t.wav" && t->loop.kind == LoopKind::Points && t->loop.start_s == 0.25 && t->loop.end_s < 0 && t->gain_db == -4.0f && !t->mute && !t->crossfade_ms);
    const MusicSlot* l1 = s.music_slot("level1");
    CHECK(l1 && l1->intro.path == "music/i.wav" && l1->loop.kind == LoopKind::Whole && l1->gain_db == 2.0f && l1->crossfade_ms && *l1->crossfade_ms == 10);
    const MusicSlot* l2 = s.music_slot("level2");
    CHECK(l2 && l2->mute && l2->file.path.empty() && !l2->file.valid());
    const MusicSlot* l4 = s.music_slot("level4");
    CHECK(l4 && l4->loop.kind == LoopKind::Off && l4->retrigger && *l4->retrigger == Retrigger::Continue);
    CHECK(!s.music_slot("level3") && !s.music_slot("cd5") && !s.music_slot("level5") && s.music_slot_count() == 4);
    CHECK(said(l.rep, "audio.music.slots[\"level3\"]: \"file\": missing file 'music/missing.wav'") && said(l.rep, "audio.music.slots[\"cd5\"]: \"file\": 'music/junk.wav': not a WAV file"));
    CHECK(s.music_defaults().crossfade_ms == 250 && s.music_defaults().loop_mode == LoopMode::Game && s.music_defaults().retrigger == Retrigger::Restart);
    // pack files: whole and streamed
    std::vector<uint8_t> bytes;
    std::string why;
    CHECK(s.read_file(t->file, bytes, why) && bytes.size() == 44 + 44100 * 4);
    auto stream = s.open_file(t->file, why);
    CHECK(stream && stream->size() == bytes.size());
    uint8_t head[4] = {};
    CHECK(stream && stream->read(0, head, 4) == 4 && std::memcmp(head, "RIFF", 4) == 0);
    CHECK(!s.read_file(FileRef{}, bytes, why) && !s.read_file(FileRef{5, "x"}, bytes, why) && !s.open_file(FileRef{0, "../x"}, why) && !s.read_file(FileRef{0, "nope.wav"}, bytes, why));
    CHECK(s.audio_bytes() == 320 * 2);
    // a ZIP pack streams stored music straight from the archive
    put_file(g_tmp + "/audio1.zip", make_zip({{"manifest.json", slurp(dir + "/manifest.json"), 0}, {"sfx/fire.wav", slurp(dir + "/sfx/fire.wav"), 8}, {"music/t.wav", slurp(dir + "/music/t.wav"), 0},
                                              {"music/i.wav", slurp(dir + "/music/i.wav"), 8}}));
    Loaded z = load({g_tmp + "/audio1.zip"});
    CHECK(z.ok[0] && z.s->sfx(key640) && z.s->music_slot("title") && z.s->music_slot("level1") && z.s->music_slot_count() == 4);
    auto zs = z.s->open_file(z.s->music_slot("title")->file, why);
    CHECK(zs && zs->size() == bytes.size() && zs->read(0, head, 4) == 4 && std::memcmp(head, "RIFF", 4) == 0);
    // layering of audio: the later pack overrides the clip with the same key, defaults merge per field
    const std::string dir2 = make_pack("audio2", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"audio\": {\"sfx\": {\"" + key640 + "\": {\"file\": \"x.wav\", \"gain_db\": 5}}, "
                                                  "\"music\": {\"defaults\": {\"fade_in_ms\": 70}, \"slots\": {\"title\": {\"file\": \"t2.wav\"}}}}}");
    put_file(dir2 + "/x.wav", make_wav(8000, 1, tone(64)));
    put_file(dir2 + "/t2.wav", make_wav(8000, 1, tone(64)));
    l = load({dir, dir2});
    CHECK(l.s->sfx(key640)->gain_db == 5.0f && l.s->sfx(key640)->clip->rate == 8000 && l.s->sfx(alias)->gain_db == -3.0f && said(l.rep, "audio.sfx[\"" + key640 + "\"]: overrides the entry of 'audio1'", 0));
    CHECK(l.s->music_slot("title")->file.pack == 1 && l.s->music_slot("title")->loop.kind == LoopKind::Default && l.s->music_defaults().crossfade_ms == 250 && l.s->music_defaults().fade_in_ms == 70);
    // scope: an audio entry for the CDTV only
    const std::string dir3 = make_pack("audio3", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"audio\": {\"sfx\": {\"" + key640 + "\": {\"file\": \"x.wav\", \"games\": [\"cdtv\"]}}, \"music\": {\"slots\": {\"cd2\": {\"file\": \"x.wav\", \"games\": [\"cdtv\"]}}}}}");
    put_file(dir3 + "/x.wav", make_wav(8000, 1, tone(64)));
    l = load({dir3}, ctx_of(GameFamily::Disk, "canonical"));
    CHECK(!l.s->has_audio());
    l = load({dir3}, ctx_of(GameFamily::Cdtv, "cdtv"));
    CHECK(l.s->sfx(key640) && l.s->music_slot("cd2"));
    // probe only (the validator without --full): entries are registered, nothing is decoded
    StackOptions vo;
    vo.decode_audio = false;
    l = load({dir}, {}, vo);
    CHECK(l.s->sfx(key640) && l.s->sfx(key640)->clip && l.s->sfx(key640)->clip->pcm.empty() && l.s->audio_bytes() == 0);
    // the decoded-sound memory limit and the clip size limit
    StackOptions mo;
    mo.limits.max_audio_total = 100;
    l = load({dir}, {}, mo);
    CHECK(!l.s->sfx(key640) && said(l.rep, "over the memory limit for decoded sound effects and speech"));
    StackOptions bo;
    bo.limits.max_clip_bytes = 100;
    l = load({dir}, {}, bo);
    CHECK(!l.s->sfx(key640) && said(l.rep, "decodes to more than 100 bytes"));
    // no codec registered: packs still load, effects are not registered, one note says so, music is checked for existence only
    set_audio_codec(nullptr);
    l = load({dir});
    CHECK(l.ok[0] && !l.s->sfx(key640) && !l.s->speech(key9000) && said(l.rep, "this build has no audio decoder yet", 0) && l.s->music_slot("title") && l.s->music_slot("cd5") && !l.s->music_slot("level3"));
    set_audio_codec(&kWavCodec);
}

// ---- discovery and the load order of a run ---------------------------------------------------------------------------------------

void test_discovery() {
    std::printf("discovery: <data dir>/packs, enabled ids, load order\n");
    const std::string data = g_tmp + "/data";
    fs::remove_all(data);
    const std::string pdir = data + "/packs";
    auto pack = [&](const std::string& folder, const std::string& manifest) {
        fs::create_directories(pdir + "/" + folder);
        put_file(pdir + "/" + folder + "/manifest.json", manifest);
    };
    const std::string k1 = key16(kH1, 4, 4);
    pack("alpha", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"id\": \"alpha\", \"name\": \"Alpha\", \"pack_version\": \"1.0.0\", \"author\": \"A\", \"license\": \"MIT\", \"games\": [\"disk\"], "
                  "\"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\"}}}");
    put_flat(pdir + "/alpha/a.png", 4, 4);
    pack("beta pack", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": \"Beta Pack\", \"audio\": {}, \"screens\": {}}");
    pack("broken", "{\"format\": ");
    pack("future", "{\"format\": \"x2-asset-pack\", \"version\": 7}");
    fs::create_directories(pdir + "/not-a-pack");
    put_file(pdir + "/readme.txt", "x");
    put_file(pdir + "/notpack.zip", make_zip({{"data.bin", "x", 0}}));
    put_file(pdir + "/gamma.zip", make_zip({{"gamma/manifest.json", "{\"format\": \"x2-asset-pack\", \"version\": 1, \"id\": \"gamma\", \"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\"}}}", 0}, {"gamma/a.png", slurp(pdir + "/alpha/a.png"), 0}}));
    put_file(pdir + "/corrupt.zip", "PK\x03\x04 broken");
    const std::vector<PackInfo> found = discover_packs(pdir);
    std::map<std::string, PackInfo> by_id;
    for (const PackInfo& p : found) by_id[p.id] = p;
    CHECK(found.size() == 6 && by_id.count("alpha") && by_id.count("beta-pack") && by_id.count("broken") && by_id.count("future") && by_id.count("gamma") && by_id.count("corrupt"));
    CHECK(by_id["alpha"].usable && by_id["alpha"].discovered && by_id["alpha"].name == "Alpha" && by_id["alpha"].pack_version == "1.0.0" && by_id["alpha"].author == "A" && by_id["alpha"].license == "MIT" &&
          by_id["alpha"].scope.games == 1 && by_id["alpha"].provides_graphics && !by_id["alpha"].provides_audio && by_id["alpha"].format_version == 2);
    CHECK(by_id["beta-pack"].usable && by_id["beta-pack"].provides_audio && by_id["beta-pack"].provides_screens && !by_id["beta-pack"].provides_graphics && by_id["beta-pack"].name == "Beta Pack");
    CHECK(!by_id["broken"].usable && by_id["broken"].problem.find("manifest.json:") == 0 && !by_id["future"].usable && by_id["future"].problem.find("needs a newer x2recomp") != std::string::npos);
    CHECK(by_id["gamma"].usable && by_id["gamma"].format_version == 1 && !by_id["corrupt"].usable && by_id["corrupt"].problem.find("not a usable ZIP file") == 0);
    CHECK(discover_packs(data + "/nothing").empty() && discover_packs("").empty());

    // load order: enabled ids in the listed order, then the paths; the same pack twice loads once; an unknown id warns
    AssetPackSettings s;
    s.packs_dir = pdir;
    s.packs_enabled = {"gamma", "missing-one", "alpha"};
    s.pack_paths = {pdir + "/alpha", pdir + "/./alpha/", pdir + "/beta pack"};
    PackReport rep;
    std::shared_ptr<PackStack> st = load_pack_stack(s, ctx_of(GameFamily::Disk, "canonical"), rep);
    CHECK(st && st->packs().size() == 3 && st->packs()[0].id == "gamma" && st->packs()[1].id == "alpha" && st->packs()[2].id == "beta-pack");
    CHECK(st && st->packs()[0].discovered && st->packs()[1].discovered && !st->packs()[2].discovered && st->packs()[0].enabled);
    CHECK(said(rep, "the enabled pack 'missing-one' was not found in " + pdir, 1) && said(rep, "listed more than once; loaded once", 0));
    CHECK(st && st->graphic(hex_of(kH1), 4, 4) && said(rep, "overrides the entry of 'gamma'", 0));
    auto lib = load_packs(s, ctx_of(GameFamily::Disk, "canonical"), rep);
    CHECK(lib && lib->packs().size() == 3);
    // nothing asked for: the null library and no message
    PackReport none;
    CHECK(load_pack_stack(AssetPackSettings{}, PackContext{}, none) == nullptr && none.messages.empty());
    AssetPackSettings only_ids;
    only_ids.packs_enabled = {"alpha"};  // no pdir directory: nothing found, one warning, a stack with no pdir
    PackReport w;
    auto empty_stack = load_pack_stack(only_ids, PackContext{}, w);
    CHECK(!empty_stack && said(w, "the enabled pack 'alpha' was not found", 1));

    // the global accessors
    set_packs(st);
    CHECK(&packs() == st.get() && packs().graphic(hex_of(kH1), 4, 4));
    set_packs(nullptr);
    CHECK(&packs() == &null_library());
}

// ---- the validator -------------------------------------------------------------------------------------------------------------

CheckResult check(const std::string& path, bool strict = false, bool full = false, const std::string& against = "", std::optional<GameFamily> game = std::nullopt, const std::string& build = "") {
    CheckOptions o;
    o.path = path;
    o.strict = strict;
    o.full = full;
    o.against = against;
    o.game = game;
    o.build = build;
    return check_asset_pack(o);
}

void test_validator() {
    std::printf("validator: findings, exit codes, --against, JSON\n");
    const std::string k1 = key16(kH1, 16, 8), k2 = key16(kH2, 16, 8), k3 = key16(kH3, 4, 4);
    // a clean pack
    std::string dir = make_pack("val_ok", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": \"Clean\", \"license\": \"CC0-1.0\", \"scale\": 2, \"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\"}}, "
                                          "\"panel\": {\"cells\": {\"" + std::string(kH1) + "-2\": {\"png\": \"cell.png\"}}}, \"screens\": {\"t\": {\"png\": \"s.png\", \"ref_palette\": [\"#000\", \"#fff\"], \"signature\": {\"min\": 6, "
                                          "\"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"" + std::string(kH1) + "\"}, {\"x\": 1, \"y\": 0, \"hash\": \"" + kH2 + "\"}, {\"x\": 2, \"y\": 0, \"hash\": \"" + kH3 + "\"}, "
                                          "{\"x\": 3, \"y\": 0, \"hash\": \"4123456789abcdef\"}, {\"x\": 4, \"y\": 0, \"hash\": \"5123456789abcdef\"}, {\"x\": 5, \"y\": 0, \"hash\": \"6123456789abcdef\"}]}}}}",
                                {{"a.png", {32, 16}}, {"cell.png", {32, 16}}, {"s.png", {640, 400}}});
    CheckResult r = check(dir);
    if (r.exit_code != 0) std::fprintf(stderr, "%s", format_check(r, false).c_str());
    CHECK(r.exit_code == 0 && r.report.count(Severity::Error) == 0 && r.report.count(Severity::Warning) == 0 && r.entries == 3 && r.skipped == 0 && r.label == "Clean");
    CHECK(check(dir, true).exit_code == 0);
    const std::string text = format_check(r, false);
    CHECK(text.find("info: asset pack: Clean: loaded: 1 graphics") != std::string::npos && text.find("check: Clean: 0 errors, 0 warnings, 3 entries used, 0 skipped: valid") != std::string::npos);

    // size, duplicate, group and file findings
    dir = make_pack("val_warn", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"unknown_top\": 1, \"scale\": 2, \"graphics\": {\"" + k1 + "\": {\"png\": \"a.png\", \"extra\": 1}, \"" + k3 +
                                         "\": {\"png\": \"a.png\", \"scale\": 1}}, "
                                         "\"atlases\": [{\"png\": \"atlas.png\", \"frames\": [{\"key\": \"" + k1 + "\", \"rect\": [0, 0, 8, 8]}]}], \"groups\": {\"ship\": [\"" + k1 + "\", \"" + k2 + "\"]}, "
                                         "\"panel\": {\"background\": {\"png\": \"pbg.png\"}, \"cells\": {\"" + std::string(kH1) + "-3\": {\"png\": \"cell.png\"}}}, "
                                         "\"screens\": {\"t\": {\"png\": \"s.png\", \"dynamic\": [{\"x\": 300, \"y\": 190, \"w\": 40, \"h\": 20}], \"signature\": {\"min\": 1, \"blocks\": [{\"x\": 0, \"y\": 0, \"hash\": \"" + std::string(kH1) + "\"}]}}}, "
                                         "\"far\": [{\"png\": \"far.png\", \"scale\": 2, \"lines\": [\"" + std::string(kH1) + "\"]}], \"preview\": \"missing.png\"}",
                {{"a.png", {32, 16}}, {"atlas.png", {16, 16}}, {"pbg.png", {600, 16}}, {"cell.png", {10, 10}}, {"s.png", {320, 200}}, {"far.png", {640, 3}}, {"unused.png", {1, 1}}});
    r = check(dir);
    CHECK(r.exit_code == 2);  // the missing preview is an error
    CHECK(said(r.report, "unknown field \"unknown_top\"", 1) && said(r.report, "unknown field \"extra\"", 1));
    CHECK(said(r.report, "graphics[\"" + k3 + "\"]: 'a.png' is 32x16, a graphic of this key at this scale is expected to be 4x4", 1));
    CHECK(said(r.report, "is defined twice (graphics and atlases[0]); the later one wins", 1));
    CHECK(said(r.report, "the frame is 8x8, a graphic of this key at scale 2 is expected to be 32x16", 1));
    CHECK(said(r.report, "groups[\"ship\"]: lists the key '" + k2 + "', which no graphics entry or atlas defines", 1));
    CHECK(said(r.report, "'pbg.png' is 600x16, the panel background is expected to be 640x16", 1) && said(r.report, "'cell.png' is 10x10, a panel run of this many cells is expected to be 48x16", 1));
    CHECK(said(r.report, "'s.png' is 320x200, a screen picture is expected to be 640x400", 1) && said(r.report, "a \"dynamic\" rectangle reaches outside the 320x200 window", 1) &&
          said(r.report, "the signature has fewer than 6 blocks", 1) && said(r.report, "has no \"ref_palette\"", 0));
    CHECK(said(r.report, "'far.png' is 3 pixels tall, not a multiple of its scale 2", 1) && said(r.report, "preview: missing file 'missing.png'", 2));
    CHECK(said(r.report, "1 file is not used by manifest.json (unused.png)", 0) && said(r.report, "memory: decoded pictures 0 MB of 512 MB", 0));
    // warnings only: with --strict that is exit code 1 (the preview is an error: remove it for this part)
    std::string man = slurp(dir + "/manifest.json");
    man.replace(man.find(", \"preview\": \"missing.png\""), 26, "");
    put_file(dir + "/manifest.json", man);
    r = check(dir);
    CHECK(r.exit_code == 0 && r.report.count(Severity::Warning) > 5);
    CHECK(check(dir, true).exit_code == 1);

    // the licence
    dir = make_pack("val_nolic", "{\"format\": \"x2-asset-pack\", \"version\": 2}");
    CHECK(check(dir).exit_code == 0 && said(check(dir).report, "license: missing", 1));
    r = check(dir, true);
    CHECK(r.exit_code == 2 && said(r.report, "license: missing", 2));

    // refused packs and unreadable paths
    CHECK(check(g_tmp + "/does-not-exist").exit_code == 3);
    CHECK(check(make_pack("val_broken", "{\"format\": ")).exit_code == 2);
    CHECK(check(make_pack("val_fmt", "{\"format\": \"other\", \"version\": 2}")).exit_code == 2);
    CHECK(check(make_pack("val_v3", "{\"format\": \"x2-asset-pack\", \"version\": 3}")).exit_code == 2 && said(check(g_tmp + "/val_v3").report, "needs a newer x2recomp"));
    put_file(g_tmp + "/garbage.zip", "not a zip at all, not a zip at all");
    CHECK(check(g_tmp + "/garbage.zip").exit_code == 3);
    put_file(g_tmp + "/nomanifest.zip", make_zip({{"a.png", "x", 0}}));
    CHECK(check(g_tmp + "/nomanifest.zip").exit_code == 2);
    fs::create_directories(g_tmp + "/val_empty_dir");
    CHECK(check(g_tmp + "/val_empty_dir").exit_code == 2);
    r = check(make_pack("val_badentry", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"MIT\", \"graphics\": {\"bad\": {\"png\": \"a.png\"}}}"));
    CHECK(r.exit_code == 2 && r.skipped == 1);

    // scoping information
    dir = make_pack("val_scope", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"MIT\", \"games\": [\"cdtv\"], \"graphics\": {\"" + k3 + "\": {\"png\": \"a.png\"}, \"" + key16(kH2, 4, 4) +
                                         "\": {\"png\": \"a.png\", \"games\": [\"disk\"]}}}",
                    {{"a.png", {4, 4}}});
    r = check(dir, false, false, "", GameFamily::Cdtv, "cdtv");
    CHECK(r.exit_code == 0 && said(r.report, "1 of 2 entries apply to cdtv, build cdtv", 0) && r.report.count(Severity::Error) == 0);
    r = check(dir, false, false, "", GameFamily::Disk, "canonical");
    CHECK(said(r.report, "the pack's games/builds do not include disk build canonical", 1));
    CHECK(r.entries == 2);  // validation loads every entry whatever the scope

    // --against: coverage, typos, byte-identical copies
    const std::string dump = g_tmp + "/val_dump";
    fs::remove_all(dump);
    put_png(dump + "/sprites/a.png", 32, 16, [](int, int) { return 0xFF112233u; });
    put_png(dump + "/sprites/b.png", 32, 16, [](int, int) { return 0xFF445566u; });
    put_png(dump + "/sprites/c.png", 4, 4, [](int, int) { return 0xFF778899u; });
    put_png(dump + "/panel/p1.png", 16, 8, [](int, int) { return 0xFF000001u; });
    put_png(dump + "/far/strip.png", 320, 2, [](int, int) { return 0xFF0000FFu; });
    put_file(dump + "/sfx/s.wav", make_wav(22050, 1, tone(100)));
    put_file(dump + "/manifest.json",
             "{\"format\": \"x2-asset-pack\", \"version\": 2, \"name\": \"dump\", \"graphics\": {\"" + k1 + "\": {\"png\": \"sprites/a.png\", \"kind\": \"sprite\", \"uses\": 4}, \"" + k2 +
                 "\": {\"png\": \"sprites/b.png\"}, \"" + k3 + "\": {\"png\": \"sprites/c.png\"}}, \"far\": [{\"png\": \"far/strip.png\", \"lines\": [\"aaaaaaaaaaaaaaaa\", \"bbbbbbbbbbbbbbbb\"]}], "
                 "\"panel\": {\"cells\": {\"" + std::string(kH1) + "-1\": {\"png\": \"panel/p1.png\"}}}, \"audio\": {\"sfx\": {\"0123456789abcdef-200\": {\"file\": \"sfx/s.wav\"}}}, \"seen_palettes\": []}");
    const std::string typo = key16("9923456789abcdef", 4, 4);
    dir = make_pack("val_against", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"CC0-1.0\", \"graphics\": {\"" + k1 + "\": {\"png\": \"mine.png\", \"scale\": 2}, \"" + k2 + "\": {\"png\": \"copy.png\", \"scale\": 2}, \"" + typo +
                                            "\": {\"png\": \"t.png\"}}, \"far\": [{\"png\": \"far.png\", \"lines\": [\"aaaaaaaaaaaaaaaa\", \"cccccccccccccccc\"]}], "
                                            "\"panel\": {\"cells\": {\"" + std::string(kH1) + "-1\": {\"png\": \"pcopy.png\"}, \"" + kH3 + "-1\": {\"png\": \"pcopy.png\"}}}, "
                                            "\"audio\": {\"sfx\": {\"0123456789abcdef-200\": {\"file\": \"s.wav\"}, \"1123456789abcdef-300\": {\"file\": \"s.wav\"}}}}",
                    {{"mine.png", {32, 16}}, {"t.png", {4, 4}}, {"far.png", {320, 2}}});
    fs::copy_file(dump + "/sprites/b.png", dir + "/copy.png");
    fs::copy_file(dump + "/panel/p1.png", dir + "/pcopy.png");
    fs::copy_file(dump + "/sfx/s.wav", dir + "/s.wav");
    r = check(dir, false, false, dump);
    if (r.exit_code != 0) std::fprintf(stderr, "%s", format_check(r, false).c_str());
    CHECK(r.exit_code == 0);
    CHECK(said(r.report, "coverage of the dump: graphics 2/3 (66%), far lines 1/2 (50%), screens 0/0, panel cells 1/1, sound effects 1/1, speech 0/0", 0));
    CHECK(said(r.report, "graphics[\"" + typo + "\"]: this key is not in the dump", 1) && said(r.report, "far[\"" + hash_hex(hex_of("cccccccccccccccc")) + "\"]: this key is not in the dump", 1) ||
          said(r.report, "far[\"cccccccccccccccc\"]: this key is not in the dump", 1));
    CHECK(said(r.report, "panel[\"" + std::string(kH3) + "-1\"]: this key is not in the dump", 1) && said(r.report, "audio.sfx[\"1123456789abcdef-300\"]: this key is not in the dump", 1));
    CHECK(said(r.report, "graphics[\"" + k2 + "\"]: 'copy.png' is byte-identical to the dump's original: a dump holds the game's own material and must not be redistributed", 1) && !said(r.report, "'mine.png' is byte-identical"));
    CHECK(said(r.report, "panel[\"" + std::string(kH1) + "-1\"]: 'pcopy.png' is byte-identical", 1) && said(r.report, "audio.sfx[\"0123456789abcdef-200\"]: 's.wav' is byte-identical", 1));
    CHECK(check(dir, true, false, dump).exit_code == 1);
    CHECK(said(check(dir, false, false, g_tmp + "/no-such-dump").report, "cannot read", 2) && check(dir, false, false, g_tmp + "/no-such-dump").exit_code == 2);

    // audio findings
    dir = make_pack("val_audio",
                    "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"MIT\", \"audio\": {\"sfx\": {\"0123456789abcdef-640\": {\"file\": \"clip.wav\"}, \"1123456789abcdef-640\": {\"file\": \"loud.wav\"}}, "
                    "\"music\": {\"slots\": {\"title\": {\"file\": \"t.wav\", \"loop\": {\"start_s\": 0.5, \"end_s\": 9}}, \"level1\": {\"file\": \"t.wav\", \"loop\": {\"start_s\": 7}}, \"level2\": {\"file\": \"t.wav\", \"loop\": {\"start_s\": 0.1, \"end_s\": 0.9}}}}}}");
    put_file(dir + "/clip.wav", make_wav(22050, 1, tone(320)));
    put_file(dir + "/loud.wav", make_wav(11111, 1, tone(320)));
    put_file(dir + "/t.wav", make_wav(8000, 1, tone(8000)));  // one second
    r = check(dir, false, true);
    CHECK(r.exit_code == 2 && said(r.report, "audio.sfx[\"1123456789abcdef-640\"]: 'loud.wav' clips: its peak is 6.0 dBFS above full scale", 1) && !said(r.report, "'clip.wav' clips"));
    CHECK(said(r.report, "audio.music.slots[\"title\"]: the loop ends at 9", 2) && said(r.report, "audio.music.slots[\"level1\"]: the loop starts at 7", 2) && !said(r.report, "[\"level2\"]: the loop", 2));
    CHECK(!said(check(dir, false, false).report, "clips: its peak"));  // without --full nothing is decoded
    put_file(g_tmp + "/val_audio.zip", make_zip({{"manifest.json", slurp(dir + "/manifest.json"), 0}, {"clip.wav", slurp(dir + "/clip.wav"), 0}, {"loud.wav", slurp(dir + "/loud.wav"), 0}, {"t.wav", slurp(dir + "/t.wav"), 8}}));
    r = check(g_tmp + "/val_audio.zip");
    CHECK(said(r.report, "'t.wav': a ZIP member stored without compression streams from the archive; this one is deflated", 1));
    set_audio_codec(nullptr);
    r = check(dir);
    CHECK(said(r.report, "this build has no audio decoder: sound files were checked for existence only", 0));
    set_audio_codec(&kWavCodec);

    // requires
    r = check(make_pack("val_req", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"license\": \"MIT\", \"requires\": {\"x2recomp\": \">=0.1\", \"packs\": [{\"id\": \"base\", \"version\": \">=1.0\"}]}}"));
    CHECK(said(r.report, "requires[\"base\"]: needs the pack 'base' >=1.0 to be loaded before this one", 0) || said(r.report, "needs the pack 'base' >=1.0", 0));

    // JSON output
    r = check(make_pack("val_json", "{\"format\": \"x2-asset-pack\", \"version\": 2, \"zzz\": 1}"));
    const std::string js = format_check(r, true);
    Json j;
    std::string why;
    CHECK(parse_json(js, j, why) && j.is_arr() && j.a.size() == r.report.messages.size() + 1);
    if (j.is_arr() && !j.a.empty()) {
        const Json& last = j.a.back();
        CHECK(last.get("severity") && last.get("severity")->s == "summary" && last.get("exit_code") && int(last.get("exit_code")->n) == r.exit_code && last.get("warnings") && last.get("warnings")->n == 2);
        CHECK(j.a[0].get("message") && j.a[0].get("message")->s.rfind("asset pack: ", 0) == 0 && j.a[0].get("severity"));
    }
    CHECK(format_check(check(g_tmp + "/does-not-exist"), true).find("\"exit_code\": 3") != std::string::npos);
}

// ---- the version 1 interface over the loader --------------------------------------------------------------------------------------

// (the version 1 tests themselves are in tests/modern_tests.cpp and run unchanged) A pack stack handed to the version 1 view.

// ---- fuzz ----------------------------------------------------------------------------------------------------------------------

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    size_t below(size_t n) { return n ? size_t(next() % n) : 0; }
};

std::string mutate(const std::string& base, Rng& r, const std::vector<std::string>& tokens) {
    std::string s = base;
    const int n = 1 + int(r.below(4));
    for (int i = 0; i < n && !s.empty(); ++i) {
        const size_t at = r.below(s.size());
        switch (r.below(7)) {
        case 0: s[at] = char(r.next()); break;
        case 1: s.erase(at, 1 + r.below(std::min<size_t>(40, s.size() - at))); break;
        case 2: s.insert(at, s.substr(r.below(s.size()), 1 + r.below(60))); break;
        case 3: s.insert(at, tokens[r.below(tokens.size())]); break;
        case 4: s.resize(at); break;
        case 5: s[at] ^= char(1u << r.below(8)); break;
        default: {
            const size_t end = std::min(s.size(), at + 1 + r.below(30));
            s.replace(at, end - at, tokens[r.below(tokens.size())]);
        }
        }
    }
    return s;
}


std::string to_json(const Json& j) {
    switch (j.t) {
    case Json::Null: return "null";
    case Json::Bool: return j.b ? "true" : "false";
    case Json::Num: {
        char b[40];
        std::snprintf(b, sizeof b, "%.17g", j.n);
        return b;
    }
    case Json::Str: return json_quote(j.s);
    case Json::Arr: {
        std::string o = "[";
        for (size_t i = 0; i < j.a.size(); ++i) o += (i ? "," : "") + to_json(j.a[i]);
        return o + "]";
    }
    case Json::Obj: {
        std::string o = "{";
        for (size_t i = 0; i < j.o.size(); ++i) o += (i ? "," : "") + json_quote(j.o[i].first) + ":" + to_json(j.o[i].second);
        return o + "}";
    }
    }
    return "null";
}

// Structure-aware mutation: edits values of the parsed tree (types, edge numbers, key forms, paths, deleted and duplicated members) so that
// most results are valid JSON and reach the manifest parser's checks.
void mutate_tree(Json& root, Rng& r) {
    std::vector<Json*> nodes;
    std::function<void(Json&)> walk = [&](Json& n) {
        nodes.push_back(&n);
        for (Json& c : n.a) walk(c);
        for (JsonMember& m : n.o) walk(m.second);
    };
    static const double kNums[] = {0, -1, 1, 2, 4, 8, 9, 16, 320, 4096, 4097, 65535, 65536, 2147483647.0, 2147483648.0, -2147483649.0, 1e9, 1e300, 0.5, 0.25, 7.5, -0.0};
    static const char* const kStrs[] = {"", "../x.png", "/etc/passwd", "con", "a\\b", "a//b", "x.png", "sprites/a.png", "0123456789abcdef", "0123456789abcdef-16x8", "0123456789abcdef-1x1",
                                        "0123456789abcdef-4097x1", "0123456789abcdef-640", "0123456789abcdef-1", "f:0123456789abcdef-4x4", "#fff", "#ffffff", "#ggg", "disk", "cdtv", "st", "canonical", "mute",
                                        "mix:-12", "mix:abc", "channel", "follow", "title", "level3", "cd5", "scale", "none", "round", "SPEECH63", ">=1.0", "1.2.3", "x2-asset-pack", "-"};
    const int edits = 1 + int(r.below(3));
    for (int e = 0; e < edits; ++e) {
        nodes.clear();  // an edit may free subtrees: collect the nodes again
        walk(root);
        Json& n = *nodes[r.below(nodes.size())];
        switch (r.below(9)) {
        case 0: n = Json(); n.t = Json::Num; n.n = kNums[r.below(sizeof kNums / sizeof *kNums)]; break;
        case 1: { const std::string t = kStrs[r.below(sizeof kStrs / sizeof *kStrs)]; n = Json(); n.t = Json::Str; n.s = t; break; }
        case 2: n = Json(); n.t = Json::Null; break;
        case 3: n = Json(); n.t = Json::Bool; n.b = r.below(2); break;
        case 4: if (n.t == Json::Obj && !n.o.empty()) n.o.erase(n.o.begin() + long(r.below(n.o.size()))); else if (n.t == Json::Arr && !n.a.empty()) n.a.erase(n.a.begin() + long(r.below(n.a.size()))); break;
        case 5: if (n.t == Json::Obj && !n.o.empty()) { JsonMember m = n.o[r.below(n.o.size())]; n.o.push_back(m); } else if (n.t == Json::Arr && !n.a.empty()) n.a.push_back(n.a[r.below(n.a.size())]); break;
        case 6: if (n.t == Json::Obj && !n.o.empty()) n.o[r.below(n.o.size())].first = kStrs[r.below(sizeof kStrs / sizeof *kStrs)]; break;
        case 7: n = Json(); n.t = r.below(2) ? Json::Arr : Json::Obj; break;
        default: if (n.t == Json::Str) n.s += std::string(1 + r.below(300), 'a'); else if (n.t == Json::Arr) { Json x; x.t = Json::Num; x.n = kNums[r.below(sizeof kNums / sizeof *kNums)]; n.a.push_back(x); } break;
        }
    }
}

void test_fuzz(int iterations) {
    std::printf("fuzz: %d manifests, ZIPs and PNGs mutated (no crash, no hang, invariants hold)\n", iterations);
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::string> tokens = {"null", "true", "false", "[", "]", "{", "}", "\"", ",", ":", "0", "-1", "1e308", "-1e999", "99999999999999999999", "0.5", "\"\\u", "\"\\ud800\"", "\"\\\\\"",
                                             "\"../x\"", "\"png\"", "\"scale\"", "\"keys\"", "\"games\"", "[\"cdtv\"]", "\"builds\"", "\"audio\"", "\"screens\"", "\"panel\"", "\"atlases\"", "\"grid\"",
                                             "\"frames\"", "\"rect\"", "[0,0,0,0]", "[4294967295,4294967295,4294967295,4294967295]", "\"file\"", "\"loop\"", "\"slots\"", "\"cells\"", "\"far\"",
                                             "\"lines\"", "\"requires\"", "\"0123456789abcdef-16x8\"", "\"0123456789abcdef-640\"", "\"0123456789abcdef-1\"", "\"#fff\"", "\"#ffffff\"", "\x01", "\xff\xfe"};
    // 1: manifests through the parser
    std::vector<std::string> bases = {slurp(g_src + "/docs/schema/examples/v2-full.example.json"), slurp(g_src + "/docs/schema/examples/v1-graphics.example.json"), slurp(g_tmp + "/gfx1/manifest.json"),
                                      slurp(g_tmp + "/audio1/manifest.json")};
    Rng rng(0x9E3779B97F4A7C15ull);
    size_t parsed = 0, accepted = 0;
    for (int i = 0; i < iterations; ++i) {
        Json j;
        std::string why;
        if (i % 3 == 0) {
            const std::string text = mutate(bases[size_t(i) % bases.size()], rng, tokens);
            if (!parse_json(text, j, why)) continue;
        } else {
            if (!parse_json(bases[size_t(i) % bases.size()], j, why)) continue;
            mutate_tree(j, rng);
            Json again;  // through the text form, as a file would be
            if (!parse_json(to_json(j), again, why)) continue;
            j = std::move(again);
        }
        ++parsed;
        Manifest m;
        PackReport rep;
        ParseOptions po;
        po.validate = i % 2 == 0;
        po.limits.max_entries = 200;
        std::string fatal;
        if (parse_manifest(j, "fuzz", po, m, rep, fatal)) {
            ++accepted;
            for (const GraphicDecl& g : m.graphics) CHECK(valid_graphic_key(g.keys[0]) || valid_flash_key(g.keys[0]));
            for (const ScreenDecl& sd : m.screens) CHECK(!sd.signature.blocks.empty() && sd.signature.min >= 1);
            for (const ClipDecl& c : m.clips) CHECK(valid_sample_key(c.key) && c.ref_period >= 0 && c.ref_period <= 65535);
            CHECK(m.entries <= 200 + 64);
        } else CHECK(!fatal.empty());
    }
    CHECK(parsed > 0 && accepted > 0);
    // 2: manifests through the loader, on a pack folder with real pictures (a fraction of the iterations: it reads files)
    const std::string fdir = g_tmp + "/fuzz_pack";
    fs::remove_all(fdir);
    fs::copy(g_tmp + "/gfx1", fdir, fs::copy_options::recursive);
    for (int i = 0; i < std::max(20, iterations / 100); ++i) {
        put_file(fdir + "/manifest.json", mutate(bases[2], rng, tokens));
        PackStack st(ctx_of(i % 3 == 0 ? GameFamily::Cdtv : GameFamily::Disk, i % 2 ? "cdtv" : ""));
        PackReport rep;
        st.add(fdir, rep);
        CHECK(st.picture_bytes() <= PackLimits().max_total);
        for (const PackMessage& m : rep.messages) CHECK(!m.format().empty());
    }
    // 3: ZIPs
    const std::string zbase = make_zip({{"pk/manifest.json", bases[0], 0}, {"pk/a.png", slurp(g_tmp + "/gfx1/a.png"), 0}, {"pk/far.png", slurp(g_tmp + "/gfx1/far.png"), 8}, {"pk/x/y.bin", "yyyy", 8}});
    for (int i = 0; i < iterations / 4; ++i) {
        const std::string z = mutate(zbase, rng, {"PK\x03\x04", "PK\x01\x02", "PK\x05\x06", std::string("\xff\xff\xff\xff", 4), std::string(2, '\0'), "../", "\\"});
        put_file(g_tmp + "/fuzz.zip", z);
        std::string why;
        auto src = open_pack_source(g_tmp + "/fuzz.zip", PackLimits(), why);
        if (!src) continue;
        for (const std::string& n : src->names()) {
            std::vector<uint8_t> out;
            src->read(n, 1u << 20, out, why);
            auto st = src->open(n, 1u << 20, why);
            if (st) {
                uint8_t b[16];
                st->read(0, b, 16);
                st->read(st->size(), b, 16);
            }
        }
    }
    // 4: PNGs
    std::string png = slurp(g_tmp + "/gfx1/a.png");
    for (int i = 0; i < iterations / 4; ++i) {
        const std::string t = mutate(png, rng, {"IHDR", "IDAT", "IEND", "PLTE", "tRNS", std::string("\xff\xff\xff\xff", 4), std::string("\x00\x00\x00\x00", 4), std::string("\x7f\xff\xff\xff", 4)});
        const std::vector<uint8_t> b(t.begin(), t.end());
        Picture p;
        std::string why;
        int w = 0, h = 0;
        png_size(b.data(), b.size(), w, h, why);
        if (decode_png(b.data(), b.size(), p, why, 256, 1u << 16)) CHECK(p.w > 0 && p.h > 0 && p.px.size() == size_t(p.w) * size_t(p.h));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    %zu manifests parsed as JSON, %zu accepted, %.1f s\n", parsed, accepted, secs);
}

// ---- the version 1 view over a stack ---------------------------------------------------------------------------------------------

} // namespace

#include "modern/assets.h"

namespace {

void test_v1_view() {
    std::printf("version 1 view: AssetLibrary adopts a stack\n");
    const std::string dir = g_tmp + "/gfx1";
    AssetPackSettings s;
    s.pack_paths = {dir};
    PackReport rep;
    auto stack = load_pack_stack(s, ctx_of(GameFamily::Disk, "canonical"), rep);
    modern::AssetLibrary lib;
    CHECK(lib.empty() && lib.graphics() == 0 && !lib.palette(nullptr == nullptr ? std::array<uint16_t, 16>{}.data() : nullptr));
    lib.adopt(stack);
    CHECK(!lib.empty() && lib.graphics() == stack->graphic_count() && lib.far_lines() == 2 && lib.palettes() == 1 && lib.bytes() == stack->picture_bytes());
    const modern::Replacement* r = lib.graphic(hex_of(kH1), 16, 8);
    CHECK(r && r->img && r->img->w == 32 && r->scale == 2 && r->ax == 3 && r->ay == -2);
    const modern::FarRow* f = lib.far_line(hex_of("bbbbbbbbbbbbbbbb"));
    CHECK(f && f->row == 2 && f->scale == 2);
    CHECK(lib.stack() == stack.get());
    // load() on a fresh library keeps the version 1 message form "<path>: <section>[<key>]: <problem>"
    modern::AssetLibrary lib2;
    std::vector<std::string> msgs;
    CHECK(lib2.load(dir, msgs) && lib2.graphics() == lib.graphics() && lib2.packs().size() == 1);
    bool found = false;
    for (const std::string& m : msgs) found = found || m.find(dir + ": graphics[\"" + key16("6123456789abcdef", 4, 4) + "\"]: missing file 'missing.png'") == 0;
    CHECK(found);
    CHECK(!lib2.load(g_tmp + "/nonexistent", msgs) && msgs.back() == g_tmp + "/nonexistent: no such asset pack (a directory or a .zip file)");
    // a context for another game: scoped entries do not load
    modern::AssetLibrary lib3;
    lib3.set_context(ctx_of(GameFamily::Cdtv, "cdtv"));
    msgs.clear();
    CHECK(lib3.load(g_tmp + "/scope1", msgs) && lib3.graphics() == 3);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: x2_pack_loader_tests SOURCE_DIR SCRATCH_DIR [fuzz iterations]\n");
        return 2;
    }
    g_src = argv[1];
    g_tmp = fs::absolute(argv[2]).string();
    const int iterations = argc > 3 ? std::atoi(argv[3]) : 20000;
    fs::remove_all(g_tmp);
    fs::create_directories(g_tmp);
    test_json();
    test_paths();
    test_versions();
    test_png();
    test_zip();
    test_manifest();
    test_loader_graphics();
    test_scope();
    test_layering();
    test_audio();
    test_discovery();
    test_validator();
    test_v1_view();
    test_fuzz(iterations);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
