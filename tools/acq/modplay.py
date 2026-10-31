#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# A small ProTracker ("M.K.") module replayer for listening to the tunes found in other ports (docs/re-notes/
# acquisitions-flux-acorn-st.md). It implements the common effects (arpeggio, slides, tone portamento, vibrato, sample
# offset, volume slide, set volume, pattern break, speed/tempo, fine slides) and renders stereo with Amiga-style
# panning. It is approximate by design (finetune is ignored) and position jumps are ignored so a song plays through
# once. Not a substitute for libopenmpt.
#
#   python3 tools/acq/modplay.py SONG.mod OUT.wav
import struct
import sys
import wave

import numpy as np

PAL_CLOCK = 3546895.0
PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320, 302, 285, 269, 254,
           240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]


def load(path):
    with open(path, "rb") as fh:
        b = fh.read()
    if b[1080:1084] != b"M.K.":
        raise ValueError("not a 4-channel M.K. module")
    samples = []
    for i in range(31):
        s = b[20 + 30 * i:50 + 30 * i]
        samples.append(dict(
            length=struct.unpack(">H", s[22:24])[0] * 2, volume=s[25],
            loop_start=struct.unpack(">H", s[26:28])[0] * 2, loop_length=struct.unpack(">H", s[28:30])[0] * 2))
    song_length = b[950]
    order = list(b[952:952 + song_length])
    pattern_count = max(b[952:1080]) + 1
    patterns = [b[1084 + 1024 * p:1084 + 1024 * (p + 1)] for p in range(pattern_count)]
    pos = 1084 + 1024 * pattern_count
    for s in samples:
        raw = b[pos:pos + s["length"]]
        pos += s["length"]
        s["data"] = np.frombuffer(raw, dtype=np.int8).astype(np.float32) / 128.0 if raw else np.zeros(0, np.float32)
    return samples, order, patterns


def _new_channel():
    return dict(period=0, sample=None, pos=0.0, volume=0, number=0, porta_target=0, porta_speed=0,
                vib_pos=0, vib_speed=0, vib_depth=0, effect=0, param=0)


