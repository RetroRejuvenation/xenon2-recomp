#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Translated-code coverage of a replay corpus on a recompiled build (docs/re-notes/cdtv-recomp.md, trace-coverage.md).

    python3 -I tools/recomp/coverage_report.py [--game disk|cdtv] --exe build-recomp/x2recomp --assets DIR
                                               [--replays DIR] [--trace DIR] [--recomp DIR] [--jobs N] [NAME...]

Runs each replay with --recomp-stats and prints a markdown table: instructions executed, share translated, instructions that fell
back to the interpreter and the most-interpreted PCs; then, from the merged traces (tools/trace/run_corpus.py) and the generator's
manifest, how many distinct instructions each module executed in the corpus against how many the translation covers.
Everything it needs is generated locally from your own game data; nothing it prints contains game bytes.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def frames_of(script):
    n = 0
    for line in open(script):
        m = re.match(r"^\s*frames\s+(\d+)", line)
        if m:
            n = int(m.group(1))
    return n


def run_one(a, name):
    script = os.path.join(a.replays, name + ".txt")
    r = subprocess.run([a.exe, "--headless", "--report-every", "0", "--assets", a.assets, "--input-script", script,
                        "--recomp-stats", "6"], capture_output=True, text=True)
    out = r.stdout + r.stderr
    m = re.search(r"recomp: (\d+) instructions, (\d+) translated \(([0-9.]+)%\), (\d+) interpreted", out)
    top = re.findall(r"interpreted ([0-9A-F]{6}) x(\d+)", out)
    return name, r.returncode, (m.groups() if m else None), top[:3], frames_of(script)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", choices=["disk", "cdtv"], default="cdtv")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--assets", required=True)
    ap.add_argument("--replays", default=None)
    ap.add_argument("--trace", default=None)
    ap.add_argument("--recomp", default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    cdtv = a.game == "cdtv"
    a.replays = a.replays or (os.path.join(ROOT, "tests", "replays", "cdtv") if cdtv else os.path.join(ROOT, "tests", "replays"))
    a.trace = a.trace or os.path.join(ROOT, "generated", "trace-cdtv" if cdtv else "trace")
    a.recomp = a.recomp or os.path.join(ROOT, "generated", "recomp-cdtv" if cdtv else "recomp")
    names = a.names or sorted(f[:-4] for f in os.listdir(a.replays) if f.endswith(".txt"))
    print("| replay | frames | instructions | translated | interpreted | hottest interpreted PCs |")
    print("|---|---:|---:|---:|---:|---|")
    tot_i = tot_t = 0
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for name, rc, st, top, frames in ex.map(lambda n: run_one(a, n), names):
            if rc != 0 or not st:
                print("| %s | %d | run failed (exit %d) | | | |" % (name, frames, rc))
                bad += 1
                continue
            total, trans, pct, interp = int(st[0]), int(st[1]), st[2], int(st[3])
            tot_i += total
            tot_t += trans
            print("| %s | %d | %d | %s%% | %d | %s |" % (name, frames, total, pct, interp,
                                                         ", ".join("$%s x%s" % t for t in top) or "-"))
    if tot_i:
        print("| **all** | | %d | **%.2f%%** | %d | |" % (tot_i, 100.0 * tot_t / tot_i, tot_i - tot_t))

    cov_path = os.path.join(a.trace, "coverage.json")
    man_path = os.path.join(a.recomp, "manifest.json")
    if os.path.exists(cov_path) and os.path.exists(man_path):
        cov = json.load(open(cov_path))
        man = json.load(open(man_path))
        print("\n| module | distinct instructions executed in the corpus | instructions translated | translated functions |")
        print("|---|---:|---:|---:|")
        funcs = {}
        for f in man["functions"]:
            funcs[f["module"]] = funcs.get(f["module"], 0) + 1
        for mod, m in cov["modules"].items():
            if mod.startswith("overlay-"):
                continue
            print("| %s | %d | %d | %d |" % (mod, m["executed_insns"], man["instructions"].get(mod, 0), funcs.get(mod, 0)))
    if bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
