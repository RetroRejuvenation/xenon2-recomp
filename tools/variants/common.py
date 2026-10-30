# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Shared helpers for the per-variant analysis recipes in tools/variants/.

Everything here reads an image the user already has and prints hashes and offsets only; nothing is written
unless an output directory is given, and the outputs are for analysis in a git-ignored folder (never committed).
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))  # tools/

import ipf  # noqa: E402
import mfm  # noqa: E402

CANON_DISC2_SHA1 = "a748afc7a142335a40cc46fb7785dea820770241"
CANON_GAME_SHA1 = "1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4"
SECTORS = 1760
SECTOR = 512


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def load_ipf(path):
    with open(path, "rb") as f:
        return ipf.parse_ipf(f.read())


def ipf_to_adf(path):
    """IPF -> (adf bytes, {track_no: TrackReport}, IpfImage). Non-standard tracks are zero-filled."""
    img = load_ipf(path)
    adf, reports = mfm.decode_image({k: t.cells for k, t in img.tracks.items()})
    return adf, reports, img


def read_adf(path):
    with open(path, "rb") as f:
        d = f.read()
    if len(d) != 901120:
        raise ValueError("%s: not an 880 KB ADF" % path)
    return d


def cells_to_bits(cells):
    return [1 if c == 1 else 0 for c in cells]


def find_sync(bits, word=0x4489, start=0, circular=False):
    """Bit offset just after the first occurrence of the 16-bit `word` at or after `start`, or None."""
    n = len(bits)
    limit = n if circular else n - 16
    v = 0
    for i in range(start, start + 16 - 1):
        v = (v << 1) | bits[i % n]
    for i in range(start + 15, start + limit):
        v = ((v << 1) | bits[i % n]) & 0xFFFF
        if v == word:
            return i + 1
    return None


def bits_to_longs(bits, start, count):
    n = len(bits)
    out = []
    for k in range(count):
        v = 0
        for j in range(32):
            v = (v << 1) | bits[(start + 32 * k + j) % n]
        out.append(v)
    return out


def sector_runs(a, b, size=SECTOR):
    """Runs of differing sectors between two equal-length byte strings: [(first, last)]."""
    runs = []
    start = prev = None
    for s in range(len(a) // size):
        if a[s * size:(s + 1) * size] != b[s * size:(s + 1) * size]:
            if start is None:
                start = prev = s
            elif s == prev + 1:
                prev = s
            else:
                runs.append((start, prev))
                start = prev = s
    if start is not None:
        runs.append((start, prev))
    return runs


def byte_ranges(a, b, gap=16):
    """Ranges (start, end_exclusive) where two byte strings differ, merged when closer than `gap` bytes."""
    ranges = []
    start = prev = None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None:
                start = prev = i
            elif i <= prev + gap:
                prev = i
            else:
                ranges.append((start, prev + 1))
                start = prev = i
    if start is not None:
        ranges.append((start, prev + 1))
    return ranges


def find_canonical(name, search):
    """First existing path among `search` (a list of candidates), or None."""
    for p in search:
        if os.path.isfile(p):
            return p
    return None


def default_canonical_disc2():
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    return find_canonical("disc2.bin", [os.path.join(root, "generated", "assets", "disc2.bin")])


def default_canonical_game():
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    return find_canonical("game_0400.bin", [os.path.join(root, "generated", "assets", "game_0400.bin")])


KNOWN_EXE_SHA1 = "778cf75e75d7e1b2036f4d68f8a557b69e99a817"  # XENON-II of the BS1 releases
XCOPY_FILL = bytes.fromhex("28572958434f505928572958434f5059")  # "(W)XCOPY" repeated: a copier's bad-track filler


def crc16_arc(data):
    """CRC-16/ARC (as the WHDLoad imager uses on the 1 KB boot block to tell the floppy versions apart)."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


IMAGER_VERSIONS = {0x7F8F: "v1", 0xC0FB: "v2", 0x7EA7: "v3", 0x7E53: "v4", 0xBDCE: "v5", 0x6827: "v6"}


def is_xenon_exe(d):
    """True for an AmigaDOS executable shaped like XENON-II: one CODE hunk starting `lea d16(pc),a0; adda.l #len,a0`
    (the same test the C++ importer uses)."""
    if len(d) < 64 or struct_be32(d, 0) != 0x3F3:
        return False
    p = 4
    while True:
        if p + 4 > len(d):
            return False
        n = struct_be32(d, p)
        p += 4
        if not n:
            break
        if n > 256:
            return False
        p += 4 * n
    if p + 12 > len(d):
        return False
    first, last = struct_be32(d, p + 4), struct_be32(d, p + 8)
    if last < first or last - first > 64:
        return False
    p += 12 + 4 * (last - first + 1)
    if p + 8 > len(d) or (struct_be32(d, p) & 0x3FFFFFFF) != 0x3E9:
        return False
    code = p + 8
    code_len = (struct_be32(d, p + 4) & 0x3FFFFFFF) * 4
    if code_len > len(d) - code or code_len < 16:
        return False
    return d[code:code + 2] == b"\x41\xfa" and d[code + 4:code + 6] == b"\xd1\xfc"


def exe_unpacked_size(d):
    """Size the XENON-II style stub would unpack to (the last long of its crunched stream), or None. The stub shape
    alone is shared by other crunched programs. The real XENON-II unpacks to `$464CA` bytes (the `$3FC00`-byte game
    image plus data after it)."""
    if not is_xenon_exe(d):
        return None
    p = 4
    while struct_be32(d, p):
        p += 4 + 4 * struct_be32(d, p)
    p += 4
    first, last = struct_be32(d, p + 4), struct_be32(d, p + 8)
    p += 12 + 4 * (last - first + 1)
    code = p + 8
    start = 2 + ((d[code + 2] << 8) | d[code + 3])
    end = start + struct_be32(d, code + 6)
    if end > len(d) - code or end < start + 8:
        return None
    return struct_be32(d, code + end - 4)


def is_game_exe(d):
    n = exe_unpacked_size(d)
    return n is not None and 0x3FC00 <= n <= 0x50000  # heuristic window; the C++ importer's is wider (to $80000)


def struct_be32(d, o):
    return (d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]


def adf_files(adf_bytes):
    """[(path, bytes)] for every file of an OFS/FFS ADF, without writing anything."""
    import tempfile
    import adf as adflib
    with tempfile.NamedTemporaryFile(suffix=".adf") as t:
        t.write(adf_bytes)
        t.flush()
        a = adflib.Adf(t.name)
        out = []
        for e in a.walk():
            if e["kind"] == "file":
                try:
                    out.append((e["path"], a.read_file(e)))
                except Exception:
                    out.append((e["path"], None))
        return out
