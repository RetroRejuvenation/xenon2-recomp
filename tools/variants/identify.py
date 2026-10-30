#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Say what an Amiga disk image (ADF or IPF) is, as far as the Xenon 2 variants go. Prints facts only.

usage: python3 tools/variants/identify.py IMAGE [IMAGE...]

For each image: the boot-block CRC16 and the WHDLoad imager's floppy version it maps to (v1..v6), whether the
file system is a normal OFS/FFS volume, any Xenon-II style executable (found by structure), the demo's `X2.BIN`
(379,232 bytes), a copier's bad-track filler, and whether the disc 2 id sector `X2B ` is present.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as c  # noqa: E402

DEMO_X2BIN_SIZE = 379232


def load(path):
    if path.lower().endswith(".ipf"):
        adf, reports, _ = c.ipf_to_adf(path)
        std = sum(1 for r in reports.values() if r.standard)
        return adf, "IPF, %d of %d tracks standard Amiga MFM" % (std, len(reports)), std == len(reports) == 160
    return c.read_adf(path), "ADF", True


def describe(path):
    adf, kind, full = load(path)
    print("== %s" % os.path.basename(path))
    print("  %s; sha1 %s" % (kind, c.sha1(adf)))
    crc = c.crc16_arc(adf[:1024])
    print("  boot-block CRC16 %04x -> imager floppy version %s" % (crc, c.IMAGER_VERSIONS.get(crc, "none of v1-v6")))
    print("  disc id sector: %s" % ("X2B (a Xenon 2 data disk)" if adf[2:6] == b"X2B " else "no"))
    if c.XCOPY_FILL in adf:
        print("  contains a copier's bad-track filler: some tracks were not read when this was copied")
    try:
        files = c.adf_files(adf) if full else []
    except Exception as e:  # damaged or custom layout
        files, why = [], str(e)
    else:
        why = "custom layout or not an OFS/FFS volume" if not files else ""
    if files:
        print("  %d files" % len(files))
    elif why:
        print("  no readable file system (%s)" % why)
    for p, b in files:
        if b and c.is_game_exe(b):
            print("  game executable (XENON-II style, unpacks to $3FC00+): %r, %d bytes, sha1 %s%s" % (
                p, len(b), c.sha1(b), " (the BS1 executable)" if c.sha1(b) == c.KNOWN_EXE_SHA1 else ""))
        if b and len(b) == DEMO_X2BIN_SIZE and p.upper().endswith("X2.BIN"):
            print("  %r is %d bytes: the playable DEMO program, not the full game" % (p, len(b)))


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    for p in argv:
        describe(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
