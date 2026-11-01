#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Run one Atari ST replay (tests/replays/st/NAME.txt) in x2recomp and in the Hatari harness from the same script and compare them.

usage: st_replay_compare.py NAME --disk KIXX.st --exe build/x2recomp --out DIR [--shift FROM:EXTRA ...] [--scene LABEL:FRAME ...]
                            [--window 6] [--audio]

* The script is converted for Hatari (tools/trace/make_st_replays.py --hatari: pokes included, patch 0002). Hatari's floppy runs
  at real speed and its load times vary from run to run, so the inputs are timed from game events: from each level start's first
  scroll movement ("anchor", found in our run) the harness runs the events relative to the same movement in Hatari. Events before
  the first anchor (the front end, its fire chain, the first pokes) land at VBL t + 436 + the --shift FROM:EXTRA in force
  (default 2200:600) and the fire chain is extended through the load. The matched game events below show how well they line up.
* Game events: the first frame at which a watched game variable takes a value ($E04 level, $E02 loop, $A07 level-complete,
  $A6C shop request, $CC6 energy at its first drop, $CCA ships, ...). The table lists both runs' frame and the difference; the
  variables ($C92 score, $CC6 energy, $CCA ships, $CF0 credits, $C9A cash, $E04 level) are compared at each matched event.
* Scenes: --scene LABEL:FRAME (our frame): our frames FRAME-W..FRAME+W against Hatari's shot at the shifted VBL; the pixel match
  is the best over our window (3-bit gun indices, st_compare.py). --audio also records Hatari's YM output (real time, ~1 min per
  1000 frames) and our WAV and scores them with st_audio_compare.py.
