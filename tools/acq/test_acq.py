# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Tests for tools/acq: every fixture is synthetic (generated here); no game bytes are involved.
#   python3 -m unittest discover -s tools/acq -p 'test_*.py'
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import arcfs  # noqa: E402
import flux  # noqa: E402
import modplay  # noqa: E402
import pdos  # noqa: E402
import rnc  # noqa: E402
import stx  # noqa: E402


class BitWriter:
    """LSB-first bits in 16-bit little-endian words (the RNC bit stream)."""

    def __init__(self):
        self.bits = []

    def put(self, value, n):
        for i in range(n):
            self.bits.append((value >> i) & 1)

    def words(self):
        bits = self.bits + [0] * (-len(self.bits) % 16)
        out = bytearray()
        for i in range(0, len(bits), 16):
            w = sum(b << j for j, b in enumerate(bits[i:i + 16]))
            out += struct.pack("<H", w)
        return bytes(out)


def rnc_literals(payload):
    """A one-chunk RNC method 1 stream holding only a literal run of 2 or 3 bytes' worth of symbols."""
    n = len(payload)
    assert 2 <= n <= 3
    w = BitWriter()
    w.put(0, 2)
    w.put(3, 5)                      # raw table: 3 leaves
    for ln in (0, 0, 1):             # symbol 2 only, code length 1
        w.put(ln, 4)
    w.put(0, 5)                      # distance table: empty
    w.put(0, 5)                      # length table: empty
    w.put(1, 16)                     # one sub-chunk
    w.put(0, 1)                      # Huffman code of symbol 2 (length 1, code 0)
    w.put(n - 2, 1)                  # extra bit: value = 2 | extra
    packed = w.words() + payload
    header = b"RNC\x01" + struct.pack(">II", n, len(packed)) + struct.pack(">HH", rnc.crc16(payload), rnc.crc16(packed)) + b"\0\1"
    return header + packed


class RncTests(unittest.TestCase):
    def test_literal_stream_round_trip(self):
        for payload in (b"AB", b"xyz"):
            stream = b"junk" + rnc_literals(payload)
            ok, h, out = rnc.verify(stream, 4)
            self.assertTrue(ok)
            self.assertEqual(out, payload)
            self.assertEqual(h["unpacked"], len(payload))

    def test_damage_is_detected(self):
        stream = bytearray(rnc_literals(b"xyz"))
        stream[-1] ^= 0xFF
        ok, _h, _o = rnc.verify(bytes(stream), 0)
        self.assertFalse(ok)

    def test_truncated_and_oversized(self):
        stream = rnc_literals(b"AB")
        ok, _h, _o = rnc.verify(stream[:-3], 0)
        self.assertFalse(ok)
        bad = stream[:4] + struct.pack(">I", 0x7FFFFFFF) + stream[8:]
        with self.assertRaises(rnc.RncError):
            rnc.unpack(bad, 0)

    def test_crc16_known_value(self):
        self.assertEqual(rnc.crc16(b"123456789"), 0xBB3D)  # CRC-16/ARC check value


class PdosTests(unittest.TestCase):
    def sectors(self, seed):
        return [bytes((seed * 31 + s * 7 + i * 13) & 0xFF for i in range(512)) for s in range(12)]

    def test_round_trip(self):
        secs = self.sectors(5)
        cells = pdos.encode_track(secs, 17)
        got = pdos.decode_track(cells, 17)
        self.assertEqual(sorted(got), list(range(12)))
        for s in range(12):
            self.assertEqual(got[s][0], secs[s])

    def test_wrong_track_and_corruption_rejected(self):
        secs = self.sectors(9)
        cells = pdos.encode_track(secs, 20)
        self.assertEqual(pdos.decode_track(cells, 21), {})
        damaged = list(cells)
        for i in range(3000 + 16 + 64 + 100, 3000 + 16 + 64 + 110):
            damaged[i] ^= 1
        got = pdos.decode_track(damaged, 20)
        self.assertNotIn(0, got)           # the checksum catches the damage in sector 0
        self.assertIn(1, got)

    def test_pll_recovers_cells_with_jitter(self):
        cells = pdos.encode_track(self.sectors(2), 33)
        # turn cells into flux intervals (ticks of 25 ns, 2 us cells = 80 ticks) with symmetric peak shift
        times, t = [], 0
        for c in cells:
            t += 80
            if c:
                times.append(t)
        shifted = [x + (6 if i % 3 == 0 else -6) for i, x in enumerate(times)]
        intervals = [b - a for a, b in zip([0] + shifted, shifted)]
        bits = flux.pll_bits(intervals, 2000.0, 0.1, 0.01)
        got = pdos.decode_track(bits, 33)
        self.assertEqual(len(got), 12)


