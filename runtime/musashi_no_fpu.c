// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Replaces vendor/musashi/softfloat for this build, so no SoftFloat code is compiled, linked or shipped.
//
// Why this is sound: Musashi reaches its FPU only from the 040fpu0/040fpu1 opcode handlers, and both are
// guarded by CPU_TYPE_IS_030_PLUS(CPU_TYPE) (vendor/musashi/m68k_in.c). This runtime is an Amiga 500, so it
// calls m68k_set_cpu_type(M68K_CPU_TYPE_68000) (runtime/machine.cpp) and M68K_EMULATE_010/EC020/020/030/040
// are all OPT_OFF (runtime/x2_m68kconf.h). The guard is therefore always false and an F-line opcode takes
// m68ki_exception_1111() instead, which is what a real 68000 does. m68kfpu.c is still *compiled* (m68kcpu.c
// includes it unconditionally), so the linker still wants the 15 SoftFloat symbols it names; these
// definitions satisfy that without bringing in SoftFloat.
//
// Each body is unreachable. They abort rather than return a plausible value: if a future change ever enables
// a 68030+ CPU type, that must fail loudly instead of silently computing wrong floating-point results.
//
// To restore real FPU support: drop this file from the `musashi` target in CMakeLists.txt, put
// ${MUSASHI_DIR}/softfloat/softfloat.c back, and restore the SoftFloat notice to packaging/licenses
// (docs/PROVENANCE.md §7.3).
#include <stdio.h>
#include <stdlib.h>

// As vendor/musashi/softfloat/softfloat.c does: m68kcpu.h defines SoftFloat's base integer types and only
// then includes softfloat.h. Including softfloat.h directly does not compile.
#include "m68kcpu.h"

int8 float_rounding_mode = 0; /* float_round_nearest_even */

static void x2_no_fpu(const char *fn)
{
    fprintf(stderr, "x2recomp: %s called, but this build has no FPU support (68000 only)\n", fn);
    abort();
}

#define X2_NO_FPU(ret, name, params)                                                                           \
    ret name params                                                                                            \
    {                                                                                                          \
        x2_no_fpu(#name);                                                                                       \
        ret zero = {0};                                                                                        \
        return zero;                                                                                           \
    }

X2_NO_FPU(floatx80, float32_to_floatx80, (float32 a))
X2_NO_FPU(floatx80, float64_to_floatx80, (float64 a))
X2_NO_FPU(floatx80, floatx80_add, (floatx80 a, floatx80 b))
X2_NO_FPU(floatx80, floatx80_div, (floatx80 a, floatx80 b))
X2_NO_FPU(floatx80, floatx80_mul, (floatx80 a, floatx80 b))
X2_NO_FPU(floatx80, floatx80_rem, (floatx80 a, floatx80 b))
X2_NO_FPU(floatx80, floatx80_sqrt, (floatx80 a))
X2_NO_FPU(floatx80, floatx80_sub, (floatx80 a, floatx80 b))
X2_NO_FPU(floatx80, int32_to_floatx80, (int32 a))
X2_NO_FPU(float32, floatx80_to_float32, (floatx80 a))
X2_NO_FPU(float64, floatx80_to_float64, (floatx80 a))
X2_NO_FPU(int32, floatx80_to_int32, (floatx80 a))
X2_NO_FPU(int32, floatx80_to_int32_round_to_zero, (floatx80 a))
X2_NO_FPU(flag, floatx80_is_nan, (floatx80 a))
