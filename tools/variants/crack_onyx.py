#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Recover the game image AND disc 2 from the ONE-DISK crunches of Xenon 2 (Vision Factory/ONYX, VF [a], Black Monks [h]).

These disks are ordinary OFS disks holding a crunched executable (the whole game as a memory snapshot) and six
crunched data files whose names encode disc 2 sector ranges: "000b00e5" = sectors $000B.., $00E5 (229) of them, and so
on. Each data file decrunches (tools/variants/onyx_crunch.py) to exactly those sectors of the standard disc 2; the
executables decrunch with three different crunchers (ONYX, an Ice-style cruncher, ByteKiller) to a snapshot that holds
the game image at $400.
Disc 2 comes out identical to the standard one except sector 0 (see _cracklib.rebuild_disc2), which these disks do not
carry; pass --disc2 with your own standard disc 2 to use it unchanged.

usage: python3 -I tools/variants/crack_onyx.py DISK.adf --out DIR [--disc2 DISC2.adf] [--canonical game_0400.bin]
  exit 0 recovered, 3 recognised but not supported, 2 unrecognised or unreadable. DIR should be git-ignored.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import _cracklib as lib  # noqa: E402
import adf  # noqa: E402
import bkunpack  # noqa: E402
import hunk  # noqa: E402
import ice_crunch  # noqa: E402
import onyx_crunch  # noqa: E402

DATA_NAME = re.compile(r"^[0-9a-fA-F]{8}$")

# exe sha1 -> (variant, cruncher, hunk index, block offset in that hunk's code, memory address of snapshot byte 0, what differs)
EXES = {
    "87fa47ae859d6a7c285f6d511b83c5ce152be535": (
        "Vision Factory / ONYX (VF, also the TOSEC 'PNA h VF t+1')", "onyx", 0, 0x20A, 0x80,
        "credit text (181 bytes), the disk-loader routine at $921A (910 bytes, replaced by the crunch's own loader, which "
        "the runtime's disk emulation bypasses) and 10 bytes elsewhere; behaviour identical to the canonical image"),
    "7b27789cfdb14244a989ff97e5cde70f59ad8b27": (
        "Vision Factory [a]", "ice", 2, 0x196, 0x80,
        "as VF plus 13 bytes (small data bytes around $D13E-$D2BD and $92C6-$953E); behaviour identical after the first "
        "screens, one frame hash differs near frame 500"),
    "48a31ecb9c009cc0b9a04f13f4a7db2a990ad51f": (
        "Black Monks [h] [t +36] (trainer menu build)", "bs1", 0, 0xE8, 0x0,
        "credit text, trainer hooks (876 bytes at $7290-$75FC and patches at $F78, $FD4, $206C, $20D6) and the loader "
        "routine at $921A; the hooks change gameplay (the replays diverge): a non-canonical build"),
}


def exe_candidates(a):
    for e in a.walk():
        if e["kind"] == "file" and e["size"] > 100000:
            yield e


def snapshot(variant, blob):
    cruncher, hidx, off = variant[1], variant[2], variant[3]
    code = hunk.parse(blob)["hunks"][hidx].data
    if cruncher == "onyx":
        return onyx_crunch.decrunch(code, off)
    if cruncher == "ice":
        ulen, plen = struct.unpack_from(">II", code, off)
        return ice_crunch.depack(code, off + 8, ulen, plen)
    out, _crc, _pos, _start = bkunpack.bs1(code, off)
    return out


def main(argv):
    usage = __doc__
    pos, opts = lib.parse_args(argv, usage)
    if len(pos) != 1 or "out" not in opts:
        print(usage)
        return 2
    try:
        a = adf.Adf(pos[0])
        files = {e["path"].lower(): a.read_file(e) for e in a.walk() if e["kind"] == "file"}
    except (OSError, ValueError, IndexError) as e:
        print("error: %s" % e)
        return 2
    exe = None
    for name, data in files.items():
        if lib.sha1(data) in EXES:
            exe = (name, data)
    if exe is None:
        print("not a recognised one-disk Xenon 2 crunch (no known executable on the disk)")
        return 2
    variant = EXES[lib.sha1(exe[1])]
    base = variant[4]
    try:
        snap = snapshot(variant, exe[1])
        data_files = {n: d for n, d in files.items() if "/" not in n and DATA_NAME.match(n)}
        chunks = {}
        for n, d in data_files.items():
            start, count = int(n[:4], 16), int(n[4:], 16)
            out = onyx_crunch.decrunch(d)
            if len(out) != count * 512:
                raise ValueError("%s decrunches to %d bytes, expected %d" % (n, len(out), count * 512))
            chunks[start] = out
        if len(chunks) != 6:
            raise ValueError("expected 6 data files, found %d" % len(chunks))
    except (ValueError, IndexError, onyx_crunch.Crunch, ice_crunch.Ice, struct.error) as e:
        print("error: %s" % e)
        return 2
    lo = lib.GAME_BASE - base
    game = bytes(snap[lo:lo + lib.GAME_SIZE])
    disc2 = lib.rebuild_disc2(chunks)
    notes = ["differs from the canonical image: " + variant[5]]
    if "disc2" in opts:
        with open(opts["disc2"], "rb") as f:
            std = f.read()
        if lib.sha1(std) != lib.DISC2_SHA1:
            notes.append("warning: the given disc 2 is not the standard one; using it anyway")
        elif std[512:] != disc2[512:]:
            notes.append("warning: the given disc 2 differs from the rebuilt chunks beyond sector 0")
        disc2 = std
    else:
        notes.append("disc 2 rebuilt from the six data files: identical to the standard disc 2 except sector 0 "
                     "(only the disc id is restored; replays are unaffected); give --disc2 for the exact disc")
    lib.write_outputs(opts["out"], game, disc2)
    lib.report(variant[0], game, disc2, opts, notes)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
