#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Analyse the 'Bombuzal + Xenon 2' compilation (SPS 1762; imager floppy version v2): two ordinary-track disks.

usage: python3 tools/variants/compilation_1762.py DISK1.(ipf|adf) DISK2.(ipf|adf) [--canonical-game FILE]
                                                   [--canonical-disc2 FILE] [--out DIR]

Findings it reproduces (hashes and offsets only):
  * every track of both IPFs decodes as standard Amiga MFM; track 1 of disk 1 is a constant fill (no Copylock);
  * disk 1 is a custom loader disk; the game image sits PLAIN (no XOR chain) at disk offset $70C00, `$3FC00` bytes;
  * that image is a different BUILD of the game from the canonical one: entry `bra.w $6C3A`, most code after
    `$D000` shifted by -1502 bytes; the runtime rejects it today (entry check) and would not reach its first VBL
    handler, so it needs its own runtime support;
  * disk 2: sector 0 (disc id) differs only after the id; modules 1-4 are identical to the canonical disc 2, the last
    sector of module 5 differs (171 bytes), and module 6 (sectors 1476-1676, shop and ending) is relinked for that
    build.
"""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as c  # noqa: E402

IMAGE_OFFSET = 0x70C00
IMAGE_SIZE = 0x3FC00
MODULES = [(11, 239), (250, 503), (514, 788), (799, 1143), (1154, 1465), (1476, 1676)]


def load(path):
    if path.lower().endswith(".ipf"):
        adf, reports, _ = c.ipf_to_adf(path)
        std = sum(1 for r in reports.values() if r.standard)
        return adf, "%d of %d tracks standard" % (std, len(reports))
    return c.read_adf(path), "ADF"


def shifts(ref, img, lo=0, hi=None, step=64, n=32):
    hi = hi or len(ref) - n
    cnt = collections.Counter()
    for off in range(lo, hi, step):
        ch = ref[off:off + n]
        if len(set(ch)) < 8:
            continue
        i = img.find(ch)
        if i >= 0:
            cnt[i - off] += 1
    return cnt.most_common(3)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("disk1")
    ap.add_argument("disk2")
    ap.add_argument("--canonical-game")
    ap.add_argument("--canonical-disc2")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    d1, n1 = load(a.disk1)
    d2, n2 = load(a.disk2)
    print("disk 1: sha1 %s (%s), boot-block CRC16 %04x = imager %s" % (
        c.sha1(d1), n1, c.crc16_arc(d1[:1024]), c.IMAGER_VERSIONS.get(c.crc16_arc(d1[:1024]), "unknown")))
    t1 = d1[5632:2 * 5632]
    print("disk 1 track 1: %s" % ("a constant fill byte %02x (no Copylock key track)" % t1[0] if len(set(t1)) == 1 else "varied data"))
    print("disk 2: sha1 %s (%s)" % (c.sha1(d2), n2))
    img = d1[IMAGE_OFFSET:IMAGE_OFFSET + IMAGE_SIZE]
    print("game image at $%X, plain: sha1 %s, entry word %s" % (IMAGE_OFFSET, c.sha1(img), img[:4].hex()))
    cg = a.canonical_game or c.default_canonical_game()
    if cg:
        ref = open(cg, "rb").read()
        print("  canonical entry word %s; bytes equal at same offset: %d of %d" % (
            ref[:4].hex(), sum(1 for x, y in zip(ref, img) if x == y), len(ref)))
        for lo in range(0, len(ref), 0x10000):
            print("  image offsets %#x..: most common shift of 32-byte chunks (shift, count): %s" % (
                lo, shifts(ref, img, lo, min(lo + 0x10000, len(ref) - 32))))
    cd = a.canonical_disc2 or c.default_canonical_disc2()
    if cd:
        ref = open(cd, "rb").read()
        runs = c.sector_runs(ref, d2)
        print("disk 2 vs canonical: %d differing sectors, runs %s" % (sum(y - x + 1 for x, y in runs), runs))
        for k, (lo, hi) in enumerate(MODULES, 1):
            nd = len([s_ for s_ in range(lo, hi + 1) if ref[s_ * 512:(s_ + 1) * 512] != d2[s_ * 512:(s_ + 1) * 512]])
            print("  module %d (sectors %d-%d): %s" % (k, lo, hi, "identical" if not nd else "%d of %d sectors differ" % (nd, hi - lo + 1)))
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "game_0400.bin"), "wb") as f:
            f.write(img)
        with open(os.path.join(a.out, "disc2.bin"), "wb") as f:
            f.write(d2)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
