#!/usr/bin/env python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Survey CDTV disc image tracks (raw 2352-byte BIN, per-track files) and export short WAV samples.

usage: cdtv_tracks.py DISCDIR [--wav OUTDIR TRACK[,TRACK...] [SECONDS]]
Each audio file begins with a 150-sector (2 s) pregap; programme audio starts at sector 150.
"""
import glob
import os
import struct
import sys
import wave

SECT = 2352
PREGAP = 150


def tracks(d):
    return sorted(glob.glob(os.path.join(d, '*Track *.bin')))


def survey(d):
    print('trk  sectors  programme    peak  lead_s  data')
    for i, f in enumerate(tracks(d), 1):
        n = os.path.getsize(f) // SECT
        if i == 1:
            print('%3d %8d  DATA MODE1' % (i, n))
            continue
        raw = open(f, 'rb').read()
        body = raw[PREGAP * SECT:]
        v = struct.unpack('<%dh' % (len(body) // 2), body)
        peak = max(max(v), -min(v))
        lead = next((k for k, x in enumerate(v) if abs(x) > 64), len(v)) // 2 / 44100.0
        sec = (n - PREGAP) / 75.0
        print('%3d %8d  %2d:%05.2f  %6d  %5.2f' % (i, n, sec // 60, sec % 60, peak, lead))


def wav(d, out, which, secs):
    fs = tracks(d)
    os.makedirs(out, exist_ok=True)
    for t in which:
        raw = open(fs[t - 1], 'rb').read()
        start = (PREGAP + 75 * 20) * SECT  # 20 s into programme (skip silent lead-ins)
        pcm = raw[start:start + secs * 75 * SECT]
        with wave.open(os.path.join(out, 'track%02d_sample.wav' % t), 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(44100)
            w.writeframes(pcm)


if __name__ == '__main__':
    survey(sys.argv[1])
    if len(sys.argv) > 3 and sys.argv[2] == '--wav':
        wav(sys.argv[1], sys.argv[3], [int(x) for x in sys.argv[4].split(',')],
            int(sys.argv[5]) if len(sys.argv) > 5 else 5)
