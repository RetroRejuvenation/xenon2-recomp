#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Acorn ArcFS archive reader (RISC OS). Header "Archive\0": directory size, data start, versions; 36-byte entries
# (method byte, 11-char name, original length, load, exec, attributes, compressed length, data offset or
# 0x80000000 | subdirectory offset). Method 0x82 = stored, 0x88 = "crunched": Unix-compress-style LZW (LSB-first, 9 to
# 16 bits, code 256 = clear) followed by Spark-style run-length decoding (0x90 n repeats the previous byte n-1 more
# times, 0x90 0 is a literal 0x90). Output is trimmed to the original length (the stream pads by a byte or two).
#
#   python3 tools/acq/arcfs.py ARCHIVE [--extract DIR]    # list (name, size, method, sha1); optional extraction
import hashlib
import os
import struct
import sys


def lzw(data, maxbits=16):
    out = bytearray()
    nb, free = 9, 257
    prefix = [0] * (1 << maxbits)
    suffix = list(range(256)) + [0] * ((1 << maxbits) - 256)
    prev = None
    bitpos, nbits = 0, len(data) * 8

    def string(code):
        s = []
        while code >= 256:
            s.append(suffix[code])
            code = prefix[code]
        s.append(code)
        s.reverse()
        return s

    while bitpos + nb <= nbits:
        v = 0
        for i in range(nb):
            b = bitpos + i
            v |= ((data[b >> 3] >> (b & 7)) & 1) << i
        bitpos += nb
        if v == 256:
            free, nb, prev = 257, 9, None
            continue
        if prev is None:
            out.append(v & 0xFF)
            prev = v
            continue
        if v < free:
            s = string(v)
        elif v == free:
            s = string(prev)
            s.append(s[0])
        else:
            break
        out += bytes(s)
        if free < (1 << maxbits):
            prefix[free] = prev
            suffix[free] = s[0]
            free += 1
            if free == (1 << nb) and nb < maxbits:
                nb += 1
        prev = v
    return bytes(out)


def rle(b):
    out = bytearray()
    last = 0
    i = 0
    while i < len(b):
        c = b[i]
        i += 1
        if c == 0x90:
            if i >= len(b):
                break
            n = b[i]
            i += 1
            if n == 0:
                out.append(0x90)
                last = 0x90
            else:
                out += bytes([last]) * (n - 1)
        else:
            out.append(c)
            last = c
    return bytes(out)


def entries(d, off, limit=100000):
    out = []
    while len(out) < limit:
        e = d[off:off + 36]
        if len(e) < 36 or e[0] == 0:
            break
        name = e[1:12].split(b"\0")[0].decode("latin1")
        origlen, load, exe, attr, comp, idx = struct.unpack("<6I", e[12:36])
        out.append(dict(method=e[0], name=name, orig=origlen, load=load, exec=exe, comp=comp, idx=idx))
        off += 36
    return out


def walk(d, max_depth=8):
    if d[:8] != b"Archive\0":
        raise ValueError("not an ArcFS archive")
    dirsize, datastart = struct.unpack("<II", d[8:16])
    files = []

    def rec(off, path, depth):
        for e in entries(d, off):
            if e["idx"] & 0x80000000:
                if depth < max_depth:
                    rec(0x60 + (e["idx"] & 0x7FFFFFFF), path + e["name"] + "/", depth + 1)
            else:
                e["path"] = path + e["name"]
                e["data_off"] = datastart + e["idx"]
                files.append(e)

    rec(0x60, "", 0)
    return files


def extract_file(d, e):
    raw = d[e["data_off"]:e["data_off"] + e["comp"]]
    if e["method"] == 0x82:
        out = raw
    elif e["method"] == 0x88:
        out = rle(lzw(raw))[:e["orig"]]
    else:
        raise ValueError("unsupported ArcFS method %#x" % e["method"])
    if len(out) != e["orig"]:
        raise ValueError("%s: length %d, expected %d" % (e["path"], len(out), e["orig"]))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as fh:
        d = fh.read()
    dest = argv[argv.index("--extract") + 1] if "--extract" in argv else None
    bad = 0
    for e in walk(d):
        try:
            data = extract_file(d, e)
            print("%-60s %8d  method %02x  sha1 %s" % (e["path"], e["orig"], e["method"], hashlib.sha1(data).hexdigest()))
            if dest:
                os.makedirs(dest, exist_ok=True)
                open(os.path.join(dest, e["path"].replace("/", "__")), "wb").write(data)
        except ValueError as ex:
            bad += 1
            print("%-60s FAILED: %s" % (e["path"], ex))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
