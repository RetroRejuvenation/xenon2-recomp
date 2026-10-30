#!/usr/bin/env python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Unpack the crunched XENON-II executable from disk 1 (backward bit-stream decruncher
transliterated from the 310-byte stub at the start of the code hunk).

usage: unpack_xenon2_disk.py XENON-II OUT.bin
Output is the memory image that the stub decrunches to $30000..$764ca; the stub then
jumps to $70000, i.e. offset 0x40000 in OUT.bin.
"""
import struct
import sys


def unpack(code):
    pos = 0x136 + 0x32260  # a0 = pc-rel base + constant, then pre-decremented
    state = {'d0': 0, 'pos': pos}

    def rd_long():
        state['pos'] -= 4
        return struct.unpack_from('>I', code, state['pos'])[0]

    def getbit():
        d0 = state['d0']
        carry = d0 & 1
        d0 >>= 1
        if d0 == 0:  # refill: X=1 sentinel into bit 31
            d0 = rd_long()
            carry = d0 & 1
            d0 = (d0 >> 1) | 0x80000000
        state['d0'] = d0
        return carry

    def bits(n):
        v = 0
        for _ in range(n):
            v = (v << 1) | getbit()
        return v

    length = rd_long()
    out = bytearray(length)
    a2 = length
    state['d0'] = rd_long()
    while True:
        d2 = bits(3)
        if d2:
            if d2 == 7:
                if getbit():
                    d2 = bits(10)
                    if d2 == 0:
                        d2 = bits(18)
                else:
                    d2 = bits(4) + 7
            for _ in range(d2):
                a2 -= 1
                out[a2] = bits(8)
        if a2 <= 0:
            break
        sel = bits(2)
        if sel == 0:
            d3, d1 = 2, 8
        elif sel == 1:
            d3, d1 = 3, (8 if getbit() else 14)
        else:
            if sel == 2:
                d3 = 4
            else:
                sub = bits(2)
                if sub == 3:
                    d3 = bits(8)
                elif sub == 2:
                    d3 = bits(2) + 7
                else:
                    d3 = sub + 5
            if not getbit():
                d1 = 16
            else:
                d1 = 8 if getbit() else 12
        off = bits(d1)
        for _ in range(d3):
            a2 -= 1
            out[a2] = out[a2 + off]
    return bytes(out)


def main():
    d = open(sys.argv[1], 'rb').read()
    code = d[36:36 + struct.unpack_from('>I', d, 32)[0] * 4]
    img = unpack(code)
    open(sys.argv[2], 'wb').write(img)
    print('unpacked', len(img), 'bytes')


if __name__ == '__main__':
    main()
