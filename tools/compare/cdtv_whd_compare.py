#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Compare the runtime's CDTV pictures with the WHDLoad-slave reference captures (docs/re-notes/cdtv-reference-whd.md).

Inputs (all git-ignored, under generated/ref/):
  runs/<run>/frame_NNNNNN.png   x2ref harness shots of the WHDLoad run (377x288 buffer), every N frames
  <ours>/oNNNNN.png             runtime shots (x2recomp --screenshot-at F:FILE, 320x256), every N frames

The two runs have different timelines (boot, loading speed, page timers), so frames are paired by picture, not by number:
each reference shot is matched to the runtime shot that looks most like it (a coarse thumbnail search, then an exact
comparison of the top candidates inside the 320x256 window at a fixed offset found once by cross-correlation). Needs numpy
(development tool; not used by the build or the tests).

  cdtv_whd_compare.py REF_DIR OURS_DIR [--json OUT.json] [--offset X,Y] [--first F] [--last L]
"""
import argparse
import glob
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import read_png  # noqa: E402

OW, OH = 320, 256


def load(path):
    w, h, rows = read_png(path)
    return np.frombuffer(b"".join(rows), dtype=np.uint8).reshape(h, w, 3)


def number(path):
    return int(re.search(r"(\d+)\.png$", path).group(1))


def thumb(img, f=8):
    h, w = img.shape[:2]
    h8, w8 = h // f * f, w // f * f
    return img[:h8, :w8].reshape(h8 // f, f, w8 // f, f, 3).astype(np.float32).mean(axis=(1, 3))


def find_offset(ref, ours):
    """Where the runtime's picture sits in the reference buffer (zero-mean FFT cross-correlation of the grey levels)."""
    r = ref.astype(np.float32).mean(axis=2)
    o = ours.astype(np.float32).mean(axis=2)
    r -= r.mean()
    oo = np.zeros_like(r)
    oo[: o.shape[0], : o.shape[1]] = o - o.mean()
    c = np.fft.irfft2(np.fft.rfft2(r) * np.conj(np.fft.rfft2(oo)), s=r.shape)
    y, x = np.unravel_index(np.argmax(c), c.shape)
    return int(x), int(y), float(c.max() / (np.linalg.norm(r) * np.linalg.norm(o - o.mean()) + 1e-9))


def crop(ref, off):
    x, y = off
    return ref[y:y + OH, x:x + OW]


def exact(a, b):
    n = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    a, b = a[: n[0], : n[1]], b[: n[0], : n[1]]
    return float((a == b).all(axis=2).mean())


def near(a, b, tol=17):
    n = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    a, b = a[: n[0], : n[1]].astype(np.int16), b[: n[0], : n[1]].astype(np.int16)
    return float((np.abs(a - b).max(axis=2) <= tol).mean())


def colours(img):
    return set(map(tuple, np.unique(img.reshape(-1, 3), axis=0)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref_dir")
    ap.add_argument("ours_dir")
    ap.add_argument("--json")
    ap.add_argument("--offset")
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, default=10 ** 9)
    a = ap.parse_args()
    refs = sorted(p for p in glob.glob(os.path.join(a.ref_dir, "frame_*.png")) if a.first <= number(p) <= a.last)
    ours = sorted(glob.glob(os.path.join(a.ours_dir, "o*.png")), key=number)
    ours_img = {number(p): load(p) for p in ours}
    thumbs = {k: thumb(v) for k, v in ours_img.items()}
    keys = sorted(thumbs)
    tmat = np.stack([thumbs[k].ravel() for k in keys])
    if a.offset:
        off = tuple(int(v) for v in a.offset.split(","))
    else:
        votes = {}
        for p in refs[:: max(1, len(refs) // 12)]:
            r = load(p)
            best = None
            for k in keys[:: max(1, len(keys) // 25)]:
                x, y, s = find_offset(r, ours_img[k])
                if best is None or s > best[0]:
                    best = (s, x, y)
            if best[0] > 0.8:
                votes[(best[1], best[2])] = votes.get((best[1], best[2]), 0) + 1
        off = max(votes, key=votes.get) if votes else (37, 18)
    print("offset of the runtime picture in the reference buffer: x=%d y=%d" % off)
    out = []
    for p in refs:
        r = crop(load(p), off)
        if r.shape[0] < OH or r.shape[1] < OW:
            continue
        rt = thumb(r).ravel()
        d = np.abs(tmat - rt).mean(axis=1)
        cand = np.argsort(d)[:3]
        best = None
        for ci in cand:
            e = exact(r, ours_img[keys[ci]])
            if best is None or e > best[0]:
                best = (e, keys[ci])
        e, k = best
        n = near(r, ours_img[k])
        cr, co = colours(r), colours(ours_img[k])
        out.append({"ref": number(p), "ours": k, "exact": round(e, 4), "near": round(n, 4),
                    "colours_ref_only": len(cr - co), "colours_ours_only": len(co - cr), "colours_shared": len(cr & co)})
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"offset": off, "pairs": out}, f, indent=1)
    ex = np.array([o["exact"] for o in out])
    print("reference shots compared: %d" % len(out))
    for th in (0.999, 0.99, 0.95, 0.9):
        print("  exact match >= %.1f%%: %d (%.0f%%)" % (th * 100, int((ex >= th).sum()), 100 * (ex >= th).mean()))
    print("  median exact %.4f, median near %.4f" % (np.median(ex), np.median([o["near"] for o in out])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
