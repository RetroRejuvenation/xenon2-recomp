#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Unit tests for tools/variants/ (crunchers and crack recipes). Every fixture is synthetic: it is generated here from
random bytes; no disk image or game byte is used or needed.

usage: python3 -I -m unittest discover -s tools/variants -p 'test_cracks.py'

Optional local-only check against your own images: set X2_CRACK_SOURCES to a folder holding the cracked ADFs (any
sub-folder depth) and the recipes are run on every recognised disk and compared with the recorded output hashes.
"""
import hashlib
import os
import random
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _cracklib as lib  # noqa: E402
import crack_retaildisk  # noqa: E402
import ice_crunch  # noqa: E402
import onyx_crunch  # noqa: E402


class OnyxWriter:
    """Builds a stream the ONYX decruncher reads backwards: bits come LSB-first from 'bit bytes', literal bytes are raw
    bytes interleaved at the point they are read."""

    def __init__(self):
        self.cons = []          # bytes in the order the decoder consumes them (highest address first)
        self.slot = None        # [index into cons, bits so far]
        self.first = True

    def bit(self, b):
        if self.slot is None:
            self.cons.append(0)
            self.slot = [len(self.cons) - 1, 0, 7 if self.first else 8]
            if self.first:
                self.cons[-1] = 0x01        # sentinel at bit 0; data bits go to bits 1..7
            self.first = False
        idx, n, cap = self.slot
        shift = (n + 1) if cap == 7 else n
        self.cons[idx] |= (b & 1) << shift
        self.slot[1] += 1
        if self.slot[1] == cap:
            self.slot = None

    def bits(self, value, n):               # get(n) builds its value MSB first
        for k in range(n - 1, -1, -1):
            self.bit((value >> k) & 1)

    def raw(self, byte):
        self.cons.append(byte)

    def finish(self, params, ulen):
        stream = bytes(reversed(self.cons))
        return bytes(params) + struct.pack(">II", ulen, len(stream)) + stream


def onyx_pack(tokens, params=(9, 8, 13, 0x18)):
    """tokens: ('lit', bytes) | ('short', off) | ('long', length, off) | ('fill', count, byte). Returns (blob, expected)."""
    w = OnyxWriter()
    S = bytearray()                          # output in decode order (highest address first)

    def copy(dist, count):
        for _ in range(count):
            S.append(S[-dist])

    prev_lit = False
    p3 = params[0]
    for t in tokens:
        if t[0] == "lit":
            data = t[1]
            assert not prev_lit and 1 <= len(data) <= 4
            w.bit(1)                         # not a match
            if len(data) == 1:
                w.bit(0)
            else:
                w.bit(1)
                w.bits(len(data) - 2, 2)
            for b in data:
                w.raw(b)
                S.append(b)
            prev_lit = True
            continue
        if not prev_lit:
            w.bit(0)                         # match family
        if t[0] == "short":
            w.bit(0)
            w.bits(t[1], 8)
            copy(t[1] + 2, 2)
        elif t[0] == "long":
            length, off = t[1], t[2]
            assert 3 <= length <= 5 and 0 <= off < 64
            w.bit(1)
            w.bits(length - 3, 2)
            w.bit(0)
            w.bits(off, 6)
            copy(off + length, length)
        else:                                # fill: run of one byte
            count, byte = t[1], t[2]
            w.bit(1)
            w.bits(3, 2)
            w.bits(7, 3)
            w.bit(0)                         # run-length fill
            # d3 = 1 in the default params: bit1 clear -> a selector bit follows; 0 selects count width p3, base $85
            w.bit(0)
            w.bits(count - 0x85, p3)
            w.bits(byte, 8)
            S.extend(bytes([byte]) * count)
        prev_lit = False
    expected = bytes(reversed(S))
    return w.finish(params, len(expected)), expected


def ice_pack(tokens):
    """tokens: ('lit', byte) | ('match', length(2..3), off) | ('long_lit', bytes). Bits LSB first from 16-bit words."""
    bits = []
    S = bytearray()

    def emit(v, n):
        for k in range(n):
            bits.append((v >> k) & 1)

    for t in tokens:
        if t[0] == "lit":
            emit(1, 1)
            emit(t[1], 8)
            S.append(t[1])
        elif t[0] == "match":
            length, off = t[1], t[2]
            assert length in (2, 3) and 1 <= off < 32
            emit(0, 1)
            emit(0, 1)                       # class 1: d1 = 1, d4 = 1
            emit(length - 2, 1)              # d4 = 1 + bit -> length = d4 + 1
            emit(1, 1)                       # offset class B
            emit(0, 1)                       # ... (carry set, then clear): d1 = 5, d2 = 0
            emit(off, 5)
            for _ in range(length):
                S.append(S[-off])
        else:
            raise AssertionError(t)
    while len(bits) % 16:
        bits.append(0)
    words = [sum(bits[i + k] << k for k in range(16)) for i in range(0, len(bits), 16)]
    body = b"".join(struct.pack(">H", w) for w in reversed(words)) + struct.pack(">H", 16)
    blob = b"\x00\x00" * 3 + body            # three words in front: the decoder reads a word ahead at the end
    expected = bytes(reversed(S))
    return blob, expected, 6, len(body)


class OnyxTests(unittest.TestCase):
    def test_literals_and_matches(self):
        rnd = random.Random(1)
        tokens = [("lit", b"abcd"), ("long", 4, 0), ("lit", b"wxyz"), ("long", 5, 0), ("lit", b"1234"), ("long", 5, 0)]
        for i in range(60):
            n = rnd.randrange(1, 5)
            tokens.append(("lit", bytes(rnd.randrange(256) for _ in range(n))))
            if rnd.random() < 0.5:
                tokens.append(("short", rnd.randrange(0, 5)))
            else:
                tokens.append(("long", rnd.randrange(3, 6), rnd.randrange(0, 8)))
            if rnd.random() < 0.3:
                tokens.append(("short", rnd.randrange(0, 4)))
        blob, expected = onyx_pack(tokens)
        self.assertEqual(onyx_crunch.decrunch(blob), expected)

    def test_fill_and_offset(self):
        tokens = [("lit", b"\x11\x22"), ("fill", 0x90, 0x5A), ("lit", b"\x7f"), ("short", 3)]
        blob, expected = onyx_pack(tokens)
        self.assertEqual(onyx_crunch.decrunch(blob), expected)
        self.assertIn(b"\x5a" * 0x90, expected)

    def test_decrunch_at_an_offset(self):
        blob, expected = onyx_pack([("lit", b"AB"), ("short", 0)])
        self.assertEqual(onyx_crunch.decrunch(b"\xaa" * 7 + blob, 7), expected)

    def test_rejects_damage(self):
        blob, _ = onyx_pack([("lit", b"AB"), ("short", 0)] * 20)
        with self.assertRaises(onyx_crunch.Crunch):
            onyx_crunch.decrunch(blob[:20])                    # truncated: packed length exceeds the file
        bad = bytearray(blob)
        bad[4:8] = struct.pack(">I", 0x7FFFFFFF)               # absurd unpacked length
        with self.assertRaises(onyx_crunch.Crunch):
            onyx_crunch.decrunch(bytes(bad))

    def test_random_input_never_escapes(self):
        rnd = random.Random(7)
        for _ in range(200):
            n = rnd.randrange(16, 120)
            blob = bytes(rnd.randrange(256) for _ in range(n))
            blob = blob[:4] + struct.pack(">II", rnd.randrange(0, 400), max(0, n - 12)) + blob[12:]
            try:
                out = onyx_crunch.decrunch(blob)
                self.assertEqual(len(out), struct.unpack(">I", blob[4:8])[0])
            except (onyx_crunch.Crunch, IndexError):
                pass


class IceTests(unittest.TestCase):
    def test_literals_and_matches(self):
        rnd = random.Random(2)
        tokens = [("lit", rnd.randrange(256)) for _ in range(40)]
        for i in range(30):
            tokens.append(("match", rnd.choice((2, 3)), rnd.randrange(1, 12)))
            tokens.append(("lit", rnd.randrange(256)))
        blob, expected, start, plen = ice_pack(tokens)
        self.assertEqual(ice_crunch.depack(blob, start, len(expected), plen), expected)

    def test_rejects_damage(self):
        blob, expected, start, plen = ice_pack([("lit", 1)] * 8)
        with self.assertRaises(ice_crunch.Ice):
            ice_crunch.depack(blob, start, len(expected), plen + 100)   # packed length beyond the file
        with self.assertRaises(ice_crunch.Ice):
            ice_crunch.depack(blob, start, 0x7FFFFFFF, plen)


class RebuildDisc2Tests(unittest.TestCase):
    def test_layout_and_fill(self):
        a = bytes(range(256)) * 2 * 3          # 3 sectors
        b = b"\x07" * 512 * 2
        disc = lib.rebuild_disc2({11: a, 20: b})
        self.assertEqual(len(disc), 1760 * 512)
        self.assertEqual(disc[:6], b"\x00\x00X2B ")
        self.assertEqual(disc[512:1024], b"\xe5" * 512)                 # sectors 1..10: formatter fill
        self.assertEqual(disc[11 * 512:14 * 512], a)
        self.assertEqual(disc[14 * 512:15 * 512], b"\xe5" * 512)        # the gap between chunks
        self.assertEqual(disc[20 * 512:22 * 512], b)
        self.assertEqual(disc[22 * 512:], b"\x00" * (1738 * 512))      # after the last chunk: zeros

    def test_rejects_bad_chunks(self):
        with self.assertRaises(ValueError):
            lib.rebuild_disc2({0: b"\x00" * 512})                       # sector 0 is the id sector
        with self.assertRaises(ValueError):
            lib.rebuild_disc2({5: b"\x00" * 513})
        with self.assertRaises(ValueError):
            lib.rebuild_disc2({5: b"\x00" * 1024, 6: b"\x00" * 512})   # overlap
        with self.assertRaises(ValueError):
            lib.rebuild_disc2({1759: b"\x00" * 1024})                   # past the end


class RetailDiskTests(unittest.TestCase):
    def make_disk(self, image):
        disk = bytearray(random.Random(3).randbytes(901120))
        disk[lib.IMAGE_OFFSET:lib.IMAGE_OFFSET + lib.GAME_SIZE] = image
        return bytes(disk)

    def test_plain_image(self):
        image = random.Random(4).randbytes(lib.GAME_SIZE)
        self.assertEqual(crack_retaildisk.extract_plain(self.make_disk(image)), image)

    def test_blackmonks_tail_repair(self):
        rnd = random.Random(5)
        plain = bytearray(rnd.randbytes(lib.GAME_SIZE))
        t = crack_retaildisk.BM_TAIL_START
        n = crack_retaildisk.BM_COPY_LEN
        c = crack_retaildisk.BM_COPY_OFFSET - lib.IMAGE_OFFSET
        plain[c:c + n] = plain[t:t + n]          # the image holds an earlier block identical to the head of the tail
        head = bytes(plain[t:t + n])
        prev = (head[-2] << 8) | head[-1]
        enc = bytearray()
        for i in range(n, len(plain) - t, 2):
            w = (plain[t + i] << 8) | plain[t + i + 1]
            e = w ^ prev
            enc += bytes([e >> 8, e & 0xFF])
            prev = w
        stored = bytearray(plain)
        stored[t:] = b"\xff" * n + enc            # the crack overwrote the head of the stored tail
        self.assertEqual(crack_retaildisk.extract_blackmonks(self.make_disk(bytes(stored))), bytes(plain))

    def test_main_recognises_by_hash_only(self):
        import io
        import tempfile
        from contextlib import redirect_stdout
        image = random.Random(6).randbytes(lib.GAME_SIZE)
        disk = self.make_disk(image)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "disk.adf")
            with open(path, "wb") as f:
                f.write(disk)
            out = os.path.join(d, "out")
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(crack_retaildisk.main(["x", path, "--out", out]), 2)     # unknown sha1
            crack_retaildisk.KNOWN[lib.sha1(disk)] = ("synthetic", "plain", "nothing")
            try:
                with redirect_stdout(buf):
                    self.assertEqual(crack_retaildisk.main(["x", path, "--out", out]), 0)
            finally:
                del crack_retaildisk.KNOWN[lib.sha1(disk)]
            with open(os.path.join(out, "game_0400.bin"), "rb") as f:
                self.assertEqual(f.read(), image)
            self.assertFalse(os.path.exists(os.path.join(out, "disc2.bin")))              # disc 2 is the user's own


EXPECTED_GAME_SHA1 = {
    "2028f31af2fb760da405618a42f9e69e15882825": "1599c37d894f4e276bee64d6f3bf874cc80e1f01",   # PNA
    "12b7bd53288d3764af062a71923c9ec8189e61f2": "2232da1c9a75e0d54b4025ee963d604ffd1d053b",   # PNA [a]
    "2b98cbbac1a89901fcfe634ef805b66612d3b2de": "86094773cb2b31c8b9886e242026ba7ba9bba721",   # QTX
    "0df9822509837bce3999806e3ae2e218cea4ddd9": "976f09d2f50c5d3bde2dbd7c68cca7d8964a3a5e",   # Black Monks
    "a3574a216c51948ac64dd308c7449740c71a84b7": "70fcb3d8c7eaf937adeafa7db13dab9887d2b6ce",   # Flashtro +16
    "2405da96182c63fc6a7e48b7f19f2904f0a44ed9": "0baf124bcfd98a62eaf8d81aecfb7af34748e423",   # Predators +9
}


@unittest.skipUnless(os.environ.get("X2_CRACK_SOURCES"), "set X2_CRACK_SOURCES to a folder of cracked ADFs")
class LocalImageTests(unittest.TestCase):
    def test_recipes_reproduce_recorded_hashes(self):
        seen = 0
        for root, _dirs, files in os.walk(os.environ["X2_CRACK_SOURCES"]):
            for name in files:
                if not name.lower().endswith(".adf"):
                    continue
                data = open(os.path.join(root, name), "rb").read()
                h = lib.sha1(data)
                if h in EXPECTED_GAME_SHA1:
                    recipe = crack_retaildisk.KNOWN[h][1]
                    self.assertEqual(lib.sha1(crack_retaildisk.RECIPES[recipe](data)), EXPECTED_GAME_SHA1[h], name)
                    seen += 1
        if not seen:
            self.skipTest("no recognised cracked disk found")


if __name__ == "__main__":
    unittest.main()
