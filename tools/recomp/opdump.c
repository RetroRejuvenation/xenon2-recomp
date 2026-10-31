// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
/* Dumps Musashi's 68000 opcode dispatch as built by m68ki_build_opcode_table(): for each opcode word, the index
 * of its handler in m68k_opcode_handler_table (-1 = m68k_op_illegal) and its 68000 base cycle count.
 * Compiled by tools/recomp/recomp.py together with the generated m68kops.c (included below so the static table
 * is visible) and m68kcpu.c, with the runtime's Musashi configuration. Output: "op index cycles" per line. */
#include "m68kops.c"

unsigned int m68k_read_memory_8(unsigned int a) { (void)a; return 0; }
unsigned int m68k_read_memory_16(unsigned int a) { (void)a; return 0; }
unsigned int m68k_read_memory_32(unsigned int a) { (void)a; return 0; }
void m68k_write_memory_8(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_32(unsigned int a, unsigned int v) { (void)a; (void)v; }
unsigned int m68k_read_disassembler_8(unsigned int a) { (void)a; return 0; }
unsigned int m68k_read_disassembler_16(unsigned int a) { (void)a; return 0; }
unsigned int m68k_read_disassembler_32(unsigned int a) { (void)a; return 0; }
void x2_instruction_hook(unsigned int pc) { (void)pc; }
unsigned int x2_int_ack(int level) { (void)level; return 0xFFFFFFFFu; }

int main(void) {
    int op, n = 0;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    while (m68k_opcode_handler_table[n].opcode_handler) ++n;
    for (op = 0; op < 0x10000; ++op) {
        int idx = -1, i;
        for (i = 0; i < n; ++i)
            if (m68k_opcode_handler_table[i].opcode_handler == m68ki_instruction_jump_table[op]) { idx = i; break; }
        printf("%d %d %d\n", op, idx, (int)m68ki_cycles[0][op]);
    }
    return 0;
}
