#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Rob Northen Copylock key-track analysis. The track layout and the LFSR are documented in keirf's public-domain
# disk-utilities (libdisk/format/amiga/copylock.c); this is an independent re-implementation of the detection
# only: per-sector sync words, the "Rob Northen Comp" signature in sector 6, and the 23-bit LFSR that generates the
# sector data (taps 1 and 23; the data byte is an 8-bit window on the state).
#
#   python3 tools/copylock.py FILE.ipf [CYL HEAD]     # default: look at every track and report Copylock tracks
import struct
import sys

SYNC_LIST = [0x8A91, 0x8A44, 0x8A45, 0x8A51, 0x8912, 0x8911, 0x8914, 0x8915, 0x8944, 0x8945, 0x8951]
SIG = b"Rob Northen Comp"


def lfsr_next(x):
    return ((x << 1) & ((1 << 23) - 1)) | (((x >> 22) ^ x) & 1)


def lfsr_byte(x):
    return (x >> 15) & 0xFF


def seed_from_bytes(b):
    """Recover the 23-bit LFSR state that produced a byte stream (needs the first 16 bytes)."""
    x = b[0] << 15
    for k in range(1, 16):
        x |= (b[k] & 1) << (15 - k)
    return x


def mfm_bytes(cells, start, count):
    """Plain (in place, not odd/even split) MFM decode: data cells are the odd cells after the sync word."""
    out = bytearray()
    pos = start
    n = len(cells)
    for _ in range(count):
        v = 0
        for _k in range(8):
            v = (v << 1) | (1 if cells[(pos + 1) % n] == 1 else 0)
            pos += 2
        out.append(v)
    return bytes(out)


def find_sync(cells, word):
    pat = [(word >> (15 - i)) & 1 for i in range(16)]
    n = len(cells)
    ext = cells + cells[:40]
    hits = []
    for i in range(n):
        if all((1 if ext[i + k] == 1 else 0) == pat[k] for k in range(16)):
            hits.append(i)
    return hits


def analyse_track(track):
    """Returns a dict describing the Copylock content of an IpfTrack, or None if it is not a Copylock track."""
    cells = track.cells
    sectors = {}
    for idx, word in enumerate(SYNC_LIST):
        for pos in find_sync(cells, word):
            body = mfm_bytes(cells, pos + 16, 1 + 512)  # index byte + 512 data
            if body[0] == idx or body[0] == idx + 1 or True:
                sectors.setdefault(word, []).append((pos, body))
    if len(sectors) < 6:
        return None
    # sector order by position on the track
    ordered = sorted(((p, w, b) for w, lst in sectors.items() for p, b in lst))
    info = dict(sync_words=[(hex(w), p, b[0]) for p, w, b in ordered], track_cells=len(cells))
    # signature in the sector whose index byte is 6
    sig_secs = [b for p, w, b in ordered if b[0] == 6 and b[1:17] == SIG]
    info["signature"] = bool(sig_secs)
    # the sector with index 0 starts the LFSR: first data byte is seed[22:15]
    s0 = [b for p, w, b in ordered if b[0] == 0]
    if s0:
        seed = seed_from_bytes(s0[0][1:17])
        x = seed
        ok = 0
        for byte in s0[0][1:]:
            if byte != lfsr_byte(x):
                break
            ok += 1
            x = lfsr_next(x)
        info["lfsr_seed"] = seed
        info["lfsr_bytes_match_sector0"] = ok
    info["times"] = sorted(set(track.times))
    return info


def main(argv):
    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    import ipf
    img = ipf.parse_ipf(open(argv[1], "rb").read())
    want = None
    if len(argv) == 4:
        want = (int(argv[2]), int(argv[3]))
    for key in sorted(img.tracks):
        if want and key != want:
            continue
        r = analyse_track(img.tracks[key])
        if r:
            print("cyl %d head %d: Copylock-style track, %d cells, signature %s, seed 0x%06x, sector 0 LFSR bytes ok %s" % (
                key[0], key[1], r["track_cells"], r["signature"], r.get("lfsr_seed", 0),
                r.get("lfsr_bytes_match_sector0")))
            print("  syncs (word, cell position, index byte):", r["sync_words"])
            print("  cell times present:", r["times"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