class FluxFileTests(unittest.TestCase):
    def test_scp_reader(self):
        intervals = [160, 240, 160, 320]
        track = b"TRK" + bytes([0]) + struct.pack("<III", sum(intervals), len(intervals), 16) + struct.pack(">%dH" % len(intervals), *intervals)
        offs = [0] * 168
        offs[0] = 16 + 672
        header = b"SCP" + bytes([0, 0x80, 1, 0, 0, 3, 0, 0, 0]) + b"\0\0\0\0\0\0\0"
        header = header[:16].ljust(16, b"\0")
        d = header + struct.pack("<168I", *offs) + track
        with tempfile.NamedTemporaryFile(suffix=".scp", delete=False) as f:
            f.write(d)
        try:
            r = flux.read_scp(f.name)
            self.assertEqual(r["tracks"][0][0], intervals)
        finally:
            os.unlink(f.name)

    def test_scp_rejects_garbage(self):
        with tempfile.NamedTemporaryFile(suffix=".scp", delete=False) as f:
            f.write(b"NOPE" * 10)
        try:
            with self.assertRaises(ValueError):
                flux.read_scp(f.name)
        finally:
            os.unlink(f.name)

    @staticmethod
    def varint(v):
        out = bytearray()
        while True:
            b = v & 0x7F
            v >>= 7
            out.append(b | (0x80 if v else 0))
            if not v:
                return bytes(out)

    def test_fluxengine_reader(self):
        # index, then pulses of 48 and 72 ticks, index, pulse 48, index
        blob = bytes([0x40, 0x80 | 48, 0x3F, 0x80 | 9, 0x40, 0x80 | 48, 0x40])
        msg = b"\x08" + self.varint(1) + b"\x10" + self.varint(0) + b"\x1a" + self.varint(len(blob)) + blob
        d = b"\x08" + self.varint(0x466C7578) + b"\x10" + self.varint(2) + b"\x1a" + self.varint(len(msg)) + msg
        with tempfile.NamedTemporaryFile(suffix=".flux", delete=False) as f:
            f.write(d)
        try:
            r = flux.read_fluxengine(f.name)
            revs = r["tracks"][2]            # cylinder 1, head 0
            self.assertEqual(len(revs), 2)
            self.assertAlmostEqual(revs[0][0] * 25 / 1000, 4.0, places=1)
            self.assertAlmostEqual(revs[0][1] * 25 / 1000, 6.0, places=1)
        finally:
            os.unlink(f.name)


class StxTests(unittest.TestCase):
    def build(self):
        sectors = [bytes([n]) * 512 for n in range(1, 11)]
        desc = b""
        for i in range(10):
            desc += struct.pack("<IHHBBBBHBB", i * 512, 0, 0, 0, 0, i + 1, 2, 0, 0, 0)
        rec = struct.pack("<IIHHHBB", 16 + len(desc) + 5120, 0, 10, 1, 6250, 0, 0) + desc + b"".join(sectors)
        return b"RSY\0" + struct.pack("<HHHBB", 3, 1, 0, 1, 2) + b"\0\0\0\0" + rec

    def test_raw_image_and_boot_checksum(self):
        img = stx.raw_image(self.build(), spt=10, cylinders=1)
        self.assertEqual(len(img), 5120)
        self.assertEqual(img[512 * 3], 4)
        boot = bytearray(512)
        struct.pack_into(">H", boot, 510, (0x1234 - 0) & 0xFFFF)
        self.assertEqual(stx.boot_checksum(bytes(boot)), 0x1234)

    def test_rejects_garbage(self):
        with self.assertRaises(ValueError):
            stx.parse_stx(b"XXXX" + bytes(40))


