#!/usr/bin/env python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Lane C helper: hunk parser, track survey for the CDTV disc image, minimal OFS reader."""
import os
import struct
import sys


def hunks(path):
    d = open(path, 'rb').read()
    p = 0
    assert struct.unpack_from('>I', d, 0)[0] == 0x3f3
    p = 4
    while struct.unpack_from('>I', d, p)[0]:
        n = struct.unpack_from('>I', d, p)[0]; p += 4 + n * 4
    p += 4
    nh, first, last = struct.unpack_from('>III', d, p); p += 12
    sizes = struct.unpack_from('>%dI' % (last - first + 1), d, p); p += 4 * (last - first + 1)
    out = []
    for i in range(last - first + 1):
        start = None
        info = {'type': 0, 'alloc': (sizes[i] & 0x3fffffff) * 4, 'sections': []}
        while True:
            ty = struct.unpack_from('>I', d, p)[0] & 0x3fffffff; p += 4
            if ty in (0x3e9, 0x3ea, 0x3e8):
                info['type'] = ty
                n = struct.unpack_from('>I', d, p)[0]; p += 4
                info['sections'].append((ty, p, n * 4)); info['code_off'] = p; info['code_len'] = n * 4
                p += n * 4
            elif ty == 0x3eb:
                n = struct.unpack_from('>I', d, p)[0]; p += 4; info['bss'] = n * 4
            elif ty == 0x3ec:
                nrel = 0
                while True:
                    n = struct.unpack_from('>I', d, p)[0]; p += 4
                    if n == 0: break
                    p += 4 + n * 4; nrel += n
                info['relocs'] = nrel
            elif ty == 0x3f0:  # symbol
                while True:
                    n = struct.unpack_from('>I', d, p)[0]; p += 4
                    if n == 0: break
                    p += (n & 0xffffff) * 4 + 4
            elif ty == 0x3f2:
                break
            else:
                info['unk'] = hex(ty); break
        out.append(info)
    return d, out


def main():
    d, hs = hunks(sys.argv[1])
    for i, h in enumerate(hs):
        print(i, {k: v for k, v in h.items() if k != 'sections'})


if __name__ == '__main__':
    main()
