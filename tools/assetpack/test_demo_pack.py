#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Tests of tools/assetpack/make_demo_pack.py with a synthetic dump (pictures and tones made here; no game bytes).

    python3 -I tools/assetpack/test_demo_pack.py [--exe PATH-TO-x2recomp]      (also run by CTest as assetpack_demo)

With --exe (or X2RECOMP) the demo pack is also checked by the runtime's own `x2recomp --check-asset-pack`, which must agree.
The data test on a real dump is demo_pack_data_test.py. Standard library only.
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import wave
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_demo_pack as D  # noqa: E402
import pack_lib as P  # noqa: E402
import validate as V  # noqa: E402

EXE = os.environ.get("X2RECOMP", "")
if "--exe" in sys.argv:
    _i = sys.argv.index("--exe")
    EXE = sys.argv[_i + 1]
    del sys.argv[_i:_i + 2]

H = ["%016x" % (0x1111111111111111 * n) for n in range(1, 12)]


def put(root, rel, data):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def picture(w, h, seed, alpha=255):
    img = P.Image(w, h)
    for y in range(h):
        for x in range(w):
            img.put(x, y, ((x * 37 + seed * 11) % 256, (y * 53 + seed * 7) % 256, (x + y + seed * 29) % 256, alpha))
    return P.encode_png(img)


