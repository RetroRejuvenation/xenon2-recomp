#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Helpers of the --dump-audio round-trip test (tests/audio/dump_roundtrip.cmake), standard library only.
  check_roundtrip.py zip DIR OUT.zip           store DIR's files uncompressed in a ZIP with manifest.json at its root
  check_roundtrip.py corr A.wav B.wav MIN      Pearson correlation of two 16-bit WAVs (both channels, every 3rd frame); exit 1 below MIN
  check_roundtrip.py same A.wav B.wav          exit 1 unless the two files hold identical samples"""
import array
import math
import os
import sys
import wave
import zipfile


def samples(path):
    w = wave.open(path, "rb")
    a = array.array("h")
    a.frombytes(w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    return a


def main():
    cmd = sys.argv[1]
    if cmd == "zip":
        src, out = sys.argv[2], sys.argv[3]
        with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
            for root, _, files in sorted(os.walk(src)):
                for f in sorted(files):
                    full = os.path.join(root, f)
                    z.write(full, os.path.relpath(full, src).replace(os.sep, "/"))
        return 0
    if cmd == "same":
        return 0 if samples(sys.argv[2]) == samples(sys.argv[3]) else 1
    if cmd == "corr":
        a, b, want = samples(sys.argv[2]), samples(sys.argv[3]), float(sys.argv[4])
        if len(a) != len(b):
            print("lengths differ: %d and %d samples" % (len(a), len(b)), file=sys.stderr)
            return 1
        n = 0
        sa = sb = saa = sbb = sab = 0.0
        for i in range(0, len(a) - 1, 6):  # every third stereo frame, both channels
            for j in (i, i + 1):
                x, y = a[j], b[j]
                n += 1
                sa += x
                sb += y
                saa += x * x
                sbb += y * y
                sab += x * y
        ma, mb = sa / n, sb / n
        va, vb = saa / n - ma * ma, sbb / n - mb * mb
        c = (sab / n - ma * mb) / math.sqrt(va * vb) if va > 0 and vb > 0 else 0.0
        print("correlation %.5f (rms %.0f and %.0f)" % (c, math.sqrt(saa / n), math.sqrt(sbb / n)))
        return 0 if c >= want else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
