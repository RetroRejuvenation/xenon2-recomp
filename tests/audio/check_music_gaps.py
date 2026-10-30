# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Checks the music gaps of a replay's --audio-out WAV (stdlib only; used by music_gaps.cmake).

usage: python3 -I check_music_gaps.py FILE.wav silent=FROM:TO playing=FROM:TO ...
       python3 -I check_music_gaps.py --own WATCHLOG FROM:TO

Sound effects only ever take AUD1 and AUD2 (the command's high byte is the channel), so the WAV's **left**
side (AUD0 + AUD3) is pure music. Each `silent` range must have a silent left side and a loud right side: that
is a rest in the tune's own note data, where only the right pair plays. Each `playing` range must have no
left-side gap longer than 1.5 s.

`--own WATCHLOG FROM:TO` checks the channel-allocation rule over a run with fire held: the watch log's first
four words are the driver's `$B(a4)` flags (`--watch 1B76C,1B778,1B784,1B790`), AUD0 and AUD3 must never be
owned by an effect, and AUD2 must be owned for most of the range.

All of it was confirmed against the Amiberry reference captures; see docs/re-notes/music-dropouts.md.
"""
import array
import sys
import wave

SAMPLES_PER_FRAME = 48000 * 71051 / 3546895  # 313 lines x 227 colour clocks per frame (accurate timing)
SILENT_RMS = 40      # int16: below this a frame has no audible music
PLAYING_RMS = 300
MAX_GAP_FRAMES = 75  # 1.5 s at 49.92 Hz


def frame_rms(data, side, first, last):
    a, b = int(first * SAMPLES_PER_FRAME), int((last + 1) * SAMPLES_PER_FRAME)
    chunk = data[a * 2 + side:b * 2:2]
    return (sum(s * s for s in chunk) / max(1, len(chunk))) ** 0.5


def check_ownership(path, first, last):
    """AUD0/AUD3 are never taken by an effect, and AUD2 is held for most of a range with fire held."""
    owned = [0, 0, 0, 0]
    total = 0
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.split()
        if not p:
            continue
        frame = int(p[0]) - 1
        if not first <= frame <= last:
            continue
        total += 1
        for ch in range(4):
            owned[ch] += (int(p[1 + ch], 16) & 0xFF) != 0
    if not total:
        print(f"FAIL: {path} has no rows in {first}-{last}")
        return 1
    share = [100.0 * n / total for n in owned]
    print("frames %d-%d: effect-owned share AUD0 %.1f%%, AUD1 %.1f%%, AUD2 %.1f%%, AUD3 %.1f%%"
          % (first, last, *share))
    ok = True
    for ch in (0, 3):
        if owned[ch]:
            print(f"FAIL: AUD{ch} was taken by a sound effect on {owned[ch]} frames; effects only ever use "
                  f"AUD1 and AUD2 (the command's high byte is the channel)")
            ok = False
    if share[2] < 60.0:
        print(f"FAIL: AUD2 held for only {share[2]:.1f}% of a range with fire held; the fire effect is 21 "
              f"ticks long and is re-issued about every 24 frames, so it should be most of it")
        ok = False
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    if sys.argv[1] == "--own":
        first, last = (int(x) for x in sys.argv[3].split(":"))
        return check_ownership(sys.argv[2], first, last)
    w = wave.open(sys.argv[1])
    if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (48000, 2, 2):
        print("FAIL: format", w.getparams())
        return 1
    data = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        data.byteswap()
    frames = int(len(data) / 2 / SAMPLES_PER_FRAME)
    print(f"{frames} emulated frames")
    ok = True
    for arg in sys.argv[2:]:
        kind, rng = arg.split("=")
        first, last = (int(x) for x in rng.split(":"))
        if last >= frames:
            print(f"FAIL: {arg}: the run is only {frames} frames")
            return 1
        left = frame_rms(data, 0, first, last)
        right = frame_rms(data, 1, first, last)
        if kind == "silent":
            print(f"rest {first}-{last}: left (AUD0+AUD3) rms {left:.0f}, right rms {right:.0f}")
            if left >= SILENT_RMS:
                print(f"FAIL: the tune's rest at {first}-{last} is not silent on the left pair")
                ok = False
            if right < PLAYING_RMS:
                print(f"FAIL: nothing on the right pair during the rest at {first}-{last}")
                ok = False
        else:
            gap = worst = 0
            for f in range(first, last + 1):
                gap = gap + 1 if frame_rms(data, 0, f, f) < SILENT_RMS else 0
                worst = max(worst, gap)
            print(f"playing {first}-{last}: left rms {left:.0f}, longest left-side gap {worst} frames")
            if left < PLAYING_RMS:
                print(f"FAIL: no music on the left pair over {first}-{last}")
                ok = False
            if worst > MAX_GAP_FRAMES:
                print(f"FAIL: a {worst}-frame left-side gap in {first}-{last}, over the {MAX_GAP_FRAMES} allowed")
                ok = False
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
