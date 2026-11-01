// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_png.h"

#include "import/importer.h"
#include "import/sha1.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace x2::pack {

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
} // namespace

uint32_t premultiply(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    auto m = [a](uint8_t c) { return uint32_t((unsigned(c) * a + 127) / 255); };
    return m(r) | m(g) << 8 | m(b) << 16 | uint32_t(a) << 24;
}

bool png_size(const uint8_t* d, size_t n, int& w, int& h, std::string& why) {
    if (n < 8 || std::memcmp(d, kSig, 8) != 0) { why = "not a PNG file"; return false; }
    if (n < 8 + 12 + 13 || be32(d + 8) != 13 || std::memcmp(d + 12, "IHDR", 4) != 0) { why = "IHDR is not the first chunk"; return false; }
    if (x2::import::crc32(d + 12, 17) != be32(d + 8 + 8 + 13)) { why = "chunk CRC mismatch (corrupted file)"; return false; }
    const uint32_t pw = be32(d + 16), ph = be32(d + 20);
    if (!pw || !ph || pw > 0x7FFFFFFFu || ph > 0x7FFFFFFFu) { why = "picture too large or empty (" + std::to_string(pw) + "x" + std::to_string(ph) + ")"; return false; }
    w = int(pw);
    h = int(ph);
    return true;
}

