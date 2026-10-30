// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Amiga 500 (OCS, PAL) machine model driving the Musashi 68000 core.
//
// Scope: 24-bit bus with 512 KB chip RAM, custom chip register file with the read-side behaviours the game relies
// on, Paula interrupt controller, two CIAs (timers, TOD, keyboard serial), a line-granular copper, the blitter
// (its memory operation in one step; its duration and the CPU's chip-bus wait states from the Accurate timing
// model, machine_timing.cpp), PC traps for HLE shims, and per-frame logging. See docs/BUILDING.md for the cycle
// budget and docs/re-notes/timing.md §8 for the timing model.
#pragma once
#include "audio/paula.h"
#include "cia.h"
#include "custom_regs.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace x2 {

class Tracer;

// Timing: 227 colour clocks per PAL line. The CPU runs at twice the colour clock (7.09379 MHz), so a line is
// 454 CPU cycles. A frame has lines_per_frame() lines: 313 in Accurate timing (a non-interlaced PAL Agnus
// shows long fields only, LOF=1, as the reference emulator does: 142,102 cycles = 49.92 Hz), 312 in Fast timing
// (the Phase 3 model: 141,648 cycles = 50.08 Hz). CIAs are clocked at E = CPU/10, i.e. 45.4 ticks per line, accumulated with the remainder carried.
// Chip-bus contention and blitter duration depend on the timing model (Timing below, docs/re-notes/timing.md §8).
constexpr int kLinesPerFrame = 312;     // Fast timing; Machine::lines_per_frame() is the current count
constexpr int kColorClocksPerLine = 227;
constexpr int kCpuCyclesPerLine = kColorClocksPerLine * 2;
constexpr int kCopperSlotCck = 16; // copper granularity within a line, in colour clocks
constexpr uint32_t kChipRamSize = 0x80000;
constexpr uint32_t kChipMask = kChipRamSize - 1;

// One CPU bus request as seen by the m68k_read/write_memory_* callbacks (difftest instrumentation, see bus_log).
struct BusAccess {
    uint32_t addr;
    uint32_t value;
    int32_t cycles; // m68k_cycles_run() at the time of the access
    uint32_t pc;    // REG_PC at the time of the access (an instruction-stream read is at pc - size)
    uint8_t size;   // 1, 2 or 4
    char kind;      // 'R' read, 'W' write
};

struct RegWrite {
    uint32_t frame;
    uint16_t line;
    uint16_t hpos;
    uint32_t pc;     // CPU PC of the writing instruction, or copper list address
    uint16_t reg;    // custom offset ($000-$1FE), or $1000|reg for CIA-A, $2000|reg for CIA-B
    uint16_t value;
    char source;     // 'C' = CPU, 'P' = copper
};

// Display parameters latched at the first line of the display window, for debug framebuffer dumps.
struct DisplaySnapshot {
    bool valid = false;
    uint32_t bplpt[6] = {};
    uint16_t bplcon0 = 0, bplcon1 = 0, bpl1mod = 0, bpl2mod = 0;
    uint16_t diwstrt = 0, diwstop = 0, ddfstrt = 0, ddfstop = 0;
    uint16_t dmacon = 0;
    uint16_t color[32] = {};
};

// Timing model (docs/re-notes/timing.md §8).
//   Fast:     the Phase 3 model. The blitter finishes in zero time when BLTSIZE is written (BBUSY never reads
//             1) and the CPU never waits for the chip bus.
//   Accurate: OCS A500 bus timing per colour clock. Refresh, audio and bitplane DMA own fixed slots; the blitter
//             runs its channel cycle sequence in the free slots (BBUSY and the blitter interrupt come when its
//             last cycle has run); every CPU bus cycle (instruction fetches and data, all in chip RAM or the
//             custom chips) needs a free slot and waits for one, with the blitter giving up a slot after the CPU
//             has waited 3 unless BLTPRI is set; CIA accesses synchronise to the E clock. Waits are charged to
//             Musashi's cycle counter from the instruction hook and the bus callbacks, which the interpreter and
//             the recompiled executor share, so both stay bit-identical.
enum class Timing { Fast, Accurate };

class Machine {
public:
    Machine();
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Copy bytes into chip RAM (addresses are masked to chip RAM).
    void load(uint32_t addr, const uint8_t* data, size_t size);
    // Reset the CPU into supervisor mode at `pc` with the given stack and SR.
    void start_cpu(uint32_t pc, uint32_t ssp, uint16_t sr = 0x2700);

