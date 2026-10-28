#!/usr/bin/env python3
"""Depackers for the two cruncher variants found on Xenon 2 disc 1.

BS1 variant ('bytekiller-like', 3-long header: packed_len, unpacked_len, crc):
    python3 -I tools/bkunpack.py bs1 BLOB_FILE HEADER_OFFSET OUT [--expect-crc]
XENON-II variant (backward 'ice/time-cruncher-like' stream, trailing unpacked_len long):
    python3 -I tools/bkunpack.py x2 BLOB_FILE START_OFF END_OFF OUT
Both depackers run backwards over the stream, writing output from the top down.
"""
import struct
import sys


class BitSrc:
    """Backward long reader with lsb-first bit extraction and sentinel bit refill."""

    def __init__(self, buf, end, crc=None):
        self.buf = buf
        self.pos = end
        self.crc = 0 if crc is None else crc
        self.d0 = 0

    def long(self):
        self.pos -= 4
        v = struct.unpack_from(">I", self.buf, self.pos)[0]
        self.crc ^= v
        return v

    def refill(self):
        v = self.long()
        c = v & 1
        self.d0 = (v >> 1) | 0x80000000
        return c

    def bit(self):
        c = self.d0 & 1
        self.d0 >>= 1
        if self.d0 == 0:
            c = self.refill()
        return c

    def bits(self, n):
        r = 0
        for _ in range(n):
            r = (r << 1) | self.bit()
        return r


def bs1(blob, hdr):
    plen, ulen, crc = struct.unpack_from(">III", blob, hdr)
    end = hdr + 12 + plen
    s = BitSrc(blob, end, crc)
    s.d0 = s.long()
    out = bytearray(ulen)
    a2 = ulen
    while a2 > 0:
        if s.bit():
            n = s.bits(2)
            if n < 2:
                nb, cnt = 9 + n, n + 3
                off = s.bits(nb)
                for _ in range(cnt):
                    a2 -= 1
                    out[a2] = out[a2 + off] if a2 + off < ulen else 0
            elif n == 3:
                cnt = s.bits(8) + 8 + 1
                for _ in range(cnt):
                    a2 -= 1
                    out[a2] = s.bits(8)
            else:
                cnt = s.bits(8) + 1
                off = s.bits(12)
                for _ in range(cnt):
                    a2 -= 1
                    out[a2] = out[a2 + off]
        else:
            if s.bit():
                off = s.bits(8)
                for _ in range(2):
                    a2 -= 1
                    out[a2] = out[a2 + off]
            else:
                cnt = s.bits(3) + 1
                for _ in range(cnt):
                    a2 -= 1
                    out[a2] = s.bits(8)
    return bytes(out), s.crc, s.pos, hdr + 12


def x2(blob, start, end):
    """XENON-II stream: end-4 holds unpacked length, below that the bit stream (backward)."""
    s = BitSrc(blob, end)
    ulen = s.long()
    s.crc = 0
    s.d0 = s.long()
    out = bytearray(ulen)
    a2 = ulen

    def getbits(n):
        # d0 is lsr'd per bit; beq -> refill (carry via X=1 trick: sentinel-shifted)
        return s.bits(n)

    while True:
        # literal run header
        d2 = getbits(3)
        if d2 != 0:
            if d2 == 7:
                if not s.bit():
                    d2 = getbits(4) + 7
                else:
                    d2 = getbits(10)
                    if d2 == 0:
                        d2 = getbits(18)
            d2 -= 1
            for _ in range(d2 + 1):
                a2 -= 1
                out[a2] = s.bits(8)
        if a2 <= 0:
            break
        d2 = getbits(2)
        d3 = 2
        d1 = 8
        if d2 == 0:
            pass
        else:
            if d2 == 2:
                d3 = 4
                d1 = 8
                d1 = _long_off(s, d1)
            elif d2 == 1:
                d3 = 3
                d1 = 8
                d1 = 14 if s.bit() == 0 else 8
            else:
                d2b = getbits(2)
                if d2b == 3:
                    d3 = getbits(8)
                elif d2b == 2:
                    d3 = getbits(2) + 7
                else:
                    d3 = d2b + 5
                d1 = _long_off(s, 16)
        if d2 == 0:
            d1 = 8
        off = getbits(d1)
        for _ in range(d3):
            a2 -= 1
            out[a2] = out[a2 + off]
    return bytes(out), a2


def _long_off(s, d1):
    # $300e0: d1=$10; bit0 -> keep 16 ; else bit -> 8 else 12
    d1 = 16
    if s.bit():
        d1 = 8
        if not s.bit():
            d1 = 12
    return d1


if __name__ == "__main__":
    a = sys.argv
    if a[1] == "bs1":
        blob = open(a[2], "rb").read()
        out, crc, pos, lo = bs1(blob, int(a[3], 0))
        print("unpacked %d bytes, crc residue %08x, stream stopped at %d (low bound %d)" % (len(out), crc, pos, lo))
        open(a[4], "wb").write(out)
    elif a[1] == "x2":
        blob = open(a[2], "rb").read()
        out, a2 = x2(blob, int(a[3], 0), int(a[4], 0))
        print("unpacked %d bytes, a2 final %d" % (len(out), a2))
        open(a[5], "wb").write(out)
