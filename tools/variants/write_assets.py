#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Write runtime assets (game_0400.bin, disc2.bin, manifest.txt) for the alternative game BUILDS.

usage: python3 tools/variants/write_assets.py power-pack  IMAGE.ipf|Disk.1       OUT_DIR
       python3 tools/variants/write_assets.py compilation-1762  DISK1.(ipf|adf) DISK2.(ipf|adf) OUT_DIR
       python3 tools/variants/write_assets.py pdos-v5  DISK.bin OUT_DIR [--allow-damaged]

A stand-in for the importer (phase P3d integrates these recipes into `x2recomp --import`): it reads the user's own
images and writes the three files the runtime needs, with a `build=` line in manifest.txt that names the build the
runtime must start (runtime/boot.h BuildInfo). OUT_DIR must be git-ignored; nothing here is ever committed.

  power-pack        SPS 2234 (or the WHDLoad `1Disk 2234` Disk.1). The game image is the main image at $E0C over
                    the canonical-shaped low area (entry stub `bra.w $7032` and zeros: the intro's memory, which the
                    game never uses once it is running); disc 2 is rebuilt from the six game files placed at their
                    module start sectors, with the disc id in sector 0 and zeros elsewhere.
  compilation-1762  SPS 1762, two disks: the plain game image at disk 1 offset $70C00 and disk 2 unchanged.
  pdos-v5           the Rob Northen PDos one-disk release: DISK.bin is the assembled 982,528-byte disk (tools/acq/pdos.py
                    --out, or the WHDLoad Disk.1). The game is the RNC stream at $58806 unpacked at $400, disc 2 is the
                    disk itself. Refused unless the game and all eleven packed files verify (both CRC16s); with
                    --allow-damaged (development only: for recording replays that never load a damaged file) the
                    game alone must verify and the files that fail are listed. The importer has no such flag.
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as c  # noqa: E402
import compilation_1762 as comp  # noqa: E402
import power_pack_2234 as pp  # noqa: E402

sys.path.insert(0, os.path.join(HERE, "..", "acq"))
import rnc  # noqa: E402

GAME_SIZE = 0x3FC00
PDOS_DISK = 982528
PDOS_STAGE2 = 0x58806
PDOS_TABLE = 0x87C
PDOS_FILES = 11
STUB = bytes.fromhex("60006c30")  # bra.w $7032, the entry every canonical-shaped image has at $400
DISC_ID = b"\x00\x00X2B "


def power_pack(path):
    _data, _intro, main, _game, files, info = pp.build(path)
    if info["bad_tracks"]:
        raise ValueError("%s: tracks failing their checksums: %s" % (path, info["bad_tracks"]))
    if len(files) != 6 or not all(ok for *_, ok in files):
        raise ValueError("%s: expected six intact game files, found %d" % (path, len(files)))
    game = bytearray(GAME_SIZE)
    game[:4] = STUB
    base = pp.MAIN_BASE - 0x400
    game[base:base + len(main)] = main
    disc2 = bytearray(c.SECTORS * c.SECTOR)
    disc2[:len(DISC_ID)] = DISC_ID
    for fid, _st, _ct, body, _ok in files:
        disc2[fid * c.SECTOR:fid * c.SECTOR + len(body)] = body
    return bytes(game), bytes(disc2), "power-pack-2234"


def compilation_1762(path1, path2):
    d1, _ = comp.load(path1)
    d2, _ = comp.load(path2)
    game = d1[comp.IMAGE_OFFSET:comp.IMAGE_OFFSET + comp.IMAGE_SIZE]
    return bytes(game), bytes(d2), "compilation-1762"


def pdos_v5(path, allow_damaged):
    disk = open(path, "rb").read()
    if len(disk) != PDOS_DISK:
        raise ValueError("%s: a PDos disk image is %d bytes" % (path, PDOS_DISK))
    if rnc.crc16(disk[:1024]) != 0xBDCE or disk[:3] != b"DOS":
        raise ValueError("%s: the boot block is not the PDos release's (CRC16 0xBDCE)" % path)
    ok, _h, game = rnc.verify(disk, PDOS_STAGE2)
    if not ok or len(game) > GAME_SIZE or game[:2] != b"\x60\x00":
        raise ValueError("%s: the game stream does not verify" % path)
    bad = []
    for i in range(PDOS_FILES):
        first, _count, off = (int.from_bytes(disk[PDOS_TABLE + 8 * i + k:PDOS_TABLE + 8 * i + k + 2], "big") for k in (0, 2, 4))
        good = False
        if first >= 12 and off < 512:
            try:
                good = rnc.verify(disk, (first - 1) * 512 + off)[0]
            except (rnc.RncError, IndexError):
                pass
        if not good:
            bad.append(i + 1)
    if bad and not allow_damaged:
        raise ValueError("%s: packed files %s do not verify (a damaged dump is refused; --allow-damaged is for development only)" % (path, bad))
    if bad:
        print("WARNING: packed files %s do not verify; the assets are for development replays that never load them" % bad)
    return bytes(game).ljust(GAME_SIZE, b"\0"), disk, "pdos-v5"


def write(out, game, disc2, build, note):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "game_0400.bin"), "wb") as f:
        f.write(game)
    with open(os.path.join(out, "disc2.bin"), "wb") as f:
        f.write(disc2)
    with open(os.path.join(out, "manifest.txt"), "w") as f:
        f.write("# x2recomp variant assets (tools/variants/write_assets.py; delete this folder to write them again)\n")
        f.write("build=%s\n" % build)
        f.write("note=%s\n" % note)
        f.write("game_0400.bin_sha1=%s\n" % hashlib.sha1(game).hexdigest())
        f.write("disc2.bin_sha1=%s\n" % hashlib.sha1(disc2).hexdigest())
    print("%s: game_0400.bin sha1 %s, disc2.bin sha1 %s, build=%s" % (
        out, hashlib.sha1(game).hexdigest(), hashlib.sha1(disc2).hexdigest(), build))


def main(argv):
    if len(argv) == 3 and argv[0] == "power-pack":
        g, d, b = power_pack(argv[1])
        write(argv[2], g, d, b, "disc 2 rebuilt from the six game files; sectors 1-10 and the gaps are zero")
        return 0
    if len(argv) == 4 and argv[0] == "compilation-1762":
        g, d, b = compilation_1762(argv[1], argv[2])
        write(argv[3], g, d, b, "disk 2 unchanged")
        return 0
    if len(argv) in (3, 4) and argv[0] == "pdos-v5" and (len(argv) == 3 or argv[3] == "--allow-damaged"):
        g, d, b = pdos_v5(argv[1], len(argv) == 4)
        write(argv[2], g, d, b, "disc 2 is the PDos disk itself; the game's own loader reads it")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
