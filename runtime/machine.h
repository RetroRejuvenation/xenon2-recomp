// Amiga 500 (OCS, PAL) machine model driving the Musashi 68000 core.
//
// Scope (Phase 3a): 24-bit bus with 512 KB chip RAM, custom chip register file with the read-side behaviours
// the game relies on, Paula interrupt controller, two CIAs (timers, TOD, keyboard serial), a line-granular
// copper, an immediate (zero-time) blitter, PC traps for HLE shims, and per-frame logging. No video or audio
// output yet; see docs/BUILDING.md for the cycle budget and known gaps.
#pragma once
#include "audio/paula.h"
#include "cia.h"
#include "custom_regs.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace x2 {

// Timing: PAL frame = 312 lines (short field, no interlace), 227 colour clocks per line (the real line is
// 227.5). The CPU runs at twice the colour clock (7.09379 MHz), so a line is 454 CPU cycles and a frame
// 141,648 cycles (= 49.9 Hz at 7.09379 MHz). DMA cycle stealing is not modelled. CIAs are clocked at E =
// CPU/10, i.e. 45.4 ticks per line, accumulated with the remainder carried.
constexpr int kLinesPerFrame = 312;
constexpr int kColorClocksPerLine = 227;
constexpr int kCpuCyclesPerLine = kColorClocksPerLine * 2;
constexpr int kCopperSlotCck = 16; // copper granularity within a line, in colour clocks
constexpr uint32_t kChipRamSize = 0x80000;
constexpr uint32_t kChipMask = kChipRamSize - 1;

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

    // Run exactly one 312-line frame. VBL (INTREQ VERTB) is raised at the start of line 0.
    void run_frame();
    // Run the CPU for `cycles` without advancing the beam (used by tests).
    int run_cycles(int cycles);

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
    // CPU at a valid PC (usually via do_rts()). ---
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
    DisplaySnapshot display;          // latched at DIW start each frame
    // Called once per frame after line 311 (e.g. driver hooks).
    std::function<void(Machine&)> on_frame_end;
    // Optional per-instruction trace sink (disassembly), enabled by the driver for selected frames.
    std::function<void(Machine&, uint32_t pc)> trace;
    // Video observer (runtime/gfx): sees every custom register write (CPU or copper) with the beam position
    // it happened at, and the end of every line (after the end-of-line copper slot).
    std::function<void(uint16_t off, uint16_t value, int line, uint16_t hpos)> on_custom_write;
    std::function<void(Machine&, int line)> on_line_end;

    // Called from the Musashi instruction hook.
    void instruction_hook(uint32_t pc);

private:
    void update_irq();
    void sync_cia_irq();
    void copper_restart(uint32_t addr);
    void copper_run(int beam_line, int beam_h);
    void do_blit();
    void do_line_blit();
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
