# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Small structural comparison helpers for the level-data blocks of the ST, Amiga and CDTV builds
# (docs/re-notes/atari-st-mirror-image.md):
#   value_mapping()  - are two blocks of 16-bit words the same layout under a one-to-one value remapping?
#   ascending_runs() - runs of ascending 32-bit pointers (the per-level path tables)
#   text_regions()   - runs of printable ASCII (assembler-source residue in a build)
# All of them take bytes and return numbers; nothing here writes game data.
import struct
import sys
from collections import defaultdict, Counter


def words(b):
    return struct.unpack(">%dH" % (len(b) // 2), b[:len(b) // 2 * 2])


def value_mapping(a, b):
    """Compare two equal-length blocks as big-endian 16-bit words.

    Returns a dict: words, zero_positions_equal (same cells empty), forward_ambiguous (distinct `a` words that map to more
    than one `b` word), reverse_ambiguous, and bijection (True when the value map is one-to-one both ways).
    """
    wa, wb = words(a), words(b)
    if len(wa) != len(wb):
        raise ValueError("blocks differ in length")
    fwd, rev = defaultdict(Counter), defaultdict(Counter)
    for x, y in zip(wa, wb):
        fwd[x][y] += 1
        rev[y][x] += 1
    fa = sum(1 for c in fwd.values() if len(c) > 1)
    ra = sum(1 for c in rev.values() if len(c) > 1)
    return dict(words=len(wa),
                zero_positions_equal=sum(1 for x, y in zip(wa, wb) if (x == 0) == (y == 0)),
                distinct_a=len(fwd), distinct_b=len(rev),
                forward_ambiguous=fa, reverse_ambiguous=ra, bijection=(fa == 0 and ra == 0),
                mapping={x: next(iter(c)) for x, c in fwd.items()})


def ascending_runs(data, minrun=60, maxrun=130, max_step=0x4000, limit=0x400000, shifts=(0, 2)):
    """Runs of big-endian longs that strictly ascend by at most max_step (a table of pointers into the data after it).

    Returns (byte_offset, count, first, last) for runs whose length is in [minrun, maxrun]; both 4-byte alignments are tried
    because overlay files carry a 28-byte header and relocated tables can sit on either.
    """
    out = []
    for sh in shifts:
        buf = data[sh:]
        n = len(buf) // 4
        w = struct.unpack(">%dI" % n, buf[:n * 4])
        i = 0
        while i < n - 1:
            j = i
            while j + 1 < n and 0 < w[j + 1] - w[j] <= max_step and w[j + 1] < limit:
                j += 1
            if minrun <= j - i + 1 <= maxrun:
                out.append((sh + i * 4, j - i + 1, w[i], w[j]))
            i = j + 1
    return out


def text_regions(data, minlen=64):
    """(offset, length) of runs of printable ASCII (plus TAB, CR, LF) at least minlen bytes long."""
    def ok(c):
        return 32 <= c < 127 or c in (9, 10, 13)
    out, i, n = [], 0, len(data)
    while i < n:
        if ok(data[i]):
            j = i
            while j < n and ok(data[j]):
                j += 1
            if j - i >= minlen:
                out.append((i, j - i))
            i = j
        else:
            i += 1
    return out


def main(argv):
    if len(argv) == 4 and argv[1] == "map":
        r = value_mapping(open(argv[2], "rb").read(), open(argv[3], "rb").read())
        r.pop("mapping")
        print(r)
        return 0
    if len(argv) == 3 and argv[1] == "runs":
        print(ascending_runs(open(argv[2], "rb").read()))
        return 0
    if len(argv) == 3 and argv[1] == "text":
        regs = text_regions(open(argv[2], "rb").read())
        print(len(regs), "regions,", sum(n for _, n in regs), "bytes:", regs)
        return 0
    print("usage: stmap.py map A B | runs FILE | text FILE")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
