#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Tests of the CDTV build of tools/recomp/recomp.py (docs/re-notes/cdtv-recomp.md).

    python3 -I tools/recomp/test_cdtv_build.py                      synthetic tests only (no game data)
    python3 -I tools/recomp/test_cdtv_build.py --assets DIR --exe X2RECOMP [--replays DIR]
                                                                     also check the statically relocated overlays against the
                                                                     overlay area of the running game (needs the CDTV import)

Exit status 77 (CTest "Skipped") when every test was skipped (no C compiler for the end-to-end generator test and no --assets).
"""
import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
_spec = importlib.util.spec_from_file_location("recomp_gen", os.path.join(HERE, "recomp.py"))
rg = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rg)

ARGS = None


def be32b(v):
    return v.to_bytes(4, "big")


def make_overlay(text, relocs):
    """A GEMDOS 0x601A file: header, text, relocation table for the listed text offsets (ascending)."""
    hdr = bytearray(0x1C)
    hdr[0:2] = b"\x60\x1a"
    hdr[2:6] = be32b(len(text))
    table = bytearray(be32b(relocs[0]))
    pos = relocs[0]
    for r in relocs[1:]:
        d = r - pos
        while d > 254:
            table.append(1)
            d -= 254
        table.append(d)
        pos = r
    table.append(0)
    return bytes(hdr) + bytes(text) + bytes(table)


class Relocation(unittest.TestCase):
    def test_first_fixup_at_offset_zero_and_skips(self):
        text = bytearray(1200)
        for off, val in ((0, 0x100), (4, 0x200), (100, 0x300), (700, 0x400), (1196, 0x500)):
            text[off:off + 4] = be32b(val)
        raw = make_overlay(text, [0, 4, 100, 700, 1196])
        out, n = rg.relocate_overlay(raw)
        self.assertEqual(n, 5)
        body = out[0x1C:0x1C + 1200]
        for off, val in ((0, 0x100), (4, 0x200), (100, 0x300), (700, 0x400), (1196, 0x500)):
            self.assertEqual(int.from_bytes(body[off:off + 4], "big"), val + rg.CDTV_RELOC_ADD)
        # nothing else changed, and the header and table are untouched
        self.assertEqual(out[:0x1C], raw[:0x1C])
        self.assertEqual(out[0x1C + 1200:], raw[0x1C + 1200:])
        self.assertEqual(sum(1 for i in range(1200) if body[i] != text[i]) <= 20, True)

    def test_long_gap_uses_the_skip_byte(self):
        text = bytearray(2000)
        raw = make_overlay(text, [8, 8 + 254 * 3 + 6])  # one gap of 768 = three skips and 6
        out, n = rg.relocate_overlay(raw)
        self.assertEqual(n, 2)
        self.assertEqual(int.from_bytes(out[0x1C + 8:0x1C + 12], "big"), rg.CDTV_RELOC_ADD)
        self.assertEqual(int.from_bytes(out[0x1C + 8 + 768:0x1C + 12 + 768], "big"), rg.CDTV_RELOC_ADD)

    def test_damaged_files_are_refused(self):
        good = make_overlay(bytearray(64), [0, 8])
        with self.assertRaises(ValueError):
            rg.relocate_overlay(b"\x00" * 100)  # not a 0x601A file
        with self.assertRaises(ValueError):
            rg.relocate_overlay(good[:0x1C + 64 + 4])  # the table stops after its first long
        bad = bytearray(make_overlay(bytearray(64), [0, 8]))
        bad[2:6] = be32b(10_000_000)  # text longer than the file
        with self.assertRaises(ValueError):
            rg.relocate_overlay(bytes(bad))
        outside = make_overlay(bytearray(64), [0])
        outside = outside[:-5] + be32b(1000) + b"\x00"  # first fix-up beyond the text
        with self.assertRaises(ValueError):
            rg.relocate_overlay(outside)
        unterminated = bytearray(make_overlay(bytearray(64), [0, 8]))
        unterminated[-1] = 5  # no zero byte at the end
        with self.assertRaises(ValueError):
            rg.relocate_overlay(bytes(unterminated))


class Images(unittest.TestCase):
    def test_holes(self):
        img = rg.Image("game", 0x400, bytes(range(256)) * 16, holes=[(0x500, 0x600)])
        self.assertTrue(img.contains(0x4FE))
        self.assertFalse(img.contains(0x500))
        self.assertFalse(img.contains(0x5FF))
        self.assertTrue(img.contains(0x600))
        self.assertIsNone(img.w16(0x4FF))  # straddles the hole
        self.assertIsNone(img.w16(0x500))
        self.assertIsNotNone(img.w16(0x600))

    def test_decode_stops_at_a_hole(self):
        # move.l #imm,d0 at 0x4FC: its extension words fall inside the hole
        data = bytearray(0x400)
        data[0xFC:0xFE] = b"\x20\x3c"
        img = rg.Image("game", 0x400, bytes(data), holes=[(0x500, 0x600)])
        table = {op: ("m68k_op_move_32_d_i", 12) for op in range(0x10000)}
        self.assertIsNone(rg.decode(img, 0x4FC, table, {}))

    def test_static_seeds(self):
        game = bytearray(0x40000)
        # an API table at $E12 with one slot into the resident code and one into the overlay area
        slot = rg.CDTV_API_TABLE - rg.CDTV_BASE
        game[slot:slot + 4] = be32b(0x30F00)
        game[slot + 4:slot + 8] = be32b(rg.CDTV_OVERLAY_BASE + 0x100)
        imgs = {"game": rg.Image("game", rg.CDTV_BASE, bytes(game), holes=[(rg.CDTV_OVERLAY_BASE,
                                                                             rg.CDTV_OVERLAY_BASE + rg.CDTV_OVERLAY_SIZE)])}
        text = bytearray(200)
        text[0:2] = b"\x60\x00"
        text[2:4] = b"\x00\x10"  # bra.w stub at text+0
        imgs["level1"] = rg.Image("level1", rg.CDTV_OVERLAY_BASE, make_overlay(text, [0, 20]))
        seeds = {}

        def add(mod, pc, why):
            pc &= 0xFFFFFF
            if mod in imgs and imgs[mod].contains(pc):
                seeds.setdefault(mod, set()).add(pc)

        rg.cdtv_static_seeds(imgs, add)
        self.assertIn(rg.CDTV_ENTRY, seeds["game"])
        self.assertIn(0x30F00, seeds["game"])  # API slot into the resident code
        self.assertIn(rg.CDTV_OVERLAY_BASE + 0x1C, seeds["level1"])  # the overlay's entry stub


@unittest.skipIf(os.name == "nt", "the generator needs a GCC-style compiler (CC=clang or gcc) on Windows, see tools/build_all.ps1")
@unittest.skipUnless(shutil.which(os.environ.get("CC", "cc")), "no C compiler (the generator builds Musashi's tables)")
class EndToEnd(unittest.TestCase):
    def test_generate_synthetic_cdtv_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            assets = os.path.join(tmp, "assets")
            os.makedirs(os.path.join(assets, "cd0"))
            game = bytearray(0x40000)
            code = bytes.fromhex("7001" "5240" "4e75")  # moveq #1,d0 ; addq.w #1,d0 ; rts at $400
            game[0:len(code)] = code
            open(os.path.join(assets, "game_0400.bin"), "wb").write(bytes(game))
            text = bytearray(300)
            text[0:4] = b"\x60\x00\x00\x3e"  # bra.w stub (entry) at text+0 -> text+0x40
            body = bytes.fromhex("203c" "00000100" "4e75")  # move.l #$100,d0 ; rts, the immediate is relocated
            text[0x40:0x40 + len(body)] = body
            relocs = [0x42]
            open(os.path.join(assets, "cd0", "LEVEL1"), "wb").write(make_overlay(text, relocs))
            out = os.path.join(tmp, "out")
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "recomp.py"), "--build", "cdtv", "--assets", assets,
                                "--out", out, "--no-capstone"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            files = sorted(os.listdir(out))
            self.assertIn("x2rc_handlers.h", files)
            self.assertIn("x2rc_tables.cpp", files)
            self.assertIn("x2rc_game_0.cpp", files)
            self.assertIn("x2rc_level1_0.cpp", files)
            self.assertFalse([f for f in files if f.startswith("x2r_")], files)
            tables = open(os.path.join(out, "x2rc_tables.cpp")).read()
            self.assertIn("extern const x2r_module x2r_modules_cdtv[]", tables)
            self.assertIn('{"game", 0x400,', tables)
            self.assertIn('{"level1", 0x4e26,', tables)
            self.assertIn("x2rc_game_000400", tables)
            cm = open(os.path.join(out, "sources.cmake")).read()
            self.assertIn("set(X2_RECOMP_CDTV_SOURCES", cm)
            self.assertIn("${X2_RECOMP_CDTV_DIR}/x2rc_tables.cpp", cm)
            self.assertNotIn("X2_RECOMP_DIR}", cm.replace("X2_RECOMP_CDTV_DIR}", ""))
            # the overlay's immediate is relocated: $100 + $406 = $506
            lvl = open(os.path.join(out, "x2rc_level1_0.cpp")).read()
            self.assertIn("0x0506", lvl)
            self.assertNotIn("0x0100)", lvl.replace("0x0000", ""))
            man = json.load(open(os.path.join(out, "manifest.json")))
            self.assertGreaterEqual(man["instructions"]["game"], 3)
            self.assertGreaterEqual(man["instructions"]["level1"], 2)

    def test_the_disk_build_keeps_its_names(self):
        # --build disk's naming is the contract of the existing translation (x2r_, x2r_modules)
        self.assertEqual(rg.PREFIX, "x2r")
        self.assertEqual(rg.cname("game", 0x400), "x2r_game_000400")


class AgainstTheRunningGame(unittest.TestCase):
    """The overlay image the generator relocates statically must equal what X2 leaves in the overlay area: translated code
    verifies every instruction word against memory, so a wrong static relocation would silently fall back to the interpreter."""

    # (replay, overlay, frame): a little after the replay loads that overlay (frames from the trace of the corpus: LEVEL2-5 load at
    # 1909, LEVEL1 at 2105 in level1_play, INTRO at 636). Later the game writes its own state into the data at the end of the text.
    CASES = [("level1_play", "LEVEL1", 2300), ("level2", "LEVEL2", 1950), ("level3", "LEVEL3", 1950),
             ("level4", "LEVEL4", 1950), ("level5", "LEVEL5", 1950), ("boss1", "INTRO", 700)]

    def test_overlays_match_ram(self):
        a = ARGS
        if a is None:
            self.skipTest("no --assets (the CDTV import) given")
        for replay, name, frame in self.CASES:
            with self.subTest(overlay=name):
                with tempfile.TemporaryDirectory() as tmp:
                    ram = os.path.join(tmp, "ram.bin")
                    r = subprocess.run([a.exe, "--headless", "--report-every", "0", "--assets", a.assets, "--input-script",
                                        os.path.join(a.replays, replay + ".txt"), "--frames", str(frame + 1),
                                        "--dump-ram-at", "%d:%s" % (frame, ram)], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                    mem = open(ram, "rb").read()
                raw = open(os.path.join(a.assets, "cd0", name), "rb").read()
                img, n = rg.relocate_overlay(raw)
                base = rg.CDTV_OVERLAY_BASE
                diff = [i for i in range(len(img)) if mem[base + i] != img[i]]
                self.assertEqual(diff, [], "%s: %d of %d bytes differ from the running game (first at +%#x)" % (
                    name, len(diff), len(img), diff[0] if diff else 0))


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets")
    ap.add_argument("--exe")
    ap.add_argument("--replays", default=os.path.join(ROOT, "tests", "replays", "cdtv"))
    ap.add_argument("unittest_args", nargs="*")
    a = ap.parse_args()
    if a.assets:
        if not (a.exe and os.path.exists(os.path.join(a.assets, "game_0400.bin")) and
                os.path.isdir(os.path.join(a.assets, "cd0"))):
            print("SKIPPED: no CDTV import in %s" % a.assets)
            sys.exit(77)
        ARGS = a
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    if not res.wasSuccessful():
        sys.exit(1)
    if res.testsRun and res.testsRun == len(res.skipped):
        sys.exit(77)


if __name__ == "__main__":
    main()