def render(path, rate=44100, max_seconds=400):
    samples, order, patterns = load(path)
    speed, tempo = 6, 125
    channels = [_new_channel() for _ in range(4)]
    out = [[], [], [], []]
    order_pos, row, break_row = 0, 0, None
    total = 0
    seen = set()

    def step_for(period):
        return PAL_CLOCK / (2 * period) / rate if period > 0 else 0.0

    while order_pos < len(order) and total < max_seconds * rate:
        if (order_pos, row) in seen:
            break
        seen.add((order_pos, row))
        pattern = patterns[order[order_pos]]
        for c in range(4):
            e = pattern[row * 16 + c * 4:row * 16 + c * 4 + 4]
            sample_no = (e[0] & 0xF0) | (e[2] >> 4)
            period = ((e[0] & 0xF) << 8) | e[1]
            effect, param = e[2] & 0xF, e[3]
            ch = channels[c]
            ch["effect"], ch["param"] = effect, param
            if sample_no:
                ch["number"] = sample_no - 1
                ch["volume"] = samples[sample_no - 1]["volume"]
            if period:
                if effect == 3:
                    ch["porta_target"] = period
                else:
                    ch["period"], ch["pos"], ch["sample"], ch["vib_pos"] = period, 0.0, ch["number"], 0
                    if effect == 9:
                        ch["pos"] = param * 256.0
            if effect == 0xC:
                ch["volume"] = min(64, param)
            elif effect == 0xF:
                if 0 < param < 32:
                    speed = param
                elif param >= 32:
                    tempo = param
            elif effect == 0xD:
                break_row = (param >> 4) * 10 + (param & 15)
            elif effect == 0xE:
                x, y = param >> 4, param & 15
                if x == 1:
                    ch["period"] = max(113, ch["period"] - y)
                elif x == 2:
                    ch["period"] = min(856, ch["period"] + y)
                elif x == 0xA:
                    ch["volume"] = min(64, ch["volume"] + y)
                elif x == 0xB:
                    ch["volume"] = max(0, ch["volume"] - y)
            elif effect == 3 and param:
                ch["porta_speed"] = param
            elif effect == 4:
                if param >> 4:
                    ch["vib_speed"] = param >> 4
                if param & 15:
                    ch["vib_depth"] = param & 15
        for tick in range(speed):
            frames = int(rate * 2.5 / tempo)
            for c in range(4):
                ch = channels[c]
                effect, param = ch["effect"], ch["param"]
                if tick > 0:
                    if effect == 1:
                        ch["period"] = max(113, ch["period"] - param)
                    elif effect == 2:
                        ch["period"] = min(856, ch["period"] + param)
                    elif effect == 3 and ch["porta_target"]:
                        if ch["period"] > ch["porta_target"]:
                            ch["period"] = max(ch["porta_target"], ch["period"] - ch["porta_speed"])
                        elif ch["period"] < ch["porta_target"]:
                            ch["period"] = min(ch["porta_target"], ch["period"] + ch["porta_speed"])
                    elif effect in (5, 6, 0xA):
                        up, down = param >> 4, param & 15
                        ch["volume"] = max(0, min(64, ch["volume"] + (up if up else -down)))
                    elif effect == 4:
                        ch["vib_pos"] = (ch["vib_pos"] + ch["vib_speed"]) & 63
                period = ch["period"]
                if effect == 4:
                    period += int(np.sin(ch["vib_pos"] / 64 * 2 * np.pi) * ch["vib_depth"] * 2)
                if effect == 0 and param and period:
                    n = tick % 3
                    if n:
                        k = PERIODS.index(min(PERIODS, key=lambda x: abs(x - ch["period"]))) + (param >> 4 if n == 1 else param & 15)
                        period = PERIODS[min(35, k)]
                buf = np.zeros(frames, np.float32)
                if ch["sample"] is not None and period > 0 and ch["volume"] > 0:
                    s = samples[ch["sample"]]
                    data = s["data"]
                    if len(data) > 2:
                        step = step_for(period)
                        idx = ch["pos"] + step * np.arange(frames)
                        if s["loop_length"] > 2:
                            end = s["loop_start"] + s["loop_length"]
                            wrap = idx >= end
                            idx[wrap] = s["loop_start"] + ((idx[wrap] - s["loop_start"]) % s["loop_length"])
                            buf = data[np.minimum(idx.astype(np.int64), len(data) - 1)]
                            ch["pos"] = float(idx[-1] + step)
                            if ch["pos"] >= end:
                                ch["pos"] = s["loop_start"] + ((ch["pos"] - s["loop_start"]) % s["loop_length"])
                        else:
                            buf = np.where(idx < len(data), data[np.minimum(idx.astype(np.int64), len(data) - 1)], 0)
                            ch["pos"] += step * frames
                            if ch["pos"] >= len(data):
                                ch["sample"] = None
                        buf = buf * (ch["volume"] / 64.0)
                out[c].append(buf)
            total += frames
        row += 1
        if break_row is not None:
            order_pos += 1
            row, break_row = break_row, None
        elif row >= 64:
            order_pos += 1
            row = 0
    chans = [np.concatenate(x) if x else np.zeros(1, np.float32) for x in out]
    n = min(len(c) for c in chans)
    left = (chans[0] + chans[3])[:n] * 0.5 + 0.2 * (chans[1] + chans[2])[:n]
    right = (chans[1] + chans[2])[:n] * 0.5 + 0.2 * (chans[0] + chans[3])[:n]
    stereo = np.stack([left, right], 1)
    peak = np.abs(stereo).max() or 1
    return (stereo / peak * 0.85 * 32767).astype(np.int16), rate


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    pcm, rate = render(argv[1])
    with wave.open(argv[2], "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())
    print("%s: %.1f s" % (argv[2], len(pcm) / rate))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
