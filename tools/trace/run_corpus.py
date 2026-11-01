#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Run the replay corpus (tests/replays/*.txt) with tracing, then merge and report.

    python3 -I tools/trace/run_corpus.py [--game disk|cdtv|st] [--exe build/x2recomp] [--assets generated/assets] [--jobs N]
                                         [--record-hashes] [--hashes-out DIR] [--no-trace] [NAME...]

  - --game cdtv runs the CDTV release's corpus (tests/replays/cdtv/*.txt) on its import (default generated/assets-cdtv) and
    writes the traces to generated/trace-cdtv (the seeds of `tools/recomp/recomp.py --build cdtv`, docs/re-notes/cdtv-recomp.md);
    the Ghidra comparison report is a disk-build tool and is skipped.

  - --game st runs the Atari ST (Kixx) corpus (tests/replays/st/*.txt and tests/recomp/st_scenarios/*.txt) on its import (default
    generated/assets-st) into generated/trace-st: the seeds of `tools/recomp/recomp.py --build st` (docs/re-notes/atari-st-recomp.md).

  - traces go to generated/trace/runs/<name>.json, then tools/trace/merge_traces.py and report.py run
    (generated/trace/{coverage,indirect_targets,report}.json and report.md);
  - --record-hashes rewrites tests/replays/<name>.hashes: frame and cumulative audio hashes every 500 frames and
    at the last frame (the regression manifest checked by the `replay_*` CTests);
  - --hashes-out DIR writes those manifests into DIR instead (tests/replays/retail/<pressing>/ for a retail
    image, docs/re-notes/retail-protection.md).
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def frames_of(script):
    n = None
    for line in open(script):
        m = re.match(r"^\s*frames\s+(\d+)", line)
        if m:
            n = int(m.group(1))
    if n is None:
        sys.exit(f"{script}: no 'frames N' line")
    return n


def hash_frames(n):
    return sorted(set(list(range(500, n, 500)) + [n - 1]))


def script_of(a, name):
    """The script of a replay. With several directories (--game st) a name that is in more than one is run once per directory: the
    first keeps its name, the later ones get the prefix `scn_` (tests/recomp/st_scenarios shares attract, level1_play, game_over with tests/replays/st)."""
    dirs = a.replays.split(os.pathsep)
    if name.startswith("scn_") and len(dirs) > 1:
        p = os.path.join(dirs[-1], name[4:] + ".txt")
        if os.path.exists(p):
            return p
    for d in dirs:
        p = os.path.join(d, name + ".txt")
        if os.path.exists(p):
            return p
    sys.exit(f"no replay {name}.txt in {a.replays}")


def run_one(a, name):
    script = script_of(a, name)
    n = frames_of(script)
    cmd = [a.exe, "--headless", "--report-every", "0", "--assets", a.assets, "--input-script", script]
    if a.game == "st":
        cmd[1:1] = ["--game", "st"]
    if not a.no_trace:
        cmd += ["--trace-out", os.path.join(a.out, "runs", name + ".json"), "--trace-label", name]
    if a.record_hashes:
        cmd += ["--audio-hash"]
        for f in hash_frames(n):
            cmd += ["--hash-at", str(f)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return name, False, r.stdout + r.stderr
    if a.record_hashes:
        lines = [l for l in r.stdout.splitlines() if l.startswith(("framehash ", "audiohash "))]
        # newline: .hashes is committed and .gitattributes keeps it LF, including on Windows.
        with open(os.path.join(a.hashes_out or a.replays, name + ".hashes"), "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    restarts = re.search(r"restarts-via-\$7032=(\d+)", r.stdout)
    return name, True, f"{n} frames, restarts {restarts.group(1) if restarts else '?'}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", choices=["disk", "cdtv", "st"], default="disk")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "x2recomp"))
    ap.add_argument("--assets", default=None)
    ap.add_argument("--replays", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--record-hashes", action="store_true")
    ap.add_argument("--hashes-out", default=None)
    ap.add_argument("--no-trace", action="store_true")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    cdtv = a.game == "cdtv"
    st = a.game == "st"
    suffix = "-cdtv" if cdtv else "-st" if st else ""
    if a.assets is None:
        a.assets = os.path.join(ROOT, "generated", "assets" + suffix)
    if a.replays is None:
        a.replays = (os.path.join(ROOT, "tests", "replays", "cdtv") if cdtv else
                     os.pathsep.join([os.path.join(ROOT, "tests", "replays", "st"), os.path.join(ROOT, "tests", "recomp", "st_scenarios")])
                     if st else os.path.join(ROOT, "tests", "replays"))
    if a.out is None:
        a.out = os.path.join(ROOT, "generated", "trace" + suffix)
    names = a.names
    if not names:
        names, seen = [], set()
        for d in a.replays.split(os.pathsep):
            for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
                if f.endswith(".txt"):
                    names.append(f[:-4] if f[:-4] not in seen else "scn_" + f[:-4])
                    seen.add(f[:-4])
        names.sort()
    os.makedirs(os.path.join(a.out, "runs"), exist_ok=True)
    ok = True
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for name, good, msg in ex.map(lambda n: run_one(a, n), names):
            print(f"{name:14s} {'ok ' if good else 'FAILED'} {msg if good else ''}")
            if not good:
                print(msg)
                ok = False
    if not ok:
        sys.exit(1)
    if a.no_trace:
        return
    here = os.path.dirname(os.path.abspath(__file__))
    subprocess.run([sys.executable, "-I", os.path.join(here, "merge_traces.py"), "--out", a.out,
                    os.path.join(a.out, "runs")], check=True)
    lane = os.path.join(ROOT, "generated", "lane-d2")
    if cdtv or st or not os.path.isdir(lane):  # the comparison with the Ghidra code map is a dev-only, disk-build report
        print(f"coverage merged into {a.out}, Ghidra comparison report skipped")
        return
    subprocess.run([sys.executable, "-I", os.path.join(here, "report.py"), "--trace", a.out,
                    "--lane", lane, "--md", os.path.join(a.out, "report.md")], check=True)
    print(f"report: {os.path.join(a.out, 'report.md')}")


if __name__ == "__main__":
    main()
