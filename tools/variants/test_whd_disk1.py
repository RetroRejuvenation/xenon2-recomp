# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Synthetic tests for tools/variants/whd_disk1.py (no game data): the CRC, the floppy-version table, and the
game-image round trip through the XOR chain."""
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))

import retail_game_image  # noqa: E402
import whd_disk1  # noqa: E402


def _table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
        t.append(c)
    return t


_T = _table()


def forged_disk(target_crc, seed):
    """Random bytes whose first 1024 have CRC-16/ARC `target_crc` (the last two bytes are searched)."""
    rnd = random.Random(seed)
    disk = bytearray(rnd.getrandbits(8) for _ in range(901120))
    state = 0
    for b in disk[:1022]:
        state = (state >> 8) ^ _T[(state ^ b) & 0xFF]
    for tail in range(65536):
        c = state
        for b in (tail >> 8, tail & 0xFF):
            c = (c >> 8) ^ _T[(c ^ b) & 0xFF]
        if c == target_crc:
            disk[1022] = tail >> 8
            disk[1023] = tail & 0xFF
            assert whd_disk1.crc16_arc(bytes(disk[:1024])) == target_crc
            return disk
    raise AssertionError("no tail reaches the CRC")


class WhdDisk1Tests(unittest.TestCase):
    def test_crc16_arc_check_value(self):
        self.assertEqual(whd_disk1.crc16_arc(b"123456789"), 0xBB3D)

    def test_version_table_and_identify(self):
        for crc, version in ((0x7F8F, "v1"), (0xC0FB, "v2"), (0x7EA7, "v3"), (0x7E53, "v4"), (0xBDCE, "v5"), (0x6827, "v6")):
            disk = forged_disk(crc, crc)
            self.assertEqual(whd_disk1.identify(bytes(disk))[0], version)
        self.assertIsNone(whd_disk1.identify(bytes(901120)))

    def test_game_image_round_trip(self):
        rnd = random.Random(5)
        image = bytes(rnd.getrandbits(8) for _ in range(retail_game_image.IMAGE_SIZE))
        disk = forged_disk(0x7EA7, 9)
        off = retail_game_image.IMAGE_OFFSET
        disk[off:off + retail_game_image.IMAGE_SIZE] = retail_game_image.chain_encode(image)
        self.assertEqual(whd_disk1.game_image(bytes(disk)), image)

    def test_differing_sectors(self):
        a = bytes(2048)
        b = bytearray(a)
        b[600] = 1
        b[1500] = 1
        self.assertEqual(whd_disk1.differing_sectors(a, bytes(b)), [1, 2])


if __name__ == "__main__":
    unittest.main()
