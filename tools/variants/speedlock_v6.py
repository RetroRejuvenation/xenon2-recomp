# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Decoder for the one-disk 'Power Pack' release of Xenon 2 (SPS 2234; floppy version v6 of the WHDLoad imager).

The disk has a standard track 0, a Speedlock key track on track 1, and custom data on tracks 2-159: one raw MFM
block per track, a `$4489` sync, four more raw bytes, then `$1B00` data bytes stored as an 'even' block of raw longs,
a long checksum, an 'odd' block and a second checksum (track 2 uses `$1800`). The layout is public: it is written
down in the WHDLoad imager source for Xenon 2 (public domain, by Wepl), which this module follows. The data itself is
two ByteKiller-style crunchers (an MSB-first flavour for the intro and the main image, an LSB-first flavour for the
six game files) plus a marker-based RLE for the game files.

No game data is stored here. Functions take bytes the user already has and return bytes in memory.
"""
import struct

M32 = 0xFFFFFFFF
M55 = 0x55555555
DATA_BYTES = 0x1B00
FIRST_DATA_TRACK = 3


def _longs(bits, start, count):
    n = len(bits)
    out = []
    for k in range(count):
        v = 0
        for j in range(32):
            v = (v << 1) | bits[(start + 32 * k + j) % n]
        out.append(v)
    return out


def find_sync(bits, word=0x4489):
    v = 0
    for i in range(15):
        v = (v << 1) | bits[i]
    for i in range(15, len(bits)):
        v = ((v << 1) | bits[i]) & 0xFFFF
        if v == word:
            return i + 1
    return None


def decode_custom_track(bits, data_bytes=DATA_BYTES, skip_bits=32):
    """One custom data track (list of 0/1 cells) -> (bytes, checksum_ok), or (None, False) with no sync."""
    p = find_sync(bits)
    if p is None:
        return None, False
    nlongs = data_bytes // 4
    s = p + skip_bits
    even = _longs(bits, s, nlongs)
    ck1 = _longs(bits, s + 32 * nlongs, 1)[0]
    odd = _longs(bits, s + 32 * nlongs + 32, nlongs)
    ck2 = _longs(bits, s + 64 * nlongs + 32, 1)[0]
    out = [((o & M55) << 1) | (e & M55) for e, o in zip(even, odd)]
    ck = ((ck2 & M55) << 1) | (ck1 & M55)
    return b"".join(struct.pack(">I", x) for x in out), (sum(out) & M32) == ck


def decode_data_tracks(ipf_image):
    """All data tracks 3..159 of an IPF image of the one-disk release.
    Returns (concatenated bytes of tracks 3..159, [track numbers that failed])."""
    chunks = []
    bad = []
    for tn in range(FIRST_DATA_TRACK, 160):
        t = ipf_image.tracks.get((tn // 2, tn % 2))
        if t is None:
            bad.append(tn)
            chunks.append(bytes(DATA_BYTES))
            continue
        data, ok = decode_custom_track([1 if c == 1 else 0 for c in t.cells])
        if data is None or not ok:
            bad.append(tn)
        chunks.append(data if data is not None else bytes(DATA_BYTES))
    return b"".join(chunks), bad


class _Reader:
    def __init__(self, packed):
        self.packed = packed
        self.pos = len(packed)
        self.xor = 0

    def long(self):
        if self.pos < 4:
            raise ValueError("crunched stream underrun")
        self.pos -= 4
        v = struct.unpack(">I", self.packed[self.pos:self.pos + 4])[0]
        self.xor ^= v
        return v


def decrunch_msb(packed, unpacked_len):
    """Intro/main cruncher (the slave's `_v6decrunch`): reads from the end, bits taken from the top of each long.
    Returns (bytes, consumed): `consumed` is True when the whole stream was used, which holds for an intact one."""
    rd = _Reader(packed)
    d0 = rd.long()
    out = bytearray(unpacked_len)
    a2 = unpacked_len

    def bit():
        nonlocal d0
        c = (d0 >> 31) & 1
        d0 = (d0 << 1) & M32
        if d0 == 0:
            d0 = rd.long()
            c = (d0 >> 31) & 1
            d0 = ((d0 << 1) | 1) & M32
        return c

    data = _crunch_body(bit, out, a2, unpacked_len)
    return data, rd.pos == 0


def decrunch_lsb(packed, unpacked_len):
    """Game-file cruncher (the loader's own routine): same token set, bits taken from the bottom of each long.
    Returns (bytes, consumed) as for decrunch_msb. (The XOR of the longs read is not zero for these streams, so
    exact consumption is the integrity signal used here.)"""
    rd = _Reader(packed)
    d0 = rd.long()
    out = bytearray(unpacked_len)
    a2 = unpacked_len

    def bit():
        nonlocal d0
        c = d0 & 1
        d0 >>= 1
        if d0 == 0:
            d0 = rd.long()
            c = d0 & 1
            d0 = (d0 >> 1) | 0x80000000
        return c

    data = _crunch_body(bit, out, a2, unpacked_len)
    return data, rd.pos == 0


def _crunch_body(bit, out, a2, total):
    def bits(n):
        v = 0
        for _ in range(n):
            v = (v << 1) | bit()
        return v

    def copy(count, off):
        nonlocal a2
        for _ in range(count + 1):
            a2 -= 1
            s = a2 + off
            out[a2] = out[s] if s < total else 0

    def literals(count):
        nonlocal a2
        for _ in range(count):
            a2 -= 1
            out[a2] = bits(8)

    while a2 > 0:
        if bit() == 0:
            if bit() == 1:
                copy(1, bits(8))
            else:
                literals(bits(3) + 1)
        else:
            sel = bits(2)
            if sel < 2:
                copy(sel + 2, bits(8 + sel))
            elif sel == 3:
                literals(bits(8) + 8 + 1)
            else:
                n = bits(8)
                copy(n, bits(8))
    return bytes(out)


def unrle(src, marker, out_len):
    """The game files' second stage: `marker value count` triples (count 0 = 256) and literal bytes, from the end."""
    out = bytearray(out_len)
    a1 = out_len
    a2 = len(src)
    while a2 > 0:
        a2 -= 1
        b = src[a2]
        if b == marker:
            a2 -= 1
            val = src[a2]
            a2 -= 1
            cnt = src[a2] or 256
            for _ in range(cnt):
                a1 -= 1
                out[a1] = val
        else:
            a1 -= 1
            out[a1] = b
    return bytes(out), a1


def load_game_file(data_tracks, start_track, count, entry):
    """Rebuild one game file. `entry` = (end_of_crunched, crunched_output_len, marker<<24|rle_end, final_len), the
    16-byte row of the game's file-address table. Returns (bytes, ok): ok = the crunched stream was consumed exactly
    and the RLE stage filled the whole output."""
    end_packed, unp_len, rle_word, final_len = entry
    off = (start_track - FIRST_DATA_TRACK) * DATA_BYTES
    blob = data_tracks[off:off + count * DATA_BYTES][:end_packed]
    stage1, ok = decrunch_lsb(blob, unp_len)
    marker, rle_end = rle_word >> 24, rle_word & 0xFFFFFF
    out, left = unrle(stage1[:rle_end], marker, final_len)
    return out, ok and left == 0
