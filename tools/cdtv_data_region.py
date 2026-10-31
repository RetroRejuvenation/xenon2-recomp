#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Structure report of the CDTV data track's region after the ISO9660 volume (docs/re-notes/cdtv-data-region.md).

usage: python3 -I tools/cdtv_data_region.py TRACK01.BIN [--wav-dir DIR [--rate HZ]]

TRACK01.BIN is the raw 2352-byte-per-sector MODE1 data track of your own disc image. The report prints sector
modes, EDC validity, the ISO volume size, the extent of the labelled filler, the silent runs and per-segment
statistics. With --wav-dir it also renders the content part as one mono 8-bit WAV file into DIR so it can be
listened to, at 22050 Hz (the rate confirmed by listening) or at the --rate you give. The data is mono: a stereo
reading plays at double speed and is wrong. Nothing is written unless --wav-dir is given, and the output is for your
own use only: it is derived from the disc and must not be committed or shared.
"""
import os
import struct
import sys
import wave

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


def _edc_table():
    tab = []
    for i in range(256):
        e = i
        for _ in range(8):
            e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
        tab.append(e)
    return tab


_TAB = _edc_table()


def edc(b):
    c = 0
    for x in b:
        c = (c >> 8) ^ _TAB[(c ^ x) & 0xFF]
    return c


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    wav_dir = argv[argv.index("--wav-dir") + 1] if "--wav-dir" in argv else None
    rate = int(argv[argv.index("--rate") + 1]) if "--rate" in argv else 22050
    size = os.path.getsize(path)
    if size % 2352:
        print("not a raw 2352-byte-sector image")
        return 2
    n = size // 2352
    f = open(path, "rb")
    modes, bad_edc = {}, 0
    user = []
    for i in range(n):
        s = f.read(2352)
        key = (s[:12] == SYNC, s[15])
        modes[key] = modes.get(key, 0) + 1
        if struct.unpack("<I", s[2064:2068])[0] != edc(s[:2064]):
            bad_edc += 1
        user.append(s[16:2064])
    print("sectors", n, "modes (sync, mode):", modes, "EDC mismatches:", bad_edc)
    pvd = user[16]
    vol = struct.unpack("<I", pvd[80:84])[0] if pvd[1:6] == b"CD001" else 0
    print("ISO volume space (sectors):", vol, "-> region after it:", n - vol, "sectors")

    # labelled filler: "NNNN\0" at the start of the sector, then a repeated 0x55/0xAA pattern
    lab = [i for i in range(vol, n) if user[i][:4].isdigit() and user[i][4] == 0 and int(user[i][:4]) == i]
    if lab:
        print("labelled filler sectors: %d..%d (%d), label == sector number" % (lab[0], lab[-1], len(lab)))
    first_fill = lab[0] if lab else n
    runs, start = [], None
    for i in range(vol, n):
        z = not any(user[i])
        if z and start is None:
            start = i
        if not z and start is not None:
            runs.append((start, i - start))
            start = None
    if start is not None:
        runs.append((start, n - start))
    print("all-zero runs (first sector, length):", [r for r in runs if r[0] < first_fill])
    content = b"".join(user[vol:first_fill])
    sig = [x - 256 if x > 127 else x for x in content]
    if sig:
        mean = sum(sig) / len(sig)
        rms = (sum(x * x for x in sig) / len(sig)) ** 0.5
        clip = sum(1 for x in sig if x in (-128, 127))
        print("content part: sectors %d..%d, %d bytes, mean %.2f, rms %.1f, clipped %d" % (vol, first_fill - 1, len(content), mean, rms, clip))
    if wav_dir:
        os.makedirs(wav_dir, exist_ok=True)
        u = bytes((x + 128) & 0xFF for x in sig)
        with wave.open(os.path.join(wav_dir, "content_mono_%d.wav" % rate), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(1)
            w.setframerate(rate)
            w.writeframes(u)
        print("wrote content_mono_%d.wav to" % rate, wav_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
