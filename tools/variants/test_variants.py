# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Tests for tools/variants/ using only synthetic data (no game bytes exist in this repository).

  python3 -I -m unittest discover -s tools/variants -p 'test_*.py'
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as c  # noqa: E402
import speedlock_v6 as sl  # noqa: E402

M32 = 0xFFFFFFFF
M55 = 0x55555555


def bits_of(value, n):
    return [(value >> (n - 1 - i)) & 1 for i in range(n)]


def pack_stream(bitlist, msb_first):
    """Pack decode-order bits into longs the way the crunchers read them. The first long read carries up to 31 bits
    with a sentinel one just above the last (the reader reloads when what is left of it is zero); every later long
    carries 32 bits (the reload itself supplies the sentinel). The first long read is the highest-addressed one."""
    longs = []
    pos = 0
    first = True
    while pos < len(bitlist) or first:
        n = min(31 if first else 32, len(bitlist) - pos)
        chunk = bitlist[pos:pos + n]
        pos += n
        v = 0
        if msb_first:
            for j, b in enumerate(chunk):
                v |= b << (31 - j)
            if first:
                v |= 1 << (31 - n)
        else:
            for j, b in enumerate(chunk):
                v |= b << j
            if first:
                v |= 1 << n
        longs.append(v)
        first = False
    return b"".join(struct.pack(">I", x) for x in reversed(longs))


def encode(tokens):
    """tokens (in decode order = from the END of the output towards its start):
    ('lit', [bytes in decode order]) one run of 1-8 bytes, ('long', [bytes]) a run of 9-264,
    ('copy2', offset8) length 2, ('copy', sel, offset) length sel+3 (sel 0/1), ('copyn', n, offset8) length n+1."""
    out = []

    def put(v, n):
        out.extend(bits_of(v, n))

    for t in tokens:
        if t[0] == "lit":
            assert 1 <= len(t[1]) <= 8
            put(0, 1); put(0, 1); put(len(t[1]) - 1, 3)
            for b in t[1]:
                put(b, 8)
        elif t[0] == "long":
            assert 9 <= len(t[1]) <= 264
            put(1, 1); put(3, 2); put(len(t[1]) - 9, 8)
            for b in t[1]:
                put(b, 8)
        elif t[0] == "copy2":
            put(0, 1); put(1, 1); put(t[1], 8)
        elif t[0] == "copy":
            put(1, 1); put(t[1], 2); put(t[2], 8 + t[1])
        elif t[0] == "copyn":
            put(1, 1); put(2, 2); put(t[1], 8); put(t[2], 8)
    return out


def simulate(tokens, total):
    """Independent model of what the tokens produce, filling the output from its end."""
    out = bytearray(total)
    a = total
    for t in tokens:
        if t[0] in ("lit", "long"):
            for b in t[1]:
                a -= 1
                out[a] = b
        else:
            if t[0] == "copy2":
                count, off = 2, t[1]
            elif t[0] == "copy":
                count, off = t[1] + 3, t[2]
            else:
                count, off = t[1] + 1, t[2]
            for _ in range(count):
                a -= 1
                out[a] = out[a + off] if a + off < total else 0
    assert a == 0, "tokens must exactly fill the output"
    return bytes(out)


TOKENS = [
    ("lit", [1, 2, 3, 4, 5, 6, 7, 8]),
    ("copy2", 3),
    ("long", list(range(100, 112))),
    ("copy", 0, 5),
    ("copy", 1, 20),
    ("copyn", 9, 7),
    ("lit", [0xAA]),
]
TOTAL = 8 + 2 + 12 + 3 + 4 + 10 + 1


