#include "import/sha1.h"

#include <array>

namespace x2::import {

namespace {
uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
} // namespace

std::string sha1_hex(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t full = size / 64 * 64;
    for (size_t i = 0; i < full; i += 64) sha1_block(h, data + i);
    std::array<uint8_t, 128> tail{};
    size_t rest = size - full;
    for (size_t i = 0; i < rest; ++i) tail[i] = data[full + i];
    tail[rest] = 0x80;
    size_t tail_len = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - size_t(i)] = uint8_t(bits >> (8 * i));
    for (size_t i = 0; i < tail_len; i += 64) sha1_block(h, tail.data() + i);
    static const char* const hex = "0123456789abcdef";
    std::string s;
    for (uint32_t v : h)
        for (int i = 7; i >= 0; --i) s += hex[(v >> (4 * i)) & 15];
    return s;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace x2::import
