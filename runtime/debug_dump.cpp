// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "debug_dump.h"

#include "machine.h"

#include <cstdio>
#include <vector>

namespace x2 {

namespace {
const char* const kCiaRegNames[16] = {"PRA", "PRB", "DDRA", "DDRB", "TALO", "TAHI", "TBLO", "TBHI",
                                      "TODLO", "TODMID", "TODHI", "R0B", "SDR", "ICR", "CRA", "CRB"};
}

bool write_reglog(const Machine& m, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# frame line hpos src pc reg value\n");
    for (const RegWrite& w : m.frame_writes) {
        char name[16];
        if (w.reg & 0x1000) std::snprintf(name, sizeof name, "CIAA_%s", kCiaRegNames[w.reg & 15]);
        else if (w.reg & 0x2000) std::snprintf(name, sizeof name, "CIAB_%s", kCiaRegNames[w.reg & 15]);
        else std::snprintf(name, sizeof name, "%s", reg::name(w.reg));
        std::fprintf(f, "%u %3u %3u %c %06X %-8s %04X\n", w.frame, w.line, w.hpos, w.source, w.pc, name, w.value);
    }
    std::fclose(f);
    return true;
}

bool write_chip_ram(const Machine& m, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = std::fwrite(m.chip.data(), 1, m.chip.size(), f);
    std::fclose(f);
    return n == m.chip.size();
}

bool write_bitplane_ppm(const Machine& m, const std::string& path) {
    const DisplaySnapshot& d = m.display;
    int planes = (d.bplcon0 >> 12) & 7;
    if (planes > 6) planes = 6;
    int vstart = d.diwstrt >> 8, vstop = d.diwstop >> 8;
    if (!(vstop & 0x80)) vstop |= 0x100;
    int height = vstop - vstart;
    int ddf0 = d.ddfstrt & 0xFC, ddf1 = d.ddfstop & 0xFC;
    int words = ddf1 >= ddf0 ? (ddf1 - ddf0) / 8 + 1 : 0;
    if (!d.valid || height <= 0 || words <= 0) { height = 256; words = 20; planes = 0; }
    bool dma_on = (d.dmacon & (reg::DMAF_MASTER | reg::DMAF_RASTER)) == (reg::DMAF_MASTER | reg::DMAF_RASTER);
    if (!dma_on) planes = 0;
    int width = words * 16;
    std::vector<uint8_t> rgb(size_t(width) * size_t(height) * 3);
    uint32_t pt[6];
    for (int p = 0; p < 6; ++p) pt[p] = d.bplpt[p];
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = 0;
            for (int p = 0; p < planes; ++p) {
                uint32_t a = (pt[p] + uint32_t(x >> 3)) & m.chip_mask;
                idx |= ((m.chip[a] >> (7 - (x & 7))) & 1) << p;
            }
            uint16_t c = d.color[idx];
            uint8_t* o = &rgb[(size_t(y) * size_t(width) + size_t(x)) * 3];
            o[0] = uint8_t(((c >> 8) & 15) * 17);
            o[1] = uint8_t(((c >> 4) & 15) * 17);
            o[2] = uint8_t((c & 15) * 17);
        }
        for (int p = 0; p < planes; ++p)
            pt[p] += uint32_t(words * 2 + int16_t(p & 1 ? d.bpl2mod : d.bpl1mod));
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

int apply_poke(Machine& m, const Poke& p, uint32_t objlist_head) {
    auto put = [&](uint32_t a) {
        for (int i = 0; i < p.size; ++i) m.chip[(a + uint32_t(i)) & m.chip_mask] = uint8_t(p.value >> (8 * (p.size - 1 - i)));
    };
    if (!p.handler) {
        put(p.addr);
        return 1;
    }
    // Plain chip reads: no bus side effects, and every address stays inside chip RAM.
    auto rd16 = [&](uint32_t a) { return uint16_t(m.chip[a & m.chip_mask] << 8 | m.chip[(a + 1) & m.chip_mask]); };
    auto rd32 = [&](uint32_t a) { return uint32_t(rd16(a)) << 16 | rd16(a + 2); };
    int hits = 0;
    uint32_t a = rd32(objlist_head) & m.chip_mask;
    for (int n = 0; n < kObjWalkLimit && a && rd16(a) != 0; ++n) {
        if (rd32(a + 0xA) == p.handler) {
            put(a + p.addr);
            ++hits;
        }
        a = rd32(a + 0x12) & m.chip_mask;
    }
    return hits;
}

} // namespace x2
