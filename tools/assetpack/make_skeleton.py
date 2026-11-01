#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Generate a valid asset pack skeleton: a version 2 manifest with every section present, and procedurally generated
placeholder pictures and tones (no game content: checkerboards, gradients, sine tones and noise).

    make_skeleton.py OUT_DIR [--scale 2] [--name "My pack"] [--force] [--check]

The placeholder keys (0000000000000001-16x12 and so on) match nothing in any game, so the skeleton loads, validates and
changes nothing on screen; replace the keys with the ones of your own dump (`x2recomp --dump-assets`, tools/assetpack/
dump_all.sh) and the pictures and sounds with your own. A README.md in the folder explains every file. --check validates
the result against docs/schema/x2-asset-pack-v2.schema.json and checks that every referenced file exists with the size the
manifest implies. The output is byte for byte reproducible. A skeleton is committed in examples/asset-pack-skeleton/.
"""
import argparse
import io
import math
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bitmapfont as F  # noqa: E402
import minischema as MS  # noqa: E402
import pack_atlas as A  # noqa: E402
import pack_lib as P  # noqa: E402

SCHEMA_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "schema", "x2-asset-pack-v2.schema.json")

# Placeholder keys (obviously fake: 15 zeros and a counter).
K_SHIP = "0000000000000001-16x12"
K_SHIP_ALT = "0000000000000002-16x12"
K_WALL = "0000000000000003-16x16"
K_WALL_ALT = "0000000000000004-16x16"
K_SHOT = "0000000000000005-8x8"
K_FLASH = "f:0000000000000006-16x12"
K_HIT = "0000000000000024-16x16"
K_ENEMY = ["0000000000000021-16x16", "0000000000000022-16x16", "0000000000000023-16x16"]
K_BOOM = ["0000000000000031-16x16", "0000000000000032-16x16", "0000000000000033-16x16"]
K_CELL1 = "0000000000000041-1"
K_CELL2 = "0000000000000042-2"
K_SFX_FIRE = "0000000000000051-640"
K_SFX_FIRE_ALT = "0000000000000052-640"
K_SFX_BOOM = "0000000000000053-1200"
K_SPEECH = "0000000000000061-9000"

PALETTE = [(0, 0, 0), (34, 34, 68), (102, 68, 34), (204, 102, 34), (68, 68, 102), (102, 102, 136), (136, 136, 170),
           (238, 204, 238), (170, 170, 204), (34, 34, 102), (0, 0, 34), (68, 0, 0), (102, 0, 0), (136, 34, 0),
           (238, 170, 0), (170, 68, 34)]


# ----------------------------------------------------------------------------------------------------------------
# Placeholder pictures

def _hue(seed):
    """A deterministic pair of colours for `seed`."""
    h = (seed * 2654435761) & 0xFFFFFFFF
    r, g, b = 60 + (h & 0x7F), 60 + ((h >> 8) & 0x7F), 60 + ((h >> 16) & 0x7F)
    return (r, g, b, 255), (255 - r // 2, 255 - g // 2, 255 - b // 2, 255)


def checker_picture(w, h, label, seed, alpha=False):
    """A checkerboard with a border and `label`; with `alpha` the corners are transparent (a sprite shape)."""
    a, b = _hue(seed)
    img = P.Image(w, h)
    cell = max(2, min(w, h) // 6)
    for y in range(h):
        for x in range(w):
            img.put(x, y, a if ((x // cell) + (y // cell)) & 1 else b)
    img.fill_rect(0, 0, w, 1, (255, 255, 255, 255))
    img.fill_rect(0, h - 1, w, 1, (255, 255, 255, 255))
    img.fill_rect(0, 0, 1, h, (255, 255, 255, 255))
    img.fill_rect(w - 1, 0, 1, h, (255, 255, 255, 255))
    if label and w >= 14 and h >= 9:
        tw, th = F.text_size(label)
        if tw + 2 <= w and th + 2 <= h:
            img.fill_rect((w - tw) // 2 - 1, (h - th) // 2 - 1, tw + 2, th + 2, (0, 0, 0, 255))
            F.draw_text(img, (w - tw) // 2, (h - th) // 2, label, (255, 255, 255, 255))
    if alpha:
        r = min(w, h) // 4
        for y in range(h):
            for x in range(w):
                cx = min(x, w - 1 - x)
                cy = min(y, h - 1 - y)
                if cx < r and cy < r and (r - cx) ** 2 + (r - cy) ** 2 > r * r:
                    img.put(x, y, (0, 0, 0, 0))
    return img


def band_picture(w, h, label, seed, label_scale=3):
    """A banded vertical gradient with diagonal lines and a big `label`: a stand-in for a screen or a background."""
    a, b = _hue(seed)
    img = P.Image(w, h)
    bands = 16
    for y in range(h):
        t = (y * bands // h) / (bands - 1)
        col = tuple(int(a[i] * (1 - t) + b[i] * t) for i in range(3)) + (255,)
        img.fill_rect(0, y, w, 1, col)
    step = max(8, w // 16)
    for x0 in range(-h, w, step):
        for y in range(h):
            img.put(x0 + y, y, (255, 255, 255, 255))
    if label:
        tw, th = F.text_size(label, label_scale)
        if tw + 8 <= w and th + 8 <= h:
            img.fill_rect((w - tw) // 2 - 4, (h - th) // 2 - 4, tw + 8, th + 8, (0, 0, 0, 255))
            F.draw_text(img, (w - tw) // 2, (h - th) // 2, label, (255, 255, 255, 255), label_scale)
    return img


# ----------------------------------------------------------------------------------------------------------------
# Placeholder sounds (16-bit mono PCM WAV)

def _wav_bytes(samples, rate):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(s)))) for s in samples))
    return buf.getvalue()


def tone_wav(freq, seconds, rate=8000, decay=4.0, amp=0.5):
    n = int(seconds * rate)
    return _wav_bytes([32767 * amp * math.sin(2 * math.pi * freq * i / rate) * math.exp(-decay * i / n) for i in range(n)], rate)


def noise_wav(seconds, rate=11025, decay=5.0, amp=0.5):
    """A burst of deterministic noise (a linear congruential generator) with an exponential decay."""
    n = int(seconds * rate)
    state = 12345
    out = []
    for i in range(n):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(32767 * amp * ((state >> 8) / 0x7FFFFF - 1.0) * math.exp(-decay * i / n))
    return _wav_bytes(out, rate)


def chirp_wav(f0, f1, seconds, rate=8000, amp=0.4):
    n = int(seconds * rate)
    out = []
    phase = 0.0
    for i in range(n):
        f = f0 + (f1 - f0) * i / n
        phase += 2 * math.pi * f / rate
        out.append(32767 * amp * math.sin(phase) * math.sin(math.pi * i / n))
    return _wav_bytes(out, rate)


def arpeggio_wav(notes, note_s, rate=11025, amp=0.35, fade=0.01):
    """A looping arpeggio: each note is a sine with short fades, so the loop point is click-free."""
    out = []
    for f in notes:
        n = int(note_s * rate)
        nf = max(1, int(fade * rate))
        for i in range(n):
            env = min(1.0, i / nf, (n - i) / nf)
            out.append(32767 * amp * env * math.sin(2 * math.pi * f * i / rate))
    return _wav_bytes(out, rate)


# ----------------------------------------------------------------------------------------------------------------
# The skeleton

def build_skeleton(scale=2, name="Skeleton pack"):
    """(manifest dict, {relative path: bytes}) of the skeleton. Pure and deterministic."""
    if not 1 <= scale <= P.MAX_SCALE:
        raise P.PackError("scale must be 1-%d" % P.MAX_SCALE)
    files = {}

    def png(rel, img):
        files[rel] = P.encode_png(img)

    s = scale
    # sprites and tiles
    png("sprites/ship.png", checker_picture(16 * s, 12 * s, "SHIP", 1, alpha=True))
    png("sprites/ship_flash.png", checker_picture(16 * s, 12 * s, "FLASH", 101, alpha=True))
    png("tiles/wall.png", checker_picture(16 * s, 16 * s, "WALL", 3))
    png("sprites/shot.png", checker_picture(8, 8, "", 5, alpha=True))  # a scale 1 picture in a scale-s pack
    png("sprites/flash_shape.png", checker_picture(16 * s, 12 * s, "HIT", 106, alpha=True))
    graphics = {
        K_SHIP: {"png": "sprites/ship.png", "flash_png": "sprites/ship_flash.png", "keys": [K_SHIP_ALT], "games": ["disk", "cdtv"]},
        K_WALL: {"png": "tiles/wall.png", "anchor": [0, 0], "keys": [K_WALL_ALT]},
        K_SHOT: {"png": "sprites/shot.png", "scale": 1, "games": ["disk"], "builds": ["canonical", "power-pack-2234"]},
        K_FLASH: {"png": "sprites/flash_shape.png"},
    }
    # far layer strip (version 1 form): 8 lines
    far_hashes = ["00000000000000%02x" % (0x10 + i) for i in range(7)] + ["-"]
    png("far/level1.png", band_picture(320 * s, len(far_hashes) * s, "", 7, label_scale=1))
    far = [{"png": "far/level1.png", "scale": s, "lines": far_hashes}]
    # palette override
    palettes = [{
        "match": ["#%x%x%x" % (r >> 4, g >> 4, b >> 4) for r, g, b in PALETTE],
        "colors": ["#%02x%02x%02x" % (min(255, r + 17), min(255, g + 17), min(255, b + 34)) for r, g, b in PALETTE],
        "layers": ["far"], "games": ["disk"]}]
    # background (whole far layer of level 1)
    ring = 64
    png("backgrounds/level1.png", band_picture(320 * s, ring * s, "LEVEL 1 BACKGROUND", 9, label_scale=2))
    backgrounds = [{"level": 1, "png": "backgrounds/level1.png", "scale": s, "ring_lines": ring}]
    # atlases: a frames atlas and a grid atlas, packed with pack_atlas (the same code the user runs)
    enemy_frames = {k: checker_picture(16 * s, 16 * s, "E%d" % (i + 1), 21 + i, alpha=True) for i, k in enumerate(K_ENEMY)}
    ent = A.build_entries(enemy_frames, "sheets/enemies.png", s, "frames", padding=1, bleed=1, max_size=256)
    files["sheets/enemies.png"] = P.encode_png(ent[0][1])
    boom_frames = {k: checker_picture(16 * s, 16 * s, "B%d" % (i + 1), 31 + i, alpha=True) for i, k in enumerate(K_BOOM)}
    order = [K_BOOM[0], K_BOOM[1], None, K_BOOM[2]]
    gent = A.build_entries(boom_frames, "sheets/explosion.png", s, "grid", columns=2, grid_order=order, max_size=256)
    files["sheets/explosion.png"] = P.encode_png(gent[0][1])
    # a frame with its own anchor and a hit-flash picture in the same sheet (`flash_rect`)
    hit = checker_picture(16 * s, 16 * s, "HIT", 24, alpha=True)
    hit_flash = checker_picture(16 * s, 16 * s, "FLASH", 124, alpha=True)
    sheet = P.Image(2 * 16 * s + 6, 16 * s + 4)
    sheet.paste(hit, 2, 2)
    sheet.paste(hit_flash, 16 * s + 4, 2)
    png("sheets/hits.png", sheet)
    hits = {"png": "sheets/hits.png", "scale": s,
            "frames": [{"key": K_HIT, "rect": [2, 2, 16 * s, 16 * s], "anchor": [0, 0], "flash_rect": [16 * s + 4, 2, 16 * s, 16 * s]}]}
    atlases = [ent[0][2], gent[0][2], hits]
    groups = {"enemies": list(K_ENEMY) + [K_HIT], "explosion": list(K_BOOM)}
    stars = {"shape": "round", "size": 1.5, "softness": 0.25}
    # screens
    png("screens/title.png", band_picture(320 * s, 200 * s, "TITLE SCREEN", 11, label_scale=3))
    png("screens/game_over.png", band_picture(320 * s, 200 * s, "GAME OVER", 12, label_scale=3))
    png("screens/title_wide.png", band_picture(480 * s, 200 * s, "TITLE WIDE", 14, label_scale=3))
    screens = {
        "title": {
            "signature": {"block": 16, "min": 3, "blocks": [
                {"x": 0, "y": 0, "hash": "0000000000000071"}, {"x": 16, "y": 0, "hash": "0000000000000072"},
                {"x": 32, "y": 16, "hash": "0000000000000073"}, {"x": 48, "y": 32, "hash": "0000000000000074"}]},
            "png": "screens/title.png", "ref_palette": ["#000", "#fff", "#f80", "#08f"], "fade": "scale", "bg_index": 0,
            "dynamic": [{"x": 8, "y": 180, "w": 120, "h": 12}], "wide_png": "screens/title_wide.png", "wide_scale": s,
            "games": ["disk", "cdtv", "st"]},
        "game_over": {
            "signature": {"block": 16, "min": 2, "blocks": [
                {"x": 96, "y": 80, "hash": "0000000000000081"}, {"x": 112, "y": 80, "hash": "0000000000000082"}]},
            "png": "screens/game_over.png", "anchor": [0, 0]},
    }
    # status panel
    png("panel/background.png", band_picture(320 * s, 8 * s, "", 13, label_scale=1))
    png("panel/digit0.png", checker_picture(8 * s, 8 * s, "", 41))
    png("panel/lives.png", checker_picture(16 * s, 8 * s, "", 42))
    panel = {"scale": s, "cell_w": 8, "max_span": 6, "ref_palette": ["#000", "#222", "#642", "#c62"], "fade": "scale",
             "background": {"png": "panel/background.png", "anchor": [0, 0]},
             "cells": {K_CELL1: {"png": "panel/digit0.png"}, K_CELL2: {"png": "panel/lives.png", "anchor": [0, -2]}}}
    # audio
    files["sfx/fire.wav"] = tone_wav(880, 0.25, 8000, decay=5.0)
    files["sfx/explosion.wav"] = noise_wav(0.3, 11025, decay=6.0)
    files["speech/shop_welcome.wav"] = chirp_wav(300, 900, 0.5)
    files["music/title_intro.wav"] = arpeggio_wav([262, 330, 392, 523], 0.125)
    files["music/title_loop.wav"] = arpeggio_wav([262, 330, 392, 330, 262, 196], 0.125)
    files["music/level1.wav"] = arpeggio_wav([196, 247, 294, 392], 0.1875)
    audio = {
        "sfx": {
            K_SFX_FIRE: {"file": "sfx/fire.wav", "ref_period": 214, "gain_db": -3, "pan": "channel", "pitch": "follow",
                         "loop": False, "cut": "channel", "fade_out_ms": 5, "original": "mute", "keys": [K_SFX_FIRE_ALT]},
            K_SFX_BOOM: {"file": "sfx/explosion.wav", "ref_period": 300, "gain_db": 0, "pan": 0.25, "pitch": "fixed",
                         "original": "mix:-12", "games": ["disk"]},
        },
        "speech": {
            K_SPEECH: {"file": "speech/shop_welcome.wav", "cd0_file": "SPEECH01", "original": "mute", "games": ["cdtv"]},
        },
        "music": {
            "defaults": {"gain_db": 0, "crossfade_ms": 250, "fade_in_ms": 100, "fade_out_ms": 300, "retrigger": "restart", "loop_mode": "pack"},
            "slots": {
                "title": {"intro": "music/title_intro.wav", "file": "music/title_loop.wav", "loop": True, "gain_db": -2},
                "level1": {"file": "music/level1.wav", "loop": {"start_s": 0.1875, "end_s": 0.75}, "fade_in_ms": 200},
                "level3": {"file": None, "mute": True},
                "level": {"file": "music/level1.wav", "loop": True, "retrigger": "continue", "loop_mode": "game"},
                "cd5": {"file": "music/level1.wav", "gain_db": -6, "games": ["cdtv"]},
            },
        },
    }
    png("preview.png", band_picture(160, 100, "PREVIEW", 15, label_scale=2))
    manifest = {
        "format": "x2-asset-pack",
        "version": 2,
        "id": "example.skeleton",
        "pack_version": "1.0.0",
        "name": name,
        "description": "A skeleton: every section of the format with placeholder keys and generated pictures and tones. "
                       "It changes nothing in any game; see README.md.",
        "author": "the xenon2-recomp authors",
        "url": "https://github.com/RetroRejuvenation/xenon2-recomp",
        "license": "CC0-1.0",
        "credits": [{"name": "make_skeleton.py", "role": "generated placeholders", "url": "https://github.com/RetroRejuvenation/xenon2-recomp"}],
        "preview": "preview.png",
        "provides": ["graphics", "screens", "audio"],
        "games": ["disk", "cdtv", "st"],
        "requires": {"x2recomp": ">=0.3.0"},
        "scale": s,
        "graphics": graphics,
        "far": far,
        "palettes": palettes,
        "backgrounds": backgrounds,
        "atlases": atlases,
        "groups": groups,
        "stars": stars,
        "screens": screens,
        "panel": panel,
        "audio": audio,
    }
    files["README.md"] = README.encode("utf-8")
    return manifest, files


README = """# Asset pack skeleton

