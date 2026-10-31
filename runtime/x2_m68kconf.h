// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Musashi build configuration for the Xenon 2 runtime (selected with -DMUSASHI_CNF="x2_m68kconf.h").
// Options set here take precedence over the defaults in vendor/musashi/m68kconf.h, which uses #ifndef guards.
#ifndef X2_M68KCONF_H
#define X2_M68KCONF_H

#define M68K_OPT_OFF             0
#define M68K_OPT_ON              1
#define M68K_OPT_SPECIFY_HANDLER 2

// Only a plain 68000 is needed (A500).
#define M68K_EMULATE_010    M68K_OPT_OFF
#define M68K_EMULATE_EC020  M68K_OPT_OFF
#define M68K_EMULATE_020    M68K_OPT_OFF
#define M68K_EMULATE_030    M68K_OPT_OFF
#define M68K_EMULATE_040    M68K_OPT_OFF

// Interrupt acknowledge: Paula drives IPL0-2 and the CPU takes the autovector ($60 + 4*level). x2_int_ack
// (machine.cpp) returns the autovector unless another machine installed a BusHooks table (runtime/bus_hooks.h): the
// Atari ST's MFP answers its own acknowledge with a vector number.
#define M68K_EMULATE_INT_ACK        M68K_OPT_SPECIFY_HANDLER
#define M68K_INT_ACK_CALLBACK(A)    x2_int_ack(A)

// Per-instruction hook: used for PC traps (disk shim at $921A) and PC sampling.
#define M68K_INSTRUCTION_HOOK         M68K_OPT_SPECIFY_HANDLER
#define M68K_INSTRUCTION_CALLBACK(pc) x2_instruction_hook(pc)

#ifdef __cplusplus
extern "C" {
#endif
void x2_instruction_hook(unsigned int pc);
unsigned int x2_int_ack(int level);
#ifdef __cplusplus
}
#endif

#include "m68kconf.h"

#endif
