# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Pasti (.stx) sector access that honours the fuzzy-mask bytes. stx.py reads a sector at `base + data_offset`, where
# `base` is the end of the sector descriptors; that is right for tracks without a fuzzy mask, but a track record with a
# fuzzy count (a weak-bit sector, as on the Copylock key track) stores the mask first, so the sector data offsets are
# relative to the end of the mask: `base + fuzzy + data_offset`. Without that, track 0 of such a disk reads as garbage
# (the Mirror Image dump's track 0, docs/re-notes/atari-st-mirror-image.md).
#
#   python3 tools/acq/stx_fz.py DISK.stx          # per-side summary: boot sectors, protected tracks, entropy
import math
import struct
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
import stx  # noqa: E402


def fuzzy_count(d, track):
    """Number of fuzzy-mask bytes stored between the sector descriptors and the sector data."""
    return struct.unpack("<I", d[track["rec"] + 4:track["rec"] + 8])[0]


def sector_bytes(d, track, sec):
    n = 128 << sec["id"][3]
    o = track["base"] + fuzzy_count(d, track) + sec["do"]
    return d[o:o + n]


def fuzzy_mask(d, track):
    return d[track["base"]:track["base"] + fuzzy_count(d, track)]


def side_image(d, side, spt=10, cylinders=80):
    """Plain single-side image (cylinder-major, sector ids 1..spt of 512 bytes); missing sectors stay zero."""
    info = stx.parse_stx(d)
    img = bytearray(cylinders * spt * 512)
    for (trk, sd), t in info["tracks"].items():
        if sd != side or trk >= cylinders:
            continue
        for s in t["secs"]:
            sn, sz = s["id"][2], s["id"][3]
            if 1 <= sn <= spt and sz == 2:
                o = (trk * spt + sn - 1) * 512
                img[o:o + 512] = sector_bytes(d, t, s)
    return bytes(img)


def boot_checksum(sector):
    return sum(struct.unpack(">256H", sector)) & 0xFFFF  # 0x1234 = executable boot sector


def entropy(b):
    if not b:
        return 0.0
    c = Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def protected_tracks(d, spt=10):
    """Tracks that are not ten clean 512-byte sectors: CRC errors (FDC status bits 3/4), odd counts or sizes."""
    out = []
    for (trk, sd), t in sorted(stx.parse_stx(d)["tracks"].items()):
        secs = t["secs"]
        if not secs:
            continue
        bad = [s["id"][2] for s in secs if s["fdc"] & 0x18]
        ids = sorted(s["id"][2] for s in secs)
        clean = (len(secs) == spt and ids == list(range(1, spt + 1))
                 and all(s["id"][3] == 2 for s in secs) and not bad)
        if not clean:
            out.append((trk, sd, len(secs), bad, fuzzy_count(d, t)))
    return out


def summary(d):
    info = stx.parse_stx(d)
    lines = []
    for side in sorted({k[1] for k in info["tracks"]}):
        img = side_image(d, side)
        ck = boot_checksum(img[:512])
        sectors = [img[i * 512:(i + 1) * 512] for i in range(800)]
        hi = sum(1 for s in sectors if entropy(s) > 7.0)
        zero = sum(1 for s in sectors if not any(s))
        lines.append(f"side {side}: boot checksum {ck:#06x}{' (executable)' if ck == 0x1234 else ''}, "
                     f"{zero} empty sectors, {hi} sectors above 7.0 bits/byte")
    lines.append("protected tracks: " + repr(protected_tracks(d)[:8]))
    return lines


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    d = open(argv[1], "rb").read()
    print("\n".join(summary(d)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
