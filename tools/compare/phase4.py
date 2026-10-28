#!/usr/bin/env python3
"""Phase 4 reference comparison: patched-Amiberry capture vs the runtime, same input script.

Inputs (all under generated/, gitignored):
  generated/ref/runs/p3/    x2ref capture: frame_NNNNNN.png, frames.log, paula.wav
                            (tools/compare/amiberry-harness/run.sh with generated/ref/scripts/p3.txt)
  generated/ref/ours/full/  runtime capture: fN.png, run.wav, cues.txt
                            (x2recomp --headless --fire 460:5 --fire 950:5 --fire 1200:5 ...)
Writes generated/ref/compare/results.jsonl, diff PNGs, and the generated section of the report.

The two runs are aligned on game-driven events, not on boot time:
  title phase: Amiberry frame = ours + TITLE_OFF (first audible title-music frame: ours 3)
  level phase: Amiberry frame = ours + LEVEL_OFF (first audible level-music frame: ours 1272)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compare as C  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
AMB = os.path.join(ROOT, "generated/ref/runs/p3")
OURS = os.path.join(ROOT, "generated/ref/ours/full")
OUT = os.path.join(ROOT, "generated/ref/compare")
OURS_SPF = 2588796 / 2701  # runtime WAV samples per emulated frame (48 kHz / 50.08 Hz)

TITLE_OFF = 6423
LEVEL_OFF = 7135
GEOM = dict(scale=(1, 1), offset=(37, 18))  # found by the free search on the title frame

CHECKPOINTS = [
    ("title (logo + trainer credit)", 150, TITLE_OFF),
    ("title", 300, TITLE_OFF),
    ("SELECT zoom-in", 470, TITLE_OFF),
    ("SELECT menu", 700, TITLE_OFF),
    ("GET READY PLAYER 1", 1190, LEVEL_OFF),
    ("level 1, +28 frames after music start", 1300, LEVEL_OFF),
    ("level 1, +128", 1400, LEVEL_OFF),
    ("level 1, +328", 1600, LEVEL_OFF),
    ("level 1, +728", 2000, LEVEL_OFF),
    ("level 1, +1128", 2400, LEVEL_OFF),
    ("level 1, +1427", 2699, LEVEL_OFF),
]


def amb_png(f):
    return os.path.join(AMB, f"frame_{f:06d}.png")


def ours_png(f):
    return os.path.join(OURS, f"f{f}.png")


def frames_log():
    rows = {}
    with open(os.path.join(AMB, "frames.log")) as f:
        for line in f:
            if not line.startswith("#"):
                p = line.split()
                rows[int(p[0])] = p
    return rows


def swap_cadence(rows, f0, f1):
    """Runs of constant COP1LC (the game's buffer swap target) in Amiberry frames [f0, f1)."""
    runs, cur, prev = [], 0, None
    for f in range(f0, f1):
        c = rows[f][1]
        if prev is not None and c != prev:
            runs.append(cur)
            cur = 0
        cur += 1
        prev = c
    hist = {}
    for r in runs[1:]:
        hist[r] = hist.get(r, 0) + 1
    return {"kind": "note", "text": f"Amiberry COP1LC swaps in frames {f0}-{f1 - 1}: {len(runs) - 1} complete "
            f"swap intervals, VBLs per swap {dict(sorted(hist.items()))}, "
            f"{round((len(runs) - 1) * 50 / (f1 - f0), 2)} swaps/s"}


def main():
    os.makedirs(OUT, exist_ok=True)
    results = []

    for label, of, off in CHECKPOINTS:
        tries = []
        for d in range(-3, 4):
            if not os.path.exists(amb_png(of + off + d)):
                continue
            r = C.frame_compare(amb_png(of + off + d), ours_png(of), **GEOM)
            tries.append((r["differing_pct"], d, r))
        best_pct, best_d, best = min(tries, key=lambda t: (t[0], abs(t[1])))
        zero = next(r for p, d, r in tries if d == 0)
        diff_png = os.path.join(OUT, f"diff_{of}.png")
        best = C.frame_compare(amb_png(of + off + best_d), ours_png(of), diff_out=diff_png, **GEOM)
        best["label"] = f"{label} (ours {of} / ref {of + off + best_d}, shift {best_d:+d}; at shift 0: " \
                        f"{zero['differing_pct']}%)"
        results.append(best)
        C.print_frame(best)

    for label, o0, o1, off in (("title + SELECT", 0, 778, TITLE_OFF), ("level 1", 1300, 2700, LEVEL_OFF)):
        r = C.cadence([ours_png(f) for f in range(o0, o1)])
        r["label"] = f"ours, {label}"
        results.append(r)
        r = C.cadence([amb_png(f) for f in range(o0 + off, o1 + off)])
        r["label"] = f"Amiberry, {label}"
        results.append(r)

    rows = frames_log()
    results.append(swap_cadence(rows, 1300 + LEVEL_OFF, 2700 + LEVEL_OFF))
    results.append(swap_cadence(rows, 150 + TITLE_OFF, 450 + TITLE_OFF))

    for label, o0, o1, off in (("title music", 0, 450, TITLE_OFF), ("level 1 music", 1260, 2690, LEVEL_OFF)):
        ref_wav = os.path.join(OUT, f"ref_{o0}.wav")
        ours_wav = os.path.join(OUT, f"ours_{o0}.wav")
        C.cut_wav(os.path.join(AMB, "paula.wav"), o0 + off, o1 + off, ref_wav,
                  frames_log=os.path.join(AMB, "frames.log"), spf_out=960)
        C.cut_wav(os.path.join(OURS, "run.wav"), o0, o1, ours_wav, spf_in=OURS_SPF, spf_out=960)
        r = C.audio_compare(ref_wav, ours_wav)
        r["label"] = f"{label} (ours frames {o0}-{o1}, Amiberry {o0 + off}-{o1 + off})"
        results.append(r)
        C.print_audio(r)

    with open(os.path.join(OUT, "results.jsonl"), "w") as f:
        for r in results:
            f.write(json.dumps(r) + "\n")
    with open(os.path.join(OUT, "generated.md"), "w") as f:
        f.write(C.render_report(results, "Generated tables"))
    print(f"wrote {OUT}/results.jsonl and generated.md")


if __name__ == "__main__":
    main()
