// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// CDTV music layer unit tests with synthetic inputs built here (no game data, no CD audio): the streaming
// inflater (stored, fixed and dynamic blocks; chunked input; corrupt and truncated streams) against vendor/puff,
// CUE parsing, the programme-start rule, data-track identification, the disc import from a folder and from ZIPs
// (stored and deflate members), its error cases (missing, short and corrupted tracks, not the CDTV disc, sizes
// beyond a CD), the manifest checks, in-place use, the CD player (exact copy at 44.1 kHz, resampling accuracy,
// the CDTV loop poll, crossfaded loops, volume, pause modes, saturation), Paula's per-source gains and the
// settings keys.
//
//   x2_cdtv_tests WORKDIR
#include "audio/paula.h"
#include "cdtv/cd_player.h"
#include "cdtv/cdtv_disc.h"
#include "cdtv/cdtv_music.h"
#include "cdtv/inflate_stream.h"
#include "import/importer.h"
#include "import/sha1.h"
#include "persist.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

using namespace x2::cdtv;
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

// A raw deflate stream with one dynamic-Huffman block (zlib level 9) of words_text() below.
const char* const kDynamicHex[] = {
    "959c6b8e1b470c84ffd729f66ab2633846e224b017418e1fc35aad48d657ec310c682d691edd7c168b1cddbe7ef97c7bb9fd7cfdf8dbebbf2f1fbebc",
    "7ebdfdf3a2fbbbfad1db9f9f1fdd4ff8f0edefd7df3f7dfbfef2faedf6f18f1fe7bc7ff0f5d3e7db873f6fdf5f5ffefbf4d7df7fd51b3cbfd2db972a",
    "9f95abdfc2d2c65d9fe7be7fd117ac1fff1e0b9cebbabfdebfbdff1f2e37565796582ff2d8cbfd62f7d7b7153cee5e4f2ae27ddce7fe7559c0fdec2a",
    "c121f7767d53822bac08d3775ea5d3575c5fd56f5a2e3e65fb765cdd745d28e9ad6bf66db36e7d5d6cefff1199ebdb1f93ea9b8292ec9b16fbb2a67c",
    "9f270d43594d495d3e7db573359a0b553ab28ac9253496f5b896ebbd5e867da62ab45f1fed4fdd23aa4d4f73d066f1d5608b1d83ad35b9b637d3d18b",
    "57741332b10a3d1696d025e2d1c76e5037bbbb335fda75ddd6a369319a4aa38bcb23767072cdb8a0ac8be1256442a89bb725bd1f592f2ddb00fa495d",
    "d3347a72e37abc9ac9cfb39b3b98f5b8fb777ff76b54993f36ee3b096e3e37a8b0e898aee77bf7341677db7fff034e7292bb9ed685060199b62c34eb",
    "b7eb139d19e3da295e3f5d4db4d13d998cb76aea3b05c17a9be140c12487f73e4452eeefc02b6447d7d1348ef7ab3f834559fc2360a86596790d3293",
    "10cf8704e8ed093aa278c0c611d73a9c325beb8e5b84d18f0850b39a1ba790aa36004574c3a27a72e372ca90a5d90ca31154e0f3549d6d3628691621",
    "4d4dd1bdaa541c9c0ed5e468cd7b1d2b03d4a7e0a7337dc52caad5e31ea79b9defa0b82cbeca84ea0014beaf160fcb2a4500e971c4c10e89fab1f812",
    "17938f093630b110004a7641ce3703ae2c2bb7902e082ea2c2c813a539e7d1ab5013ae584e0654556e67596d0a310b05a3e05ddd5466a9cdc800549a",
    "e2c6d4c3342d074a25dd905955e1595deeb0035767a2cc45cf38137135ea088b04d66c408808ec8d11215cebd530e00f8f608a5af40b7ac832aac450",
    "18dbc2129b6a88f19a53d76af90e2b266adac0df568479929fae0201c3305c57c8b9b808048518d1ad2814d37628163914454a85b94cd80582234ccc",
    "70728df72dddf7037a628f028c37108002252458fcff523def99e071d39664e7fdb06ed1cc58e5a8460f6604d28c1a8bc95831335180e9d0e4c8d7f2",
    "72cecb04425ee6925d7627e39c59a965a20dca5c705f99d9551de4ac9bc81a4b039a2a9423d453b1e1566d0198f1da5e9b50290e353f533a93c58ff4",
    "50b0284a8f9a693181e8b815a443183d802b75ab455c94cc5e09cd751c9908512ad1ae9120cfa555c31d716749e8b3cf540c588ee9d8e8e71a287a2e",
    "fc9181336eb130577ac84d658150f971f027c674024c5edc428ee78e8b86e6f6ae486e2bb1cf787f2414c8c3873cc12a10c6278a8673c9635dc0dac9",
    "791c3741aad284ee145403dd4de3f67219616c658013b63daecd406cc20a5b3177725bd5d6a9bd83b4a57cc87eccc913ee84fe915318dc1466611575",
    "b36d21afe9f50b77e443958dd2ddd20d279d1886f7ae6568c4f5990723d633d549191ac157b522c7d0a854cf047e63df0a02333ff1e1215c29ef2063",
    "c9d281cf6977f1064f8f4183e01c417def1b9eda3d97860c166d380b1fe0c5d619f0169383450fcefe6a35471333116f07d622f2b2900d86b7b00753",
    "5486fe49a6d7f5ab83461a69f114a343cd0b504139f428cf1f38e380fab018e1616d9c61655260010e041c51fb0a99d0629cd6ca86316d5866392e56",
    "b9c3e4a6c91bd58dd86ca18b3c9ca128430e4834a3d3b7061feb6657b74a9aeebe8e6dcb14c39590fc54620db7b1987c2fce4f300627c69cfc4fb346",
    "4e5ff5f64cd10e19718cefa1a8e3d1364fbe82226c61bc33c7b8e7b64cef2091b8d23987963817c7b93603f666e190b9020dcd5b5c9feb7bc040eae9",
    "25149c2b4c9b3bd462e1e2a00b39a5cf78e93c4396ba2c913bdaa695621d48f958b90f759e1070f4dbd2fbb6b5085c80164eb35183fbbde6828e565c",
    "000ac7670755a417626d4e0e40925f4a66d684433fa3992d23a742c4da65da6c00b34ccaf3395873f85886d7a0a45762370e43121bbc641809be957a",
    "3058bbcf5106dfd7444e4c3d6cb47b9789202327309c2075d4a1e3098dfd87e998b0636528a3ccd924162014ed0cd1a946f481446bca61697f264408",
    "1b78f0d1a1b3b6cd7268cbd6d008e4893e816571cd8ce1f200d1a94088dd2b4cc4ba4062210a37bf14393ade735bdbf43a7a300607002223479ea898",
    "b4629cc53a528ca50837274759d1a8b51db4220c66096d98a4935f21f2f72ab857c8ccb9a834de208235f27816205bfb750faa3654a4f599a0440158",
    "8ce28878a1ed831df970eca17dd617eb5c8a59f5999fa39431f2076314cb0b514107473ab6b41428b730f5400670a0c71705e7649be2789a270fdc55",
    "1a75d012b8bd660ab067274e0e331ca450a316b43cc4706d66dd0b8f6d763db752239659a82701f325ee988e906eb696fa67295aac4569a48397e677",
    "6788af8cb7ed48e0a9e0c0ac2187a85fae4cae8d22c4195dce31ded86de946bc1783582b9f36da23b1d40a0ffd2dddc45496ed1ec99d8823d306cfde",
    "86fec842ef265e1fb1a195cb734cc79c4b8787378f3624dae6f3b66764ef70f2c43a1d46b753252d98534cec8a72cf8629da111d1c64c758943ec9d4",
    "320d425c793881d1779afd3661293f6112198933ef7e1a485801c079be0c1ade6340334d46a5471922284e4f81ae09394d5813c57adc6cfc6d8368e7",
    "3cd64650203532dab02dd6cb48317051a013adb63ef11a0b53c52eb745c5fc1036f668dbeeb11ed1dafcee8814b3ff5e9461532cd4a1175ae7cfa2fd",
    "c20f5118f36345f4423969219a8524ff8257177a2a4b5be8bc000d59818081088de5ea7aed6a9e7fa842e0ceec2e2b139098c008f76249ba5403ee6c",
    "576ebb0e432d0f6bf1807864002c7c7259e9fe6486bc8afbf438fef2e47a8a8e19cd511ade862796dc7e05683a0ea2678d81d2b16ec31a3104fdb1c4",
    "326162e3f62992105ada619bebe6f2872a5ce5e9e2d8ed86c775a8cf24fb81009c98f4a22c1206db33959b6ae68f499c6666e61dfe07",
};

