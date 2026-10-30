# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Minimal 68000 instruction decoder for the recompiler: instruction length and control flow only.

Semantics never come from here (they are Musashi's handlers, see docs/re-notes/recompiler-design.md). A wrong
length here cannot cause a mistranslation: extension words beyond the decoded length are read from memory at run
time, and successor guesses are checked against REG_PC after every instruction. Lengths are cross-checked against
capstone (tools/dis68k.py) by recomp.py.
"""


def _ea_ext(mode, reg, size):
    """Extension bytes of an effective address; size is 'b', 'w' or 'l' (only matters for #imm)."""
    if mode in (5, 6):
        return 2
    if mode == 7:
        if reg == 0:
            return 2
        if reg == 1:
            return 4
        if reg in (2, 3):
            return 2
        if reg == 4:
            return 4 if size == "l" else 2
        return None  # invalid
    return 0


_SZ = {0: "b", 1: "w", 2: "l"}


def length(op):
    """Instruction length in bytes for opcode word `op`, or None if it is not a valid 68000 instruction form
    this decoder recognises (such instructions are left to the interpreter)."""
    g = op >> 12
    mode, reg = (op >> 3) & 7, op & 7
    sz2 = (op >> 6) & 3

    def ea(size):
        e = _ea_ext(mode, reg, size)
        return None if e is None else 2 + e

    if g == 0:
        if op in (0x003C, 0x023C, 0x0A3C, 0x007C, 0x027C, 0x0A7C):
            return 4
        if op & 0x0100:
            if mode == 1:
                return 4  # MOVEP
            return ea("b")
        if (op & 0x0F00) == 0x0800:
            e = ea("b")
            return None if e is None else e + 2
        if sz2 == 3:
            return None
        e = ea(_SZ[sz2])
        if e is None:
            return None
        return e + (4 if sz2 == 2 else 2)
    if g in (1, 2, 3):
        size = {1: "b", 3: "w", 2: "l"}[g]
        s = _ea_ext(mode, reg, size)
        dmode, dreg = (op >> 6) & 7, (op >> 9) & 7
        d = _ea_ext(dmode, dreg, size)
        if s is None or d is None:
            return None
        return 2 + s + d
    if g == 4:
        if op in (0x4E70, 0x4E71, 0x4E73, 0x4E75, 0x4E76, 0x4E77, 0x4AFC):
            return 2
        if op == 0x4E72:
            return 4
        if (op & 0xFFF0) == 0x4E40 or (op & 0xFFF8) == 0x4E58 or (op & 0xFFF0) == 0x4E60:
            return 2
        if (op & 0xFFF8) == 0x4E50:
            return 4
        if (op & 0xFF80) == 0x4E80:
            return ea("l")  # JSR/JMP
        if (op & 0xFB80) == 0x4880:
            if mode == 0:
                return 2  # EXT
            e = ea("l")
            return None if e is None else e + 2  # MOVEM
        if (op & 0xF1C0) in (0x41C0, 0x4180):
            return ea("l" if (op & 0xF1C0) == 0x41C0 else "w")
        return ea(_SZ.get(sz2, "w"))
    if g == 5:
        if sz2 == 3 and mode == 1:
            return 4  # DBcc
        return ea(_SZ.get(sz2, "b"))
    if g == 6:
        return 4 if (op & 0xFF) == 0 else 2
    if g == 7:
        return 2
    if g in (8, 9, 0xB, 0xC, 0xD):
        if sz2 == 3:
            size = "l" if (g in (9, 0xB, 0xD) and op & 0x100) else "w"
            return ea(size)
        if g in (8, 0xC) and (op & 0x1F0) == 0x100:
            return 2  # SBCD / ABCD
        if g == 0xC and (op & 0x130) == 0x100:
            return 2  # EXG
        if g in (9, 0xD) and (op & 0x130) == 0x100:
            return 2  # SUBX / ADDX
        if g == 0xB and (op & 0x138) == 0x108:
            return 2  # CMPM
        return ea(_SZ[sz2])
    if g == 0xE:
        if sz2 == 3:
            return ea("w")
        return 2
    return 2  # line A / line F


class Flow:
    """Control flow of one instruction: kind in {'seq','branch','jump','call','ret','stop'} plus a static target."""

    __slots__ = ("kind", "target")

    def __init__(self, kind, target=None):
        self.kind, self.target = kind, target


def _s8(v):
    return v - 0x100 if v & 0x80 else v


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def flow(op, pc, words):
    """Flow for the instruction at `pc`; `words` are its words (opcode first)."""
    ext = words[1:]
    if (op & 0xF000) == 0x6000:
        d = op & 0xFF
        tgt = (pc + 2 + (_s16(ext[0]) if d == 0 else _s8(d))) & 0xFFFFFFFF
        cond = (op >> 8) & 15
        if cond == 0:
            return Flow("jump", tgt)
        if cond == 1:
            return Flow("call", tgt)
        return Flow("branch", tgt)
    if (op & 0xF0F8) == 0x50C8:
        return Flow("branch", (pc + 2 + _s16(ext[0])) & 0xFFFFFFFF)
    if (op & 0xFF80) == 0x4E80:
        kind = "call" if (op & 0x40) == 0 else "jump"
        mode, reg = (op >> 3) & 7, op & 7
        tgt = None
        if mode == 7 and reg == 0:
            tgt = _s16(ext[0]) & 0xFFFFFFFF
        elif mode == 7 and reg == 1:
            tgt = (ext[0] << 16) | ext[1]
        elif mode == 7 and reg == 2:
            tgt = (pc + 2 + _s16(ext[0])) & 0xFFFFFFFF
        if kind == "jump" and tgt is None:
            return Flow("ret")  # computed jump: successor resolved at run time
        return Flow(kind, tgt)
    if op in (0x4E73, 0x4E75, 0x4E77):
        return Flow("ret")
    if op == 0x4AFC or (op & 0xF000) in (0xA000, 0xF000):
        return Flow("stop")
    return Flow("seq")