    // Run exactly one frame (lines_per_frame() lines). VBL (INTREQ VERTB) is raised at the start of line 0.
    void run_frame();
    // Run the CPU for `cycles` without advancing the beam (used by tests).
    int run_cycles(int cycles);

    // Select the timing model. Call before running (a pending blit is finished first).
    void set_timing(Timing t);
    Timing timing() const { return timing_; }
    bool accurate() const { return timing_ == Timing::Accurate; }
    int lines_per_frame() const { return lines_; }

    // Accurate timing hooks for the Musashi callbacks: charge the wait states of one CPU data access (size 1, 2
    // or 4 bytes; a long is two bus cycles) before it happens.
    void cpu_bus_access(uint32_t addr, int size);
    void cpu_bus_done() { access_cck_ = -1; }

    // --- bus (Musashi callbacks land here) ---
    uint8_t read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);
    void write8(uint32_t addr, uint8_t v);
    void write16(uint32_t addr, uint16_t v);
    void write32(uint32_t addr, uint32_t v);

    uint16_t custom_read(uint16_t off);
    void custom_write(uint16_t off, uint16_t v, char source = 'C');

    // --- CPU register access helpers ---
    uint32_t pc() const;
    uint32_t dreg(int n) const;
    uint32_t areg(int n) const;
    void set_dreg(int n, uint32_t v);
    void set_areg(int n, uint32_t v);
    void set_pc(uint32_t pc);
    uint16_t sr() const;
    void set_sr(uint16_t sr);
    // Emulate RTS: pop the return address from A7 into PC.
    void do_rts();

    // --- PC traps (HLE shims). The handler runs before the instruction at `addr` executes; it must leave the
    // CPU at a valid PC (usually via do_rts()). A second trap at the same address runs after the first, unless
    // the first moved the CPU. ---
    void add_trap(uint32_t addr, std::function<void(Machine&)> fn);

    // --- input ---
    // Host keyboard event, Amiga raw keycode (0-$67). Delivered through CIA-A SDR with handshake.
    void key_event(uint8_t code, bool released);
    // Joystick in port 2 (JOY1DAT + CIA-A PRA bit 7).
    void set_joystick(bool up, bool down, bool left, bool right, bool fire);
    // Joystick in port 1, the mouse port (JOY0DAT + CIA-A PRA bit 6). The game only reads port 2.
    void set_joystick_port1(bool up, bool down, bool left, bool right, bool fire);

    // --- state (public for the driver, tests and the future renderer) ---
    std::vector<uint8_t> chip;
    std::array<uint16_t, 0x100> regs{}; // last value written to each custom register
    uint16_t dmacon = 0, intena = 0, intreq = 0, adkcon = 0;
    Cia cia_a, cia_b;
    uint32_t frame = 0;   // frames completed
    int line = 0;         // current beam line
    uint16_t hpos() const; // current beam colour clock (derived from CPU cycles into the line)
    // Absolute emulated time in colour clocks (frame, line, hpos); Paula's time base.
    uint64_t cck_now() const;
    Paula paula;

    // --- instrumentation ---
    bool log_writes = false;
    std::vector<RegWrite> frame_writes;          // cleared by the driver
    std::array<uint32_t, 0x100> custom_reads{};  // per word register
    std::array<uint32_t, 32> cia_reads{};        // [0..15] CIA-A, [16..31] CIA-B
    std::unordered_map<uint32_t, uint32_t> pc_samples; // PC sampled once per line
    uint32_t unmapped_accesses = 0;
    uint32_t last_unmapped_addr = 0;
    uint32_t restarts = 0;            // entries to the $7032 restart vector target (exceptions)
    uint32_t last_restart_from = 0;
    uint32_t blits = 0;
    uint32_t line_blits = 0;          // line-mode blits (subset of blits)
    uint32_t copper_moves = 0;
    // Accurate timing statistics (cumulative; zero in Fast mode).
    uint64_t cpu_wait_cycles = 0;     // CPU cycles spent waiting for the chip bus or the E clock
    uint64_t blit_cck = 0;            // colour clocks from BLTSIZE to BBUSY clear, summed over blits
    uint32_t blit_busy_lines = 0;     // lines whose end found the blitter busy (the driver resets it per frame)
    uint32_t blit_restarts = 0;       // BLTSIZE written while a blit was still running
    uint32_t blit_reg_writes_busy = 0; // other blitter register writes while a blit was running
    // Optional blit log (accurate timing): "frame line hpos S w h bltcon0 bltcon1" at each blit start and
    // "frame line hpos E" when BBUSY clears, for frames blit_log_from..blit_log_to (as the reference's blits.log).
    FILE* blit_log = nullptr;
    uint32_t blit_log_from = 0, blit_log_to = 0;
    DisplaySnapshot display;          // latched at DIW start each frame
    // Called once per frame after line 311 (e.g. driver hooks).
    std::function<void(Machine&)> on_frame_end;
    // Optional per-instruction trace sink (disassembly), enabled by the driver for selected frames.
    std::function<void(Machine&, uint32_t pc)> trace;
    // Optional coverage tracer (runtime/trace), set by the driver with --trace-out; null = no tracing cost.
    Tracer* tracer = nullptr;
    // Video observer (runtime/gfx): sees every custom register write (CPU or copper) with the beam position
    // it happened at, and the end of every line (after the end-of-line copper slot).
    std::function<void(uint16_t off, uint16_t value, int line, uint16_t hpos)> on_custom_write;
    std::function<void(Machine&, int line)> on_line_end;
    // Draw-command capture (runtime/modern, off by default): on_blit runs at the start of every blit, before its
    // memory operation, with the blitter registers as the CPU set them; on_cpu_write sees every CPU write to chip
    // RAM (a long write arrives as two words). Pure observers: they must not change machine state.
    std::function<void(Machine&)> on_blit;
    std::function<void(uint32_t addr, uint32_t value, int size)> on_cpu_write;

    // CPU slice runner used by run_frame()/run_cycles(): m68k_execute, or the hybrid recompiled executor
    // (runtime/recomp) which has the same contract.
    int (*cpu_execute)(int cycles);
    // Optional log of every CPU bus request (null = off). Used by the recompiler difftest.
    std::vector<BusAccess>* bus_log = nullptr;

    // Called from the Musashi instruction hook.
    void instruction_hook(uint32_t pc);
    // Optional probe at every instruction start, before the instruction's own bus cycles (diagnostics).
    std::function<void(Machine&, uint32_t pc)> on_insn_start;
    // Absolute CPU cycle count (Accurate timing; the time base of the bus model).
    int64_t cpu_now() const;

