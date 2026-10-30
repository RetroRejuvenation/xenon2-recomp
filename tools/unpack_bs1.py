#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Native unpacker for disc 1 file 'BS1' (backwards bit-cruncher with XOR checksum).

Single chip CODE hunk: decruncher at code $000-$0E7, header {packed_len, unpacked_len, checksum}
at $0E8, packed stream after it. Decrunches to $30000 and JMPs there.
Usage: unpack_bs1.py BS1 out.bin    (out.bin = unpacked image, base $30000)
"""
import struct
import sys


def code_hunk(d):
    nh = struct.unpack('>I', d[8:12])[0]
    p = 20 + 4 * nh
    assert struct.unpack('>I', d[p:p + 4])[0] == 0x3E9
    n = struct.unpack('>I', d[p + 4:p + 8])[0]
    return d[p + 8:p + 8 + n * 4]


def unpack(code):
    packed_len, out_len, chk = struct.unpack('>III', code[0xE8:0xF4])
    pos = 0xF4 + packed_len
    out = bytearray(out_len)
    a2 = out_len
    state = {'chk': chk}

    def rl():
        nonlocal pos
        pos -= 4
        v = struct.unpack('>I', code[pos:pos + 4])[0]
        state['chk'] ^= v
        return v

    d0 = rl()

    def bit():
        nonlocal d0
        c = d0 & 1
        d0 >>= 1
        if d0 == 0:
            d0 = rl()
            c = d0 & 1
            d0 = (d0 >> 1) | 0x80000000
        return c

    def bits(n):
        v = 0
        for _ in range(n):
            v = (v << 1) | bit()
        return v

    def lit(cnt):
        nonlocal a2
        for _ in range(cnt):
            a2 -= 1
            out[a2] = bits(8)

    def copy(cnt, nb):
        nonlocal a2
        off = bits(nb)
        for _ in range(cnt):
            a2 -= 1
            out[a2] = out[a2 + off]

    while a2 > 0:
        if bit() == 0:
            if bit():
                copy(2, 8)
            else:
                lit(bits(3) + 1)
        else:
            k = bits(2)
            if k < 2:
                copy(k + 3, 9 + k)
            elif k == 3:
                lit(bits(8) + 8 + 1)
            else:
                copy(bits(8) + 1, 12)
    return bytes(out), state['chk']


if __name__ == '__main__':
    img, chk = unpack(code_hunk(open(sys.argv[1], 'rb').read()))
    open(sys.argv[2], 'wb').write(img)
    print('unpacked %#x bytes, base $30000, checksum residue %#x (0 = ok)' % (len(img), chk))
