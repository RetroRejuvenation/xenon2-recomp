# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Flux readers for the two dump formats seen on the Internet Archive for Amiga disks, and a phase-tracking PLL.
#   * SuperCard Pro (.scp): header "SCP", per-track offset table, revolutions of big-endian 16-bit deltas (25 ns units;
#     a 0 word adds 65536). Revolutions are returned as lists of intervals in 25 ns ticks.
#   * FluxEngine (.flux): a protobuf stream: field 1 = magic 0x466C7578 ("Flux"), field 2 = version, then repeated
#     field 3 messages {1: cylinder, 2: head, 3: flux bytes}. Flux bytes: bit 7 = pulse, bit 6 = index, low 6 bits =
#     ticks of 83.333 ns (12 MHz); 0x3F adds 63 ticks and continues. Revolutions are the spans between index marks.
# Both return intervals in 25 ns ticks so the same PLL serves either.
import struct

TICK_NS = 25.0


def read_scp(path):
    with open(path, "rb") as fh:
        d = fh.read()
    if d[:3] != b"SCP":
        raise ValueError("not an SCP file")
    revs, first, last = d[5], d[6], d[7]
    if revs == 0 or revs > 32:
        raise ValueError("implausible revolution count")
    offs = struct.unpack("<168I", d[16:16 + 672])
    tracks = {}
    for t in range(min(168, last + 1)):
        o = offs[t]
        if not o or o + 4 + 12 * revs > len(d) or d[o:o + 3] != b"TRK":
            continue
        out = []
        for r in range(revs):
            dur, cnt, doff = struct.unpack("<III", d[o + 4 + 12 * r:o + 16 + 12 * r])
            if o + doff + 2 * cnt > len(d):
                continue
            raw = struct.unpack(">%dH" % cnt, d[o + doff:o + doff + 2 * cnt])
            vals, acc = [], 0
            for x in raw:
                if x == 0:
                    acc += 65536
                    continue
                vals.append(x + acc)
                acc = 0
            out.append(vals)
        tracks[t] = out
    return dict(version=d[3], disk_type=d[4], revolutions=revs, first=first, last=last, flags=d[8], tracks=tracks)


def _varint(b, i):
    v = s = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return v, i


def read_fluxengine(path):
    with open(path, "rb") as fh:
        d = fh.read()
    i = 0
    k, i = _varint(d, i)
    magic, i = _varint(d, i)
    if k != 8 or magic != 0x466C7578:
        raise ValueError("not a FluxEngine .flux file")
    k, i = _varint(d, i)
    version, i = _varint(d, i)
    tracks = {}
    while i < len(d):
        k, i = _varint(d, i)
        ln, i = _varint(d, i)
        end = i + ln
        if k != 0x1A or end > len(d):
            break
        cyl = head = None
        blob = b""
        while i < end:
            kk, i = _varint(d, i)
            fn = kk >> 3
            if kk & 7 == 0:
                v, i = _varint(d, i)
                if fn == 1:
                    cyl = v
                elif fn == 2:
                    head = v
            else:
                l2, i = _varint(d, i)
                blob = d[i:i + l2]
                i += l2
        if cyl is not None and head is not None:
            tracks[cyl * 2 + head] = fluxengine_revolutions(blob)
    return dict(version=version, tracks=tracks)


def fluxengine_revolutions(blob):
    acc = 0
    pulses, idx = [], []
    for b in blob:
        if b == 0x3F:
            acc += 63
            continue
        acc += b & 0x3F
        if b & 0x80:
            pulses.append(acc)
            acc = 0
        if b & 0x40:
            idx.append(len(pulses))
    scale = 83.3333 / TICK_NS
    return [[x * scale for x in pulses[a:b]] for a, b in zip(idx, idx[1:])]


def pll_bits(intervals, cell_ns=2000.0, kphase=0.1, kfreq=0.01, lo=2, hi=4, span=0.1):
    """Phase-tracking PLL: every transition is placed on the cell grid by its absolute time, so symmetric peak shift
    cancels. lo/hi clamp the run length to the valid MFM range (2-4 cells)."""
    nominal = cell_ns / TICK_NS
    clk, t, last = nominal, 0.0, 0.0
    out = []
    for x in intervals:
        t += x
        n = min(hi, max(lo, int(round((t - last) / clk))))
        pred = last + n * clk
        err = t - pred
        last = pred + kphase * err
        clk = min(max(clk + kfreq * err / n, nominal * (1 - span)), nominal * (1 + span))
        out.extend([0] * (n - 1) + [1])
    return out
