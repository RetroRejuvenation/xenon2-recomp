#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Amiga MFM track decoder (and a small encoder, used by the tests to build synthetic tracks).
# Input: a track as a list of cells (1 = flux transition in the cell, 0 = none, 2 = weak/unreadable, read as 0),
# e.g. IpfTrack.cells from tools/ipf.py, or the bit cells of a flux decoder. Output: per-sector status and data.
#
# Amiga track layout per sector (all MFM, 1088 bytes): 2 x 0xAA gap bytes, sync 0x4489 0x4489, then (odd/even
# long pairs) info (format 0xFF, track, sector, sectors-to-gap), 16 label bytes, header checksum, data checksum,
# then 512 data bytes (all odd bits, then all even bits). 11 sectors per track, 2 tracks per cylinder, 80+ cylinders.
#
#   python3 tools/mfm.py FILE.ipf [--adf OUT.adf] [--tracks]   # per-track report; optional ADF of standard tracks
import hashlib
import sys

SYNC = 0x4489
SECTORS = 11
SECTOR_SIZE = 512
RAW_SECTOR_BITS = (8 + 32 + 8 + 8 + 1024) * 8  # raw cells after the sync words: info, label, two checksums, data


def _cells_to_int_lists(cells):
    return [1 if c == 1 else 0 for c in cells]


def _bits_value(bits, start, n):
    v = 0
    for i in range(n):
        v = (v << 1) | bits[(start + i) % len(bits)]
    return v


def _decode_long(odd, even):
    return ((odd & 0x55555555) << 1) | (even & 0x55555555)


class Sector:
    def __init__(self):
        self.number = None
        self.track_no = None
        self.sectors_to_gap = None
        self.label = b""
        self.header_ok = False
        self.data_ok = False
        self.data = b"\0" * SECTOR_SIZE
        self.cell_pos = 0  # position of the sync in the cell stream


class TrackReport:
    def __init__(self, cell_count):
        self.cell_count = cell_count
        self.sectors = {}  # number -> Sector (first good one wins)
        self.bad = []  # sectors found with a checksum problem
        self.sync_count = 0
        self.weak_cells = 0
        self.nonstandard_syncs = []  # positions of sync-like patterns that did not start a valid sector

    @property
    def good(self):
        return sorted(n for n, s in self.sectors.items() if s.header_ok and s.data_ok)

    @property
    def standard(self):
        return len(self.good) == SECTORS


def _read_longs(bits, start, count):
    return [_bits_value(bits, start + 32 * i, 32) for i in range(count)]


def decode_track(cells, expect_track=None):
    """Find and decode every Amiga sector in a track (circular). Returns a TrackReport."""
    rep = TrackReport(len(cells))
    rep.weak_cells = sum(1 for c in cells if c == 2)
    bits = _cells_to_int_lists(cells)
    n = len(bits)
    if n < 4000:
        return rep
    # find 0x4489 0x4489 (two consecutive sync words), circularly
    word = 0
    ext = bits + bits[:RAW_SECTOR_BITS + 64]
    syncs = []
    for i, b in enumerate(ext):
        word = ((word << 1) | b) & 0xFFFFFFFF
        if i >= 31 and word == (SYNC << 16 | SYNC) and i - 31 < n:
            syncs.append(i + 1)  # cell index just after the second sync word
    rep.sync_count = len(syncs)
    for s in syncs:
        sec = Sector()
        sec.cell_pos = s - 32
        # the sync word is followed immediately by the odd/even info longs
        info_odd, info_even = _read_longs(ext, s, 2)
        label_odd = _read_longs(ext, s + 64, 4)
        label_even = _read_longs(ext, s + 192, 4)
        hdr_odd, hdr_even = _read_longs(ext, s + 320, 2)
        dat_odd, dat_even = _read_longs(ext, s + 384, 2)
        info = _decode_long(info_odd, info_even)
        sec.number = (info >> 8) & 0xFF
        sec.track_no = (info >> 16) & 0xFF
        sec.sectors_to_gap = info & 0xFF
        fmt = (info >> 24) & 0xFF
        sec.label = b"".join(_decode_long(o, e).to_bytes(4, "big") for o, e in zip(label_odd, label_even))
        chk = 0
        for v in [info_odd, info_even] + label_odd + label_even:
            chk ^= v
        chk &= 0x55555555
        sec.header_ok = fmt == 0xFF and _decode_long(hdr_odd, hdr_even) == chk and sec.number < SECTORS \
            and sec.sectors_to_gap <= SECTORS
        if sec.header_ok and expect_track is not None and sec.track_no != expect_track:
            sec.header_ok = False
        data_raw = _read_longs(ext, s + 448, 256)
        dchk = 0
        for v in data_raw:
            dchk ^= v
        dchk &= 0x55555555
        sec.data_ok = _decode_long(dat_odd, dat_even) == dchk
        odd, even = data_raw[:128], data_raw[128:]
        sec.data = b"".join(_decode_long(o, e).to_bytes(4, "big") for o, e in zip(odd, even))
        if sec.header_ok and sec.data_ok:
            rep.sectors.setdefault(sec.number, sec)
        else:
            rep.bad.append(sec)
    return rep


