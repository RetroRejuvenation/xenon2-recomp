// Runtime support for generated recompiled code (generated/recomp/*.cpp). See docs/re-notes/recompiler-design.md.
//
// Translated code runs Musashi's own opcode handlers (copied into x2r_handlers.h by tools/recomp/recomp.py) on
// Musashi's own CPU state. This header includes Musashi's internals and then redirects only the instruction-stream
// reads: inside a handler, REG_IR is the constant opcode `ir_`, and extension words come from the constant array
// `e_[0..n_)` baked in at generation time (any read past it falls back to Musashi's memory read, so a short
// array can never produce a wrong value). Everything else - flags, cycles, exceptions, bus accesses - is
// Musashi's code unchanged.
//
// Only generated code includes this (it redefines REG_IR and the immediate reads for the whole translation unit).
#pragma once
#include <cstdint>

extern "C" {
#include "m68kcpu.h"
}

#include "recomp/x2r_api.h"

#if defined(_MSC_VER)
#define X2R_INLINE __forceinline
#else
#define X2R_INLINE inline __attribute__((always_inline))
#endif

// Code identity: the word at a (24-bit, below $80000) in chip RAM.
#define X2R_CODE16(a) (uint16_t((x2r_chip[(a)] << 8) | x2r_chip[(a) + 1]))

// Musashi's loop condition, then its per-instruction hook. A hook that moves the PC (an HLE trap emulating RTS)
// ends the translated run; the executor then continues at the new PC without calling the hook again, exactly
// like m68k_execute().
#define X2R_BOUNDARY(pc)                                                                                        \
    do {                                                                                                        \
        if (GET_CYCLES() <= 0) return k;                                                                        \
        m68ki_instr_hook(REG_PC);                                                                               \
        if (REG_PC != (ph | (pc))) { x2r_hooked = 1; return k; }                                                \
    } while (0)
// The bytes in memory are not the ones this code was translated from: let the executor find another
// translation (another level module) or interpret. The hook for this boundary has already run.
#define X2R_UNVERIFIED()                                                                                        \
    do { x2r_hooked = 1; return k; } while (0)
// What m68k_execute() does around the handler call (opcode fetch included).
#define X2R_BEGIN(pc, op)                                                                                       \
    REG_PPC = ph | (pc);                                                                                        \
    m68ki_cpu.ir = (op);                                                                                        \
    REG_PC = ph | ((pc) + 2)
#define X2R_END(cyc)                                                                                            \
    USE_CYCLES(cyc);                                                                                            \
    ++k

// ---- instruction-stream redirection, used only inside the h_* handler bodies ----
#undef REG_IR
#define REG_IR ir_

#define X2R_IMM16() (ei_ < n_ ? (REG_PC += 2, uint(e_[ei_++])) : (m68ki_read_imm_16)())
#define X2R_IMM32()                                                                                             \
    (ei_ + 1 < n_ ? (REG_PC += 4, ei_ += 2, (uint(e_[ei_ - 2]) << 16) | uint(e_[ei_ - 1]))                     \
                  : (ei_ = n_, (m68ki_read_imm_32)()))

// 68000 brief extension word (Musashi's m68ki_get_ea_ix, CPU_TYPE_IS_010_LESS path).
static X2R_INLINE uint x2r_ea_ix(uint An, uint extension) {
    uint Xn = REG_DA[extension >> 12];
    if (!BIT_B(extension)) Xn = MAKE_INT_16(Xn);
    return An + Xn + MAKE_INT_8(extension);
}

