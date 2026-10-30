#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Catalogue the extra Xenon 2 Amiga images staged under generated/sources-extra/ (docs/re-notes/sources.md).

usage: catalog_extra.py [DIR_OR_FILE ...] [--json OUT.json]
Default root: generated/sources-extra. For every file it prints size, SHA-1 and what it is:
  ADF / Disk.N  boot block, DOS type, volume name, file list, XENON-II executable hash and whether it has the
                packed-executable shape the importer depacks
  IPF           the CAPS record chain: INFO (encoder, release, tracks, platform, disk number) and the number of
                IMGE/DATA records, and whether the chain ends exactly at the end of the file
  ZIP           members, each listed and hashed (nested archives one level)
No game bytes are written anywhere; the output is hashes and names only (read-only on every input).
"""
import hashlib
import json
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import adf as adflib
except Exception:  # pragma: no cover
    adflib = None

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "generated", "sources-extra")


def digests(data):
    return {"size": len(data), "sha1": hashlib.sha1(data).hexdigest(), "sha256": hashlib.sha256(data).hexdigest()}


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def bootblock_ok(b):
    s = 0
    for i in range(0, 1024, 4):
        s += be32(b, i)
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    return s == 0xFFFFFFFF


def xenon_exe_shape(exe):
    """Same structural test as is_xenon_exe() in runtime/import/importer.cpp."""
    if len(exe) < 64 or be32(exe, 0) != 0x3F3:
        return False
    p = 4
    while True:
        if p + 4 > len(exe):
            return False
        n = be32(exe, p)
        p += 4
        if not n:
            break
        if n > 256:
            return False
        p += 4 * n
    if p + 12 > len(exe):
        return False
    first, last = be32(exe, p + 4), be32(exe, p + 8)
    if last < first or last - first > 64:
        return False
    p += 12 + 4 * (last - first + 1)
    if p + 8 > len(exe) or (be32(exe, p) & 0x3FFFFFFF) != 0x3E9:
        return False
    code_len = (be32(exe, p + 4) & 0x3FFFFFFF) * 4
    code = p + 8
    if code_len > len(exe) - code or code_len < 16:
        return False
    return struct.unpack_from(">H", exe, code)[0] == 0x41FA and struct.unpack_from(">H", exe, code + 4)[0] == 0xD1FC


def describe_adf(data, name=""):
    d = {"kind": "disk", "bootblock_checksum_ok": bootblock_ok(data[:1024]) if len(data) >= 1024 else False}
    d["dostype"] = data[:4].hex() if len(data) >= 4 else ""
    d["is_dos"] = data[:3] == b"DOS"
    if len(data) != 901120:
        d["note"] = "not an 880 KB image (%d bytes)" % len(data)
        return d
    if adflib is None:
        return d
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".adf") as tf:
        tf.write(data)
        tf.flush()
        try:
            a = adflib.Adf(tf.name)
            ri = a.root_info()
            d["root_type_ok"] = ri["type"] == 2 and ri["sec_type"] == 1
            d["root_checksum_ok"] = ri["checksum_ok"]
            d["volume"] = ri["name"]
            files = []
            for e in a.walk():
                files.append(e)
            d["n_entries"] = len(files)
            names = []
            for e in files:
                nm = e.get("path") if isinstance(e, dict) else getattr(e, "path", str(e))
                names.append(nm)
            d["files"] = sorted(names)[:60]
            exe = None
            for e in files:
                nm = e.get("path") if isinstance(e, dict) else getattr(e, "path", "")
                if nm and nm.split("/")[-1].upper() in ("XENON-II", "XENON_II", "XENON2", "XENONII"):
                    try:
                        exe = a.read_file(e)
                    except Exception as ex:  # noqa
                        d["exe_error"] = str(ex)
                    d["exe_name"] = nm
                    break
            if exe:
                d["exe"] = digests(exe)
                d["exe_packed_shape"] = xenon_exe_shape(exe)
        except Exception as ex:
            d["parse_error"] = str(ex)
    return d


def describe_ipf(data):
    d = {"kind": "ipf"}
    p = 0
    n = {"IMGE": 0, "DATA": 0}
    try:
        while p + 12 <= len(data):
            tag = data[p:p + 4].decode("latin-1")
            ln = be32(data, p + 4)
            if ln < 12:
                d["bad_record"] = (tag, ln, p)
                break
            if tag == "CAPS":
                pass
            elif tag == "INFO":
                v = struct.unpack_from(">21I", data, p + 12)
                d["info"] = dict(media_type=v[0], encoder_type=v[1], encoder_rev=v[2], file_key=v[3], file_rev=v[4],
                                 origin=v[5], min_track=v[6], max_track=v[7], min_side=v[8], max_side=v[9],
                                 date=v[10], time=v[11], platforms=list(v[12:16]), disk_number=v[16], creator=v[17])
            elif tag in n:
                n[tag] += 1
            adv = ln
            if tag == "DATA":
                adv += be32(data, p + 12)
            p += adv
    except Exception as ex:
        d["error"] = str(ex)
    d["records"] = n
    d["chain_ends_at_eof"] = p == len(data)
    return d


def describe(name, data, depth=0):
    base = {"name": name, **digests(data)}
    low = name.lower()
    if data[:4] == b"CAPS":
        base.update(describe_ipf(data))
    elif data[:2] == b"PK" and depth < 2:
        try:
            import io
            z = zipfile.ZipFile(io.BytesIO(data))
            base["kind"] = "zip"
            base["members"] = [describe(i.filename, z.read(i), depth + 1) for i in z.infolist() if not i.filename.endswith("/")]
        except Exception as ex:
            base["error"] = str(ex)
    elif len(data) == 901120 and (low.endswith(".adf") or data[:3] == b"DOS" or True):
        base.update(describe_adf(data, name))
    elif low.endswith((".lha", ".lzh")):
        base["kind"] = "lha"
    elif len(data) in (1085184,) or low.startswith("disk."):
        base.update(describe_adf(data, name))
    return base


def walk(paths):
    for p in paths:
        if os.path.isdir(p):
            for r, ds, fs in os.walk(p):
                ds.sort()
                for f in sorted(fs):
                    yield os.path.join(r, f)
        else:
            yield p


def main(argv):
    out = None
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--json":
            out = argv[i + 1]
            i += 2
            continue
        args.append(argv[i])
        i += 1
    paths = args or [os.path.normpath(ROOT)]
    rows = []
    for f in walk(paths):
        if f.endswith((".txt",)) or "/x/" in f.replace("\\", "/") and f.endswith((".info",)):
            continue
        with open(f, "rb") as fh:
            data = fh.read()
        rows.append(describe(f, data))
    if out:
        with open(out, "w") as fh:
            json.dump(rows, fh, indent=1)
    for r in rows:
        extra = ""
        if r.get("kind") == "ipf":
            inf = r.get("info", {})
            extra = " IPF tracks %s-%s disk %s enc %s rev %s imge=%d data=%d eof=%s" % (
                inf.get("min_track"), inf.get("max_track"), inf.get("disk_number"), inf.get("encoder_type"),
                inf.get("encoder_rev"), r["records"]["IMGE"], r["records"]["DATA"], r["chain_ends_at_eof"])
        elif r.get("kind") == "disk":
            extra = " vol=%r dos=%s bb=%s exe=%s" % (r.get("volume"), r.get("dostype"), r.get("bootblock_checksum_ok"),
                                                      (r.get("exe") or {}).get("sha1", "-")[:12])
        print("%s  %9d  %s%s" % (r["sha1"], r["size"], os.path.relpath(r["name"]), extra))
        for m in r.get("members", []):
            print("    %s  %9d  %s" % (m["sha1"][:12], m["size"], m["name"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
