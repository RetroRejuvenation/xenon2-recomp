#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Compare our Atari ST pictures with Hatari's, frame by frame (docs/re-notes/atari-st-video.md).

Hatari's screenshots (tools/compare/hatari-harness) are 832x552: a 416x276 raster drawn at 2x, the 320x200 picture at
(48, 29) in raster pixels. Ours are the 320x200 picture (x2recomp --game st --st-shots-dir). Colours are compared as
3-bit gun indices (Hatari draws a gun value g as 34*g, we draw it as 255*g/7), so a pixel matches when all three guns agree.

usage: st_compare.py HATARI_DIR OURS_DIR --offset N [--search W] [--from A] [--to B] [--csv FILE]
  A Hatari shot n is compared with our frames n-N-W .. n-N+W (the best one wins; the shift is reported, because the two
  machines boot at different times and their busy loops run at slightly different speeds). Shots are shot_<n>.png in both.
"""
import argparse
import glob
import os
import re
import statistics
import sys

import numpy as np
from PIL import Image

CROP_X, CROP_Y, W, H = 48, 29, 320, 200


def hatari_indices(path):
    a = np.asarray(Image.open(path).convert("RGB"))[::2, ::2][CROP_Y:CROP_Y + H, CROP_X:CROP_X + W]
    return np.rint(a.astype(np.float32) / 34.0).astype(np.uint8)


def our_indices(path):
    a = np.asarray(Image.open(path).convert("RGB"))
    return (a >> 5).astype(np.uint8)


def numbers(d):
    out = {}
    for f in glob.glob(os.path.join(d, "shot_*.png")):
        m = re.search(r"shot_(\d+)\.png$", f)
        if m:
            out[int(m.group(1))] = f
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hatari")
    ap.add_argument("ours")
    ap.add_argument("--offset", type=int, required=True)
    ap.add_argument("--search", type=int, default=0)
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=10**9)
    ap.add_argument("--csv")
    args = ap.parse_args()
    hs, os_ = numbers(args.hatari), numbers(args.ours)
    cache = {}

    def ours(n):
        if n not in cache:
            cache[n] = our_indices(os_[n]) if n in os_ else None
        return cache[n]

    rows = []
    for hn in sorted(hs):
        if not (args.lo <= hn <= args.hi):
            continue
        h = hatari_indices(hs[hn])
        best = None
        for d in range(-args.search, args.search + 1):
            n = hn - args.offset + d
            o = ours(n)
            if o is None:
                continue
            same = float((o == h).all(axis=2).mean())
            if best is None or same > best[0]:
                best = (same, d, n)
        if best:
            rows.append((hn, best[2], best[1], best[0]))
    if not rows:
        print("no frames compared")
        return 1
    fr = [r[3] for r in rows]
    print("frames compared: %d" % len(rows))
    print("exact pixel match: median %.4f  mean %.4f  min %.4f" % (statistics.median(fr), statistics.mean(fr), min(fr)))
    for t in (0.9999, 0.999, 0.99, 0.95):
        print("  >= %.2f%%: %d (%.1f%%)" % (t * 100, sum(f >= t for f in fr), 100.0 * sum(f >= t for f in fr) / len(fr)))
    sh = [r[2] for r in rows]
    print("best frame shift: median %d, min %d, max %d" % (statistics.median(sh), min(sh), max(sh)))
    if args.csv:
        with open(args.csv, "w") as f:
            f.write("hatari_vbl,our_frame,shift,match\n")
            for r in rows:
                f.write("%d,%d,%d,%.6f\n" % r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