Bytes words_text() {
    static const char* const words[] = {"megablast", "xenon", "bitmap", "brothers", "amiga", "cdtv", "track", "\n"};
    uint32_t x = 12345;
    std::string s;
    for (int i = 0; i < 3000; ++i) {
        x = (x * 1103515245u + 12345u) & 0x7fffffffu;
        s += words[(x >> 16) % 8];
        s += ' ';
    }
    return Bytes(s.begin(), s.end());
}

Bytes from_hex(const char* const* lines, size_t n) {
    Bytes b;
    for (size_t i = 0; i < n; ++i)
        for (const char* p = lines[i]; p[0] && p[1]; p += 2) b.push_back(uint8_t(std::stoi(std::string(p, 2), nullptr, 16)));
    return b;
}

// ---- a small deflate encoder for test inputs: fixed Huffman codes, greedy LZ77 (or stored blocks) ----
struct BitWriter {
    Bytes out;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t v, int bits) { // LSB first
        for (int i = 0; i < bits; ++i) {
            acc |= ((v >> i) & 1u) << n;
            if (++n == 8) { out.push_back(uint8_t(acc)); acc = 0; n = 0; }
        }
    }
    void huff(uint32_t code, int bits) { // Huffman codes go most significant bit first
        for (int i = bits - 1; i >= 0; --i) put((code >> i) & 1u, 1);
    }
    Bytes finish() {
        if (n) out.push_back(uint8_t(acc));
        return out;
    }
};

void fixed_literal(BitWriter& w, int v) {
    if (v < 144) w.huff(uint32_t(0x30 + v), 8);
    else if (v < 256) w.huff(uint32_t(0x190 + v - 144), 9);
    else if (v < 280) w.huff(uint32_t(v - 256), 7);
    else w.huff(uint32_t(0xC0 + v - 280), 8);
}

Bytes deflate_fixed(const Bytes& d) {
    static const int lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    BitWriter w;
    w.put(1, 1); // final
    w.put(1, 2); // fixed
    std::vector<int64_t> head(1 << 16, -1);
    auto hash = [&](size_t i) { return (d[i] << 8 ^ d[i + 1] << 4 ^ d[i + 2]) & 0xFFFF; };
    for (size_t i = 0; i < d.size();) {
        int best = 0;
        size_t dist = 0;
        if (i + 3 <= d.size()) {
            int64_t c = head[size_t(hash(i))];
            if (c >= 0 && i - size_t(c) <= 32768) {
                int len = 0;
                while (len < 258 && i + size_t(len) < d.size() && d[size_t(c) + size_t(len)] == d[i + size_t(len)]) ++len;
                if (len >= 3) { best = len; dist = i - size_t(c); }
            }
            head[size_t(hash(i))] = int64_t(i);
        }
        if (!best) { fixed_literal(w, d[i]); ++i; continue; }
        int li = 28;
        while (lbase[li] > best) --li;
        fixed_literal(w, 257 + li);
        w.put(uint32_t(best - lbase[li]), lext[li]);
        int di = 29;
        while (dbase[di] > int(dist)) --di;
        w.huff(uint32_t(di), 5);
        w.put(uint32_t(int(dist) - dbase[di]), dext[di]);
        for (int k = 1; k < best; ++k)
            if (i + size_t(k) + 3 <= d.size()) head[size_t(hash(i + size_t(k)))] = int64_t(i + size_t(k));
        i += size_t(best);
    }
    fixed_literal(w, 256);
    return w.finish();
}

