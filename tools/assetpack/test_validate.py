# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Tests of tools/assetpack/validate.py with synthetic packs (no game bytes), and a cross-check that it and the runtime's
`x2recomp --check-asset-pack` give the same verdict (exit code) on the same packs.

    python3 -I tools/assetpack/test_validate.py [--exe PATH-TO-x2recomp]      (also run by CTest as assetpack_validate)

The cross-check runs when --exe (or the X2RECOMP environment variable) names the executable and is skipped otherwise. Audio is left
out of the cross-check: the runtime's decoders are registered by the audio stage, the Python tool reads WAV on its own.
Standard library only.
"""
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import pack_lib as P  # noqa: E402
import validate as V  # noqa: E402

EXE = os.environ.get("X2RECOMP", "")
if "--exe" in sys.argv:  # before the test classes are defined: skipUnless looks at it
    _i = sys.argv.index("--exe")
    EXE = sys.argv[_i + 1]
    del sys.argv[_i:_i + 2]
H1, H2, H3 = "0123456789abcdef", "1123456789abcdef", "2123456789abcdef"


def put(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def flat(w, h, rgba=(255, 0, 0, 255)):
    return P.encode_png(P.Image(w, h, bytes(rgba) * (w * h)))


def wav(frames, rate=22050, amp=8000):
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", amp if (i // 16) & 1 else -amp) for i in range(frames)))
    return b.getvalue()


def manifest(**kw):
    m = {"format": "x2-asset-pack", "version": 2, "license": "CC0-1.0"}
    m.update(kw)
    return m


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="x2val_")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def pack(self, name, man, files=None):
        d = os.path.join(self.tmp, name)
        put(os.path.join(d, "manifest.json"), man if isinstance(man, (str, bytes)) else json.dumps(man))
        for rel, data in (files or {}).items():
            put(os.path.join(d, rel), data)
        return d

    def check(self, path, **kw):
        rep, code, entries, skipped = V.run_check(path, **kw)
        return rep, code

    def said(self, rep, text, severity=None):
        return any(text in V.format_message(m) and (severity is None or m["severity"] == severity) for m in rep.messages)


class Findings(Base):
    def test_clean_pack(self):
        k1 = H1 + "-16x8"
        d = self.pack("clean", manifest(name="Clean", scale=2, graphics={k1: {"png": "a.png"}},
                                        panel={"cells": {H1 + "-2": {"png": "cell.png"}}}),
                      {"a.png": flat(32, 16), "cell.png": flat(32, 16)})
        rep, code = self.check(d)
        self.assertEqual(code, 0, V.format_report(rep, code, 0, 0, False))
        self.assertEqual(rep.count(V.ERROR) + rep.count(V.WARNING), 0)
        self.assertEqual(self.check(d, strict=True)[1], 0)
        text = V.format_report(rep, code, 2, 0, False)
        self.assertIn("info: asset pack: Clean: memory:", text)
        self.assertTrue(text.rstrip().endswith("valid"))

    def test_size_warnings_and_strict(self):
        k1, k3 = H1 + "-16x8", H3 + "-4x4"
        d = self.pack("warn", manifest(unknown_top=1, scale=2, graphics={k1: {"png": "a.png", "extra": 1}, k3: {"png": "a.png", "scale": 1}},
                                       atlases=[{"png": "atlas.png", "frames": [{"key": k1, "rect": [0, 0, 8, 8]}]}],
                                       groups={"ship": [k1, H2 + "-4x4"]}, far=[{"png": "far.png", "scale": 2, "lines": [H1]}],
                                       panel={"background": {"png": "pbg.png"}}, screens={"t": {"png": "s.png", "dynamic": [{"x": 300, "y": 190, "w": 40, "h": 20}],
                                                                                                "signature": {"min": 1, "blocks": [{"x": 0, "y": 0, "hash": H1}]}}}),
                      {"a.png": flat(32, 16), "atlas.png": flat(16, 16), "far.png": flat(640, 3), "pbg.png": flat(600, 16), "s.png": flat(320, 200), "unused.png": flat(1, 1)})
        rep, code = self.check(d)
        self.assertEqual(code, 0, V.format_report(rep, code, 0, 0, False))
        for text in ['unknown field "unknown_top"', 'unknown field "extra"', "is 32x16, a graphic of this key at this scale is expected to be 4x4", "is defined twice (graphics and atlases[0])",
                     "the frame is 8x8, a graphic of this key at scale 2 is expected to be 32x16", "lists the key '" + H2 + "-4x4'", "'pbg.png' is 600x16, the panel background is expected to be 640x16",
                     "'s.png' is 320x200, a screen picture is expected to be 640x400", 'a "dynamic" rectangle reaches outside', "fewer than 6 blocks", "not a multiple of its scale 2",
                     "1 file is not used by manifest.json (unused.png)"]:
            self.assertTrue(self.said(rep, text), text)
        self.assertEqual(self.check(d, strict=True)[1], 1)

    def test_unknown_fields_are_counted_once(self):
        g = {"%016x-4x4" % (i + 1): {"png": "a.png", "kind": "sprite", "uses": 3} for i in range(5)}
        d = self.pack("dumplike", {"format": "x2-asset-pack", "version": 1, "license": "MIT", "graphics": g}, {"a.png": flat(4, 4)})
        rep, code = self.check(d)
        self.assertEqual(code, 0)
        self.assertEqual(rep.count(V.WARNING), 2)
        self.assertTrue(self.said(rep, 'graphics["0000000000000001-4x4"]: unknown field "kind" (ignored; also in 4 other entries of this section)', V.WARNING))

    def test_license(self):
        d = self.pack("nolic", {"format": "x2-asset-pack", "version": 2})
        rep, code = self.check(d)
        self.assertEqual(code, 0)
        self.assertTrue(self.said(rep, "license: missing", V.WARNING))
        rep, code = self.check(d, strict=True)
        self.assertEqual(code, 2)
        self.assertTrue(self.said(rep, "license: missing", V.ERROR))

    def test_refusals(self):
        self.assertEqual(self.check(os.path.join(self.tmp, "nothing"))[1], 3)
        self.assertEqual(self.check(self.pack("broken", '{"format": '))[1], 2)
        self.assertEqual(self.check(self.pack("fmt", {"format": "other", "version": 2}))[1], 2)
        rep, code = self.check(self.pack("v3", {"format": "x2-asset-pack", "version": 3}))
        self.assertEqual(code, 2)
        self.assertTrue(self.said(rep, "needs a newer x2recomp"))
        put(os.path.join(self.tmp, "garbage.zip"), b"not a zip at all, not a zip at all")
        self.assertEqual(self.check(os.path.join(self.tmp, "garbage.zip"))[1], 3)
        os.makedirs(os.path.join(self.tmp, "empty"))
        self.assertEqual(self.check(os.path.join(self.tmp, "empty"))[1], 2)
        rep, code = self.check(self.pack("badentry", manifest(graphics={"bad": {"png": "a.png"}})))
        self.assertEqual(code, 2)

    def test_missing_and_unsafe_files(self):
        k = H1 + "-4x4"
        rep, code = self.check(self.pack("missing", manifest(graphics={k: {"png": "gone.png"}})))
        self.assertEqual(code, 2)
        self.assertTrue(self.said(rep, "missing file 'gone.png'", V.ERROR))
        rep, code = self.check(self.pack("unsafe", manifest(graphics={k: {"png": "../outside.png"}})))
        self.assertEqual(code, 2)
        rep, code = self.check(self.pack("badpng", manifest(graphics={k: {"png": "x.png"}}), {"x.png": b"not a png"}))
        self.assertEqual(code, 2)
        self.assertTrue(self.said(rep, "'x.png'", V.ERROR))

    def test_version1_sections(self):
        rep, code = self.check(self.pack("v1", {"format": "x2-asset-pack", "version": 1, "license": "MIT", "screens": {}}))
        self.assertEqual(code, 2)

    def test_scope_report(self):
        k = H3 + "-4x4"
        d = self.pack("scope", manifest(games=["cdtv"], graphics={k: {"png": "a.png"}, H2 + "-4x4": {"png": "a.png", "games": ["disk"]}}), {"a.png": flat(4, 4)})
        rep, code = V.run_check(d, game="cdtv", build="cdtv")[:2]
        self.assertEqual(code, 0)
        self.assertTrue(self.said(rep, "1 of 2 entries apply to cdtv, build cdtv", V.INFO))
        rep, code = V.run_check(d, game="disk", build="canonical")[:2]
        self.assertTrue(self.said(rep, "the pack's games/builds do not include disk build canonical", V.WARNING))

    def test_json_output(self):
        d = self.pack("json", {"format": "x2-asset-pack", "version": 2, "zzz": 1})
        rep, code, entries, skipped = V.run_check(d)
        js = json.loads(V.format_report(rep, code, entries, skipped, True))
        self.assertEqual(js[-1]["severity"], "summary")
        self.assertEqual(js[-1]["exit_code"], code)
        self.assertEqual(js[-1]["warnings"], 2)
        self.assertTrue(js[0]["message"].startswith("asset pack: "))

    def test_skeleton_validates_with_warnings_only(self):
        rep, code = self.check(os.path.join(ROOT, "examples", "asset-pack-skeleton"))
        self.assertEqual(code, 0, V.format_report(rep, code, 0, 0, False))
        self.assertEqual(rep.count(V.ERROR), 0)

    def test_schema_examples_parse(self):
        for ex in ("v1-graphics.example.json", "v2-full.example.json"):
            d = os.path.join(self.tmp, ex)
            with open(os.path.join(ROOT, "docs", "schema", "examples", ex)) as f:
                put(os.path.join(d, "manifest.json"), f.read())
            rep, code = self.check(d)
            # the files the examples name do not exist: only missing-file errors, never a schema or syntax error
            self.assertFalse(any(m["severity"] == V.ERROR and "missing file" not in m["text"] for m in rep.messages), V.format_report(rep, code, 0, 0, False))


class Audio(Base):
    def test_clipping_loops_and_zip(self):
        d = self.pack("audio", manifest(audio={"sfx": {H1 + "-640": {"file": "clip.wav"}, H2 + "-640": {"file": "gone.wav"}, H3 + "-640": {"file": "junk.wav"}},
                                               "music": {"slots": {"title": {"file": "t.wav", "loop": {"start_s": 0.5, "end_s": 9}}, "level1": {"file": "t.wav", "loop": {"start_s": 7}},
                                                                   "level2": {"file": "t.wav", "loop": {"start_s": 0.1, "end_s": 0.9}}, "level3": {"file": "t.wav", "loop": {"start_s": 0.1, "end_s": 1.00001}}}}}),
                      {"clip.wav": wav(320), "junk.wav": b"junk", "t.wav": wav(8000, 8000)})
        rep, code = self.check(d, full=True)
        self.assertEqual(code, 2)
        self.assertTrue(self.said(rep, "missing file 'gone.wav'", V.ERROR))
        self.assertTrue(self.said(rep, "'junk.wav': not a WAV, FLAC or Ogg Vorbis file", V.ERROR))
        self.assertTrue(self.said(rep, 'audio.music.slots["title"]: the loop ends at 9 s', V.ERROR))
        self.assertTrue(self.said(rep, 'audio.music.slots["level1"]: the loop starts at 7 s', V.ERROR))
        self.assertFalse(self.said(rep, '["level2"]: the loop'))
        self.assertFalse(self.said(rep, '["level3"]: the loop'))     # two frames of rounding are fine
        # a deflated music member in a ZIP is flagged
        z = os.path.join(self.tmp, "a.zip")
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("manifest.json", json.dumps(manifest(audio={"music": {"slots": {"title": {"file": "t.wav"}}}})), zipfile.ZIP_STORED)
            zf.writestr("t.wav", wav(800, 8000), zipfile.ZIP_DEFLATED)
        rep, code = self.check(z)
        self.assertEqual(code, 0)
        self.assertTrue(self.said(rep, "'t.wav': a ZIP member stored without compression", V.WARNING))
        # flac and ogg are accepted by their signature
        d = self.pack("sig", manifest(audio={"sfx": {H1 + "-10": {"file": "a.flac"}, H2 + "-10": {"file": "b.ogg"}}}), {"a.flac": b"fLaC\0\0\0\0", "b.ogg": b"OggS\0\0\0\0"})
        self.assertEqual(self.check(d)[1], 0)


class Zips(Base):
    def make(self, name, members, stored=True):
        z = os.path.join(self.tmp, name)
        with zipfile.ZipFile(z, "w") as zf:
            for n, data in members:
                zi = zipfile.ZipInfo("member")
                zi.filename = n  # assigned after the constructor, which would turn a backslash into '/' on Windows
                zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
                zf.writestr(zi, data)
        return z

    def test_zip_pack_and_rules(self):
        k = H1 + "-4x4"
        m = json.dumps(manifest(graphics={k: {"png": "a.png"}}))
        rep, code = self.check(self.make("ok.zip", [("pk/manifest.json", m), ("pk/a.png", flat(4, 4))]))
        self.assertEqual(code, 0, V.format_report(rep, code, 0, 0, False))
        rep, code = self.check(self.make("bs.zip", [("manifest.json", m), ("a.png", flat(4, 4)), ("sprites\\x.png", flat(1, 1))]))
        self.assertEqual(code, 0)
        self.assertTrue(self.said(rep, "backslashes were read as '/'", V.WARNING))
        rep, code = self.check(self.make("case.zip", [("manifest.json", m), ("A.png", b"x"), ("a.PNG", b"y")]))
        self.assertEqual(code, 2)
        self.assertTrue(self.said(rep, "collide on a case-insensitive file system"))
        rep, code = self.check(self.make("none.zip", [("a.png", b"x")]))
        self.assertEqual(code, 2)
        rep, code = self.check(self.make("evil.zip", [("manifest.json", m), ("a.png", flat(4, 4)), ("../evil.png", b"x"), ("con.png", b"x")]))
        self.assertEqual(code, 0)
        self.assertEqual(sum(1 for x in rep.messages if "is ignored" in x["text"]), 2)


class AgainstDump(Base):
    def test_against(self):
        k1, k2, k3, typo = H1 + "-16x8", H2 + "-16x8", H3 + "-4x4", "9923456789abcdef-4x4"
        dump = os.path.join(self.tmp, "dump")
        put(os.path.join(dump, "sprites/a.png"), flat(32, 16, (1, 2, 3, 255)))
        put(os.path.join(dump, "sprites/b.png"), flat(32, 16, (4, 5, 6, 255)))
        put(os.path.join(dump, "sprites/c.png"), flat(4, 4, (7, 8, 9, 255)))
        put(os.path.join(dump, "panel/p1.png"), flat(16, 8, (0, 0, 1, 255)))
        put(os.path.join(dump, "far/strip.png"), flat(320, 2))
        put(os.path.join(dump, "manifest.json"), json.dumps({"format": "x2-asset-pack", "version": 2, "name": "dump",
                                                              "graphics": {k1: {"png": "sprites/a.png"}, k2: {"png": "sprites/b.png"}, k3: {"png": "sprites/c.png"}},
                                                              "far": [{"png": "far/strip.png", "lines": ["aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb"]}],
                                                              "panel": {"cells": {H1 + "-1": {"png": "panel/p1.png"}}}}))
        d = self.pack("against", manifest(graphics={k1: {"png": "mine.png", "scale": 2}, k2: {"png": "copy.png", "scale": 2}, typo: {"png": "t.png"}},
                                          far=[{"png": "far.png", "lines": ["aaaaaaaaaaaaaaaa", "cccccccccccccccc"]}],
                                          panel={"cells": {H1 + "-1": {"png": "pcopy.png"}, H3 + "-1": {"png": "pcopy.png"}}}),
                      {"mine.png": flat(32, 16), "t.png": flat(4, 4), "far.png": flat(320, 2)})
        shutil.copy(os.path.join(dump, "sprites/b.png"), os.path.join(d, "copy.png"))
        shutil.copy(os.path.join(dump, "panel/p1.png"), os.path.join(d, "pcopy.png"))
        rep, code = self.check(d, against=dump)
        self.assertEqual(code, 0, V.format_report(rep, code, 0, 0, False))
        self.assertTrue(self.said(rep, "coverage of the dump: graphics 2/3 (66%), far lines 1/2 (50%), screens 0/0, panel cells 1/1, sound effects 0/0, speech 0/0", V.INFO))
        self.assertTrue(self.said(rep, 'graphics["%s"]: this key is not in the dump' % typo, V.WARNING))
        self.assertTrue(self.said(rep, 'far["cccccccccccccccc"]: this key is not in the dump', V.WARNING))
        self.assertTrue(self.said(rep, "'copy.png' is byte-identical to the dump's original", V.WARNING))
        self.assertFalse(self.said(rep, "'mine.png' is byte-identical"))
        self.assertEqual(self.check(d, against=dump, strict=True)[1], 1)
        rep, code = self.check(d, against=os.path.join(self.tmp, "no-dump"))
        self.assertEqual(code, 2)


@unittest.skipUnless(EXE and os.path.exists(EXE), "no x2recomp executable given (--exe or X2RECOMP): the cross-check is skipped")
class CrossCheck(Base):
    """validate.py and `x2recomp --check-asset-pack` agree on the exit code of the same pack."""

    def both(self, path, *extra):
        py = V.run_check(path, strict="--strict" in extra, against=extra[extra.index("--against") + 1] if "--against" in extra else None)[1]
        p = subprocess.run([EXE, "--check-asset-pack", path] + list(extra), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        return py, p.returncode, p.stdout + p.stderr

    def test_same_verdicts(self):
        k1, k3 = H1 + "-16x8", H3 + "-4x4"
        screens = {"t": {"png": "s.png", "ref_palette": ["#000", "#fff"], "signature": {"min": 6, "blocks": [{"x": i, "y": 0, "hash": "%x123456789abcde" % i} for i in range(6)]}}}
        cases = {
            "clean": (manifest(scale=2, graphics={k1: {"png": "a.png"}}, screens=screens), {"a.png": flat(32, 16), "s.png": flat(640, 400)}, []),
            "warn": (manifest(scale=2, zzz=1, graphics={k1: {"png": "a.png"}, k3: {"png": "a.png", "scale": 1}}), {"a.png": flat(32, 16)}, []),
            "warn_strict": (manifest(scale=2, zzz=1, graphics={k1: {"png": "a.png"}, k3: {"png": "a.png", "scale": 1}}), {"a.png": flat(32, 16)}, ["--strict"]),
            "nolic": ({"format": "x2-asset-pack", "version": 2}, {}, []),
            "nolic_strict": ({"format": "x2-asset-pack", "version": 2}, {}, ["--strict"]),
            "missing": (manifest(graphics={k1: {"png": "gone.png"}}), {}, []),
            "badkey": (manifest(graphics={"bad": {"png": "a.png"}}), {"a.png": flat(4, 4)}, []),
            "unsafe": (manifest(graphics={k1: {"png": "../x.png"}}), {}, []),
            "v1_v2_sections": ({"format": "x2-asset-pack", "version": 1, "license": "MIT", "screens": {}}, {}, []),
            "v3": ({"format": "x2-asset-pack", "version": 3}, {}, []),
            "far_wrong_width": (manifest(far=[{"png": "f.png", "lines": [H1]}]), {"f.png": flat(100, 1)}, []),
            "bg_ok": (manifest(backgrounds=[{"level": 1, "png": "b.png", "ring_lines": 4}]), {"b.png": flat(320, 4)}, []),
            "bg_bad_height": (manifest(backgrounds=[{"level": 1, "png": "b.png", "ring_lines": 5}]), {"b.png": flat(320, 4)}, []),
            "atlas_outside": (manifest(atlases=[{"png": "s.png", "frames": [{"key": H1 + "-4x4", "rect": [10, 10, 4, 4]}]}]), {"s.png": flat(8, 8)}, []),
            "atlas_ok": (manifest(atlases=[{"png": "s.png", "frames": [{"key": H1 + "-4x4", "rect": [0, 0, 4, 4]}]}]), {"s.png": flat(8, 8)}, []),
            "panel_ok": (manifest(panel={"cells": {H1 + "-2": {"png": "c.png"}}}), {"c.png": flat(16, 8)}, []),
            "bad_screen_name": (manifest(screens={"Bad Name": {"png": "s.png", "signature": {"min": 1, "blocks": [{"x": 0, "y": 0, "hash": H1}]}}}), {"s.png": flat(320, 200)}, []),
            "bad_clip_key": (manifest(audio={"sfx": {"zz": {"file": "x.wav"}}}), {}, []),
        }
        for name, (man, files, extra) in cases.items():
            d = self.pack("x_" + name, man, files)
            py, cpp, out = self.both(d, *extra)
            self.assertEqual(py, cpp, "%s: validate.py says %d, x2recomp says %d\n%s" % (name, py, cpp, out))

    def test_zip_verdicts(self):
        m = json.dumps(manifest(graphics={H1 + "-4x4": {"png": "a.png"}}))
        for name, members in {"ok": [("manifest.json", m), ("a.png", flat(4, 4))], "case": [("manifest.json", m), ("A.png", b"x"), ("a.PNG", b"y")], "none": [("a.png", b"x")]}.items():
            z = os.path.join(self.tmp, name + ".zip")
            with zipfile.ZipFile(z, "w") as zf:
                for n, d in members:
                    zf.writestr(n, d)
            py, cpp, out = self.both(z)
            self.assertEqual(py, cpp, "%s: validate.py says %d, x2recomp says %d\n%s" % (name, py, cpp, out))
        put(os.path.join(self.tmp, "g.zip"), b"garbage garbage garbage garbage")
        py, cpp, out = self.both(os.path.join(self.tmp, "g.zip"))
        self.assertEqual((py, cpp), (3, 3), out)


if __name__ == "__main__":
    unittest.main()
