# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""List the intervals in which the game's tracker music is silent, for any replay.

The input is a music-only render of a replay: a headless run with the sound effects muted by the Phase 8
per-source gains (`--sfx-volume 0`), so what is left in the WAV is only what the music driver put on Paula.
Pass the plain render of the same run as `--mix` to see what was still audible during each interval.

    R=tests/replays/level1_play.txt
    x2recomp --headless --report-every 0 --assets generated/assets --input-script $R \\
             --sfx-volume 0 --audio-out music.wav --audio-cues cues.txt \\
             --watch-log watch.txt --watch E04,CC6,CCA,9FC,A0A,CDC,414,418,CD0
    x2recomp --headless --report-every 0 --assets generated/assets --input-script $R --audio-out mix.wav

    python3 -I tools/audio/music_dropouts.py music.wav --mix mix.wav --cues cues.txt --watch watch.txt

Sound effects only ever take AUD1 and AUD2 (the command's high byte is the channel: `$414` -> 2, `$418` -> 1,
see docs/re-notes/music-dropouts.md), so the **left** side of the plain render (AUD0 + AUD3) is always pure
music. The tool reports that side separately: a left-side gap is in the tune's own note data, while a gap that
is on the right side only means a sound effect is holding a music voice.

Needs numpy, which is not a project dependency; run with `python3 -I` so a user site-packages numpy is found.
"""

import argparse
import sys
import wave

import numpy as np

FRAME_RATE = 49.92  # emulated PAL frames per second (312 lines)


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 2:
            sys.exit(f"{path}: expected 16-bit stereo")
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        return data.reshape(-1, 2).astype(np.float64), w.getframerate()


def per_frame_rms(samples, frames, side=None):
    """RMS per emulated frame of one side, or of the louder side when `side` is None."""
    edges = (np.arange(frames + 1) * samples.shape[0] // frames).astype(np.int64)
    x = np.abs(samples).max(axis=1) if side is None else np.abs(samples[:, side])
    cum = np.concatenate([[0.0], np.cumsum(x**2)])
    return np.sqrt((cum[edges[1:]] - cum[edges[:-1]]) / np.maximum(np.diff(edges), 1))


def load_cues(path):
    cues = []
    for line in open(path):
        p = line.split()
        if not line.startswith("#") and len(p) >= 5:
            cues.append((int(p[0]), p[2], p[3], p[4]))
    return cues


def load_watch(path):
    names, rows = [], {}
    for line in open(path):
        if line.startswith("#"):
            names = [t.lstrip("w") for t in line.split()[2:]]
            continue
        p = line.split()
        if p:
            rows[int(p[0])] = p[1:]
    return names, rows


def intervals(silent, min_frames):
    out, start = [], None
    for i, s in enumerate(np.append(silent, False)):
        if s and start is None:
            start = i
        elif not s and start is not None:
            if i - start >= min_frames:
                out.append((start, i - 1))
            start = None
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", help="music-only render of the replay (--sfx-volume 0 --audio-out)")
    ap.add_argument("--mix", help="plain render of the same run, for the audible level and the left-side split")
    ap.add_argument("--frames", type=int, default=0, help="emulated frames in the run (default: from the WAV at 49.92 Hz)")
    ap.add_argument("--threshold", type=float, default=40.0, help="int16 RMS below which a frame counts as silent (default 40)")
    ap.add_argument("--min-seconds", type=float, default=0.3, help="shortest interval to report (default 0.3)")
    ap.add_argument("--cues", help="--audio-cues log of the same run")
    ap.add_argument("--watch", help="--watch-log of the same run")
    ap.add_argument("--rms-csv", help="write 'frame,music,music_left,mix' for every frame here")
    ap.add_argument("--quiet", action="store_true", help="one line per interval, no cue or state context")
    args = ap.parse_args()

    music, rate = read_wav(args.wav)
    frames = args.frames or int(round(music.shape[0] / rate * FRAME_RATE))
    rms = per_frame_rms(music, frames)
    mix = left = None
    if args.mix:
        mixed, _ = read_wav(args.mix)
        mix = per_frame_rms(mixed, frames)
        left = per_frame_rms(mixed, frames, 0)

    if args.rms_csv:
        with open(args.rms_csv, "w") as f:
            f.write("frame,music,music_left,mix\n")
            for i in range(frames):
                f.write(f"{i},{rms[i]:.1f},{(left[i] if left is not None else 0):.1f},"
                        f"{(mix[i] if mix is not None else 0):.1f}\n")

    cues = load_cues(args.cues) if args.cues else []
    names, watch = load_watch(args.watch) if args.watch else ([], {})
    min_frames = max(1, int(round(args.min_seconds * FRAME_RATE)))

    print(f"{args.wav}: {frames} frames, music RMS mean {rms.mean():.0f}, silent threshold {args.threshold:.0f}, "
          f"shortest reported {min_frames} frames ({args.min_seconds} s)")
    found = intervals(rms < args.threshold, min_frames)
    total = sum(b - a + 1 for a, b in found)
    print(f"{len(found)} intervals with no music at all, {total} frames ({total / FRAME_RATE:.1f} s)")
    for a, b in found:
        line = f"  frames {a}-{b} ({(b - a + 1) / FRAME_RATE:.2f} s)"
        if mix is not None:
            line += f"  mix rms {mix[a:b + 1].mean():.0f}"
        print(line)
        if args.quiet:
            continue
        if watch:
            st = watch.get(a + 1) or watch.get(a) or []
            print("      state " + " ".join(f"{n}={v}" for n, v in zip(names, st)))
        for c in [c for c in cues if c[0] <= a][-3:]:
            print(f"      cue  f{c[0]} {c[1]} d0={c[2]} caller={c[3]}")
        for c in [c for c in cues if c[0] > a][:2]:
            print(f"      next f{c[0]} {c[1]} d0={c[2]} caller={c[3]}")

    if left is not None:
        lf = intervals(left < args.threshold, min_frames)
        lt = sum(b - a + 1 for a, b in lf)
        print(f"{len(lf)} intervals with the left pair (AUD0+AUD3, never taken by effects) silent, "
              f"{lt} frames ({lt / FRAME_RATE:.1f} s): these are rests in the tune itself")
        for a, b in lf:
            print(f"  frames {a}-{b} ({(b - a + 1) / FRAME_RATE:.2f} s)  music rms {rms[a:b + 1].mean():.0f}"
                  f"  mix rms {mix[a:b + 1].mean():.0f}")


if __name__ == "__main__":
    main()
