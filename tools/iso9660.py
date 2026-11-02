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


def vis(s):
    """Printable form of an image-supplied name: control characters are shown escaped."""
    return "".join(c if c.isprintable() else "\\x%02x" % ord(c) for c in s)


def clean_name(name):
    """Drop a trailing ';version' suffix (digits only); other text is kept as is."""
    if ';' in name:
        base, ver = name.rsplit(';', 1)
        if ver.isdigit():
            return base
    return name


def plain_name(name):
    """True when an image-supplied component is a plain file name (no traversal, no separators)."""
    return not (not name or name in ('.', '..') or any(c in name for c in '/\\:\0') or any(ord(c) < 32 for c in name))


def walk(f, lba, size, path='', parts=(), seen=None):
    """Yield (path, lba, size, is_dir, parts); parts are the cleaned name components. A directory extent
    that is reachable again (a loop or a shared extent) is not descended into twice."""
    if seen is None:
        seen = {lba}
    for name, elba, esize, isdir in parse_dir(f, lba, size):
        if name in ('.', '..'):
            continue
        full = path + '/' + name
        p = tuple(parts) + (clean_name(name),)
        yield full, elba, esize, isdir, p
        if isdir:
            if elba in seen:
                continue
            seen.add(elba)
            yield from walk(f, elba, esize, full, p, seen)


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
        out_root = os.path.realpath(out) if out else None
        for full, lba, size, isdir, parts in walk(f, rlba, rsize):
            print('%-40s lba=%-6d size=%-9d %s' % (vis(full), lba, size, 'DIR' if isdir else ''))
            if out:
                if not all(plain_name(p) for p in parts):
                    print('skipped %s: the name is not a plain file name (traversal or separator in image data)' % vis(full))
                    continue
                dest = os.path.join(out_root, *parts)
                if os.path.commonpath([os.path.realpath(dest), out_root]) != out_root:
                    print('skipped %s: it resolves outside the output directory' % vis(full))
                    continue
                if isdir:
                    os.makedirs(dest, exist_ok=True)
                else:
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    with open(dest, 'wb') as o:
                        o.write(read_extent(f, lba, size))


if __name__ == '__main__':
    main()