Bytes deflate_stored(const Bytes& d) {
    Bytes out;
    size_t i = 0;
    do {
        size_t n = std::min<size_t>(65535, d.size() - i);
        out.push_back(i + n == d.size() ? 1 : 0);
        out.push_back(uint8_t(n));
        out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(~n));
        out.push_back(uint8_t(~n >> 8));
        out.insert(out.end(), d.begin() + long(i), d.begin() + long(i + n));
        i += n;
    } while (i < d.size());
    return out;
}

InflateResult inflate_chunked(const Bytes& src, size_t chunk, Bytes& out) {
    size_t pos = 0;
    out.clear();
    return inflate_stream(
        [&](uint8_t* b, size_t cap) {
            size_t n = std::min({cap, chunk, src.size() - pos});
            std::memcpy(b, src.data() + pos, n);
            pos += n;
            return n;
        },
        [&](const uint8_t* d, size_t n) { out.insert(out.end(), d, d + n); return true; });
}

Bytes pseudo_random(size_t n, uint32_t seed, int alphabet) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        b[i] = uint8_t((seed >> 24) % uint32_t(alphabet));
    }
    return b;
}

void test_inflate() {
    // Dynamic block, any input chunking, same as puff.
    const Bytes text = words_text(), dyn = from_hex(kDynamicHex, sizeof kDynamicHex / sizeof *kDynamicHex);
    std::vector<uint8_t> puffed;
    CHECK(x2::import::inflate_raw(dyn.data(), dyn.size(), text.size(), puffed) && puffed == text);
    for (size_t chunk : {size_t(1), size_t(7), size_t(1) << 16}) {
        Bytes out;
        CHECK(inflate_chunked(dyn, chunk, out) == InflateResult::Done);
        CHECK(out == text);
    }
    // Fixed blocks with long matches across the 64 KB output window, and stored blocks.
    Bytes big = pseudo_random(300000, 7, 6);
    for (size_t i = 100000; i < 200000; ++i) big[i] = big[i - 32768]; // matches at the maximum distance
    const Bytes fixed = deflate_fixed(big), stored = deflate_stored(big);
    CHECK(fixed.size() < big.size());
    for (const Bytes* s : {&fixed, &stored}) {
        Bytes out;
        CHECK(inflate_chunked(*s, 1000, out) == InflateResult::Done && out == big);
        std::vector<uint8_t> p;
        CHECK(x2::import::inflate_raw(s->data(), s->size(), big.size(), p) && p == big);
    }
    // Truncated input; corrupt input never crashes and never reports success with the wrong data.
    Bytes out;
    CHECK(inflate_chunked(Bytes(fixed.begin(), fixed.begin() + long(fixed.size() / 2)), 4096, out) == InflateResult::Truncated);
    CHECK(inflate_chunked(Bytes(), 1, out) == InflateResult::Truncated);
    CHECK(inflate_chunked(Bytes{0x07}, 1, out) == InflateResult::Corrupt); // block type 3
    int wrong = 0;
    for (int k = 0; k < 200; ++k) {
        Bytes bad = dyn;
        bad[size_t(k * 11) % bad.size()] ^= uint8_t(1u << (k % 8));
        InflateResult r = inflate_chunked(bad, 333, out);
        if (r == InflateResult::Done && out == text) ++wrong; // a flipped bit cannot decode to the same text
    }
    CHECK(wrong == 0);
    // The sink can stop early.
    size_t seen = 0;
    size_t pos = 0;
    InflateResult r = inflate_stream(
        [&](uint8_t* b, size_t cap) { size_t n = std::min(cap, stored.size() - pos); std::memcpy(b, stored.data() + pos, n); pos += n; return n; },
        [&](const uint8_t*, size_t n) { seen += n; return seen < 50000; });
    CHECK(r == InflateResult::Stopped && seen >= 50000 && seen < 120000);
    // CRC-32 matches the importer's.
    CHECK(crc32_update(0, big.data(), big.size()) == x2::import::crc32(big.data(), big.size()));
    CHECK(crc32_update(crc32_update(0, big.data(), 1000), big.data() + 1000, big.size() - 1000) ==
          x2::import::crc32(big.data(), big.size()));
}

// ---- synthetic CDTV disc ----
const char* kTrackName = "Synthetic CDTV (Track %02d).bin";

std::string track_file(int n) {
    char b[64];
    std::snprintf(b, sizeof b, kTrackName, n);
    return b;
}

Bytes data_track(const char* volume) {
    Bytes t(size_t(kSectorBytes) * 20, 0);
    for (int s = 0; s < 20; ++s) {
        uint8_t* p = &t[size_t(s) * kSectorBytes];
        p[0] = 0;
        std::memset(p + 1, 0xFF, 10);
        p[11] = 0;
        p[15] = 1;
    }
    uint8_t* pvd = &t[size_t(kSectorBytes) * 16 + 16];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    std::memset(pvd + 8, ' ', 64);
    std::memcpy(pvd + 8, "CDTV", 4);
    std::memcpy(pvd + 40, volume, std::strlen(volume));
    return t;
}

