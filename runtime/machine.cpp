#include "machine.h"

#include "trace/tracer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include "m68k.h"
}

namespace x2 {

namespace {
Machine* g_machine = nullptr;

int irq_level_for(uint16_t bits) {
    if (bits & 0x2000) return 6;
    if (bits & 0x1800) return 5;
    if (bits & 0x0780) return 4;
    if (bits & 0x0070) return 3;
    if (bits & 0x0008) return 2;
    if (bits & 0x0007) return 1;
    return 0;
}

// SET/CLR register semantics (DMACON, INTENA, INTREQ, ADKCON): bit 15 selects set or clear.
void setclr(uint16_t& r, uint16_t v) {
    if (v & 0x8000) r |= v & 0x7FFF;
    else r &= ~v;
}
} // namespace

Machine* current_machine() { return g_machine; }

Machine::Machine() : chip(kChipRamSize, 0), trap_index_(kChipRamSize / 2, 0) {
    g_machine = this;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    cpu_execute = &m68k_execute;
    cia_a.reset();
    cia_b.reset();
    // CIA-A port A inputs: /FIR1 /FIR0 /RDY /TK0 /WPRO /CHNG high (inactive), except /TK0 low = head on
    // track 0 and /RDY low = drive ready, so any residual drive polling terminates.
    cia_a.ext_pra = 0xFF & ~0x10 & ~0x20;
    cia_b.ext_prb = 0xFF;
    // Port directions as Kickstart leaves them: CIA-A PA0/PA1 (OVL, LED/filter) outputs, CIA-B PB0-7 (drive
    // control) and PA6/PA7 (DTR/RTS) outputs. Drive lines are idle-high (all drives deselected, motor off).
    cia_a.write(Cia::DDRA, 0x03);
    cia_a.write(Cia::PRA, 0x00); // OVL off, LED (audio filter) on
    cia_b.write(Cia::DDRB, 0xFF);
    cia_b.write(Cia::PRB, 0xFF);
    cia_b.write(Cia::DDRA, 0xC0);
    paula.attach(chip.data(), kChipMask, [this](uint16_t bits) {
        intreq |= bits;
        update_irq();
    });
    paula.set_led_filter(true); // PRA bit 1 = 0 above
    // CIA-A CRA bit 7 always reads 0 on the 8520, whatever Kickstart wrote, so the game's test at $768A
    // selects $410=5: one logic frame per 3 VBLs (docs/re-notes/timing.md).
}

Machine::~Machine() {
    if (g_machine == this) g_machine = nullptr;
}

void Machine::load(uint32_t addr, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) chip[(addr + i) & kChipMask] = data[i];
}

void Machine::start_cpu(uint32_t pc, uint32_t ssp, uint16_t sr) {
    // Musashi fetches SSP/PC from $0/$4 on reset: provide them, then restore the RAM contents.
    uint8_t saved[8];
    std::memcpy(saved, chip.data(), 8);
    const uint8_t vec[8] = {uint8_t(ssp >> 24), uint8_t(ssp >> 16), uint8_t(ssp >> 8), uint8_t(ssp),
                            uint8_t(pc >> 24),  uint8_t(pc >> 16),  uint8_t(pc >> 8),  uint8_t(pc)};
    std::memcpy(chip.data(), vec, 8);
    m68k_pulse_reset();
    std::memcpy(chip.data(), saved, 8);
    m68k_set_reg(M68K_REG_SR, sr);
    m68k_set_reg(M68K_REG_ISP, ssp);
    m68k_set_reg(M68K_REG_A7, ssp);
    m68k_set_reg(M68K_REG_PC, pc);
}

// ---------------------------------------------------------------------------------------------------------
// CPU register helpers

uint32_t Machine::pc() const { return m68k_get_reg(nullptr, M68K_REG_PC); }
uint32_t Machine::dreg(int n) const { return m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + n)); }
uint32_t Machine::areg(int n) const { return m68k_get_reg(nullptr, m68k_register_t(M68K_REG_A0 + n)); }
void Machine::set_dreg(int n, uint32_t v) { m68k_set_reg(m68k_register_t(M68K_REG_D0 + n), v); }
void Machine::set_areg(int n, uint32_t v) { m68k_set_reg(m68k_register_t(M68K_REG_A0 + n), v); }
void Machine::set_pc(uint32_t v) { m68k_set_reg(M68K_REG_PC, v); }
uint16_t Machine::sr() const { return uint16_t(m68k_get_reg(nullptr, M68K_REG_SR)); }
void Machine::set_sr(uint16_t v) { m68k_set_reg(M68K_REG_SR, v); }

void Machine::do_rts() {
    uint32_t sp = areg(7);
    uint32_t ret = read32(sp);
    set_areg(7, sp + 4);
    set_pc(ret & 0xFFFFFF);
}

