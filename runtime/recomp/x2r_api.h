// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Types shared by the generated dispatch tables (generated/recomp/x2r_tables.cpp) and the hybrid executor.
#pragma once
#include <cstdint>

typedef int (*x2r_fn)(uint32_t pc24);
struct x2r_entry {
    uint32_t pc;    // 24-bit address of the instruction
    uint16_t op;    // opcode word (also checked by the generated code)
    uint8_t cycles; // CYC_INSTRUCTION[op] baked into the code; verified against Musashi at start-up
    uint8_t len;    // instruction length in bytes as decoded by the generator
    x2r_fn fn;      // translated function containing this instruction
};
struct x2r_func {
    uint32_t entry; // code-map (or call/profile) entry point
    x2r_fn fn;
};
struct x2r_module {
    const char* name;
    uint32_t base, end;             // load address range of the image the code was translated from
    uint32_t disc_sector, sectors;  // level modules: raw sectors on disc 2 (0, 0 for the resident game image)
    const x2r_entry* entries;
    int count;
    const x2r_func* funcs;
    int func_count;
};
extern const x2r_module x2r_modules[]; // generated/recomp/x2r_tables.cpp, terminated by name == nullptr

extern uint8_t* x2r_chip; // chip RAM (Machine::chip), for the code-identity checks
extern int x2r_hooked;    // set when a translated function returns after calling the hook for REG_PC
