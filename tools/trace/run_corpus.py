#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Run the replay corpus (tests/replays/*.txt) with tracing, then merge and report.

    python3 -I tools/trace/run_corpus.py [--exe build/x2recomp] [--assets generated/assets] [--jobs N]
                                         [--record-hashes] [--hashes-out DIR] [--no-trace] [NAME...]

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


def run_one(a, name):
    script = os.path.join(a.replays, name + ".txt")
    n = frames_of(script)
    cmd = [a.exe, "--headless", "--report-every", "0", "--assets", a.assets, "--input-script", script]
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
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "x2recomp"))
    ap.add_argument("--assets", default=os.path.join(ROOT, "generated", "assets"))
    ap.add_argument("--replays", default=os.path.join(ROOT, "tests", "replays"))
    ap.add_argument("--out", default=os.path.join(ROOT, "generated", "trace"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--record-hashes", action="store_true")
    ap.add_argument("--hashes-out", default=None)
    ap.add_argument("--no-trace", action="store_true")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    names = a.names or sorted(f[:-4] for f in os.listdir(a.replays) if f.endswith(".txt"))
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
    if not os.path.isdir(lane):  # the comparison with the Ghidra code map is a dev-only report
        print(f"no {lane}: coverage merged into {a.out}, report skipped")
        return
    subprocess.run([sys.executable, "-I", os.path.join(here, "report.py"), "--trace", a.out,
                    "--lane", lane, "--md", os.path.join(a.out, "report.md")], check=True)
    print(f"report: {os.path.join(a.out, 'report.md')}")


if __name__ == "__main__":
    main()
