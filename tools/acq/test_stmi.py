# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Synthetic tests for stx_fz.py and stmap.py (hand-built fixtures; no game bytes).
#   python3 -m unittest tools/acq/test_stmi.py
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stx  # noqa: E402
import stx_fz  # noqa: E402
import stmap  # noqa: E402


def stx_file(tracks):
    """tracks: list of (track, side, fuzzy_bytes, [(sector_number, data512, fdc)]). Sector data follow the fuzzy mask."""
    body = b""
    for trk, side, fuzzy, secs in tracks:
        descr, data, off = b"", b"", 0
        for sn, sd, fdc in secs:
            descr += struct.pack("<IHHBBBBHBB", off, 0, 16000, trk, side, sn, 2, 0, fdc, 0)
            data += sd
            off += len(sd)
        rec = descr + fuzzy + data
        rs = 16 + len(rec)
        body += struct.pack("<IIHHHBB", rs, len(fuzzy), len(secs), 0x61, 6250, trk | (side << 7), 0) + rec
    return b"RSY\0" + struct.pack("<HHHBB", 3, 0x0002, 0, len(tracks), 2) + b"\0" * 4 + body


def boot_sector():
    b = bytearray(512)
    b[0:2] = b"\x60\x1c"
    s = sum(struct.unpack(">256H", bytes(b)))
    b[510:512] = struct.pack(">H", (0x1234 - s) & 0xFFFF)
    return bytes(b)


def clean_track(trk, side, fill):
    return (trk, side, b"", [(n, bytes([fill + n]) * 512, 0) for n in range(1, 11)])


class StxFuzzy(unittest.TestCase):
    def test_fuzzy_mask_shifts_sector_data(self):
        key = b"Rob Northen Comp" + bytes(496)
        secs = [(1, boot_sector(), 0), (2, b"\x11" * 512, 0), (6, key, 0x88)] + [(n, bytes([n]) * 512, 0) for n in (3, 4, 5, 7, 8, 9, 10)]
        d = stx_file([(0, 0, b"\xff" * 512, secs)])
        info = stx.parse_stx(d)
        t = info["tracks"][(0, 0)]
        by_id = {s["id"][2]: s for s in t["secs"]}
        self.assertEqual(stx_fz.fuzzy_count(d, t), 512)
        self.assertEqual(stx_fz.sector_bytes(d, t, by_id[6]), key)
        self.assertEqual(stx_fz.sector_bytes(d, t, by_id[2]), b"\x11" * 512)
        # the unshifted reader returns the mask for the first sectors: the bug this module avoids
        self.assertNotEqual(stx.sector_data(d, t, by_id[2]), b"\x11" * 512)
        self.assertEqual(len(stx_fz.fuzzy_mask(d, t)), 512)

    def test_no_fuzzy_mask_matches_stx(self):
        d = stx_file([clean_track(0, 0, 0x20)])
        t = stx.parse_stx(d)["tracks"][(0, 0)]
        for s in t["secs"]:
            self.assertEqual(stx_fz.sector_bytes(d, t, s), stx.sector_data(d, t, s))

    def test_side_image_boot_and_protected_tracks(self):
        secs = [(1, boot_sector(), 0)] + [(n, bytes([n]) * 512, 0x08 if n == 6 else 0) for n in range(2, 11)]
        d = stx_file([(0, 0, b"", secs), clean_track(0, 1, 0x40), clean_track(1, 0, 0x60)])
        img0 = stx_fz.side_image(d, 0)
        self.assertEqual(stx_fz.boot_checksum(img0[:512]), 0x1234)
        self.assertEqual(img0[512 * 5:512 * 6], bytes([6]) * 512)
        img1 = stx_fz.side_image(d, 1)
        self.assertEqual(img1[:512], bytes([0x41]) * 512)
        self.assertEqual(stx_fz.protected_tracks(d), [(0, 0, 10, [6], 0)])
        self.assertTrue(any("side 0" in line and "(executable)" in line for line in stx_fz.summary(d)))

    def test_entropy(self):
        self.assertEqual(stx_fz.entropy(b""), 0.0)
        self.assertEqual(stx_fz.entropy(b"\0" * 64), 0.0)
        self.assertAlmostEqual(stx_fz.entropy(bytes(range(256))), 8.0)


class Stmap(unittest.TestCase):
    def test_bijection_and_zero_cells(self):
        a = struct.pack(">8H", 0, 9, 0x39, 9, 0, 0x41, 0x39, 0)
        remap = {0: 0, 9: 9, 0x39: 0x41, 0x41: 0x49}
        b = struct.pack(">8H", *(remap[x] for x in struct.unpack(">8H", a)))
        r = stmap.value_mapping(a, b)
        self.assertTrue(r["bijection"])
        self.assertEqual(r["zero_positions_equal"], 8)
        self.assertEqual(r["mapping"][0x39], 0x41)
        self.assertEqual((r["distinct_a"], r["distinct_b"]), (4, 4))

    def test_not_a_function(self):
        a = struct.pack(">4H", 1, 1, 2, 3)
        b = struct.pack(">4H", 5, 6, 7, 8)
        r = stmap.value_mapping(a, b)
        self.assertFalse(r["bijection"])
        self.assertEqual(r["forward_ambiguous"], 1)
        with self.assertRaises(ValueError):
            stmap.value_mapping(b"\0\0", b"\0\0\0\0")

    def test_ascending_runs(self):
        table = struct.pack(">90I", *range(0x1000, 0x1000 + 90 * 8, 8))
        data = b"\xff" * 40 + table + b"\xff" * 40
        runs = stmap.ascending_runs(data, minrun=60)
        self.assertEqual(runs, [(40, 90, 0x1000, 0x1000 + 89 * 8)])
        shifted = stmap.ascending_runs(b"\xff\xff" + data, minrun=60)
        self.assertEqual([r[1] for r in shifted], [90])
        self.assertEqual(shifted[0][0] % 4, 2)
        self.assertEqual(stmap.ascending_runs(bytes(400), minrun=60), [])

    def test_text_regions(self):
        data = bytes(10) + b"label\tequ\t*-16 ; unused\r\n" * 4 + bytes(5) + b"short" + bytes(3)
        regs = stmap.text_regions(data, minlen=64)
        self.assertEqual(len(regs), 1)
        self.assertEqual(regs[0][0], 10)
        self.assertEqual(stmap.text_regions(b"abc", minlen=2), [(0, 3)])


if __name__ == "__main__":
    unittest.main()