"""
import argparse
import os
import re
import signal
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import st_compare  # noqa: E402

WATCH = [0xE04, 0xE02, 0xA06, 0xA6C, 0xCC6, 0xCCA, 0xCF0, 0xC92, 0xC94, 0xC9A, 0xC9C, 0xCD0, 0x428]
NAMES = {0xE04: "level", 0xE02: "loop", 0xA06: "done", 0xA6C: "shop", 0xCC6: "energy", 0xCCA: "ships", 0xCF0: "credits", 0xC92: "scoreH",
         0xC94: "scoreL", 0xC9A: "cashH", 0xC9C: "cashL", 0xCD0: "pos", 0x428: "player"}


def sh(cmd, env=None, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, env=env, **kw)


def run_ours(a, out, shots, audio):
    os.makedirs(out, exist_ok=True)
    cmd = [a.exe, "--game", "st", "--headless", "--st-disk", a.disk, "--st-fdc-speed", "8", "--report-every", "0",
           "--input-script", os.path.join(a.replays, a.name + ".txt"), "--st-watch", ",".join("%X" % w for w in WATCH),
           "--st-shots-dir", os.path.join(out, "shots"), "--st-shots", shots]
    if audio:
        cmd += ["--audio-out", os.path.join(out, "ours.wav")]
    r = sh(cmd)
    if r.returncode:
        sys.exit(f"x2recomp failed: {r.stdout[-400:]}{r.stderr[-400:]}")
    # watch lines: "watch F $A=$V $B=$V"
    series = {w: [] for w in WATCH}
    for m in re.finditer(r"^watch (\d+)(.*)$", r.stdout, re.M):
        for ad, v in re.findall(r"\$([0-9A-F]+)=\$([0-9A-F]{4})", m.group(2)):
            series[int(ad, 16)].append((int(m.group(1)), int(v, 16)))
    return series, int(re.search(r"st run: (\d+) frames", r.stdout).group(1))


def run_hatari(a, out, shots, script, quit_vbl, audio):
    env = dict(os.environ, DISK=a.disk, X2_GENERATED=a.generated, WATCH=",".join("%X" % w for w in WATCH))
    if audio:
        env.update(WAV="1", FF="off")
    os.makedirs(out, exist_ok=True)
    log = os.path.join(out, "frames.log")
    if os.path.exists(log):
        os.remove(log)
    p = subprocess.Popen(["sh", os.path.join(HERE, "hatari-harness", "run.sh"), out, str(quit_vbl), shots, script], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    t0, done_at = time.time(), None
    limit = quit_vbl / 50 * 1.5 + 120 if audio else 300
    while p.poll() is None and time.time() - t0 < limit:
        # a recording Hatari can hang at exit after the harness closed the WAV: once the last VBL is logged, give it a moment, then end it
        if done_at is None and os.path.exists(log):
            with open(log, "rb") as f:
                f.seek(max(0, os.path.getsize(log) - 200))
                tail = f.read().split(b"\n")
            if len(tail) >= 2 and tail[-2][:12].split(b" ")[0].isdigit() and int(tail[-2].split(b" ")[0]) >= quit_vbl:
                done_at = time.time()
        if done_at is not None and time.time() - done_at > 5:
            break
        time.sleep(0.5)
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
    series = {w: [] for w in WATCH}
    last = {}
    with open(log) as f:
        for line in f:
            q = line.split()
            vbl = int(q[0])
            for tok in q[4:]:
                ad, v = tok.split("=")
                ad, v = int(ad, 16), int(v, 16)
                if last.get(ad) != v:
                    last[ad] = v
                    series[ad].append((vbl, v))
    return series


def value_at(series, frame):
    v = None
    for f, x in series:
        if f > frame:
            break
        v = x
    return v


def level_anchors(series):
    """[(frame, level)]: the first scroll movement ($CD0 leaving $1200) after each start of a level, the moments the harness times the
    scripted inputs from (patch 0002 ANCHOR). A restart of the same level (a death) does not count: only a change of level, or the
    first level, starts a new anchor."""
    out, armed = [], None
    for f, v in series[0xCD0]:
        lv = value_at(series[0xE04], f)
        if v == 0x1200:
            armed = lv
        elif armed is not None and lv == armed and 0 < v < 0x1200:
            if lv is not None and 1 <= lv <= 5 and (not out or out[-1][1] != lv):
                out.append((f, lv))
            armed = None
    return out


def pair_anchors(ours, hat):
    """The Hatari anchor for each of ours (same level, after the one before), or None when Hatari has not got there."""
    out, i = [], 0
    for _, lv in ours:
        while i < len(hat) and hat[i][1] != lv:
            i += 1
        if i < len(hat):
            out.append(hat[i])
            i += 1
        else:
            out.append(None)
    return out


def events(series):
    """(label, frame) of game events: first time each watched variable takes a value after the game proper started ($E04 changes)."""
    ev = []
    seen = set()
    # the first time $E04 is 1 (the game proper; before it EmuTOS's own use of low RAM shows up in the Hatari log)
    start = next((f for f, v in series[0xE04] if v == 1), 0)
    ok = {0xE04: lambda v: 1 <= v <= 5, 0xA6C: lambda v: v >> 8 == 0xFF, 0xA06: lambda v: v == 0xFF, 0xCCA: lambda v: v <= 9,
          0xCF0: lambda v: v <= 9, 0x428: lambda v: v <= 1}
    for ad in (0xE04, 0xA06, 0xA6C, 0xCCA, 0xCF0, 0x428):
        prev = None
        for f, v in series[ad]:
            if f < start - 5:
                prev = v
                continue
            if prev is not None and v != prev and (ad, v) not in seen and ok[ad](v):
                seen.add((ad, v))
                ev.append((f"{NAMES[ad]}={v:X}", f))
            prev = v
    # the game loop starting again after each level load ($E02 going 0 -> 1 after the game proper started)
    n, prev = 0, None
    for f, v in series[0xE02]:
        if f >= start and prev == 0 and v == 1:
            n += 1
            ev.append((f"loop starts #{n}", f))
        prev = v
    # a level ready to play: the scroll position at its start value $1200 (levels 1-5 start there)
    n, prev = 0, None
    for f, v in series[0xCD0]:
        if f >= start and v == 0x1200 and prev is not None and prev != 0x1200:
            n += 1
            ev.append((f"scroll at start #{n}", f))
        prev = v
    # energy: first drop below its full value after the loop flag came up
    for f, v in series[0xCC6]:
        if f >= start and v and v < 0x27 and ("energy-drop", 0) not in seen:
            seen.add(("energy-drop", 0))
            ev.append(("energy first drop", f))
            break
    # score: first non-zero
    for f, v in series[0xC94]:
        if f >= start and v:
            ev.append(("score first non-zero", f))
            break
    return sorted(ev, key=lambda e: e[1])


def snapshot(series, frame):
    d = {}
    for ad in (0xE04, 0xCC6, 0xCCA, 0xCF0, 0xC92, 0xC94, 0xC9A, 0xC9C):
        d[ad] = value_at(series[ad], frame)
    return d


def fmt(d):
    score = (d[0xC92] or 0) << 16 | (d[0xC94] or 0)
    cash = (d[0xC9A] or 0) << 16 | (d[0xC9C] or 0)
    return f"L{d[0xE04]} E{(d[0xCC6] or 0):X} S{d[0xCCA]} C{d[0xCF0]} score {score} cash {cash}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--disk", required=True)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "x2recomp"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--replays", default=os.path.join(ROOT, "tests", "replays", "st"))
    ap.add_argument("--generated", default=os.path.join(ROOT, "generated"))
    ap.add_argument("--shift", action="append", default=[])
    ap.add_argument("--scene", action="append", default=[])
    ap.add_argument("--window", type=int, default=6)
    ap.add_argument("--audio", action="store_true")
    ap.add_argument("--base", type=int, default=436)
    ap.add_argument("--chain-extend", type=int, default=0, help="lengthen the fire chains before the first anchor by this many VBLs")
    ap.add_argument("--series", metavar="FROM:TO:STEP",
                    help="also compare a picture every STEP script frames from FROM to TO (after the first anchor) and summarise when the runs part")
    ap.add_argument("--seed-rng", metavar="FRAME", type=int,
                    help="comparison only: at script frame FRAME set the game's random generator ($28A0/$28A4, the routine at $286C) to its "
                         "load-time seed in both runs, which removes the generator state the two loaders leave behind")
    a = ap.parse_args()
    a.disk = os.path.abspath(a.disk)
    shifts = sorted(tuple(int(x) for x in s.split(":")) for s in a.shift) or [(2200, 600)]
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)

    scenes = []
    for s in a.scene:
        label, fr = s.rsplit(":", 1)
        scenes.append((label, int(fr)))
    series_frames = []
    if a.series:
        s0, s1, s2 = (int(x) for x in a.series.split(":"))
        series_frames = list(range(s0, s1 + 1, s2))
    ours_shots = ",".join([f"{f - a.window}-{f + a.window}" for _, f in scenes] + [f"{f - 3}-{f + 3}" for f in series_frames]) or "0"

    sys.path.insert(0, os.path.join(ROOT, "tools", "trace"))
    import make_st_replays as g
    src = open(os.path.join(a.replays, a.name + ".txt")).read()
    if a.seed_rng is not None:
        lines = src.rstrip("\n").split("\n")
        seed = [f"{a.seed_rng} poke $28A0 $63141592.l", f"{a.seed_rng} poke $28A4 $35358979.l"]
        i = next(k for k, l in enumerate(lines) if l.startswith("frames "))
        src = "\n".join(lines[:i] + ["# comparison only: the generator seeded identically in both runs"] + seed + lines[i:]) + "\n"
        a.replays = out
        open(os.path.join(out, a.name + ".txt"), "w").write(src)
    nframes = g.total_frames(src)

    def at(t):
        extra = 0
        for frm, ex in shifts:
            if t >= frm:
                extra = ex
        return t + a.base + extra

    ours, n = run_ours(a, os.path.join(out, "ours"), ours_shots, a.audio)
    ao = level_anchors(ours)
    first = ao[0][0] if ao else None
    script = os.path.join(out, a.name + ".hatari.txt")
    open(script, "w").write(g.to_hatari(src, a.base, shifts, ao, [f for _, f in scenes] + series_frames, a.chain_extend))
    abs_shots = ",".join(str(at(f)) for _, f in scenes if first is None or f < first) or "1"
    hat = run_hatari(a, os.path.join(out, "hatari"), abs_shots, script, at(nframes) + 1000, a.audio)
    ah = pair_anchors(ao, level_anchors(hat))

    def hat_vbl(f):
        """The Hatari VBL of our script frame f: from the anchor in force at f, or the shifted frame before the first one."""
        k = None
        for i, (fa, _) in enumerate(ao):
            if f >= fa:
                k = i
        if k is None:
            return at(f)
        if ah[k] is None:
            return -1
        return ah[k][0] + (f - ao[k][0])

    print(f"## {a.name}: {n} frames; shifts before the first anchor {shifts or 'none'}")
    print("anchors (first scroll movement of a level start): " + ", ".join(
        f"level {lv}: ours {fo}, Hatari {ah[i][0] if ah[i] else 'not reached'}" for i, (fo, lv) in enumerate(ao)) + "\n")
    eo, eh = dict(), dict()
    for k, f in events(ours):
        eo.setdefault(k, f)
    for k, f in events(hat):
        eh.setdefault(k, f)
    print("| event | ours | Hatari | Hatari - ours | variables ours | variables Hatari |")
    print("|---|---|---|---|---|---|")
    for k in eo:
        if k in eh:
            print(f"| {k} | {eo[k]} | {eh[k]} | {eh[k] - eo[k]} | {fmt(snapshot(ours, eo[k] + 3))} | {fmt(snapshot(hat, eh[k] + 3))} |")
        else:
            print(f"| {k} | {eo[k]} | (not reached) | | {fmt(snapshot(ours, eo[k] + 3))} | |")
    for k in eh:
        if k not in eo:
            print(f"| {k} | (not reached) | {eh[k]} | | | {fmt(snapshot(hat, eh[k] + 3))} |")

    print("\n| scene | our frame | Hatari VBL | best our frame | pixel match | variables ours | variables Hatari |")
    print("|---|---|---|---|---|---|---|")
    hs = st_compare.numbers(os.path.join(out, "hatari"))
    os_ = st_compare.numbers(os.path.join(out, "ours", "shots"))
    for label, f in scenes:
        hv = hat_vbl(f)
        if hv not in hs:
            print(f"| {label} | {f} | {hv} | | no Hatari shot | | |")
            continue
        hi = st_compare.hatari_indices(hs[hv])
        best = (-1.0, None)
        for of in range(f - a.window, f + a.window + 1):
            if of in os_:
                m = float((st_compare.our_indices(os_[of]) == hi).all(axis=2).mean())
                if m > best[0]:
                    best = (m, of)
        print(f"| {label} | {f} | {hv} | {best[1]} | {best[0] * 100:.2f}% | {fmt(snapshot(ours, f))} | {fmt(snapshot(hat, hv))} |")

    if series_frames:
        res = []
        for f in series_frames:
            hv = hat_vbl(f)
            if hv not in hs:
                continue
            hi = st_compare.hatari_indices(hs[hv])
            best = max((float((st_compare.our_indices(os_[of]) == hi).all(axis=2).mean()), of) for of in range(f - 3, f + 4) if of in os_)
            res.append((f, best[0], (value_at(hat[0xCD0], hv) or 0) - (value_at(ours[0xCD0], f) or 0)))
        if res:
            ms = sorted(m for _, m, _ in res)
            part = next((f for f, m, _ in res if m < 0.95), None)
            print(f"\n### pixel match every {a.series.split(':')[2]} frames, {res[0][0]}..{res[-1][0]} ({len(res)} points, best of +-3 frames): "
                  f"median {ms[len(ms) // 2] * 100:.1f}%, >=99%: {sum(m >= 0.99 for m in ms)}, >=95%: {sum(m >= 0.95 for m in ms)}, "
                  f"first point under 95%: {part} ({(part - res[0][0]) if part else '-'} frames after the first)")
            print("series (frame:match%, scroll position Hatari-ours): " + " ".join(f"{f}:{m * 100:.0f}({d:+d})" for f, m, d in res))

    if a.audio:
        r = sh([sys.executable, os.path.join(HERE, "st_audio_compare.py"), os.path.join(out, "ours", "ours.wav"),
                os.path.join(out, "hatari", "ym.wav"), "--local-lag", "1.5"])
        print("\n### audio (st_audio_compare.py, YM output)\n```\n" + (r.stdout + r.stderr).strip() + "\n```")


if __name__ == "__main__":
    main()