// Audio track n: sector 0 = the previous track's last sector (non-zero) except for tracks 12 and 13, 150 more
// sectors of silence, then `prog` sectors of programme (track 12/13: programme from sector 150).
Bytes audio_track(int n, uint32_t prog) {
    const uint32_t start = n >= 12 ? 150 : 151;
    Bytes t(size_t(kSectorBytes) * (start + prog), 0);
    if (n < 12)
        for (uint32_t i = 0; i < kSectorBytes; ++i) t[i] = uint8_t(i * 7 + 3);
    uint32_t x = uint32_t(n) * 2654435761u;
    for (size_t i = size_t(start) * kSectorBytes; i < t.size(); ++i) {
        x = x * 1664525u + 1013904223u;
        t[i] = uint8_t(x >> 24);
    }
    // The first programme byte is non-zero on purpose.
    t[size_t(start) * kSectorBytes] |= 1;
    return t;
}

std::string cue_text(int last_track = 13) {
    std::string c = "FILE \"" + track_file(1) + "\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n";
    for (int n = 2; n <= last_track; ++n)
        c += "FILE \"" + track_file(n) + "\" BINARY\n  TRACK " + (n < 10 ? "0" : "") + std::to_string(n) +
             " AUDIO\r\n    INDEX 00 00:00:00\n    INDEX 01 00:01:6" + (n < 5 ? "2" : "3") + "\n";
    return c;
}

void write_file(const fs::path& p, const Bytes& d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}
void write_text(const fs::path& p, const std::string& s) { write_file(p, Bytes(s.begin(), s.end())); }
Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

constexpr uint32_t kProg = 80; // sectors of programme per track: 47040 frames, just over a second

struct DiscFiles {
    std::vector<std::pair<std::string, Bytes>> files; // name, contents (cue first)
};

DiscFiles make_disc(const char* volume = "XENONII", int last_track = 13, uint32_t short_track = 0) {
    DiscFiles d;
    d.files.push_back({"Synthetic CDTV.cue", Bytes()});
    std::string cue = cue_text(last_track);
    d.files[0].second.assign(cue.begin(), cue.end());
    d.files.push_back({track_file(1), data_track(volume)});
    for (int n = 2; n <= last_track; ++n) d.files.push_back({track_file(n), audio_track(n, uint32_t(n) == short_track ? 10 : kProg)});
    return d;
}

void write_folder(const fs::path& dir, const DiscFiles& d, const std::string& skip = "") {
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (auto& [name, data] : d.files)
        if (name != skip) write_file(dir / name, data);
}

// ZIP writer: method 0 (stored) or 8 (fixed-Huffman deflate) per member.
Bytes make_zip(const DiscFiles& d, bool deflate, const std::string& skip = "", uint32_t fake_usize_for_track2 = 0) {
    Bytes z, cd;
    auto le16 = [](Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
    auto le32 = [&](Bytes& b, uint32_t v) { le16(b, v & 0xFFFF); le16(b, v >> 16); };
    uint16_t count = 0;
    for (auto& [name, data] : d.files) {
        if (name == skip) continue;
        const Bytes payload = deflate ? deflate_fixed(data) : data;
        const uint32_t crc = crc32_update(0, data.data(), data.size());
        uint32_t usize = uint32_t(data.size());
        if (fake_usize_for_track2 && name == track_file(2)) usize = fake_usize_for_track2;
        const uint32_t local = uint32_t(z.size());
        le32(z, 0x04034b50); le16(z, 20); le16(z, 0); le16(z, deflate ? 8 : 0); le32(z, 0);
        le32(z, crc); le32(z, uint32_t(payload.size())); le32(z, usize); le16(z, uint16_t(name.size())); le16(z, 0);
        z.insert(z.end(), name.begin(), name.end());
        z.insert(z.end(), payload.begin(), payload.end());
        le32(cd, 0x02014b50); le16(cd, 20); le16(cd, 20); le16(cd, 0); le16(cd, deflate ? 8 : 0); le32(cd, 0);
        le32(cd, crc); le32(cd, uint32_t(payload.size())); le32(cd, usize); le16(cd, uint16_t(name.size()));
        le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, local);
        cd.insert(cd.end(), name.begin(), name.end());
        ++count;
    }
    const uint32_t cd_off = uint32_t(z.size());
    z.insert(z.end(), cd.begin(), cd.end());
    le32(z, 0x06054b50); le16(z, 0); le16(z, 0); le16(z, count); le16(z, count);
    le32(z, uint32_t(cd.size())); le32(z, cd_off); le16(z, 0);
    return z;
}

bool has_message(const ImportReport& r, const std::string& part) {
    for (const std::string& m : r.messages)
        if (m.find(part) != std::string::npos) return true;
    return false;
}

void test_cue_and_rules() {
    std::vector<CueTrack> t;
    std::string why;
    CHECK(parse_cue(cue_text(), t, why));
    CHECK(t.size() == 13 && !t[0].audio && t[0].mode == "MODE1/2352" && t[1].audio && t[12].number == 13);
    CHECK(t[1].index0 == 0 && t[1].index1 == 137 && t[5].index1 == 138 && t[1].file == track_file(2));
    CHECK(!parse_cue("TRACK 01 AUDIO\n", t, why));                                       // before FILE
    CHECK(!parse_cue("FILE \"a.bin\" BINARY\nTRACK 01 AUDIO\n", t, why));                // no INDEX 01
    CHECK(!parse_cue("FILE \"a.bin\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:61:00\n", t, why));
    CHECK(!parse_cue("FILE \"a.bin\" BINARY\nTRACK 02 AUDIO\nINDEX 01 0\nTRACK 01 AUDIO\n", t, why));
    CHECK(!parse_cue("", t, why));
    // Programme start: past the previous track's sector and the silent pregap, not at INDEX 01.
    const Bytes a = audio_track(5, 100), b = audio_track(12, 100);
    CHECK(find_programme_start(a.data(), a.size(), 137) == 151);
    CHECK(find_programme_start(b.data(), b.size(), 137) == 150);
    CHECK(find_programme_start(a.data(), a.size(), 0) == 151); // sector 0 is never the start
    Bytes silent(size_t(kSectorBytes) * 300, 0);
    CHECK(find_programme_start(silent.data(), silent.size(), 137) == 137); // nothing found: the CUE's value
    Bytes late = audio_track(12, 100);
    late.insert(late.begin(), size_t(kSectorBytes) * 100, 0); // music only after the search window
    CHECK(find_programme_start(late.data(), late.size(), 0) == 0);
    std::string vol;
    const Bytes d = data_track("XENONII"), other = data_track("OTHERDISC");
    CHECK(identify_data_track(d.data(), d.size(), vol, why) && vol == "XENONII");
    CHECK(!identify_data_track(other.data(), other.size(), vol, why) && vol == "OTHERDISC");
    CHECK(!identify_data_track(d.data(), size_t(kSectorBytes) * 10, vol, why));
    Bytes nosync = d;
    nosync[size_t(kSectorBytes) * 16 + 3] = 0;
    CHECK(!identify_data_track(nosync.data(), nosync.size(), vol, why));
}

