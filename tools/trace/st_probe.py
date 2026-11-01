#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Probe the Atari ST release: run `x2recomp --game st` headless on a script and print the game state over time.

  st_probe.py --exe build/x2recomp --disk game.st --script tests/replays/st/level2.txt [--every 500] [--at F ...] [--objects]
              [--loads] [-- EXTRA ARGS]

The columns are the run report's fields (level, energy, ships, credits, score, unmapped accesses). With --at F (repeatable) or
--dump-every N the machine's RAM is dumped at those frames (512 KB, a temporary file, never kept) and the engine variables are read
from it; --objects adds the hittable-object list ($B4C: type, hit handler at +$A, HP at +$32), which the `pokeobj` directive and
the boss replays use; --loads lists the disk reads of the game's level loads (side 1, tracks 0-13) with their frames, which shows
which levels a replay reached. Extra arguments after `--` go to x2recomp (--god-mode, --credits N, --st-watch-pc ADDR[:LEN] ...).
Nothing printed contains game data beyond addresses and small variable values. Used to map the coverage pokes
(docs/re-notes/atari-st-hooks.md) and to state what each replay exercises (docs/re-notes/atari-st-replays.md).
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

FIELDS = ("level", "energy", "ships", "credits", "score", "unmapped")

VARS = [  # name, absolute address, size ('b','w','l'): the disk build's engine variable block at the ST's addresses
    ("level", 0xE04, "w"), ("loop", 0xE02, "w"), ("done", 0xA07, "b"), ("shop", 0xA6C, "b"), ("pos", 0xCD0, "w"),
    ("clamp", 0xCE8, "w"), ("energy", 0xCC6, "w"), ("ships", 0xCCA, "w"), ("credits", 0xCF0, "w"), ("score", 0xC92, "l"),
    ("cash", 0xC9A, "l"), ("player", 0x428, "w"), ("guards", 0xD7A, "w"), ("specials", 0xCA0, "w"), ("dying", 0xD84, "b"),
    ("shopcol", 0x4F2, "w"), ("shoprow", 0x4F4, "w"), ("tick", 0xCDC, "w"),
]
OBJ_HEAD = 0xB4C


def peek(d, a, size):
    if size == "b":
        return d[a]
    n = 2 if size == "w" else 4
    return struct.unpack(">H" if size == "w" else ">L", d[a:a + n])[0]


def objects(d, limit=300):
    out, p, seen = [], struct.unpack(">L", d[OBJ_HEAD:OBJ_HEAD + 4])[0], set()
    while p and p not in seen and p + 0x40 <= len(d) and len(out) < limit:
        seen.add(p)
        out.append((p, peek(d, p, "w"), peek(d, p + 0xA, "l"), peek(d, p + 0x32, "w")))
        p = peek(d, p + 0x12, "l")
    return out


def object_count(d, limit=400):
    return len(objects(d, limit))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--disk", required=True, help="a plain .st image (--st-disk)")
    ap.add_argument("--script", required=True)
    ap.add_argument("--every", type=int, default=500)
    ap.add_argument("--at", type=int, action="append", default=[], help="frame whose RAM to dump and read (repeatable)")
    ap.add_argument("--dump-every", type=int, default=0, help="also dump RAM every N frames")
    ap.add_argument("--objects", action="store_true", help="print the hittable-object list of every dumped frame")
    ap.add_argument("--loads", action="store_true", help="list the game's level loads (side 1 tracks 0-13)")
    ap.add_argument("--fdc-speed", type=int, default=8)
    ap.add_argument("extra", nargs="*", help="after --: more x2recomp arguments")
    a = ap.parse_args(argv)
    total = 0
    for line in open(a.script):
        if line.startswith("frames "):
            total = int(line.split()[1])
    frames = list(a.at)
    if a.dump_every and total:
        frames += list(range(a.dump_every, total, a.dump_every))
    frames = sorted(set(f for f in frames if f > 0))
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [a.exe, "--game", "st", "--headless", "--st-disk", a.disk, "--st-fdc-speed", str(a.fdc_speed),
               "--input-script", a.script, "--report-every", str(a.every)]
        if a.loads:
            cmd.append("--st-fdc-log")
        for f in frames:
            cmd += ["--st-ram-dump-at", f"{f}:{os.path.join(tmp, 'r%d.ram' % f)}"]
        out = subprocess.run(cmd + a.extra, capture_output=True, text=True)
        print("frame  pc       " + " ".join("%-8s" % f for f in FIELDS))
        loads = []
        for line in out.stdout.splitlines():
            m = re.match(r"frame (\d+) pc=\$([0-9A-F]+)", line)
            fr = re.match(r"fdc read: frame (\d+) track (\d+) side (\d+) sectors (\d+)-(\d+)", line)
            if m:
                vals = [re.search(r"\b%s=(\d+)" % f, line) for f in FIELDS]
                print("%-6s %-8s %s" % (m.group(1), m.group(2), " ".join("%-8s" % (v.group(1) if v else "?") for v in vals)))
            elif fr:
                if int(fr.group(3)) == 1 and int(fr.group(2)) <= 13:
                    loads.append((int(fr.group(1)), int(fr.group(2))))
            elif not line.startswith("frame") and not line.startswith("fdc read"):
                print("  | " + line)
        if a.loads:
            runs, last = [], None
            for f, t in loads:  # a level load is a run of consecutive side-1 track reads
                if last is not None and f - last[0] < 60:
                    runs[-1][1].append(t)
                else:
                    runs.append((f, [t]))
                last = (f, t)
            print("# level loads (side 1, tracks 0-13): frame of the first read, tracks read")
            for f, ts in runs:
                print("  %6d  tracks %d-%d" % (f, min(ts), max(ts)))
        prev, worst = None, None
        for f in frames:
            path = os.path.join(tmp, "r%d.ram" % f)
            if not os.path.exists(path):
                continue
            d = open(path, "rb").read()
            v = {n: peek(d, ad, sz) for n, ad, sz in VARS}
            rate = ""
            if prev is not None:  # the game's logic step counter ($CDC) per 100 frames: ~33 when it runs every third frame
                r = ((v["tick"] - prev[1]) & 0xFFFF) * 100 // max(1, f - prev[0])
                rate = " steps/100f=%d objs=%d" % (r, object_count(d))
                if f >= 2400 and (worst is None or r < worst[1]):
                    worst = (f, r)
            prev = (f, v["tick"])
            print("frame %d: " % f + " ".join("%s=%X" % (n, v[n]) for n, _, _ in VARS) + rate)
            if a.objects:
                for p, t, h, hp in objects(d):
                    print("    obj %06X type=%04X handler=%08X hp=%04X" % (p, t, h, hp))
        if worst is not None:
            print("# liveness: the slowest stretch of the game's logic step counter ($CDC) after frame 2400 ends at frame %d, "
                  "%d steps per 100 frames (about 33 when it runs every third frame; under 5 is a stall)" % worst)
        if out.returncode:
            print("x2recomp exited with %d\n%s" % (out.returncode, out.stderr), file=sys.stderr)
        return out.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
