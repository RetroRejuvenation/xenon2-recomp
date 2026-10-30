#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Depacker for the Ice-style cruncher found in the "Vision Factory [a]" Xenon 2 executable.

Layout of a block: unpacked length (long), packed length (long), then the packed stream, which is read backwards from
its end as 16-bit words (a bit queue consumed from the low end of a 32-bit register) while the output is written
backwards from its end. Literal runs carry their bytes inside the bit stream. Written from a study of the
decruncher stub in that executable; verified against the stub run on a 68000 core (docs/re-notes/cracks.md).
No game data is stored here.
"""
import struct


class Ice(Exception):
    pass


def depack(blob, stream_start, ulen, plen):
    """The packed stream is blob[stream_start:stream_start+plen]. Returns ulen unpacked bytes."""
    if stream_start + plen > len(blob) or plen < 6 or ulen > 0x1000000:
        raise Ice("block does not fit the file")
    out = bytearray(ulen)
    a1 = ulen
    a2 = stream_start + plen
    over = 0

    def word():
        nonlocal a2
        nonlocal over
        a2 -= 2
        if a2 < stream_start:               # the stub's last refill reads one word ahead, in front of the stream
            over += 1
            if over > 4:
                raise Ice("read past the start of the stream")
            return 0
        return (blob[a2] << 8) | blob[a2 + 1]

    if plen < 8:
        raise Ice("stream too short")
    a2 -= 2
    d0 = (blob[a2] << 8) | blob[a2 + 1]            # move.w -(a2),d0
    a2 -= 4
    d6 = struct.unpack_from(">I", blob, a2)[0]      # move.l -(a2),d6
    d7 = 0x10 - d0
    d6 >>= d7 & 63
    d7 = d0
    d3 = 0x10

    def get1():
        nonlocal d6, d7
        d7 = (d7 - 1) & 0xFFFF
        if d7 != 0:
            c = d6 & 1
            d6 >>= 1
            return c
        d7 = 0x10
        low = d6 & 0xFFFF
        d6 >>= 1
        d6 = ((d6 & 0xFFFF) << 16) | (d6 >> 16)         # swap
        d6 = (d6 & 0xFFFF0000) | word()                  # move.w -(a2),d6
        d6 = ((d6 & 0xFFFF) << 16) | (d6 >> 16)         # swap
        return low & 1

    def getn(n):
        nonlocal d6, d7
        v = d6 & 0xFFFF
        d6 >>= n
        d7 = (d7 - n) & 0xFFFF
        s = d7 - 0x10000 if d7 & 0x8000 else d7
        if s <= 0:
            d7 = (d7 + d3) & 0xFFFF
            r = d7 & 31
            d6 = ((d6 >> r) | (d6 << (32 - r))) & 0xFFFFFFFF if r else d6      # ror.l d7,d6
            d6 = (d6 & 0xFFFF0000) | word()
            d6 = ((d6 << r) | (d6 >> (32 - r))) & 0xFFFFFFFF if r else d6      # rol.l d7,d6
        return v & ((1 << n) - 1)

    def lits(count):
        nonlocal a1
        for _ in range(count):
            a1 -= 1
            out[a1] = getn(8)

    while a1 > 0:
        if get1():                       # literal run
            lits(1)
            continue
        if get1():
            if get1():
                if get1():
                    d1, d4 = 8, 0x17
                else:
                    d1, d4 = 4, 7
            else:
                d1, d4 = 2, 3
        else:
            d1, d4 = 1, 1
        d4 += getn(d1)
        if d4 == 0x16:                   # long literal run
            d4 = 14
            d1 = 5 if get1() else 14
            d4 += getn(d1)
            lits(d4 + 1)
            continue
        if d4 > 0x16:
            d4 -= 1
        if not get1():
            d1, d2 = 9, 0x20
        elif not get1():
            d1, d2 = 5, 0
        else:
            d1, d2 = 14, 0x220
        off = getn(d1) + d2
        for _ in range(d4 + 1):
            a1 -= 1
            out[a1] = out[a1 + off]
        # note: (a1,d0.w) is taken from the position before the first byte; the stub's -(a3) walk
    return bytes(out)