private:
    void update_irq();
    void sync_cia_irq();
    void copper_restart(uint32_t addr);
    void copper_run(int beam_line, int beam_h);
    void copper_dma(int beam_h, int cycles);
    void start_blit();
    void do_blit();
    void do_line_blit();
    uint16_t write_hpos(char source) const;
    void line_start();
    void line_end();
    void keyboard_step();
    void latch_display();
    void advance_bitplane_pointers();
    uint32_t reg_ptr(uint16_t off) const { return ((uint32_t(regs[off >> 1]) << 16) | regs[(off >> 1) + 1]) & kChipMask & ~1u; }
    void set_reg_ptr(uint16_t off, uint32_t v) { regs[off >> 1] = uint16_t((v >> 16) & 0x7); regs[(off >> 1) + 1] = uint16_t(v & 0xFFFE); }
    uint8_t cia_read(uint32_t addr);
    void cia_write(uint32_t addr, uint8_t v);
    void log_write(uint16_t reg, uint16_t value, char source, uint32_t pc);

    // beam / cpu timing
    int line_cycles_ = 0;     // CPU cycles already executed in this line (outside the current execute call)
    bool in_execute_ = false;
    int cpu_debt_ = 0;        // overshoot carried into the next line
    uint32_t cia_eclock_acc_ = 0;
    int irq_level_ = 0;

    // copper
    uint32_t cop_pc_ = 0;
    bool cop_waiting_ = false, cop_stopped_ = true, cop_skip_ = false;
    uint16_t cop_wait_ir1_ = 0, cop_wait_ir2_ = 0;
    uint32_t cop_cur_insn_ = 0;

    // blitter
    bool blit_pending_ = false;
    bool blit_zero_ = true;

    // --- accurate timing (machine_timing.cpp) ---
    Timing timing_ = Timing::Accurate;
    int lines_ = 313;
    int64_t line_start_abs_ = 0;  // absolute CPU cycle of the start of the current beam line
    int64_t line_base_ = 0;       // absolute CPU cycle at which line_cycles_ was 0 (line start + carried debt)
    int64_t bus_cursor_ = 0;      // absolute CPU cycle at which the CPU's previous bus cycle ended
    int64_t access_cck_ = -1;     // colour clock of the CPU bus cycle in progress (-1 outside one)
    uint32_t prev_pc_ = 0;
    int prev_len_ = 0;
    std::vector<uint8_t> insn_len_; // instruction length in words per opcode word (0 = not yet known)
    // Fixed DMA slots per colour clock of a line (bit set = used by refresh, audio or bitplane DMA).
    std::array<uint64_t, 4> dma_blank_{}, dma_display_{};
    bool dma_dirty_ = true;
    int bpl_vstart_ = 0, bpl_vstop_ = 0;
    uint8_t audio_dma_ = 0;       // audio channels that fetched during the previous line
    std::array<uint32_t, 4> audio_fetches_{};
    // Copper DMA slots of one beam line. The copper takes even colour clocks, which are exactly the ones the
    // CPU would otherwise have (refresh, audio and the game's lores bitplane fetches are all odd).
    std::array<uint64_t, 4> dma_cop_{};
    int64_t cop_dma_line_ = -1;   // absolute beam line dma_cop_ belongs to
    int cop_dma_next_ = 0;        // first colour clock of that line the copper has not taken yet
    int64_t cop_write_cck_ = -1;  // colour clock of the copper's latest fetch (where a MOVE takes effect)
    // Blitter cycle sequencer (a port of the shifter logic of WinUAE/Amiberry blitter.cpp, which the reference
    // emulator runs cycle-exact).
    struct BlitSeq {
        bool main = false;        // channel cycles still to run (BBUSY)
        int finald = 0;           // final D write countdown (3 -> 1, written at 1)
        bool wait = false;        // the BLTSIZE write cycle itself
        int counter = 0;          // -3..-1 start-up idle cycles, then words
        bool shifter[4] = {}, out = false;
        int first = 0;
        bool skip_b = false, skip_y = false, fill_idle = false, line = false, ddatuse = false;
        uint16_t con0 = 0;
        int hcount = 0, vcount = 0, hsize = 0, vsize = 0;
    } bs_;
    uint64_t blt_cck_ = 0;        // next colour clock the blitter has not processed yet
    uint64_t blt_start_cck_ = 0;
    uint64_t blt_done_cck_ = 0;   // colour clock of the last main cycle of the latest finished blit (0 = none)

    void timing_line_start();
    void timing_insn_end();
    void timing_insn(uint32_t pc);
    void timing_interrupt();
    int insn_words(uint16_t op);
    int div_delta(uint32_t pc, uint16_t op) const;
    int insn_delta(uint32_t pc, uint16_t op) const;
    int div_pending_ = 0;         // time correction (DIVU/DIVS, table fixes) of the instruction now running
    int movem_extra_ = 0;         // MOVEM to registers: its extra (discarded) read, charged at the next boundary
    int exc_pending_ = 0;         // interrupt exception waits, charged at the handler's first instruction
    int exc_prefetch_ = 0;        // instruction words the exception sequence has already prefetched
    bool in_exception_ = false;   // inside an interrupt exception: its bus cycles are already placed
    int64_t chip_cycle(int64_t t);
    int64_t cia_cycle(int64_t t) const;
    void rebuild_dma_maps();
    bool fixed_dma(uint64_t cck);
    bool blitter_active() const { return bs_.main || bs_.finald; }
    void blitter_begin(uint64_t cck);
    void blitter_run_until(uint64_t cck);
    bool blitter_cycle(uint64_t cck, bool cpu_steal);
    bool blitter_shift();
    int blitter_channel() const;
    void blitter_done(uint64_t cck);
    void beam_at(int64_t cck, int& line_out, int& h_out) const;

    // keyboard
    std::deque<uint8_t> key_queue_;
    bool kb_wait_handshake_ = false;
    int kb_wait_lines_ = 0;
    int kb_cooldown_ = 0;
    bool kb_hs_seen_ = false;

    // joystick
    uint16_t joy0dat_ = 0;
    uint16_t joy1dat_ = 0;
    bool fire1_ = false;

    std::vector<std::function<void(Machine&)>> trap_fns_;
    std::vector<uint16_t> trap_index_; // per word of chip RAM: 0 = none, else index+1 into trap_fns_
};

// The single machine instance Musashi's C callbacks talk to.
Machine* current_machine();

} // namespace x2
