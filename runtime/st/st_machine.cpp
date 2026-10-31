// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "st/st_machine.h"

#include "m68k.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace x2::st {

namespace {
StMachine* g_st = nullptr;

uint32_t h_read8(uint32_t a) { return g_st->read8(a); }
uint32_t h_read16(uint32_t a) { return g_st->read16(a); }
uint32_t h_read32(uint32_t a) { return g_st->read32(a); }
void h_write8(uint32_t a, uint32_t v) { g_st->write8(a, v); }
void h_write16(uint32_t a, uint32_t v) { g_st->write16(a, v); }
void h_write32(uint32_t a, uint32_t v) { g_st->write32(a, v); }
uint32_t h_dasm8(uint32_t a) { return g_st->ram_size() > (a & 0xFFFFFF) ? g_st->ram()[a & 0xFFFFFF] : 0; }
uint32_t h_dasm16(uint32_t a) { return (h_dasm8(a) << 8) | h_dasm8(a + 1); }
uint32_t h_ack(int level) { return g_st->int_ack(level); }

constexpr uint32_t kAutoVector = 0xFFFFFFFFu; // M68K_INT_ACK_AUTOVECTOR
constexpr uint32_t kSpurious = 0xFFFFFFFEu;   // M68K_INT_ACK_SPURIOUS
constexpr uint32_t kRomSize = 0x30000;
} // namespace

StMachine::StMachine(const StConfig& cfg)
    : ram_(cfg.ram_bytes == 2 * kRamDefault ? cfg.ram_bytes : kRamDefault, 0), rom_(kRomSize, 0),
      picture_(size_t(kFbWidth) * kFbHeight * 4, 0) {
    g_st = this;
    hooks_.read8 = h_read8;
    hooks_.read16 = h_read16;
    hooks_.read32 = h_read32;
    hooks_.write8 = h_write8;
    hooks_.write16 = h_write16;
    hooks_.write32 = h_write32;
    hooks_.dasm8 = h_dasm8;
    hooks_.dasm16 = h_dasm16;
    hooks_.int_ack = h_ack;
    g_bus_hooks = &hooks_;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    for (size_t i = 3; i < picture_.size(); i += 4) picture_[i] = 0xFF;
    fdc_.set_ram(ram_.data(), uint32_t(ram_.size()));
    fdc_.set_speed(cfg.fdc_speed);
    ym_.set_sound(cfg.audio_rate, cfg.audio_filters);
    fdc_.set_irq_callback([this](bool asserted, uint64_t at) { on_fdc_irq(asserted, at); });
    // The stub system ROM: a header word, a reset target, RTE for interrupts, a STOP for CPU exceptions.
    rom_[0] = 0x60; rom_[1] = 0x1E; // bra.s, as a TOS header starts
    auto put16 = [&](uint32_t at, uint16_t v) { rom_[at] = uint8_t(v >> 8); rom_[at + 1] = uint8_t(v); };
    put16(0x20, 0x4EF9); put16(0x22, 0x00FC); put16(0x24, 0x0020); // jmp $FC0020: a reset loop
    put16(kRomRte - kRomBase, 0x4E73);                             // rte
    put16(kRomFatal - kRomBase, 0x4E72); put16(kRomFatal - kRomBase + 2, 0x2700); // stop #$2700
    put16(kRomFatal - kRomBase + 4, 0x60FA);                       // bra.s back to the stop
}

StMachine::~StMachine() {
    if (g_st == this) {
        g_st = nullptr;
        g_bus_hooks = nullptr;
    }
}

void StMachine::insert_disk(StDisk disk) {
    disk_ = std::move(disk);
    fdc_.set_disk(&disk_);
}

void StMachine::set_instruction_probe(std::function<void(StMachine&, uint32_t pc)> fn) {
    insn_probe_ = std::move(fn);
    hooks_.insn = insn_probe_ ? +[](uint32_t pc) { if (g_st) g_st->probe(pc); } : nullptr;
}

uint32_t StMachine::pc() const { return m68k_get_reg(nullptr, M68K_REG_PC); }