void check_imported(const Disc& disc, const DiscFiles& files) {
    CHECK(disc.tracks.size() == 12);
    for (const TrackSource& t : disc.tracks) {
        const uint32_t start = t.number >= 12 ? 150 : 151;
        CHECK(t.start_sector == start && t.frames == uint64_t(kProg) * kFramesPerSector);
        // The PCM is the programme part of the .bin, byte for byte.
        PcmReader r;
        CHECK(r.open(t));
        std::vector<int16_t> pcm(size_t(t.frames) * 2);
        r.read(0, size_t(t.frames), pcm.data());
        const Bytes& bin = files.files[size_t(t.number)].second;
        bool same = true;
        for (size_t i = 0; i < pcm.size() && same; ++i)
            same = pcm[i] == int16_t(uint16_t(bin[size_t(start) * kSectorBytes + i * 2] | bin[size_t(start) * kSectorBytes + i * 2 + 1] << 8));
        CHECK(same);
    }
}

void test_import(const fs::path& work) {
    const DiscFiles disc = make_disc();
    const fs::path folder = work / "disc", out = work / "imported";
    write_folder(folder, disc);
    CHECK(looks_like_disc_image(folder.string()));
    CHECK(looks_like_disc_image((folder / "Synthetic CDTV.cue").string()));
    // Folder -> import.
    fs::remove_all(out);
    ImportReport r = import_disc(folder.string(), out.string());
    for (auto& m : r.messages) std::printf("  %s\n", m.c_str());
    CHECK(r.status == Status::Ok);
    CHECK(has_message(r, "track 2: programme from sector 151 (CUE INDEX 01 at 137)"));
    CHECK(has_message(r, "track 13: programme from sector 150"));
    CHECK(fs::exists(out / "manifest.txt") && fs::exists(out / "track02.pcm") && !fs::exists(out / "track02.pcm.tmp"));
    Disc d;
    std::string why;
    CHECK(open_imported(out.string(), d, why, true));
    check_imported(d, disc);
    CHECK(open_disc(out.string(), d, why) && d.imported);
    // The .cue in place: same audio, nothing copied.
    Disc inplace;
    CHECK(open_in_place((folder / "Synthetic CDTV.cue").string(), inplace, why));
    check_imported(inplace, disc);
    CHECK(!inplace.imported && inplace.tracks[0].offset == 151u * kSectorBytes);
    CHECK(open_disc(folder.string(), inplace, why) && !inplace.imported);
    // ZIP, stored and deflated: the same PCM files (by CRC) as the folder import.
    for (bool deflate : {false, true}) {
        const fs::path zip = work / (deflate ? "disc_deflate.zip" : "disc_stored.zip"), zout = work / "imported_zip";
        write_file(zip, make_zip(disc, deflate));
        CHECK(looks_like_disc_image(zip.string()));
        fs::remove_all(zout);
        ImportReport z = import_disc(zip.string(), zout.string());
        CHECK(z.status == Status::Ok);
        Disc zd;
        CHECK(open_imported(zout.string(), zd, why, true));
        CHECK(zd.tracks.size() == 12);
        for (size_t i = 0; i < zd.tracks.size() && i < d.tracks.size(); ++i) CHECK(zd.tracks[i].crc == d.tracks[i].crc);
        CHECK(!open_in_place(zip.string(), zd, why) && why.find("--import-cdtv") != std::string::npos);
    }
    // Manifest checks: a changed byte in the first MB, a truncated file, another version.
    Bytes pcm = read_file(out / "track05.pcm");
    Bytes changed = pcm;
    changed[1000] ^= 0x40;
    write_file(out / "track05.pcm", changed);
    CHECK(!open_imported(out.string(), d, why) && why.find("changed") != std::string::npos);
    write_file(out / "track05.pcm", Bytes(pcm.begin(), pcm.end() - 4));
    CHECK(!open_imported(out.string(), d, why));
    write_file(out / "track05.pcm", pcm);
    CHECK(open_imported(out.string(), d, why));
    std::string man;
    {
        Bytes m = read_file(out / "manifest.txt");
        man.assign(m.begin(), m.end());
    }
    write_text(out / "manifest.txt", "x2recomp-cdtv 99\n");
    CHECK(!open_imported(out.string(), d, why) && why.find("version") != std::string::npos);
    std::string evil = man;
    evil.replace(evil.find("track02.pcm"), 11, "../evil.pcm");
    write_text(out / "manifest.txt", evil);
    CHECK(!open_imported(out.string(), d, why)); // no paths outside the directory
    write_text(out / "manifest.txt", man);
    CHECK(open_imported(out.string(), d, why));
    // A failed re-import leaves no valid manifest behind (never a mix of old and new tracks).
    write_folder(work / "broken", make_disc("XENONII", 13, 7));
    ImportReport br = import_disc((work / "broken").string(), out.string());
    CHECK(br.status == Status::BadInput && has_message(br, "less than one second"));
    CHECK(!open_imported(out.string(), d, why));
    CHECK(!fs::exists(out / "track07.pcm.tmp"));
}

