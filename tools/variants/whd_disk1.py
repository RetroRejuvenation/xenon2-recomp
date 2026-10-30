#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Recover the retail Xenon 2 game image from a WHDLoad `Disk.1` (or any 880 KB image of a retail disk 1) and prove it
equals the image decoded from the preservation IPF of the same pressing.

A WHDLoad retail `Disk.1` is not a special layout: the install's imager (RawDIC) writes the disk's standard tracks
as they are and leaves the Copylock key track (cylinder 0, head 1) blank, so the file is a plain 880 KB image whose
sectors match the IPF's decoded sectors everywhere except that track. The game image is then stored exactly as on the
disk (tools/retail_game_image.py: a 16-bit-word XOR chain from disk offset $70C00), so the same arithmetic recovers it.
The imager identifies the floppy version by the CRC-16/ARC of the 1 KB boot block ($7f8f v1, $c0fb v2, $7ea7 v3,
$7e53 v4, $bdce v5, $6827 v6; docs/re-notes/whdload.md).

usage: python3 tools/variants/whd_disk1.py DISK1 [--ipf FILE.ipf]
  prints the floppy version, the sha1 of the disk and of the decoded game image; with --ipf, decodes the IPF with
  tools/ipf.py + tools/mfm.py and reports which sectors of the disk differ and whether the decoded game images are equal.
Nothing is written; no game data leaves this process.
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import retail_game_image  # noqa: E402

VERSIONS = {
    0x7F8F: ("v1", "retail two-disk release, second pressing (SPS 2874 [a])"),
    0xC0FB: ("v2", "retail two-disk release as on the Bombuzal + Xenon 2 compilation (SPS 1762)"),
    0x7EA7: ("v3", "retail two-disk release, first pressing (SPS 0297)"),
    0x7E53: ("v4", "one-disk budget release, standard tracks"),
    0xBDCE: ("v5", "one-disk budget release (Rob Northen PDos)"),
    0x6827: ("v6", "one-disk The Power Pack release (Speedlock)"),
}


def crc16_arc(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def identify(disk):
    return VERSIONS.get(crc16_arc(disk[:1024]))


def game_image(disk):
    """The retail game image of a retail two-disk disk 1 (v1-v3)."""
    return retail_game_image.extract(disk)


def decode_ipf(path):
    import ipf
    import mfm

    with open(path, "rb") as f:
        img = ipf.parse_ipf(f.read())
    adf, _reports = mfm.decode_image({k: t.cells for k, t in img.tracks.items()})
    return adf


def differing_sectors(a, b):
    return [i // 512 for i in range(0, min(len(a), len(b)), 512) if a[i:i + 512] != b[i:i + 512]]


def main(argv):
    args = argv[1:]
    ipf_path = None
    if "--ipf" in args:
        i = args.index("--ipf")
        ipf_path = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        print(__doc__)
        return 2
    with open(args[0], "rb") as f:
        disk = f.read()
    ident = identify(disk)
    print("disk: %d bytes, sha1 %s, boot-block CRC-16/ARC %04x -> %s" % (
        len(disk), hashlib.sha1(disk).hexdigest(), crc16_arc(disk[:1024]),
        "floppy version %s: %s" % ident if ident else "not a known retail disk 1"))
    if not ident or ident[0] not in ("v1", "v2", "v3") or len(disk) != 901120:
        print("no game image decoded (only the retail two-disk versions v1-v3 as 880 KB images are handled here)")
        return 1
    img = game_image(disk)
    print("decoded game image: %d bytes, sha1 %s" % (len(img), hashlib.sha1(img).hexdigest()))
    if ipf_path:
        adf = decode_ipf(ipf_path)
        diff = differing_sectors(disk, adf)
        print("IPF-decoded disk: sha1 %s; sectors that differ from the file: %d%s" % (
            hashlib.sha1(adf).hexdigest(), len(diff),
            "" if not diff else " (%s..%s: track %s)" % (diff[0], diff[-1], sorted({s // 11 for s in diff}))))
        ipf_img = game_image(adf)
        same = ipf_img == img
        print("game image from the IPF: sha1 %s -> %s" % (hashlib.sha1(ipf_img).hexdigest(),
                                                           "IDENTICAL" if same else "DIFFERENT"))
        return 0 if same else 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
