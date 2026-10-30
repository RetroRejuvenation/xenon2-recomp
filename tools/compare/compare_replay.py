#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Compare one replay between x2recomp and the x2ref Amiberry reference (input made by to_harness.py).

    python3 -I tools/compare/compare_replay.py REPLAY.txt REF_RUN_DIR OUT_DIR [--shift 2] [--segments A-B,...]

REF_RUN_DIR is a harness run (run.sh) of the converted script: frame_<vsync>.png from its "G<frame> SHOT" lines,
frames.log (last column = game frame, -1 before BASE) and paula.wav. The runtime side is captured into OUT_DIR
(screenshots at every checkpoint +-shift frames, run.wav). For each checkpoint the reference picture is compared
with the runtime pictures at G-shift..G+shift (a logic step lasts 3 VBLs and the two runs drift by 1-2 VBLs;
docs/re-notes/reference-compare.md section 5) at the fixed geometry of section 3. Audio is compared per segment
of game frames (by default the stretches between disc loads) on a common 960-samples-per-frame grid.
With X2REF_WATCH set for the reference run (default WATCH below), the game state at every checkpoint is also
compared with x2recomp's chip RAM, and the first checkpoint where they differ is reported.
Results: OUT_DIR/results.jsonl and a Markdown summary on stdout.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import compare as C  # noqa: E402

PAL_CCK = 3546895  # colour clocks per second


def ours_spf(timing):
    """Runtime WAV samples per emulated frame: 48 kHz output, 313 lines (accurate timing) or 312 (fast) of 227
    colour clocks."""
    return 48000 * (313 if timing == "accurate" else 312) * 227 / PAL_CCK
