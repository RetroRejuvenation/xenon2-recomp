/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the xenon2-recomp authors
 *
 * tools/acq/stemu.c: a word access at 0xFFFFFF (the I/O range) wraps its second byte to the bottom of mem[] instead of
 * indexing mem[0x1000000]. The real source is included with main renamed; the Musashi entry points are stubs. No game data.
 * Run under AddressSanitizer/UBSan too. */
#define main stemu_main
#include "tools/acq/stemu.c"
#undef main

int m68k_execute(int n) { (void)n; return 0; }
int m68k_cycles_run(void) { return 0; }
void m68k_init(void) {}
void m68k_set_cpu_type(unsigned int t) { (void)t; }
void m68k_pulse_reset(void) {}
void m68k_set_irq(unsigned int l) { (void)l; }
void m68k_end_timeslice(void) {}
unsigned int m68k_get_reg(void* c, m68k_register_t r) { (void)c; (void)r; return 0; }
void m68k_set_reg(m68k_register_t r, unsigned int v) { (void)r; (void)v; }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(void)
{
    m68k_write_memory_16(0xFFFFFF, 0x1234);
    CHECK(mem[0xFFFFFF] == 0x12 && mem[0] == 0x34);
    CHECK(m68k_read_memory_16(0xFFFFFF) == 0x1234);
    /* an ordinary word is unchanged */
    m68k_write_memory_16(0x2000, 0xBEEF);
    CHECK(m68k_read_memory_16(0x2000) == 0xBEEF);
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("ok\n");
    return 0;
}
