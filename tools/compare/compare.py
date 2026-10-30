#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Compare runtime output with a reference emulator capture (Phase 4).

Stdlib only. Subcommands:

  frame REF OURS [--diff OUT.png]     pixel diff after scale/offset/palette alignment
  cadence FRAME.png ...               how many VBL frames each distinct picture is shown
  audio REF.wav OURS.wav              RMS envelope, onset, L/R balance and band spectrum
  wavinfo WAV                         the same measurements for one file
  cutwav WAV F0 F1 OUT                cut emulated frames [F0,F1) (x2ref frames.log or constant
                                      samples/frame), optionally resampled to a fixed frame grid
  report RESULTS.jsonl OUT.md         render collected results as Markdown

Every measuring subcommand prints a summary and, with --json FILE, appends one
JSON object per comparison to FILE (JSON lines), which `report` turns into
docs/re-notes/reference-compare.md.
"""
import argparse
import array
import cmath
import json
import math
import os
import re
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import read_png, write_png  # noqa: E402

FRAME_RATE = 50  # PAL VBLs per second, used to express audio times in frames


# ---------------------------------------------------------------- frames

def load_pixels(path):
    """Return (w, h, grid) with grid[y][x] = 0xRRGGBB."""
    w, h, rows = read_png(path)
    grid = []
    for r in rows:
        grid.append([(r[i] << 16) | (r[i + 1] << 8) | r[i + 2] for i in range(0, 3 * w, 3)])
    return w, h, grid


def q12(c):
    """Reduce 24-bit colour to OCS 12-bit (absorbs x*16 vs x*17 expansion)."""
    return ((c >> 20) & 0xF) << 8 | ((c >> 12) & 0xF) << 4 | ((c >> 4) & 0xF)


def chan_diff(a, b):
    return max(abs(((a >> s) & 0xFF) - ((b >> s) & 0xFF)) for s in (16, 8, 0))


def rescale(w, h, grid, sx, sy):
    """Point-sample a sx*sy enlarged capture back to 1:1 lores pixels."""
    return w // sx, h // sy, [row[0::sx][: w // sx] for row in grid[0::sy][: h // sy]]


def best_alignment(ref, ours, max_off):
    """Find (score, dx, dy) placing `ours` at ref[y+dy][x+dx] with most 12-bit matches.

    Coarse pass: every 4th offset on a sparse pixel grid; then a dense refine around the best.
    """
    rw, rh, rg = ref
    ow, oh, og = ours
    rq = [[q12(c) for c in row] for row in rg]
    oq = [[q12(c) for c in row] for row in og]
    coarse = [(x, y) for y in range(3, oh, 9) for x in range(5, ow, 11)]
    fine = [(x, y) for y in range(0, oh, 3) for x in range(0, ow, 3)]
    samples = coarse

    def score(dx, dy):
        n = hit = 0
        for x, y in samples:
            rx, ry = x + dx, y + dy
            if 0 <= rx < rw and 0 <= ry < rh:
                n += 1
                hit += rq[ry][rx] == oq[y][x]
        # require a decent overlap so tiny overlaps cannot win
        return hit / len(samples) if n * 2 >= len(samples) else -1.0

    lo_x, hi_x = -max_off, max(rw - ow, 0) + max_off
    lo_y, hi_y = -max_off, max(rh - oh, 0) + max_off
    best = (-2.0, 0, 0)
    for dy in range(lo_y, hi_y + 1, 2):
        for dx in range(lo_x, hi_x + 1, 4):
            s = score(dx, dy)
            if s > best[0]:
                best = (s, dx, dy)
    _, bx, by = best
    samples = fine
    best = (-2.0, bx, by)
    for dy in range(by - 3, by + 4):
        for dx in range(bx - 4, bx + 5):
            s = score(dx, dy)
            if s > best[0]:
                best = (s, dx, dy)
    return best


def frame_compare(ref_path, ours_path, tol=16, max_off=48, diff_out=None, scale=None, offset=None):
    """Diff `ours` against `ref`. With both `scale` and `offset` given the geometry search is skipped."""
    rw, rh, rg = load_pixels(ref_path)
    ow, oh, og = load_pixels(ours_path)
    if scale and offset is not None:
        best = (-1.0, offset[0], offset[1], scale[0], scale[1], rescale(rw, rh, rg, *scale))
    else:
        if scale:
            cands = [scale]
        else:
            cands = [(sx, sy) for sx in (1, 2, 4) for sy in (1, 2)
                     if rw // sx >= ow * 0.75 and rh // sy >= oh * 0.6]
        best = None
        for sx, sy in cands:
            ref = rescale(rw, rh, rg, sx, sy)
            s, dx, dy = best_alignment(ref, (ow, oh, og), max_off)
            if best is None or s > best[0]:
                best = (s, dx, dy, sx, sy, ref)
    s, dx, dy, sx, sy, (fw, fh, fg) = best

    total = overlap = exact = within = same12 = 0
    pal = {}
    diff_rows = []
    for y in range(oh):
        out = bytearray(3 * ow)
        for x in range(ow):
            total += 1
            o = og[y][x]
            rx, ry = x + dx, y + dy
            if not (0 <= rx < fw and 0 <= ry < fh):
                out[3 * x:3 * x + 3] = b"\x00\x00\xff"
                continue
            overlap += 1
            r = fg[ry][rx]
            pal.setdefault(o, {}).setdefault(r, 0)
            pal[o][r] += 1
            d = chan_diff(r, o)
            exact += d == 0
            within += d <= tol
            same12 += q12(r) == q12(o)
            if d <= tol:
                g = ((o >> 16 & 0xFF) + (o >> 8 & 0xFF) + (o & 0xFF)) // 9
                out[3 * x:3 * x + 3] = bytes((g, g, g))
            else:
                out[3 * x:3 * x + 3] = b"\xff\x20\x20"
        diff_rows.append(out)
    if diff_out:
        write_png(diff_out, ow, oh, diff_rows)

    # palette consistency: does each of our colours map to one reference colour?
    pal_rows = []
    for o, refs in sorted(pal.items(), key=lambda kv: -sum(kv[1].values())):
        n = sum(refs.values())
        r, cnt = max(refs.items(), key=lambda kv: kv[1])
        pal_rows.append({"ours": f"{o:06X}", "ref": f"{r:06X}", "share": round(cnt / n, 4), "pixels": n})
    remap = [p for p in pal_rows if p["ours"] != p["ref"] and p["share"] >= 0.9 and p["pixels"] >= 50]

    res = {
        "kind": "frame", "ref": ref_path, "ours": ours_path,
        "ref_size": [rw, rh], "ours_size": [ow, oh], "scale": [sx, sy],
        "offset": [dx, dy], "align_score": round(s, 4), "tolerance": tol,
        "overlap_pct": round(100 * overlap / total, 3),
        "differing_pct": round(100 * (overlap - within) / max(overlap, 1), 3),
        "exact_match_pct": round(100 * exact / max(overlap, 1), 3),
        "match12_pct": round(100 * same12 / max(overlap, 1), 3),
        "palette_top": pal_rows[:16], "palette_remaps": remap[:16],
    }
    return res


def print_frame(res):
    print(f"frame {os.path.basename(res['ref'])} vs {os.path.basename(res['ours'])}: "
          f"ref {res['ref_size'][0]}x{res['ref_size'][1]} scale {res['scale'][0]}x{res['scale'][1]} "
          f"offset ({res['offset'][0]},{res['offset'][1]}) overlap {res['overlap_pct']}%")
    print(f"  differing (> {res['tolerance']}/255 any channel): {res['differing_pct']}%   "
          f"exact {res['exact_match_pct']}%   same OCS 12-bit colour {res['match12_pct']}%")
    for p in res["palette_remaps"]:
        print(f"  palette: ours {p['ours']} -> ref {p['ref']} ({100 * p['share']:.1f}% of {p['pixels']} px)")


def frame_number(path):
    m = re.findall(r"\d+", os.path.basename(path))
    return int(m[-1]) if m else 0


def cadence(paths, min_diff_px=1):
    """Run lengths of identical consecutive frames (frames must be consecutive VBLs)."""
    paths = sorted(paths, key=frame_number)
    prev = None
    runs, cur, changes = [], 0, []
    for p in paths:
        _, _, rows = read_png(p)
        data = b"".join(rows)
        if prev is not None:
            diff = sum(1 for i in range(0, len(data), 3) if data[i:i + 3] != prev[i:i + 3]) \
                if data != prev else 0
            if diff >= min_diff_px:
                runs.append(cur)
                changes.append(frame_number(p))
                cur = 0
        cur += 1
        prev = data
    runs.append(cur)
    inner = runs[1:-1]  # first/last run are truncated by the window
    hist = {}
    for r in inner:
        hist[r] = hist.get(r, 0) + 1
    n = len(paths)
    res = {
        "kind": "cadence", "frames": [frame_number(paths[0]), frame_number(paths[-1])], "count": n,
        "picture_changes": len(changes), "run_hist": {str(k): v for k, v in sorted(hist.items())},
        "pictures_per_s": round(len(changes) * FRAME_RATE / max(n - 1, 1), 3),
        "change_frames": changes[:200],
    }
    return res


# ---------------------------------------------------------------- audio

def load_wav(path):
    with wave.open(path, "rb") as w:
        rate, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError(f"{path}: only 16-bit WAV supported")
    a = array.array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":
        a.byteswap()
    if ch == 1:
        return rate, a, a
    return rate, a[0::ch], a[1::ch]


def ac_rms(seg):
    """RMS with the window's mean removed (Amiberry's idle output carries a DC offset)."""
    n = len(seg)
    m = sum(seg) / n
    return math.sqrt(max(sum(v * v for v in seg) / n - m * m, 0.0))


def envelope(rate, left, right):
    """Per-VBL-frame (1/50 s) AC RMS of each channel."""
    win = rate // FRAME_RATE
    n = len(left) // win
    el, er = [], []
    for i in range(n):
        s = slice(i * win, (i + 1) * win)
        el.append(ac_rms(left[s]))
        er.append(ac_rms(right[s]))
    return el, er


def fft(x):
    n = len(x)
    j = 0
    a = list(x)
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        for start in range(0, n, size):
            wk = 1
            for k in range(size // 2):
                u = a[start + k]
                t = wk * a[start + k + size // 2]
                a[start + k] = u + t
                a[start + k + size // 2] = u - t
                wk *= w
        size <<= 1
    return a


BANDS = [(0, 100), (100, 500), (500, 2000), (2000, 5000), (5000, 24000)]


def band_energy(rate, left, right, f0, f1, nfft=4096, hop_frames=10):
    """Share of energy per band for frames [f0, f1), Hann-windowed FFTs every hop_frames."""
    win = rate // FRAME_RATE
    hann = [0.5 - 0.5 * math.cos(2 * math.pi * i / nfft) for i in range(nfft)]
    acc = [0.0] * len(BANDS)
    for fr in range(f0, f1, hop_frames):
        s = fr * win
        if s + nfft > len(left):
            break
        x = [(left[s + i] + right[s + i]) * hann[i] for i in range(nfft)]
        spec = fft(x)
        for k in range(1, nfft // 2):
            f = k * rate / nfft
            e = abs(spec[k]) ** 2
            for b, (lo, hi) in enumerate(BANDS):
                if lo <= f < hi:
                    acc[b] += e
                    break
    tot = sum(acc) or 1.0
    return {f"{lo}-{hi}Hz": round(100 * e / tot, 2) for (lo, hi), e in zip(BANDS, acc)}


def onset(env, start=0, thr=200.0, hold=3):
    """First frame >= start where RMS stays above thr for `hold` frames."""
    for i in range(start, len(env) - hold):
        if all(env[i + k] > thr for k in range(hold)):
            return i
    return None


def balance(el, er, f0, f1, thr=200.0):
    sl = sr = 0.0
    ratios = []
    for i in range(f0, min(f1, len(el))):
        if el[i] > thr and er[i] > thr:
            ratios.append(er[i] / el[i])
        sl += el[i] ** 2
        sr += er[i] ** 2
    ratios.sort()
    return {
        "rms_left": round(math.sqrt(sl / max(f1 - f0, 1)), 1),
        "rms_right": round(math.sqrt(sr / max(f1 - f0, 1)), 1),
        "right_over_left_total": round(math.sqrt(sr / sl), 3) if sl else None,
        "right_over_left_median": round(ratios[len(ratios) // 2], 3) if ratios else None,
        "frames_both_audible": len(ratios),
    }


def wav_info(path, f0=None, f1=None, thr=200.0):
    rate, left, right = load_wav(path)
    el, er = envelope(rate, left, right)
    mono = [math.hypot(a, b) for a, b in zip(el, er)]
    first = onset(mono, 0, thr)
    f0 = first if f0 is None else f0
    f1 = len(el) if f1 is None else min(f1, len(el))
    peak = max(max(left, default=0), -min(left, default=0), max(right, default=0), -min(right, default=0))
    return {
        "kind": "wav", "file": path, "rate": rate, "frames": len(el),
        "first_sound_frame": first, "window": [f0, f1], "peak": peak,
        "balance": balance(el, er, f0 or 0, f1, thr),
        "bands": band_energy(rate, left, right, f0 or 0, f1),
    }, (el, er, mono)


def cut_wav(wav_path, f0, f1, out_path, frames_log=None, spf_in=None, spf_out=None):
    """Cut emulated frames [f0, f1) out of a WAV so that the result starts exactly at frame f0.

    Frame boundaries come from an x2ref frames.log (column 3 = sample position at the start of each
    frame; Amiberry's host-synced sound clock gives a slightly varying count per frame) or from a
    constant `spf_in` samples per frame (the runtime). With `spf_out`, every frame is linearly
    resampled to exactly that many samples, so both captures share one 1/50 s frame grid.
    """
    if frames_log:
        pos = {}
        with open(frames_log) as f:
            for line in f:
                if not line.startswith("#"):
                    p = line.split()
                    pos[int(p[0])] = int(p[2])
        bounds = [pos[i] for i in range(f0, f1 + 1)]
    else:
        bounds = [i * spf_in for i in range(f0, f1 + 1)]
    with wave.open(wav_path, "rb") as w:
        params = w.getparams()
        start = int(bounds[0])
        w.setpos(start)
        raw = w.readframes(int(math.ceil(bounds[-1])) - start + 1)
    a = array.array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":
        a.byteswap()
    if not spf_out:
        out = a[: 2 * (int(bounds[-1]) - start)]
    else:
        out = array.array("h")
        n = len(a) // 2
        for k in range(f1 - f0):
            b0, b1 = bounds[k] - start, bounds[k + 1] - start
            for j in range(spf_out):
                t = b0 + (b1 - b0) * j / spf_out
                i = int(t)
                fr = t - i
                i2 = min(i + 1, n - 1)
                for c in (0, 1):
                    out.append(int(round(a[2 * i + c] * (1 - fr) + a[2 * i2 + c] * fr)))
    if sys.byteorder == "big":
        out.byteswap()
    with wave.open(out_path, "wb") as o:
        o.setparams(params)
        o.writeframes(out.tobytes())
    return len(out) // 2


def xcorr_lag(a, b, max_lag):
    """Lag L (frames) maximising corr(a[i], b[i+L]) of mean-removed envelopes."""
    def norm(v):
        m = sum(v) / len(v)
        return [x - m for x in v]
    a, b = norm(a), norm(b)
    best = (-2.0, 0)
    for lag in range(-max_lag, max_lag + 1):
        num = da = db = 0.0
        for i in range(len(a)):
            j = i + lag
            if 0 <= j < len(b):
                num += a[i] * b[j]
                da += a[i] * a[i]
                db += b[j] * b[j]
        if da and db:
            c = num / math.sqrt(da * db)
            if c > best[0]:
                best = (c, lag)
    return best


def audio_compare(ref_path, ours_path, ref_start=None, ours_start=None, length=None,
                  max_lag=25, thr=200.0):
    """Align on the first sound (or given start frames), then compare envelopes."""
    ri, (rl, rr, rm) = wav_info(ref_path, thr=thr)
    oi, (ol, orr, om) = wav_info(ours_path, thr=thr)
    rs = ri["first_sound_frame"] if ref_start is None else ref_start
    os_ = oi["first_sound_frame"] if ours_start is None else ours_start
    if rs is None or os_ is None:
        raise ValueError("no sound found to align on; pass --ref-start/--ours-start")
    n = min(len(rm) - rs, len(om) - os_)
    if length:
        n = min(n, length)
    c, lag = xcorr_lag(rm[rs:rs + n], om[os_:os_ + n], max_lag)
    os_ += lag
    n = min(n, len(om) - os_)
    level = (sum(om[os_:os_ + n]) / max(sum(rm[rs:rs + n]), 1e-9))
    err = math.sqrt(sum((om[os_ + i] / level - rm[rs + i]) ** 2 for i in range(n)) / n) / \
        max(math.sqrt(sum(v * v for v in rm[rs:rs + n]) / n), 1e-9)
    ri2, _ = wav_info(ref_path, rs, rs + n, thr)
    oi2, _ = wav_info(ours_path, os_, os_ + n, thr)
    return {
        "kind": "audio", "ref": ref_path, "ours": ours_path,
        "ref_start": rs, "ours_start": os_, "frames": n, "lag_refined": lag,
        "envelope_corr": round(c, 4), "level_ours_over_ref": round(level, 3),
        "envelope_rel_err": round(err, 4),
        "ref_balance": ri2["balance"], "ours_balance": oi2["balance"],
        "ref_bands": ri2["bands"], "ours_bands": oi2["bands"],
    }


def print_audio(res):
    print(f"audio: ref start frame {res['ref_start']}, ours start frame {res['ours_start']} "
          f"(xcorr adjust {res['lag_refined']:+d}), {res['frames']} frames compared")
    print(f"  envelope correlation {res['envelope_corr']}, level ours/ref {res['level_ours_over_ref']}, "
          f"relative envelope error {res['envelope_rel_err']}")
    for k in ("ref", "ours"):
        b = res[f"{k}_balance"]
        print(f"  {k:4s} L/R RMS {b['rms_left']}/{b['rms_right']}  R/L total {b['right_over_left_total']} "
              f"median {b['right_over_left_median']}")
        print(f"       bands {res[f'{k}_bands']}")


# ---------------------------------------------------------------- report

def render_report(results, title="Reference comparison"):
    out = [f"# {title}", "", "Generated by `tools/compare/compare.py report`. Do not edit by hand; "
           "re-run the comparison instead.", ""]
    frames = [r for r in results if r["kind"] == "frame"]
    if frames:
        out += ["## Frames", "",
                "| Checkpoint | Ref | Ours | Scale | Offset | Overlap % | Differing % | Exact % | Same 12-bit % |",
                "|---|---|---|---|---|---|---|---|---|"]
        for r in frames:
            out.append(f"| {r.get('label', '')} | {os.path.basename(r['ref'])} | {os.path.basename(r['ours'])} "
                       f"| {r['scale'][0]}x{r['scale'][1]} | {r['offset'][0]},{r['offset'][1]} "
                       f"| {r['overlap_pct']} | {r['differing_pct']} | {r['exact_match_pct']} | {r['match12_pct']} |")
        remaps = [(r.get("label", ""), p) for r in frames for p in r["palette_remaps"]]
        if remaps:
            out += ["", "Consistent colour remaps (ours -> reference, >= 90% of a colour's pixels):", ""]
            out += [f"- {lab}: `{p['ours']}` -> `{p['ref']}` ({p['pixels']} px)" for lab, p in remaps[:40]]
        out.append("")
    cads = [r for r in results if r["kind"] == "cadence"]
    if cads:
        out += ["## Picture cadence", "", "| Source | Frames | Picture changes | Pictures/s | VBLs per picture (count) |",
                "|---|---|---|---|---|"]
        for r in cads:
            hist = ", ".join(f"{k}: {v}" for k, v in r["run_hist"].items())
            out.append(f"| {r.get('label', '')} | {r['frames'][0]}-{r['frames'][1]} | {r['picture_changes']} "
                       f"| {r['pictures_per_s']} | {hist} |")
        out.append("")
    auds = [r for r in results if r["kind"] == "audio"]
    if auds:
        out += ["## Audio", ""]
        for r in auds:
            rb, ob = r["ref_balance"], r["ours_balance"]
            out += [f"### {r.get('label', os.path.basename(r['ref']))}", "",
                    f"- Aligned: reference frame {r['ref_start']} = our frame {r['ours_start']}; {r['frames']} frames.",
                    f"- Envelope correlation {r['envelope_corr']}; level ours/ref {r['level_ours_over_ref']}; "
                    f"relative envelope error {r['envelope_rel_err']}.",
                    f"- Right/left RMS: reference {rb['right_over_left_total']} (median {rb['right_over_left_median']}), "
                    f"ours {ob['right_over_left_total']} (median {ob['right_over_left_median']}).",
                    f"- Bands, reference: {r['ref_bands']}",
                    f"- Bands, ours: {r['ours_bands']}", ""]
    notes = [r for r in results if r["kind"] == "note"]
    if notes:
        out += ["## Notes", ""] + [f"- {r['text']}" for r in notes] + [""]
    return "\n".join(out)


def emit(res, args):
    if getattr(args, "label", None):
        res["label"] = args.label
    if getattr(args, "json", None):
        with open(args.json, "a") as f:
            f.write(json.dumps(res) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("frame")
    p.add_argument("ref")
    p.add_argument("ours")
    p.add_argument("--tol", type=int, default=16, help="per-channel tolerance, 0-255 (default 16)")
    p.add_argument("--max-offset", type=int, default=48)
    p.add_argument("--scale", help="force reference scale, e.g. 2x2")
    p.add_argument("--offset", help="with --scale: fixed placement DX,DY of ours in the reference")
    p.add_argument("--diff", help="write a diff PNG (red = differs, blue = no reference pixel)")

    p = sub.add_parser("cadence")
    p.add_argument("frames", nargs="+")
    p.add_argument("--min-diff-px", type=int, default=1)

    p = sub.add_parser("audio")
    p.add_argument("ref")
    p.add_argument("ours")
    p.add_argument("--ref-start", type=int)
    p.add_argument("--ours-start", type=int)
    p.add_argument("--length", type=int, help="frames to compare")
    p.add_argument("--max-lag", type=int, default=25)
    p.add_argument("--thr", type=float, default=200.0, help="RMS silence threshold (int16 units)")

    p = sub.add_parser("wavinfo")
    p.add_argument("wav")
    p.add_argument("--from", dest="f0", type=int)
    p.add_argument("--to", dest="f1", type=int)
    p.add_argument("--thr", type=float, default=200.0)

    for name in ("frame", "cadence", "audio", "wavinfo"):
        sp = sub.choices[name]
        sp.add_argument("--json", help="append the result as a JSON line to this file")
        sp.add_argument("--label", help="checkpoint name used in the report")

    p = sub.add_parser("cutwav")
    p.add_argument("wav")
    p.add_argument("f0", type=int)
    p.add_argument("f1", type=int)
    p.add_argument("out")
    p.add_argument("--frames-log", help="x2ref frames.log giving per-frame sample positions")
    p.add_argument("--spf-in", type=float, help="constant input samples per frame (runtime WAV)")
    p.add_argument("--spf-out", type=int, help="resample every frame to this many samples (e.g. 960)")

    p = sub.add_parser("report")
    p.add_argument("results")
    p.add_argument("out")
    p.add_argument("--title", default="Reference comparison")

    args = ap.parse_args(argv)
    if args.cmd == "frame":
        scale = tuple(int(v) for v in args.scale.split("x")) if args.scale else None
        off = tuple(int(v) for v in args.offset.split(",")) if args.offset else None
        res = frame_compare(args.ref, args.ours, args.tol, args.max_offset, args.diff, scale, off)
        print_frame(res)
    elif args.cmd == "cadence":
        res = cadence(args.frames, args.min_diff_px)
        print(f"cadence frames {res['frames'][0]}-{res['frames'][1]}: {res['picture_changes']} changes, "
              f"{res['pictures_per_s']} pictures/s, VBLs per picture {res['run_hist']}")
    elif args.cmd == "audio":
        res = audio_compare(args.ref, args.ours, args.ref_start, args.ours_start, args.length,
                            args.max_lag, args.thr)
        print_audio(res)
    elif args.cmd == "wavinfo":
        res, _ = wav_info(args.wav, args.f0, args.f1, args.thr)
        b = res["balance"]
        print(f"{res['file']}: {res['frames']} frames @ {res['rate']} Hz, first sound frame "
              f"{res['first_sound_frame']}, peak {res['peak']}")
        print(f"  window {res['window']}: L/R RMS {b['rms_left']}/{b['rms_right']}, R/L total "
              f"{b['right_over_left_total']}, median {b['right_over_left_median']}")
        print(f"  bands {res['bands']}")
    elif args.cmd == "cutwav":
        n = cut_wav(args.wav, args.f0, args.f1, args.out, args.frames_log, args.spf_in, args.spf_out)
        print(f"wrote {args.out}: frames {args.f0}-{args.f1}, {n} samples")
        return 0
    else:
        with open(args.results) as f:
            results = [json.loads(line) for line in f if line.strip()]
        with open(args.out, "w") as f:
            f.write(render_report(results, args.title))
        print(f"wrote {args.out} ({len(results)} results)")
        return 0
    emit(res, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
