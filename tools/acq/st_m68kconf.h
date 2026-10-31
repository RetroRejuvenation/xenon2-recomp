// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Musashi configuration for the Atari ST analysis harness (tools/acq/stemu.c): the project's own configuration
// (runtime/x2_m68kconf.h) with the trace exception ENABLED, which the ST loaders rely on. Never used by the runtime.
#ifndef ST_M68KCONF_H
#define ST_M68KCONF_H

#define M68K_OPT_OFF             0
#define M68K_OPT_ON              1
#define M68K_OPT_SPECIFY_HANDLER 2

#define M68K_EMULATE_010    M68K_OPT_OFF
#define M68K_EMULATE_EC020  M68K_OPT_OFF
#define M68K_EMULATE_020    M68K_OPT_OFF
#define M68K_EMULATE_030    M68K_OPT_OFF
#define M68K_EMULATE_040    M68K_OPT_OFF

#define M68K_EMULATE_INT_ACK        M68K_OPT_SPECIFY_HANDLER
#define M68K_INT_ACK_CALLBACK(A)    st_int_ack(A)

#define M68K_EMULATE_TRACE          M68K_OPT_ON

#define M68K_INSTRUCTION_HOOK         M68K_OPT_SPECIFY_HANDLER
#define M68K_INSTRUCTION_CALLBACK(pc) st_instruction_hook(pc)

#ifdef __cplusplus
extern "C" {
#endif
void st_instruction_hook(unsigned int pc);
int st_int_ack(int level);
#ifdef __cplusplus
}
#endif

#include "m68kconf.h"

#endif
