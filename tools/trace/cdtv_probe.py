#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Run a CDTV replay script and print what game state it reaches (docs/re-notes/cdtv-replays.md).

    python3 tools/trace/cdtv_probe.py --x2recomp build/x2recomp --assets DIR SCRIPT.txt [--at F ...] [--objects] [--every N]

For each probed frame it dumps chip RAM (1 MB, never written to the repo) and prints the engine variables of the CDTV build
(level, loop counter, level-complete flag, shop request, scroll position, energy, ships, credits, score, cash, ending flag), and
with --objects the hittable-object list (type, hit handler, HP at +$32) that the `pokeobj` directive and the boss replays use.
It also lists the disc files the game opened (the CD0: loader log) with the frame of each open, which shows the levels, the shop
and the speech banks a replay reached. Nothing printed contains game data beyond addresses and small variable values.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

VARS = [  # name, absolute address, size ('b','w','l')
    ("frame", 0xCE2, "w"), ("level", 0xE0A, "w"), ("loop", 0xE08, "w"), ("done", 0xA0D, "b"), ("shop", 0xA72, "b"),
    ("pos", 0xCD6, "w"), ("clamp", 0xCEE, "w"), ("energy", 0xCCC, "w"), ("ships", 0xCD0, "w"), ("credits", 0xCF6, "w"),
    ("score", 0xC98, "l"), ("cash", 0xCA0, "l"), ("ending", 0x3D24E, "b"), ("specials", 0xCA6, "w"),
    ("shopcol", 0x4F8, "w"), ("shoprow", 0x4FA, "w"),
]
OBJ_HEAD = 0xB52
OVERLAY_BASE, OVERLAY_SIZE = 0x4E26, 180000  # where X2 loads LEVELn/INTRO/PICCODE (docs/re-notes/cdtv-code.md)


def peek(d, a, size):
    if size == "b":
        return d[a]
    return struct.unpack(">H" if size == "w" else ">L", d[a:a + (2 if size == "w" else 4)])[0]


def objects(d, limit=300):
    out, p, seen = [], struct.unpack(">L", d[OBJ_HEAD:OBJ_HEAD + 4])[0], set()
    while p and p not in seen and p + 0x40 <= len(d) and len(out) < limit:
        seen.add(p)
        out.append((p, peek(d, p, "w"), peek(d, p + 0xA, "l"), peek(d, p + 0x32, "w")))
        p = peek(d, p + 0x12, "l")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--x2recomp", default="build/x2recomp")
    ap.add_argument("--assets", required=True)
    ap.add_argument("--at", type=int, action="append", default=[], help="frame to dump (repeatable)")
    ap.add_argument("--every", type=int, default=0, help="also dump every N frames")
    ap.add_argument("--objects", action="store_true")
    ap.add_argument("--overlay-only", action="store_true",
                    help="with --objects: only objects whose hit handler lies in the overlay buffer ($4E26 + 180,000 bytes)")
    a = ap.parse_args()
    frames = list(a.at)
    total = 0
    for line in open(a.script):
        if line.startswith("frames "):
            total = int(line.split()[1])
    if a.every:
        frames += list(range(a.every, total + 1, a.every))
    frames = sorted(set(f for f in frames if 0 < f < total or f == total - 1)) or [total - 1]
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [a.x2recomp, "--headless", "--game", "cdtv", "--assets", a.assets, "--report-every", "0", "--input-script", a.script,
               "--cdtv-log", os.path.join(tmp, "cd.log")]
        for f in frames:
            cmd += ["--dump-ram-at", f"{f}:{os.path.join(tmp, f'r{f}.ram')}"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        tail = [ln for ln in (r.stdout + r.stderr).splitlines() if "unmapped" in ln or "coverage" in ln or "error" in ln.lower()]
        print("\n".join(tail))
        print("# disc files opened (frame event name):")
        for ln in open(os.path.join(tmp, "cd.log")):
            if " open CD0:" in ln:
                p = ln.split()
                print(f"  {p[0]:>6} {p[3]}")
        for f in frames:
            d = open(os.path.join(tmp, f"r{f}.ram"), "rb").read()
            v = {n: peek(d, ad, sz) for n, ad, sz in VARS}
            print(f"frame {f}: " + " ".join(f"{n}={v[n]:X}" for n, _, _ in VARS))
            if a.objects:
                for p, t, h, hp in objects(d):
                    if a.overlay_only and not (OVERLAY_BASE <= h < OVERLAY_BASE + OVERLAY_SIZE):
                        continue
                    print(f"    obj {p:06X} type={t:04X} handler={h:08X} hp={hp:04X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
