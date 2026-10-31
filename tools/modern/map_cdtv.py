#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Map disk-build code addresses to the CDTV build by instruction shape (docs/re-notes/cdtv-modern.md).

The CDTV engine is the disk build's code re-assembled with long absolute addressing, so addresses drift while the
instruction sequences stay the same. Both images are swept linearly, each instruction is reduced to its mnemonic and
register operands (numbers and absolute-address sizes dropped), and unique 8-instruction windows that occur once in
each image anchor the mapping; from an anchor the two streams are walked in step.

usage: python3 -I tools/modern/map_cdtv.py DISK_IMAGE CDTV_IMAGE ADDR [ADDR...]
       python3 -I tools/modern/map_cdtv.py DISK_IMAGE CDTV_IMAGE --show ADDR COUNT   (side by side listing)
       python3 -I tools/modern/map_cdtv.py DISK_IMAGE CDTV_IMAGE --vars ADDR [ADDR...] (variable address mapping by vote)
Images are the importer's `game_0400.bin` of each build (both live at $400). Prints `disk -> cdtv` per address, or
`?` when no anchor reaches it. No game bytes are printed.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from dis68k import Dis  # noqa: E402

BASE = 0x400
_NUM = re.compile(r"\$[0-9a-fA-F]+|#-?\d+|-?\d+")
_ABS = re.compile(r"(?<=[N)])\.[wl]")


def shape(mnem, ops):
    s = _ABS.sub("", _NUM.sub("N", ops))
    return mnem.split(".")[0] + ("." + mnem.split(".")[1] if "." in mnem else "") + " " + s


def sweep(data, start, end):
    dis = Dis()
    out = []
    for a, _b, m, ops in dis.sweep(data, BASE, start - BASE, end - start):
        out.append((a, shape(m, ops)))
    return out


def anchors(da, db, n=8):
    ia, ib = {}, {}
    for idx in range(len(da) - n):
        k = tuple(s for _, s in da[idx:idx + n])
        ia[k] = idx if k not in ia else None
    for idx in range(len(db) - n):
        k = tuple(s for _, s in db[idx:idx + n])
        ib[k] = idx if k not in ib else None
    pairs = [(ia[k], ib[k]) for k in ia if ia[k] is not None and ib.get(k) is not None]
    pairs.sort()
    # keep a monotonic chain
    chain, last = [], -1
    for a, b in pairs:
        if b > last:
            chain.append((a, b))
            last = b
    return chain


def map_addresses(disk, cdtv, addrs, disk_end=None, cdtv_end=None):
    da = sweep(disk, BASE, disk_end or BASE + len(disk))
    db = sweep(cdtv, BASE, cdtv_end or BASE + len(cdtv))
    chain = anchors(da, db)
    aidx = {a: i for i, (a, _) in enumerate(da)}
    res = {}
    for want in addrs:
        i = aidx.get(want)
        if i is None:
            res[want] = None
            continue
        best = None
        for ca, cb in chain:
            if ca <= i:
                best = (ca, cb)
            else:
                break
        if best is None:
            res[want] = None
            continue
        ca, cb = best
        ok = True
        while ca < i:
            if da[ca][1] != db[cb][1]:
                ok = False
                break
            ca += 1
            cb += 1
        res[want] = db[cb][0] if ok and ca == i and da[ca][1] == db[cb][1] else None
    return res


_ABSNUM = re.compile(r"\$([0-9a-fA-F]+)(?=\)?\.[wl]|\b)")


def _absolutes(ops):
    """Absolute-address operands (`$xxx.w` / `$xxx.l`) of one operand string, in order; immediates (#$..) are skipped."""
    out = []
    for m in re.finditer(r"(?<![#\w])\$([0-9a-fA-F]+)(?:\)?\.[wl])", ops):
        out.append(int(m.group(1), 16))
    return out


def variable_votes(disk, cdtv, lo=0x400, hi=0xC6000):
    """Vote for disk variable address -> CDTV address from the absolute operands of every aligned instruction pair."""
    dis = Dis()
    da = sweep(disk, BASE, BASE + len(disk))
    db = sweep(cdtv, BASE, BASE + len(cdtv))
    chain = anchors(da, db)
    votes = {}
    # walk every anchor forward until the shapes diverge
    seen = set()
    for ca, cb in chain:
        while ca < len(da) and cb < len(db) and da[ca][1] == db[cb][1] and ca not in seen:
            seen.add(ca)
            sa, ma, oa = dis.one(bytes(disk[da[ca][0] - BASE:da[ca][0] - BASE + 10]), da[ca][0])
            sb, mb, ob = dis.one(bytes(cdtv[db[cb][0] - BASE:db[cb][0] - BASE + 10]), db[cb][0])
            xa, xb = _absolutes(oa), _absolutes(ob)
            if len(xa) == len(xb):
                for u, v in zip(xa, xb):
                    if lo <= u < hi and lo <= v < hi and not (ma.startswith("j") or ma.startswith("b")):
                        votes.setdefault(u, {}).setdefault(v, 0)
                        votes[u][v] += 1
            ca += 1
            cb += 1
    return votes


def variable_map(disk, cdtv, addrs):
    votes = variable_votes(disk, cdtv)
    res = {}
    for a in addrs:
        v = votes.get(a)
        if not v:
            res[a] = None
            continue
        best = sorted(v.items(), key=lambda kv: -kv[1])
        res[a] = (best[0][0], best[0][1], sum(v.values()))
    return res


def show_pairs(disk, cdtv, start, count):
    """Side by side listing of `count` instructions from disk address `start` and the mapped CDTV address."""
    dis = Dis()
    m = map_addresses(disk, cdtv, [start])[start]
    if m is None:
        print("no mapping for %06x" % start)
        return
    a, b = start, m
    for _ in range(count):
        sa, ma, oa = dis.one(bytes(disk[a - BASE:a - BASE + 10]), a)
        sb, mb, ob = dis.one(bytes(cdtv[b - BASE:b - BASE + 10]), b)
        print("%05x %-28s | %05x %s" % (a, ("%s %s" % (ma, oa))[:28], b, "%s %s" % (mb, ob)))
        a += sa
        b += sb


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    disk = open(argv[1], "rb").read()
    cdtv = open(argv[2], "rb").read()[:437404]
    if argv[3] == "--show":
        show_pairs(disk, cdtv, int(argv[4], 0), int(argv[5], 0))
        return 0
    if argv[3] == "--vars":
        for a, r in variable_map(disk, cdtv, [int(x, 0) for x in argv[4:]]).items():
            print("%06x -> %s" % (a, "?" if r is None else "%06x (%d of %d votes)" % r))
        return 0
    addrs = [int(a, 0) for a in argv[3:]]
    for a, b in map_addresses(disk, cdtv, addrs).items():
        print("%06x -> %s" % (a, "?" if b is None else "%06x" % b))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
