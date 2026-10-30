#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Extract the game image from a decoded RETAIL Xenon 2 disk 1 (an ADF made by tools/mfm.py from an IPF, or any
ADF/flux decode of an unprotected-by-us retail disk) and compare it with the canonical game image.

On the retail disk 1 the game image ($3FC00 bytes, the same size as the canonical image) is stored at disk offset
$70C00 (track 80 onwards) as a 16-bit-word XOR chain: stored[k] = plain[k] ^ plain[k-1], with plain[-1] = $002A.
Undoing it is pure arithmetic: it needs neither the Copylock key track nor any emulation.

usage: python3 tools/retail_game_image.py DISK1.adf [CANONICAL_game_0400.bin]
  prints the decoded image's size and sha1; with a canonical image, every range where the two differ.
Nothing is written; no game data leaves this process.
"""
import hashlib
import sys

IMAGE_OFFSET = 0x70C00
IMAGE_SIZE = 0x3FC00
CHAIN_INIT = 0x002A


def chain_decode(stored, init=CHAIN_INIT):
    out = bytearray(len(stored))
    prev = init
    for i in range(0, len(stored) - 1, 2):
        w = ((stored[i] << 8) | stored[i + 1]) ^ prev
        out[i] = w >> 8
        out[i + 1] = w & 0xFF
        prev = w
    return bytes(out)


def chain_encode(plain, init=CHAIN_INIT):
    out = bytearray(len(plain))
    prev = init
    for i in range(0, len(plain) - 1, 2):
        w = (plain[i] << 8) | plain[i + 1]
        e = w ^ prev
        out[i] = e >> 8
        out[i + 1] = e & 0xFF
        prev = w
    return bytes(out)


def extract(adf):
    if len(adf) != 901120:
        raise ValueError("not an 880 KB ADF")
    return chain_decode(adf[IMAGE_OFFSET:IMAGE_OFFSET + IMAGE_SIZE])


def diff_ranges(a, b, gap=16):
    diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    ranges = []
    start = prev = None
    for i in diff:
        if start is None:
            start = prev = i
        elif i <= prev + gap:
            prev = i
        else:
            ranges.append((start, prev + 1))
            start = prev = i
    if start is not None:
        ranges.append((start, prev + 1))
    return ranges


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__)
        return 2
    adf = open(argv[1], "rb").read()
    img = extract(adf)
    print("decoded retail image: %d bytes, sha1 %s" % (len(img), hashlib.sha1(img).hexdigest()))
    if len(argv) == 3:
        canon = open(argv[2], "rb").read()
        rs = diff_ranges(img, canon)
        total = sum(e - s for s, e in rs)
        print("canonical image: %d bytes, sha1 %s" % (len(canon), hashlib.sha1(canon).hexdigest()))
        print("%d differing ranges, %d bytes in all (addresses are game addresses, image base $400)" % (len(rs), total))
        for s, e in rs:
            print("  $%05x  %4d bytes" % (s + 0x400, e - s))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
