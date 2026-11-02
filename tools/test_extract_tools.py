#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Regression tests for the extraction developer scripts tools/adf.py and tools/iso9660.py: names carried by an
image must never place a file outside the output directory, loops must terminate, and ordinary images still extract.
Every fixture is synthetic (built here); no disk image or game byte is used.

usage: python3 -I tools/test_extract_tools.py
"""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adf  # noqa: E402
import iso9660  # noqa: E402

BS = 512


def adf_image(dirs=(), files=()):
    """A 901120-byte FFS image. dirs: (block, name, parent_block, hash_slots{slot: child_block}); files: (block, name,
    data_block, payload). The root (block 880) points at the entries listed in `root_slots`, set by the caller."""
    img = bytearray(901120)
    img[0:4] = b"DOS\x01"

    def header(blk, name, sec, parent):
        h = blk * BS
        struct.pack_into(">I", img, h + 0, 2)
        nb = name.encode("latin-1")
        assert len(nb) <= 30
        img[h + BS - 80] = len(nb)
        img[h + BS - 79:h + BS - 79 + len(nb)] = nb
        struct.pack_into(">I", img, h + BS - 12, parent)
        struct.pack_into(">i", img, h + BS - 4, sec)
        return h

    for blk, name, parent, slots in dirs:
        h = header(blk, name, 2, parent)
        for slot, child in slots.items():
            struct.pack_into(">I", img, h + 24 + 4 * slot, child)
    for blk, name, parent, data_blk, payload in files:
        h = header(blk, name, -3, parent)
        struct.pack_into(">I", img, h + 8, 1)
        struct.pack_into(">I", img, h + 16, data_blk)
        struct.pack_into(">I", img, h + BS - 204, data_blk)
        struct.pack_into(">I", img, h + BS - 188, len(payload))
        img[data_blk * BS:data_blk * BS + len(payload)] = payload
    return img


def set_root_slots(img, slots):
    for slot, child in slots.items():
        struct.pack_into(">I", img, 880 * BS + 24 + 4 * slot, child)


def run_adf(img, tmp):
    path = os.path.join(tmp, "t.adf")
    with open(path, "wb") as f:
        f.write(img)
    out = os.path.join(tmp, "a", "b", "out")
    os.makedirs(out)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        adf.cmd_extract(adf.Adf(path), out)
    return out, buf.getvalue()


def files_under(root):
    return sorted(os.path.relpath(os.path.join(d, f), root) for d, _, fs in os.walk(root) for f in fs)


class AdfExtract(unittest.TestCase):
    def test_ordinary_tree_extracts(self):
        img = adf_image(dirs=[(2, "sub", 880, {0: 3})], files=[(3, "ok.txt", 2, 5, b"hello"), (4, "top.bin", 880, 6, b"top")])
        set_root_slots(img, {0: 2, 1: 4})
        with tempfile.TemporaryDirectory() as tmp:
            out, text = run_adf(img, tmp)
            self.assertEqual(files_under(out), [os.path.join("sub", "ok.txt"), "top.bin"])
            with open(os.path.join(out, "sub", "ok.txt"), "rb") as f:
                self.assertEqual(f.read(), b"hello")
            self.assertNotIn("PROBLEM", text)

    def test_traversal_names_are_skipped(self):
        for name in ("../escape.txt", "..", ".", "a/b", "c:x", "back\\slash", "/abs"):
            img = adf_image(files=[(2, name, 880, 5, b"PWNED")])
            set_root_slots(img, {0: 2})
            with tempfile.TemporaryDirectory() as tmp:
                out, text = run_adf(img, tmp)
                self.assertEqual(files_under(os.path.join(tmp)), [os.path.join("t.adf")], name)
                self.assertIn("skipped", text, name)

    def test_directory_loop_terminates(self):
        # directory 2 lists itself in its own hash table
        img = adf_image(dirs=[(2, "d", 880, {0: 2})])
        set_root_slots(img, {0: 2})
        with tempfile.TemporaryDirectory() as tmp:
            out, text = run_adf(img, tmp)
            self.assertIn("reachable twice", text)

    def test_names_are_printed_without_control_characters(self):
        self.assertEqual(adf.vis("a\x1b[2Jb"), "a\\x1b[2Jb")
        self.assertEqual(adf.vis("plain name.txt"), "plain name.txt")


def iso_image(entries):
    """Raw MODE1 image: PVD at LBA 16, root directory at 17, then payload sectors. entries: (name bytes, lba, size, isdir)."""
    RAW = 2352
    sectors = {}

    def rec(name, lba, size, isdir):
        ln = 33 + len(name)
        ln += ln % 2
        r = bytearray(ln)
        r[0] = ln
        struct.pack_into("<I", r, 2, lba)
        struct.pack_into("<I", r, 10, size)
        r[25] = 2 if isdir else 0
        r[32] = len(name)
        r[33:33 + len(name)] = name
        return bytes(r)

    def put(lba, payload):
        s = bytearray(RAW)
        s[16:16 + len(payload)] = payload
        sectors[lba] = bytes(s)

    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[40:72] = b"TEST".ljust(32)
    struct.pack_into("<I", pvd, 80, 24)
    pvd[156:190] = rec(b"\x00", 17, 2048, True)[:34]
    put(16, bytes(pvd))
    put(17, b"".join(rec(*e) for e in entries))
    return sectors, RAW


def run_iso(entries, payloads, tmp):
    sectors, RAW = iso_image(entries)
    for lba, data in payloads.items():
        s = bytearray(RAW)
        s[16:16 + len(data)] = data
        sectors[lba] = bytes(s)
    path = os.path.join(tmp, "img.bin")
    with open(path, "wb") as f:
        for lba in range(24):
            f.write(sectors.get(lba, bytes(RAW)))
    out = os.path.join(tmp, "a", "b", "out")
    os.makedirs(os.path.dirname(out))
    old = sys.argv
    buf = io.StringIO()
    try:
        sys.argv = ["iso9660.py", path, "--extract", out]
        with contextlib.redirect_stdout(buf):
            iso9660.main()
    finally:
        sys.argv = old
    return out, buf.getvalue()


class IsoExtract(unittest.TestCase):
    def test_ordinary_tree_extracts(self):
        entries = [(b"README.TXT;1", 18, 5, False), (b"SUB;1", 19, 2048, True)]
        with tempfile.TemporaryDirectory() as tmp:
            sectors, RAW = iso_image(entries)
            # a sub directory with one file
            out, text = run_iso(entries, {18: b"hello", 19: iso_dir_payload([(b"IN.BIN;1", 20, 2, False)]), 20: b"ok"}, tmp)
            self.assertEqual(files_under(out), ["README.TXT", os.path.join("SUB", "IN.BIN")])
            with open(os.path.join(out, "README.TXT"), "rb") as f:
                self.assertEqual(f.read(), b"hello")

    def test_traversal_names_are_skipped(self):
        entries = [(b"../escape.txt;1", 18, 1, False), (b"..;1", 19, 2048, True), (b"a/b;1", 21, 1, False)]
        payloads = {18: b"x", 19: iso_dir_payload([(b"y.txt;1", 20, 1, False)]), 20: b"y", 21: b"z"}
        with tempfile.TemporaryDirectory() as tmp:
            out, text = run_iso(entries, payloads, tmp)
            self.assertEqual(files_under(tmp), ["img.bin"])
            self.assertIn("skipped", text)

    def test_directory_loop_terminates(self):
        entries = [(b"LOOP;1", 19, 2048, True)]
        payloads = {19: iso_dir_payload([(b"AGAIN;1", 19, 2048, True)])}
        with tempfile.TemporaryDirectory() as tmp:
            out, text = run_iso(entries, payloads, tmp)
            self.assertIn("LOOP", text)

    def test_version_suffix_only_when_numeric(self):
        self.assertEqual(iso9660.clean_name("FILE;1"), "FILE")
        self.assertEqual(iso9660.clean_name("A;B"), "A;B")
        self.assertEqual(iso9660.vis("x\x1by"), "x\\x1by")


def iso_dir_payload(entries):
    def rec(name, lba, size, isdir):
        ln = 33 + len(name)
        ln += ln % 2
        r = bytearray(ln)
        r[0] = ln
        struct.pack_into("<I", r, 2, lba)
        struct.pack_into("<I", r, 10, size)
        r[25] = 2 if isdir else 0
        r[32] = len(name)
        r[33:33 + len(name)] = name
        return bytes(r)
    return b"".join(rec(*e) for e in entries)


if __name__ == "__main__":
    unittest.main()
