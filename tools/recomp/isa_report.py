#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Instruction-set usage of the game, static (all translated instructions, from generated/recomp/manifest.json)
and dynamic (instructions executed in a run, from a runtime/trace JSON written by x2recomp --trace-out).

usage: python3 -I tools/recomp/isa_report.py [--recomp generated/recomp] [--trace RUN.json]

Groups are Musashi handler families (mnemonic_size_mode); the mode suffix is Musashi's addressing-mode code
(d, a, ai, pi, pd, di, ix, aw, al, pcdi, pcix, i, ...).
"""
import argparse
import collections
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recomp", default=os.path.join(ROOT, "generated", "recomp"))
    ap.add_argument("--trace")
    a = ap.parse_args()
    man = json.load(open(os.path.join(a.recomp, "manifest.json")))
    names = re.findall(r"^\t\{(m68k_op_\w+)\s*,", open(os.path.join(a.recomp, "musashi", "m68kops.c")).read(), re.M)
    optable = {}
    for line in open(os.path.join(a.recomp, "musashi", "optable.txt")):
        op, idx, cyc = (int(x) for x in line.split())
        optable[op] = "m68k_op_illegal" if idx < 0 else names[idx]
    print("static: %d translated instructions, %d Musashi handlers, %d mnemonics" % (
        sum(man["instructions"].values()), len(man["handlers"]), len(man["mnemonics"])))
    if not a.trace:
        return
    t = json.load(open(a.trace))
    import sys
    sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))
    assets = os.path.join(ROOT, "generated", "assets")
    game = open(os.path.join(assets, "game_0400.bin"), "rb").read()
    disc2 = open(os.path.join(assets, "disc2.bin"), "rb").read()
    ep = json.load(open(os.path.join(ROOT, "generated", "lane-d2", "entrypoints_v2.json")))
    sectors = ep["images"]["level_modules"]["sectors"]
    handlers, mnem, total = collections.Counter(), collections.Counter(), 0
    for mod in t["modules"]:
        if mod["name"] == "game":
            mem, base = game, 0x400
        else:
            m = re.match(r"level(\d)", mod["name"])
            if not m:
                print("skipping trace module %s" % mod["name"])
                continue
            s, n = sectors[int(m.group(1)) - 1]
            mem, base = disc2[s * 512:(s + n) * 512], 0x54E00
        for pc, count, ln, flags in mod["insns"]:
            o = pc - base
            if o < 0 or o + 2 > len(mem):
                continue
            op = (mem[o] << 8) | mem[o + 1]
            h = optable[op][len("m68k_op_"):]
            handlers[h] += count
            mnem[h.split("_")[0]] += count
            total += count
    print("dynamic (%s): %d instructions executed, %d distinct handlers, %d mnemonics" % (
        os.path.basename(a.trace), total, len(handlers), len(mnem)))
    print("mnemonics by executions:")
    for k, v in mnem.most_common():
        print("  %-8s %10d  %6.2f%%" % (k, v, 100.0 * v / total))
    print("handlers:")
    print("  " + " ".join(sorted(handlers)))


if __name__ == "__main__":
    main()
