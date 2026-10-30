#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Convert an x2recomp replay script (tests/replays/*.txt) into an x2ref harness input script, so the patched
Amiberry reference plays the same game inputs and coverage pokes (tools/compare/amiberry-harness/x2ref_harness.inc).

    python3 -I tools/compare/to_harness.py REPLAY.txt OUT.txt [--x2recomp build/x2recomp] [--assets DIR]

How the two timelines are tied together:
  - The boot inputs (intro click, trainer menu, disc swap) are copied from the harness script
    inputs/title_to_level1.txt (its lines before BASE), and game frame 0 is reference vsync BASE = 6423 (the
    title-phase offset of docs/re-notes/reference-compare.md).
  - x2recomp serves disc loads instantly; the reference takes hundreds of frames. The disc loads of the replay
    are found by running it in x2recomp with --report-every 1 (the load_sectors call counter); after each group of
    loads a "WAITLOAD" line holds the reference's game time until its own load is over. With --step-sync it is a
    "WAITSTEP" line instead: the reference then resumes at the start of a logic step, as game frame G = x2recomp's
    first logic-step start (a change of $404, from --watch-log) at least WAIT_Q frames after the load, so both
    runs continue in step (docs/re-notes/reference-compare.md section 10).
  - Runtime input events become state changes (held directions and fire are OR-ed per frame, as x2recomp does);
    poke/pokeobj become POKE/POKEOBJ lines that the harness applies at the start of the same game frames.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
BASE = 6423       # reference vsync of game frame 0 (title-phase offset)
LOAD_GAP = 60     # runtime loads closer than this belong to one group
WAIT_Q = 25       # the reference resumes after this many vsyncs without a load
DIRS = {"U": ("JOY2_UP", 1), "D": ("JOY2_DOWN", 2), "L": ("JOY2_LEFT", 4), "R": ("JOY2_RIGHT", 8)}


def num(t):
    t = t.strip()
    return int(t[1:], 16) if t.startswith("$") else int(t, 0)


def parse_value(v):
    size = 2
    if len(v) > 2 and v[-2] == ".":
        size = {"b": 1, "w": 2, "l": 4}[v[-1]]
        v = v[:-2]
    return num(v), size


def parse_script(path):
    """Mirror of load_input_script in runtime/main.cpp."""
    held, keys, pokes, frames, last = [], [], [], 0, 0
    for raw in open(path):
        line = raw.split("#", 1)[0].split()
        if not line:
            continue
        if line[0] == "frames":
            frames = int(line[1], 0)
            continue
        f = int(line[0][1:], 0) + last if line[0][0] == "+" else int(line[0], 0)
        last = f
        cmd, tok = line[1], line[2:]
        every, until = 0, f
        if "every" in tok:
            i = tok.index("every")
            every, until = int(tok[i + 1], 0), int(tok[i + 3], 0)
            tok = tok[:i]
        tok += [""] * (3 - len(tok))
        a, b, c = tok[:3]
        starts = range(f, until + 1, every) if every else [f]
        for g in starts:
            if cmd == "fire":
                held.append((g, int(a or "3", 0), 16))
            elif cmd == "key":
                keys.append((g, int(a, 16)))
            elif cmd == "joy":
                bits = 0
                for ch in a:
                    bits |= 16 if ch == "F" else DIRS[ch][1]
                held.append((g, int(b or "1", 0), bits))
            elif cmd == "poke":
                v, size = parse_value(b)
                pokes.append(("POKE", g, int(c or "1", 0), f"{num(a):X} {v:X} {size}"))
            elif cmd == "pokeobj":
                v, size = parse_value(c)
                n = int(tok[3], 0) if len(tok) > 3 and tok[3] else 1
                pokes.append(("POKEOBJ", g, n, f"{num(a):X} {num(b):X} {v:X} {size}"))
            else:
                sys.exit(f"{path}: unknown command {cmd}")
    return held, keys, pokes, frames


def runtime_loads(x2recomp, assets, script, frames):
    """Frames in which x2recomp served a disc load (load_sectors calls)."""
    out = subprocess.run([x2recomp, "--headless", "--no-reglog", "--report-every", "1", "--assets", assets,
                          "--input-script", script, "--frames", str(frames)],
                         capture_output=True, text=True, check=True).stdout
    loads, prev = [], 0
    for m in re.finditer(r"\[frames (\d+)-\d+\].* disk=(\d+)/", out):
        f, calls = int(m.group(1)), int(m.group(2))
        if calls != prev:
            loads.append(f)
        prev = calls
    return loads


