#include "gfx/video.h"

#include "custom_regs.h"
#include "machine.h"

#include <algorithm>
#include <cstring>

namespace x2 {

namespace {
constexpr uint16_t kDmaBpl = reg::DMAF_MASTER | reg::DMAF_RASTER;
constexpr uint16_t kDmaSpr = reg::DMAF_MASTER | reg::DMAF_SPRITE;
constexpr uint16_t kSprSlotsEnd = 0x18; // colour clock by which the sprite DMA slots of a line have happened

bool display_reg(uint16_t off) {
    return (off >= reg::DIWSTRT && off <= reg::DMACON) || (off >= reg::BPL1PTH && off < 0xF8) ||
           (off >= reg::BPLCON0 && off <= reg::BPL2MOD) || (off >= reg::SPR0PTH && off < 0x1C0);
}

uint16_t half_bright(uint16_t c) { return uint16_t((c >> 1) & 0x777); }
} // namespace

uint32_t Video::rgba_of(uint16_t c) {
    uint32_t r = ((c >> 8) & 15) * 17, g = ((c >> 4) & 15) * 17, b = (c & 15) * 17;
    return r | (g << 8) | (b << 16) | 0xFF000000u; // little-endian RGBA byte order
}

void Video::attach(Machine& m) {
    auto r = [&](uint16_t off) { return m.regs[off >> 1]; };
    auto ptr = [&](uint16_t off) { return ((uint32_t(r(off)) << 16) | r(off + 2)) & ~1u; };
    s_.diwstrt = r(reg::DIWSTRT);
    s_.diwstop = r(reg::DIWSTOP);
    s_.ddfstrt = r(reg::DDFSTRT);
    s_.ddfstop = r(reg::DDFSTOP);
    s_.dmacon = m.dmacon;
    s_.bplcon0 = r(reg::BPLCON0);
    s_.bplcon1 = r(reg::BPLCON1);
    s_.bplcon2 = r(reg::BPLCON2);
    s_.bpl1mod = r(reg::BPL1MOD);
    s_.bpl2mod = r(reg::BPL2MOD);
    for (int p = 0; p < 6; ++p) s_.bplpt[p] = ptr(uint16_t(reg::BPL1PTH + 4 * p));
    for (int i = 0; i < 32; ++i) s_.color[i] = r(uint16_t(reg::COLOR00 + 2 * i));
    for (int i = 0; i < 8; ++i) {
        Sprite& sp = s_.spr[i];
        sp.pt = ptr(uint16_t(reg::SPR0PTH + 4 * i));
        sp.pos = r(uint16_t(reg::SPR0POS + 8 * i));
        sp.ctl = r(uint16_t(reg::SPR0POS + 8 * i + 2));
    }
    m.on_custom_write = [this](uint16_t off, uint16_t v, int line, uint16_t hpos) { custom_write(off, v, line, hpos); };
    m.on_line_end = [this](Machine& mm, int line) { line_end(mm.chip.data(), kChipMask, line, line == mm.lines_per_frame() - 1); };
}

void Video::custom_write(uint16_t off, uint16_t value, int, uint16_t hpos) {
    if (display_reg(off)) events_.push_back(Event{off, value, hpos});
}

void Video::apply(const Event& e) {
    uint16_t off = e.off, v = e.value;
    if (off >= reg::COLOR00) { s_.color[(off - reg::COLOR00) >> 1] = v & 0xFFF; return; }
    if (off >= reg::SPR0POS) {
        Sprite& sp = s_.spr[(off - reg::SPR0POS) >> 3];
        switch ((off >> 1) & 3) {
        case 0: sp.pos = v; break;
        case 1: sp.ctl = v; sp.armed = false; break;
        case 2: sp.data = v; sp.armed = true; break;
        default: sp.datb = v; break;
        }
        return;
    }
    if (off >= reg::SPR0PTH) {
        Sprite& sp = s_.spr[(off - reg::SPR0PTH) >> 2];
        if (off & 2) sp.pt = (sp.pt & 0xFFFF0000u) | (v & 0xFFFE);
        else sp.pt = (sp.pt & 0xFFFF) | (uint32_t(v & 0x1F) << 16);
        return;
    }
    if (off >= reg::BPL1PTH && off < 0xF8) {
        uint32_t& pt = s_.bplpt[(off - reg::BPL1PTH) >> 2];
        if (off & 2) pt = (pt & 0xFFFF0000u) | (v & 0xFFFE);
        else pt = (pt & 0xFFFF) | (uint32_t(v & 0x1F) << 16);
        return;
    }
    switch (off) {
    case reg::DIWSTRT: s_.diwstrt = v; break;
    case reg::DIWSTOP: s_.diwstop = v; break;
    case reg::DDFSTRT: s_.ddfstrt = v; break;
    case reg::DDFSTOP: s_.ddfstop = v; break;
    case reg::DMACON:
        if (v & 0x8000) s_.dmacon |= v & 0x7FF;
        else s_.dmacon &= uint16_t(~v);
        break;
    case reg::BPLCON0: s_.bplcon0 = v; break;
    case reg::BPLCON1: s_.bplcon1 = v; break;
    case reg::BPLCON2: s_.bplcon2 = v; break;
    case reg::BPL1MOD: s_.bpl1mod = v; break;
    case reg::BPL2MOD: s_.bpl2mod = v; break;
    default: break;
    }
}

// Sprite DMA: on the first line after vertical blank the control words (POS, CTL) are fetched from SPRxPT. When
// the beam reaches VSTART the channel fetches DATA/DATB (arming the sprite) on each line until VSTOP, where it
// fetches the next control word pair instead (a zero pair ends the sprite for this frame).
void Video::sprite_dma(const uint8_t* chip, uint32_t mask, int line) {
    if ((s_.dmacon & kDmaSpr) != kDmaSpr || line < kSpriteDmaFirstLine) return;
    auto rd = [&](uint32_t a) { a &= mask; return uint16_t((chip[a] << 8) | chip[(a + 1) & mask]); };
    for (Sprite& sp : s_.spr) {
        auto fetch_ctl = [&] {
            sp.pos = rd(sp.pt);
            sp.ctl = rd(sp.pt + 2);
            sp.armed = false;
            sp.pt += 4;
            sp.dma = Sprite::WaitStart;
        };
        if (sp.dma == Sprite::FetchCtl) { fetch_ctl(); continue; }
        if (sp.dma == Sprite::WaitStart && line == sp.vstart()) sp.dma = Sprite::Active;
        if (sp.dma == Sprite::Active) {
            if (line == sp.vstop()) fetch_ctl();
            else {
                sp.data = rd(sp.pt);
                sp.datb = rd(sp.pt + 2);
                sp.armed = true;
                sp.pt += 4;
            }
        }
    }
}

void Video::line_end(const uint8_t* chip, uint32_t mask, int line, bool last_line) {
    if (line == 0)
        for (Sprite& sp : s_.spr) sp.dma = Sprite::FetchCtl;
    size_t ev = 0;
    while (ev < events_.size() && events_[ev].hpos < kSprSlotsEnd) apply(events_[ev++]);
    sprite_dma(chip, mask, line);

    // Bitplane DMA at DDFSTRT, inside the vertical display window.
    while (ev < events_.size() && events_[ev].hpos < (s_.ddfstrt & 0xFC)) apply(events_[ev++]);
    int vstart = s_.diwstrt >> 8, vstop = s_.diwstop >> 8;
    if (!(vstop & 0x80)) vstop |= 0x100;
    bool vwin = line >= vstart && line < vstop;
    bool hires = s_.bplcon0 & 0x8000;
    int planes = std::min((s_.bplcon0 >> 12) & 7, hires ? 4 : 6);
    pf_.fill(0);
    if (vwin && planes && (s_.dmacon & kDmaBpl) == kDmaBpl) {
        int strt = s_.ddfstrt & 0xFC, stop = s_.ddfstop & 0xFC;
        int words = stop >= strt ? ((stop - strt) / 8 + 1) * (hires ? 2 : 1) : 0;
        int px = hires ? 1 : 2;                         // hires positions per bitplane pixel
        int start = 2 * (strt * 2 + (hires ? 9 : 17)); // first pixel, in hires positions
        for (int p = 0; p < planes; ++p) {
            int delay = 2 * ((p & 1) ? (s_.bplcon1 >> 4) & 15 : s_.bplcon1 & 15);
            uint32_t pt = s_.bplpt[p];
            for (int w = 0; w < words; ++w, pt += 2) {
                uint16_t d = uint16_t((chip[pt & mask] << 8) | chip[(pt + 1) & mask]);
                int base = start + delay + w * 16 * px;
                for (int b = 0; b < 16; ++b) {
                    if (!((d >> (15 - b)) & 1)) continue;
                    for (int k = 0; k < px; ++k) {
                        int pos = base + b * px + k;
                        if (pos < kHiresSpan) pf_[size_t(pos)] |= uint8_t(1 << p);
                    }
                }
            }
            int mod = int16_t((p & 1) ? s_.bpl2mod : s_.bpl1mod);
            s_.bplpt[p] = (pt + uint32_t(mod)) & ~1u;
        }
    }

    // Denise output, applying the remaining writes at their colour clock.
    int row = line - kFbTop;
    if (row >= 0 && row < kFbHeight) {
        uint8_t* out = &back_[size_t(row) * kFbWidth * 4];
        uint16_t ham = s_.color[0];
        for (int X = 0; X < kFbWidth; ++X) {
            int x = X + kFbLeft;
            while (ev < events_.size() && events_[ev].hpos * 2 <= x) apply(events_[ev++]);
            int hstart = s_.diwstrt & 0xFF, hstop = (s_.diwstop & 0xFF) | 0x100;
            uint16_t c[2];
            if (!vwin || x < hstart || x >= hstop) {
                c[0] = c[1] = s_.color[0];
            } else {
                // Sprites: lowest-numbered visible pair wins. SPRxPOS H = $80 lines up with DIW x = $81.
                int sgroup = -1, scol = 0;
                for (int k = 0; k < 4 && sgroup < 0; ++k) {
                    int v[2];
                    for (int j = 0; j < 2; ++j) {
                        const Sprite& sp = s_.spr[2 * k + j];
                        int d = x - (sp.hstart() + 1);
                        v[j] = (sp.armed && d >= 0 && d < 16)
                                   ? ((sp.data >> (15 - d)) & 1) | (((sp.datb >> (15 - d)) & 1) << 1)
                                   : 0;
                    }
                    if (s_.spr[2 * k + 1].ctl & 0x80) { // attached pair: 4-bit colour 16-31
                        if (int a = v[0] | (v[1] << 2)) { sgroup = k; scol = 16 + a; }
                    } else if (v[0]) { sgroup = k; scol = 16 + 4 * k + v[0]; }
                    else if (v[1]) { sgroup = k; scol = 16 + 4 * k + v[1]; }
                }
                bool dpf = s_.bplcon0 & 0x0400;
                bool ham_mode = (s_.bplcon0 & 0x0800) && !hires && !dpf;
                int pf1p = s_.bplcon2 & 7, pf2p = (s_.bplcon2 >> 3) & 7;
                bool pf2pri = s_.bplcon2 & 0x40;
                for (int k = 0; k < 2; ++k) {
                    int idx = pf_[size_t(2 * x + k)];
                    // Depth keys, smaller is in front: sprite group g sits in front of a playfield whose
                    // BPLCON2 priority code is > g. Single playfield uses PF2P.
                    int best = sgroup >= 0 ? (2 * sgroup + 1) * 4 : 1 << 20;
                    uint16_t col = sgroup >= 0 ? s_.color[scol] : s_.color[0];
                    if (dpf) {
                        int p1 = (idx & 1) | ((idx >> 1) & 2) | ((idx >> 2) & 4);
                        int p2 = ((idx >> 1) & 1) | ((idx >> 2) & 2) | ((idx >> 3) & 4);
                        int k1 = 8 * pf1p + (pf2pri ? 2 : 1), k2 = 8 * pf2p + (pf2pri ? 1 : 2);
                        if (p1 && k1 < best) { best = k1; col = s_.color[p1]; }
                        if (p2 && k2 < best) { best = k2; col = s_.color[8 + p2]; }
                    } else {
                        uint16_t pc;
                        if (ham_mode) {
                            int data = idx & 15;
                            switch ((idx >> 4) & 3) {
                            case 0: ham = s_.color[data]; break;
                            case 1: ham = uint16_t((ham & 0xFF0) | data); break;
                            case 2: ham = uint16_t((ham & 0x0FF) | (data << 8)); break;
                            default: ham = uint16_t((ham & 0xF0F) | (data << 4)); break;
                            }
                            pc = ham;
                        } else if (idx >= 32) {
                            pc = half_bright(s_.color[idx & 31]); // EHB
                        } else {
                            pc = s_.color[idx];
                        }
                        if (ham_mode || idx) {
                            if (8 * pf2p < best) col = pc;
                        } else if (sgroup < 0) {
                            col = pc;
                        }
                    }
                    c[k] = col;
                }
            }
            uint32_t a = rgba_of(c[0]), b = rgba_of(c[1]);
            uint8_t* o = out + X * 4;
            for (int ch = 0; ch < 4; ++ch) {
                unsigned va = (a >> (8 * ch)) & 0xFF, vb = (b >> (8 * ch)) & 0xFF;
                o[ch] = uint8_t((va + vb + 1) / 2);
            }
        }
    }
    while (ev < events_.size()) apply(events_[ev++]);
    events_.clear();

    if (last_line) {
        front_.swap(back_);
        ++frames_;
    }
}

uint64_t Video::front_hash() const {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : front_) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace x2