#define m68ki_read_imm_16() X2R_IMM16()
#define m68ki_read_imm_32() X2R_IMM32()
#define m68ki_read_imm_8() MASK_OUT_ABOVE_8(X2R_IMM16())
#define m68ki_get_ea_ix(An) x2r_ea_ix((An), X2R_IMM16())
// The PC is read before the extension word (which advances it). GNU statement expressions where available;
// MSVC (or -DX2R_NO_STMT_EXPR, to test this path elsewhere) gets the same sequence as immediately invoked lambdas.
#if defined(__GNUC__) && !defined(X2R_NO_STMT_EXPR)
#define m68ki_get_ea_pcdi() ({ uint o_ = REG_PC; o_ + MAKE_INT_16(X2R_IMM16()); })
#define m68ki_get_ea_pcix() ({ uint o_ = REG_PC; x2r_ea_ix(o_, X2R_IMM16()); })
#else
#define m68ki_get_ea_pcdi() ([&]() -> uint { uint o_ = REG_PC; return o_ + MAKE_INT_16(X2R_IMM16()); }())
#define m68ki_get_ea_pcix() ([&]() -> uint { uint o_ = REG_PC; return x2r_ea_ix(o_, X2R_IMM16()); }())
#endif

// m68kcpu.h defines these operand fetchers as functions, compiled against the original immediate reads.
#if defined(__GNUC__) && !defined(X2R_NO_STMT_EXPR)
#define X2R_OPER(ea, rd) ({ uint ea_ = (ea); rd(ea_); })
#else
#define X2R_OPER(ea, rd) ([&]() -> uint { uint ea_ = (ea); return rd(ea_); }())
#endif
#define OPER_AY_DI_8() X2R_OPER(EA_AY_DI_8(), m68ki_read_8)
#define OPER_AY_DI_16() X2R_OPER(EA_AY_DI_16(), m68ki_read_16)
#define OPER_AY_DI_32() X2R_OPER(EA_AY_DI_32(), m68ki_read_32)
#define OPER_AY_IX_8() X2R_OPER(EA_AY_IX_8(), m68ki_read_8)
#define OPER_AY_IX_16() X2R_OPER(EA_AY_IX_16(), m68ki_read_16)
#define OPER_AY_IX_32() X2R_OPER(EA_AY_IX_32(), m68ki_read_32)
#define OPER_AX_DI_8() X2R_OPER(EA_AX_DI_8(), m68ki_read_8)
#define OPER_AX_DI_16() X2R_OPER(EA_AX_DI_16(), m68ki_read_16)
#define OPER_AX_DI_32() X2R_OPER(EA_AX_DI_32(), m68ki_read_32)
#define OPER_AX_IX_8() X2R_OPER(EA_AX_IX_8(), m68ki_read_8)
#define OPER_AX_IX_16() X2R_OPER(EA_AX_IX_16(), m68ki_read_16)
#define OPER_AX_IX_32() X2R_OPER(EA_AX_IX_32(), m68ki_read_32)
#define OPER_AW_8() X2R_OPER(EA_AW_8(), m68ki_read_8)
#define OPER_AW_16() X2R_OPER(EA_AW_16(), m68ki_read_16)
#define OPER_AW_32() X2R_OPER(EA_AW_32(), m68ki_read_32)
#define OPER_AL_8() X2R_OPER(EA_AL_8(), m68ki_read_8)
#define OPER_AL_16() X2R_OPER(EA_AL_16(), m68ki_read_16)
#define OPER_AL_32() X2R_OPER(EA_AL_32(), m68ki_read_32)
#define OPER_PCDI_8() X2R_OPER(EA_PCDI_8(), m68ki_read_pcrel_8)
#define OPER_PCDI_16() X2R_OPER(EA_PCDI_16(), m68ki_read_pcrel_16)
#define OPER_PCDI_32() X2R_OPER(EA_PCDI_32(), m68ki_read_pcrel_32)
#define OPER_PCIX_8() X2R_OPER(EA_PCIX_8(), m68ki_read_pcrel_8)
#define OPER_PCIX_16() X2R_OPER(EA_PCIX_16(), m68ki_read_pcrel_16)
#define OPER_PCIX_32() X2R_OPER(EA_PCIX_32(), m68ki_read_pcrel_32)
