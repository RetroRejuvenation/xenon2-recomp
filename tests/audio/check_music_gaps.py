# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Checks the music gaps of a replay's --audio-out WAV (stdlib only; used by music_gaps.cmake).

usage: python3 -I check_music_gaps.py FILE.wav SILENT_FROM:SILENT_TO PLAYING_FROM:PLAYING_TO ...

Sound effects only ever take AUD1 and AUD2 (the command's high byte is the channel), so the WAV's **left**
side (AUD0 + AUD3) is pure music. Each `SILENT` range must have a silent left side and a loud right side: that
is a rest in the tune's own note data, where only the right pair plays. Each `PLAYING` range must have no
left-side gap longer than 1.5 s. Both shapes were confirmed against the Amiberry reference capture; see
docs/re-notes/music-dropouts.md.
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


def main():
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