def wav(frames, rate, amp=9000):
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join((amp if (i // 20) % 2 else -amp).to_bytes(2, "little", signed=True) for i in range(frames)))
    return b.getvalue()


def make_dump(root, scale=1, ring=16):
    """A small dump with every section the demo pack uses; the keys are made up."""
    sprites = [("%s-16x12" % H[0], "sprite"), ("%s-16x12" % H[1], "sprite"), ("%s-24x20" % H[2], "sprite"), ("%s-16x16" % H[3], "tile"),
               ("%s-8x8" % H[10], "sprite")]
    graphics = {}
    for n, (key, kind) in enumerate(sprites):
        w, h = P.parse_gfx_key(key)[1:]
        rel = "%ss/%s.png" % (kind, key)
        put(root, rel, picture(w * scale, h * scale, n))
        graphics[key] = {"png": rel, "kind": kind, "w": w, "h": h, "uses": 3 + n, "first_frame": 10, "palette": 0}
    fkey = "f:%s-16x12" % H[4]
    put(root, "flashes/flash.png", picture(16 * scale, 12 * scale, 9))
    graphics[fkey] = {"png": "flashes/flash.png", "kind": "flash", "w": 16, "h": 12}
    put(root, "far/strip_000.png", picture(320 * scale, 8 * scale, 5))
    put(root, "far_full/level1.png", picture(320 * scale, ring * scale, 6))
    put(root, "screens/screen_a.png", picture(320 * scale, 200 * scale, 7))
    put(root, "screens/screen_a_reference.png", picture(320 * scale, 200 * scale, 8))
    put(root, "panel/%s-1.png" % H[5], picture(8 * scale, 8 * scale, 3))
    put(root, "panel/%s-2.png" % H[6], picture(16 * scale, 8 * scale, 4))
    put(root, "sfx/%s-640.wav" % H[7], wav(11025, 22050))
    put(root, "sfx/%s-1002.wav" % H[8], wav(8000, 44100))
    put(root, "speech/%s-9000.wav" % H[9], wav(33075, 11025))
    blocks = [{"x": 16 * i, "y": 0, "hash": H[i % 11]} for i in range(8)]
    pal = ["#000", "#222", "#642", "#c62", "#446", "#668", "#88a", "#ece", "#aac", "#224", "#002", "#400", "#600", "#820", "#ea0", "#a42"]
    manifest = {
        "format": "x2-asset-pack", "version": 2, "name": "dump", "note": "synthetic", "scale": scale, "graphics": graphics,
        "far": [{"png": "far/strip_000.png", "lines": H[:8]}],
        "seen_palettes": [["#000"] * 16, pal],
        "backgrounds": [{"level": 1, "png": "far_full/level1.png", "scale": scale, "ring_lines": ring}],
        "screens": {"screen_a": {"signature": {"block": 16, "min": 6, "blocks": blocks}, "png": "screens/screen_a.png",
                                 "reference_png": "screens/screen_a_reference.png",
                                 "ref_palette": ["#000000", "#ffffff", "#ff8800", "#0088ff"], "fade": "scale", "bg_index": 0,
                                 "dynamic": [{"x": 64, "y": 0, "w": 112, "h": 32}, {"x": 0, "y": 192, "w": 320, "h": 8}],
                                 "first_frame": 291, "frames": 33, "uses": 1}},
        "panel": {"scale": scale, "cell_w": 8, "max_span": 6, "ref_palette": ["#000000", "#222222", "#664422", "#cc6622"], "fade": "scale",
                  "cells": {"%s-1" % H[5]: {"png": "panel/%s-1.png" % H[5], "uses": 5},
                            "%s-2" % H[6]: {"png": "panel/%s-2.png" % H[6], "uses": 2}}},
        "audio": {
            "sfx": {"%s-640" % H[7]: {"file": "sfx/%s-640.wav" % H[7], "ref_period": 357, "bytes": 640, "uses": 9, "channels": 4, "peak": 0.9},
                    "%s-1002" % H[8]: {"file": "sfx/%s-1002.wav" % H[8], "ref_period": 214, "loop": True, "bytes": 1002}},
            "speech": {"%s-9000" % H[9]: {"file": "speech/%s-9000.wav" % H[9], "cd0_file": "SPEECH01", "bytes": 9000}}},
    }
    put(root, "manifest.json", json.dumps(manifest, indent=1).encode())
    return manifest


def wav_info(data):
    with wave.open(io.BytesIO(data), "rb") as w:
        return w.getnframes(), w.getframerate(), w.getnchannels(), w.getsampwidth()


class DemoPack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="demopack-")
        cls.dump = os.path.join(cls.tmp, "dump")
        cls.dm = make_dump(cls.dump)
        cls.pk = D.build_demo([cls.dump], scale=2, game="disk", atlas=2)
        cls.m = cls.pk.manifest

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def png(self, rel):
        return P.decode_png(self.pk.files[rel])

    def test_header(self):
        m = self.m
        self.assertEqual((m["format"], m["version"], m["scale"], m["games"]), ("x2-asset-pack", 2, 2, ["disk"]))
        self.assertEqual(m["provides"], ["graphics", "screens", "audio"])
        self.assertIn("distribut", m["license"] + m["description"])
        self.assertIn("preview.png", self.pk.files)

    def test_graphics_sizes_and_recolour(self):
        g = self.m["graphics"]
        for key, e in g.items():
            w, h = P.parse_gfx_key(key[2:] if key.startswith("f:") else key)[1:]
            img = self.png(e["png"])
            self.assertEqual((img.w, img.h), (w * 2, h * 2), key)
        src = P.read_png(os.path.join(self.dump, "tiles", "%s-16x16.png" % H[3]))
        out = self.png(g["%s-16x16" % H[3]]["png"])
        # recoloured: the top row's channels are the source's rotated, and the picture is box-scaled
        r, gr, b, a = src.get(3, 0)
        self.assertEqual(out.get(6, 0), (gr, b, r, a))
        self.assertEqual(out.get(7, 0), out.get(6, 0))
        self.assertNotEqual(out.data, src.scaled(2).data)

    def test_flash_is_a_bright_silhouette(self):
        e = self.m["graphics"]["f:%s-16x12" % H[4]]
        img = self.png(e["png"])
        self.assertEqual(img.get(0, 0)[:3], D.FLASH_RGB)

    def test_atlas_and_groups_and_alias(self):
        atl = self.m["atlases"]
        self.assertEqual(len(atl), 1)
        self.assertEqual(atl[0]["scale"], 2)
        keys = {f["key"] for f in atl[0]["frames"]}
        self.assertEqual(keys, {"%s-24x20" % H[2], "%s-8x8" % H[10]})
        self.assertEqual(self.m["groups"]["demo-atlas"], sorted(keys))
        # the second of two same-sized sprites shares the first's picture and has no entry of its own
        g = self.m["graphics"]
        k0, k1 = "%s-16x12" % H[0], "%s-16x12" % H[1]
        self.assertEqual(g[k0]["keys"], [k1])
        self.assertNotIn(k1, g)
        for k in keys:
            self.assertNotIn(k, g)

    def test_alias_without_atlas(self):
        pk = D.build_demo([self.dump], scale=2, atlas=0, audio=False, music=False)
        g = pk.manifest["graphics"]
        self.assertEqual(g["%s-16x12" % H[0]]["keys"], ["%s-16x12" % H[1]])
        self.assertNotIn("%s-16x12" % H[1], g)

    def test_far_background_stars_palettes(self):
        far = self.m["far"][0]
        self.assertEqual((far["scale"], far["lines"]), (2, H[:8]))
        self.assertEqual(self.png(far["png"]).w, 640)
        bg = self.m["backgrounds"][0]
        img = self.png(bg["png"])
        self.assertEqual((bg["level"], bg["ring_lines"], img.w, img.h), (1, 16, 640, 32))
        self.assertTrue(img.is_opaque())

        def row(y):
            return img.data[y * img.w * 4:(y + 1) * img.w * 4]

        def diff(a, b):
            return sum(abs(x - y) for x, y in zip(a, b))

        # seamless: the step from the last row to the first is no larger than the largest step between neighbouring rows
        steps = max(diff(row(y), row(y + 1)) for y in range(img.h - 1))
        self.assertLessEqual(diff(row(img.h - 1), row(0)), steps)
        self.assertEqual(self.m["stars"]["shape"], "round")
        self.assertEqual(len(self.m["palettes"]), 1)
        self.assertEqual(len(self.m["palettes"][0]["colors"]), 16)

    def test_screens_keep_the_matching_data(self):
        s = self.m["screens"]["screen_a"]
        d = self.dm["screens"]["screen_a"]
        for k in ("signature", "ref_palette", "fade", "bg_index", "dynamic"):
            self.assertEqual(s[k], d[k], k)
        for k in ("reference_png", "first_frame", "frames", "uses"):
            self.assertNotIn(k, s)
        img = self.png(s["png"])
        self.assertEqual((img.w, img.h, s["scale"]), (640, 400, 2))
        self.assertTrue(img.is_opaque())

    def test_panel_cells_are_flat(self):
        p = self.m["panel"]
        self.assertEqual((p["scale"], p["cell_w"], p["max_span"]), (2, 8, 6))
        self.assertEqual(p["ref_palette"], self.dm["panel"]["ref_palette"])
        self.assertEqual(set(p["cells"]), {"%s-1" % H[5], "%s-2" % H[6]})
        for key, e in p["cells"].items():
            img = self.png(e["png"])
            span = int(key.split("-")[1])
            self.assertEqual((img.w, img.h), (16 * span, 16))
            colours = {tuple(img.data[i:i + 4]) for i in range(0, len(img.data), 4)}
            self.assertLessEqual(colours, {D.PANEL_RGB + (255,), (0, 0, 0, 0)})
        self.assertEqual(self.png(p["background"]["png"]).w, 640)

    def test_clips_match_duration_and_period(self):
        sfx = self.m["audio"]["sfx"]
        a = sfx["%s-640" % H[7]]
        n, rate, ch, width = wav_info(self.pk.files[a["file"]])
        self.assertEqual((ch, width), (1, 2))
        self.assertAlmostEqual(n / rate, 11025 / 22050, delta=1.0 / rate)
        self.assertEqual((a["ref_period"], a["original"]), (357, "mute"))
        self.assertNotIn("loop", a)
        b = sfx["%s-1002" % H[8]]
        n, rate, _, _ = wav_info(self.pk.files[b["file"]])
        self.assertAlmostEqual(n / rate, 8000 / 44100, delta=1.0 / rate)
        self.assertEqual((b["ref_period"], b["loop"]), (214, True))
        # a loop holds a whole number of cycles: it starts and ends near zero
        with wave.open(io.BytesIO(self.pk.files[b["file"]]), "rb") as w:
            raw = w.readframes(w.getnframes())
        s = [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2)]
        self.assertEqual(s[0], 0)
        self.assertLess(abs(s[-1]), 3000)
        sp = self.m["audio"]["speech"]["%s-9000" % H[9]]
        n, rate, _, _ = wav_info(self.pk.files[sp["file"]])
        self.assertAlmostEqual(n / rate, 33075 / 11025, delta=1.0 / rate)
        self.assertEqual(sp["cd0_file"], "SPEECH01")

    def test_music_slots(self):
        mu = self.m["audio"]["music"]
        self.assertEqual(sorted(mu["slots"]), sorted(D.MUSIC_SLOTS))
        t = mu["slots"]["title"]
        self.assertIn("intro", t)
        self.assertTrue(t["loop"])
        for slot in mu["slots"].values():
            self.assertIn(slot["file"], self.pk.files)

    def test_options(self):
        none = D.build_demo([self.dump], scale=1, audio=False, music=False, atlas=0)
        self.assertNotIn("audio", none.manifest)
        self.assertNotIn("atlases", none.manifest)
        self.assertNotIn("games", none.manifest)
        self.assertEqual(none.manifest["scale"], 1)
        st = D.build_demo([self.dump], scale=2, game="st", audio=False)
        self.assertNotIn("audio", st.manifest)
        sfx_only = D.build_demo([self.dump], scale=2, music=False)
        self.assertNotIn("music", sfx_only.manifest["audio"])
        self.assertEqual(len(sfx_only.manifest["audio"]["sfx"]), 2)

    def test_scale_limits_and_dump_scale(self):
        with self.assertRaises(P.PackError):
            D.build_demo([self.dump], scale=9)
        d2 = os.path.join(self.tmp, "dump2")
        make_dump(d2, scale=2)
        pk = D.build_demo([d2], scale=3, atlas=0, audio=False, music=False)
        self.assertEqual(pk.manifest["scale"], 6)
        for key, e in pk.manifest["graphics"].items():
            w, h = P.parse_gfx_key(key[2:] if key.startswith("f:") else key)[1:]
            img = P.decode_png(pk.files[e["png"]])
            self.assertEqual((img.w, img.h), (w * 6, h * 6))
        with self.assertRaises(P.PackError):
            D.build_demo([d2], scale=5)

    def test_bad_dumps_are_refused(self):
        with self.assertRaises(P.PackError):
            D.build_demo([os.path.join(self.tmp, "missing")])
        with self.assertRaises(P.PackError):
            D.build_demo([])
        bad = os.path.join(self.tmp, "bad")
        make_dump(bad)
        with open(os.path.join(bad, "manifest.json")) as f:
            m = json.load(f)
        m["graphics"]["%s-16x12" % H[0]]["png"] = "../escape.png"
        put(bad, "manifest.json", json.dumps(m).encode())
        with self.assertRaises(P.PackError):
            D.build_demo([bad])
        notpack = os.path.join(self.tmp, "notpack")
        put(notpack, "manifest.json", b'{"format": "something"}')
        with self.assertRaises(P.PackError):
            D.build_demo([notpack])

    def test_deterministic(self):
        again = D.build_demo([self.dump], scale=2, game="disk", atlas=2)
        self.assertEqual(again.files, self.pk.files)
        self.assertEqual(again.manifest, self.m)

    def test_written_pack_validates_folder_and_zip(self):
        folder = os.path.join(self.tmp, "out-folder")
        D.write_pack(self.pk, folder)
        zpath = os.path.join(self.tmp, "out.zip")
        D.write_pack(self.pk, zpath)
        with self.assertRaises(P.PackError):
            D.write_pack(self.pk, folder)
        with self.assertRaises(P.PackError):
            D.write_pack(self.pk, zpath)
        D.write_pack(self.pk, folder, force=True)
        with zipfile.ZipFile(zpath) as z:
            self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()))
            self.assertEqual(sorted(z.namelist()), sorted(set(self.pk.files) | {"manifest.json"}))
        for path in (folder, zpath):
            rep, code, used, skipped = V.run_check(path, game="disk", against=self.dump, full=True, strict=False)
            texts = [V.format_message(m) for m in rep.messages if m["severity"] in (V.WARNING, V.ERROR)]
            self.assertEqual((code, texts), (0, []), path)
            self.assertEqual(skipped, 0)
            self.assertGreater(used, 10)
        rep, code, _, _ = V.run_check(folder, against=self.dump)
        cover = [V.format_message(m) for m in rep.messages if "coverage" in V.format_message(m)]
        self.assertTrue(cover and "graphics 5/5 (100%)" in cover[0] and "sound effects 2/2" in cover[0], cover)

    @unittest.skipUnless(EXE and os.path.exists(EXE), "no --exe: the runtime's validator is not cross-checked")
    def test_runtime_validator_agrees(self):
        folder = os.path.join(self.tmp, "out-rt")
        D.write_pack(self.pk, folder)
        zpath = os.path.join(self.tmp, "out-rt.zip")
        D.write_pack(self.pk, zpath)
        for path in (folder, zpath):
            r = subprocess.run([EXE, "--check-asset-pack", path, "--game", "disk"], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("0 errors, 0 warnings", r.stdout + r.stderr)


class Cli(unittest.TestCase):
    def test_main_writes_and_validates(self):
        tmp = tempfile.mkdtemp(prefix="demopack-cli-")
        try:
            dump = os.path.join(tmp, "dump")
            make_dump(dump)
            out = os.path.join(tmp, "pack")
            buf = io.StringIO()
            old = sys.stdout
            sys.stdout = buf
            try:
                rc = D.main([dump, out, "--scale", "2", "--game", "cdtv", "--atlas", "1"])
            finally:
                sys.stdout = old
            self.assertEqual(rc, 0, buf.getvalue())
            self.assertIn("demo pack:", buf.getvalue())
            self.assertIn("valid", buf.getvalue())
            self.assertTrue(os.path.isfile(os.path.join(out, "manifest.json")))
            err = io.StringIO()
            old_err, sys.stderr = sys.stderr, err
            try:
                self.assertEqual(D.main([dump, out, "--no-check"]), 2)  # exists, not empty
                self.assertEqual(D.main([os.path.join(tmp, "nope"), os.path.join(tmp, "x")]), 2)
            finally:
                sys.stderr = old_err
            self.assertIn("not empty", err.getvalue())
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