class CrunchTests(unittest.TestCase):
    def test_msb_flavour(self):
        want = simulate(TOKENS, TOTAL)
        packed = pack_stream(encode(TOKENS), msb_first=True)
        got, consumed = sl.decrunch_msb(packed, TOTAL)
        self.assertEqual(got, want)
        self.assertTrue(consumed)

    def test_lsb_flavour(self):
        want = simulate(TOKENS, TOTAL)
        packed = pack_stream(encode(TOKENS), msb_first=False)
        got, consumed = sl.decrunch_lsb(packed, TOTAL)
        self.assertEqual(got, want)
        self.assertTrue(consumed)

    def test_truncated_stream_raises(self):
        packed = pack_stream(encode(TOKENS), msb_first=False)
        with self.assertRaises(ValueError):
            sl.decrunch_lsb(packed[8:], TOTAL)

    def test_unrle_exact(self):
        marker = 0xEE
        # output (from the end): 4 x 0x41 (count 4), then literal 0x42, 0x43
        src = bytes([0x43, 0x42, 4, 0x41, marker])
        out, left = sl.unrle(src, marker, 6)
        self.assertEqual(out, bytes([0x43, 0x42, 0x41, 0x41, 0x41, 0x41]))
        self.assertEqual(left, 0)


def raw_custom_track(data, corrupt=False, gap=100):
    n = len(data) // 4
    longs = list(struct.unpack(">%dI" % n, data))
    even = [x & M55 for x in longs]
    odd = [(x >> 1) & M55 for x in longs]
    ck = sum(longs) & M32
    bits = [0] * gap
    bits += bits_of(0x4489, 16) + bits_of(0x44A944A9, 32)
    for v in even:
        bits += bits_of(v, 32)
    bits += bits_of(ck & M55, 32)
    for v in odd:
        bits += bits_of(v, 32)
    bits += bits_of((ck >> 1) & M55, 32)
    bits += [0] * gap
    if corrupt:
        bits[gap + 16 + 32 + 41] ^= 1
    return bits


class TrackTests(unittest.TestCase):
    def test_decode_good_track(self):
        data = bytes((i * 7 + 3) & 0xFF for i in range(sl.DATA_BYTES))
        got, ok = sl.decode_custom_track(raw_custom_track(data))
        self.assertTrue(ok)
        self.assertEqual(got, data)

    def test_checksum_failure_reported(self):
        data = bytes((i * 5) & 0xFF for i in range(sl.DATA_BYTES))
        got, ok = sl.decode_custom_track(raw_custom_track(data, corrupt=True))
        self.assertFalse(ok)
        self.assertNotEqual(got, data)

    def test_no_sync(self):
        got, ok = sl.decode_custom_track([0, 1] * 5000)
        self.assertIsNone(got)
        self.assertFalse(ok)


class HelperTests(unittest.TestCase):
    def test_crc16_arc_check_value(self):
        self.assertEqual(c.crc16_arc(b"123456789"), 0xBB3D)

    def test_sector_runs_and_ranges(self):
        a = bytes(512 * 10)
        b = bytearray(a)
        b[512 * 2 + 5] = 1
        b[512 * 3] = 1
        b[512 * 7 + 511] = 1
        self.assertEqual(c.sector_runs(a, bytes(b)), [(2, 3), (7, 7)])
        self.assertEqual(c.byte_ranges(a, bytes(b), gap=4), [(1029, 1030), (1536, 1537), (4095, 4096)])

    def test_xcopy_filler_constant(self):
        self.assertEqual(c.XCOPY_FILL[:8], b"(W)XCOPY")

    def _hunk_exe(self, unpacked):
        code = bytearray(0x60)
        code[0:2] = b"\x41\xfa"
        code[2:4] = (0x0010).to_bytes(2, "big")
        code[4:6] = b"\xd1\xfc"
        code[6:10] = (0x40).to_bytes(4, "big")
        start = 2 + 0x10
        end = start + 0x40
        code[end - 4:end] = unpacked.to_bytes(4, "big")
        head = struct.pack(">IIIII", 0x3F3, 0, 1, 0, 0) + struct.pack(">I", 0x60 // 4)
        return head + struct.pack(">II", 0x3E9, 0x60 // 4) + bytes(code)

    def test_game_exe_detection(self):
        good = self._hunk_exe(0x464CA)
        self.assertTrue(c.is_xenon_exe(good))
        self.assertEqual(c.exe_unpacked_size(good), 0x464CA)
        self.assertTrue(c.is_game_exe(good))
        # the same stub shape that unpacks to a different size is some other crunched program
        self.assertFalse(c.is_game_exe(self._hunk_exe(0x5C762)))
        self.assertFalse(c.is_game_exe(b"\x00" * 100))


if __name__ == "__main__":
    unittest.main()
