#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Convert a stemu --shot file (docs/re-notes/atari-st-s0.md) to a PNG: low resolution, 4 interleaved bitplanes,
320x200, the ST palette (9 bits: three bits per colour in bits 8-10 red, 4-6 green, 0-2 blue). Standard library only.

usage: stshot.py IN.stshot OUT.png [SCALE]     (SCALE: integer pixel multiplier, default 2)
       stshot.py --decode BYTES32000 PALETTE32 -> used by the tests through decode()
"""
import struct
import sys
import zlib


def st_colour(word):
    r, g, b = (word >> 8) & 7, (word >> 4) & 7, word & 7
    return (r * 255 // 7, g * 255 // 7, b * 255 // 7)


def decode(screen, palette):
    """screen: 32000 bytes (200 lines x 160 bytes, 20 groups of 4 words per line); palette: 32 bytes. Returns 200 rows of
    320 (r, g, b) tuples."""
    pal = [st_colour((palette[2 * i] << 8) | palette[2 * i + 1]) for i in range(16)]
    rows = []
    for y in range(200):
        row = []
        base = y * 160
        for g in range(20):
            planes = struct.unpack(">4H", screen[base + g * 8:base + g * 8 + 8])
            for x in range(16):
                bit = 15 - x
                c = ((planes[0] >> bit) & 1) | (((planes[1] >> bit) & 1) << 1) | (((planes[2] >> bit) & 1) << 2) | (((planes[3] >> bit) & 1) << 3)
                row.append(pal[c])
        rows.append(row)
    return rows


def png(rows, scale=1):
    h, w = len(rows), len(rows[0])
    raw = bytearray()
    for row in rows:
        line = bytearray()
        for (r, g, b) in row:
            line += bytes((r, g, b)) * scale
        for _ in range(scale):
            raw += b"\x00" + line

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    d = open(argv[1], "rb").read()
    if d[:6] != b"STSHOT" or len(d) < 6 + 4 + 32000 + 32:
        print("not a stemu --shot file")
        return 1
    screen = d[10:10 + 32000]
    palette = d[10 + 32000:10 + 32000 + 32]
    scale = int(argv[3]) if len(argv) > 3 else 2
    open(argv[2], "wb").write(png(decode(screen, palette), scale))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
