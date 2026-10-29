#include "modern/draw_capture.h"

#include "custom_regs.h"
#include "machine.h"

extern "C" {
#include "m68kcpu.h"
}

namespace x2::modern {

uint32_t DrawCapture::long_at(const Machine& m, uint32_t a) {
    auto b = [&](uint32_t o) { return uint32_t(m.chip[(a + o) & kChipMask]); };
    return (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
}

int DrawCapture::locate(uint32_t addr, uint32_t draw, uint32_t other, int& plane, int& y, int& xbyte) {
    addr &= kChipMask;
    int buf = -1;
    uint32_t off = 0;
    if (addr >= draw && addr < draw + kBufferBytes) { buf = 0; off = addr - draw; }
    else if (addr >= other && addr < other + kBufferBytes) { buf = 1; off = addr - other; }
    if (buf < 0) { plane = y = xbyte = 0; return -1; }
    y = int(off / kLineBytes);
    plane = int(off % kLineBytes / kRowBytes);
    xbyte = int(off % kRowBytes);
    return buf;
}

void DrawCapture::attach(Machine& m) {
    m.on_blit = [this](Machine& mm) { blit(mm); };
    m.on_cpu_write = [this, &m](uint32_t addr, uint32_t value, int size) { cpu_write(m, addr, value, size); };
}

void DrawCapture::detach(Machine& m) {
    m.on_blit = nullptr;
    m.on_cpu_write = nullptr;
}

// The object whose draw handler is running: run_object_list_draw pushed the next record's address and then
// called the handler, so the stack holds {..., $3678, next}. The record drawn is the one linking to `next`.
uint32_t DrawCapture::find_object(const Machine& m) const {
    uint32_t sp = m.areg(7) & kChipMask;
    for (int i = 0; i < 16; ++i) {
        if ((long_at(m, sp + 4 * uint32_t(i)) & 0xFFFFFF) != kObjectDrawReturn) continue;
        uint32_t next = long_at(m, sp + 4 * uint32_t(i) + 4) & kChipMask;
        for (uint32_t head : kListHeads) {
            uint32_t o = long_at(m, head) & kChipMask;
            for (int guard = 0; guard < 256 && ((m.chip[o] << 8) | m.chip[o + 1]); ++guard) {
                uint32_t n = long_at(m, o + 0x12) & kChipMask;
                if (n == next) return o;
                o = n;
            }
        }
        return 0;
    }
    return 0;
}

// The A/B/C words the blitter will consume, in the order Machine::do_blit processes them (same masks, shifter
// and pointer arithmetic), read before the blit runs.
void DrawCapture::capture_words(const Machine& m, BlitCmd& b) {
    if (b.line_mode()) return;
    bool use_a = b.con0 & 0x800, use_b = b.con0 & 0x400, use_c = b.con0 & 0x200;
    bool desc = b.con1 & 2;
    int ash = b.con0 >> 12, bsh = b.con1 >> 12;
    int inc = desc ? -2 : 2;
    int amod = b.amod & ~1, bmod = b.bmod & ~1, cmod = b.cmod & ~1;
    if (desc) { amod = -amod; bmod = -bmod; cmod = -cmod; }
    uint32_t apt = b.apt, bpt = b.bpt, cpt = b.cpt;
    uint16_t adat = b.adat, bdat = b.bdat, cdat = b.cdat, a_old = 0, b_old = 0;
    auto rd = [&](uint32_t a) { a &= kChipMask; return uint16_t((m.chip[a] << 8) | m.chip[(a + 1) & kChipMask]); };
    auto shift = [&](uint16_t cur, uint16_t old, int sh) -> uint16_t {
        if (!sh) return cur;
        if (!desc) return uint16_t(((uint32_t(old) << 16) | cur) >> sh);
        return uint16_t(((uint32_t(cur) << 16 | old) << sh) >> 16);
    };
    b.data = uint32_t(cur_.words.size());
    for (int y = 0; y < b.h; ++y) {
        for (int x = 0; x < b.w; ++x) {
            if (use_a) { adat = rd(apt); apt += inc; }
            if (use_b) { bdat = rd(bpt); bpt += inc; }
            if (use_c) { cdat = rd(cpt); cpt += inc; }
            uint16_t am = adat;
            if (x == 0) am &= b.fwm;
            if (x == b.w - 1) am &= b.lwm;
            cur_.words.push_back(shift(am, a_old, ash));
            cur_.words.push_back(shift(bdat, b_old, bsh));
            cur_.words.push_back(cdat);
            a_old = am;
            b_old = bdat;
        }
        if (use_a) apt += amod;
        if (use_b) bpt += bmod;
        if (use_c) cpt += cmod;
    }
}

void DrawCapture::blit(Machine& m) {
    auto r = [&](uint16_t off) { return m.regs[off >> 1]; };
    auto p = [&](uint16_t off) { return ((uint32_t(r(off)) << 16) | r(off + 2)) & kChipMask & ~1u; };
    if (!started_) {
        started_ = true;
        cur_.first_frame = m.frame;
    }
    BlitCmd b;
    b.seq = seq_++;
    b.pc = m68k_get_reg(nullptr, M68K_REG_PPC) & 0xFFFFFF;
    b.caller = long_at(m, m.areg(7)) & 0xFFFFFF;
    b.con0 = r(reg::BLTCON0);
    b.con1 = r(reg::BLTCON1);
    b.fwm = r(reg::BLTAFWM);
    b.lwm = r(reg::BLTALWM);
    uint16_t size = r(reg::BLTSIZE);
    b.h = uint16_t(size >> 6 ? size >> 6 : 1024);
    b.w = uint16_t(size & 63 ? size & 63 : 64);
    b.apt = p(reg::BLTAPTH);
    b.bpt = p(reg::BLTBPTH);
    b.cpt = p(reg::BLTCPTH);
    b.dpt = p(reg::BLTDPTH);
    b.amod = int16_t(r(reg::BLTAMOD));
    b.bmod = int16_t(r(reg::BLTBMOD));
    b.cmod = int16_t(r(reg::BLTCMOD));
    b.dmod = int16_t(r(reg::BLTDMOD));
    b.adat = r(reg::BLTADAT);
    b.bdat = r(reg::BLTBDAT);
    b.cdat = r(reg::BLTCDAT);
    for (int i = 0; i < 8; ++i) {
        b.regs[i] = m.dreg(i);
        b.regs[8 + i] = m.areg(i);
    }
    b.frame = m.frame;
    b.line = uint16_t(m.line);
    b.obj = find_object(m);
    int xbyte = 0;
    b.buffer = locate(b.dpt, long_at(m, kDrawPtr), long_at(m, kShowPtr), b.plane, b.y, xbyte);
    b.x = (xbyte & ~1) * 8 + (b.line_mode() ? 0 : b.ash());
    if (capture_data && !b.fill_mode()) capture_words(m, b);
    cur_.blits.push_back(b);
}

void DrawCapture::cpu_write(Machine& m, uint32_t addr, uint32_t value, int size) {
    if (addr == kShowPtr + 2 && size == 2) {
        // Low word of the display pointer: the swap's `move.l d0,$40A` completes now.
        uint32_t shown = (long_at(m, kShowPtr) & 0xFFFF0000u) | value;
        if (shown != long_at(m, kShowPtr)) {
            cur_.draw_buf = shown;
            cur_.other_buf = long_at(m, kDrawPtr);
            finish_step(m);
        }
        return;
    }
    int plane = 0, y = 0, xbyte = 0;
    int buf = locate(addr, long_at(m, kDrawPtr), long_at(m, kShowPtr), plane, y, xbyte);
    if (buf < 0 && !all_writes) return;
    if (!started_) {
        started_ = true;
        cur_.first_frame = m.frame;
    }
    uint32_t old = size == 1 ? m.chip[addr] : (uint32_t(m.chip[addr]) << 8) | m.chip[(addr + 1) & kChipMask];
    cur_.writes.push_back(
        CpuWrite{seq_++, m68k_get_reg(nullptr, M68K_REG_PPC) & 0xFFFFFF, addr, value, old, uint8_t(size), buf});
}

void DrawCapture::finish_step(Machine& m) {
    cur_.index = next_index_++;
    cur_.swap_frame = m.frame;
    cur_.swap_line = uint16_t(m.line);
    auto w16 = [&](uint32_t a) { a &= kChipMask; return uint16_t((m.chip[a] << 8) | m.chip[(a + 1) & kChipMask]); };
    for (uint8_t l = 0; l < 5; ++l) {
        // The head holds a pointer to the first record; a record whose +0 word is 0 ends the list.
        uint32_t o = long_at(m, kListHeads[l]) & kChipMask;
        for (int guard = 0; w16(o) && guard < 256; ++guard) {
            ObjectSnap s;
            s.addr = o;
            s.list = l;
            s.update = long_at(m, o + 2) & 0xFFFFFF;
            s.draw = long_at(m, o + 6) & 0xFFFFFF;
            s.hit = long_at(m, o + 0xA) & 0xFFFFFF;
            s.gfx = long_at(m, o + 0x16) & 0xFFFFFF;
            s.x = int16_t(w16(o + 0x20));
            s.y = int16_t(w16(o + 0x24));
            cur_.objects.push_back(s);
            o = long_at(m, o + 0x12) & kChipMask;
        }
    }
    for (int i = 0; i < kVarCount; ++i) cur_.vars[i] = w16(kVarAddr[i]);
    for (int i = 0; i < kStars * 3; ++i) cur_.stars[size_t(i)] = w16(kStarTable + 2 * uint32_t(i));
    for (int i = 0; i < 32; ++i) cur_.palette[i] = m.regs[(reg::COLOR00 >> 1) + i];
    if (capture_data) {
        uint32_t base = cur_.draw_buf - kGutterLines * kLineBytes;
        cur_.image.resize(size_t(kGutterLines + kLines) * kLineBytes);
        for (size_t i = 0; i < cur_.image.size(); ++i) cur_.image[i] = m.chip[(base + i) & kChipMask];
    }
    if (on_step) on_step(cur_);
    if (keep) {
        if (steps_.size() >= max_kept) steps_.erase(steps_.begin());
        steps_.push_back(std::move(cur_));
    }
    cur_ = Step{};
    cur_.first_frame = m.frame;
    seq_ = 0;
}

void write_step_log(FILE* f, const Step& s) {
    std::fprintf(f, "S %u %u %u %u %06X %06X", s.index, s.first_frame, s.swap_frame, s.swap_line, s.draw_buf, s.other_buf);
    for (uint16_t v : s.vars) std::fprintf(f, " %04X", v);
    std::fprintf(f, "\n");
    for (const BlitCmd& b : s.blits) {
        std::fprintf(f, "B %06X %06X %06X %u %u %04X %04X %04X %04X %u %u %06X %06X %06X %06X %d %d %d %d %04X %04X %d %d %d %d",
                     b.pc, b.caller, b.obj, b.frame, b.line, b.con0, b.con1, b.fwm, b.lwm, b.w, b.h, b.apt, b.bpt, b.cpt,
                     b.dpt, b.amod, b.bmod, b.cmod, b.dmod, b.adat, b.bdat, b.buffer, b.plane, b.y, b.x);
        for (uint32_t r : b.regs) std::fprintf(f, " %08X", r);
        std::fprintf(f, "\n");
    }
    for (const CpuWrite& w : s.writes)
        std::fprintf(f, "W %06X %06X %u %X %X %d\n", w.pc, w.addr, w.size, w.value, w.old, w.buffer);
    for (const ObjectSnap& o : s.objects)
        std::fprintf(f, "O %06X %u %06X %06X %06X %06X %d %d\n", o.addr, o.list, o.update, o.draw, o.hit, o.gfx, o.x, o.y);
}

} // namespace x2::modern