def lzw_compress(data, maxbits=16):
    """Unix-compress-style LZW as ArcFS stores it (LSB-first, 9..maxbits bits, 256 = clear, no clears emitted)."""
    out = BitWriter()
    table = {bytes([i]): i for i in range(256)}
    free, nb = 257, 9
    w = b""
    for c in data:
        wc = w + bytes([c])
        if wc in table:
            w = wc
        else:
            out.put(table[w], nb)
            if free < (1 << maxbits):
                table[wc] = free
                free += 1
                if free - 1 == (1 << nb) and nb < maxbits:   # the decoder is one entry behind: widen after it adds
                    pass
            w = bytes([c])
    out.put(table[w], nb)
    return out


class ArcfsTests(unittest.TestCase):
    def test_rle_decoder(self):
        self.assertEqual(arcfs.rle(b"a\x90\x04b"), b"aaaab")
        self.assertEqual(arcfs.rle(b"\x90\x00"), b"\x90")

    def test_lzw_short_stream(self):
        # a stream of nine-bit literals only (fewer than 256 entries): decoding is the identity
        w = BitWriter()
        for ch in b"Xenon":
            w.put(ch, 9)
        self.assertEqual(arcfs.lzw(w.words())[:5], b"Xenon")

    def test_archive_walk_with_stored_file(self):
        body = b"hello arcfs"
        name = b"Readme".ljust(11, b"\0")
        entry = bytes([0x82]) + name + struct.pack("<6I", len(body), 0xFFFFFF47, 0, 0, len(body), 0)
        directory = entry + bytes(36)
        header = b"Archive\0" + struct.pack("<5I", len(directory), 0x60 + len(directory), 40, 100, 0)
        header = header.ljust(0x60, b"\0")
        d = header + directory + body
        files = arcfs.walk(d)
        self.assertEqual([f["path"] for f in files], ["Readme"])
        self.assertEqual(arcfs.extract_file(d, files[0]), body)

    def test_rejects_garbage(self):
        with self.assertRaises(ValueError):
            arcfs.walk(b"nothing here" * 20)


class ModplayTests(unittest.TestCase):
    def make_module(self):
        header = bytearray(1084)
        header[:5] = b"TEST\0"
        # sample 1: 64-byte square wave, full volume, looping
        struct.pack_into(">H", header, 20 + 22, 32)
        header[20 + 25] = 64
        struct.pack_into(">H", header, 20 + 26, 0)
        struct.pack_into(">H", header, 20 + 28, 32)
        header[950] = 1
        header[952] = 0
        header[1080:1084] = b"M.K."
        pattern = bytearray(1024)
        pattern[0:4] = bytes([0x01, 0xAC, 0x10, 0x00])      # sample 1, period 428
        sample = bytes([100] * 32 + [156] * 32)             # +100 / -100 as signed bytes
        return bytes(header) + bytes(pattern) + sample

    def test_renders_sound(self):
        with tempfile.NamedTemporaryFile(suffix=".mod", delete=False) as f:
            f.write(self.make_module())
        try:
            pcm, rate = modplay.render(f.name, rate=22050)
            self.assertEqual(rate, 22050)
            self.assertGreater(len(pcm), 22050)
            self.assertGreater(int(abs(pcm).max()), 1000)
        finally:
            os.unlink(f.name)

    def test_rejects_non_module(self):
        with tempfile.NamedTemporaryFile(suffix=".mod", delete=False) as f:
            f.write(bytes(2000))
        try:
            with self.assertRaises(ValueError):
                modplay.load(f.name)
        finally:
            os.unlink(f.name)


if __name__ == "__main__":
    unittest.main()
