// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// An Atari ST (PAL, 8 MHz 68000) around the Musashi core, enough for the Xenon 2 ST build: RAM, a 24-bit bus with the
// ST's I/O page, the Shifter, the MFP (timers, interrupt controller), the keyboard ACIA and IKBD, the YM2149 register
// file and the WD1772 floppy controller with its DMA, a line/frame scheduler (313 lines of 512 cycles) with the vertical
// blank on level 4 and the MFP on level 6 (vectored), and the hand-over TOS leaves at the end of its boot sequence. There
// is no TOS: after the boot sector nothing in the game calls it (docs/re-notes/atari-st-requirements.md), and the
// machine starts the real boot sector from the real disk image on the emulated controller.
//
// The machine talks to Musashi through the BusHooks seam (runtime/bus_hooks.h): one machine kind per process, and an
// StMachine must not coexist with an Amiga Machine. Time is the CPU cycle count (8,021,247 per second); events (MFP timer
// underflows, FDC and ACIA completions, line boundaries) end the CPU's slices so devices see the CPU at cycle accuracy
// to within one instruction. Instruction timing is Musashi's nominal 68000 table with each instruction rounded up to a
// multiple of 4 cycles (the ST's bus gives the CPU a memory slot every 4 cycles; StConfig::bus_rounding, S3), and the
// display is drawn line by line as the beam passes (shifter.h; docs/re-notes/atari-st-machine.md, atari-st-video.md).
#pragma once
#include "bus_hooks.h"
#include "st/fdc.h"
#include "st/ikbd.h"
#include "st/mfp.h"
#include "st/shifter.h"
#include "st/st_disk.h"
#include "st/ym2149.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace x2::st {

constexpr uint32_t kRamDefault = 512 * 1024;
constexpr uint32_t kBootBase = 0x70000;       // where the boot sector is loaded and run (TOS's disk buffer)
constexpr uint32_t kRomBase = 0xFC0000;       // the (stub) system ROM: 192 KB
constexpr uint32_t kRomRte = 0xFC0200;        // the stub every interrupt vector starts out at: RTE
constexpr uint32_t kRomFatal = 0xFC0210;      // the stub CPU exception vectors start out at: STOP, a visible fault
constexpr uint32_t kCyclesPerFrame = uint32_t(kFrameLines) * kLineCycles;

struct StConfig {
    uint32_t ram_bytes = kRamDefault; // 512 KB or 1 MB
    uint32_t fdc_speed = 1;           // 1 = a real drive's speed, N = N times faster
    uint32_t audio_rate = 0;          // host sample rate of the YM output (Hz); 0 = no sound is generated
    bool audio_filters = true;        // the AC coupling and output low-pass (tests turn them off)
    bool io_waits = true;             // device wait states (YM2149, MFP, ACIA)
    bool bus_rounding = true;         // round every instruction up to a multiple of 4 cycles (the ST's bus-slot wait states)
};

// The video counter is read about this many cycles into the reading instruction (the core reports an instruction's
// time at its start; the bus read happens later in it).
constexpr int kCounterReadSkew = 8;

struct IoStat {
    uint64_t reads = 0, writes = 0;
    uint32_t first_read_pc = 0, first_write_pc = 0;
    uint32_t last_value = 0;
};

class StMachine {
public:
    explicit StMachine(const StConfig& cfg = {});
    ~StMachine();
    StMachine(const StMachine&) = delete;
    StMachine& operator=(const StMachine&) = delete;

    // Insert the disk (drive A). The machine keeps it.
    void insert_disk(StDisk disk);
    // Do what TOS does at the end of its boot sequence: clear RAM, set up the vectors and system variables, read the
    // boot sector, check it is executable, and start it in supervisor mode. False with `why` for a disk that does not boot.
    bool boot(std::string& why);

    // One 313-line frame. The vertical blank is raised at line 0, the video counter is reloaded from the base register at
    // line 310 cycle 56, and each display line (63-262) is decoded into the picture as its last cycle passes.
    void run_frame();

    // --- state ---
    uint32_t ram_size() const { return uint32_t(ram_.size()); }
    uint8_t* ram() { return ram_.data(); }
    const uint8_t* ram() const { return ram_.data(); }
    uint32_t frame() const { return frame_; }
    uint64_t cycles() const { return cycles_; }
    uint64_t now() const;              // the cycle counter including the instruction in flight
    void beam(uint64_t at, int& line, int& cycle) const;
    // The YM output of the last frame (mono int16 at StConfig::audio_rate; empty when the sound is off).
    const std::vector<int16_t>& audio() const { return ym_.samples(); }
    const std::vector<uint8_t>& picture() const { return picture_; } // RGBA8 320x200 of the last completed frame
    uint64_t picture_hash() const;
    Shifter& shifter() { return shifter_; }
    Mfp& mfp() { return mfp_; }
    FdcDma& fdc() { return fdc_; }
    Ikbd& ikbd() { return ikbd_; }
    const Ikbd& ikbd() const { return ikbd_; }
    Ym2149& ym() { return ym_; }
    uint32_t pc() const;
    uint32_t sr() const;
    uint32_t read32_ram(uint32_t addr) const; // side-effect free big-endian read of RAM
    // Registers and the exception frame on the stack, for the message when the program faults.
    std::string fault_report() const;