def decode_image(tracks, num_tracks=160):
    """tracks: {(cyl, head): cells}. Returns (adf bytes, {track_no: TrackReport}). Missing sectors are zero-filled."""
    out = bytearray(num_tracks * SECTORS * SECTOR_SIZE)
    reports = {}
    for (cyl, head), cells in tracks.items():
        no = cyl * 2 + head
        if no >= num_tracks:
            continue
        rep = decode_track(cells, no)
        reports[no] = rep
        for num, sec in rep.sectors.items():
            o = (no * SECTORS + num) * SECTOR_SIZE
            out[o:o + SECTOR_SIZE] = sec.data
    return bytes(out), reports


# ------------------------------------------------------------------------------------------------ encoder

def mfm_encode_bytes(data, prev_bit=0):
    """Plain MFM of decoded bytes -> list of cells (16 per byte). Returns (cells, last data bit)."""
    cells = []
    prev = prev_bit
    for byte in data:
        for k in range(7, -1, -1):
            bit = (byte >> k) & 1
            cells.append(0 if (prev or bit) else 1)  # clock
            cells.append(bit)
            prev = bit
    return cells, prev


def split_odd_even(longs):
    """Amiga odd/even arrangement of 32-bit values: all odd-bit halves first, then all even-bit halves."""
    return [(v >> 1) & 0x55555555 for v in longs] + [v & 0x55555555 for v in longs]


def encode_raw_longs(vals, prev_bit=0):
    """Raw MFM cells (32 per long) of values whose data bits sit at the 0x55555555 positions (cell 0 of each long is
    a clock cell, cell 1 a data cell, ...). Returns (cells, last data bit)."""
    cells = []
    prev = prev_bit
    for v in vals:
        for k in range(16):
            bit = (v >> (30 - 2 * k)) & 1
            cells.append(0 if (prev or bit) else 1)
            cells.append(bit)
            prev = bit
    return cells, prev


def _xor_all(vals):
    r = 0
    for v in vals:
        r ^= v
    return r & 0x55555555


def amiga_sector_longs(track_no, sector, data, label=b"\0" * 16):
    """The raw longs of one sector after its sync words (data bits at 0x55555555; clock bits are added by
    encode_raw_longs). The checksums only involve the data bit positions, so they are the XOR of the split values."""
    info = split_odd_even([0xFF000000 | (track_no << 16) | (sector << 8) | (SECTORS - sector)])
    lab = split_odd_even([int.from_bytes(label[i:i + 4], "big") for i in range(0, 16, 4)])
    dat = split_odd_even([int.from_bytes(data[i:i + 4], "big") for i in range(0, 512, 4)])
    hchk = _xor_all(info + lab)
    dchk = _xor_all(dat)
    return info + lab + [0, hchk, 0, dchk] + dat


def encode_track(sector_data, track_no, gap_bytes=700, rotate=0):
    """Encode 11 sectors (512-byte blocks) into a cell list: per sector 2 gap bytes, the raw sync, the body; a gap
    follows the last sector. rotate shifts the index position (in cells)."""
    cells = []
    prev = 0
    for sec, data in enumerate(sector_data):
        c, prev = mfm_encode_bytes(b"\x00\x00", prev)
        cells += c
        cells += [int(b) for b in format(SYNC, "016b")] * 2  # raw sync words
        prev = 1
        c, prev = encode_raw_longs(amiga_sector_longs(track_no, sec, data), prev)
        cells += c
    c, prev = mfm_encode_bytes(b"\x00" * gap_bytes, prev)
    cells += c
    if rotate:
        rotate %= len(cells)
        cells = cells[rotate:] + cells[:rotate]
    return cells


def adf_sha1(adf):
    return hashlib.sha1(adf).hexdigest()


def main(argv):
    import ipf
    args = argv[1:]
    adf_out = None
    show = False
    paths = []
    i = 0
    while i < len(args):
        if args[i] == "--adf":
            adf_out = args[i + 1]
            i += 2
        elif args[i] == "--tracks":
            show = True
            i += 1
        else:
            paths.append(args[i])
            i += 1
    if len(paths) != 1:
        print(__doc__)
        return 2
    with open(paths[0], "rb") as f:
        img = ipf.parse_ipf(f.read())
    tracks = {k: t.cells for k, t in img.tracks.items()}
    adf, reports = decode_image(tracks)
    standard = [n for n, r in sorted(reports.items()) if r.standard]
    odd = [n for n, r in sorted(reports.items()) if not r.standard]
    print("tracks in image: %d, fully standard (11 good sectors): %d, others: %d" % (
        len(reports), len(standard), len(odd)))
    if show or odd:
        for n in sorted(reports):
            r = reports[n]
            if r.standard and not show:
                continue
            t = img.tracks[(n // 2, n % 2)]
            print("track %3d (cyl %2d hd %d) type %d cells %6d syncs %2d good %2d bad %2d weak %5d timings %s" % (
                n, n // 2, n % 2, t.type, r.cell_count, r.sync_count, len(r.good), len(r.bad), r.weak_cells,
                "/".join(str(x) for x in sorted(set(t.times)))))
    print("adf sha1 %s" % adf_sha1(adf))
    if adf_out:
        with open(adf_out, "wb") as f:
            f.write(adf)
        print("wrote", adf_out)
    return 0


if __name__ == "__main__":
    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    sys.exit(main(sys.argv))
