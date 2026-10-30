#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Decruncher of the "ONYX" cruncher used by the one-disk Xenon 2 crunches (Vision Factory, Black Monks [h]).

A crunched block is: 4 parameter bytes, unpacked length (long), packed length (long), then the packed stream, which is
read BACKWARDS from its end while the output is written backwards from its end. The parameter bytes (file order
b0 b1 b2 b3) are bit widths of variable fields (b0: run-length fill count, b1: long literal run, b2: far match offset,
b3 low nibble: escape match length) and b3's high nibble holds three mode flags. The algorithm is a re-implementation written from a study of the decruncher stub that sits in the
crunched executables; it is verified byte for byte against the stub run on a 68000 core (see docs/re-notes/cracks.md).
No game data is stored here.
"""
import struct


class Crunch(Exception):
    pass


def decrunch(blob, offset=0):
    """blob[offset:] = params, unpacked length, packed length, stream. Returns the unpacked bytes."""
    b0, b1, b2, b3 = blob[offset:offset + 4]
    ulen, plen = struct.unpack_from(">II", blob, offset + 4)
    start = offset + 12
    if start + plen > len(blob) or ulen > 0x1000000:
        raise Crunch("block does not fit the file")
    # widths and flags, named as in the stub: p3 = b0, p2 = b1, p1 = b2, p0 = b3 & 15, d3 = b3 >> 4
    p3, p2, p1, p0, d3 = b0, b1, b2, b3 & 0xF, b3 >> 4
    pos = start + plen                      # a0: read pointer, pre-decremented
    out = bytearray(ulen)
    a1 = ulen                               # write pointer, pre-decremented (index into out)

    def rd():
        nonlocal pos
        if pos <= start:
            raise Crunch("read past the start of the stream")
        pos -= 1
        return blob[pos]

    d7 = rd()
    while d7 == 0:
        d7 = rd()
    x = 1                                    # move #$10,ccr
    while True:                              # find the sentinel bit
        carry = d7 & 1
        d7 = (x << 7) | (d7 >> 1)
        x = carry
        if d7 == 0:
            d7 = rd()
            while d7 == 0:
                d7 = rd()
            x = 1
            continue
        if carry:
            break

    def bit():
        nonlocal d7
        c = d7 & 1
        d7 >>= 1
        if d7 == 0:                          # that bit was the sentinel: refill
            d7 = rd()
            c = d7 & 1
            d7 = (1 << 7) | (d7 >> 1)
        return c

    def get(n):
        v = 0
        for _ in range(n & 0xFF):
            v = (v << 1) | bit()
        return v

    def copy_back(count, dist):
        nonlocal a1
        for _ in range(count):               # move.b -1(a1,d5.l),-(a1)
            a1 -= 1
            out[a1] = out[a1 + dist]

    def literal(count):
        nonlocal pos, a1
        for _ in range(count):
            a1 -= 1
            if pos <= start:
                raise Crunch("literal read past the start")
            pos -= 1
            out[a1] = blob[pos]

    def match_tail(d4, d5):                  # the stub's $120: distance = offset + length
        copy_back(d4, d5 + d4)

    d2 = 0
    while a1 > 0:
        take_match = False
        if d2 & 1:
            take_match = True
        d2 >>= 1
        if not take_match:
            if get(1) == 0:
                take_match = True
        if not take_match:
            # literal / run-length family
            if get(1) == 0:
                literal(1)
                d2 = 1
                continue
            n = get(2)
            if n != 3:
                literal(n + 1 + 1)
                d2 = 1
                continue
            n = get(2)
            if n != 3:
                literal(n + 4 + 1)
                d2 = 1
                continue
            n = get(3)
            if n != 7:
                literal(n + 7 + 1)
                d2 = 1
                continue
            d4, d2x = p2, 0x10E
            if d3 & 4:
                d2x = 0x0E
            else:
                if get(1) == 0:
                    d4, d2x = 8, 0x0E
            literal(get(d4) + d2x + 1)
            d2 = 1
            continue
        # match family
        if get(1) == 0:                      # short match: 2 bytes
            off = get(8)
            copy_back(2, off + 2)
            d2 = 0
            continue
        n = get(2)
        if n != 3:
            length = n + 3
        else:
            n = get(3)
            if n != 7:
                length = n + 6
            elif get(1) != 0:
                length = get(p0) + 13
            else:                            # run-length fill
                d4, d2x = 7, 5
                if d3 & 2:
                    d4 = p3
                elif get(1) == 0:
                    d2x, d4 = 0x85, p3
                cnt = get(d4) + d2x
                fill = get(8) & 0xFF
                for _ in range(cnt):
                    a1 -= 1
                    out[a1] = fill
                d2 = 0
                continue
        if get(1) == 0:
            off = get(6)
        elif not (d3 & 1):
            off = get(p1) + 0x40
        elif get(1) == 0:
            off = get(8) + 0x40
        else:
            off = get(p1) + 0x140
        match_tail(length, off)
        d2 = 0
    return bytes(out)


def main(argv):
    import sys
    if len(argv) != 3:
        print("usage: onyx_crunch.py CRUNCHED_FILE OUT_FILE [OFFSET]")
        return 2
    with open(argv[1], "rb") as f:
        data = f.read()
    with open(argv[2], "wb") as f:
        f.write(decrunch(data))
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