void test_import_errors(const fs::path& work) {
    const DiscFiles disc = make_disc();
    const fs::path out = work / "err_out";
    std::string why;
    // Missing track file.
    write_folder(work / "missing", disc, track_file(7));
    ImportReport r = import_disc((work / "missing").string(), out.string());
    CHECK(r.status == Status::BadInput && has_message(r, "track 7 file missing"));
    write_file(work / "missing.zip", make_zip(disc, true, track_file(9)));
    r = import_disc((work / "missing.zip").string(), out.string());
    CHECK(r.status == Status::BadInput && has_message(r, "track 9 file missing"));
    // Corrupted ZIP member: CRC mismatch or a deflate error, never a silent import.
    for (bool deflate : {false, true}) {
        Bytes z = make_zip(disc, deflate);
        // Damage the middle of track 4's member data.
        const std::string name = track_file(4);
        size_t at = size_t(std::search(z.begin(), z.end(), name.begin(), name.end()) - z.begin()) + name.size();
        z[at + 50000] ^= 0x10;
        write_file(work / "corrupt.zip", z);
        r = import_disc((work / "corrupt.zip").string(), out.string());
        CHECK(r.status == Status::BadInput && (has_message(r, "CRC-32") || has_message(r, "deflate") || has_message(r, "more data")));
    }
    // Truncated archive.
    Bytes z = make_zip(disc, false);
    write_file(work / "truncated.zip", Bytes(z.begin(), z.begin() + long(z.size() / 2)));
    r = import_disc((work / "truncated.zip").string(), out.string());
    CHECK(r.status != Status::Ok);
    // Short track.
    write_folder(work / "short", make_disc("XENONII", 13, 3));
    r = import_disc((work / "short").string(), out.string());
    CHECK(r.status == Status::BadInput && has_message(r, "track 3: less than one second"));
    // Not the Xenon 2 CDTV disc: other volume, too few tracks, no data track, no .cue at all.
    write_folder(work / "other", make_disc("OTHERDISC"));
    r = import_disc((work / "other").string(), out.string());
    CHECK(r.status == Status::Unsupported && has_message(r, "not the Xenon 2 CDTV disc"));
    write_folder(work / "eleven", make_disc("XENONII", 12));
    r = import_disc((work / "eleven").string(), out.string());
    CHECK(r.status == Status::Unsupported && has_message(r, "expected audio tracks 2-13"));
    DiscFiles audio_only = disc;
    std::string cue = cue_text();
    cue = cue.substr(cue.find("FILE", 5));
    audio_only.files[0].second.assign(cue.begin(), cue.end());
    write_folder(work / "audio_only", audio_only);
    r = import_disc((work / "audio_only").string(), out.string());
    CHECK(r.status == Status::Unsupported);
    fs::create_directories(work / "nocue");
    write_file(work / "nocue" / "x.adf", Bytes(100, 0));
    CHECK(!looks_like_disc_image((work / "nocue").string()));
    r = import_disc((work / "nocue").string(), out.string());
    CHECK(r.status == Status::Unsupported);
    DiscFiles plain;
    plain.files.push_back({"game.adf", Bytes(1000, 1)});
    write_file(work / "plain.zip", make_zip(plain, false));
    CHECK(!looks_like_disc_image((work / "plain.zip").string()));
    write_file(work / "garbage.zip", pseudo_random(5000, 3, 256));
    r = import_disc((work / "garbage.zip").string(), out.string());
    CHECK(r.status == Status::Unsupported && has_message(r, "not a ZIP"));
    r = import_disc((work / "does-not-exist").string(), out.string());
    CHECK(r.status == Status::BadInput && has_message(r, "not found"));
    // Sizes beyond a CD: a directory entry claiming 2 GB is refused before anything is read; a member that
    // inflates to more than its directory entry says is refused while streaming.
    write_file(work / "huge.zip", make_zip(disc, true, "", 0x80000000u));
    r = import_disc((work / "huge.zip").string(), out.string());
    CHECK(r.status == Status::BadInput && has_message(r, "larger than any CD track"));
    write_file(work / "bomb.zip", make_zip(disc, true, "", 1000));
    r = import_disc((work / "bomb.zip").string(), out.string());
    CHECK(r.status == Status::BadInput && has_message(r, "more data than the ZIP directory says"));
    // Analysis only writes nothing.
    write_folder(work / "analyse", disc);
    fs::remove_all(work / "analyse_out");
    r = import_disc((work / "analyse").string(), (work / "analyse_out").string(), true);
    CHECK(r.status == Status::Ok && r.disc.tracks.size() == 12 && !fs::exists(work / "analyse_out"));
}

// ---- CD player ----

// A synthetic track file: frames of a function, written as s16le stereo PCM.
TrackSource synth_track(const fs::path& p, uint64_t frames, double (*f)(uint64_t, int)) {
    Bytes d(size_t(frames) * 4);
    for (uint64_t i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c) {
            const int16_t v = int16_t(std::lround(f(i, c)));
            d[size_t(i) * 4 + size_t(c) * 2] = uint8_t(uint16_t(v));
            d[size_t(i) * 4 + size_t(c) * 2 + 1] = uint8_t(uint16_t(v) >> 8);
        }
    write_file(p, d);
    TrackSource t;
    t.number = 2;
    t.path = p.string();
    t.frames = frames;
    return t;
}

double noise(uint64_t i, int c) {
    uint32_t x = uint32_t(i * 2 + uint64_t(c)) * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return double(int(x % 40001) - 20000);
}

std::vector<int16_t> render(CdPlayer& p, size_t frames) {
    std::vector<int16_t> out(frames * 2, 0);
    p.render(out.data(), frames);
    return out;
}