GEOMETRY = dict(scale=(1, 1), offset=(37, 18))
# X2REF_WATCH=E04,A06,A6C,C92,C94,C9A,C9C,CD0,CC6,CCA,428,3F03C,3F040 (word addresses, logged every vsync)
STATE = [("level $E04", 0xE04, 2), ("complete $A07", 0xA07, 1), ("shop $A6C", 0xA6C, 1), ("score $C92", 0xC92, 4),
         ("cash $C9A", 0xC9A, 4), ("scroll $CD0", 0xCD0, 2), ("lives $CCA", 0xCCA, 2), ("player $428", 0x428, 2),
         ("ship x", 0x3F03C, 2), ("ship y", 0x3F040, 2)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("ref")
    ap.add_argument("out")
    ap.add_argument("--shift", type=int, default=2)
    ap.add_argument("--segments", default="", help="game-frame ranges for audio, e.g. 0-750,780-1090")
    ap.add_argument("--x2recomp", default=os.path.join(ROOT, "build", "x2recomp"))
    ap.add_argument("--assets", default=os.path.join(ROOT, "generated", "assets"))
    ap.add_argument("--timing", default="accurate", choices=["accurate", "fast"], help="x2recomp --timing")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    game_of, vsync_of, watch, ref_ram = {}, {}, [], {}
    for line in open(os.path.join(a.ref, "frames.log")):
        p = line.split()
        if line.startswith("#"):
            watch = [int(w[1:], 16) for w in p if re.fullmatch(r"w[0-9A-F]+", w)]
            continue
        v, g = int(p[0]), int(p[-1])
        game_of[v] = g
        if g >= 0 and g not in vsync_of:
            vsync_of[g] = v
            ref_ram[g] = {addr: int(x, 16) for addr, x in zip(watch, p[3:-1])}
    shots = {}
    for name in os.listdir(a.ref):
        m = re.fullmatch(r"frame_(\d+)\.png", name)
        if m and game_of.get(int(m.group(1)), -1) >= 0:
            shots[game_of[int(m.group(1))]] = os.path.join(a.ref, name)
    frames = int(re.search(r"^frames (\d+)", open(a.replay).read(), re.M).group(1))
    shots = {g: p for g, p in shots.items() if g < frames}
    last = min(max(game_of.values()), frames - 1)

    want = sorted({g + d for g in shots for d in range(-a.shift, a.shift + 1) if g + d >= 0})
    cmd = [a.x2recomp, "--headless", "--no-reglog", "--report-every", "0", "--assets", a.assets, "--timing", a.timing,
           "--input-script", a.replay, "--audio-out", os.path.join(a.out, "run.wav")]
    for f in want:
        cmd += ["--screenshot-at", f"{f}:{os.path.join(a.out, f'f{f:05d}.png')}"]
    for g in shots:
        if g > 0:  # chip RAM after frame g-1 = the state the reference logs at the vsync that starts frame g
            cmd += ["--dump-ram-at", f"{g - 1}:{os.path.join(a.out, f'r{g:05d}.bin')}"]
    subprocess.run(cmd, check=True, capture_output=True)

    def ref_state(g, addr, size):
        words = ref_ram.get(g, {})
        if size == 1:
            w = words.get(addr & ~1)
            return None if w is None else (w >> 8 if addr % 2 == 0 else w & 0xFF)
        if size == 2:
            return words.get(addr)
        hi, lo = words.get(addr), words.get(addr + 2)
        return None if hi is None or lo is None else hi << 16 | lo

    state_rows = []
    for g in sorted(shots):
        path = os.path.join(a.out, f"r{g:05d}.bin")
        if not os.path.exists(path):
            continue
        ram = open(path, "rb").read()
        ours = {n: int.from_bytes(ram[addr:addr + size], "big") for n, addr, size in STATE}
        ref = {n: ref_state(g, addr, size) for n, addr, size in STATE}
        state_rows.append((g, ours, ref))

    res_path = os.path.join(a.out, "results.jsonl")
    open(res_path, "w").close()
    rows = []
    for g in sorted(shots):
        best = None
        for d in range(-a.shift, a.shift + 1):
            ours = os.path.join(a.out, f"f{g + d:05d}.png")
            if not os.path.exists(ours):
                continue
            r = C.frame_compare(shots[g], ours, **GEOMETRY)
            r["game_frame"], r["shift"], r["ref_vsync"] = g, d, vsync_of.get(g)
            if d == 0:
                at0 = r["differing_pct"]
            if best is None or r["differing_pct"] < best["differing_pct"]:
                best = r
        best["differing_pct_shift0"] = at0
        if best["differing_pct"] > 1.0:
            best["diff_png"] = os.path.join(a.out, f"diff_{g:05d}.png")
            C.frame_compare(shots[g], os.path.join(a.out, f"f{g + best['shift']:05d}.png"),
                            diff_out=best["diff_png"], **GEOMETRY)
        best.pop("palette_top", None)
        with open(res_path, "a") as f:
            f.write(json.dumps(best) + "\n")
        rows.append(best)

    print(f"## {os.path.basename(a.replay)}: {len(rows)} checkpoints (game frames 0-{last})\n")
    print("| Game frame | Ref vsync | Best shift | Differing % | Differing % at shift 0 |")
    print("|---|---|---|---|---|")
    for r in rows:
        print(f"| {r['game_frame']} | {r['ref_vsync']} | {r['shift']:+d} | {r['differing_pct']} | "
              f"{r['differing_pct_shift0']} |")
    bad = [r["game_frame"] for r in rows if r["differing_pct"] > 1.0]
    print(f"\ncheckpoints within 1% differing pixels: {len(rows) - len(bad)}/{len(rows)}; over 1%: {bad}\n")

    known = [r for r in state_rows if all(v is not None for v in r[2].values())]
    if known:
        print("| Game frame | " + " | ".join(n for n, _, _ in STATE) + " |")
        print("|---|" + "---|" * len(STATE))
        first = None
        for g, ours, ref in known:
            same = all(ours[n] == ref[n] for n, _, _ in STATE)
            if not same and first is None:
                first = g
            print(f"| {g} | " + " | ".join(str(ours[n]) if ours[n] == ref[n] else f"**{ours[n]} / {ref[n]}**"
                                         for n, _, _ in STATE) + " |")
            with open(res_path, "a") as f:
                f.write(json.dumps({"kind": "state", "game_frame": g, "ours": ours, "ref": ref}) + "\n")
        print(f"\ngame state (ours / ref where different) identical at {sum(1 for r in known if r[1] == r[2])}"
              f"/{len(known)} checkpoints; first difference at game frame {first}\n")

    segs = []
    if a.segments:
        segs = [tuple(map(int, s.split("-"))) for s in a.segments.split(",")]
    else:  # stretches of game time without a reference pause (WAITLOAD), at least 200 frames
        start = None
        for g in range(0, last + 1):
            paused = vsync_of.get(g + 1, 0) - vsync_of.get(g, 0) > 1
            if start is None:
                start = g
            if paused or g == last:
                if g - start >= 200:
                    segs.append((start + 30, g - 5))
                start = None
    if segs:
        print("| Audio segment (game frames) | Envelope corr | Rel. envelope error | Level ours/ref | R/L ref | R/L ours |")
        print("|---|---|---|---|---|---|")
    for g0, g1 in segs:
        v0, v1 = vsync_of[g0], vsync_of[g0] + (g1 - g0)
        rw, ow = os.path.join(a.out, f"ref_{g0}.wav"), os.path.join(a.out, f"ours_{g0}.wav")
        C.cut_wav(os.path.join(a.ref, "paula.wav"), v0, v1, rw, frames_log=os.path.join(a.ref, "frames.log"),
                  spf_out=960)
        C.cut_wav(os.path.join(a.out, "run.wav"), g0, g1, ow, spf_in=ours_spf(a.timing), spf_out=960)
        try:
            r = C.audio_compare(rw, ow)
        except ZeroDivisionError:
            print(f"| {g0}-{g1} | (silent) | | | | |")
            continue
        r["segment"] = [g0, g1]
        with open(res_path, "a") as f:
            f.write(json.dumps(r) + "\n")
        print(f"| {g0}-{g1} | {r['envelope_corr']} | {r['envelope_rel_err']} | {r['level_ours_over_ref']} | "
              f"{r['ref_balance']['right_over_left_total']} | {r['ours_balance']['right_over_left_total']} |")


if __name__ == "__main__":
    main()
