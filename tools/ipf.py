#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause AND GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Portions derived from MAME's src/lib/formats/ipf_dsk.cpp, license:BSD-3-Clause, copyright-holders:Olivier Galibert
# (the record layouts, gap/data description decoding and track timing rules below follow that file).
#
# IPF (SPS/CAPS) reader: parses the CAPS/INFO/IMGE/DATA records, verifies every CRC, and rebuilds each track as a
# stream of MFM cells (1 = flux transition in the cell, 0 = none, 2 = unreadable/weak cell) with a nominal cell
# time in nanoseconds. It does NOT use or link the SPS capsimg library (whose licence forbids redistribution).
# tools/mfm.py turns the cell streams into Amiga sectors.
#
#   python3 tools/ipf.py FILE.ipf        # prints the header and one line per track
import struct
import sys
import zlib

MG_0, MG_1, MG_N = 0, 1, 2  # cell values


class IpfError(Exception):
    pass


def crc32r(data):
    return zlib.crc32(data) & 0xFFFFFFFF


class IpfTrack:
    def __init__(self):
        self.cylinder = self.head = self.type = 0
        self.size_bytes = self.size_cells = 0
        self.index_bytes = self.index_cells = 0
        self.datasize_cells = self.gapsize_cells = 0
        self.block_count = self.weak_bits = 0
        self.data_size_bits = 0
        self.info_set = False
        self.data = None
        self.cells = []  # MG_0 / MG_1 / MG_N
        self.times = []  # ns per cell (nominal 2000)

    @property
    def number(self):
        return self.cylinder * 2 + self.head

    @property
    def weak_cells(self):
        return sum(1 for c in self.cells if c == MG_N)


class IpfImage:
    def __init__(self):
        self.info = {}
        self.tracks = {}  # (cyl, head) -> IpfTrack


def _u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def _rb(d, pos, count):
    v = 0
    for _ in range(count):
        v = (v << 8) | d[pos]
        pos += 1
    return v, pos


def _write_raw(track, tpos, data, cells, context):
    for i in range(cells):
        track[tpos + i] = MG_1 if data[i >> 3] & (0x80 >> (i & 7)) else MG_0
    if cells:
        context = track[tpos + cells - 1] == MG_1
    return tpos + cells, context


def _write_mfm(track, tpos, data, start_offset, patlen, cells, context):
    patlen *= 2
    start_offset &= 0xFFFFFFFF  # the reference computes (i + start_offset) in uint32_t
    for i in range(cells):
        pos = ((i + start_offset) & 0xFFFFFFFF) % patlen
        bit = bool(data[pos >> 4] & (0x80 >> ((pos >> 1) & 7)))
        if pos & 1:
            track[tpos + i] = MG_1 if bit else MG_0
            context = bit
        else:
            track[tpos + i] = MG_0 if (context or bit) else MG_1
    return tpos + cells, context


def _write_weak(track, tpos, cells):
    for i in range(cells):
        track[tpos + i] = MG_N
    return tpos + cells


def _generate_block_data(d, pos, dlimit, track, tpos, tlimit, dmb, raw, context):
    while True:
        if pos >= dlimit:
            return False, context
        val = d[pos]
        pos += 1
        if (val >> 5) > dlimit - pos:
            return False, context
        param, pos = _rb(d, pos, val >> 5)
        tleft = tlimit - tpos
        bitcount = param if dmb else param * 8
        bytecount = (bitcount + 7) // 8
        op = val & 0x1F
        if op == 0:
            return tleft == 0, context
        if op == 1:  # sync mark, unencoded cells
            if raw or bitcount > tleft or bytecount > dlimit - pos:
                return False, context
            tpos, context = _write_raw(track, tpos, d[pos:pos + bytecount], bitcount, context)
            pos += bytecount
        elif op in (2, 3):  # MFM-encoded data / gap bytes
            if raw or 2 * bitcount > tleft or bytecount > dlimit - pos:
                return False, context
            tpos, context = _write_mfm(track, tpos, d[pos:pos + bytecount], 0, bitcount, 2 * bitcount, context)
            pos += bytecount
        elif op == 4:  # raw cell bits, size in bytes whatever the size mode
            if not raw:
                return False, context
            bitcount = param * 8
            bytecount = param
            if bitcount > tleft or bytecount > dlimit - pos:
                return False, context
            tpos, context = _write_raw(track, tpos, d[pos:pos + bytecount], bitcount, context)
            pos += bytecount
        elif op == 5:  # weak bytes
            if raw or 2 * bitcount > tleft:
                return False, context
            tpos = _write_weak(track, tpos, 2 * bitcount)
            context = False
        else:
            return False, context