std::string StMachine::fault_report() const {
    char buf[256];
    std::string out;
    std::snprintf(buf, sizeof buf, "sr=$%04X sp=$%06X", m68k_get_reg(nullptr, M68K_REG_SR), m68k_get_reg(nullptr, M68K_REG_A7));
    out += buf;
    for (int i = 0; i < 8; ++i) {
        std::snprintf(buf, sizeof buf, " d%d=$%08X", i, m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i)));
        out += buf;
    }
    out += "\n  ";
    for (int i = 0; i < 8; ++i) {
        std::snprintf(buf, sizeof buf, " a%d=$%08X", i, m68k_get_reg(nullptr, m68k_register_t(M68K_REG_A0 + i)));
        out += buf;
    }
    const uint32_t sp = m68k_get_reg(nullptr, M68K_REG_A7);
    out += "\n  stack:";
    for (uint32_t i = 0; i < 8; ++i) {
        std::snprintf(buf, sizeof buf, " %08X", read32_ram(sp + i * 4));
        out += buf;
    }
    return out;
}

uint32_t StMachine::read32_ram(uint32_t a) const {
    if (a + 3 >= ram_.size()) return 0;
    return (uint32_t(ram_[a]) << 24) | (uint32_t(ram_[a + 1]) << 16) | (uint32_t(ram_[a + 2]) << 8) | ram_[a + 3];
}

uint64_t StMachine::now() const { return in_execute_ ? cycles_ + uint64_t(m68k_cycles_run()) : cycles_; }

void StMachine::beam(uint64_t at, int& line, int& cycle) const {
    const uint64_t rel = at > frame_start_ ? at - frame_start_ : 0;
    uint64_t l = rel / kLineCycles;
    if (l >= uint64_t(kFrameLines)) l = kFrameLines - 1;
    line = int(l);
    cycle = int(rel - l * kLineCycles);
    if (cycle >= kLineCycles) cycle = kLineCycles - 1;
}

uint64_t StMachine::picture_hash() const {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : picture_) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

// ---------------------------------------------------------------------------------------------------------
// Boot

void StMachine::install_vectors() {
    auto put32 = [&](uint32_t at, uint32_t v) {
        ram_[at] = uint8_t(v >> 24); ram_[at + 1] = uint8_t(v >> 16); ram_[at + 2] = uint8_t(v >> 8); ram_[at + 3] = uint8_t(v);
    };
    put32(0, 0x00010000);       // the ROM mirror at 0: initial SSP and PC
    put32(4, kRomBase + 0x20);
    for (uint32_t v = 2; v < 64; ++v) put32(v * 4, kRomFatal); // CPU exceptions, autovector slots (overwritten below) and traps
    for (uint32_t v = 25; v <= 31; ++v) put32(v * 4, kRomRte);  // the interrupt autovectors $64-$7C
    for (uint32_t v = 64; v < 80; ++v) put32(v * 4, kRomRte);   // the MFP vectors $100-$13C
}

