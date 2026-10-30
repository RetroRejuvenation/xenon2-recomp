#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Analyse the one-disk 'Power Pack' release (SPS 2234 IPF) and rebuild its game data in memory.

usage: python3 tools/variants/power_pack_2234.py DISK.ipf|Disk.1 [--out DIR] [--canonical-game FILE] [--canonical-disc2 FILE]

The input is either the SPS 2234 IPF or the WHDLoad `1Disk 2234` install's `Disk.1` (1,085,184 bytes: the decoded
data tracks 3..159, byte for byte what this tool decodes from the IPF, sha1 6461b317...).

Prints sizes, sha1s and comparisons only. With --out it also writes the decoded pieces there (analysis only: the
folder must be git-ignored; nothing is ever committed). Stages:
  1. custom data tracks 3..159 -> `$1B00` bytes each, checksum verified (the layout of the WHDLoad imager, version v6)
  2. intro (disk offset 0, `$4B170` bytes -> `$63D9C` bytes) and main (track 48, `$2A314` bytes -> `$3F1F4` bytes at `$E0C`)
  3. the six game files named by the file table inside the main image (at `$9512`/`$94B2`) -> the six level modules
"""
import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as c  # noqa: E402
import speedlock_v6 as sl  # noqa: E402

INTRO_PACKED, INTRO_LEN = 0x4B170, 0x63D9C
MAIN_TRACK, MAIN_PACKED, MAIN_LEN, MAIN_BASE = 48, 0x2A314, 0x3F1F4, 0xE0C
FILE_TABLE, ADDR_TABLE = 0x9512, 0x94B2


def build(path):
    if path.lower().endswith(".ipf"):
        img = c.load_ipf(path)
        data, bad = sl.decode_data_tracks(img)
    else:
        with open(path, "rb") as f:
            data = f.read()
        if len(data) != 157 * sl.DATA_BYTES:
            raise ValueError("%s: expected an IPF or a %d-byte Disk.1" % (path, 157 * sl.DATA_BYTES))
        bad = []
    info = {"bad_tracks": bad}
    intro, ok_i = sl.decrunch_msb(data[:INTRO_PACKED], INTRO_LEN)
    off = (MAIN_TRACK - sl.FIRST_DATA_TRACK) * sl.DATA_BYTES
    main, ok_m = sl.decrunch_msb(data[off:off + MAIN_PACKED], MAIN_LEN)
    info["intro_consumed"], info["main_consumed"] = ok_i, ok_m
    mem = bytearray(0x40000)
    mem[0x400:0x400 + min(len(intro), 0x40000 - 0x400)] = intro[:0x40000 - 0x400]
    mem[MAIN_BASE:MAIN_BASE + len(main)] = main
    game = bytes(mem[0x400:0x400 + 0x3FC00])
    files = []
    o = FILE_TABLE - 0x400
    i = 0
    while True:
        fid, st, ct = struct.unpack(">HBB", game[o + 4 * i:o + 4 * i + 4])
        if fid == 0xFF00 or ct == 0:
            break
        a = (ADDR_TABLE - 0x400) + 16 * i
        entry = struct.unpack(">IIII", game[a:a + 16])
        body, ok = sl.load_game_file(data, st, ct, entry)
        files.append((fid, st, ct, body, ok))
        i += 1
    return data, intro, main, game, files, info


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--out")
    ap.add_argument("--canonical-game")
    ap.add_argument("--canonical-disc2")
    a = ap.parse_args(argv)
    data, intro, main_, game, files, info = build(a.image)
    print("data tracks: %d bytes sha1 %s, tracks failing sync/checksum: %s" % (len(data), c.sha1(data), info["bad_tracks"] or "none"))
    print("intro: %d bytes sha1 %s (stream consumed exactly: %s)" % (len(intro), c.sha1(intro), info["intro_consumed"]))
    print("main: %d bytes sha1 %s (stream consumed exactly: %s)" % (len(main_), c.sha1(main_), info["main_consumed"]))
    print("composite game image (intro low area + main): sha1 %s" % c.sha1(game))
    disc2 = bytearray(c.SECTORS * c.SECTOR)
    for fid, st, ct, body, ok in files:
        print("file id %d (tracks %d+%d): %d bytes sha1 %s intact %s" % (fid, st, ct, len(body), c.sha1(body)[:12], ok))
        disc2[fid * c.SECTOR:fid * c.SECTOR + len(body)] = body
    can_game = a.canonical_game or c.default_canonical_game()
    can_d2 = a.canonical_disc2 or c.default_canonical_disc2()
    if can_game:
        ref = open(can_game, "rb").read()
        rg = c.byte_ranges(ref, game, gap=32)
        print("vs canonical game image: %d ranges, %d bytes differ" % (len(rg), sum(b - a_ for a_, b in rg)))
        print("  ranges (address, length):", [(hex(x + 0x400), y - x) for x, y in rg])
    if can_d2:
        ref = open(can_d2, "rb").read()
        for fid, st, ct, body, ok in files:
            same = ref[fid * c.SECTOR:fid * c.SECTOR + len(body)] == body
            diff = sum(1 for x, y in zip(ref[fid * c.SECTOR:fid * c.SECTOR + len(body)], body) if x != y)
            print("file id %d vs canonical disc 2 at sector %d: %s (%d bytes differ)" % (fid, fid, "identical" if same else "differs", diff))
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        for name, blob in (("intro.bin", intro), ("main.bin", main_), ("game_composite.bin", game), ("disc2_modules.bin", bytes(disc2))):
            with open(os.path.join(a.out, name), "wb") as f:
                f.write(blob)
        print("wrote analysis outputs to", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
