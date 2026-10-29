// Accurate bus timing (Timing::Accurate): the OCS chip-bus slot model, the blitter's cycle sequence and the CPU's
// wait states. docs/re-notes/timing.md §8 describes the model, its sources and what it leaves out.
//
// Time base: absolute CPU cycles (7.09 MHz) since power-on; a colour clock (cck, one chip-bus slot) is two CPU
// cycles, and cck c is at beam position (c / 227) % 312, c % 227.
#include "machine.h"

#include <algorithm>

extern "C" {
#include "m68kcpu.h" // m68ki_cpu registers and m68ki_remaining_cycles (the slice counter translated code shares)
}

namespace x2 {

const uint8_t* g_dasm_scratch = nullptr; // machine.cpp: the disassembler reads from here while it is set

namespace {
// Slot positions as the reference emulator (WinUAE/Amiberry custom.cpp) numbers them: REFRESH_FIRST_HPOS 3 (four
// refresh slots, every other cck), DMAL_FIRST_HPOS 11 (three disk slots, then AUD0-3).
constexpr int kRefreshFirst = 3;
constexpr int kAudioFirst = 11 + 3 * 2;
// Bitplane fetch order within a fetch unit (HRM figure 6-9): lores units are 8 cck, plane 1..6 at these offsets;
// hires units are 4 cck, planes 1..4.
constexpr int kLoresSlot[6] = {7, 3, 5, 1, 6, 2};
constexpr int kHiresSlot[4] = {3, 1, 2, 0};
constexpr int kBlitFinished = -1000; // WinUAE CYCLECOUNT_FINISHED
constexpr int kNastySteal = 3;       // BLIT_NASTY_CPU_STEAL_CYCLE_COUNT: CPU denied 3 slots in a row gets the next

void charge(Machine& m, int64_t extra) {
    if (extra <= 0) return;
    m68ki_remaining_cycles -= int(extra);
    m.cpu_wait_cycles += uint64_t(extra);
}

// Exact 68000 DIVU/DIVS times, as the reference emulator computes them (WinUAE newcpu_common.cpp:
// getDivu68kCycles, getDivs68kCycles, after Jorge Cwik's analysis). Each returns the instruction's cycles without
// its effective-address time, so it compares directly with Musashi's fixed table values (DIVU 140 and DIVS 158,
// the worst cases). Musashi already times MULU/MULS by the source operand's bits.
int divu_cycles(uint32_t dividend, uint16_t divisor) {
    if ((dividend >> 16) >= divisor) return 10; // overflow
    int m = 38;
    uint32_t hdivisor = uint32_t(divisor) << 16;
    for (int i = 0; i < 15; ++i) {
        uint32_t temp = dividend;
        dividend <<= 1;
        if (int32_t(temp) < 0) {
            dividend -= hdivisor;
        } else {
            m += 2;
            if (dividend >= hdivisor) {
                dividend -= hdivisor;
                --m;
            }
        }
    }
    return m * 2;
}
int divs_cycles(int32_t dividend, int16_t divisor) {
    int m = 6;
    if (dividend < 0) ++m;
    uint32_t adend = dividend < 0 ? 0u - uint32_t(dividend) : uint32_t(dividend);
    uint16_t asor = uint16_t(divisor < 0 ? -divisor : divisor);
    if ((adend >> 16) >= asor) return (m + 2) * 2; // overflow
    uint32_t aquot = adend / asor;
    m += 55;
    if (divisor >= 0) m += dividend >= 0 ? -1 : 1;
    for (int i = 0; i < 15; ++i) {
        if (int16_t(aquot) >= 0) ++m;
        aquot <<= 1;
    }
    return m * 2;
}
} // namespace

void Machine::set_timing(Timing t) {
    if (t == timing_) return;
    if (blitter_active()) {
        bs_ = BlitSeq{};
        intreq |= reg::INTF_BLIT;
        update_irq();
    }
    timing_ = t;
    lines_ = t == Timing::Accurate ? 313 : kLinesPerFrame;
    dma_dirty_ = true;
}

int64_t Machine::cpu_now() const {
    return line_base_ + line_cycles_ + (in_execute_ ? m68k_cycles_run() : 0);
}

void Machine::timing_line_start() {
    line_start_abs_ = (int64_t(frame) * lines_ + line) * kCpuCyclesPerLine;
    line_base_ = line_start_abs_ + cpu_debt_;
}

void Machine::beam_at(int64_t cck, int& l, int& h) const {
    int64_t rel = std::max<int64_t>(0, cck - line_start_abs_ / 2);
    if (rel < kColorClocksPerLine) {
        l = line;
        h = int(rel);
        return;
    }
    // The CPU has run past the end of this line (it finishes its instruction first): report the next line.
    l = line + 1 == lines_ ? 0 : line + 1;
    h = int(std::min<int64_t>(rel - kColorClocksPerLine, kColorClocksPerLine - 1));
}

// ---------------------------------------------------------------------------------------------------------
// Fixed DMA slots

void Machine::rebuild_dma_maps() {
    dma_dirty_ = false;
    dma_blank_.fill(0);
    auto set = [](std::array<uint64_t, 4>& m, int h) {
        if (h >= 0 && h < kColorClocksPerLine) m[size_t(h >> 6)] |= 1ull << (h & 63);
    };
    for (int i = 0; i < 4; ++i) set(dma_blank_, kRefreshFirst + 2 * i);
    dma_display_ = dma_blank_;
    bpl_vstart_ = bpl_vstop_ = 0;
    uint16_t con0 = regs[reg::BPLCON0 >> 1];
    int planes = (con0 >> 12) & 7;
    bool hires = con0 & 0x8000;
    if ((dmacon & (reg::DMAF_MASTER | reg::DMAF_RASTER)) != (reg::DMAF_MASTER | reg::DMAF_RASTER) || !planes) return;
    planes = std::min(planes, hires ? 4 : 6);
    int ddf0 = regs[reg::DDFSTRT >> 1] & 0xFC, ddf1 = regs[reg::DDFSTOP >> 1] & 0xFC;
    // The last fetch unit starts at DDFSTOP.
    for (int u = ddf0; u <= ddf1; u += 8)
        for (int p = 0; p < planes; ++p) {
            if (hires) {
                set(dma_display_, u + kHiresSlot[p]);
                set(dma_display_, u + 4 + kHiresSlot[p]);
            } else {
                set(dma_display_, u + kLoresSlot[p]);
            }
        }
    bpl_vstart_ = regs[reg::DIWSTRT >> 1] >> 8;
    int vstop = regs[reg::DIWSTOP >> 1] >> 8;
    if (!(vstop & 0x80)) vstop |= 0x100;
    bpl_vstop_ = vstop;
}

bool Machine::fixed_dma(uint64_t cck) {
    if (dma_dirty_) rebuild_dma_maps();
    uint64_t abs_line = cck / kColorClocksPerLine;
    int h = int(cck - abs_line * kColorClocksPerLine);
    int ln = int(abs_line % uint64_t(lines_));
    const auto& m = ln >= bpl_vstart_ && ln < bpl_vstop_ ? dma_display_ : dma_blank_;
    if ((m[size_t(h >> 6)] >> (h & 63)) & 1) return true;
    int a = h - kAudioFirst;
    return audio_dma_ && a >= 0 && a < 8 && !(a & 1) && ((audio_dma_ >> (a >> 1)) & 1);
}

// ---------------------------------------------------------------------------------------------------------
// Blitter cycle sequence. Every blitter cycle needs a colour clock that no fixed DMA channel owns; cycles that do
// not access memory (start-up, idle steps of the channel diagram) leave the bus to the CPU.

void Machine::blitter_begin(uint64_t cck) {
    BlitSeq& b = bs_;
    b = BlitSeq{};
    uint16_t con0 = regs[reg::BLTCON0 >> 1], con1 = regs[reg::BLTCON1 >> 1], size = regs[reg::BLTSIZE >> 1];
    b.con0 = con0;
    b.line = con1 & 1;
    b.hsize = (size & 63) ? (size & 63) : 64;
    b.vsize = (size >> 6) ? (size >> 6) : 1024;
    b.skip_b = !(con0 & 0x400);
    if (b.line) {
        b.skip_y = true;
    } else {
        b.skip_y = (con0 & 0x300) != 0x300;
        // Fill mode with D but not C adds an idle cycle per word (it still needs a free slot).
        if ((con1 & 0x18) && (con0 & 0x300) == 0x100) {
            b.skip_y = false;
            b.fill_idle = true;
        }
    }
    b.counter = -3;
    b.wait = true; // the BLTSIZE write cycle
    b.main = true;
    blt_cck_ = cck;
    blt_start_cck_ = cck;
    if (blit_log && frame >= blit_log_from && frame <= blit_log_to) {
        uint64_t l = cck / kColorClocksPerLine;
        std::fprintf(blit_log, "%llu %llu %llu S %d %d %04X %04X\n", (unsigned long long)(l / uint64_t(lines_)),
                     (unsigned long long)(l % uint64_t(lines_)), (unsigned long long)(cck % kColorClocksPerLine), b.hsize,
                     b.vsize, con0, con1);
    }
}

bool Machine::blitter_shift() {
    BlitSeq& b = bs_;
    bool tmp[4] = {b.shifter[0], b.shifter[1], b.shifter[2], b.shifter[3]};
    std::fill(b.shifter, b.shifter + 4, false);
    bool out = false, dodat = false;
    if (b.out) {
        dodat = true;
        if (!b.line && (b.con0 & 0x100)) b.ddatuse = true;
        if (++b.hcount >= b.hsize) {
            b.hcount = 0;
            if (++b.vcount >= b.vsize) {
                b.out = false;
                b.counter = kBlitFinished;
                if (!b.line && b.ddatuse && (b.con0 & 0x100)) b.finald = 3;
            }
        }
        b.shifter[0] = b.out;
    }
    if (b.first > 0) {
        b.first = -1;
        b.shifter[0] = true;
    } else {
        if (b.skip_b) {
            b.shifter[2] = tmp[0];
        } else {
            b.shifter[1] = tmp[0];
            b.shifter[2] = tmp[1];
        }
        if (b.skip_y) {
            out = b.shifter[2];
        } else {
            b.shifter[3] = tmp[2];
            out = b.shifter[3];
        }
    }
    b.out = out;
    return dodat;
}

// 0 idle, 1 A, 2 B, 3 C, 4 D; line mode also 5 (A slot, free) and 6 (D slot with C off, nothing written).
int Machine::blitter_channel() const {
    const BlitSeq& b = bs_;
    if (b.counter < 0) return 0;
    bool ca = b.con0 & 0x800, cb = b.con0 & 0x400, cc = b.con0 & 0x200, cd = b.con0 & 0x100;
    bool last = b.hcount + 1 == b.hsize;
    if (b.line) {
        if (b.shifter[0]) return last ? 5 : (ca ? 1 : 0);
        if (b.shifter[1] && cb) return 2;
        if (b.shifter[2]) {
            if (cc) return last ? 4 : 3;
            if (last) return 6;
        }
        return 0;
    }
    if (b.shifter[0] && ca && !b.shifter[3]) return 1;
    if (b.shifter[2] && cc) return 3;
    if (b.shifter[3] && cc && (b.shifter[0] || b.shifter[1])) return 3;
    if (b.shifter[1] && cb) return 2;
    if (b.shifter[0] || b.first >= 0) return 0;
    if (b.ddatuse) {
        if (b.fill_idle) {
            if (b.shifter[3]) return 0;
            if (b.shifter[2] && cd) return 4;
        } else {
            if (b.shifter[2] && !cc && cd) return 4;
            if (b.shifter[3] && cc && cd) return 4;
        }
    }
    return 0;
}

void Machine::blitter_done(uint64_t cck) {
    bs_.main = false;
    if (blit_log && frame >= blit_log_from && frame <= blit_log_to) {
        uint64_t l = cck / kColorClocksPerLine;
        std::fprintf(blit_log, "%llu %llu %llu E\n", (unsigned long long)(l / uint64_t(lines_)),
                     (unsigned long long)(l % uint64_t(lines_)), (unsigned long long)(cck % kColorClocksPerLine));
    }
    // Busy time as the reference measures it (BLTSIZE write to the cycle that clears BBUSY).
    blit_cck += cck - blt_start_cck_;
    blt_done_cck_ = cck;
    intreq |= reg::INTF_BLIT;
    update_irq();
}

// Runs the blitter for colour clock `cck`. Returns true if the slot is not available to the CPU (fixed DMA or a
// blitter memory cycle). With `cpu_steal` (the CPU has been denied kNastySteal slots in a row) a blitter without
// BLTPRI skips this cycle and the CPU gets it.
bool Machine::blitter_cycle(uint64_t cck, bool cpu_steal) {
    bool fixed = fixed_dma(cck);
    if (cck < blt_cck_ || !blitter_active()) return fixed;
    blt_cck_ = cck + 1;
    BlitSeq& b = bs_;
    if (b.finald > 1) --b.finald;
    if (b.wait) {
        b.wait = false;
        return fixed;
    }
    if (fixed || (dmacon & (reg::DMAF_MASTER | reg::DMAF_BLITTER)) != (reg::DMAF_MASTER | reg::DMAF_BLITTER))
        return fixed;
    if (cpu_steal && !(dmacon & reg::DMAF_BLITHOG)) return false;
    if (b.finald == 1) {
        b.finald = 0;
        return (b.con0 & 0x100) && !b.line;
    }
    if (!b.main) return false;
    ++b.counter;
    if (b.counter == 0) {
        b.first = 1;
        b.finald = 0;
        blitter_shift();
    }
    int ch = blitter_channel();
    if (b.counter >= 0) blitter_shift();
    if (b.counter < -3) blitter_done(cck);
    return b.line ? (ch == 3 || ch == 4) : (ch >= 1 && ch <= 4);
}

void Machine::blitter_run_until(uint64_t cck) {
    while (blt_cck_ < cck && blitter_active()) blitter_cycle(blt_cck_, false);
}

// ---------------------------------------------------------------------------------------------------------
// CPU bus cycles

// One 68000 bus cycle to chip RAM or a custom register, requested at CPU cycle t: one colour clock of address
// set-up, then the transfer, which needs a free slot. Returns the CPU cycle at which the bus cycle ends (t + 4
// without contention).
int64_t Machine::chip_cycle(int64_t t) {
    uint64_t s = uint64_t(t + 1) / 2 + 1;
    for (int waited = 0;; ++s, ++waited) {
        bool busy;
        if (blitter_active()) {
            blitter_run_until(s);
            busy = blitter_cycle(s, waited >= kNastySteal);
        } else {
            busy = fixed_dma(s);
        }
        if (!busy) break;
    }
    return int64_t(s + 1) * 2;
}

// CIA access: the 68000 synchronises to the E clock (CPU/10). WinUAE cia.cpp: wait for E phase 2, then 4 + 6
// cycles, in place of the 4-cycle bus cycle.
int64_t Machine::cia_cycle(int64_t t) const {
    int div = int(t % 10);
    int sync = div < 2 ? 2 - div : div > 2 ? 10 - div + 2 : 0;
    return t + sync + 10;
}

void Machine::cpu_bus_access(uint32_t addr, int size) {
    int64_t t = std::max(bus_cursor_, cpu_now());
    int n = size == 4 ? 2 : 1;
    addr &= 0xFFFFFF;
    int64_t end = t;
    if (addr < 0x200000 || (addr & 0xFFF000) == 0xDFF000) {
        for (int i = 0; i < n; ++i) end = chip_cycle(end);
    } else if ((addr & 0xFF0000) == 0xBF0000) {
        for (int i = 0; i < n; ++i) end = cia_cycle(end);
    } else {
        end = t + 4 * n;
    }
    charge(*this, end - t - 4 * n);
    bus_cursor_ = end;
    access_cck_ = end / 2 - 1;
}

namespace {
bool cond(int cc) {
    switch (cc) {
    case 0: return true;
    case 1: return false;
    case 2: return COND_HI();
    case 3: return COND_LS();
    case 4: return COND_CC();
    case 5: return COND_CS();
    case 6: return COND_NE();
    case 7: return COND_EQ();
    case 8: return COND_VC();
    case 9: return COND_VS();
    case 10: return COND_PL();
    case 11: return COND_MI();
    case 12: return COND_GE();
    case 13: return COND_LT();
    case 14: return COND_GT();
    default: return COND_LE();
    }
}

// Where a 68000 instruction spends internal cycles relative to its bus cycles (Yacht, "Yet Another Cycle Hunting
// Table", and the 68000 microcode as the reference emulator's cycle-exact core follows it). Musashi gives only the
// total; the bus model places the prefetches at the start of the instruction, then the data accesses, then the
// remaining internal cycles. These exceptions move internal cycles in front:
//   pre: before the first bus cycle (branches: "n np np" taken, "nn np" not taken; DBcc; a -(An) operand read:
//        "n nr"); mid: between the prefetches and the data (a d8(An,Xn) or d8(PC,Xn) operand: "np n nr").
// The flags decide Bcc/DBcc, so they are evaluated here exactly as the instruction will.
void bus_shape(uint16_t op, int& pre, int& mid) {
    pre = mid = 0;
    int line = op >> 12;
    if (line == 6) { // BRA, BSR, Bcc
        int cc = (op >> 8) & 15;
        pre = (cc <= 1 || cond(cc)) ? 2 : 4;
        return;
    }
    if ((op & 0xF0F8) == 0x50C8) { // DBcc: cc true 12 "nn np np"; branch 10 "n np np"; expired 14 "n np np np"
        pre = cond((op >> 8) & 15) ? 4 : 2;
        return;
    }
    // Only instructions with an effective address in bits 5-0 have operand timing.
    if (line == 0x6 || line == 0x7 || line == 0xA || line == 0xF) return;
    if (line == 0x4 && ((op & 0xFFC0) == 0x4E40 || (op & 0xFFF8) == 0x4E70 || (op & 0xFFF8) == 0x4840)) return;
    if (line == 0xE && (op & 0xC0) != 0xC0) return;            // register shifts: count/register, not an EA
    if (line == 0x0 && (op & 0x0138) == 0x0108) return;        // MOVEP
    if ((op & 0xFB80) == 0x4880) return; // MOVEM: no operand-time internal cycles
    int mode = (op >> 3) & 7, reg = op & 7;
    bool indexed = mode == 6 || (mode == 7 && reg == 3);
    bool lea_pea = (op & 0xF1C0) == 0x41C0 || (op & 0xFFC0) == 0x4840;
    if (mode == 4 && !lea_pea) pre = 2;
    if (indexed) {
        if (lea_pea) pre = 2;
        else mid = 2;
    }
    if (line >= 1 && line <= 3) { // MOVE: destination EA in bits 11-6
        int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        if (dmode == 6 || (dmode == 7 && dreg == 3)) mid += 2;
    }
}
} // namespace

int Machine::insn_words(uint16_t op) {
    uint8_t& w = insn_len_[op];
    if (!w) {
        // On the 68000 the length depends only on the opcode word: disassemble it with zero extension words.
        uint8_t buf[16] = {uint8_t(op >> 8), uint8_t(op)};
        char text[256];
        g_dasm_scratch = buf;
        unsigned bytes = m68k_disassemble(text, 0, M68K_CPU_TYPE_68000);
        g_dasm_scratch = nullptr;
        w = uint8_t(std::clamp(bytes / 2, 1u, 5u));
    }
    return w;
}

// Instruction boundary (from the Musashi instruction hook, which the recompiled executor calls at the same
// boundaries): the instruction's prefetch bus cycles, one per instruction word, are placed at its start. A taken
// branch or jump of one word refills the two-word prefetch queue, so it costs one more read, charged here.
// Correction of Musashi's fixed DIVU/DIVS times for the instruction about to run at `pc` (opcode `op`): the exact
// time minus Musashi's, from the operand values (0 if not a division, or the divisor is not in plain memory).
int Machine::div_delta(uint32_t pc, uint16_t op) const {
    bool is_signed;
    switch (op & 0xF1C0) {
    case 0x80C0: is_signed = false; break;
    case 0x81C0: is_signed = true; break;
    default: return 0;
    }
    int mode = (op >> 3) & 7, r = op & 7;
    auto word = [&](uint32_t a) -> int {
        a &= 0xFFFFFF;
        if (a >= 0x200000 || (a & 1)) return -1;
        a &= kChipMask;
        return (chip[a] << 8) | chip[(a + 1) & kChipMask];
    };
    auto index = [&](uint32_t base, uint32_t ext_pc) -> uint32_t {
        int ext = word(ext_pc);
        uint32_t xn = m68ki_cpu.dar[(ext >> 12) & 15];
        int32_t x = (ext & 0x800) ? int32_t(xn) : int32_t(int16_t(xn));
        return base + uint32_t(x) + uint32_t(int8_t(ext & 0xFF));
    };
    int src;
    switch (mode) {
    case 0: src = int(m68ki_cpu.dar[r] & 0xFFFF); break;
    case 1: return 0; // An is not a valid source
    case 2: case 3: src = word(m68ki_cpu.dar[8 + r]); break;
    case 4: src = word(m68ki_cpu.dar[8 + r] - 2); break;
    case 5: src = word(m68ki_cpu.dar[8 + r] + uint32_t(int16_t(word(pc + 2)))); break;
    case 6: src = word(index(m68ki_cpu.dar[8 + r], pc + 2)); break;
    default:
        switch (r) {
        case 0: src = word(uint32_t(int16_t(word(pc + 2)))); break;
        case 1: src = word((uint32_t(word(pc + 2)) << 16) | uint32_t(word(pc + 4))); break;
        case 2: src = word(pc + 2 + uint32_t(int16_t(word(pc + 2)))); break;
        case 3: src = word(index(pc + 2, pc + 2)); break;
        case 4: src = word(pc + 2); break;
        default: return 0;
        }
    }
    if (src < 0) return 0;
    uint32_t dst = m68ki_cpu.dar[(op >> 9) & 7];
    if (!src) return 0; // divide by zero: the exception's time is Musashi's
    return is_signed ? divs_cycles(int32_t(dst), int16_t(src)) - 158 : divu_cycles(dst, uint16_t(src)) - 140;
}

void Machine::timing_insn_end() {
    // The previous instruction's time correction (DIVU/DIVS, table fixes), now that Musashi has charged its table time.
    if (div_pending_ > 0) charge(*this, div_pending_);
    else if (div_pending_ < 0) m68ki_remaining_cycles -= div_pending_;
    div_pending_ = 0;
}

// Musashi's 68000 cycle table follows the Motorola table without some of its footnotes. Corrections, confirmed
// against the reference emulator's cycle-exact core (per-instruction times measured on lines without DMA,
// docs/re-notes/timing.md §8.4):
//   ADD/SUB/AND/OR.L <ea>,Dn and ADDA/SUBA.L <ea>,An with a register or immediate source: +2 (8 for Dn,Dn);
//   ADDQ/SUBQ.W #,An: 8, not 4; ANDI.L #,Dn: 16, not 14;
//   BCHG/BCLR/BSET on a data register: Musashi always charges the maximum, the real time is 2 less for bit
//   numbers 0-15.
int Machine::insn_delta(uint32_t pc, uint16_t op) const {
    int line = op >> 12, opmode = (op >> 6) & 7, mode = (op >> 3) & 7, r = op & 7;
    bool reg_or_imm = mode == 0 || mode == 1 || (mode == 7 && r == 4);
    switch (line) {
    case 0x8: case 0x9: case 0xC: case 0xD: // OR, SUB, AND, ADD (and SUBA, ADDA)
        if (opmode == 2 && reg_or_imm) return 2;                                    // .L <ea>,Dn
        if (opmode == 7 && (line == 0x9 || line == 0xD) && reg_or_imm) return 2;    // SUBA/ADDA.L
        return 0;
    case 0x5:
        if ((op & 0x00F8) == 0x0048) return 4; // ADDQ/SUBQ.W #,An
        return 0;
    case 0x0: {
        if ((op & 0xFFF8) == 0x0280) return 2; // ANDI.L #,Dn
        if (mode != 0) return 0;
        int bitop = (op >> 6) & 3; // 1 BCHG, 2 BCLR, 3 BSET (0 BTST is exact)
        if (!bitop) return 0;
        int bit;
        if (op & 0x0100) bit = int(m68ki_cpu.dar[(op >> 9) & 7]);          // Dn,Dn
        else if ((op & 0x0F00) == 0x0800) {                                  // #n,Dn
            uint32_t a = (pc + 2) & kChipMask;
            bit = chip[(a + 1) & kChipMask];
        } else return 0;
        return (bit & 31) < 16 ? -2 : 0;
    }
    default:
        return 0;
    }
}

void Machine::timing_insn(uint32_t pc) {
    int64_t t = std::max(bus_cursor_, cpu_now());
    int64_t end = t, ideal = t;
    // The previous instruction's leftovers: a taken one-word branch or jump refills two prefetch words (one more
    // read than its length); MOVEM to registers reads one word more than it loads.
    int carried = (prev_len_ == 1 && pc != prev_pc_ + 2 ? 1 : 0) + movem_extra_;
    movem_extra_ = 0;
    for (int i = 0; i < carried; ++i) end = chip_cycle(end);
    ideal += 4 * carried;
    int len = 1;
    if (pc < 0x200000) {
        uint32_t a = pc & kChipMask;
        uint16_t op = uint16_t((chip[a] << 8) | chip[(a + 1) & kChipMask]);
        len = insn_words(op);
        int pre, mid;
        bus_shape(op, pre, mid);
        end += pre;
        ideal += pre;
        for (int i = 0; i < len; ++i) end = chip_cycle(end);
        ideal += 4 * len;
        end += mid;
        ideal += mid;
        div_pending_ = ((op & 0xF0C0) == 0x80C0 ? div_delta(pc, op) : 0) + insn_delta(pc, op);
        if ((op & 0xFB80) == 0x4C80) movem_extra_ = 1;
    }
    // The pre/mid internal cycles are part of Musashi's instruction time; only the waits are extra.
    charge(*this, end - ideal);
    bus_cursor_ = end;
    prev_pc_ = pc;
    prev_len_ = len;
}

} // namespace x2