bool StMachine::boot(std::string& why) {
    if (!disk_.valid()) { why = "no valid disk in the drive"; return false; }
    const uint8_t* bs = disk_.sector(0, 0, 1);
    if (!bs) { why = "the disk has no boot sector"; return false; }
    if (!boot_sector_executable(bs)) { why = "the boot sector is not executable (its word checksum is not $1234)"; return false; }
    if (ram_.size() < kBootBase + 512) { why = "not enough RAM for the boot sector"; return false; }
    std::fill(ram_.begin(), ram_.end(), uint8_t(0));
    install_vectors();
    auto put32 = [&](uint32_t at, uint32_t v) {
        ram_[at] = uint8_t(v >> 24); ram_[at + 1] = uint8_t(v >> 16); ram_[at + 2] = uint8_t(v >> 8); ram_[at + 3] = uint8_t(v);
    };
    const uint32_t phystop = uint32_t(ram_.size());
    const uint32_t screen = phystop - 0x8000;
    put32(0x420, 0x752019F3);       // memvalid
    ram_[0x425] = mem_config_;      // memctrl
    put32(0x42E, phystop);          // phystop
    put32(0x432, 0x00000800);       // _membot
    put32(0x436, screen);           // _memtop
    put32(0x43A, 0x237698AA);       // memval2
    ram_[0x447] = 0;                // _bootdev = A:
    put32(0x44E, screen);           // _v_bas_ad
    ram_[0x4A7] = 1;                // _nflops
    put32(0x4C2, 1);                // _drvbits: A:
    put32(0x4C6, kBootBase);        // _dskbufp
    put32(0x4F2, kRomBase);         // _sysbase
    shifter_.reset();
    shifter_.write8(0x01, uint8_t(screen >> 16));
    shifter_.write8(0x03, uint8_t(screen >> 8));
    mfp_.reset();
    mfp_.write(11, 0x48, cycles_);       // VR: vector base $40, software end of interrupt, as TOS leaves it
    ikbd_.reset(cycles_);
    ikbd_.write_control(0x96, cycles_);  // keyboard ACIA: 8N1, divide by 64, receive interrupt enabled, as TOS leaves it
    ym_.reset();
    std::memcpy(ram_.data() + kBootBase, bs, 512);
    fdc_.set_disk(&disk_);
    fdc_.set_ram(ram_.data(), uint32_t(ram_.size()));
    vbl_pending_ = false;
    irq_level_ = 0;
    mfp_.set_input(5, true, cycles_);  // INTRQ idle (active low)
    mfp_.set_input(4, true, cycles_);  // ACIA idle
    m68k_pulse_reset();                // reads SSP/PC from the vectors above, through the bus
    m68k_set_reg(M68K_REG_SR, 0x2300); // supervisor, interrupt mask 3 as TOS hands over
    m68k_set_reg(M68K_REG_ISP, kBootBase);
    m68k_set_reg(M68K_REG_A7, kBootBase);
    m68k_set_reg(M68K_REG_PC, kBootBase);
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Interrupts, events and the frame loop

void StMachine::on_fdc_irq(bool asserted, uint64_t at) { mfp_.set_input(5, !asserted, at); }

void StMachine::update_irq() {
    const uint64_t t = now();
    mfp_.set_input(4, !ikbd_.irq(), t); // the ACIA's interrupt request: GPIP bit 4, active low
    int level = 0;
    if (vbl_pending_) level = 4;
    if (mfp_.irq()) level = 6;
    if (level != irq_level_) {
        irq_level_ = level;
        m68k_set_irq(level);
    }
}

uint32_t StMachine::int_ack(int level) {
    if (level == 4) {
        vbl_pending_ = false;
        ++vbl_taken_;
        update_irq();
        return kAutoVector;
    }
    if (level == 6) {
        mfp_.advance(now());
        const int v = mfp_.acknowledge();
        ++mfp_taken_;
        update_irq();
        return v < 0 ? kSpurious : uint32_t(v);
    }
    return kAutoVector;
}

void StMachine::process_events() {
    mfp_.advance(cycles_);
    fdc_.run_to(cycles_);
    ikbd_.run_to(cycles_);
    update_irq();
}

uint64_t StMachine::next_event_time() const {
    return std::min({mfp_.next_event(), fdc_.next_event(), ikbd_.next_event()});
}

void StMachine::run_cpu_until(uint64_t target) {
    while (cycles_ < target) {
        const uint64_t ev = next_event_time();
        if (ev <= cycles_) process_events();
        uint64_t stop = std::min(target, next_event_time());
        if (stop <= cycles_) stop = std::min(target, cycles_ + 1);
        const int want = int(std::max<uint64_t>(1, stop - cycles_));
        in_execute_ = true;
        const int ran = m68k_execute(want);
        in_execute_ = false;
        cycles_ += uint64_t(std::max(ran, 1));
        const uint32_t p = pc();
        if (p >= kRomFatal && p < kRomFatal + 8) ++fatal_hits_; // a CPU exception landed on the stub: the program is dead
        process_events();
    }
}

void StMachine::line_begin(int line) {
    if (line == 0) vbl_pending_ = true;
    if (line == kFirstDisplayLine) {
        latch_base_ = shifter_.base();
        latch_palette_ = shifter_.palette_array();
        latch_res_ = shifter_.resolution();
        if (latch_res_ != last_res_) {
            ++res_changes_;
            last_res_ = latch_res_;
        }
    }
    if ((m68k_get_reg(nullptr, M68K_REG_SR) & 0x0700) < 0x0200) ++hbl_unmasked_;
    update_irq();
}

void StMachine::run_frame() {
    ym_.samples().clear();
    frame_start_ = uint64_t(frame_) * kCyclesPerFrame;
    if (cycles_ > frame_start_ + 1024) frame_start_ = cycles_ - (cycles_ % kLineCycles); // resynchronise after a pause in driving
    for (int line = 0; line < kFrameLines; ++line) {
        line_begin(line);
        run_cpu_until(frame_start_ + uint64_t(line + 1) * kLineCycles);
        if (on_line_end) on_line_end(*this, line);
    }
    if (latch_res_ != 0) ++shifter_.unsupported_resolution_frames;
    Shifter::render(ram_.data(), uint32_t(ram_.size()), latch_base_, latch_palette_, latch_res_, picture_);
    ym_.render_to(frame_start_ + kCyclesPerFrame);
    ++frame_;
    if (on_frame_end) on_frame_end(*this);
}

// ---------------------------------------------------------------------------------------------------------
// Bus

void StMachine::unmapped(uint32_t a) {
    ++unmapped_;
    last_unmapped_ = a;
    if (unmapped_ <= 8)
        std::fprintf(stderr, "st: unmapped access at $%06X (pc $%06X)\n", a, uint32_t(m68k_get_reg(nullptr, M68K_REG_PPC)));
}

void StMachine::note_io(uint32_t a, bool write, uint32_t value) {
    IoStat& s = io_[a];
    const uint32_t pcv = uint32_t(m68k_get_reg(nullptr, M68K_REG_PPC));
    if (write) {
        if (!s.writes) s.first_write_pc = pcv;
        ++s.writes;
        s.last_value = value;
    } else {
        if (!s.reads) s.first_read_pc = pcv;
        ++s.reads;
    }
}

uint32_t StMachine::io_read8(uint32_t a) {
    const uint64_t t = now();
    if (io_summary_on_) note_io(a, false, 0);
    uint32_t v = 0xFF;
    if (a == 0xFF8001) return mem_config_;
    if (a >= 0xFF8200 && a < 0xFF8300) {
        int line, cyc;
        beam(t, line, cyc);
        return shifter_.read8(a - 0xFF8200, line, cyc);
    }
    if (a >= 0xFF8600 && a < 0xFF8610) { v = fdc_.read8(a - 0xFF8600, t); update_irq(); return v; }
    if (a >= 0xFF8800 && a < 0xFF8900) return ym_.read8((a - 0xFF8800) & 3);
    if (a >= 0xFFFA00 && a < 0xFFFA30) {
        if (!(a & 1)) return 0xFF;
        v = mfp_.read((a - 0xFFFA00) >> 1, t);
        update_irq();
        return v;
    }
    if (a >= 0xFFFC00 && a < 0xFFFC08) {
        if (a & 1) return 0xFF;
        switch (a) {
        case 0xFFFC00: v = ikbd_.status(t); break;
        case 0xFFFC02: v = ikbd_.read_data(t); break;
        case 0xFFFC04: v = 0x02; break; // the MIDI ACIA: transmit register empty, nothing received
        default: v = 0; break;
        }
        update_irq();
        return v;
    }
    unmapped(a);
    return 0xFF;
}

void StMachine::io_write8(uint32_t a, uint32_t v) {
    const uint64_t t = now();
    v &= 0xFF;
    if (io_summary_on_) note_io(a, true, v);
    if (a == 0xFF8001) { mem_config_ = uint8_t(v); return; }
    if (a >= 0xFF8200 && a < 0xFF8300) { shifter_.write8(a - 0xFF8200, uint8_t(v)); return; }
    if (a >= 0xFF8600 && a < 0xFF8610) { fdc_.write8(a - 0xFF8600, uint8_t(v), t); update_irq(); return; }
    if (a >= 0xFF8800 && a < 0xFF8900) {
        ym_.write8((a - 0xFF8800) & 3, uint8_t(v), t);
        fdc_.set_side((ym_.port_a() & 1) ? 0 : 1);
        return;
    }
    if (a >= 0xFFFA00 && a < 0xFFFA30) {
        if (a & 1) { mfp_.write((a - 0xFFFA00) >> 1, uint8_t(v), t); update_irq(); }
        return;
    }
    if (a >= 0xFFFC00 && a < 0xFFFC08) {
        if (a & 1) return;
        if (a == 0xFFFC00) ikbd_.write_control(uint8_t(v), t);
        else if (a == 0xFFFC02) ikbd_.write_data(uint8_t(v), t);
        update_irq();
        return;
    }
    unmapped(a);
}

uint32_t StMachine::io_read16(uint32_t a) {
    const uint64_t t = now();
    if (a == 0xFF8604 || a == 0xFF8606) {
        if (io_summary_on_) note_io(a, false, 0);
        const uint16_t v = fdc_.read16(a - 0xFF8600, t);
        update_irq();
        return v;
    }
    if (a >= 0xFF8240 && a < 0xFF8260 && !(a & 1)) {
        if (io_summary_on_) note_io(a, false, 0);
        return shifter_.read16(a - 0xFF8200, 0, 0);
    }
    return (io_read8(a) << 8) | io_read8(a + 1);
}

void StMachine::io_write16(uint32_t a, uint32_t v) {
    const uint64_t t = now();
    if (a == 0xFF8604 || a == 0xFF8606) {
        if (io_summary_on_) note_io(a, true, v);
        fdc_.write16(a - 0xFF8600, uint16_t(v), t);
        update_irq();
        return;
    }
    if (a >= 0xFF8240 && a < 0xFF8260 && !(a & 1)) {
        if (io_summary_on_) note_io(a, true, v);
        shifter_.write16(a - 0xFF8200, uint16_t(v));
        return;
    }
    io_write8(a, (v >> 8) & 0xFF);
    io_write8(a + 1, v & 0xFF);
}

uint32_t StMachine::read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < ram_.size()) return ram_[a];
    if (a >= 0xFF8000) return io_read8(a);
    if (a >= kRomBase && a < kRomBase + rom_.size()) return rom_[a - kRomBase];
    if (a >= 0xFA0000 && a < 0xFC0000) return 0xFF; // no cartridge
    unmapped(a);
    return 0xFF;
}

uint32_t StMachine::read16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a + 1 < ram_.size()) return (uint32_t(ram_[a]) << 8) | ram_[a + 1];
    if (a >= 0xFF8000) return io_read16(a);
    return (read8(a) << 8) | read8(a + 1);
}

uint32_t StMachine::read32(uint32_t a) { return (read16(a) << 16) | read16(a + 2); }

void StMachine::write8(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    if (a < ram_.size()) { ram_[a] = uint8_t(v); return; }
    if (a >= 0xFF8000) { io_write8(a, v); return; }
    if (a >= kRomBase && a < kRomBase + rom_.size()) return; // ROM: ignored
    if (a >= 0xFA0000 && a < 0xFC0000) return;
    unmapped(a);
}

void StMachine::write16(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    if (a + 1 < ram_.size()) { ram_[a] = uint8_t(v >> 8); ram_[a + 1] = uint8_t(v); return; }
    if (a >= 0xFF8000) { io_write16(a, v); return; }
    write8(a, (v >> 8) & 0xFF);
    write8(a + 1, v & 0xFF);
}

void StMachine::write32(uint32_t a, uint32_t v) {
    write16(a, v >> 16);
    write16(a + 2, v & 0xFFFF);
}

} // namespace x2::st
