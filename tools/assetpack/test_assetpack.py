# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Tests of tools/assetpack/ with synthetic data only (no game bytes exist in this repository).

    python3 -I tools/assetpack/test_assetpack.py            (also run by CTest as assetpack_tools)

Standard library only. The `jsonschema` cross-check runs when that package is installed and is skipped otherwise.
"""
import io
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import contact_sheet as CS  # noqa: E402
import make_skeleton as MK  # noqa: E402
import merge_dumps as MD  # noqa: E402
import minischema as MS  # noqa: E402
import pack_atlas as PA  # noqa: E402
import pack_lib as P  # noqa: E402
import slice as SL  # noqa: E402

SCHEMA = os.path.join(ROOT, "docs", "schema", "x2-asset-pack-v2.schema.json")
EXAMPLES = os.path.join(ROOT, "docs", "schema", "examples")


def put(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def chunk(ctype, body, crc=None):
    c = zlib.crc32(ctype + body) & 0xFFFFFFFF if crc is None else crc
    return struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", c)


def raw_png(w, h, ctype, depth, rows, plte=None, trns=None, interlace=0, filters=None):
    """A PNG from already-packed scanline bytes `rows` (one bytes object per row); filter 0 unless `filters` is given."""
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)
    raw = b"".join(bytes([(filters or [0] * h)[y]]) + rows[y] for y in range(h))
    out = P.PNG_SIGNATURE + chunk(b"IHDR", ihdr)
    if plte is not None:
        out += chunk(b"PLTE", plte)
    if trns is not None:
        out += chunk(b"tRNS", trns)
    return out + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def gradient(w, h, seed=0, alpha=False):
    img = P.Image(w, h)
    for y in range(h):
        for x in range(w):
            img.put(x, y, ((x * 7 + seed) & 255, (y * 11 + seed * 3) & 255, (x * y + seed) & 255, ((x + y) % 3) * 100 if alpha else 255))
    return img


class TmpTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="x2ap_")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def path(self, *parts):
        return os.path.join(self.tmp, *parts)


class PngCodec(TmpTest):
    def test_round_trip_rgba_and_rgb(self):
        for alpha in (True, False):
            img = gradient(17, 11, 3, alpha)
            data = P.encode_png(img)
            self.assertEqual(P.decode_png(data), img)
            self.assertEqual(data[25], 6 if alpha and not img.is_opaque() else 2)  # IHDR colour type byte
        # deterministic bytes
        self.assertEqual(P.encode_png(gradient(9, 9)), P.encode_png(gradient(9, 9)))

    def test_one_pixel_and_wide_images(self):
        for w, h in ((1, 1), (1, 40), (300, 1)):
            img = gradient(w, h, 5, True)
            self.assertEqual(P.decode_png(P.encode_png(img)), img)

    def test_greyscale_depths(self):
        # 1-bit grey: 8 pixels 10110001
        img = P.decode_png(raw_png(8, 1, 0, 1, [bytes([0b10110001])]))
        self.assertEqual([img.get(x, 0)[0] for x in range(8)], [255, 0, 255, 255, 0, 0, 0, 255])
        # 2-bit and 4-bit grey scale to 0..255
        img = P.decode_png(raw_png(4, 1, 0, 2, [bytes([0b00011011])]))
        self.assertEqual([img.get(x, 0)[0] for x in range(4)], [0, 85, 170, 255])
        img = P.decode_png(raw_png(2, 1, 0, 4, [bytes([0x0F])]))
        self.assertEqual([img.get(x, 0)[0] for x in range(2)], [0, 255])
        # 16-bit grey keeps the high byte
        img = P.decode_png(raw_png(2, 1, 0, 16, [struct.pack(">HH", 0x1234, 0xFFFF)]))
        self.assertEqual([img.get(0, 0)[0], img.get(1, 0)[0]], [0x12, 0xFF])

    def test_grey_alpha_rgb_rgba_16(self):
        img = P.decode_png(raw_png(1, 1, 4, 8, [bytes([100, 50])]))
        self.assertEqual(img.get(0, 0), (100, 100, 100, 50))
        img = P.decode_png(raw_png(1, 1, 2, 8, [bytes([1, 2, 3])]))
        self.assertEqual(img.get(0, 0), (1, 2, 3, 255))
        img = P.decode_png(raw_png(1, 1, 6, 16, [struct.pack(">HHHH", 0x1100, 0x2200, 0x3300, 0x4400)]))
        self.assertEqual(img.get(0, 0), (0x11, 0x22, 0x33, 0x44))

    def test_palette_with_transparency(self):
        pal = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9])
        img = P.decode_png(raw_png(4, 1, 3, 2, [bytes([0b00011011])], plte=pal, trns=bytes([0, 128])))
        self.assertEqual([img.get(x, 0) for x in range(4)],
                         [(255, 0, 0, 0), (0, 255, 0, 128), (0, 0, 255, 255), (9, 9, 9, 255)])
        # an index beyond the palette is an error
        with self.assertRaises(P.PngError):
            P.decode_png(raw_png(1, 1, 3, 8, [bytes([5])], plte=pal))
        with self.assertRaises(P.PngError):
            P.decode_png(raw_png(1, 1, 3, 8, [bytes([0])]))  # no PLTE

    def test_transparent_colour_key(self):
        img = P.decode_png(raw_png(2, 1, 2, 8, [bytes([1, 2, 3, 4, 5, 6])], trns=struct.pack(">HHH", 1, 2, 3)))
        self.assertEqual([img.get(0, 0)[3], img.get(1, 0)[3]], [0, 255])

    def test_filters(self):
        # rows 0..4 use the five filter types; the decoded picture must equal the unfiltered one
        w, h = 5, 5
        base = gradient(w, h, 9, True)
        rows = []
        raw_rows = [bytes(base.data[y * w * 4:(y + 1) * w * 4]) for y in range(h)]
        bpp = 4
        prev = bytes(w * bpp)
        for y, line in enumerate(raw_rows):
            if y == 0:
                f = line
            elif y == 1:
                f = bytes((line[i] - (line[i - bpp] if i >= bpp else 0)) & 255 for i in range(len(line)))
            elif y == 2:
                f = bytes((line[i] - prev[i]) & 255 for i in range(len(line)))
            elif y == 3:
                f = bytes((line[i] - (((line[i - bpp] if i >= bpp else 0) + prev[i]) >> 1)) & 255 for i in range(len(line)))
            else:
                out = bytearray()
                for i in range(len(line)):
                    a = line[i - bpp] if i >= bpp else 0
                    b = prev[i]
                    c = prev[i - bpp] if i >= bpp else 0
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                    out.append((line[i] - pr) & 255)
                f = bytes(out)
            rows.append(f)
            prev = line
        img = P.decode_png(raw_png(w, h, 6, 8, rows, filters=[0, 1, 2, 3, 4]))
        self.assertEqual(img, base)
        with self.assertRaises(P.PngError):
            P.decode_png(raw_png(w, h, 6, 8, raw_rows, filters=[9] * h))

    def test_corrupt_files(self):
        good = P.encode_png(gradient(6, 6, 1, True))
        bad = [b"", b"notapng", good[:20], good[:-5], good[:8] + b"\x00" * 30,
               good[:30] + bytes([good[30] ^ 1]) + good[31:]]  # flipped byte: bad CRC
        for data in bad:
            with self.assertRaises(P.PngError, msg=data[:12]):
                P.decode_png(data)
        # bad chunk CRC with a correct length
        broken = P.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0), crc=0)
        with self.assertRaises(P.PngError):
            P.decode_png(broken)

    def test_interlaced_refused(self):
        with self.assertRaises(P.PngError) as cm:
            P.decode_png(raw_png(1, 1, 2, 8, [bytes([1, 2, 3])], interlace=1))
        self.assertIn("interlaced", str(cm.exception))

    def test_limits_are_checked_before_allocation(self):
        # IHDR claims 60000 x 60000: refused without decoding or allocating
        huge = P.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 60000, 60000, 8, 6, 0, 0, 0)) + chunk(b"IEND", b"")
        with self.assertRaises(P.PngError) as cm:
            P.decode_png(huge)
        self.assertIn("larger than the limit", str(cm.exception))
        # a wide but low picture beyond the pixel limit
        wide = P.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 4096, 4096, 8, 6, 0, 0, 0)) + chunk(b"IEND", b"")
        with self.assertRaises(P.PngError):
            P.decode_png(wide)
        # raised limits are honoured for backgrounds
        P.decode_png(raw_png(2, 2, 2, 8, [bytes(6)] * 2), P.MAX_SIDE_BACKGROUND, P.MAX_PIXELS_BACKGROUND)

    def test_decompression_bomb_and_wrong_lengths(self):
        # a tiny IDAT that inflates to a lot more than the 1x1 picture needs
        ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
        bomb = zlib.compress(b"\x00" * (50 * 1024 * 1024), 9)
        data = P.PNG_SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", bomb) + chunk(b"IEND", b"")
        with self.assertRaises(P.PngError):
            P.decode_png(data)
        short = P.PNG_SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"\x00\x01")) + chunk(b"IEND", b"")
        with self.assertRaises(P.PngError):
            P.decode_png(short)

    def test_file_helpers(self):
        img = gradient(8, 8, 2, True)
        p = self.path("sub", "a.png")
        P.write_png(p, img)
        self.assertEqual(P.read_png(p), img)
        with self.assertRaises(P.PngError):
            P.read_png(self.path("missing.png"))


class ImageOps(unittest.TestCase):
    def test_crop_paste_scaled(self):
        img = gradient(10, 8, 4)
        c = img.crop(2, 3, 4, 2)
        self.assertEqual((c.w, c.h), (4, 2))
        self.assertEqual(c.get(0, 0), img.get(2, 3))
        with self.assertRaises(P.PackError):
            img.crop(8, 0, 4, 4)
        canvas = P.Image(10, 10)
        canvas.paste(c, 8, 9)  # clipped on the right and bottom
        self.assertEqual(canvas.get(8, 9), c.get(0, 0))
        self.assertEqual(canvas.get(9, 9), c.get(1, 0))
        big = c.scaled(3)
        self.assertEqual((big.w, big.h), (12, 6))
        self.assertEqual(big.get(5, 4), c.get(1, 1))
        with self.assertRaises(P.PackError):
            c.scaled(9)

    def test_bleed_extends_edges(self):
        sheet = P.Image(12, 12)
        frame = gradient(4, 4, 7)
        sheet.paste(frame, 4, 4)
        sheet.bleed(4, 4, 4, 4, 2)
        self.assertEqual(sheet.get(2, 4), frame.get(0, 0))
        self.assertEqual(sheet.get(3, 7), frame.get(0, 3))
        self.assertEqual(sheet.get(9, 9), frame.get(3, 3))
        self.assertEqual(sheet.get(7, 2), frame.get(3, 0))


class KeysPathsJson(TmpTest):
    def test_keys(self):
        self.assertEqual(P.parse_gfx_key("0048848ccf64d966-16x12"), ("0048848ccf64d966", 16, 12))
        self.assertEqual(P.format_gfx_key(0x48848ccf64d966, 16, 12), "0048848ccf64d966-16x12")
        for bad in ("0048848ccf64d96-16x12", "0048848CCF64D966-16x12", "0048848ccf64d966-0x12", "x", "0048848ccf64d966-16x12 "):
            with self.assertRaises(P.PackError, msg=bad):
                P.parse_gfx_key(bad)
        self.assertEqual(P.parse_clip_key("0123456789abcdef-640"), ("0123456789abcdef", 640))
        self.assertEqual(P.parse_cell_key("0123456789abcdef-8")[1], 8)
        with self.assertRaises(P.PackError):
            P.parse_cell_key("0123456789abcdef-9")
        with self.assertRaises(P.PackError):
            P.format_gfx_key(1 << 64, 1, 1)

    def test_safe_paths(self):
        for ok in ("a.png", "sprites/ship.png", "a/b/c.wav", "x" * 240):
            self.assertTrue(P.is_safe_rel_path(ok), ok)
        for bad in ("", "/etc/passwd", "\\win", "a\\b.png", "../x.png", "a/../x.png", "a/./b.png", "a//b.png", "C:x.png",
                    "c:/x.png", "a/b/..", "x" * 241, "a\x01b.png", "a/", "./a"):
            self.assertFalse(P.is_safe_rel_path(bad), repr(bad))

    def test_safe_join_and_symlink_escape(self):
        root = self.path("pack")
        os.makedirs(os.path.join(root, "sub"))
        self.assertTrue(P.safe_join(root, "sub/x.png").endswith(os.path.join("sub", "x.png")))
        with self.assertRaises(P.PackError):
            P.safe_join(root, "../x")
        outside = self.path("outside")
        os.makedirs(outside)
        try:
            os.symlink(outside, os.path.join(root, "link"))
        except (OSError, NotImplementedError, AttributeError):
            self.skipTest("symlinks are not available")
        with self.assertRaises(P.PackError):
            P.safe_join(root, "link/secret.png")

    def test_json_helpers(self):
        p = self.path("m.json")
        P.write_json_file(p, {"b": 1, "a": [1, 2], "é": "ü"})
        with open(p, "rb") as f:
            raw = f.read()
        self.assertTrue(raw.endswith(b"\n") and b"\r" not in raw)
        self.assertEqual(P.read_json_file(p), {"b": 1, "a": [1, 2], "é": "ü"})
        self.assertLess(raw.index(b'"b"'), raw.index(b'"a"'))  # insertion order kept
        put(p, b"\xef\xbb\xbf{\"x\": 1}")
        self.assertEqual(P.read_json_file(p), {"x": 1})
        put(p, b"{not json")
        with self.assertRaises(P.PackError):
            P.read_json_file(p)
        with self.assertRaises(P.PackError):
            P.read_json_file(p, max_bytes=3)
        with self.assertRaises(P.PackError):
            P.read_json_file(self.path("none.json"))


def make_frames(sizes, scale=1):
    frames = {}
    for i, (w, h) in enumerate(sizes):
        key = P.format_gfx_key(0x100 + i, w, h)
        frames[key] = gradient(w * scale, h * scale, i + 1, True)
    return frames


class Atlas(TmpTest):
    def test_packing_is_deterministic_and_non_overlapping(self):
        frames = make_frames([(16, 12), (8, 8), (16, 16), (24, 10), (8, 8), (5, 30), (16, 12)], scale=2)
        a = PA.build_entries(frames, "s.png", 2, padding=1, bleed=1)
        b = PA.build_entries(dict(reversed(list(frames.items()))), "s.png", 2, padding=1, bleed=1)
        self.assertEqual(P.encode_png(a[0][1]), P.encode_png(b[0][1]))
        self.assertEqual(a[0][2], b[0][2])
        entry = a[0][2]
        sheet = a[0][1]
        boxes = []
        for f in entry["frames"]:
            x, y, w, h = f["rect"]
            self.assertEqual(sheet.crop(x, y, w, h), frames[f["key"]])
            boxes.append((x - 1, y - 1, x + w + 1, y + h + 1))
        for i, p in enumerate(boxes):
            for q in boxes[i + 1:]:
                self.assertTrue(p[2] <= q[0] or q[2] <= p[0] or p[3] <= q[1] or q[3] <= p[1], (p, q))
        self.assertEqual(entry["scale"], 2)
        self.assertEqual(len(entry["frames"]), 7)

    def test_bleed_and_padding_limits(self):
        frames = make_frames([(8, 8)])
        with self.assertRaises(P.PackError):
            PA.build_entries(frames, "s.png", None, padding=1, bleed=2)
        (name, sheet, entry), = PA.build_entries(frames, "s.png", None, padding=2, bleed=2)
        x, y, w, h = entry["frames"][0]["rect"]
        self.assertEqual((x, y), (2, 2))
        self.assertEqual(sheet.get(0, y), sheet.get(x, y))  # bled into the padding

    def test_pot_width_and_split(self):
        frames = make_frames([(30, 30)] * 1 + [(20, 20)] * 9)
        # keys of the make_frames helper differ by counter, so these are 10 distinct frames
        (_, sheet, _), = PA.build_entries(frames, "s.png", None, pot=True)
        self.assertTrue(sheet.w & (sheet.w - 1) == 0 and sheet.h & (sheet.h - 1) == 0)
        with self.assertRaises(P.PackError):
            PA.build_entries(frames, "s.png", None, max_size=64)
        entries = PA.build_entries(frames, "s.png", None, max_size=64, split=True)
        self.assertGreater(len(entries), 1)
        self.assertEqual([e[0] for e in entries][:2], ["s.png", "s-2.png"])
        self.assertEqual(sum(len(e[2]["frames"]) for e in entries), 10)
        for _, sheet, _ in entries:
            self.assertLessEqual(max(sheet.w, sheet.h), 64)
        with self.assertRaises(P.PackError):
            PA.build_entries(make_frames([(70, 10)]), "s.png", None, max_size=64, split=True)

    def test_fixed_width_and_errors(self):
        frames = make_frames([(16, 16)] * 1)
        with self.assertRaises(P.PackError):
            PA.build_entries(frames, "s.png", None, width=10)
        with self.assertRaises(P.PackError):
            PA.pack_sheets({})
        (_, sheet, _), = PA.build_entries(frames, "s.png", None, width=64, padding=0)
        self.assertEqual(sheet.w, 16)  # trimmed to the used extent

    def test_grid_layout_with_empty_cell(self):
        frames = make_frames([(8, 8)] * 3)
        keys = sorted(frames)
        order = [keys[0], "-", keys[1], keys[2]]
        (_, sheet, entry), = PA.build_entries(frames, "g.png", 1, layout="grid", columns=2, grid_order=order)
        self.assertEqual(entry["grid"], {"cell": [8, 8], "columns": 2, "keys": [keys[0], None, keys[1], keys[2]]})
        self.assertEqual((sheet.w, sheet.h), (16, 16))
        self.assertEqual(sheet.get(8, 0), (0, 0, 0, 0))  # the empty cell
        out = dict(SL.slice_entry(sheet, entry))
        self.assertEqual(out, frames)
        with self.assertRaises(P.PackError):
            PA.build_entries(frames, "g.png", 1, layout="grid", grid_order=[keys[0], keys[1]])
        with self.assertRaises(P.PackError):
            PA.build_entries(make_frames([(8, 8), (9, 9)]), "g.png", 1, layout="grid")

    def test_collect_frames_checks_sizes_and_duplicates(self):
        root = self.path("frames")
        frames = make_frames([(8, 8), (4, 6)], scale=2)
        for k, img in frames.items():
            P.write_png(os.path.join(root, "sprites", k + ".png"), img)
        P.write_png(os.path.join(root, "sprites", "readme.png"), gradient(3, 3))  # not a key: ignored
        got = PA.collect_frames(root, 2)
        self.assertEqual(got, frames)
        with self.assertRaises(P.PackError) as cm:
            PA.collect_frames(root, 3)
        self.assertIn("scale 3", str(cm.exception))
        self.assertEqual(len(PA.collect_frames(root, 3, allow_mismatch=True)), 2)
        # the same key twice with different pictures is an error, with the same picture it is fine
        k = sorted(frames)[0]
        P.write_png(os.path.join(root, "tiles", k + ".png"), frames[k])
        PA.collect_frames(root, 2)
        P.write_png(os.path.join(root, "tiles", k + ".png"), gradient(frames[k].w, frames[k].h, 99))
        with self.assertRaises(P.PackError):
            PA.collect_frames(root, 2)

    def test_cli_pack_into_manifest_and_slice_back(self):
        root = self.path("pack")
        frames = make_frames([(8, 8), (8, 6), (12, 12)], scale=2)
        src = self.path("src")
        for k, img in frames.items():
            P.write_png(os.path.join(src, k + ".png"), img)
        manifest = os.path.join(root, "manifest.json")
        os.makedirs(root)
        P.write_json_file(manifest, {"format": "x2-asset-pack", "version": 1, "scale": 2})
        rc = PA.main([src, "--out-png", os.path.join(root, "sheets", "a.png"), "--into", manifest, "--scale", "2", "--bleed", "1"])
        self.assertEqual(rc, 0)
        m = P.read_json_file(manifest)
        self.assertEqual(m["version"], 2)  # an atlas needs version 2
        self.assertEqual(m["atlases"][0]["png"], "sheets/a.png")
        # packing again replaces the entry instead of adding a second
        PA.main([src, "--out-png", os.path.join(root, "sheets", "a.png"), "--into", manifest, "--scale", "2"])
        self.assertEqual(len(P.read_json_file(manifest)["atlases"]), 1)
        # slice it back with the manifest
        out = self.path("out")
        self.assertEqual(SL.main(["--manifest", manifest, "--out", out]), 0)
        for k, img in frames.items():
            self.assertEqual(P.read_png(os.path.join(out, k + ".png")), img)
        # size errors and unsafe paths are reported, not raised
        self.assertEqual(PA.main([src, "--out-png", os.path.join(root, "x.png"), "--scale", "3"]), 1)
        self.assertEqual(PA.main([src, "--out-png", os.path.join(root, "x.png"), "--png-ref", "../x.png", "--scale", "2"]), 1)

    def test_fragment_output(self):
        src = self.path("src")
        frames = make_frames([(8, 8)])
        for k, img in frames.items():
            P.write_png(os.path.join(src, k + ".png"), img)
        frag = self.path("frag.json")
        self.assertEqual(PA.main([src, "--out-png", self.path("sheets", "s.png"), "--out-json", frag, "--pack-root", self.tmp]), 0)
        self.assertEqual(P.read_json_file(frag)["atlases"][0]["png"], "sheets/s.png")
        self.assertTrue(os.path.isfile(self.path("sheets", "s.png")))


class Slice(TmpTest):
    def test_grid_unkeyed_and_keyed(self):
        sheet = gradient(24, 16, 3)
        cells = SL.slice_grid(sheet, (8, 8))
        self.assertEqual([c[0] for c in cells][:4], ["cell_000_000", "cell_000_001", "cell_000_002", "cell_001_000"])
        self.assertEqual(len(cells), 6)
        k1, k2 = P.format_gfx_key(1, 8, 8), P.format_gfx_key(2, 8, 8)
        keyed = SL.slice_grid(sheet, (8, 8), 3, [k1, "-", k2])
        self.assertEqual([c[0] for c in keyed], [k1, k2])
        self.assertEqual(keyed[1][1], sheet.crop(16, 0, 8, 8))
        with self.assertRaises(P.PackError):
            SL.slice_grid(sheet, (8, 8), 4)
        with self.assertRaises(P.PackError):
            SL.slice_grid(sheet, (8, 8), 3, [k1] * 7)
        with self.assertRaises(P.PackError):
            SL.slice_grid(sheet, (8, 8), 3, ["bad"])

    def test_rectangles_must_lie_inside(self):
        sheet = gradient(16, 16)
        k = P.format_gfx_key(5, 8, 8)
        with self.assertRaises(P.PackError):
            SL.slice_entry(sheet, {"frames": [{"key": k, "rect": [12, 12, 8, 8]}]})
        with self.assertRaises(P.PackError):
            SL.slice_entry(sheet, {"png": "x.png"})
        with self.assertRaises(P.PackError):
            SL.slice_entry(sheet, {"frames": [{"key": "nokey", "rect": [0, 0, 1, 1]}]})

    def test_cli_frames_file_and_grid(self):
        sheet = gradient(16, 8, 4)
        sp = self.path("sheet.png")
        P.write_png(sp, sheet)
        k1, k2 = P.format_gfx_key(1, 8, 8), P.format_gfx_key(2, 8, 8)
        fj = self.path("frames.json")
        P.write_json_file(fj, [{"key": k1, "rect": [0, 0, 8, 8]}, {"key": k2, "rect": [8, 0, 8, 8]}])
        out = self.path("o1")
        self.assertEqual(SL.main([sp, "--frames", fj, "--out", out]), 0)
        self.assertEqual(P.read_png(os.path.join(out, k2 + ".png")), sheet.crop(8, 0, 8, 8))
        kf = self.path("keys.txt")
        put(kf, "%s\n-\n" % k1)
        out2 = self.path("o2")
        self.assertEqual(SL.main([sp, "--grid", "8,8", "--keys", kf, "--out", out2]), 0)
        self.assertEqual(os.listdir(out2), [k1 + ".png"])
        out3 = self.path("o3")
        self.assertEqual(SL.main([sp, "--grid", "8,8", "--unkeyed", "--out", out3]), 0)
        self.assertEqual(sorted(os.listdir(out3)), ["cell_000_000.png", "cell_000_001.png"])
        # a grid needs keys or --unkeyed; a manifest with several atlases needs a choice
        self.assertEqual(SL.main([sp, "--grid", "8,8", "--out", self.path("o4")]), 1)
        m = self.path("m", "manifest.json")
        os.makedirs(os.path.dirname(m))
        P.write_png(self.path("m", "a.png"), sheet)
        P.write_png(self.path("m", "b.png"), sheet)
        P.write_json_file(m, {"atlases": [
            {"png": "a.png", "frames": [{"key": k1, "rect": [0, 0, 8, 8]}]},
            {"png": "b.png", "frames": [{"key": k2, "rect": [8, 0, 8, 8]}]}]})
        self.assertEqual(SL.main(["--manifest", m, "--out", self.path("o5")]), 1)
        self.assertEqual(SL.main(["--manifest", m, "--atlas", "1", "--out", self.path("o6")]), 0)
        self.assertEqual(os.listdir(self.path("o6")), [k2 + ".png"])
        self.assertEqual(SL.main(["--manifest", m, "--all", "--out", self.path("o7")]), 0)
        self.assertEqual(len(os.listdir(self.path("o7"))), 2)
        # an unsafe png path in a manifest is refused
        P.write_json_file(m, {"atlases": [{"png": "../a.png", "frames": [{"key": k1, "rect": [0, 0, 8, 8]}]}]})
        self.assertEqual(SL.main(["--manifest", m, "--out", self.path("o8")]), 1)


class ContactSheet(TmpTest):
    def make_pack(self):
        root = self.path("dump")
        frames = make_frames([(8, 8), (16, 12), (4, 20)])
        entries = {}
        for i, (k, img) in enumerate(sorted(frames.items())):
            rel = "sprites/%s.png" % k
            P.write_png(os.path.join(root, rel), img)
            entries[k] = {"png": rel, "kind": "tile" if i == 2 else "sprite"}
        P.write_json_file(os.path.join(root, "manifest.json"),
                          {"format": "x2-asset-pack", "version": 2, "graphics": entries, "groups": {"g": sorted(frames, reverse=True)}})
        return root, frames

    def test_items_kinds_and_groups(self):
        root, frames = self.make_pack()
        self.assertEqual({k for k, _, _ in CS.load_items(root)}, set(frames))
        self.assertEqual(len(CS.load_items(root, "tile")), 1)
        self.assertEqual(len(CS.load_items(root, "sprite")), 2)
        self.assertEqual([k for k, _, _ in CS.load_items(root, group="g")], sorted(frames, reverse=True))
        with self.assertRaises(P.PackError):
            CS.load_items(root, group="nope")

    def test_plain_folder_and_atlas_frames(self):
        root = self.path("plain")
        frames = make_frames([(8, 8), (8, 9)])
        for k, img in frames.items():
            P.write_png(os.path.join(root, k + ".png"), img)
        self.assertEqual({k for k, _, _ in CS.load_items(root)}, set(frames))
        # a manifest folder whose graphics live in an atlas
        m = self.path("atl")
        (_, sheet, entry), = PA.build_entries(frames, "s.png", 1)
        P.write_png(os.path.join(m, "s.png"), sheet)
        P.write_json_file(os.path.join(m, "manifest.json"), {"format": "x2-asset-pack", "version": 2, "atlases": [entry]})
        self.assertEqual({k for k, _, _ in CS.load_items(m)}, set(frames))

    def test_render_and_pagination(self):
        root, frames = self.make_pack()
        items = CS.load_items(root)
        pages = CS.build_pages(items, scale=2, columns=2, cell=64, max_height=4096)
        self.assertEqual(len(pages), 1)
        page = pages[0]
        self.assertGreater(page.w, 2 * 64)
        self.assertFalse(all(b == page.data[0] for b in page.data[:2000]))  # not blank
        many = [(P.format_gfx_key(i, 8, 8), gradient(8, 8, i), "sprite") for i in range(1, 40)]
        pages = CS.build_pages(many, scale=1, columns=4, cell=32, max_height=200)
        self.assertGreater(len(pages), 1)
        self.assertTrue(all(p.h <= 200 for p in pages))
        self.assertEqual(CS.page_names("a/b.png", 3), ["a/b.png", "a/b-2.png", "a/b-3.png"])
        with self.assertRaises(P.PackError):
            CS.build_pages([])
        # large pictures are shrunk by an integer step and the label says so
        big = CS.render_cell(P.format_gfx_key(1, 300, 300), gradient(300, 300), 1, 64)
        self.assertGreaterEqual(big.w, 64)

    def test_cli(self):
        root, _ = self.make_pack()
        out = self.path("sheet.png")
        self.assertEqual(CS.main([root, "--out", out, "--scale", "1", "--cell", "32"]), 0)
        self.assertTrue(P.read_png(out, P.MAX_SIDE_BACKGROUND, P.MAX_PIXELS_BACKGROUND).w > 32)
        self.assertEqual(CS.main([root, "--out", out, "--scale", "9"]), 1)
        self.assertEqual(CS.main([self.path("empty"), "--out", out]), 1)


def write_dump(root, name, graphics, far_lines, palettes, scale=1):
    """A synthetic version 1 dump: graphics {key: (Image, uses, palette index)}, far line hashes with their rows."""
    entries = {}
    for key, (img, uses, pal) in graphics.items():
        rel = "sprites/%s.png" % key
        P.write_png(os.path.join(root, rel), img)
        _, w, h = P.parse_gfx_key(key)
        entries[key] = {"png": rel, "kind": "sprite", "w": w, "h": h, "uses": uses, "first_frame": 10, "palette": pal}
    far = []
    if far_lines:
        strip = P.Image(8 * scale, 192 * scale)
        lines = ["-"] * 192
        for i, (h, shade) in enumerate(far_lines):
            strip.fill_rect(0, i * scale, 8 * scale, scale, (shade, shade, shade, 255))
            lines[i] = h
        P.write_png(os.path.join(root, "far", "strip_000.png"), strip)
        far = [{"png": "far/strip_000.png", "lines": lines}]
    os.makedirs(root, exist_ok=True)
    P.write_json_file(os.path.join(root, "manifest.json"), {
        "format": "x2-asset-pack", "version": 1, "name": name, "note": "n", "scale": scale,
        "graphics": entries, "far": far, "seen_palettes": palettes, "palettes": []})


class MergeDumps(TmpTest):
    def test_merge_graphics_far_and_palettes(self):
        ka, kb, kc = P.format_gfx_key(1, 4, 4), P.format_gfx_key(2, 4, 4), P.format_gfx_key(3, 4, 4)
        dim = P.Image(4, 4, bytes([10, 10, 10, 255]) * 16)
        bright = P.Image(4, 4, bytes([200, 200, 200, 255]) * 16)
        pal0 = ["#000"] * 16
        pal1 = ["#%x%x%x" % (i, i, i) for i in range(16)]
        a, b = self.path("a"), self.path("b")
        write_dump(a, "A", {ka: (dim, 3, 0), kb: (bright, 2, 1)}, [("1" * 16, 50), ("2" * 16, 60)], [pal0, pal1])
        write_dump(b, "B", {ka: (bright, 4, 0), kc: (dim, 1, 1)}, [("2" * 16, 60), ("3" * 16, 70)], [pal1, pal0])
        out = self.path("m")
        m = MD.merge_dumps([a, b], out)
        self.assertEqual(sorted(m["graphics"]), sorted([ka, kb, kc]))
        self.assertEqual(m["graphics"][ka]["uses"], 7)
        self.assertEqual(P.read_png(os.path.join(out, "sprites", ka + ".png")), bright)  # the brighter copy won
        self.assertEqual(m["seen_palettes"], [pal0, pal1])  # union in order of first appearance
        # palette indices are remapped into the merged list: kc came from B's palette 1 (= pal0, merged index 0)
        self.assertEqual(m["graphics"][kc]["palette"], 0)
        self.assertEqual(m["graphics"][kb]["palette"], 1)
        # far: every distinct line once, rebuilt into 192-line strips, with the right rows
        self.assertEqual(len(m["far"]), 1)
        lines = m["far"][0]["lines"]
        self.assertEqual(lines[:3], ["1" * 16, "2" * 16, "3" * 16])
        self.assertEqual(lines[3:], ["-"] * 189)
        strip = P.read_png(os.path.join(out, "far", "strip_000.png"))
        self.assertEqual([strip.get(0, i)[0] for i in range(3)], [50, 60, 70])
        # the merged dump is a valid version 1 manifest
        self.assertEqual(MS.validate(P.read_json_file(os.path.join(out, "manifest.json")), P.read_json_file(SCHEMA)), [])

    def test_errors_and_extra_folders(self):
        a = self.path("a")
        write_dump(a, "A", {P.format_gfx_key(1, 4, 4): (gradient(4, 4), 1, 0)}, [], [["#000"] * 16])
        os.makedirs(os.path.join(a, "screens"))
        put(os.path.join(a, "screens", "x.txt"), "s")
        out = self.path("m")
        MD.merge_dumps([a], out)
        self.assertTrue(os.path.isfile(os.path.join(out, "screens", "x.txt")))
        with self.assertRaises(P.PackError):
            MD.merge_dumps([a], out)  # not empty
        with self.assertRaises(P.PackError):
            MD.merge_dumps([], self.path("none"))
        with self.assertRaises(P.PackError):
            MD.merge_dumps([self.path("missing")], self.path("m2"))
        b = self.path("b")
        write_dump(b, "B", {}, [], [], scale=2)
        with self.assertRaises(P.PackError):
            MD.merge_dumps([a, b], self.path("m3"))

    def test_png_path_escape_is_refused(self):
        a = self.path("a")
        write_dump(a, "A", {P.format_gfx_key(1, 4, 4): (gradient(4, 4), 1, 0)}, [], [])
        m = P.read_json_file(os.path.join(a, "manifest.json"))
        m["graphics"][P.format_gfx_key(1, 4, 4)]["png"] = "../a/sprites/x.png"
        P.write_json_file(os.path.join(a, "manifest.json"), m)
        with self.assertRaises(P.PackError):
            MD.merge_dumps([a], self.path("o"))


def pcm_of(data):
    with wave.open(io.BytesIO(data), "rb") as w:
        return w.getnchannels(), w.getsampwidth(), w.getframerate(), struct.unpack("<%dh" % w.getnframes(), w.readframes(w.getnframes()))


class Skeleton(TmpTest):
    def test_skeleton_is_valid_consistent_and_deterministic(self):
        out = self.path("skel")
        m1 = MK.write_skeleton(out)
        self.assertEqual(MK.check_pack(out), [])
        m2, files2 = MK.build_skeleton()
        m3, files3 = MK.build_skeleton()
        self.assertEqual(files2, files3)  # byte for byte, run to run
        self.assertEqual(m1, m2)
        for section in ("graphics", "far", "palettes", "backgrounds", "atlases", "groups", "stars", "screens", "panel", "audio"):
            self.assertIn(section, m1)
        self.assertEqual(set(m1["audio"]), {"sfx", "speech", "music"})
        self.assertEqual(m1["version"], 2)
        total = sum(len(d) for d in files2.values())
        self.assertLess(total, 200 * 1024)
        for rel, data in files2.items():
            self.assertLess(len(data), 64 * 1024, rel)  # the hygiene allowance for this folder
        # no key of the skeleton is a plausible real hash: all are 15 zeros and a counter
        keys = list(m1["graphics"]) + list(m1["audio"]["sfx"]) + list(m1["audio"]["speech"]) + list(m1["panel"]["cells"])
        for k in keys:
            self.assertTrue(k.replace("f:", "", 1).startswith("00000000000000"), k)
        self.assertTrue(any(k.startswith("f:") for k in m1["graphics"]))  # an explicit hit-flash key
        self.assertTrue(any("flash_rect" in f for a in m1["atlases"] for f in a.get("frames", [])))

    def test_other_scale_and_force(self):
        out = self.path("s3")
        MK.write_skeleton(out, scale=3)
        self.assertEqual(MK.check_pack(out), [])
        with self.assertRaises(P.PackError):
            MK.write_skeleton(out)
        MK.write_skeleton(out, scale=1, force=True)
        self.assertEqual(MK.check_pack(out), [])
        with self.assertRaises(P.PackError):
            MK.build_skeleton(scale=9)
        self.assertEqual(MK.main([self.path("s4"), "--check"]), 0)
        self.assertEqual(MK.main([self.path("s4")]), 1)  # exists and is not empty

    def test_check_pack_finds_problems(self):
        out = self.path("bad")
        MK.write_skeleton(out)
        os.remove(os.path.join(out, "sprites", "ship.png"))
        P.write_png(os.path.join(out, "tiles", "wall.png"), gradient(5, 5))
        m = P.read_json_file(os.path.join(out, "manifest.json"))
        m["audio"]["sfx"]["0000000000000051-640"]["file"] = "sfx/missing.wav"
        m["screens"]["title"]["fade"] = "sideways"
        P.write_json_file(os.path.join(out, "manifest.json"), m)
        text = "\n".join(MK.check_pack(out))
        self.assertIn("ship", text)
        self.assertIn("expected 32x32", text)
        self.assertIn("missing.wav", text)
        self.assertIn("screens.title.fade", text)

    def test_committed_skeleton_is_current(self):
        committed = os.path.join(ROOT, "examples", "asset-pack-skeleton")
        if not os.path.isdir(committed):
            self.skipTest("examples/asset-pack-skeleton is not part of this source tree")
        self.assertEqual(MK.check_pack(committed), [])
        manifest, files = MK.build_skeleton()
        self.assertEqual(P.read_json_file(os.path.join(committed, "manifest.json")), manifest)
        on_disk = set()
        for dp, _, fns in os.walk(committed):
            for fn in fns:
                on_disk.add(os.path.relpath(os.path.join(dp, fn), committed).replace(os.sep, "/"))
        self.assertEqual(on_disk, set(files) | {"manifest.json"})
        for rel, data in files.items():
            with open(os.path.join(committed, *rel.split("/")), "rb") as f:
                disk = f.read()
            if rel.endswith(".png"):
                # compressed bytes can differ between zlib builds: compare the pictures
                self.assertEqual(P.decode_png(disk, 4096, 8 << 20), P.decode_png(data, 4096, 8 << 20), rel)
            elif rel.endswith(".wav"):
                # sin/exp can differ in the last place between libms: compare the samples within 2 LSB
                a, b = pcm_of(disk), pcm_of(data)
                self.assertEqual(a[:3], b[:3], rel)
                self.assertEqual(len(a[3]), len(b[3]), rel)
                self.assertLessEqual(max(abs(x - y) for x, y in zip(a[3], b[3])), 2, rel)
            else:
                # a Windows checkout may have turned LF into CRLF
                self.assertEqual(disk.replace(b"\r\n", b"\n"), data, rel)


class SchemaChecks(unittest.TestCase):
    def setUp(self):
        self.schema = P.read_json_file(SCHEMA)

    def examples(self):
        return [P.read_json_file(os.path.join(EXAMPLES, n)) for n in sorted(os.listdir(EXAMPLES)) if n.endswith(".json")]

    def bad_manifests(self):
        ok = {"format": "x2-asset-pack", "version": 2}
        key = "0123456789abcdef-16x12"
        return [
            {"format": "x", "version": 2},
            {"version": 2},
            {"format": "x2-asset-pack", "version": 3},
            dict(ok, graphics={"nothex-1x1": {"png": "a.png"}}),
            dict(ok, graphics={key: {"png": "../a.png"}}),
            dict(ok, graphics={key: {"png": "/abs/a.png"}}),
            dict(ok, graphics={key: {"png": "a\\b.png"}}),
            dict(ok, graphics={key: {"png": "C:/a.png"}}),
            dict(ok, graphics={key: {}}),
            dict(ok, scale=9),
            dict(ok, scale=0),
            {"format": "x2-asset-pack", "version": 1, "audio": {}},  # v2 section in a version 1 pack
            dict(ok, games=["amiga"]),
            dict(ok, games=[]),
            dict(ok, audio={"music": {"slots": {"level9": {"file": "a.ogg"}}}}),
            dict(ok, audio={"sfx": {"abc-1": {"file": "a.wav"}}}),
            dict(ok, audio={"sfx": {"0123456789abcdef-640": {"file": "a.wav", "gain_db": 99}}}),
            dict(ok, audio={"music": {"slots": {"title": {"gain_db": 0}}}}),
            dict(ok, atlases=[{"png": "a.png"}]),
            dict(ok, screens={"Title": {"signature": {"blocks": [], "min": 1}, "png": "a.png"}}),
            dict(ok, panel={"cell_w": 12}),
            dict(ok, id="Bad Id"),
            dict(ok, pack_version="one"),
        ]

    def test_examples_validate(self):
        for m in self.examples():
            self.assertEqual(MS.validate(m, self.schema), [])

    def test_bad_manifests_are_rejected(self):
        for i, m in enumerate(self.bad_manifests()):
            self.assertTrue(MS.validate(m, self.schema), "bad manifest %d was accepted: %r" % (i, m))

    def test_minischema_keywords(self):
        v = MS.validate
        self.assertEqual(v(5, {"type": "integer", "minimum": 1, "maximum": 5}), [])
        self.assertTrue(v(True, {"type": "integer"}))
        self.assertTrue(v(5.5, {"type": "integer"}))
        self.assertEqual(v(5.0, {"type": "integer"}), [])
        self.assertTrue(v([1, 1], {"uniqueItems": True}))
        self.assertTrue(v(1, {"const": True}))
        self.assertTrue(v("a", {"enum": ["b"]}))
        self.assertTrue(v({"a": 1}, {"additionalProperties": False}))
        self.assertTrue(v({}, {"required": ["a"]}))
        self.assertEqual(v(None, {"type": ["string", "null"]}), [])
        self.assertTrue(v(1, {"oneOf": [{"type": "integer"}, {"minimum": 0}]}))  # matches both
        self.assertTrue(v(1, {"not": {"type": "integer"}}))
        self.assertTrue(v("ab", {"maxLength": 1}))
        self.assertEqual(v({"x": 1}, {"properties": {"x": {"$ref": "#/$defs/n"}}, "$defs": {"n": {"type": "integer"}}}), [])
        self.assertTrue(v({"x": 1}, {"properties": {"x": False}}))

    def test_agrees_with_jsonschema_when_installed(self):
        try:
            import jsonschema
        except ImportError:
            self.skipTest("the jsonschema package is not installed")
        if not hasattr(jsonschema, "Draft202012Validator"):
            self.skipTest("the installed jsonschema package predates draft 2020-12")
        validator = jsonschema.Draft202012Validator(self.schema)
        for m in self.examples():
            self.assertEqual(list(validator.iter_errors(m)), [])
        for i, m in enumerate(self.bad_manifests()):
            self.assertTrue(list(validator.iter_errors(m)), "jsonschema accepted bad manifest %d" % i)
        manifest, _ = MK.build_skeleton()
        self.assertEqual(list(validator.iter_errors(manifest)), [])


@unittest.skipIf(os.name == "nt" or shutil.which("sh") is None, "dump_all.sh needs a POSIX shell")
class DumpAllScript(TmpTest):
    FAKE = r'''
import os, sys
sys.path.insert(0, %r)
import pack_lib as P
args = sys.argv[1:]
out = args[args.index("--dump-assets") + 1]
script = os.path.basename(args[args.index("--input-script") + 1])
if script.startswith("fail"):
    sys.exit(3)
n = 1 if script.startswith("a") else 2
key = P.format_gfx_key(n, 4, 4)
img = P.Image(4, 4, bytes([60 * n, 10, 10, 255]) * 16)
os.makedirs(os.path.join(out, "sprites"))
P.write_png(os.path.join(out, "sprites", key + ".png"), img)
P.write_json_file(os.path.join(out, "manifest.json"), {"format": "x2-asset-pack", "version": 1, "scale": 1,
    "graphics": {key: {"png": "sprites/%%s.png" %% key, "kind": "sprite", "w": 4, "h": 4, "uses": 1, "first_frame": 1, "palette": 0}},
    "far": [], "seen_palettes": [["#000"] * 16], "palettes": []})
with open(os.path.join(os.environ["X2_TEST_ARGS_DIR"], "args_" + script), "w") as f:
    f.write(" ".join(args))
'''

    def setUp(self):
        super().setUp()
        self.fake_py = self.path("fake.py")
        with open(self.fake_py, "w") as f:
            f.write(self.FAKE % HERE)
        self.exe = self.path("fake_x2recomp")
        with open(self.exe, "w") as f:
            f.write('#!/bin/sh\nexec "%s" "%s" "$@"\n' % (sys.executable, self.fake_py))
        os.chmod(self.exe, os.stat(self.exe).st_mode | stat.S_IXUSR)
        self.replays = self.path("replays")
        os.makedirs(self.replays)
        for n in ("a_first.txt", "b_second.txt", "fail_third.txt"):
            put(os.path.join(self.replays, n), "0 0\n")

    def run_script(self, *extra, expect=0):
        cmd = ["sh", os.path.join(HERE, "dump_all.sh"), "-x", self.exe, "-r", self.replays, "-o", self.path("dump")] + list(extra)
        r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, PYTHON=sys.executable, X2_TEST_ARGS_DIR=self.tmp))
        self.assertEqual(r.returncode, expect, r.stderr)
        return r

    def test_merges_replays_and_skips_failures(self):
        r = self.run_script("-g", "cdtv", "-a", "/assets dir", "-f", "123")
        self.assertIn("2 replays merged", r.stderr)
        self.assertIn("1 skipped", r.stderr)
        m = P.read_json_file(self.path("dump", "manifest.json"))
        self.assertEqual(len(m["graphics"]), 2)
        self.assertFalse(os.path.exists(self.path("dump", ".parts")))
        self.assertEqual([n for n in os.listdir(self.tmp) if n.startswith("dump.parts")], [])
        with open(self.path("args_a_first.txt")) as f:
            args = f.read()
        for want in ("--game cdtv", "--assets /assets dir", "--frames 123", "--headless", "--input-script"):
            self.assertIn(want, args)

    def test_keep_parts_and_refusals(self):
        self.run_script("-k")
        self.assertTrue(os.path.isdir(self.path("dump", ".parts", "a_first")))
        self.run_script(expect=1)  # the folder now exists and is not empty
        r = subprocess.run(["sh", os.path.join(HERE, "dump_all.sh")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        r = subprocess.run(["sh", os.path.join(HERE, "dump_all.sh"), "-o", self.path("d2"), "-g", "amiga"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        r = subprocess.run(["sh", os.path.join(HERE, "dump_all.sh"), "-o", self.path("d3"), "-x", self.path("nope")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)


if __name__ == "__main__":
    unittest.main(verbosity=1)