Generated by `tools/assetpack/make_skeleton.py`. It is a complete, valid pack with **placeholder keys**: nothing in it
matches a real graphic or sound of any game, so loading it changes nothing. It shows the shape of every section of the
format (`docs/ASSET-PACKS.md`, `docs/schema/x2-asset-pack-v2.schema.json`).

How to turn it into your pack:

1. Make a dump of your own copy of the game: `x2recomp --dump-assets DIR ...` (all the replays of a build in one go:
   `tools/assetpack/dump_all.sh`). The dump's `manifest.json` lists the real keys.
2. Replace the placeholder keys below with the real keys of the graphics you redraw, and the pictures with your own.
3. Delete the sections you do not use. `version` must stay 2 when a v2 section is present.
4. Check it: `x2recomp --check-asset-pack PACK_DIR` (or `python3 tools/assetpack/validate.py PACK_DIR`) and
   `--asset-pack PACK_DIR --modern interp,scale=4`. The skeleton itself reports two warnings (its screens have fewer than 6
   signature blocks) and nothing else.

Files:

| Path | What it is |
|---|---|
| `manifest.json` | the manifest; every section, with notes on what each field does in `docs/ASSET-PACKS.md` |
| `sprites/`, `tiles/` | sprite and tile pictures (`graphics`): `w*scale` x `h*scale` pixels; `ship_flash.png` is the hit-flash picture |
| `far/level1.png` | a far-layer strip identified by line hashes (the version 1 form) |
| `backgrounds/level1.png` | a whole level background, `320*scale` x `ring_lines*scale` |
| `sheets/enemies.png`, `sheets/explosion.png` | sprite sheets: a frame list and a grid (empty cell = `null`); made with `pack_atlas.py` |
| `sheets/hits.png` | a sheet with a frame, its own anchor and its hit-flash picture (`flash_rect`) |
| `sprites/flash_shape.png` | the picture of an explicit `f:` hit-flash key |
| `screens/*.png` | whole-screen pictures for the title and game-over screens: `320*scale` x `200*scale`; `title_wide.png` fills the widescreen surround |
| `panel/*.png` | status panel background and cells (`8n*scale` x `8*scale`) |
| `sfx/`, `speech/`, `music/` | placeholder tones (16-bit mono WAV): effect, shop speech, intro and loop music |
| `preview.png` | a picture for pack lists |

