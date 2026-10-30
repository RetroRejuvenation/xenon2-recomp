# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Shared helpers for the crack recipes (tools/variants/crack_*.py). No game data is stored here: images are
recognised by the SHA-1 of the whole disk, and every constant below is a layout fact (offsets, sizes, fill bytes)."""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import retail_game_image  # noqa: E402

GAME_SIZE = retail_game_image.IMAGE_SIZE            # $3FC00: the game image as it lives at $400
GAME_BASE = 0x400
IMAGE_OFFSET = retail_game_image.IMAGE_OFFSET       # $70C00 on a retail-layout disk 1
DISC2_SECTORS = 1760
DISC2_SHA1 = "a748afc7a142335a40cc46fb7785dea820770241"
CANONICAL_GAME_SHA1 = "1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4"
# The retail disc 2 starts with a sector holding the disc id ("X2B " at offset 2) and carries the formatter's $E5
# fill in the unused sectors between its data chunks; the unused tail is zero.
DISC_ID_SECTOR = b"\x00\x00X2B "
FORMAT_FILL = 0xE5

chain_decode = retail_game_image.chain_decode


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def diff_ranges(a, b, join=16):
    """Ranges (start, end) where a and b differ; ranges closer than `join` bytes are merged."""
    out = []
    n = min(len(a), len(b))
    i = 0
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        last = i
        j = i
        while j < n and j - last <= join:
            if a[j] != b[j]:
                last = j
            j += 1
        out.append((i, last + 1))
        i = last + 1
    if len(a) != len(b):
        out.append((n, max(len(a), len(b))))
    return out


def describe_diff(ref, img, base=GAME_BASE, limit=40):
    rr = diff_ranges(ref, img)
    lines = ["%d range(s), %d byte(s) differ from the canonical image" % (len(rr), sum(e - s for s, e in rr))]
    for s, e in rr[:limit]:
        lines.append("  address $%X..$%X (%d bytes)" % (s + base, e + base, e - s))
    return lines


def rebuild_disc2(chunks):
    """Disc 2 from {start_sector: data} chunks (data a multiple of 512 bytes). Sector 0 is the disc-id sector only:
    the rest of its payload is not on the disks these chunks come from (it is not read by the game)."""
    disc = bytearray(DISC2_SECTORS * 512)
    end = 0
    for start, data in sorted(chunks.items()):
        if len(data) % 512 or start < 1 or start * 512 + len(data) > len(disc):
            raise ValueError("chunk at sector %d does not fit" % start)
        if start < end:
            raise ValueError("overlapping chunks at sector %d" % start)
        end = start + len(data) // 512
    for s in range(1, end):
        disc[s * 512:(s + 1) * 512] = bytes([FORMAT_FILL]) * 512
    for start, data in chunks.items():
        disc[start * 512:start * 512 + len(data)] = data
    disc[0:512] = DISC_ID_SECTOR + bytes([FORMAT_FILL]) * (512 - len(DISC_ID_SECTOR))
    return bytes(disc)


def write_outputs(out_dir, game, disc2=None):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "game_0400.bin"), "wb") as f:
        f.write(game)
    if disc2 is not None:
        with open(os.path.join(out_dir, "disc2.bin"), "wb") as f:
            f.write(disc2)


def parse_args(argv, usage, extra_flags=()):
    """Tiny argument parser: positional paths, --out DIR, --disc2 FILE, --canonical FILE."""
    pos, opts = [], {}
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("--out", "--disc2", "--canonical"):
            if i + 1 >= len(argv):
                raise SystemExit(usage)
            opts[a[2:]] = argv[i + 1]
            i += 2
        elif a in ("-h", "--help"):
            raise SystemExit(usage)
        elif a.startswith("--"):
            raise SystemExit("unknown option %s\n%s" % (a, usage))
        else:
            pos.append(a)
            i += 1
    return pos, opts


def report(variant, game, disc2, opts, notes):
    print("variant      : %s" % variant)
    print("game image   : %d bytes, sha1 %s%s" % (len(game), sha1(game),
          "  (the canonical image)" if sha1(game) == CANONICAL_GAME_SHA1 else ""))
    if disc2 is not None:
        print("disc 2       : %d bytes, sha1 %s%s" % (len(disc2), sha1(disc2),
              "  (the canonical disc 2)" if sha1(disc2) == DISC2_SHA1 else ""))
    for n in notes:
        print("note         : %s" % n)
    if "canonical" in opts:
        with open(opts["canonical"], "rb") as f:
            ref = f.read()
        for line in describe_diff(ref, game):
            print("compare      : %s" % line)