void Machine::add_trap(uint32_t addr, std::function<void(Machine&)> fn) {
    trap_fns_.push_back(std::move(fn));
    trap_index_[(addr & kChipMask) >> 1] = uint16_t(trap_fns_.size());
}

void Machine::instruction_hook(uint32_t pc) {
    if (trace) trace(*this, pc);
    if (tracer) tracer->on_instruction(pc);
    if (pc < kChipRamSize) {
        if (pc == 0x7032) {
            // $7032 is the target of every exception vector once the game has initialised. The first entry
            // comes from $400 (bra.w $7032); later ones mean an exception or a deliberate restart.
            ++restarts;
            last_restart_from = m68k_get_reg(nullptr, M68K_REG_PPC);
        }
        if (uint16_t t = trap_index_[pc >> 1]) {
            trap_fns_[t - 1](*this);
            // A trap that moved the CPU (an HLE shim's emulated RTS) skips the hook for the next instruction.
            if (tracer && this->pc() != pc) tracer->on_redirect(pc, this->pc());
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Bus

uint8_t Machine::read8(uint32_t addr) {
    addr &= 0xFFFFFF;
    if (addr < 0x200000) return chip[addr & kChipMask];
    if ((addr & 0xFF0000) == 0xBF0000) return cia_read(addr);
    if ((addr & 0xFFF000) == 0xDFF000) {
        uint16_t w = custom_read(addr & 0x1FE);
        return (addr & 1) ? uint8_t(w) : uint8_t(w >> 8);
    }
    ++unmapped_accesses;
    last_unmapped_addr = addr;
    return 0;
}

uint16_t Machine::read16(uint32_t addr) {
    addr &= 0xFFFFFF;
    if (addr < 0x200000) {
        uint32_t a = addr & kChipMask;
        return uint16_t((chip[a] << 8) | chip[(a + 1) & kChipMask]);
    }
    if ((addr & 0xFFF000) == 0xDFF000) return custom_read(addr & 0x1FE);
    return uint16_t((read8(addr) << 8) | read8(addr + 1));
}

uint32_t Machine::read32(uint32_t addr) {
    return (uint32_t(read16(addr)) << 16) | read16(addr + 2);
}

void Machine::write8(uint32_t addr, uint8_t v) {
    addr &= 0xFFFFFF;
    if (tracer) tracer->on_cpu_write(addr, 1);
    if (addr < 0x200000) { chip[addr & kChipMask] = v; return; }
    if ((addr & 0xFF0000) == 0xBF0000) { cia_write(addr, v); return; }
    if ((addr & 0xFFF000) == 0xDFF000) {
        // The 68000 drives a byte write on both halves of the data bus; the custom chips see a word write.
        custom_write(addr & 0x1FE, uint16_t((v << 8) | v));
        return;
    }
    ++unmapped_accesses;
    last_unmapped_addr = addr;
}

void Machine::write16(uint32_t addr, uint16_t v) {
    addr &= 0xFFFFFF;
    if (tracer) tracer->on_cpu_write(addr, 2);
    if (addr < 0x200000) {
        uint32_t a = addr & kChipMask;
        chip[a] = uint8_t(v >> 8);
        chip[(a + 1) & kChipMask] = uint8_t(v);
        return;
    }
    if ((addr & 0xFFF000) == 0xDFF000) { custom_write(addr & 0x1FE, v); return; }
    write8(addr, uint8_t(v >> 8));
    write8(addr + 1, uint8_t(v));
}

void Machine::write32(uint32_t addr, uint32_t v) {
    write16(addr, uint16_t(v >> 16));
    write16(addr + 2, uint16_t(v));
}

// CIA-A is selected by A12=0 and sits on the low data byte (odd addresses), CIA-B by A13=0 on the high byte
// (even addresses). The register number is A8-A11.
uint8_t Machine::cia_read(uint32_t addr) {
    int r = (addr >> 8) & 15;
    uint8_t v = 0xFF;
    if ((addr & 1) && !(addr & 0x1000)) {
        ++cia_reads[r];
        v = cia_a.read(r);
    } else if (!(addr & 1) && !(addr & 0x2000)) {
        ++cia_reads[16 + r];
        v = cia_b.read(r);
    }
    if (r == Cia::ICR) sync_cia_irq();
    return v;
}

void Machine::cia_write(uint32_t addr, uint8_t v) {
    int r = (addr >> 8) & 15;
    uint32_t pc_now = m68k_get_reg(nullptr, M68K_REG_PPC);
    if ((addr & 1) && !(addr & 0x1000)) {
        bool hs_before = cia_a.cra() & 0x40;
        if (r == Cia::PRA || r == Cia::DDRA) paula.run_until(cck_now());
        cia_a.write(r, v);
        // PA1 drives the power LED and the audio filter: output low = LED on = filter on.
        paula.set_led_filter((cia_a.ddra() & 2) && !(cia_a.pra_out() & 2));
        log_write(uint16_t(0x1000 | r), v, 'C', pc_now);
        if (r == Cia::CRA) {
            bool hs_after = cia_a.cra() & 0x40;
            // Keyboard handshake: the game pulses SPMODE (CRA bit 6) high then low to acknowledge a byte.
            if (hs_before && !hs_after && kb_wait_handshake_) kb_hs_seen_ = true;
        }
    }
    if (!(addr & 1) && !(addr & 0x2000)) {
        cia_b.write(r, v);
        log_write(uint16_t(0x2000 | r), v, 'C', pc_now);
    }
    sync_cia_irq();
}

// ---------------------------------------------------------------------------------------------------------
// Custom chips

uint16_t Machine::hpos() const {
    int c = line_cycles_ + (in_execute_ ? m68k_cycles_run() : 0);
    int h = c / 2;
    return uint16_t(h < kColorClocksPerLine ? h : kColorClocksPerLine - 1);
}

uint64_t Machine::cck_now() const {
    return (uint64_t(frame) * kLinesPerFrame + uint64_t(line)) * kColorClocksPerLine + hpos();
}

uint16_t Machine::custom_read(uint16_t off) {
    off &= 0x1FE;
    ++custom_reads[off >> 1];
    switch (off) {
    case reg::DMACONR:
        return uint16_t(dmacon | (blit_pending_ ? reg::DMAF_BLTDONE : 0) | (blit_zero_ ? reg::DMAF_BLTNZERO : 0));
    case reg::VPOSR: return uint16_t((line >> 8) & 1); // LOF=0, Agnus id 0 (OCS PAL)
    case reg::VHPOSR: return uint16_t(((line & 0xFF) << 8) | (hpos() & 0xFF));
    case reg::JOY0DAT: return joy0dat_;
    case reg::JOY1DAT: return joy1dat_;
    case reg::CLXDAT: return 0x8000;
    case reg::ADKCONR: return adkcon;
    case reg::POT0DAT:
    case reg::POT1DAT: return 0;
    case reg::POTGOR: return 0xFF00; // second/third buttons released
    case reg::SERDATR: return 0x3000; // TBE + TSRE: transmitter idle
    case reg::DSKBYTR: return 0;
    case reg::INTENAR: return intena;
    case reg::INTREQR: return intreq;
    default: return 0; // write-only registers read back as 0 here
    }
}

void Machine::log_write(uint16_t r, uint16_t value, char source, uint32_t pc_now) {
    if (!log_writes) return;
    frame_writes.push_back(RegWrite{frame, uint16_t(line), hpos(), pc_now, r, value, source});
}

void Machine::custom_write(uint16_t off, uint16_t v, char source) {
    off &= 0x1FE;
    log_write(off, v, source, source == 'P' ? cop_cur_insn_ : m68k_get_reg(nullptr, M68K_REG_PPC));
    if (on_custom_write) on_custom_write(off, v, line, hpos());
    if (off == reg::DMACON || off == reg::ADKCON || (off >= reg::AUD0LCH && off < 0xE0)) {
        // Bring Paula up to this colour clock so the write takes effect at the right sample.
        paula.run_until(cck_now());
        if (off >= reg::AUD0LCH) paula.write_reg(off, v);
    }
    switch (off) {
    case reg::DMACON: {
        setclr(dmacon, v);
        dmacon &= 0x07FF;
        regs[off >> 1] = v;
        paula.set_dmacon(dmacon);
        if (blit_pending_ && (dmacon & (reg::DMAF_MASTER | reg::DMAF_BLITTER)) == (reg::DMAF_MASTER | reg::DMAF_BLITTER))
            do_blit();
        return;
    }
    case reg::INTENA:
        setclr(intena, v);
        intena &= 0x7FFF;
        update_irq();
        return;
    case reg::INTREQ:
        setclr(intreq, v);
        intreq &= 0x7FFF;
        sync_cia_irq(); // a CIA still asserting /IRQ re-latches its INTREQ bit immediately
        update_irq();
        return;
    case reg::ADKCON:
        setclr(adkcon, v);
        adkcon &= 0x7FFF;
        paula.set_adkcon(adkcon);
        return;
    case reg::COPJMP1:
        copper_restart(reg_ptr(reg::COP1LCH));
        copper_run(line, hpos());
        return;
    case reg::COPJMP2:
        copper_restart(reg_ptr(reg::COP2LCH));
        copper_run(line, hpos());
        return;
    case reg::BLTSIZE:
        regs[off >> 1] = v;
        blit_pending_ = true;
        if ((dmacon & (reg::DMAF_MASTER | reg::DMAF_BLITTER)) == (reg::DMAF_MASTER | reg::DMAF_BLITTER)) do_blit();
        return;
    default:
        regs[off >> 1] = v;
        return;
    }
}

void Machine::update_irq() {
    int level = (intena & reg::INTF_INTEN) ? irq_level_for(intena & intreq & 0x3FFF) : 0;
    if (level == irq_level_) return;
    irq_level_ = level;
    m68k_set_irq(level);
    // Musashi only samples the IPL lines when m68k_execute() is entered; end the slice so a newly raised
    // level is taken after the current instruction.
    if (in_execute_ && level) m68k_end_timeslice();
}

void Machine::sync_cia_irq() {
    uint16_t before = intreq;
    if (cia_a.irq()) intreq |= reg::INTF_PORTS;
    if (cia_b.irq()) intreq |= reg::INTF_EXTER;
    if (intreq != before) update_irq();
}

// ---------------------------------------------------------------------------------------------------------
// Copper: runs in slots rather than cycle by cycle: at the start of each line (beam h = 0), every
// kCopperSlotCck colour clocks during the line, and at the end of the line (h = $E2). A WAIT therefore takes
// effect within one slot of its target position.

void Machine::copper_restart(uint32_t addr) {
    cop_pc_ = addr & kChipMask & ~1u;
    cop_waiting_ = false;
    cop_stopped_ = false;
    cop_skip_ = false;
}

void Machine::copper_run(int beam_line, int beam_h) {
    if (cop_stopped_) return;
    if ((dmacon & (reg::DMAF_MASTER | reg::DMAF_COPPER)) != (reg::DMAF_MASTER | reg::DMAF_COPPER)) return;
    auto beam_reached = [&](uint16_t ir1, uint16_t ir2) {
        uint16_t vp = ir1 >> 8, hp = ir1 & 0xFE;
        uint16_t ve = ((ir2 >> 8) & 0x7F) | 0x80, he = ir2 & 0xFE;
        uint16_t bv = uint16_t(beam_line & 0xFF) & ve, bh = uint16_t(beam_h) & he;
        vp &= ve;
        hp &= he;
        return bv > vp || (bv == vp && bh >= hp);
    };
    for (int guard = 0; guard < 4096; ++guard) {
        if (cop_waiting_) {
            if (!beam_reached(cop_wait_ir1_, cop_wait_ir2_)) return;
            cop_waiting_ = false;
        }
        uint16_t ir1 = uint16_t((chip[cop_pc_] << 8) | chip[cop_pc_ + 1]);
        uint16_t ir2 = uint16_t((chip[(cop_pc_ + 2) & kChipMask] << 8) | chip[(cop_pc_ + 3) & kChipMask]);
        cop_cur_insn_ = cop_pc_;
        cop_pc_ = (cop_pc_ + 4) & kChipMask;
        if (!(ir1 & 1)) { // MOVE
            if (cop_skip_) { cop_skip_ = false; continue; }
            uint16_t r = ir1 & 0x1FE;
            bool cdang = regs[reg::COPCON >> 1] & 2;
            if (r < 0x40 || (r < 0x80 && !cdang)) { cop_stopped_ = true; return; }
            ++copper_moves;
            custom_write(r, ir2, 'P');
            if (r == reg::COPJMP1 || r == reg::COPJMP2) return; // custom_write restarted and ran the copper
            continue;
        }
        if (ir1 == 0xFFFF && ir2 == 0xFFFE) { cop_stopped_ = true; return; } // end of list
        if (!(ir2 & 1)) { // WAIT
            cop_skip_ = false;
            cop_waiting_ = true;
            cop_wait_ir1_ = ir1;
            cop_wait_ir2_ = ir2;
        } else { // SKIP
            cop_skip_ = beam_reached(ir1, ir2);
        }
    }
    cop_stopped_ = true; // runaway list
}

// ---------------------------------------------------------------------------------------------------------
// Blitter: area and line mode, executed in zero time when BLTSIZE is written (DMACONR BBUSY therefore always
// reads 0).

void Machine::do_blit() {
    blit_pending_ = false;
    ++blits;
    uint16_t con0 = regs[reg::BLTCON0 >> 1], con1 = regs[reg::BLTCON1 >> 1];
    uint16_t size = regs[reg::BLTSIZE >> 1];
    int h = size >> 6, w = size & 63;
    if (h == 0) h = 1024;
    if (w == 0) w = 64;
    if (con1 & 1) {
        do_line_blit();
        return;
    }
    bool use_a = con0 & 0x800, use_b = con0 & 0x400, use_c = con0 & 0x200, use_d = con0 & 0x100;
    int ash = con0 >> 12, bsh = con1 >> 12;
    uint8_t minterm = uint8_t(con0);
    bool desc = con1 & 2;
    bool ife = con1 & 8, efe = con1 & 0x10, fci = con1 & 4;
    int inc = desc ? -2 : 2;
    uint32_t apt = reg_ptr(reg::BLTAPTH), bpt = reg_ptr(reg::BLTBPTH), cpt = reg_ptr(reg::BLTCPTH), dpt = reg_ptr(reg::BLTDPTH);
    int amod = int16_t(regs[reg::BLTAMOD >> 1]) & ~1, bmod = int16_t(regs[reg::BLTBMOD >> 1]) & ~1;
    int cmod = int16_t(regs[reg::BLTCMOD >> 1]) & ~1, dmod = int16_t(regs[reg::BLTDMOD >> 1]) & ~1;
    if (desc) { amod = -amod; bmod = -bmod; cmod = -cmod; dmod = -dmod; }
    uint16_t fwm = regs[reg::BLTAFWM >> 1], lwm = regs[reg::BLTALWM >> 1];
    uint16_t adat = regs[reg::BLTADAT >> 1], bdat = regs[reg::BLTBDAT >> 1], cdat = regs[reg::BLTCDAT >> 1];
    uint16_t a_old = 0, b_old = 0;
    bool zero = true;
    auto rd = [&](uint32_t a) { a &= kChipMask; return uint16_t((chip[a] << 8) | chip[(a + 1) & kChipMask]); };
    auto shift = [&](uint16_t cur, uint16_t old, int sh) -> uint16_t {
        if (!sh) return cur;
        if (!desc) return uint16_t(((uint32_t(old) << 16) | cur) >> sh);
        return uint16_t(((uint32_t(cur) << 16 | old) << sh) >> 16);
    };
    std::vector<uint16_t> row(static_cast<size_t>(w));
    uint32_t d_lo = kChipMask, d_hi = 0; // range written by D, reported to the tracer
    for (int y = 0; y < h; ++y) {
        uint32_t row_dpt = dpt;
        for (int x = 0; x < w; ++x) {
            if (use_a) { adat = rd(apt); apt += inc; }
            if (use_b) { bdat = rd(bpt); bpt += inc; }
            if (use_c) { cdat = rd(cpt); cpt += inc; }
            uint16_t am = adat;
            if (x == 0) am &= fwm;
            if (x == w - 1) am &= lwm;
            uint16_t as = shift(am, a_old, ash);
            uint16_t bs = shift(bdat, b_old, bsh);
            a_old = am;
            b_old = bdat;
            uint16_t d = 0;
            for (int m = 0; m < 8; ++m) {
                if (!(minterm & (1 << m))) continue;
                uint16_t t = (m & 4) ? as : uint16_t(~as);
                t &= (m & 2) ? bs : uint16_t(~bs);
                t &= (m & 1) ? cdat : uint16_t(~cdat);
                d |= t;
            }
            row[size_t(x)] = d;
            dpt += inc;
        }
        if (ife || efe) {
            // Fill runs from the rightmost word, bit 0 upwards (the blitter must be in descending mode).
            bool fc = fci;
            for (int x = 0; x < w; ++x) {
                uint16_t d = row[size_t(x)], out = 0;
                for (int i = 0; i < 16; ++i) {
                    bool bit = (d >> i) & 1;
                    bool o;
                    if (efe) { fc ^= bit; o = fc; }
                    else { o = fc || bit; fc ^= bit; }
                    out |= uint16_t(o) << i;
                }
                row[size_t(x)] = out;
            }
        }
        for (int x = 0; x < w; ++x) {
            uint16_t d = row[size_t(x)];
            if (d) zero = false;
            if (use_d) {
                uint32_t a = (row_dpt + uint32_t(x * inc)) & kChipMask;
                chip[a] = uint8_t(d >> 8);
                chip[(a + 1) & kChipMask] = uint8_t(d);
                if (tracer) { d_lo = std::min(d_lo, a); d_hi = std::max(d_hi, a + 1); }
            }
        }
        if (use_a) apt += amod;
        if (use_b) bpt += bmod;
        if (use_c) cpt += cmod;
        dpt += dmod;
    }
    set_reg_ptr(reg::BLTAPTH, apt);
    set_reg_ptr(reg::BLTBPTH, bpt);
    set_reg_ptr(reg::BLTCPTH, cpt);
    set_reg_ptr(reg::BLTDPTH, dpt);
    regs[reg::BLTADAT >> 1] = adat;
    regs[reg::BLTBDAT >> 1] = bdat;
    regs[reg::BLTCDAT >> 1] = cdat;
    blit_zero_ = zero;
    if (tracer && d_lo <= d_hi) tracer->on_dma_write(d_lo, d_hi);
    intreq |= reg::INTF_BLIT;
    update_irq();
}

// Line mode: one pixel per BLTSIZE row (height = length of the major axis + 1, width must be 2). BLTAPTL is the
// Bresenham error term (initially 4*sdelta - 2*ldelta, SIGN in BLTCON1 bit 6 = its sign), BLTBMOD = 4*sdelta is
// added while it is negative, BLTAMOD = 4*(sdelta-ldelta) otherwise (with a step on the minor axis).
// BLTCON1 SUD (bit 4) set = major axis is x; AUL (bit 2) / SUL (bit 3) reverse the major / minor step. A is the
// single-pixel mask BLTADAT ($8000) & BLTAFWM shifted right by ASH (the pixel's bit in the word); B is the texture,
// BLTBDAT rotated right by BSH, one bit per pixel (bit 0, rotating left after each pixel); C/D are the bitmap
// word at the C pointer, whose row stride is BLTCMOD. The first pixel is written to BLTDPT, the following ones
// to the C address they were read from. SING (bit 1) draws only the first pixel of each row, for fill outlines.
void Machine::do_line_blit() {
    ++line_blits;
    uint16_t con0 = regs[reg::BLTCON0 >> 1], con1 = regs[reg::BLTCON1 >> 1];
    int len = regs[reg::BLTSIZE >> 1] >> 6;
    if (len == 0) len = 1024;
    bool use_a = con0 & 0x800, use_c = con0 & 0x200, use_d = con0 & 0x100;
    uint8_t minterm = uint8_t(con0);
    bool sud = con1 & 0x10, sul = con1 & 0x08, aul = con1 & 0x04, sing = con1 & 0x02;
    int ash = con0 >> 12, bsh = con1 >> 12;
    uint32_t cpt = reg_ptr(reg::BLTCPTH), dpt = reg_ptr(reg::BLTDPTH);
    int16_t err = int16_t(regs[(reg::BLTAPTH >> 1) + 1]);
    int16_t amod = int16_t(regs[reg::BLTAMOD >> 1]), bmod = int16_t(regs[reg::BLTBMOD >> 1]);
    int cmod = int16_t(regs[reg::BLTCMOD >> 1]);
    bool sign = con1 & 0x40;
    uint16_t adat = uint16_t(regs[reg::BLTADAT >> 1] & regs[reg::BLTAFWM >> 1]);
    uint16_t bdat = regs[reg::BLTBDAT >> 1], cdat = regs[reg::BLTCDAT >> 1];
    uint16_t texture = bsh ? uint16_t((bdat >> bsh) | (bdat << (16 - bsh))) : bdat;
    bool dot_in_row = false, zero = true;
    auto step_x = [&](bool dec) {
        if (dec) { if (--ash < 0) { ash = 15; cpt -= 2; } }
        else if (++ash > 15) { ash = 0; cpt += 2; }
    };
    auto step_y = [&](bool dec) {
        cpt += uint32_t(dec ? -cmod : cmod);
        dot_in_row = false;
    };
    for (int i = 0; i < len; ++i) {
        cpt &= kChipMask & ~1u;
        if (use_c) cdat = uint16_t((chip[cpt] << 8) | chip[cpt + 1]);
        uint16_t a = uint16_t(adat >> ash);
        if (sing && dot_in_row) a = 0;
        dot_in_row = true;
        uint16_t b = (texture & 1) ? 0xFFFF : 0;
        uint16_t d = 0;
        for (int m = 0; m < 8; ++m) {
            if (!(minterm & (1 << m))) continue;
            uint16_t t = (m & 4) ? a : uint16_t(~a);
            t &= (m & 2) ? b : uint16_t(~b);
            t &= (m & 1) ? cdat : uint16_t(~cdat);
            d |= t;
        }
        if (d) zero = false;
        uint32_t dst = (i == 0 ? dpt : cpt) & kChipMask & ~1u;
        if (use_d) {
            chip[dst] = uint8_t(d >> 8);
            chip[dst + 1] = uint8_t(d);
        }
        dpt = dst;
        texture = uint16_t((texture << 1) | (texture >> 15));
        if (use_a) err = int16_t(err + (sign ? bmod : amod));
        if (!sign) { if (sud) step_y(sul); else step_x(sul); } // minor axis
        if (sud) step_x(aul); else step_y(aul);                  // major axis
        sign = err < 0;
    }
    regs[(reg::BLTAPTH >> 1) + 1] = uint16_t(err);
    regs[reg::BLTCON0 >> 1] = uint16_t((con0 & 0x0FFF) | (ash << 12));
    regs[reg::BLTCON1 >> 1] = uint16_t((con1 & ~0x40) | (sign ? 0x40 : 0));
    regs[reg::BLTCDAT >> 1] = cdat;
    set_reg_ptr(reg::BLTCPTH, cpt);
    set_reg_ptr(reg::BLTDPTH, dpt);
    blit_zero_ = zero;
    intreq |= reg::INTF_BLIT;
    update_irq();
}

// ---------------------------------------------------------------------------------------------------------
// Input

void Machine::key_event(uint8_t code, bool released) {
    key_queue_.push_back(uint8_t((code & 0x7F) | (released ? 0x80 : 0)));
}

namespace {
// JOYxDAT: bit 1 = right, bit 9 = left, bit 0 = right XOR down, bit 8 = left XOR up.
uint16_t joydat(bool up, bool down, bool left, bool right) {
    uint16_t v = 0;
    if (right) v |= 0x0002;
    if (left) v |= 0x0200;
    if (right != down) v |= 0x0001;
    if (left != up) v |= 0x0100;
    return v;
}
} // namespace

void Machine::set_joystick(bool up, bool down, bool left, bool right, bool fire) {
    joy1dat_ = joydat(up, down, left, right);
    fire1_ = fire;
    cia_a.ext_pra = uint8_t((cia_a.ext_pra & 0x7F) | (fire ? 0 : 0x80));
}

void Machine::set_joystick_port1(bool up, bool down, bool left, bool right, bool fire) {
    joy0dat_ = joydat(up, down, left, right);
    cia_a.ext_pra = uint8_t((cia_a.ext_pra & 0xBF) | (fire ? 0 : 0x40));
}

void Machine::keyboard_step() {
    if (kb_cooldown_ > 0) { --kb_cooldown_; return; }
    if (kb_wait_handshake_) {
        if (kb_hs_seen_ || ++kb_wait_lines_ > kLinesPerFrame * 10) {
            kb_wait_handshake_ = false;
            kb_hs_seen_ = false;
            kb_cooldown_ = 2;
        }
        return;
    }
    if (key_queue_.empty()) return;
    uint8_t code = key_queue_.front();
    key_queue_.pop_front();
    // The keyboard sends bits 6..0 then the up/down bit, active low: SDR = ~rol(code, 1).
    uint8_t wire = uint8_t(~((code << 1) | (code >> 7)));
    cia_a.serial_in(wire);
    kb_wait_handshake_ = true;
    kb_wait_lines_ = 0;
    sync_cia_irq();
}

// ---------------------------------------------------------------------------------------------------------
// Frame loop

void Machine::latch_display() {
    display.valid = true;
    for (int i = 0; i < 6; ++i) display.bplpt[i] = reg_ptr(uint16_t(reg::BPL1PTH + 4 * i));
    display.bplcon0 = regs[reg::BPLCON0 >> 1];
    display.bplcon1 = regs[reg::BPLCON1 >> 1];
    display.bpl1mod = regs[reg::BPL1MOD >> 1];
    display.bpl2mod = regs[reg::BPL2MOD >> 1];
    display.diwstrt = regs[reg::DIWSTRT >> 1];
    display.diwstop = regs[reg::DIWSTOP >> 1];
    display.ddfstrt = regs[reg::DDFSTRT >> 1];
    display.ddfstop = regs[reg::DDFSTOP >> 1];
    display.dmacon = dmacon;
    for (int i = 0; i < 32; ++i) display.color[i] = regs[(reg::COLOR00 >> 1) + i];
}

// Bitplane DMA advances BPLxPT by (fetch width + modulo) per displayed line; apply it in one step when the
// window closes so the pointer registers end the frame where the hardware would leave them.
void Machine::advance_bitplane_pointers() {
    if ((dmacon & (reg::DMAF_MASTER | reg::DMAF_RASTER)) != (reg::DMAF_MASTER | reg::DMAF_RASTER)) return;
    int planes = (regs[reg::BPLCON0 >> 1] >> 12) & 7;
    int vstart = regs[reg::DIWSTRT >> 1] >> 8;
    int vstop = regs[reg::DIWSTOP >> 1] >> 8;
    if (!(vstop & 0x80)) vstop |= 0x100;
    int lines = vstop - vstart;
    int ddf0 = regs[reg::DDFSTRT >> 1] & 0xFC, ddf1 = regs[reg::DDFSTOP >> 1] & 0xFC;
    int words = ddf1 >= ddf0 ? (ddf1 - ddf0) / 8 + 1 : 0;
    if (lines <= 0 || words <= 0) return;
    for (int i = 0; i < planes && i < 6; ++i) {
        int mod = int16_t(regs[(i & 1 ? reg::BPL2MOD : reg::BPL1MOD) >> 1]);
        uint16_t off = uint16_t(reg::BPL1PTH + 4 * i);
        set_reg_ptr(off, reg_ptr(off) + uint32_t(lines * (words * 2 + mod)));
    }
}

void Machine::line_start() {
    if (line == 0) {
        intreq |= reg::INTF_VERTB;
        cia_a.tod_pulse(); // CIA-A TOD: 50 Hz tick
        copper_restart(reg_ptr(reg::COP1LCH));
        display.valid = false;
        sync_cia_irq();
        update_irq();
    }
    copper_run(line, 0);
    int vstart = regs[reg::DIWSTRT >> 1] >> 8;
    if (line == vstart) latch_display();
    int vstop = regs[reg::DIWSTOP >> 1] >> 8;
    if (!(vstop & 0x80)) vstop |= 0x100;
    if (line == vstop) advance_bitplane_pointers();
    keyboard_step();
}

void Machine::line_end() {
    copper_run(line, 0xE2);
    if (on_line_end) on_line_end(*this, line);
    paula.run_until((uint64_t(frame) * kLinesPerFrame + uint64_t(line) + 1) * kColorClocksPerLine);
    cia_b.tod_pulse(); // CIA-B TOD: hsync
    cia_eclock_acc_ += kCpuCyclesPerLine;
    uint32_t ticks = cia_eclock_acc_ / 10;
    cia_eclock_acc_ %= 10;
    cia_a.tick(ticks);
    cia_b.tick(ticks);
    sync_cia_irq();
}

int Machine::run_cycles(int cycles) {
    int done = 0;
    line_cycles_ = 0;
    while (done < cycles) {
        in_execute_ = true;
        int n = cpu_execute(cycles - done);
        in_execute_ = false;
        done += n;
        line_cycles_ += n;
    }
    return done;
}

void Machine::run_frame() {
    for (line = 0; line < kLinesPerFrame; ++line) {
        line_cycles_ = 0;
        line_start();
        int budget = kCpuCyclesPerLine - cpu_debt_;
        // Besides the start and end of the line, the copper gets a slot every kCopperSlotCck colour clocks, so a
        // WAIT for a horizontal position takes effect within that many colour clocks of it.
        for (int slot_end = 2 * kCopperSlotCck;; slot_end += 2 * kCopperSlotCck) {
            int seg_end = slot_end < budget ? slot_end : budget;
            while (line_cycles_ < seg_end) {
                in_execute_ = true;
                int n = cpu_execute(seg_end - line_cycles_);
                in_execute_ = false;
                line_cycles_ += n;
            }
            if (seg_end == budget) break;
            copper_run(line, hpos());
        }
        cpu_debt_ = line_cycles_ - budget;
        ++pc_samples[pc()];
        line_end();
    }
    line = 0;
    ++frame;
    if (on_frame_end) on_frame_end(*this);
}

} // namespace x2

// ---------------------------------------------------------------------------------------------------------
// Musashi callbacks

extern "C" {
static void x2_bus_log(unsigned int a, unsigned int v, int size, char kind) {
    x2::g_machine->bus_log->push_back(
        x2::BusAccess{a, v, m68k_cycles_run(), m68k_get_reg(nullptr, M68K_REG_PC), uint8_t(size), kind});
}
unsigned int m68k_read_memory_8(unsigned int a) {
    unsigned int v = x2::g_machine->read8(a);
    if (x2::g_machine->bus_log) x2_bus_log(a, v, 1, 'R');
    return v;
}
unsigned int m68k_read_memory_16(unsigned int a) {
    unsigned int v = x2::g_machine->read16(a);
    if (x2::g_machine->bus_log) x2_bus_log(a, v, 2, 'R');
    return v;
}
unsigned int m68k_read_memory_32(unsigned int a) {
    unsigned int v = x2::g_machine->read32(a);
    if (x2::g_machine->bus_log) x2_bus_log(a, v, 4, 'R');
    return v;
}
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    if (x2::g_machine->bus_log) x2_bus_log(a, v & 0xFF, 1, 'W');
    x2::g_machine->write8(a, uint8_t(v));
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    if (x2::g_machine->bus_log) x2_bus_log(a, v & 0xFFFF, 2, 'W');
    x2::g_machine->write16(a, uint16_t(v));
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    if (x2::g_machine->bus_log) x2_bus_log(a, v, 4, 'W');
    x2::g_machine->write32(a, v);
}
// The disassembler must not trigger side effects (CIA ICR reads etc.), so it only sees chip RAM.
unsigned int m68k_read_disassembler_8(unsigned int a) { return x2::g_machine->chip[a & x2::kChipMask]; }
unsigned int m68k_read_disassembler_16(unsigned int a) {
    auto& c = x2::g_machine->chip;
    return (c[a & x2::kChipMask] << 8) | c[(a + 1) & x2::kChipMask];
}
unsigned int m68k_read_disassembler_32(unsigned int a) {
    return (m68k_read_disassembler_16(a) << 16) | m68k_read_disassembler_16(a + 2);
}
void x2_instruction_hook(unsigned int pc) { x2::g_machine->instruction_hook(pc & 0xFFFFFF); }
}
