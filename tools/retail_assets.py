#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Prototype of the retail import (the native importer will do the same, docs/re-notes/retail-protection.md):
turn a retail disk 1 IPF and disk 2 (IPF or ADF) into an assets directory the runtime can run.

  python3 tools/retail_assets.py DISK1.ipf DISK2.(ipf|adf) --out DIR

  DIR/game_0400.bin   the retail game image, chain-decoded, byte for byte as on the disc ($3FC00 bytes)
  DIR/disc2.bin       disc 2 as an 880 KB sector image
  DIR/copylock.txt    the receipt: the key track on disc 1 was read and verified (signature, sync order, LFSR data)

The receipt is only written when the key track checks out; without it the runtime refuses the retail image
(runtime/retail_protection.h). The image bytes are never modified. Output goes where you point it (keep it under the
git-ignored generated/); nothing is read from or written to the repository.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import copylock  # noqa: E402
import ipf  # noqa: E402
import mfm  # noqa: E402
import retail_game_image  # noqa: E402

KEY_TRACK = (0, 1)
STUB_AT = 0x70F6 - 0x400
# move.l a6,-(a7); lea x(pc),a6; movem.l d0-d7/a0-a7,(a6); lea $40(a6),a6 (the lea displacement is not compared)
STUB = bytes.fromhex("2f0e4dfa0000" + "48d6ffff" + "4dee0040")
ENTRY = bytes.fromhex("6000" + "6c30")  # bra.w $7032


def receipt_text(seed):
    """Must stay identical to format_copylock_datum() in runtime/retail_protection.cpp."""
    return "x2recomp-copylock 1\nsignature rob-northen-comp\nsyncs ok\nseed 0x%06X\n" % (seed & 0x7FFFFF)


def verify_key_track(track):
    """(seed, None) for a verified Copylock key track, else (None, reason)."""
    info = copylock.analyse_track(track)
    if not info:
        return None, "cylinder 0 head 1 is not a Copylock key track"
    if not info["signature"]:
        return None, "the 'Rob Northen Comp' signature is missing from sector 6"
    words = [int(w, 16) for w, _pos, _idx in info["sync_words"]]
    idxs = [i for _w, _pos, i in info["sync_words"]]
    if words != copylock.SYNC_LIST or idxs != list(range(11)):
        return None, "the eleven sync words or index bytes are not in the Copylock order"
    if info.get("lfsr_bytes_match_sector0") != 512 or not info.get("lfsr_seed"):
        return None, "the sector 0 data is not the Copylock LFSR sequence"
    return info["lfsr_seed"], None


def has_retail_stub(image):
    if len(image) < STUB_AT + len(STUB):
        return False
    p = image[STUB_AT:STUB_AT + len(STUB)]
    return all(p[i] == STUB[i] for i in range(len(STUB)) if i not in (4, 5))


def decode_ipf_adf(data):
    img = ipf.parse_ipf(data)
    adf, reports = mfm.decode_image({k: t.cells for k, t in img.tracks.items()})
    return img, adf, reports


def build(disk1, disk2):
    """(game_image, disc2, receipt) from the bytes of the two files; raises ValueError with a reason."""
    img1, adf1, _ = decode_ipf_adf(disk1)
    if KEY_TRACK not in img1.tracks:
        raise ValueError("disk 1 has no key track (cylinder 0, head 1): an IPF or flux image is needed, not an ADF")
    seed, why = verify_key_track(img1.tracks[KEY_TRACK])
    if why:
        raise ValueError("disk 1: " + why)
    game = retail_game_image.extract(adf1)
    if game[:4] != ENTRY:
        raise ValueError("disk 1: the decoded game image does not start with the expected entry code")
    if not has_retail_stub(game):
        raise ValueError("disk 1: the decoded game image has no Copylock routine; it is not a retail image")
    if disk2[:4] == b"CAPS":
        _, disc2, reports = decode_ipf_adf(disk2)
        bad = [n for n, r in sorted(reports.items()) if not r.standard]
        if bad or len(reports) < 160:
            raise ValueError("disk 2: tracks that do not decode as standard Amiga tracks: %s" % bad[:8])
    else:
        disc2 = disk2
    if len(disc2) != 880 * 1024:
        raise ValueError("disk 2: expected an 880 KB image")
    if disc2[2:6] != b"X2B ":
        raise ValueError("disk 2: the \"X2B \" disc id is missing")
    return game, bytes(disc2), receipt_text(seed)


def main(argv):
    args = argv[1:]
    out = None
    paths = []
    i = 0
    while i < len(args):
        if args[i] == "--out" and i + 1 < len(args):
            out = args[i + 1]
            i += 2
        else:
            paths.append(args[i])
            i += 1
    if len(paths) != 2 or not out:
        print(__doc__)
        return 2
    try:
        with open(paths[0], "rb") as f:
            d1 = f.read()
        with open(paths[1], "rb") as f:
            d2 = f.read()
        game, disc2, receipt = build(d1, d2)
    except (OSError, ValueError, ipf.IpfError) as e:
        print("retail_assets: %s" % e, file=sys.stderr)
        return 3
    os.makedirs(out, exist_ok=True)
    for name, data in (("game_0400.bin", game), ("disc2.bin", disc2), ("copylock.txt", receipt.encode())):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
    print("wrote %s: game image %d bytes, disc 2 %d bytes, key track verified" % (out, len(game), len(disc2)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
