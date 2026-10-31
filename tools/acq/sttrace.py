# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Static decryption of the Atari ST "trace-vector" loader found on the Xenon 2 ST disks.
#
# The loader turns the 68000 trace bit on (an `illegal` handler toggles T in the stacked SR). The trace handler then
# decrypts each instruction just before it runs and puts the encrypted bytes back afterwards:
#
#     restore  long at previous_pc   <- saved encrypted long
#     save     long at pc            -> saved
#     key  = swap(~long at pc-4)      (the 4 bytes just before the instruction, which are encrypted at that moment)
#     long at pc ^= key
#
# So the plain first longword of an instruction at A is  raw(A) ^ swap(~raw(A-4))  with RAW memory, and only the
# first 4 bytes of an instruction are ever encrypted. This module follows the control flow statically (bra, bcc,
# bsr/jsr fall-through, dbra) over a raw memory image and returns the decrypted instructions.
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import dis68k  # noqa: E402


def plain_long(mem, base, addr):
    """Plain first longword of the instruction at addr (mem is the raw image mapped at base)."""
    o = addr - base
    raw = int.from_bytes(mem[o:o + 4], "big")
    prev = int.from_bytes(mem[o - 4:o], "big")
    key = ((~prev) & 0xFFFFFFFF)
    key = ((key << 16) | (key >> 16)) & 0xFFFFFFFF  # swap
    return (raw ^ key).to_bytes(4, "big")


def instruction_at(dis, mem, base, addr):
    o = addr - base
    data = plain_long(mem, base, addr) + bytes(mem[o + 4:o + 12])
    size, mnem, ops = dis.one(data, addr)
    return size, bytes(data[:size]), mnem, ops


def follow(mem, base, start, limit=4000, stop=None):
    """Follow decrypted control flow from start. Returns {addr: (size, bytes, mnemonic, operands)}."""
    dis = dis68k.Dis()
    seen, todo = {}, [start]
    while todo and len(seen) < limit:
        a = todo.pop()
        while a not in seen and base <= a < base + len(mem) - 12 and (stop is None or a not in stop):
            size, b, m, ops = instruction_at(dis, mem, base, a)
            seen[a] = (size, b, m, ops)
            nxt = a + size
            m = m.split(".")[0]  # bra.w / bra.s / jmp.l all mean the same here
            tgt = _target(m, ops, a)
            if m in ("rts", "rte", "rtr"):
                break
            if m == "illegal":
                nxt = a + size  # the toggling `illegal` ends the traced region; keep sweeping for the report
                break
            if m in ("bra", "jmp") and tgt is not None:
                a = tgt
                continue
            if m in ("jmp",):
                break
            if tgt is not None and (m.startswith("b") or m.startswith("db") or m in ("jsr",)):
                todo.append(tgt)
            a = nxt
    return seen


def _target(m, ops, addr):
    if (m.startswith("b") or m.startswith("db") or m in ("jsr", "jmp")) and "$" in ops and "(" not in ops:
        try:
            return int(ops.split("$")[-1].split(",")[0].strip(), 16)
        except ValueError:
            return None
    return None


def main(argv):
    if len(argv) < 4:
        print("usage: sttrace.py RAW_IMAGE BASE START [LIMIT]")
        return 2
    mem = open(argv[1], "rb").read()
    base, start = int(argv[2], 0), int(argv[3], 0)
    res = follow(mem, base, start, int(argv[4], 0) if len(argv) > 4 else 4000)
    for a in sorted(res):
        size, b, m, ops = res[a]
        print("%08x  %-16s %s %s" % (a, b.hex(), m, ops))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
