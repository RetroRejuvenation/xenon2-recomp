#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Run the runtime on a candidate game image + disc 2 and compare it with the recorded replay hashes.

usage: python3 tools/variants/replay_compare.py --exe BUILD/x2recomp --game FILE --disc2 FILE [--repo DIR] [REPLAY ...]

Writes a temporary assets folder (removed afterwards), runs each tests/replays/NAME.txt headless and prints, per
replay, how many frame hashes and audio hashes match tests/replays/NAME.hashes. Prints counts only. Use it to
decide whether a variant's decoded data is equivalent to the canonical import (frames 100% and audio 100%) or to
see exactly what differs (frames match but audio differs: different samples; nothing matches: a different build).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = ["attract", "level1_play", "boss1", "level3", "level5", "shop5", "ending_natural"]


def hashes(text):
    return {(k, int(f)): h for k, f, h in re.findall(r"(frame|audio)hash (\d+) ([0-9a-f]+)", text)}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--disc2", required=True)
    ap.add_argument("--repo", default=os.path.abspath(os.path.join(HERE, "..", "..")))
    ap.add_argument("replays", nargs="*")
    a = ap.parse_args(argv)
    names = a.replays or DEFAULT
    tmp = tempfile.mkdtemp(prefix="x2variant")
    try:
        shutil.copy(a.game, os.path.join(tmp, "game_0400.bin"))
        shutil.copy(a.disc2, os.path.join(tmp, "disc2.bin"))
        for name in names:
            want = hashes(open(os.path.join(a.repo, "tests", "replays", name + ".hashes")).read())
            cmd = [a.exe, "--headless", "--report-every", "0", "--assets", tmp, "--input-script",
                   os.path.join(a.repo, "tests", "replays", name + ".txt"), "--audio-hash"]
            for kind, frame in sorted(want):
                if kind == "frame":
                    cmd += ["--hash-at", str(frame)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print("%-16s runtime failed (exit %d): %s" % (name, r.returncode, (r.stderr or r.stdout).strip()[:100]))
                continue
            got = hashes(r.stdout)
            fr = [k for k in want if k[0] == "frame"]
            au = [k for k in want if k[0] == "audio"]
            print("%-16s frames %d/%d  audio %d/%d" % (
                name, sum(1 for k in fr if got.get(k) == want[k]), len(fr),
                sum(1 for k in au if got.get(k) == want[k]), len(au)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