def runtime_step_starts(x2recomp, assets, script, frames, timing):
    """Frames at whose start x2recomp's logic step has just begun ($404 changed during the previous frame)."""
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "w.log")
        subprocess.run([x2recomp, "--headless", "--no-reglog", "--report-every", "0", "--assets", assets, "--timing",
                        timing, "--input-script", script, "--frames", str(frames), "--watch", "404",
                        "--watch-log", log], capture_output=True, check=True)
        rows = [l.split() for l in open(log) if not l.startswith("#")]
    return [int(b[0]) for a, b in zip(rows, rows[1:]) if a[1] != b[1]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("out")
    ap.add_argument("--x2recomp", default=os.path.join(ROOT, "build", "x2recomp"))
    ap.add_argument("--assets", default=os.path.join(ROOT, "generated", "assets"))
    ap.add_argument("--shots", default="", help="game frames to save as PNG, e.g. 100,200,300 or 0-6000/100")
    ap.add_argument("--boot", default=os.path.join(HERE, "amiberry-harness", "inputs", "title_to_level1.txt"))
    ap.add_argument("--step-sync", action="store_true", help="WAITSTEP: resume each load at a logic-step start")
    ap.add_argument("--timing", default="accurate", choices=["accurate", "fast"], help="x2recomp --timing (step-sync)")
    a = ap.parse_args()

    held, keys, pokes, frames = parse_script(a.replay)
    loads = runtime_loads(a.x2recomp, a.assets, a.replay, frames)
    groups = []
    for f in loads:
        if groups and f - groups[-1][-1] <= LOAD_GAP:
            groups[-1].append(f)
        else:
            groups.append([f])

    ev = []  # (game frame, order, text)
    state = 0
    for f in range(frames + 1):
        cur = 0
        for g, n, bits in held:
            if g <= f < g + n:
                cur |= bits
        for name, bit in [("JOY2_FIRE", 16)] + [DIRS[d] for d in "UDLR"]:
            if (cur ^ state) & bit:
                ev.append((f, 1, f"G{f} {name} {1 if cur & bit else 0}"))
        state = cur
    for g, code in keys:
        ev.append((g, 1, f"G{g} KEY{code:02X} 1"))
        ev.append((g + 2, 1, f"G{g + 2} KEY{code:02X} 0"))
    for kind, g, n, args in pokes:
        ev.append((g, 2, f"G{g} {kind} {args} {n}"))
    for spec in filter(None, a.shots.split(",")):
        m = re.fullmatch(r"(\d+)(?:-(\d+)(?:/(\d+))?)?", spec)
        lo, hi, step = int(m.group(1)), int(m.group(2) or m.group(1)), int(m.group(3) or 1)
        for g in range(lo, hi + 1, step):
            ev.append((g, 3, f"G{g} SHOT"))
    starts = runtime_step_starts(a.x2recomp, a.assets, a.replay, frames, a.timing) if a.step_sync else []
    for grp in groups:
        g = grp[-1] + 1
        if a.step_sync:
            gb = next(f for f in starts if f >= g + WAIT_Q)
            ev.append((g, 0, f"G{g} WAITSTEP {WAIT_Q} {gb}"))
        else:
            ev.append((g, 0, f"G{g} WAITLOAD {WAIT_Q}"))
    ev.sort(key=lambda e: (e[0], e[1]))

    with open(a.out, "w") as out:
        out.write(f"# {os.path.basename(a.replay)} for the x2ref harness, generated by tools/compare/to_harness.py\n")
        out.write(f"# runtime disc loads at frames {loads}\n")
        for line in open(a.boot):
            p = line.split()
            if p and not p[0].startswith("#") and int(p[0]) < BASE:
                out.write(line)
        out.write(f"BASE {BASE}\n")
        for _, _, text in ev:
            out.write(text + "\n")
    print(f"{a.out}: {len(ev)} game events, {len(groups)} load groups {[g[-1] for g in groups]}, {frames} frames")


if __name__ == "__main__":
    main()
