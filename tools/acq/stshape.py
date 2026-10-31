# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Instruction-shape window matching between 68000 images (docs/re-notes/atari-st-analysis.md, same method as
# docs/re-notes/cdtv-code.md section 5 and tools/modern/map_cdtv.py): every instruction is reduced to its mnemonic and
# operand shape (numbers, absolute addresses and branch targets dropped), and a window of N consecutive shapes from one
# image is "found" in another image if the same window occurs there. A linear sweep is used, so data decoded as
# instructions adds noise to both sides; the control (a data image) measures how much. Also a byte-window comparison
# (windows of 48 bytes with some variety) for shared tables and data.
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import dis68k  # noqa: E402

_NUM = re.compile(r"\$[0-9a-fA-F]+|#-?\d+|-?\d+")
_ABS = re.compile(r"(?<=[N)])\.[wl]")


def shape(mnem, ops):
    s = _ABS.sub("", _NUM.sub("N", ops))
    return mnem + " " + s


def shapes(data, dis=None):
    """Linear sweep of data; returns the list of instruction shapes (dc.w fallbacks are skipped)."""
    dis = dis or dis68k.Dis()
    out = []
    for _a, _b, m, ops in dis.sweep(data, 0, 0, len(data)):
        if m != "dc.w":
            out.append(shape(m, ops))
    return out


def windows(seq, n):
    return [tuple(seq[i:i + n]) for i in range(len(seq) - n + 1)]


def match_fraction(a_windows, b_set):
    if not a_windows:
        return 0.0, 0, 0
    hit = sum(1 for w in a_windows if w in b_set)
    return hit / len(a_windows), hit, len(a_windows)


def byte_windows(data, n=48, step=2):
    """Windows of n bytes that are not low-variety; returns {window: first offset}."""
    out = {}
    for o in range(0, len(data) - n + 1, step):
        w = data[o:o + n]
        if len(set(w)) >= 8:
            out.setdefault(bytes(w), o)
    return out


def byte_overlap(a, b, n=48):
    wb = byte_windows(b, n)
    wa = [bytes(a[o:o + n]) for o in range(0, len(a) - n + 1, 2) if len(set(a[o:o + n])) >= 8]
    if not wa:
        return 0.0, 0, 0
    hit = sum(1 for w in wa if w in wb)
    return hit / len(wa), hit, len(wa)


def compare(name_a, a, name_b, b, n=6, dis=None):
    dis = dis or dis68k.Dis()
    sa, sb = shapes(a, dis), shapes(b, dis)
    wa, wb = windows(sa, n), windows(sb, n)
    seta, setb = set(wa), set(wb)
    fa = match_fraction(wa, setb)
    fb = match_fraction(wb, seta)
    return {
        "a": name_a, "b": name_b, "n": n, "instr_a": len(sa), "instr_b": len(sb),
        "a_in_b": fa, "b_in_a": fb,
    }


def main(argv):
    if len(argv) < 3:
        print("usage: stshape.py FILE_A FILE_B [FILE_CONTROL|-] [WINDOW]")
        return 2
    a, b = open(argv[1], "rb").read(), open(argv[2], "rb").read()
    n = int(argv[4]) if len(argv) > 4 else 6
    dis = dis68k.Dis()
    r = compare(argv[1], a, argv[2], b, n, dis)
    print("%s vs %s (windows of %d): %d / %d instructions" % (os.path.basename(r["a"]), os.path.basename(r["b"]), n,
                                                              r["instr_a"], r["instr_b"]))
    print("  A found in B: %.1f%% (%d of %d)" % (100 * r["a_in_b"][0], r["a_in_b"][1], r["a_in_b"][2]))
    print("  B found in A: %.1f%% (%d of %d)" % (100 * r["b_in_a"][0], r["b_in_a"][1], r["b_in_a"][2]))
    if len(argv) > 3 and argv[3] != "-":
        c = open(argv[3], "rb").read()
        rc = compare(argv[1], a, argv[3], c, n, dis)
        print("  control (A vs %s): %.1f%%" % (os.path.basename(argv[3]), 100 * rc["a_in_b"][0]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