def _gap_reserved_size(d, pos, dlimit):
    res = 0
    while True:
        if pos >= dlimit:
            return None, pos
        val = d[pos]
        pos += 1
        if (val >> 5) > dlimit - pos:
            return None, pos
        param, pos = _rb(d, pos, val >> 5)
        op = val & 0x1F
        if op == 0:
            return res, pos
        if op == 1:
            res += param * 2
        elif op == 2:
            pos += (param + 7) // 8
        else:
            return None, pos


def _gap_from_description(d, pos, dlimit, track, tpos, size, pre, context):
    """Returns (ok, newpos, context)."""
    res, _ = _gap_reserved_size(d, pos, dlimit)
    if res is None or res > size:
        return False, pos, context
    pattern = bytearray(16)
    pattern_size = 0
    p = 0
    block_size = 0
    while True:
        if pos >= dlimit:
            return False, pos, context
        val = d[pos]
        pos += 1
        param, pos = _rb(d, pos, val >> 5)
        op = val & 0x1F
        if op == 0:
            return size == p, pos, context
        if op == 1:
            if block_size:
                return False, pos, context
            block_size = param * 2
            pattern_size = 0
        elif op == 2:
            pattern_size = param
            if pattern_size > 16 * 8:
                return False, pos, context
            nb = (pattern_size + 7) // 8
            pattern[:nb] = d[pos:pos + nb]
            pos += nb
            if pre:
                if pos < dlimit and not (d[pos] & 0x1F):
                    block_size = size - p
                elif not block_size:
                    block_size = pattern_size
                if p + block_size > size:
                    return False, pos, context
                _, context = _write_mfm(track, tpos + p, pattern, 0, pattern_size, block_size, context)
                p += block_size
            else:
                if p == 0 and block_size and res != size:
                    block_size = size - (res - block_size)
                if not block_size:
                    block_size = size - res
                if p + block_size > size:
                    return False, pos, context
                _, context = _write_mfm(track, tpos + p, pattern, -block_size, pattern_size, block_size, context)
                p += block_size
            block_size = 0
        else:
            return False, pos, context


def _gap0(track, tpos, gap_cells, pattern, ipos, context):
    spos = ipos if (ipos >= 16 and ipos + 16 <= gap_cells) else gap_cells >> 1
    pat = bytes([pattern & 0xFF])
    t, context = _write_mfm(track, tpos, pat, 0, 8, spos, context)
    delta = 0
    if gap_cells & 1:
        track[t] = MG_0
        t += 1
        delta = 1
    _write_mfm(track, t, pat, spos + delta - gap_cells, 8, gap_cells - spos - delta, context)
    return True, spos, context


def _gap1(track, tpos, gap_cells, ipos, d, pos, dlimit, context):
    spos = ipos if (ipos >= 16 and ipos < gap_cells - 16) else 0
    ok, _, context = _gap_from_description(d, pos, dlimit, track, tpos, gap_cells, True, context)
    return ok, spos, context


def _gap2(track, tpos, gap_cells, ipos, d, pos, dlimit, context):
    spos = ipos if (ipos >= 16 and ipos < gap_cells - 16) else gap_cells
    ok, _, context = _gap_from_description(d, pos, dlimit, track, tpos, gap_cells, False, context)
    return ok, spos, context


