#include "gfx/image_io.h"

#include <cstdio>

namespace x2 {

namespace {
uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32(out, uint32_t(data.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(&out[start], out.size() - start));
}

bool save(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    return std::fclose(f) == 0 && n == bytes.size();
}
} // namespace

bool write_ppm(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height) {
    std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    std::vector<uint8_t> out(header.begin(), header.end());
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i) out.insert(out.end(), &rgba[i * 4], &rgba[i * 4 + 3]);
    return save(path, out);
}

bool write_png(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height) {
    // Raw scanlines: filter byte 0 + RGB.
    std::vector<uint8_t> raw;
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * size_t(width) + size_t(x)) * 4];
            raw.insert(raw.end(), p, p + 3);
        }
    }
    // zlib stream of stored deflate blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        pos += n;
        if (n == 0) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, uint32_t(width));
    put32(ihdr, uint32_t(height));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB, deflate, no filter, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return save(path, out);
}

bool write_image(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height) {
    bool png = path.size() >= 4 && (path.compare(path.size() - 4, 4, ".png") == 0 || path.compare(path.size() - 4, 4, ".PNG") == 0);
    return png ? write_png(path, rgba, width, height) : write_ppm(path, rgba, width, height);
}

} // namespace x2
