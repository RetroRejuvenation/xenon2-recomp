// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The seam between the Musashi 68000 core's C callbacks and a machine model. The core calls global functions
// (m68k_read_memory_*, m68k_write_memory_*, the instruction hook, the interrupt acknowledge); they live in
// machine.cpp and talk to the Amiga Machine. A different machine (the Atari ST, runtime/st) installs a BusHooks table
// instead: while g_bus_hooks is non-null every callback goes through it and the Amiga Machine is not used at all.
// One machine kind per process, as with the core itself (Musashi keeps its CPU state in globals). With no table set
// the callbacks behave exactly as before (the extra cost is one predictable pointer test per access).
#pragma once
#include <cstdint>

namespace x2 {

struct BusHooks {
    uint32_t (*read8)(uint32_t addr) = nullptr;
    uint32_t (*read16)(uint32_t addr) = nullptr;
    uint32_t (*read32)(uint32_t addr) = nullptr;
    void (*write8)(uint32_t addr, uint32_t value) = nullptr;
    void (*write16)(uint32_t addr, uint32_t value) = nullptr;
    void (*write32)(uint32_t addr, uint32_t value) = nullptr;
    // Side-effect free reads for the disassembler.
    uint32_t (*dasm8)(uint32_t addr) = nullptr;
    uint32_t (*dasm16)(uint32_t addr) = nullptr;
    // Before each instruction (null = none).
    void (*insn)(uint32_t pc) = nullptr;
    // Interrupt acknowledge at `level` (1-7): the vector number to use, or 0xFFFFFFFF for the autovector.
    uint32_t (*int_ack)(int level) = nullptr;
};

// Null for the Amiga machine; the active machine's table otherwise.
extern const BusHooks* g_bus_hooks;

} // namespace x2