    // --- input (host to IKBD) ---
    void set_joystick(int port, uint8_t state) { ikbd_.joystick(port, state, now()); }
    void key_event(uint8_t scancode, bool released) { ikbd_.key(scancode, released, now()); }

    // --- instrumentation ---
    uint64_t vbl_taken() const { return vbl_taken_; }
    uint64_t mfp_taken() const { return mfp_taken_; }
    uint64_t unmapped_accesses() const { return unmapped_; }
    uint32_t last_unmapped_address() const { return last_unmapped_; }
    uint64_t fatal_vector_hits() const { return fatal_hits_; }   // CPU slices that ended on the stub for CPU exceptions
    uint64_t hbl_unmasked_lines() const { return hbl_unmasked_; } // lines that began with the CPU's mask below 2 (an HBL would fire)
    uint64_t resolution_changes() const { return res_changes_; }
    uint64_t rounded_instructions() const { return rounded_; } // instructions whose time was rounded up (bus_rounding)
    uint64_t rounding_cycles() const { return rounding_cycles_; }
    void enable_io_summary() { io_summary_on_ = true; }
    const std::map<uint32_t, IoStat>& io_summary() const { return io_; }
    // Optional probes. on_frame_end runs after each frame is rendered; on_line_end after each line.
    std::function<void(StMachine&)> on_frame_end;
    std::function<void(StMachine&, int line)> on_line_end;
    void set_instruction_probe(std::function<void(StMachine&, uint32_t pc)> fn);

    // Bus (the Musashi callbacks land here).
    uint32_t read8(uint32_t a);
    uint32_t read16(uint32_t a);
    uint32_t read32(uint32_t a);
    void write8(uint32_t a, uint32_t v);
    void write16(uint32_t a, uint32_t v);
    void write32(uint32_t a, uint32_t v);
    uint32_t int_ack(int level);
    uint64_t io_wait_cycles() const { return wait_cycles_; }   // cycles added by device wait states (YM2149, MFP, ACIA)
    void probe(uint32_t pc) {
        if (bus_rounding_) round_previous_instruction();
        ++insn_serial_;
        insn_movem_ = pc + 1 < ram_.size() && (((uint32_t(ram_[pc]) << 8 | ram_[pc + 1]) & 0xFB80u) == 0x4880u);
        if (insn_probe_) insn_probe_(*this, pc);
    }

private:
    uint32_t io_read8(uint32_t a);
    void io_write8(uint32_t a, uint32_t v);
    uint32_t io_read16(uint32_t a);
    void io_write16(uint32_t a, uint32_t v);
    void unmapped(uint32_t a);
    void note_io(uint32_t a, bool write, uint32_t value);
    void update_irq();
    void process_events();
    uint64_t next_event_time() const;
    void line_begin(int line);
    void run_cpu_until(uint64_t target);
    void install_vectors();
    void on_fdc_irq(bool asserted, uint64_t at);
    void round_previous_instruction();
    void io_wait(int cycles);
    void ym_wait();
    void acia_wait(uint64_t t);
    uint32_t frame_cycle(uint64_t t) const;

    std::vector<uint8_t> ram_;
    std::vector<uint8_t> rom_;
    StDisk disk_;
    Shifter shifter_;
    Mfp mfp_;
    FdcDma fdc_;
    Ikbd ikbd_;
    Ym2149 ym_;
    BusHooks hooks_;
    std::function<void(StMachine&, uint32_t)> insn_probe_;

    bool bus_rounding_ = true;
    int slice_pad_ = 0;         // cycles added by rounding in the running slice (the core does not count them)
    int last_run_ = 0;          // the core's cycle count when the previous instruction hook ran
    uint64_t rounded_ = 0, rounding_cycles_ = 0;
    bool io_waits_ = true;
    uint64_t wait_cycles_ = 0;
    uint64_t insn_serial_ = 1, ym_serial_ = 0, acia_serial_ = 0;
    int ym_accesses_ = 0;
    bool insn_movem_ = false;
    uint64_t cycles_ = 0;       // absolute CPU cycles: the start of the slice running (or run)
    uint64_t frame_start_ = 0;
    bool in_execute_ = false;
    uint32_t frame_ = 0;
    uint8_t mem_config_ = 0x0A;
    bool vbl_pending_ = false;
    int irq_level_ = 0;
    uint64_t vbl_taken_ = 0, mfp_taken_ = 0, unmapped_ = 0, fatal_hits_ = 0, hbl_unmasked_ = 0, res_changes_ = 0;
    uint32_t last_unmapped_ = 0;
    uint8_t last_res_ = 0;
    uint8_t latch_res_ = 0; // the resolution at the first display line (counted for the unsupported-frame statistics)
    std::vector<uint8_t> picture_;
    bool io_summary_on_ = false;
    std::map<uint32_t, IoStat> io_;
};

} // namespace x2::st