def _gap3(track, tpos, gap_cells, ipos, d, pos, dlimit, context):
    presize, p1 = _gap_reserved_size(d, pos, dlimit)
    if presize is None:
        return False, 0, context
    postsize, _ = _gap_reserved_size(d, p1, dlimit)
    if postsize is None:
        return False, 0, context
    delta = gap_cells & 1
    usable = gap_cells - delta
    if presize + postsize > usable:
        rem = presize + postsize - usable
        rem0 = rem >> 1
        rem1 = rem - rem0
        if presize < rem0:
            rem1 += rem0 - presize
            rem0 = presize
        if postsize < rem1:
            rem0 += rem1 - postsize
            rem1 = postsize
        presize -= rem0
        postsize -= rem1
    if ipos >= 16 and ipos < gap_cells - 16:
        spos = ipos
        if spos < presize:
            spos = presize
        if spos > usable - postsize:
            spos = usable - postsize
    else:
        spos = presize + (usable - presize - postsize) // 2
    ok, pos, context = _gap_from_description(d, pos, dlimit, track, tpos, spos, True, context)
    if not ok:
        return False, spos, context
    if delta:
        track[tpos + spos] = MG_0
    ok, pos, context = _gap_from_description(d, pos, dlimit, track, tpos + spos + delta, gap_cells - spos - delta,
                                              False, context)
    return ok, spos, context


def _generate_block(t, idx, ipos, track, pos, context, encoder_type):
    d = t.data
    dend = len(d)
    th = 32 * idx
    data_cells = _u32(d, th)
    gap_cells = _u32(d, th + 4)
    if gap_cells < 8:
        gap_cells = 0
    encoder = _u32(d, th + 16)
    if encoder not in (1, 2):
        return None
    raw = encoder == 2
    flags = 0 if encoder_type == 1 else _u32(d, th + 20)
    gap_type = flags & 3
    dmb = bool(flags & 4)
    dpos = pos
    gpos = dpos + data_cells
    pos = gpos + gap_cells
    if pos > t.size_cells:
        return None
    ok, context = _generate_block_data(d, _u32(d, th + 28), dend, track, dpos, gpos, dmb, raw, context)
    if not ok:
        return None
    # the gap-description offset and the type-0 pattern share the thead fields
    gip = ipos - gpos if ipos > gpos else 0
    gdesc = _u32(d, th + 8)
    pattern = _u32(d, th + 24)
    if gap_type == 0:
        ok, spos, context = _gap0(track, gpos, gap_cells, pattern, gip, context)
    elif gap_type == 1:
        ok, spos, context = _gap1(track, gpos, gap_cells, gip, d, gdesc, dend, context)
    elif gap_type == 2:
        ok, spos, context = _gap2(track, gpos, gap_cells, gip, d, gdesc, dend, context)
    elif gap_type == 3:
        ok, spos, context = _gap3(track, gpos, gap_cells, gip, d, gdesc, dend, context)
    else:
        ok = False
    if not ok:
        return None
    return pos, dpos, gpos, spos + gpos, context


def _block_real_size(t):
    size = 0
    for i in range(t.block_count):
        dc = _u32(t.data, 32 * i)
        gc = _u32(t.data, 32 * i + 4)
        if gc < 8:
            gc = 0
        size += dc + gc
    return size


def _timing_set(times, start, end, value):
    for i in range(start, end):
        times[i] = value


