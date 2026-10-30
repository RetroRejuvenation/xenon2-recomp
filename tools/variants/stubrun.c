// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// stubrun: run 68000 code in a flat 16 MB memory on the project's Musashi core until a stop address, then dump a memory
// range. Used to check the Python decrunchers in this folder against the decruncher stubs inside the crunched
// executables (docs/re-notes/cracks.md); it never touches the runtime and is not part of the build.
//
// build (after `cmake --build build` has produced build/libmusashi.a):
//   cc -O1 -w -DMUSASHI_CNF='"x2_m68kconf.h"' -Ivendor/musashi -Ibuild/musashi_gen -Iruntime \
//      tools/variants/stubrun.c build/libmusashi.a -o stubrun
// usage: stubrun IMAGE LOADADDR PC STOPPC A0 OUTFILE OUTADDR OUTLEN [MAXINSN]
//   loads IMAGE at LOADADDR, starts at PC with A0 set, stops when PC == STOPPC, writes OUTLEN bytes from OUTADDR.
//   Writes to $DFF000-$DFFFFF are ignored (the stubs poke the colour registers). Exit 0 stopped, 3 instruction cap.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"

static uint8_t mem[1 << 24];
static unsigned stop_pc;
static unsigned long insns;
static int stopped;

void x2_instruction_hook(unsigned pc) {
    insns++;
    if (pc == stop_pc) {
        stopped = 1;
        m68k_end_timeslice();
    }
}
unsigned int m68k_read_memory_8(unsigned a) { return mem[a & 0xFFFFFF]; }
unsigned int m68k_read_memory_16(unsigned a) { a &= 0xFFFFFF; return (mem[a] << 8) | mem[a + 1]; }
unsigned int m68k_read_memory_32(unsigned a) {
    a &= 0xFFFFFF;
    return ((unsigned)mem[a] << 24) | (mem[a + 1] << 16) | (mem[a + 2] << 8) | mem[a + 3];
}
unsigned int m68k_read_disassembler_16(unsigned a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned a) { return m68k_read_memory_32(a); }
static int custom(unsigned a) { return a >= 0xDFF000 && a <= 0xDFFFFF; }
void m68k_write_memory_8(unsigned a, unsigned v) { a &= 0xFFFFFF; if (!custom(a)) mem[a] = (uint8_t)v; }
void m68k_write_memory_16(unsigned a, unsigned v) {
    a &= 0xFFFFFF;
    if (!custom(a)) { mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }
}
void m68k_write_memory_32(unsigned a, unsigned v) {
    a &= 0xFFFFFF;
    if (!custom(a)) { mem[a] = (uint8_t)(v >> 24); mem[a + 1] = (uint8_t)(v >> 16); mem[a + 2] = (uint8_t)(v >> 8); mem[a + 3] = (uint8_t)v; }
}

int main(int argc, char** argv) {
    if (argc < 9) {
        fprintf(stderr, "usage: stubrun IMAGE LOADADDR PC STOPPC A0 OUTFILE OUTADDR OUTLEN [MAXINSN]\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    unsigned load = (unsigned)strtoul(argv[2], 0, 0);
    if (load >= sizeof(mem)) { fprintf(stderr, "load address out of range\n"); return 2; }
    fread(mem + load, 1, sizeof(mem) - load, f);
    fclose(f);
    unsigned pc = (unsigned)strtoul(argv[3], 0, 0);
    stop_pc = (unsigned)strtoul(argv[4], 0, 0);
    unsigned a0 = (unsigned)strtoul(argv[5], 0, 0);
    unsigned oa = (unsigned)strtoul(argv[7], 0, 0), ol = (unsigned)strtoul(argv[8], 0, 0);
    if ((size_t)oa + ol > sizeof(mem)) { fprintf(stderr, "output range out of range\n"); return 2; }
    unsigned long maxi = argc > 9 ? strtoul(argv[9], 0, 0) : 400000000UL;
    mem[4] = (uint8_t)(pc >> 24); mem[5] = (uint8_t)(pc >> 16); mem[6] = (uint8_t)(pc >> 8); mem[7] = (uint8_t)pc;
    mem[1] = 0x08;                                   // initial stack at $80000
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_PC, pc);
    m68k_set_reg(M68K_REG_A0, a0);
    m68k_set_reg(M68K_REG_SR, 0x2700);
    while (!stopped && insns < maxi) m68k_execute(100000);
    fprintf(stderr, "stopped=%d insns=%lu pc=%06x\n", stopped, insns, m68k_get_reg(NULL, M68K_REG_PC));
    f = fopen(argv[6], "wb");
    if (!f) { perror(argv[6]); return 2; }
    fwrite(mem + oa, 1, ol, f);
    fclose(f);
    return stopped ? 0 : 3;
}
