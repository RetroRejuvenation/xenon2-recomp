#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Unit tests for tools/ipf.py, tools/mfm.py, tools/copylock.py and tools/retail_game_image.py.
Every fixture is synthetic (generated here); no disk image, game byte or SPS file is used or needed.

usage: python3 -I -m unittest discover -s tools -p 'test_disk_formats.py'
"""
import os
import random
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import copylock  # noqa: E402
import ipf  # noqa: E402
import mfm  # noqa: E402
import retail_assets  # noqa: E402
import retail_game_image  # noqa: E402


def sectors(seed, n=11):
    rnd = random.Random(seed)
    return [bytes(rnd.randrange(256) for _ in range(512)) for _ in range(n)]


def tag(name, payload_after_crc, size=None):
    """An IPF record: 4-byte name, size, CRC32 (of the record with the CRC field zeroed), then the fields."""
    size = size or 12 + len(payload_after_crc)
    rec = bytearray(name + struct.pack(">II", size, 0) + payload_after_crc)
    crc = zlib.crc32(bytes(rec)) & 0xFFFFFFFF
    rec[8:12] = struct.pack(">I", crc)
    return bytes(rec)


def compress16(v):
    """The 16 data bits of a raw long (data bits at the 0x55555555 positions) as two bytes."""
    bits = [(v >> (30 - 2 * k)) & 1 for k in range(16)]
    return bytes([int("".join(map(str, bits[:8])), 2), int("".join(map(str, bits[8:])), 2)])


def synthetic_ipf(track_no, secs, gap_cells=64):
    """A one-track IPF (SPS encoder 2) holding the 11 Amiga sectors as MFM/sync/gap descriptions, one block."""
    desc = bytearray()
    data_cells = 0
    for s, data in enumerate(secs):
        desc += bytes([(1 << 5) | 2, 2, 0, 0])  # 2 gap bytes: MFM of zeros
        data_cells += 32
        desc += bytes([(1 << 5) | 1, 4, 0x44, 0x89, 0x44, 0x89])  # raw sync words
        data_cells += 32
        stream = b"".join(compress16(v) for v in mfm.amiga_sector_longs(track_no, s, data))
        desc += bytes([(2 << 5) | 2]) + struct.pack(">H", len(stream)) + stream
        data_cells += 16 * len(stream)
    desc += b"\x00"
    block = struct.pack(">IIIIIIII", data_cells, gap_cells, 0, 0, 1, 0, 0xAA, 32)  # encoder 1 = MFM, gap type 0
    payload = block + bytes(desc)
    size_cells = data_cells + gap_cells
    cyl, head = track_no // 2, track_no % 2
    # INFO after the 12-byte header: type, encoder, encoder revision, release, revision, origin, min/max cylinder,
    # min/max head, credit day, credit time, 4 platform words, 5 extra words
    info = struct.pack(">" + "I" * 21, 1, 2, 1, 1, 1, 0, 0, 83, 0, 1, 19961224, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0)
    # IMGE layout after the 12-byte header: cyl, head, type, sigtype, size_bytes, index_bytes, index_cells,
    # datasize_cells, gapsize_cells, size_cells, block_count, process, weak_bits, track index, 3 reserved
    imge = struct.pack(">IIIIIIIIIIIIIIIII", cyl, head, 2, 1, len(payload), 0, 0, data_cells, gap_cells, size_cells,
                       1, 0, 0, 0, 0, 0, 0)
    out = tag(b"CAPS", b"")
    out += tag(b"INFO", info, 96)
    out += tag(b"IMGE", imge, 80)
    data_head = struct.pack(">IIII", len(payload), len(payload) * 8, zlib.crc32(payload) & 0xFFFFFFFF, 0)
    out += tag(b"DATA", data_head, 28) + payload
    return out


class MfmTests(unittest.TestCase):
    def test_round_trip_with_index_rotation(self):
        secs = sectors(1)
        for rot in (0, 1234, 5000):
            cells = mfm.encode_track(secs, 7, rotate=rot)
            rep = mfm.decode_track(cells, 7)
            self.assertEqual(rep.good, list(range(11)))
            self.assertTrue(rep.standard)
            self.assertEqual([rep.sectors[i].data for i in range(11)], secs)

    def test_corrupt_data_is_reported_and_not_used(self):
        secs = sectors(2)
        cells = mfm.encode_track(secs, 3)
        bad = list(cells)
        # flip a data cell inside sector 4's data area (clock cells are not checksummed, data cells are)
        pos = 4 * 1088 * 8 + 6000
        bad[pos | 1] ^= 1
        rep = mfm.decode_track(bad, 3)
        self.assertLess(len(rep.good), 11)
        self.assertTrue(rep.bad)

    def test_wrong_track_number_is_rejected(self):
        cells = mfm.encode_track(sectors(3), 9)
        self.assertEqual(mfm.decode_track(cells, 10).good, [])

    def test_image_assembly_zero_fills_missing_tracks(self):
        secs = sectors(4)
        adf, reports = mfm.decode_image({(0, 1): mfm.encode_track(secs, 1)}, num_tracks=4)
        self.assertEqual(len(adf), 4 * 11 * 512)
        self.assertEqual(adf[11 * 512:11 * 512 + 512], secs[0])
        self.assertEqual(adf[:11 * 512], bytes(11 * 512))
        self.assertTrue(reports[1].standard)


class IpfTests(unittest.TestCase):
    def test_synthetic_track_decodes_to_its_sectors(self):
        secs = sectors(5)
        img = ipf.parse_ipf(synthetic_ipf(5, secs))
        t = img.tracks[(2, 1)]
        self.assertEqual(t.number, 5)
        rep = mfm.decode_track(t.cells, 5)
        self.assertEqual(rep.good, list(range(11)))
        self.assertEqual([rep.sectors[i].data for i in range(11)], secs)

    def test_record_crc_is_checked(self):
        data = bytearray(synthetic_ipf(5, sectors(6)))
        data[20] ^= 0x01  # inside the INFO record
        with self.assertRaises(ipf.IpfError):
            ipf.parse_ipf(bytes(data))

    def test_payload_crc_is_checked(self):
        data = bytearray(synthetic_ipf(5, sectors(7)))
        data[-5] ^= 0x01  # inside the DATA payload
        with self.assertRaises(ipf.IpfError):
            ipf.parse_ipf(bytes(data))

    def test_truncated_file_is_rejected(self):
        data = synthetic_ipf(5, sectors(8))
        with self.assertRaises(ipf.IpfError):
            ipf.parse_ipf(data[:-100])

    def test_garbage_is_rejected(self):
        with self.assertRaises(ipf.IpfError):
            ipf.parse_ipf(b"not an ipf file at all")


def lfsr_stream(seed, count):
    x = seed
    out = bytearray()
    for _ in range(count):
        out.append(copylock.lfsr_byte(x))
        x = copylock.lfsr_next(x)
    return bytes(out)


def synthetic_copylock_track(seed):
    """11 sectors: sync word, index byte, 512 LFSR bytes (sector 6 starts with the signature), a zero byte; plain MFM."""
    stream = lfsr_stream(seed, 11 * 512)
    cells = [0, 1] * 400
    prev = 0
    for idx, word in enumerate(copylock.SYNC_LIST):
        cells += [int(b) for b in format(word, "016b")]
        prev = word & 1
        body = bytes([idx]) + stream[idx * 512:(idx + 1) * 512]
        if idx == 6:
            body = body[:1] + copylock.SIG + body[17:]
        c, prev = mfm.mfm_encode_bytes(body + b"\x00", prev)
        cells += c
        c, prev = mfm.mfm_encode_bytes(b"\x00" * 44, prev)
        cells += c
    return cells


class FakeTrack:
    def __init__(self, cells):
        self.cells = cells
        self.times = [2000] * len(cells)


class CopylockTests(unittest.TestCase):
    def test_detects_signature_and_recovers_the_seed(self):
        seed = 0x36FA43
        info = copylock.analyse_track(FakeTrack(synthetic_copylock_track(seed)))
        self.assertIsNotNone(info)
        self.assertTrue(info["signature"])
        self.assertEqual(info["lfsr_seed"], seed)
        self.assertEqual(info["lfsr_bytes_match_sector0"], 512)
        self.assertEqual(len(info["sync_words"]), 11)

    def test_standard_track_is_not_copylock(self):
        self.assertIsNone(copylock.analyse_track(FakeTrack(mfm.encode_track(sectors(9), 1))))


class RetailImageTests(unittest.TestCase):
    def test_chain_encode_decode_are_inverse(self):
        plain = bytes(random.Random(10).randrange(256) for _ in range(4096))
        self.assertEqual(retail_game_image.chain_decode(retail_game_image.chain_encode(plain)), plain)

    def test_chain_of_zeros_stays_zero_after_the_first_word(self):
        enc = retail_game_image.chain_encode(bytes(8))
        self.assertEqual(enc, struct.pack(">HHHH", 0x002A, 0, 0, 0))

    def test_diff_ranges_merge_nearby_bytes(self):
        a = bytes(100)
        b = bytearray(a)
        b[10] = b[12] = b[90] = 1
        self.assertEqual(retail_game_image.diff_ranges(a, bytes(b)), [(10, 13), (90, 91)])

    def test_extract_rejects_a_non_adf(self):
        with self.assertRaises(ValueError):
            retail_game_image.extract(b"short")


class RetailAssetsTests(unittest.TestCase):
    def test_receipt_text_is_the_runtime_format(self):
        # runtime/retail_protection.cpp format_copylock_datum() produces exactly this text
        self.assertEqual(retail_assets.receipt_text(0x36FA43),
                         "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0x36FA43\n")
        self.assertEqual(retail_assets.receipt_text(0xFF123456), retail_assets.receipt_text(0x123456))

    def test_key_track_verifies(self):
        seed, why = retail_assets.verify_key_track(FakeTrack(synthetic_copylock_track(0x36FA43)))
        self.assertIsNone(why)
        self.assertEqual(seed, 0x36FA43)

    def test_standard_track_is_refused(self):
        seed, why = retail_assets.verify_key_track(FakeTrack(mfm.encode_track(sectors(9), 1)))
        self.assertIsNone(seed)
        self.assertIn("not a Copylock", why)

    def test_wrong_lfsr_data_is_refused(self):
        cells = synthetic_copylock_track(0x36FA43)
        # corrupt sector 0's data: flip cells well after its sync word
        pos = copylock.find_sync(cells, copylock.SYNC_LIST[0])[0] + 16 + 16 * 8 * 40
        for k in range(64):
            cells[pos + k] ^= 1
        seed, why = retail_assets.verify_key_track(FakeTrack(cells))
        self.assertIsNone(seed)
        self.assertIsNotNone(why)

    def test_retail_stub_detection_ignores_only_the_lea_displacement(self):
        img = bytearray(0x3FC00)
        img[retail_assets.STUB_AT:retail_assets.STUB_AT + 14] = retail_assets.STUB
        self.assertTrue(retail_assets.has_retail_stub(bytes(img)))
        img[retail_assets.STUB_AT + 4] = 0x12
        img[retail_assets.STUB_AT + 5] = 0x34
        self.assertTrue(retail_assets.has_retail_stub(bytes(img)))
        img[retail_assets.STUB_AT + 6] ^= 1
        self.assertFalse(retail_assets.has_retail_stub(bytes(img)))
        self.assertFalse(retail_assets.has_retail_stub(b"short"))

    def test_an_adf_has_no_key_track(self):
        with self.assertRaises(Exception):
            retail_assets.build(bytes(901120), bytes(901120))


if __name__ == "__main__":
    unittest.main()
