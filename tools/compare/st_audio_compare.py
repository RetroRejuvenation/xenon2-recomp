#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Compare the YM2149 output of x2recomp's Atari ST runtime with a reference capture (Hatari's ym.wav).

usage: st_audio_compare.py OURS.wav REFERENCE.wav [--window-frames 100] [--json OUT.json]

Both files are turned to mono and resampled to 22,050 Hz. The two runs do not start together (the reference boots EmuTOS
first, and floppy timing differs), so the reference is aligned to ours by cross-correlating the 1 kHz RMS envelopes (a
coarse pass over the whole of the shorter file, then a fine pass). After alignment, each window of N video frames
(default 100 = 2 s at 50 Hz) is scored with
  env   Pearson correlation of the 20 ms RMS envelopes,
  band  Pearson correlation of the 16-band log spectra averaged over the window (timbre),
  fine  mean over time of the per-frame correlation between the two 16-band log-spectrum vectors.
The summary gives the lag, and the median / 10th percentile / minimum of each score, for the whole run and for the
windows where both signals are not silent. Needs numpy and scipy.

--local-lag S (default 0 = off) refines the alignment per window within +-S seconds (the two runs' floppy loads take
different times, so a single global lag only fits one part of a run): each window is shifted to the offset that best
matches the 1 kHz envelopes before it is scored, and the offsets are reported.
"""
import argparse
import json
import sys
import wave

import numpy as np
from scipy.signal import resample_poly

RATE = 22050
HOP = 441          # 20 ms
NFFT = 1024
BANDS = 16
FRAME_S = 313 * 512 / 8021247  # one ST video frame in seconds


def load_mono(path):
    w = wave.open(path)
    n, ch, sw, fr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    if sw != 2:
        sys.exit(f"{path}: expected 16-bit PCM")
    d = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, ch).astype(np.float64).mean(axis=1)
    g = np.gcd(fr, RATE)
    return resample_poly(d, RATE // g, fr // g), fr


def envelope_1k(x):
    n = len(x) // 22 * 22  # ~1 kHz at 22,050 Hz
    return np.sqrt((x[:n].reshape(-1, 22) ** 2).mean(axis=1))


def best_lag(a, b, lo_s, hi_s):
    """Shift of b (in 1 kHz samples) that best matches a: a[t] ~ b[t + lag]."""
    ea, eb = np.log1p(envelope_1k(a)), np.log1p(envelope_1k(b))
    ea, eb = ea - ea.mean(), eb - eb.mean()
    n = 1
    while n < len(ea) + len(eb):
        n *= 2
    xc = np.fft.irfft(np.fft.rfft(eb, n) * np.conj(np.fft.rfft(ea, n)), n)
    best, best_v = 0, -1e30
    for lag in range(int(lo_s * 1000), int(hi_s * 1000) + 1):
        i = lag % n
        v = xc[i]
        if v > best_v:
            best, best_v = lag, v
    return best


def band_log_spectra(x):
    win = np.hanning(NFFT)
    nfr = (len(x) - NFFT) // HOP + 1
    edges = np.geomspace(100, 10000, BANDS + 1)
    freqs = np.fft.rfftfreq(NFFT, 1 / RATE)
    idx = [(np.searchsorted(freqs, edges[i]), np.searchsorted(freqs, edges[i + 1])) for i in range(BANDS)]
    out = np.zeros((nfr, BANDS))
    rms = np.zeros(nfr)
    for t in range(nfr):
        seg = x[t * HOP:t * HOP + NFFT]
        s = np.abs(np.fft.rfft(seg * win)) ** 2
        for b, (i0, i1) in enumerate(idx):
            out[t, b] = s[i0:max(i1, i0 + 1)].sum()
        rms[t] = np.sqrt((seg ** 2).mean())
    return np.log10(out + 1.0), rms


def pearson(a, b):
    a, b = a - a.mean(), b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 1e-9 else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("ref")
    ap.add_argument("--window-frames", type=int, default=100)
    ap.add_argument("--max-lag", type=float, default=40.0, help="seconds the reference may be late or early")
    ap.add_argument("--local-lag", type=float, default=0.0, help="per-window refinement range in seconds (0 = off)")
    ap.add_argument("--json")
    a = ap.parse_args()
    ours, _ = load_mono(a.ours)
    ref, _ = load_mono(a.ref)
    lag_ms = best_lag(ours, ref, -a.max_lag, a.max_lag)
    shift = int(round(lag_ms * RATE / 1000))
    # a[t] ~ b[t + shift]
    if shift >= 0:
        ref = ref[shift:]
    else:
        ours = ours[-shift:]
    n = min(len(ours), len(ref))
    ours, ref = ours[:n], ref[:n]
    per = max(1, int(round(a.window_frames * FRAME_S / (HOP / RATE))))
    win_samples = per * HOP
    rows = []
    for w0 in range(0, (n - NFFT) // HOP - per + 1, per):
        s0 = w0 * HOP
        seg_a = ours[s0:s0 + win_samples + NFFT]
        off = 0
        if a.local_lag > 0:
            rng = int(a.local_lag * RATE)
            lo, hi = max(-s0, -rng), min(n - (s0 + win_samples + NFFT), rng)
            ea = np.log1p(envelope_1k(seg_a))
            best_v = -2.0
            for o in range(lo, hi + 1, 22):  # 1 ms steps
                eb = np.log1p(envelope_1k(ref[s0 + o:s0 + o + len(seg_a)]))
                m = min(len(ea), len(eb))
                v = pearson(ea[:m], eb[:m])
                if not np.isnan(v) and v > best_v:
                    best_v, off = v, o
        seg_r = ref[s0 + off:s0 + off + len(seg_a)]
        la, rms_a = band_log_spectra(seg_a)
        ra, rms_r = band_log_spectra(seg_r)
        k = min(len(la), len(ra), per)
        la, ra, rms_a, rms_r = la[:k], ra[:k], rms_a[:k], rms_r[:k]
        env = pearson(rms_a, rms_r)
        band = pearson(la.mean(axis=0), ra.mean(axis=0))
        fine = [pearson(la[t], ra[t]) for t in range(k)]
        fine = float(np.nanmean(fine)) if not np.all(np.isnan(fine)) else float("nan")
        active = bool(rms_a.mean() > 50 and rms_r.mean() > 50)
        rows.append({"start_s": s0 / RATE, "env": env, "band": band, "fine": fine, "active": active,
                     "local_offset_ms": off * 1000.0 / RATE, "rms_ours": float(rms_a.mean()), "rms_ref": float(rms_r.mean())})

    def stats(rs, key):
        v = np.array([r[key] for r in rs if not np.isnan(r[key])])
        if not len(v):
            return None
        return {"median": float(np.median(v)), "p10": float(np.percentile(v, 10)), "min": float(v.min()), "n": int(len(v))}

    act = [r for r in rows if r["active"]]
    summary = {"lag_ms": lag_ms, "seconds_compared": n / RATE, "windows": len(rows), "active_windows": len(act),
               "level_ratio_median": float(np.median([r["rms_ours"] / r["rms_ref"] for r in act])) if act else None,
               "all": {k: stats(rows, k) for k in ("env", "band", "fine")},
               "active": {k: stats(act, k) for k in ("env", "band", "fine")}}
    print(json.dumps(summary, indent=2))
    if a.json:
        json.dump({"summary": summary, "windows": rows}, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