def _generate_timings(t, times, data_pos, gap_pos):
    _timing_set(times, 0, t.size_cells, 2000)
    bc = t.block_count
    ty = t.type
    if ty == 2:
        pass
    elif ty == 3:
        if bc >= 5:
            _timing_set(times, gap_pos[3], data_pos[4], 1890)
            _timing_set(times, data_pos[4], gap_pos[4], 1890)
        if bc >= 6:
            _timing_set(times, gap_pos[4], data_pos[5], 1990)
            _timing_set(times, data_pos[5], gap_pos[5], 1990)
        if bc >= 7:
            _timing_set(times, gap_pos[5], data_pos[6], 2090)
            _timing_set(times, data_pos[6], gap_pos[6], 2090)
    elif ty == 4:
        _timing_set(times, gap_pos[bc - 1], t.size_cells, 1890)
        _timing_set(times, data_pos[0], gap_pos[0], 1890)
        if bc >= 2:
            _timing_set(times, gap_pos[0], data_pos[1], 1990)
            _timing_set(times, data_pos[1], gap_pos[1], 1990)
        if bc >= 3:
            _timing_set(times, gap_pos[1], data_pos[2], 2090)
            _timing_set(times, data_pos[2], gap_pos[2], 2090)
    elif ty == 5:
        if bc >= 6:
            _timing_set(times, data_pos[5], gap_pos[5], 2100)
    elif ty == 6:
        if bc >= 2:
            _timing_set(times, data_pos[1], gap_pos[1], 2200)
        if bc >= 3:
            _timing_set(times, data_pos[2], gap_pos[2], 1800)
    elif ty == 7:
        if bc >= 2:
            _timing_set(times, data_pos[1], gap_pos[1], 2100)
    elif ty == 8:
        for blk, ns in ((1, 2200), (2, 2100), (4, 1900), (5, 1800), (6, 1700)):
            if bc >= blk + 1:
                _timing_set(times, data_pos[blk], gap_pos[blk], ns)
    elif ty == 9:
        mask = _u32(t.data, 24)
        for i in range(1, bc):
            _timing_set(times, data_pos[i], gap_pos[i], 1900 if mask & (1 << (i - 1)) else 2100)
    else:
        return False
    return True


def _generate_track(t, encoder_type):
    if not t.size_cells:
        return True
    if t.type == 1:  # noise / unformatted
        t.cells = [MG_N] * t.size_cells
        t.times = [2000] * t.size_cells
        return True
    if len(t.data) < 32 * t.block_count:
        return False
    t.size_cells = _block_real_size(t)
    if t.index_cells >= t.size_cells:
        return False
    track = [0] * t.size_cells
    times = [0] * t.size_cells
    data_pos = [0] * (t.block_count + 1)
    gap_pos = [0] * t.block_count
    splice_pos = [0] * t.block_count
    context = False
    pos = 0
    for i in range(t.block_count):
        ipos = t.size_cells - t.index_cells if i == t.block_count - 1 else 0xFFFFFFFF
        r = _generate_block(t, i, ipos, track, pos, context, encoder_type)
        if r is None:
            return False
        pos, data_pos[i], gap_pos[i], splice_pos[i], context = r
    if pos != t.size_cells:
        return False
    data_pos[t.block_count] = pos
    # mark_track_splice: the write splice flips three cells
    sp = splice_pos[t.block_count - 1]
    for i in range(3):
        k = (sp + i) % t.size_cells
        if track[k] == MG_0:
            track[k] = MG_1
        elif track[k] == MG_1:
            track[k] = MG_0
    if not _generate_timings(t, times, data_pos, gap_pos):
        return False
    if t.index_cells:
        off = t.size_cells - t.index_cells
        track = track[off:] + track[:off]
        times = times[off:] + times[:off]
    t.cells = track
    t.times = times
    return True


