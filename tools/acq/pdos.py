#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# "Rob Northen PDos" track format (WHDLoad imager floppy version v5; docs/re-notes/acquisitions-flux-acorn-st.md).
# Cylinder 0 head 0 is ordinary Amiga MFM; the other 159 tracks hold 12 sectors of 512 bytes in the layout below,
# decoded here from bit cells exactly as the imager's _decodev5 does:
#   per sector: marker 16 cells ($4891 in the first sector's clock context), info = two raw longs (odd/even:
#   ((a & 0x55555555) << 1) | (b & 0x55555555), bit 31 cleared) = [sector:8][track:8][checksum:16], then 128 raw longs
#   (even half) followed 512 bytes later by the odd half; the output is chained with a running key
#   (out_k = dec_k ^ dec_(k-1)); the checksum is the XOR of all 256 raw longs, masked 0x55555555, folded to 16 bits.
#   Sectors are 8,288 cells apart.
#
#   python3 tools/acq/pdos.py DUMP.scp [--flux DUMP.flux] [--out DIR]   # assemble the disk, report, verify RNC streams
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import flux as F  # noqa: E402
import rnc  # noqa: E402

M = 0x55555555
SECTORS = 12
SECTOR_CELLS = 8288
TRACK_BYTES = SECTORS * 512


def _long(cells, p):
    v = 0
    n = len(cells)
    for j in range(32):
        v = (v << 1) | cells[(p + j) % n]
    return v


def decode_sector(cells, p, track, sector):
    """p = first cell of the info longs (after the 16-cell marker). Returns (512 bytes, next position) or None."""
    info = (((_long(cells, p) & M) << 1) | (_long(cells, p + 32) & M)) & 0x7FFFFFFF
    if ((info >> 16) & 0xFF) != track or ((info >> 24) & 0xFF) != sector:
        return None
    q = p + 64
    x = 0
    key = 0
    out = bytearray()
    for k in range(128):
        a = _long(cells, q + 32 * k)
        o = _long(cells, q + 32 * k + 4096)
        x ^= a ^ o
        dec = (((a & M) << 1) | (o & M)) & 0xFFFFFFFF
        key ^= dec
        out += struct.pack(">I", key)
        key = dec
    x &= M
    if ((x & 0xFFFF) | (((x >> 16) << 1) & 0xFFFF)) != (info & 0xFFFF):
        return None
    return bytes(out), q + 8192


def decode_track(cells, track):
    """{sector: (data, position)} for every sector whose number, track and checksum all check out."""
    found = {}
    hits = []
    w = 0
    for i, x in enumerate(cells):
        w = ((w << 1) | x) & 0xFFFF
        if i >= 15 and w == 0x4489:
            hits.append(i + 1)
    for h in hits:
        for dl in range(-6, 7):
            for sec in range(SECTORS):
                if sec in found:
                    continue
                r = decode_sector(cells, h + dl, track, sec)
                if r:
                    found[sec] = (r[0], h + dl)
    return found


PLL_SWEEP = [(0.1, 0.0), (0.1, 0.01), (0.1, 0.02), (0.2, 0.0), (0.2, 0.01), (0.2, 0.05), (0.3, 0.0), (0.3, 0.05), (0.4, 0.02), (0.4, 0.05)]


def read_track(revolutions, track, sweep=PLL_SWEEP):
    """Merge every revolution and PLL setting; only direct (checksum-valid) reads are ever accepted."""
    got = {}
    for rev in revolutions:
        for kp, kf in sweep:
            for lo, hi in ((2, 4),):
                for sec, (data, _pos) in decode_track(F.pll_bits(rev, 2000.0, kp, kf, lo, hi), track).items():
                    got.setdefault(sec, data)
        if len(got) == SECTORS:
            break
    return got


# ---- encoder (tests): the inverse of decode_sector/decode_track on bit cells ----
def _data_cells(value, prev):
    """A 32-bit 'data only' long (data bits at the 0x55555555 positions) as 32 MFM cells (clock, data pairs)."""
    cells = []
    for j in range(16):
        d = (value >> (30 - 2 * j)) & 1
        cells += [1 if (not prev and not d) else 0, d]
        prev = d
    return cells, prev


def encode_track(sectors, track, lead=3000):
    """sectors: 12 x 512 bytes. Returns valid MFM bit cells: a gap, then per sector the 0x4489 marker, info, the even
    half and the odd half of the data, and a short gap, 8,288 cells per sector."""
    cells = [0, 1] * (lead // 2)
    prev = 0
    for s in range(SECTORS):
        longs = struct.unpack(">128I", sectors[s])
        plain, key = [], 0
        for v in longs:            # inverse of out_k = dec_k ^ dec_(k-1)
            key = v ^ key
            plain.append(key)
        even = [(d >> 1) & M for d in plain]
        odd = [d & M for d in plain]
        x = 0
        for e, o in zip(even, odd):
            x ^= e ^ o
        x &= M
        chk = (x & 0xFFFF) | (((x >> 16) << 1) & 0xFFFF)
        info = (s << 24) | (track << 16) | chk
        cells += [(0x4489 >> (15 - j)) & 1 for j in range(16)]
        prev = 1
        for v in ((info >> 1) & M, info & M) + tuple(even) + tuple(odd):
            c, prev = _data_cells(v, prev)
            cells += c
        cells += [0, 1] * ((SECTOR_CELLS - 16 - 64 - 8192) // 2)
        prev = 1
    return cells


def assemble(read_revs, report=print):
    """read_revs(track) -> list of revolutions. Returns (image bytes, missing [(track, sector)])."""
    import mfm
    t0 = read_revs(0)
    rep = mfm.decode_track(F.pll_bits(t0[0], 2000.0, 0.1, 0.01, 1, 12), 0)
    img = bytearray(b"".join(rep.sectors[i].data if i in rep.sectors else b"\0" * 512 for i in range(11)))
    missing = []
    for t in range(1, 160):
        got = read_track(read_revs(t), t)
        for s in range(SECTORS):
            if s in got:
                img += got[s]
            else:
                img += b"\0" * 512
                missing.append((t, s))
    return bytes(img), missing


def main(argv):
    args = argv[1:]
    if not args:
        print(__doc__)
        return 2
    scp = F.read_scp(args[0])
    fl = F.read_fluxengine(args[args.index("--flux") + 1]) if "--flux" in args else None
    out = args[args.index("--out") + 1] if "--out" in args else None

    def revs(t):
        r = list(scp["tracks"].get(t, []))
        if fl and t in fl["tracks"]:
            r += fl["tracks"][t]
        return r

    img, missing = assemble(revs)
    print("image %d bytes sha1 %s, %d sectors unreadable: %s" % (len(img), hashlib.sha1(img).hexdigest(), len(missing), missing))
    ok = 0
    import re
    for m in re.finditer(rb"RNC\x01", img):
        good, h, data = rnc.verify(img, m.start())
        ok += good
        print("RNC @%#x unpacked %d: %s" % (m.start(), h["unpacked"], ("CRCs OK sha1 " + hashlib.sha1(data).hexdigest()) if good else "not verified"))
        if good and out:
            os.makedirs(out, exist_ok=True)
            open(os.path.join(out, "rnc_%x.bin" % m.start()), "wb").write(data)
    if out:
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, "pdos_disk.bin"), "wb").write(img)
    print("%d streams verified" % ok)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