The pictures are checkerboards and gradients labelled with what they stand for, drawn by a built-in 5x7 font; the tones
are sine, noise and chirp bursts. The pack is licensed CC0-1.0: do anything with it. Only ever distribute artwork and
audio you made yourself (or that is licensed to you for this), never anything taken from a game or from a dump.
"""


def write_skeleton(out, scale=2, name="Skeleton pack", force=False):
    """Write the skeleton into `out` (created; must be empty unless `force`). Returns the manifest."""
    if os.path.exists(out) and os.listdir(out) and not force:
        raise P.PackError("%s exists and is not empty (use --force)" % out)
    manifest, files = build_skeleton(scale, name)
    os.makedirs(out, exist_ok=True)
    for rel, data in files.items():
        dst = P.safe_join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
    P.write_json_file(os.path.join(out, "manifest.json"), manifest)
    return manifest


# ----------------------------------------------------------------------------------------------------------------
# A private consistency check (the full validator is `x2recomp --check-asset-pack`)

def check_pack(root, schema=None):
    """Schema errors plus missing files and wrong picture sizes of the pack in `root`; [] means consistent."""
    errs = []
    m = P.read_json_file(os.path.join(root, "manifest.json"))
    if schema is None:
        schema = P.read_json_file(SCHEMA_PATH)
    errs += MS.validate(m, schema)
    scale = m.get("scale", 1)
    cache = {}

    def pic(rel, what):
        if rel in cache:
            return cache[rel]
        try:
            cache[rel] = P.read_png(P.safe_join(root, rel), P.MAX_SIDE_BACKGROUND, P.MAX_PIXELS_BACKGROUND)
        except (P.PackError, OSError) as e:
            errs.append("%s: %s" % (what, e))
            cache[rel] = None
        return cache[rel]

    def size(what, img, w, h):
        if img is not None and (img.w, img.h) != (w, h):
            errs.append("%s: picture is %dx%d, expected %dx%d" % (what, img.w, img.h, w, h))

    for key, e in m.get("graphics", {}).items():
        _, w, h = P.parse_gfx_key(key[2:] if key.startswith("f:") else key)
        sc = e.get("scale", scale)
        for field in ("png", "flash_png"):
            if field in e:
                size("graphics %s %s" % (key, field), pic(e[field], key), w * sc, h * sc)
    for i, e in enumerate(m.get("far", [])):
        size("far[%d]" % i, pic(e["png"], "far[%d]" % i), 320 * e.get("scale", scale), len(e["lines"]) * e.get("scale", scale))
    for i, e in enumerate(m.get("backgrounds", [])):
        sc = e.get("scale", scale)
        if "ring_lines" in e:
            size("backgrounds[%d]" % i, pic(e["png"], "backgrounds[%d]" % i), 320 * sc, e["ring_lines"] * sc)
    for i, e in enumerate(m.get("atlases", [])):
        sheet = pic(e["png"], "atlases[%d]" % i)
        sc = e.get("scale", scale)
        if sheet is None:
            continue
        for f in e.get("frames", []):
            x, y, w, h = f["rect"]
            _, kw, kh = P.parse_gfx_key(f["key"])
            if x + w > sheet.w or y + h > sheet.h:
                errs.append("atlases[%d] %s: rect outside the sheet" % (i, f["key"]))
            if (w, h) != (kw * sc, kh * sc):
                errs.append("atlases[%d] %s: rect is %dx%d, expected %dx%d" % (i, f["key"], w, h, kw * sc, kh * sc))
            if "flash_rect" in f:
                fx, fy, fw, fh = f["flash_rect"]
                if fx + fw > sheet.w or fy + fh > sheet.h:
                    errs.append("atlases[%d] %s: flash_rect outside the sheet" % (i, f["key"]))
                if (fw, fh) != (kw * sc, kh * sc):
                    errs.append("atlases[%d] %s: flash_rect is %dx%d, expected %dx%d" % (i, f["key"], fw, fh, kw * sc, kh * sc))
        g = e.get("grid")
        if g:
            cw, ch = g["cell"]
            cols = g.get("columns") or sheet.w // cw
            rows = (len(g["keys"]) + cols - 1) // cols
            if cols * cw > sheet.w or rows * ch > sheet.h:
                errs.append("atlases[%d]: grid does not fit the sheet" % i)
            for k in g["keys"]:
                if k is not None and (cw, ch) != tuple(v * sc for v in P.parse_gfx_key(k)[1:]):
                    errs.append("atlases[%d] %s: grid cell %dx%d does not match the key and scale" % (i, k, cw, ch))
    for name, e in m.get("screens", {}).items():
        sc = e.get("scale", scale)
        size("screens %s" % name, pic(e["png"], "screens %s" % name), 320 * sc, 200 * sc)
    pn = m.get("panel")
    if pn:
        sc = pn.get("scale", scale)
        if "background" in pn:
            size("panel background", pic(pn["background"]["png"], "panel background"), 320 * sc, 8 * sc)
        for key, e in pn.get("cells", {}).items():
            _, span = P.parse_cell_key(key)
            if "anchor" not in e:
                size("panel cell %s" % key, pic(e["png"], "panel cell " + key), 8 * span * sc, 8 * sc)
            else:
                pic(e["png"], "panel cell " + key)
    if "preview" in m:
        pic(m["preview"], "preview")
    au = m.get("audio", {})

    def audio(rel, what):
        try:
            with wave.open(P.safe_join(root, rel), "rb") as w:
                if w.getnchannels() not in (1, 2) or not 8000 <= w.getframerate() <= 192000:
                    errs.append("%s: unsupported WAV format" % what)
        except (P.PackError, OSError, wave.Error, EOFError) as e:
            errs.append("%s: %s" % (what, e))

    for kind in ("sfx", "speech"):
        for key, e in au.get(kind, {}).items():
            audio(e["file"], "%s %s" % (kind, key))
    for slot, e in au.get("music", {}).get("slots", {}).items():
        for field in ("file", "intro"):
            if e.get(field):
                audio(e[field], "music %s %s" % (slot, field))
    return errs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="folder to create")
    ap.add_argument("--scale", type=int, default=2, help="picture scale of the skeleton (1-8, default 2)")
    ap.add_argument("--name", default="Skeleton pack")
    ap.add_argument("--force", action="store_true", help="write into a folder that is not empty")
    ap.add_argument("--check", action="store_true", help="validate the result against the schema and the files")
    a = ap.parse_args(argv)
    try:
        write_skeleton(a.out, a.scale, a.name, a.force)
        print("make_skeleton: wrote the skeleton to %s" % a.out, file=sys.stderr)
        if a.check:
            errs = check_pack(a.out)
            for e in errs:
                print("make_skeleton: CHECK: %s" % e, file=sys.stderr)
            if errs:
                return 1
            print("make_skeleton: the skeleton is consistent", file=sys.stderr)
    except P.PackError as e:
        print("make_skeleton: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
