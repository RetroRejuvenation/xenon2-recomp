# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Pasti (.stx) reader for Atari ST disk images: header "RSY\0", track records with 16-byte sector descriptors, sector
# data located relative to the end of the descriptors. raw_image() lays the standard sectors out as a plain .st image.
# (Pasti timing/fuzzy data are ignored: only sector contents are needed.)
import struct


def parse_stx(d):
    if d[:4] != b"RSY\0":
        raise ValueError("not a Pasti image")
    ver, tool, _res, ntr, rev = struct.unpack("<HHHBB", d[4:12])
    off, tracks = 16, {}
    for _ in range(ntr):
        if off + 16 > len(d):
            break
        rs, fz, sc, fl, tl, tn, tt = struct.unpack("<IIHHHBB", d[off:off + 16])
        if rs < 16:
            break
        p = off + 16
        secs = []
        for _i in range(sc):
            do, bp, rt, tr, hd, sn, sz, crc, fdc, _r = struct.unpack("<IHHBBBBHBB", d[p:p + 16])
            p += 16
            secs.append(dict(do=do, bitpos=bp, time=rt, id=(tr, hd, sn, sz), crc=crc, fdc=fdc))
        tracks[(tn & 0x7F, tn >> 7)] = dict(rec=off, sc=sc, flags=fl, secs=secs, base=p)
        off += rs
    return dict(version=ver, tool=tool, revision=rev, tracks=tracks)


def sector_data(d, track, sec):
    n = 128 << sec["id"][3]
    o = track["base"] + sec["do"]
    return d[o:o + n]


def raw_image(d, spt=10, cylinders=80, sides=1):
    info = parse_stx(d)
    img = bytearray(cylinders * sides * spt * 512)
    for (trk, side), t in info["tracks"].items():
        if trk >= cylinders or side >= sides:
            continue
        for s in t["secs"]:
            sn, sz = s["id"][2], s["id"][3]
            if 1 <= sn <= spt and sz == 2:
                o = ((trk * sides + side) * spt + sn - 1) * 512
                img[o:o + 512] = sector_data(d, t, s)
    return bytes(img)


def boot_checksum(img):
    return sum(struct.unpack(">256H", img[:512])) & 0xFFFF  # 0x1234 = bootable
