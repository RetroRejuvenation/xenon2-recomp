#!/usr/bin/env python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Minimal ISO9660 reader for raw 2352-byte MODE1 BIN images.

usage: iso9660.py IMAGE.bin [--extract OUTDIR]
"""
import os
import struct
import sys

RAW = 2352
USER_OFF = 16  # sync(12) + header(4) for MODE1


def sector(f, lba):
    f.seek(lba * RAW)
    d = f.read(RAW)
    if len(d) < RAW:
        raise EOFError(lba)
    return d[USER_OFF:USER_OFF + 2048]


def read_extent(f, lba, size):
    out = bytearray()
    n = (size + 2047) // 2048
    for i in range(n):
        out += sector(f, lba + i)
    return bytes(out[:size])


def parse_dir(f, lba, size):
    data = read_extent(f, lba, size)
    pos = 0
    ents = []
    while pos < len(data):
        ln = data[pos]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        r = data[pos:pos + ln]
        pos += ln
        elba = struct.unpack_from('<I', r, 2)[0]
        esize = struct.unpack_from('<I', r, 10)[0]
        flags = r[25]
        nlen = r[32]
        name = r[33:33 + nlen]
        if name == b'\x00':
            name = '.'
        elif name == b'\x01':
            name = '..'
        else:
            name = name.decode('latin-1')
        ents.append((name, elba, esize, bool(flags & 2)))
    return ents


def walk(f, lba, size, path=''):
    for name, elba, esize, isdir in parse_dir(f, lba, size):
        if name in ('.', '..'):
            continue
        full = path + '/' + name
        yield full, elba, esize, isdir
        if isdir:
            yield from walk(f, elba, esize, full)


def main():
    img = sys.argv[1]
    out = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == '--extract' else None
    with open(img, 'rb') as f:
        pvd = sector(f, 16)
        assert pvd[1:6] == b'CD001', 'no PVD'
        print('volume:', pvd[40:72].decode('latin-1').strip(),
              'blocks:', struct.unpack_from('<I', pvd, 80)[0])
        root = pvd[156:190]
        rlba, rsize = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
        for full, lba, size, isdir in walk(f, rlba, rsize):
            print('%-40s lba=%-6d size=%-9d %s' % (full, lba, size, 'DIR' if isdir else ''))
            if out:
                dest = os.path.join(out, full.lstrip('/').replace(';1', ''))
                if isdir:
                    os.makedirs(dest, exist_ok=True)
                else:
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    with open(dest, 'wb') as o:
                        o.write(read_extent(f, lba, size))


if __name__ == '__main__':
    main()