void test_player(const fs::path& work) {
    const uint64_t n = 47040;
    TrackSource t = synth_track(work / "noise.pcm", n, noise);
    // 44.1 kHz output: an exact copy at the CD gain.
    CdPlayer p;
    p.configure(kSampleRate, 0);
    p.play(&t, 2);
    CHECK(p.track() == 2);
    std::vector<int16_t> o = render(p, 1000);
    bool exact = true;
    for (size_t i = 0; i < 1000 && exact; ++i)
        for (int c = 0; c < 2; ++c) exact = o[i * 2 + size_t(c)] == int16_t(std::lround(noise(i, c) * CdPlayer::kCdGain));
    CHECK(exact);
    // The CDTV loop: the track ends, silence until the next 50-tick poll, then it plays again from the start.
    CdPlayer q;
    q.configure(kSampleRate, 0);
    q.play(&t, 2);
    std::vector<int16_t> all;
    for (int frame = 0; frame < 110; ++frame) {
        std::vector<int16_t> f = render(q, 882);
        all.insert(all.end(), f.begin(), f.end());
        q.tick();
    }
    bool silent = true;
    for (size_t i = n; i < 100 * 882; ++i) silent = silent && all[i * 2] == 0 && all[i * 2 + 1] == 0;
    CHECK(silent);
    CHECK(q.loops() == 1);
    CHECK(all[100 * 882 * 2] == int16_t(std::lround(noise(0, 0) * CdPlayer::kCdGain)));
    CHECK(all[(100 * 882 + 5) * 2 + 1] == int16_t(std::lround(noise(5, 1) * CdPlayer::kCdGain)));
    // Crossfaded loop: no gap, the restart overlaps the end.
    CdPlayer x;
    x.configure(kSampleRate, 100);
    x.play(&t, 2);
    std::vector<int16_t> xs = render(x, size_t(n) + 20000);
    double min_rms = 1e9;
    for (size_t w = 0; w + 441 <= xs.size() / 2; w += 441) {
        double s = 0;
        for (size_t i = w; i < w + 441; ++i) s += double(xs[i * 2]) * xs[i * 2];
        min_rms = std::min(min_rms, std::sqrt(s / 441));
    }
    CHECK(min_rms > 5000); // noise at 0.75 x ~11500 rms; never a silent window
    CHECK(x.loops() == 1 && x.position() > 15000);
    // Volume: 50% after the 10 ms ramp.
    CdPlayer v;
    v.configure(kSampleRate, 0);
    v.set_volume(50);
    v.play(&t, 2);
    std::vector<int16_t> vo = render(v, 2000);
    CHECK(vo[1500 * 2] == int16_t(std::lround(noise(1500, 0) * CdPlayer::kCdGain * 0.5)));
    // Stop fades out within 5 ms, then silence.
    v.stop();
    CHECK(v.track() == 0);
    std::vector<int16_t> so = render(v, 1000);
    bool quiet = true;
    for (size_t i = 300; i < 1000; ++i) quiet = quiet && so[i * 2] == 0;
    CHECK(quiet && std::abs(so[0]) > 0);
    // Pause: stop holds the position and outputs nothing; duck plays at a quarter.
    CdPlayer h;
    h.configure(kSampleRate, 0);
    h.set_pause_mode(PauseMode::Stop);
    h.play(&t, 2);
    render(h, 100);
    h.set_game_paused(true);
    std::vector<int16_t> ho = render(h, 500);
    CHECK(std::all_of(ho.begin(), ho.end(), [](int16_t s) { return s == 0; }) && h.position() == 100);
    h.set_game_paused(false);
    ho = render(h, 1);
    CHECK(ho[0] == int16_t(std::lround(noise(100, 0) * CdPlayer::kCdGain)));
    CdPlayer dk;
    dk.configure(kSampleRate, 0);
    dk.set_pause_mode(PauseMode::Duck);
    dk.play(&t, 2);
    dk.set_game_paused(true);
    std::vector<int16_t> dko = render(dk, 2000);
    CHECK(dko[1800 * 2] == int16_t(std::lround(noise(1800, 0) * CdPlayer::kCdGain * 0.25)));
    // Mixing saturates and counts.
    CdPlayer m;
    m.configure(kSampleRate, 0);
    m.play(&t, 2);
    std::vector<int16_t> buf(200, 30000);
    m.render(buf.data(), 100);
    bool sat = false;
    for (int16_t s : buf) sat = sat || s == 32767 || s == -32768;
    CHECK(sat && m.clipped() > 0);
    // Resampling 44.1 -> 48 kHz: a 1 kHz sine comes out at -60 dB error or better; 15 kHz keeps its level.
    for (double hz : {1000.0, 15000.0}) {
        static double g_hz;
        g_hz = hz;
        TrackSource s = synth_track(work / "sine.pcm", 48000, [](uint64_t i, int) { return 16000.0 * std::sin(2 * std::numbers::pi * g_hz * double(i) / 44100.0); });
        CdPlayer r;
        r.configure(48000, 0);
        r.play(&s, 2);
        std::vector<int16_t> ro = render(r, 40000);
        double err = 0, sig = 0, peak = 0;
        for (size_t i = 1000; i < 39000; ++i) {
            const double want = 16000.0 * CdPlayer::kCdGain * std::sin(2 * std::numbers::pi * hz * double(i) / 48000.0);
            err += (ro[i * 2] - want) * (ro[i * 2] - want);
            sig += want * want;
            peak = std::max(peak, std::fabs(double(ro[i * 2])));
        }
        const double snr = 10 * std::log10(sig / err);
        std::printf("  resample %.0f Hz: SNR %.1f dB, peak %.0f (ideal %.0f)\n", hz, snr, peak, 16000.0 * CdPlayer::kCdGain);
        CHECK(snr > (hz < 5000 ? 60 : 40));
        CHECK(std::fabs(peak / (16000.0 * CdPlayer::kCdGain) - 1.0) < 0.06);
    }
    // Determinism: the same calls give the same samples whatever the render block sizes.
    CdPlayer a, b;
    for (CdPlayer* pl : {&a, &b}) { pl->configure(48000, 0); pl->play(&t, 2); }
    std::vector<int16_t> one = render(a, 5000), parts;
    for (size_t k : {size_t(1), size_t(999), size_t(4000)}) {
        std::vector<int16_t> f = render(b, k);
        parts.insert(parts.end(), f.begin(), f.end());
    }
    CHECK(one == parts);
}

