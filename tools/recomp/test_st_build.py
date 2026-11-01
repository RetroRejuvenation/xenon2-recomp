#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Tests of the Atari ST build of tools/recomp/recomp.py (docs/re-notes/atari-st-recomp.md), on a synthetic disk (no game bytes).

    python3 -I tools/recomp/test_st_build.py

Exit status 77 (CTest "Skipped") when every test was skipped (no C compiler for the end-to-end generator test).
"""
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "acq"))
_spec = importlib.util.spec_from_file_location("recomp_gen", os.path.join(HERE, "recomp.py"))
rg = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rg)
import rnc  # noqa: E402

TRACK = 5120


def rnc_pack(data):
    """A literal-only RNC method 1 stream (flat 31-leaf 5-bit tables), the inverse of rnc.unpack for the tests."""
    out = bytearray()
    state = {"pos": 0, "bits": 16}

    def bit(b):
        if state["bits"] == 16:
            state["pos"] = len(out)
            out.extend(b"\0\0")
            state["bits"] = 0
        if b:
            out[state["pos"] + (state["bits"] >> 3)] |= 1 << (state["bits"] & 7)
        state["bits"] += 1

    def bits(v, n):
        for i in range(n):
            bit((v >> i) & 1)

    def code(sym):
        for i in range(4, -1, -1):
            bit((sym >> i) & 1)

    def value(v):
        s = v
        if v >= 2:
            s = v.bit_length()
        code(s)
        if s >= 2:
            bits(v & ((1 << (s - 1)) - 1), s - 1)

    bits(0, 2)
    for _ in range(3):
        bits(31, 5)
        for _ in range(31):
            bits(5, 4)
    bits(1, 16)
    value(len(data))
    out.extend(data)  # the literals follow the consumed words directly
    packed = bytes(out)
    return (b"RNC\x01" + struct.pack(">IIHHBB", len(data), len(packed), rnc.crc16(data), rnc.crc16(packed), 0, 1)) + packed


def fnv(b):
    return rg.fnv1a64(b)


def synthetic_disk():
    """(disk.bin bytes, manifest text, {module: unpacked image}) for a disk laid out like the Kixx release's: the engine raw from
    side 0 cylinder 34, the front end as RNC on side 0, five level modules as RNC on side 1."""
    streams = [bytearray(80 * TRACK), bytearray(80 * TRACK)]
    engine_code = bytes.fromhex("7001" "5240" "4e75")  # moveq #1,d0 ; addq.w #1,d0 ; rts at $400
    streams[0][34 * TRACK:34 * TRACK + len(engine_code)] = engine_code
    manifest = ["build=atari-st-kixx", "game=st", "cylinders=80", "sides=2", "sectors_per_track=10", "sector_bytes=512"]
    images = {}
    front = bytearray(4096)
    front[0:4] = bytes.fromhex("7002" "4e75")  # moveq #2,d0 ; rts
    images["frontend"] = bytes(front)
    packed = rnc_pack(bytes(front))
    streams[0][TRACK + 12:TRACK + 12 + len(packed)] = packed
    manifest.append("rnc.0=side=0,cylinder=1,offset=12,side_offset=%d,packed=%d,unpacked=%d" % (TRACK + 12, len(packed) - 18, len(front)))
    for n in range(1, 6):
        lvl = bytearray(4096)
        lvl[0:4] = b"\x00\x06\x69\x1c"          # a pointer in the header the program patches after unpacking
        lvl[16:20] = bytes.fromhex("7003" "4e75")  # moveq #3,d0 ; rts, the first code after the header
        lvl[100:101] = bytes([n])               # makes each module's key different
        images["level%d" % n] = bytes(lvl)
        packed = rnc_pack(bytes(lvl))
        off = (n - 1) * 14000
        streams[1][off:off + len(packed)] = packed
        manifest.append("rnc.%d=side=1,cylinder=%d,offset=%d,side_offset=%d,packed=%d,unpacked=%d" % (
            n, off // TRACK, off % TRACK, off, len(packed) - 18, len(lvl)))
    disk = bytearray()
    for c in range(80):
        for s in range(2):
            disk += streams[s][c * TRACK:(c + 1) * TRACK]
    return bytes(disk), "\n".join(manifest) + "\n", images


class Keys(unittest.TestCase):
    def test_fnv1a64(self):
        self.assertEqual(fnv(b""), 0xcbf29ce484222325)
        self.assertEqual(fnv(b"a"), 0xaf63dc4c8601ec8c)
        self.assertEqual(fnv(b"foobar"), 0x85944171f73967e8)

    def test_synthetic_rnc_round_trips(self):
        data = bytes(range(256)) * 9
        ok, h, out = rnc.verify(rnc_pack(data), 0)
        self.assertTrue(ok)
        self.assertEqual(out, data)


class Images(unittest.TestCase):
    def test_images_and_keys(self):
        disk, manifest, images = synthetic_disk()
        with tempfile.TemporaryDirectory() as tmp:
            open(os.path.join(tmp, "disk.bin"), "wb").write(disk)
            open(os.path.join(tmp, "manifest.txt"), "w").write(manifest)
            imgs, keys = rg.load_images_st(tmp)
        self.assertEqual(sorted(imgs), sorted(rg.ST_MODULES))
        self.assertEqual(imgs["game"].base, 0x400)
        self.assertEqual(imgs["game"].data[:6], bytes.fromhex("7001" "5240" "4e75"))
        self.assertEqual(imgs["frontend"].base, 0x40000)
        self.assertEqual(imgs["frontend"].data, images["frontend"])
        for n in range(1, 6):
            self.assertEqual(imgs["level%d" % n].base, 0x4F000)
            self.assertEqual(imgs["level%d" % n].data, images["level%d" % n])
        # the tracer's keys: the front end from byte 0, a level module from byte 16 (the program patches the header)
        self.assertEqual(keys["%016x" % fnv(images["frontend"][:2048])], "frontend")
        for n in range(1, 6):
            self.assertEqual(keys["%016x" % fnv(images["level%d" % n][16:16 + 2048])], "level%d" % n)

    def test_another_release_is_refused(self):
        disk, manifest, _ = synthetic_disk()
        with tempfile.TemporaryDirectory() as tmp:
            open(os.path.join(tmp, "disk.bin"), "wb").write(disk)
            open(os.path.join(tmp, "manifest.txt"), "w").write(manifest.replace("atari-st-kixx", "atari-st-other"))
            with self.assertRaises(SystemExit):
                rg.load_images_st(tmp)

    def test_a_damaged_stream_is_refused(self):
        disk, manifest, _ = synthetic_disk()
        bad = bytearray(disk)
        bad[(2 * 1) * TRACK + 12 + 40] ^= 0xFF  # a byte inside the front end's packed stream (cylinder 1, side 0)
        with tempfile.TemporaryDirectory() as tmp:
            open(os.path.join(tmp, "disk.bin"), "wb").write(bytes(bad))
            open(os.path.join(tmp, "manifest.txt"), "w").write(manifest)
            with self.assertRaises(SystemExit):
                rg.load_images_st(tmp)


class TraceMapping(unittest.TestCase):
    def test_modules_are_mapped_by_key(self):
        keys = {"00000000000000aa": "level2"}
        warned = set()
        self.assertEqual(rg.trace_module("st", "game", {}, {}, keys, warned), "game")
        self.assertEqual(rg.trace_module("st", "frontend", {}, {}, keys, warned), "frontend")
        self.assertIsNone(rg.trace_module("st", "stage1", {}, {}, keys, warned))
        self.assertIsNone(rg.trace_module("st", "overlay-unloaded", {}, {}, keys, warned))
        self.assertEqual(rg.trace_module("st", "mod_x", {"content_keys": ["00000000000000aa"], "executed_insns": 3}, {}, keys, warned), "level2")
        self.assertIsNone(rg.trace_module("st", "mod_unknown", {"content_keys": ["1"], "executed_insns": 3}, {}, keys, warned))
        self.assertEqual(warned, {"mod_unknown"})
        self.assertEqual(rg.trace_module("disk", "level3", {}, {}, {}, warned), "level3")  # the other builds keep their names


@unittest.skipIf(os.name == "nt", "the generator needs a GCC-style compiler (CC=clang or gcc) on Windows, see tools/build_all.ps1")
@unittest.skipUnless(shutil.which(os.environ.get("CC", "cc")), "no C compiler (the generator builds Musashi's tables)")
class EndToEnd(unittest.TestCase):
    def test_generate_synthetic_st_build(self):
        disk, manifest, images = synthetic_disk()
        with tempfile.TemporaryDirectory() as tmp:
            assets = os.path.join(tmp, "assets")
            os.makedirs(assets)
            open(os.path.join(assets, "disk.bin"), "wb").write(disk)
            open(os.path.join(assets, "manifest.txt"), "w").write(manifest)
            # a trace: the front end's entry, level 3's first code (named by its content key) and a stage-1 PC that must be ignored
            trace = os.path.join(tmp, "trace")
            os.makedirs(trace)
            key3 = "%016x" % fnv(images["level3"][16:16 + 2048])
            cov = {"modules": {
                "game": {"insns": [[0x400, 2, 5, 1, 1]], "content_keys": [], "executed_insns": 1},
                "stage1": {"insns": [[0x500, 2, 5, 1, 1]], "content_keys": [], "executed_insns": 1},
                "frontend": {"insns": [[0x40000, 2, 5, 1, 1]], "content_keys": [], "executed_insns": 1},
                "mod_" + key3: {"insns": [[0x4F010, 2, 5, 1, 1]], "content_keys": [key3], "executed_insns": 1},
            }}
            json.dump(cov, open(os.path.join(trace, "coverage.json"), "w"))
            out = os.path.join(tmp, "out")
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "recomp.py"), "--build", "st", "--assets", assets, "--trace", trace,
                                "--out", out, "--no-capstone", "--no-static"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            files = sorted(os.listdir(out))
            for f in ("x2rs_handlers.h", "x2rs_tables.cpp", "x2rs_game_0.cpp", "x2rs_frontend_0.cpp", "x2rs_level3_0.cpp"):
                self.assertIn(f, files)
            self.assertFalse([f for f in files if f.startswith(("x2r_", "x2rc_"))], files)
            self.assertFalse([f for f in files if "level1" in f or "level2" in f], files)  # nothing traced there, nothing translated
            tables = open(os.path.join(out, "x2rs_tables.cpp")).read()
            self.assertIn("extern const x2r_module x2r_modules_st[]", tables)
            self.assertIn('{"game", 0x400,', tables)
            self.assertIn('{"frontend", 0x40000,', tables)
            self.assertIn('{"level3", 0x4f000,', tables)
            self.assertIn("x2rs_game_000400", tables)
            self.assertIn("x2rs_level3_04f010", tables)
            cm = open(os.path.join(out, "sources.cmake")).read()
            self.assertIn("set(X2_RECOMP_ST_SOURCES", cm)
            self.assertIn("${X2_RECOMP_ST_DIR}/x2rs_tables.cpp", cm)

    def test_the_static_seeds_alone_give_a_build(self):
        disk, manifest, _ = synthetic_disk()
        with tempfile.TemporaryDirectory() as tmp:
            assets = os.path.join(tmp, "assets")
            os.makedirs(assets)
            open(os.path.join(assets, "disk.bin"), "wb").write(disk)
            open(os.path.join(assets, "manifest.txt"), "w").write(manifest)
            out = os.path.join(tmp, "out")
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "recomp.py"), "--build", "st", "--assets", assets, "--out", out,
                                "--no-capstone"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("x2rs_tables.cpp", os.listdir(out))


class Seeds(unittest.TestCase):
    def test_static_seeds_land_in_the_right_module(self):
        imgs = {"game": rg.Image("game", 0x400, bytes(0x39000)), "frontend": rg.Image("frontend", 0x40000, bytes(0x27000))}
        seeds = {}

        def add(mod, pc, why):
            pc &= 0xFFFFFF
            if mod in imgs and imgs[mod].contains(pc):
                seeds.setdefault(mod, set()).add(pc)

        rg.st_static_seeds(imgs, add)
        self.assertEqual(seeds["game"], {rg.ST_ENTRY, rg.ST_VBL, rg.ST_ACIA})
        self.assertEqual(seeds["frontend"], {rg.ST_TIMER_A, rg.ST_FRONT_ENTRY})

    def test_the_names_of_the_other_builds_are_unchanged(self):
        self.assertEqual(rg.PREFIX, "x2r")
        self.assertEqual(rg.cname("game", 0x400), "x2r_game_000400")


if __name__ == "__main__":
    res = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    if not res.wasSuccessful():
        sys.exit(1)
    if res.testsRun and res.testsRun == len(res.skipped):
        sys.exit(77)