bool decode_png(const uint8_t* fd, size_t fn, Picture& out, std::string& why, int max_side, size_t max_pixels) {
    const uint8_t* const f = fd;
    const size_t fsize = fn;
    if (fsize < 8 || std::memcmp(f, kSig, 8) != 0) { why = "not a PNG file"; return false; }
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, interlace = 0;
    std::vector<uint8_t> idat, plte, trns;
    bool ihdr = false, iend = false;
    for (size_t p = 8; p + 12 <= fsize && !iend;) {
        const uint32_t len = be32(&f[p]);
        if (len > fsize - p - 12) { why = "truncated chunk"; return false; }
        const uint8_t* type = &f[p + 4];
        const uint8_t* d = &f[p + 8];
        if (x2::import::crc32(type, len + 4) != be32(d + len)) { why = "chunk CRC mismatch (corrupted file)"; return false; }
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len != 13) { why = "bad IHDR"; return false; }
            w = be32(d);
            h = be32(d + 4);
            depth = d[8];
            ctype = d[9];
            interlace = d[12];
            if (d[10] || d[11]) { why = "unknown compression or filter method"; return false; }
            // Size limits before anything is inflated.
            if (!w || !h || w > uint32_t(max_side) || h > uint32_t(max_side) || size_t(w) * h > max_pixels) {
                why = "picture too large or empty (" + std::to_string(w) + "x" + std::to_string(h) + ", limit " +
                      std::to_string(max_side) + " per side, " + std::to_string(max_pixels) + " pixels)";
                return false;
            }
            ihdr = true;
        } else if (!ihdr) {
            why = "IHDR is not the first chunk";
            return false;
        } else if (!std::memcmp(type, "PLTE", 4)) plte.assign(d, d + len);
        else if (!std::memcmp(type, "tRNS", 4)) trns.assign(d, d + len);
        else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), d, d + len);
        else if (!std::memcmp(type, "IEND", 4)) iend = true;
        p += 12 + len;
    }
    if (!ihdr || idat.empty()) { why = "no image data"; return false; }
    if (interlace) { why = "interlaced PNGs are not supported"; return false; }
    int channels = 0;
    switch (ctype) {
    case 0: channels = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) channels = 0; break;
    case 2: channels = 3; if (depth != 8 && depth != 16) channels = 0; break;
    case 3: channels = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8) channels = 0; break;
    case 4: channels = 2; if (depth != 8 && depth != 16) channels = 0; break;
    case 6: channels = 4; if (depth != 8 && depth != 16) channels = 0; break;
    default: break;
    }
    if (!channels) { why = "unsupported colour type / bit depth"; return false; }
    if (ctype == 3 && (plte.empty() || plte.size() % 3)) { why = "palette missing"; return false; }
    const size_t bits = size_t(channels) * size_t(depth), row = (size_t(w) * bits + 7) / 8, bpp = std::max<size_t>(1, bits / 8);
    // zlib stream: CM 8, no preset dictionary.
    if (idat.size() < 6 || (idat[0] & 0x0F) != 8 || ((idat[0] << 8) | idat[1]) % 31 || (idat[1] & 0x20)) {
        why = "bad zlib stream";
        return false;
    }
    // Deflate cannot expand more than 1032:1: a header claiming more rows than the data can fill is refused before the
    // output buffer is allocated.
    if (size_t(h) * (row + 1) / 1032 > idat.size() + 64) { why = "corrupted image data"; return false; }
    std::vector<uint8_t> raw;
    if (!x2::import::inflate_raw(&idat[2], idat.size() - 6, size_t(h) * (row + 1), raw)) { why = "corrupted image data"; return false; }
    // Undo the row filters in place.
    std::vector<uint8_t> prev(row, 0);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* r = &raw[size_t(y) * (row + 1)];
        const uint8_t ft = r[0];
        uint8_t* c = r + 1;
        for (size_t i = 0; i < row; ++i) {
            const int a = i >= bpp ? c[i - bpp] : 0, b = prev[i], d = i >= bpp ? prev[i - bpp] : 0;
            int v = c[i];
            switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int p = a + b - d, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - d);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : d;
                break;
            }
            default: why = "bad row filter"; return false;
            }
            c[i] = uint8_t(v);
        }
        std::memcpy(prev.data(), c, row);
    }
    out.w = int(w);
    out.h = int(h);
    out.px.assign(size_t(w) * h, 0);
    auto sample = [&](const uint8_t* r, size_t idx) -> unsigned { // channel sample idx of a row, native depth
        if (depth == 8) return r[idx];
        if (depth == 16) return unsigned(r[2 * idx]) << 8 | r[2 * idx + 1];
        const size_t bit = idx * size_t(depth);
        return (r[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
    };
    auto to8 = [&](unsigned v) -> uint8_t {
        if (depth == 16) return uint8_t(v >> 8);
        if (depth == 8 || ctype == 3) return uint8_t(v);
        return uint8_t(v * 255 / ((1u << depth) - 1));
    };
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* r = &raw[size_t(y) * (row + 1) + 1];
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t R, G, B, A = 255;
            if (ctype == 0) {
                const unsigned v = sample(r, x);
                R = G = B = to8(v);
                if (trns.size() >= 2 && v == (unsigned(trns[0]) << 8 | trns[1])) A = 0;
            } else if (ctype == 2) {
                const unsigned r16 = sample(r, 3 * x), g16 = sample(r, 3 * x + 1), b16 = sample(r, 3 * x + 2);
                R = to8(r16);
                G = to8(g16);
                B = to8(b16);
                if (trns.size() >= 6 && r16 == (unsigned(trns[0]) << 8 | trns[1]) && g16 == (unsigned(trns[2]) << 8 | trns[3]) &&
                    b16 == (unsigned(trns[4]) << 8 | trns[5]))
                    A = 0;
            } else if (ctype == 3) {
                const unsigned i = sample(r, x);
                if (i * 3 + 2 >= plte.size()) { why = "palette index out of range"; return false; }
                R = plte[i * 3];
                G = plte[i * 3 + 1];
                B = plte[i * 3 + 2];
                if (i < trns.size()) A = trns[i];
            } else if (ctype == 4) {
                R = G = B = to8(sample(r, 2 * x));
                A = to8(sample(r, 2 * x + 1));
            } else {
                R = to8(sample(r, 4 * x));
                G = to8(sample(r, 4 * x + 1));
                B = to8(sample(r, 4 * x + 2));
                A = to8(sample(r, 4 * x + 3));
            }
            out.px[size_t(y) * w + x] = premultiply(R, G, B, A);
        }
    }
    return true;
}


} // namespace x2::pack
