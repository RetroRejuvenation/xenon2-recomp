#!/usr/bin/env python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Extract and catalogue Xenon 2 Amiga disk images.

Usage: python3 -I tools/catalog_sources.py <out_dir> <zip> [<zip> ...]
Each archive (and nested zips) is extracted into its own fresh subdir of
<out_dir>; then every non-zip file is hashed and equal-size files are
compared pairwise per 512-byte sector. Member names are sanitised to their
basename (no path traversal).
"""
import hashlib, io, itertools, os, sys, zipfile

SECTOR = 512


def safe_name(n):
    return os.path.basename(n.replace("\\", "/")) or "unnamed"


def extract(zbytes, dest, label, out):
    os.makedirs(dest, exist_ok=False)
    with zipfile.ZipFile(io.BytesIO(zbytes)) as z:
        for i in z.infolist():
            if i.is_dir():
                continue
            data = z.read(i)
            name = safe_name(i.filename)
            if name.lower().endswith(".zip"):
                sub = os.path.join(dest, "z%02d" % len(os.listdir(dest)))
                extract(data, sub, label + "/" + name, out)
            else:
                p = os.path.join(dest, name)
                with open(p, "wb") as f:
                    f.write(data)
                out.append((label + "/" + name, p, data))


def short(label):
    """Short id: archive tag + file basename tail (nested zip names repeat the adf name)."""
    return label.split("/")[-1][:-4].replace("Xenon II - Megablast", "X2").replace("(1989)(ImageWorks)", "")


def sector_diff(a, b):
    secs, nbytes, first = 0, 0, []
    for s in range(0, min(len(a), len(b)), SECTOR):
        x, y = a[s:s + SECTOR], b[s:s + SECTOR]
        if x != y:
            secs += 1
            nbytes += sum(1 for p, q in zip(x, y) if p != q)
            if len(first) < 8:
                first.append(s // SECTOR)
    return secs, nbytes, first


def main():
    out_dir, zips = sys.argv[1], sys.argv[2:]
    files = []
    for zp in zips:
        with open(zp, "rb") as f:
            extract(f.read(), os.path.join(out_dir, safe_name(zp)[:-4]), safe_name(zp), files)
    print("# files: sha1 size hdr label")
    for lab, p, d in files:
        print("%s %8d %r %s" % (hashlib.sha1(d).hexdigest(), len(d), d[:4], short(lab)))
    print("# equal-size pairs: sectors-differing bytes-differing first-sectors")
    for (la, _, da), (lb, _, db) in itertools.combinations(files, 2):
        if len(da) == len(db):
            s, b, f = sector_diff(da, db)
            print("%s <> %s: sectors=%d bytes=%d first=%s" % (short(la), short(lb), s, b, f))


if __name__ == "__main__":
    main()
