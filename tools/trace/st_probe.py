#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Probe the Atari ST release: run `x2recomp --game st` headless on a script and print the game state over time.

  st_probe.py --exe build/x2recomp --disk game.st --script tests/replays/st/level2.txt [--every 500] [-- EXTRA ARGS]

The columns are the report line's fields (level, energy, ships, credits, score, the PC, unmapped accesses). Extra arguments after
`--` go to x2recomp (--god-mode, --credits N, --st-watch-pc ADDR[:LEN], --st-ram-dump-at F:FILE ...). Reads only the run's own output;
no game bytes leave the process. Used to map the coverage pokes (docs/re-notes/atari-st-hooks.md).
"""
import argparse
import re
import subprocess
import sys

FIELDS = ("level", "energy", "ships", "credits", "score", "unmapped")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--disk", required=True, help="a plain .st image (--st-disk)")
    ap.add_argument("--script", required=True)
    ap.add_argument("--every", type=int, default=500)
    ap.add_argument("--fdc-speed", type=int, default=8)
    ap.add_argument("extra", nargs="*", help="after --: more x2recomp arguments")
    a = ap.parse_args(argv)
    cmd = [a.exe, "--game", "st", "--headless", "--st-disk", a.disk, "--st-fdc-speed", str(a.fdc_speed),
           "--input-script", a.script, "--report-every", str(a.every)] + a.extra
    out = subprocess.run(cmd, capture_output=True, text=True)
    print("frame  pc       " + " ".join("%-8s" % f for f in FIELDS))
    for line in out.stdout.splitlines():
        m = re.match(r"frame (\d+) pc=\$([0-9A-F]+)", line)
        if m:
            vals = [re.search(r"\b%s=(\d+)" % f, line) for f in FIELDS]
            print("%-6s %-8s %s" % (m.group(1), m.group(2), " ".join("%-8s" % (v.group(1) if v else "?") for v in vals)))
        elif not line.startswith("frame"):
            print("  | " + line)
    if out.returncode:
        print("x2recomp exited with %d\n%s" % (out.returncode, out.stderr), file=sys.stderr)
    return out.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
