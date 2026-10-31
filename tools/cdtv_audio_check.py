#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Checks the CDTV release's Paula effects and speech against the files they come from (docs/re-notes/cdtv-audio.md).

Reads the user's own import (the `cd0/` folder of the CDTV assets), the per-frame register logs of a run
(`x2recomp --game cdtv ... --dump-dir DIR`, written to DIR/regs/) and the audio render of the same run
(`--audio-out FILE`). Prints nothing from the files: counts, periods, volumes and correlation figures only.

  python3 -I tools/cdtv_audio_check.py events DIR/regs [--from F] [--to G]
      every channel start (DMA enable): frame, channel, sample pointer, length, period, volume
  python3 -I tools/cdtv_audio_check.py speech DIR/regs RENDER.wav CD0_DIR --at FRAME:FILE [--at ...]
      for each speech start at FRAME, correlates the render with FILE (8-bit samples at the played period) and prints the
      normalised correlation and the gain per output channel; the gain is output units per sample unit: about 94.5 for a
      channel at volume 64, in proportion to the volume (pan is the volume pair of channels 0/1 and 2/3)

Needs numpy and scipy for the `speech` command.
"""
import argparse
import glob
import os
import re
import sys
import wave

PAULA_CLOCK = 3546895.0 # PAL colour clock rate; Paula period p plays p / clock seconds per sample


def events(regs_dir, first=0, last=1 << 30):
    cur = {}
    out = []
    for path in sorted(glob.glob(os.path.join(regs_dir, "frame_*.log"))):
        for line in open(path):
            if line.startswith("#"):
                continue
            p = line.split()
            if len(p) < 7:
                continue
            frame, _line, _hpos, _src, _pc, reg, val = p[:7]
            frame = int(frame)
            if frame > last:
                return out
            m = re.match(r"AUD(\d)(LCH|LCL|LEN|PER|VOL)$", reg)
            if m:
                c, k, v = int(m.group(1)), m.group(2), int(val, 16)
                d = cur.setdefault(c, {})
                if k == "LCH":
                    d["h"] = v
                elif k == "LCL":
                    d["ptr"] = (d.get("h", 0) << 16) | v
                else:
                    d[k] = v
            elif reg == "DMACON":
                v = int(val, 16)
                if v & 0x8000 and frame >= first:
                    for c in range(4):
                        if v & (1 << c) and c in cur:
                            d = cur[c]
                            out.append((frame, c, d.get("ptr", 0), d.get("LEN", 0) * 2, d.get("PER", 0), d.get("VOL", 0)))
    return out


def cmd_events(a):
    for frame, c, ptr, nbytes, per, vol in events(a.regs, a.first, a.last):
        rate = PAULA_CLOCK / per if per else 0
        print("frame %5d ch%d ptr=%06X %6d bytes period=%4d (%5.0f Hz) vol=%2d" % (frame, c, ptr, nbytes, per, rate, vol))
    return 0


def cmd_speech(a):
    import numpy as np
    import scipy.signal as ss

    w = wave.open(a.wav)
    sr = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 2).astype(float)
    evs = events(a.regs)
    status = 0
    for spec in a.at:
        frame_s, name = spec.split(":", 1)
        frame = int(frame_s)
        mine = [e for e in evs if e[0] == frame and e[3] > 1000]
        if not mine:
            print("frame %d: no sample start longer than 1000 bytes" % frame)
            status = 1
            continue
        period = mine[0][4]
        data = np.fromfile(os.path.join(a.cd0, name), dtype=np.int8).astype(float)
        src = PAULA_CLOCK / period
        n = int(len(data) / src * sr)
        ref = np.interp(np.arange(n) / sr * src, np.arange(len(data)), data)
        s = max(0, int((frame / 50.0 - 0.2) * sr))
        seg = x[s:s + n + int(0.8 * sr)]
        # Each output channel is aligned on its own (a channel at volume 4 is dwarfed by one at 60).
        row = []
        lags = []
        for ch, label in ((0, "L"), (1, "R")):
            c = ss.correlate(seg[:, ch], ref, mode="valid", method="fft")
            k = int(np.argmax(np.abs(c)))
            b = seg[k:k + n, ch]
            denom = np.sqrt(np.dot(b, b) * np.dot(ref, ref))
            ncc = np.dot(b, ref) / denom if denom else 0.0
            gain = np.dot(b, ref) / np.dot(ref, ref)
            row.append("%s ncc %.3f gain %.1f" % (label, ncc, gain))
            lags.append(k / sr - 0.2)
        print("frame %d %s period %d (%.0f Hz): lag %.3f s; %s" % (frame, name, period, src, max(lags), "; ".join(row)))
    return status


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("events")
    e.add_argument("regs")
    e.add_argument("--from", dest="first", type=int, default=0)
    e.add_argument("--to", dest="last", type=int, default=1 << 30)
    s = sub.add_parser("speech")
    s.add_argument("regs")
    s.add_argument("wav")
    s.add_argument("cd0")
    s.add_argument("--at", action="append", required=True, metavar="FRAME:FILE")
    a = ap.parse_args(argv)
    return cmd_events(a) if a.cmd == "events" else cmd_speech(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
