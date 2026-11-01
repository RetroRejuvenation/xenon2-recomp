#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""What each Atari ST replay reaches: levels loaded from the disk, boss objects seen (by hit handler, with the lowest HP), game
state extremes, the shop and level-complete flags, run time and determinism (docs/re-notes/atari-st-replays.md).

usage: st_coverage.py --exe build/x2recomp --disk KIXX.st [--replays tests/replays/st] [--every 100] [--twice] [NAME...]

RAM is dumped every N frames (`--st-ram-dump-at`) and read with the same helpers as st_probe.py, so a short-lived state between two
dumps can be missed: the table says what was *seen*, not that nothing else happened. --twice runs each replay a second time and
compares the hash manifests of both runs (the same frames as the committed .hashes) as the determinism check. Prints a Markdown table.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import st_probe as P  # noqa: E402

BOSSES = {  # (level, hit handler) -> label: the ST's level code is at $4F000 and differs per level, so a handler only counts in its own level
    (1, 0x50396): "L1 nautilus", (2, 0x50A16): "L2 spider", (2, 0x50B00): "L2 parts", (3, 0x4F800): "L3 boss", (3, 0x4FD76): "L3 helpers",
    (4, 0x4FB76): "L4 head", (4, 0x4FBBC): "L4 eyes", (4, 0x504EE): "L4 structure", (5, 0x50360): "L5 guardians", (5, 0xE48): "L5 core target",
}


def run(exe, disk, script, extra):
    t0 = time.time()
    r = subprocess.run([exe, "--game", "st", "--headless", "--st-disk", disk, "--st-fdc-speed", "8", "--report-every", "0",
                        "--input-script", script] + extra, capture_output=True, text=True, timeout=600)
    return r, time.time() - t0


def level_loads(out):
    """Level loads: runs of side-1 reads of tracks 0-13 (a run = reads less than 60 frames apart), as in st_probe.py --loads."""
    last, n = None, 0
    for m in re.finditer(r"fdc read: frame (\d+) track (\d+) side (\d+)", out):
        f, track, side = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if side == 1 and track <= 13:
            if last is None or f - last >= 60:
                n += 1
            last = f
    return n


def main():
    ap = argparse.ArgumentParser()
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    ap.add_argument("--exe", default=os.path.join(root, "build", "x2recomp"))
    ap.add_argument("--disk", required=True)
    ap.add_argument("--replays", default=os.path.join(root, "tests", "replays", "st"))
    ap.add_argument("--every", type=int, default=100)
    ap.add_argument("--twice", action="store_true")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    names = a.names or sorted(f[:-7] for f in os.listdir(a.replays) if f.endswith(".hashes"))
    print("| replay | frames | s | level loads | levels seen | bosses seen (lowest HP) | level done | shop | min energy | min ships | credits | max score | deterministic |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for name in names:
        script = os.path.join(a.replays, name + ".txt")
        n = int(re.search(r"^frames (\d+)", open(script).read(), re.M).group(1))
        frames = list(range(a.every, n, a.every))
        with tempfile.TemporaryDirectory() as tmp:
            extra = ["--st-fdc-log"]
            for f in frames:
                extra += ["--st-ram-dump-at", f"{f}:{tmp}/r{f}"]
            r, secs = run(a.exe, a.disk, script, extra)
            if r.returncode:
                print(f"| {name} | exited {r.returncode} |")
                continue
            levels, done, shop, emin, smin, cred, score, boss = set(), False, False, 99, 99, [], 0, {}
            for f in frames:
                path = f"{tmp}/r{f}"
                if not os.path.exists(path):
                    continue
                d = open(path, "rb").read()
                if f > 2400:
                    levels.add(P.peek(d, 0xE04, "w"))
                    emin = min(emin, P.peek(d, 0xCC6, "w"))
                    smin = min(smin, P.peek(d, 0xCCA, "w"))
                done |= P.peek(d, 0xA07, "b") != 0
                shop |= P.peek(d, 0xA6C, "b") != 0
                cred.append(P.peek(d, 0xCF0, "w"))
                score = max(score, P.peek(d, 0xC92, "l"))
                lv = P.peek(d, 0xE04, "w")
                for _, _, h, hp in P.objects(d):
                    if (lv, h) in BOSSES:
                        boss[(lv, h)] = min(boss.get((lv, h), 0xFFFF), hp)
        det = "-"
        if a.twice:
            runs = []
            x = ["--audio-hash"] + sum((["--hash-at", str(f)] for f in list(range(500, n, 500)) + [n - 1]), [])
            for _ in range(2):
                rr, _ = run(a.exe, a.disk, script, x)
                runs.append(re.findall(r"^(?:frame|audio)hash \d+ [0-9a-f]+$", rr.stdout, re.M))
            want = open(os.path.join(a.replays, name + ".hashes")).read().split("\n")
            det = "yes" if runs[0] == runs[1] == [w for w in want if w] else "NO"
        bosses = ", ".join(f"{BOSSES[k]} ({hp:X})" for k, hp in sorted(boss.items())) or "-"
        lv = ",".join(str(x) for x in sorted(levels) if x) or "-"
        print(f"| {name} | {n} | {secs:.1f} | {level_loads(r.stdout)} | {lv} | {bosses} | {'yes' if done else '-'} | {'yes' if shop else '-'} | "
              f"{emin:X} | {smin} | {min(cred)}-{max(cred)} | {score} | {det} |")


if __name__ == "__main__":
    main()