// ---- Paula per-source gains ----
void test_paula_sources() {
    struct Rig {
        std::vector<uint8_t> ram = std::vector<uint8_t>(0x80000, 0);
        x2::Paula p;
        bool effect = false;
        explicit Rig(bool query) {
            for (size_t i = 0; i < 0x2000; ++i) ram[0x1000 + i] = uint8_t(int8_t(std::lround(100 * std::sin(double(i) * 0.3))));
            for (size_t i = 0; i < 0x2000; ++i) ram[0x4000 + i] = uint8_t(int8_t(std::lround(90 * std::sin(double(i) * 0.05))));
            p.attach(ram.data(), 0x7FFFF, [](uint16_t) {});
            p.configure(x2::PaulaConfig{});
            p.set_output_enabled(true);
            if (query) p.set_writer_query([this]() { return effect ? x2::AudioSource::Effect : x2::AudioSource::Music; });
        }
        void setup(int ch, uint32_t lc, uint16_t per) {
            const uint16_t base = uint16_t(0xA0 + 16 * ch);
            p.write_reg(base, uint16_t(lc >> 16));
            p.write_reg(base + 2, uint16_t(lc));
            p.write_reg(base + 4, 0x1000);
            p.write_reg(base + 6, per);
            p.write_reg(base + 8, 64);
        }
        std::vector<int16_t> run(int gm, int ge, int channels) {
            p.set_source_gains(gm, ge);
            if (channels & 1) setup(0, 0x1000, 300); // music, left
            effect = true;
            if (channels & 2) setup(3, 0x4000, 200); // effect, also left
            effect = false;
            p.set_dmacon(uint16_t(0x200 | ((channels & 1) ? 1 : 0) | ((channels & 2) ? 8 : 0)));
            p.run_until(3546895 / 10);
            return p.output();
        }
    };
    const std::vector<int16_t> plain = Rig(false).run(256, 256, 3);
    CHECK(Rig(true).run(256, 256, 3) == plain);        // gains at unity: bit-identical
    const std::vector<int16_t> music_only = Rig(true).run(256, 256, 1), effect_only = Rig(true).run(256, 256, 2);
    CHECK(Rig(true).run(0, 256, 3) == effect_only);    // music muted, effects untouched
    CHECK(Rig(true).run(256, 0, 3) == music_only);     // and the other way round
    Rig r(true);
    r.run(256, 256, 3);
    CHECK(r.p.channel_source(0) == x2::AudioSource::Music && r.p.channel_source(3) == x2::AudioSource::Effect);
    // Half gain on music: the music part of the mix halves (within rounding of the filters).
    const std::vector<int16_t> half = Rig(true).run(128, 256, 3);
    double worst = 0;
    for (size_t i = 0; i < half.size(); ++i) worst = std::max(worst, std::fabs(half[i] - (music_only[i] * 0.5 + effect_only[i])));
    CHECK(worst <= 2.0);
}

void test_settings_and_names() {
    x2::Settings s, r;
    std::string err;
    CHECK(x2::parse_settings("music = mix\nmusic_volume = 40\nsfx_volume = 70\ncd_crossfade = 250\ncd_pause = duck\n"
                             "cdtv = /x/cdtv\ncdtv_tracks = 2,4,6,7,12,13\n", s, err) && err.empty());
    CHECK(s.music == "mix" && s.music_volume == 40 && s.sfx_volume == 70 && s.cd_crossfade == 250 && s.cd_pause == "duck" &&
          s.cdtv == "/x/cdtv" && s.cdtv_tracks == "2,4,6,7,12,13");
    CHECK(x2::parse_settings(x2::format_settings(s), r, err) && err.empty());
    CHECK(r.music == s.music && r.music_volume == 40 && r.sfx_volume == 70 && r.cd_crossfade == 250 && r.cd_pause == "duck" &&
          r.cdtv == s.cdtv && r.cdtv_tracks == s.cdtv_tracks);
    x2::Settings d;
    CHECK(x2::parse_settings(x2::format_settings(d), r, err) && r.music == "amiga" && r.cdtv.empty() && r.cdtv_tracks == "2,3,5,2,3,5");
    err.clear();
    CHECK(!x2::parse_settings("music = cd\ncd_crossfade = 900\ncd_pause = mute\ncdtv_tracks = 1,2,3,4,5,6\nsfx_volume = -1\n", r, err));
    CHECK(std::count(err.begin(), err.end(), '\n') == 5);
    std::array<int, 6> t{};
    CHECK(x2::parse_cdtv_tracks("2, 3,5,2,3,5", t) && t[2] == 5);
    CHECK(!x2::parse_cdtv_tracks("2,3,5,2,3", t) && !x2::parse_cdtv_tracks("2,3,5,2,3,5,6", t) && !x2::parse_cdtv_tracks("2,3,5,2,3,14", t));
    MusicSource ms;
    CHECK(parse_music_source("cdtv", ms) && ms == MusicSource::Cdtv && !parse_music_source("CD", ms));
    PauseMode pm;
    CHECK(parse_pause_mode("stop", pm) && pm == PauseMode::Stop && !parse_pause_mode("", pm));
    CHECK(std::string(track_title(2)).find("Xenon II Mix") != std::string::npos && std::string(track_title(14)).empty());
    MusicMap map;
    CHECK(map.track[0] == 2 && map.track[1] == 3 && map.track[2] == 5 && map.track[3] == 2 && map.track[4] == 3 && map.track[5] == 5);
}

} // namespace

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x2_cdtv_tests";
    fs::remove_all(work);
    fs::create_directories(work);
    test_inflate();
    test_cue_and_rules();
    test_import(work);
    test_import_errors(work);
    test_player(work);
    test_paula_sources();
    test_settings_and_names();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