def parse_ipf(data):
    """Parse an IPF file (bytes) into an IpfImage; raises IpfError on any structural or CRC problem."""
    data = bytearray(data)
    img = IpfImage()
    infos = {}
    pos = 0
    info = {}
    while pos != len(data):
        if len(data) - pos < 12:
            raise IpfError("truncated record header")
        tag = bytes(data[pos:pos + 4])
        tsize = _u32(data, pos + 4)
        if tsize < 12 or len(data) - pos < tsize:
            raise IpfError("bad record size for %r" % tag)
        crc = _u32(data, pos + 8)
        rec = bytearray(data[pos:pos + tsize])
        rec[8:12] = b"\0\0\0\0"
        if crc32r(bytes(rec)) != crc:
            raise IpfError("CRC error in %r record at %d" % (tag, pos))
        rec = bytes(rec)
        pos += tsize
        if tag == b"CAPS":
            if tsize != 12:
                raise IpfError("bad CAPS record")
        elif tag == b"INFO":
            if tsize != 96:
                raise IpfError("bad INFO record")
            info = dict(type=_u32(rec, 12), encoder_type=_u32(rec, 16), encoder_revision=_u32(rec, 20),
                        release=_u32(rec, 24), revision=_u32(rec, 28), origin=_u32(rec, 32),
                        min_cylinder=_u32(rec, 36), max_cylinder=_u32(rec, 40), min_head=_u32(rec, 44),
                        max_head=_u32(rec, 48), credit_day=_u32(rec, 52), credit_time=_u32(rec, 56),
                        platform=[_u32(rec, 60 + 4 * i) for i in range(4)],
                        extra=[_u32(rec, 76 + 4 * i) for i in range(5)])
            if info["type"] != 1 or info["encoder_type"] not in (1, 2) or info["encoder_revision"] != 1:
                raise IpfError("unsupported INFO (type/encoder)")
            img.info = info
        elif tag == b"IMGE":
            if tsize != 80:
                raise IpfError("bad IMGE record")
            idx = _u32(rec, 64)
            if idx > 1000:
                raise IpfError("track index out of range")
            t = infos.setdefault(idx, IpfTrack())
            t.info_set = True
            t.cylinder = _u32(rec, 12)
            t.head = _u32(rec, 16)
            if not (info["min_cylinder"] <= t.cylinder <= info["max_cylinder"]) or \
                    not (info["min_head"] <= t.head <= info["max_head"]):
                raise IpfError("track outside the image geometry")
            t.type = _u32(rec, 20)
            if _u32(rec, 24) != 1 or _u32(rec, 56) != 0:
                raise IpfError("unsupported signal type or encoder process")
            t.size_bytes = _u32(rec, 28)
            t.index_bytes = _u32(rec, 32)
            t.index_cells = _u32(rec, 36)
            t.datasize_cells = _u32(rec, 40)
            t.gapsize_cells = _u32(rec, 44)
            t.size_cells = _u32(rec, 48)
            t.block_count = _u32(rec, 52)
            t.weak_bits = _u32(rec, 60)
        elif tag == b"DATA":
            if tsize != 28:
                raise IpfError("bad DATA record")
            idx = _u32(rec, 24)
            if idx > 1000:
                raise IpfError("track index out of range")
            t = infos.setdefault(idx, IpfTrack())
            t.data_size_bits = _u32(rec, 16)
            n = _u32(rec, 12)
            if n > len(data) - pos:
                raise IpfError("DATA payload truncated")
            payload = bytes(data[pos:pos + n])
            if crc32r(payload) != _u32(rec, 20):
                raise IpfError("DATA payload CRC error (track index %d)" % idx)
            t.data = payload
            pos += n
        # unknown tags (TRCK, CTEX, CTEI...) carry nothing needed for decoding
    for idx in sorted(infos):
        t = infos[idx]
        if t.info_set and t.data is not None:
            if not _generate_track(t, info["encoder_type"]):
                raise IpfError("could not rebuild cylinder %d head %d" % (t.cylinder, t.head))
            img.tracks[(t.cylinder, t.head)] = t
        elif t.info_set or t.data is not None:
            raise IpfError("IMGE/DATA mismatch for track index %d" % idx)
    return img


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        img = parse_ipf(f.read())
    i = img.info
    print("IPF release %d revision %d, encoder %d, cyl %d-%d, heads %d-%d, platform %s" % (
        i["release"], i["revision"], i["encoder_type"], i["min_cylinder"], i["max_cylinder"], i["min_head"],
        i["max_head"], i["platform"]))
    for key in sorted(img.tracks):
        t = img.tracks[key]
        timings = sorted(set(t.times))
        print("cyl %2d head %d  type %d  cells %6d  index %6d  blocks %2d  weak %5d  timing %s" % (
            t.cylinder, t.head, t.type, len(t.cells), t.index_cells, t.block_count, t.weak_cells,
            "/".join(str(x) for x in timings)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
