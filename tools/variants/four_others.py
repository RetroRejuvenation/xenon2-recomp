#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Analyse a 'Xenon 2 Megablast & 4 Others' compilation (two ADFs, TOSEC '(19xx)(-)', plus its `[a]` variant).

usage: python3 tools/variants/four_others.py DISK1.adf DISK2.adf [--canonical-disc2 FILE] [--out DIR]

Disk 1 is a menu disk (`s/menu`, F1 = Xenon 2, 'Datadisk required') whose game file has an unrelated name: the
executable is found by structure, not by name. Prints sizes and hashes only; --out writes the game image's packed
executable and nothing else (analysis folder, git-ignored).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as c  # noqa: E402


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("disk1")
    ap.add_argument("disk2")
    ap.add_argument("--canonical-disc2")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    d1, d2 = c.read_adf(a.disk1), c.read_adf(a.disk2)
    print("disk 1 sha1 %s, boot-block CRC16 %04x" % (c.sha1(d1), c.crc16_arc(d1[:1024])))
    exes = [(p, b) for p, b in c.adf_files(d1) if b and c.is_game_exe(b)]
    for p, b in exes:
        print("game executable: file %r, %d bytes, sha1 %s (%s)" % (
            p, len(b), c.sha1(b), "identical to the BS1 executable" if c.sha1(b) == c.KNOWN_EXE_SHA1 else "DIFFERENT"))
    if not exes:
        print("no Xenon-II style executable found on disk 1")
    print("disk 2 sha1 %s, sector 0 id %r" % (c.sha1(d2), d2[2:6]))
    can = a.canonical_disc2 or c.default_canonical_disc2()
    if can:
        ref = open(can, "rb").read()
        runs = c.sector_runs(ref, d2)
        print("disk 2 vs canonical disc 2: %d differing sectors in runs %s" % (sum(y - x + 1 for x, y in runs), runs))
        for x, y in runs:
            blk = d2[x * 512:(y + 1) * 512]
            kinds = []
            if c.XCOPY_FILL in blk:
                kinds.append("XCOPY bad-track filler")
            if set(ref[x * 512:(y + 1) * 512]) <= {0, 0xE5}:
                kinds.append("canonical side is zero/format fill (unused)")
            print("  sectors %d-%d (track %d..%d): %s" % (x, y, x // 11, y // 11, "; ".join(kinds) or "other"))
    if a.out and exes:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "XENON-II.packed"), "wb") as f:
            f.write(exes[0][1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
