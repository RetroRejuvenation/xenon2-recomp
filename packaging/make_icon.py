#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Write a simple generated application icon as a PNG (no game art: a geometric arrow on a gradient).

    python3 -I packaging/make_icon.py OUT.png [SIZE]

Used by tools/package.sh (macOS .icns via sips + iconutil). Standard library only.
"""
import struct
import sys
import zlib


def png(path, size, pixel):
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # filter: none
        for x in range(size):
            rows.extend(pixel(x / size, y / size))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 9)))
        f.write(chunk(b"IEND", b""))


def inside_arrow(u, v):
    # An upward arrowhead (a ship seen from above) with a notch at the back, centred.
    if not 0.22 <= v <= 0.80:
        return False
    half = (v - 0.22) / 0.58 * 0.30  # half width grows towards the bottom
    if abs(u - 0.5) > half:
        return False
    return not (v > 0.62 and abs(u - 0.5) < (v - 0.62) * 0.9)


def pixel(u, v):
    # Rounded square with a vertical blue-violet gradient, a light arrow, transparent corners.
    r = 0.18
    cx, cy = min(max(u, r), 1 - r), min(max(v, r), 1 - r)
    if (u - cx) ** 2 + (v - cy) ** 2 > r * r:
        return (0, 0, 0, 0)
    if inside_arrow(u, v):
        return (235, 240, 255, 255)
    return (int(30 + 60 * v), int(40 + 20 * v), int(120 + 90 * (1 - v)), 255)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
    png(sys.argv[1], size, pixel)


if __name__ == "__main__":
    main()
