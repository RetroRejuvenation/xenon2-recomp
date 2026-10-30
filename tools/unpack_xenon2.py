#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Native unpacker for the Xenon 2 'XENON-II' loader (ByteKiller-style backwards cruncher).

The AmigaDOS file is a single chip CODE hunk holding a tiny decruncher (code offsets
$000-$135) followed by the packed stream (read backwards from code offset $32396).
The decruncher unpacks to $30000 and then does JMP $70000.
Usage: unpack_xenon2.py XENON-II out.bin   (out.bin = unpacked image, base $30000)
"""
import struct
import sys

PACK_START = 2 + 0x134      # lea $134(pc),a0
PACK_END = PACK_START + 0x32260   # adda.l #$32260,a0


def code_hunk(d):
    assert struct.unpack('>I', d[:4])[0] == 0x3F3
    nh = struct.unpack('>I', d[8:12])[0]
    p = 20 + 4 * nh
    assert struct.unpack('>I', d[p:p + 4])[0] == 0x3E9
    n = struct.unpack('>I', d[p + 4:p + 8])[0]
    return d[p + 8:p + 8 + n * 4]


def unpack(code):
    pos = PACK_END

    def rl():
        nonlocal pos
        pos -= 4
        return struct.unpack('>I', code[pos:pos + 4])[0]

    size = rl()
    out = bytearray(size)
    a2 = size
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

    while True:
        n = bits(3)
        if n:
            if n == 7:
                if bit() == 0:
                    n = bits(4) + 7
                else:
                    n = bits(10)
                    if n == 0:
                        n = bits(0x12)
            for _ in range(n):
                a2 -= 1
                out[a2] = bits(8)
        if a2 <= 0:
            break
        k = bits(2)
        if k == 0:
            ln, nb = 2, 8
        elif k == 1:
            ln = 3
            nb = 8 if bit() else 14
        else:
            if k == 2:
                ln = 4
            else:
                j = bits(2)
                ln = bits(8) if j == 3 else (bits(2) + 7 if j == 2 else j + 5)
            if bit() == 0:
                nb = 16
            else:
                nb = 8 if bit() else 12
        off = bits(nb)
        for _ in range(ln):
            out[a2 - 1] = out[a2 - 1 + off]
            a2 -= 1
    return bytes(out)


if __name__ == '__main__':
    img = unpack(code_hunk(open(sys.argv[1], 'rb').read()))
    open(sys.argv[2], 'wb').write(img)
    print('unpacked %#x bytes, base $30000, entry $70000' % len(img))
