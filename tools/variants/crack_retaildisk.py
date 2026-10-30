#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Recover the game image from a CRACKED RETAIL DISK 1 (PNA, QTX, Black Monks, Flashtro, Predators).

These cracks are the retail disk 1 with the Copylock removed: the boot block still loads a loader image from tracks 2-79
and the game image still sits at disk offset $70C00 (tracks 80+), but cracked disks store it as plain bytes (the retail
disk stores it as a 16-bit-word XOR chain, tools/retail_game_image.py). They are NOT single-disk games: the game still
asks for the standard disc 2, so disc 2 comes from the user's own disc 2 (sha1 a748afc7...), unchanged.

usage: python3 -I tools/variants/crack_retaildisk.py DISK1.adf --out DIR [--disc2 DISC2.adf] [--canonical game_0400.bin]
  writes DIR/game_0400.bin (and DIR/disc2.bin when --disc2 is given); DIR should be git-ignored.
  exit 0 recovered, 3 recognised but not supported, 2 unrecognised or unreadable.
Nothing here is game data: images are recognised by the SHA-1 of the whole disk.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cracklib as lib  # noqa: E402

# Black Monks: the body is plain, but the last 19455 bytes of the image ($3B400..$3FFFF) are left in the retail
# XOR-chain form, with the crack having overwritten the first two stored words of that part. The image happens to hold
# an earlier block ($3AB4F, 1088 bytes) identical to the head of the tail, so the head is read from there and the rest
# of the tail decodes from the end of that copy (this relies on that duplicate in the data, so it is per-disk).
BM_TAIL_START = 0x3B000       # image offset of the chain-encoded tail
BM_COPY_OFFSET = 0xAB74F      # disk offset ($70C00 + $3AB4F) of the block that equals the tail's head
BM_COPY_LEN = 1088

# sha1 of the whole disk -> (name, recipe, what it changes against the canonical image)
KNOWN = {
    "2028f31af2fb760da405618a42f9e69e15882825": (
        "PNA", "plain",
        "credit text only (181 bytes at $874A and $894A); behaviour identical to the canonical image"),
    "12b7bd53288d3764af062a71923c9ec8189e61f2": (
        "PNA [a]", "plain",
        "credit text (181 bytes) and a 4-byte trainer patch at $7B00 that stops the ship counter decreasing (infinite "
        "ships): gameplay differs from the canonical image"),
    "2b98cbbac1a89901fcfe634ef805b66612d3b2de": (
        "Quartex (QTX)", "plain",
        "credit text (220 bytes) and 6 bytes at $70FA, $76AE and $93F0 (protection sites patched in QTX's own way); "
        "behaviour identical to the canonical image in every replay"),
    "0df9822509837bce3999806e3ae2e218cea4ddd9": (
        "Black Monks", "blackmonks",
        "credit text, 6 bytes at the protection sites, a 5-byte trainer hook at $7913 (changes gameplay: boss and level 2 "
        "replays diverge) and a 4-byte hook at $30400"),
    "a3574a216c51948ac64dd308c7449740c71a84b7": (
        "Flashtro +16 (crack of the FIRST pressing)", "plain",
        "the first-pressing code revision ($1F8A, 99 bytes; the loader routine at $93F0, 396 bytes), credit text, trainer "
        "bytes at $703A-$7154 and $764A; behaviour identical to the canonical image in every replay"),
    "1b4855f98f7df8d5052e5578d387dbf486068f8f": (
        "Flashtro +16 [a]", None,
        "stored in a transformed form that is not the plain image nor the retail XOR chain: the first three words are "
        "patched and from $E0C on tens of KB differ from any chain decoding (not analysed further)"),
    "2405da96182c63fc6a7e48b7f19f2904f0a44ed9": (
        "Predators +9", "plain",
        "replaces 224 bytes of the entry code at $400 with the trainer's start-up: the runtime refuses the image "
        "('unexpected entry code'), and running the trainer is not supported"),
}


def read_disk(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != 901120:
        raise ValueError("not an 880 KB ADF (%d bytes)" % len(data))
    return data


def extract_plain(disk):
    """The game image stored as plain bytes at $70C00."""
    return bytes(disk[lib.IMAGE_OFFSET:lib.IMAGE_OFFSET + lib.GAME_SIZE])


def extract_blackmonks(disk):
    """Plain body, plus the tail rebuilt as described at BM_TAIL_START."""
    img = bytearray(extract_plain(disk))
    stored = bytes(img[BM_TAIL_START:])
    head = bytes(disk[BM_COPY_OFFSET:BM_COPY_OFFSET + BM_COPY_LEN])
    init = (head[-2] << 8) | head[-1]
    img[BM_TAIL_START:] = head + lib.chain_decode(stored[BM_COPY_LEN:], init)
    return bytes(img)


RECIPES = {"plain": extract_plain, "blackmonks": extract_blackmonks}


def main(argv):
    usage = __doc__
    pos, opts = lib.parse_args(argv, usage)
    if len(pos) != 1 or "out" not in opts:
        print(usage)
        return 2
    try:
        disk = read_disk(pos[0])
    except (OSError, ValueError) as e:
        print("error: %s" % e)
        return 2
    h = lib.sha1(disk)
    if h not in KNOWN:
        print("not a recognised cracked retail disk 1 (sha1 %s)" % h)
        return 2
    name, recipe, what = KNOWN[h]
    if recipe is None:
        print("variant      : %s\nnot supported: %s" % (name, what))
        return 3
    game = RECIPES[recipe](disk)
    disc2 = None
    notes = ["differs from the canonical image: " + what,
             "disc 2 is not on this disk: use the standard disc 2 (sha1 %s)" % lib.DISC2_SHA1]
    if "disc2" in opts:
        with open(opts["disc2"], "rb") as f:
            disc2 = f.read()
        if lib.sha1(disc2) != lib.DISC2_SHA1:
            notes.append("warning: the given disc 2 is not the standard one")
    lib.write_outputs(opts["out"], game, disc2)
    lib.report(name, game, disc2, opts, notes)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
