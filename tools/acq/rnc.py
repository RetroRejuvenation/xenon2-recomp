#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Rob Northen Compression (RNC "ProPack") method 1 decoder, written from the public format description and checked
# against the CRC16s every stream carries in its header (a stream only counts as decoded when both its packed and
# its unpacked CRC16 match).
#
# Header (18 bytes, big-endian): "RNC" 01, unpacked length (4), packed length (4), CRC16 of the unpacked data (2),
# CRC16 of the packed data (2), leeway (1), chunk count (1). Then an LSB-first bit stream of 16-bit little-endian
# words: 2 bits skipped; per chunk three Huffman tables (raw lengths, distances, match lengths: 5-bit leaf count,
# 4-bit code lengths, canonical codes read MSB-first), a 16-bit sub-chunk count, then for each sub-chunk a raw literal
# run (length from the raw table, the literal bytes follow the consumed bit-stream words directly) and, except in
# the last one, a match (distance + 1 and length + 2). A symbol >= 2 is followed by symbol-1 extra bits.
#
#   python3 tools/acq/rnc.py IMAGE [--all]     # find and verify every RNC stream in a file (hashes only)
import hashlib
import re
import struct
import sys

HEADER = 18


def crc16(data):
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        table.append(c)
    crc = 0
    for b in data:
        crc = (crc >> 8) ^ table[(crc ^ b) & 0xFF]
    return crc


class RncError(Exception):
    pass


class _Bits:
    def __init__(self, data, pos):
        self.d = data
        self.p = pos
        self.buf = 0
        self.cnt = 0

    def _fill(self, n):
        while self.cnt < n:
            if self.p + 1 >= len(self.d):
                raise RncError("bit stream runs off the end of the data")
            self.buf |= (self.d[self.p] | (self.d[self.p + 1] << 8)) << self.cnt
            self.p += 2
            self.cnt += 16

    def peek(self, n):
        self._fill(n)
        return self.buf & ((1 << n) - 1)

    def adv(self, n):
        self.buf >>= n
        self.cnt -= n

    def read(self, n):
        if n == 0:
            return 0
        v = self.peek(n)
        self.adv(n)
        return v


def _mirror(v, n):
    r = 0
    for _ in range(n):
        r = (r << 1) | (v & 1)
        v >>= 1
    return r


def _table(b):
    num = b.read(5)
    if not num:
        return []
    lens = [b.read(4) for _ in range(num)]
    out = []
    code = 0
    for length in range(1, 17):
        for sym, ln in enumerate(lens):
            if ln == length:
                out.append((length, _mirror(code, length), sym))
                code += 1
        code <<= 1
    return out


def _symbol(b, table):
    for length, code, sym in table:
        if b.peek(length) == code:
            b.adv(length)
            if sym >= 2:
                sym = (1 << (sym - 1)) | b.read(sym - 1)
            return sym
    raise RncError("no Huffman code matches")


def parse_header(d, off):
    if d[off:off + 4] != b"RNC\x01":
        raise RncError("not an RNC method 1 stream")
    usz, psz, ucrc, pcrc, leeway, chunks = struct.unpack(">IIHHBB", d[off + 4:off + 18])
    return dict(unpacked=usz, packed=psz, unpacked_crc=ucrc, packed_crc=pcrc, leeway=leeway, chunks=chunks)


def unpack(d, off, max_unpacked=64 << 20):
    h = parse_header(d, off)
    if h["unpacked"] > max_unpacked or off + HEADER + h["packed"] > len(d):
        raise RncError("sizes out of range")
    b = _Bits(d, off + HEADER)
    b.read(2)
    out = bytearray()
    while len(out) < h["unpacked"]:
        raw, dist, ln = _table(b), _table(b), _table(b)
        count = b.read(16)
        while True:
            n = _symbol(b, raw) if raw else 0
            if n:
                if b.p + n > len(d):
                    raise RncError("literal run off the end")
                out += d[b.p:b.p + n]
                b.p += n
            count -= 1
            if count <= 0:
                break
            dd = _symbol(b, dist) + 1
            length = _symbol(b, ln) + 2
            if dd > len(out) or len(out) + length > h["unpacked"] + 0x10000:
                raise RncError("match out of range")
            for _ in range(length):
                out.append(out[-dd])
    return bytes(out[:h["unpacked"]]), h


def verify(d, off):
    """(ok, header, unpacked) - ok only when the packed and the unpacked CRC16 both match."""
    h = parse_header(d, off)
    if off + HEADER + h["packed"] > len(d):
        return False, h, b""
    if crc16(d[off + HEADER:off + HEADER + h["packed"]]) != h["packed_crc"]:
        return False, h, b""
    try:
        out, _ = unpack(d, off)
    except (RncError, IndexError):
        return False, h, b""
    return crc16(out) == h["unpacked_crc"], h, out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as fh:
        d = fh.read()
    n_ok = 0
    for m in re.finditer(rb"RNC\x01", d):
        ok, h, out = verify(d, m.start())
        n_ok += ok
        print("RNC @%#x unpacked %d packed %d: %s%s" % (m.start(), h["unpacked"], h["packed"], "CRCs OK" if ok else "not verified",
              " sha1 " + hashlib.sha1(out).hexdigest() if ok else ""))
    return 0 if n_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
