#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Build a runnable demo asset pack from YOUR OWN `x2recomp --dump-assets` / `--dump-audio` dump.

    make_demo_pack.py DUMP OUT [--scale N] [--game disk|cdtv|st] [--atlas N] [--no-audio] [--no-music]
                      [--name NAME] [--force] [--no-check]

DUMP is a folder written by `x2recomp --dump-assets DIR` (and `--dump-audio DIR`, into the same folder or another one
given as an extra DUMP; `dump_all.sh` makes a merged one). OUT is the pack folder to create (or a `.zip` file: written
with stored members, the way the loader reads best). The pack is the quickest way to see that a build is pack-ready, and
it exercises every feature of the format:

  * sprites, tiles and flashes: recoloured (colour channels rotated), shaded top to bottom and box-scaled by N; one run of
    sprites goes into an atlas (`--atlas`, with a `groups` entry), two sprites of one size share one picture through `keys`;
  * the far layer: its strips recoloured and scaled, plus a generated seamless `backgrounds` picture for every level the
    dump lists, a `palettes` remap and round `stars`;
  * screens: a generated gradient with a label in place of each original, keeping the signature, `ref_palette`, `fade`,
    `bg_index` and the `dynamic` rectangles (so the game's live text still shows);
  * the status panel: every dumped cell redrawn as a flat two-colour glyph over a generated background strip;
  * sound effects and speech: every dumped clip replaced by a synthesised chirp of the same duration and `ref_period`
    (loops stay seamless); music: a synthesised loop for the slots title, level1-level5 and level, the title with an intro.

It needs your own dump to learn the real keys. What it writes is derived from your copy of the game: use it to test, and
never distribute it (the pack says so). Standard library only; see pack_lib.py.
"""
import argparse
import colorsys
import math
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bitmapfont as F  # noqa: E402
import make_skeleton as SK  # noqa: E402
import pack_atlas as A  # noqa: E402
import pack_lib as P  # noqa: E402

PACK_ID = "demo.generated"
LICENSE = "Not for distribution: made from your own copy of the game"
FLASH_RGB = (255, 240, 120)
PANEL_RGB = (255, 214, 64)
MUSIC_SLOTS = ("title", "level1", "level2", "level3", "level4", "level5", "level")


# ----------------------------------------------------------------------------------------------------------------
# Pictures

def _hsv(h, s, v):
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, max(0.0, min(1.0, s)), max(0.0, min(1.0, v)))
    return int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5)


def recolour(img):
    """Rotate the colour channels (R, G, B) -> (G, B, R) and shade the picture from 100 % at the top to 75 % at the bottom."""
    out = bytearray(img.data)
    h = img.h
    for y in range(h):
        k = 1.0 - 0.25 * (y / max(1, h - 1))
        for x in range(img.w):
            i = (y * img.w + x) * 4
            r, g, b = img.data[i], img.data[i + 1], img.data[i + 2]
            out[i] = int(g * k)
            out[i + 1] = int(b * k)
            out[i + 2] = int(r * k)
    return P.Image(img.w, img.h, out)


def tint_flash(img):
    """A flash silhouette keeps its alpha and takes one bright colour."""
    out = bytearray(img.data)
    for i in range(0, len(out), 4):
        out[i], out[i + 1], out[i + 2] = FLASH_RGB
    return P.Image(img.w, img.h, out)


def flat_cell(img):
    """A panel cell as a flat glyph: pixels at least a third as bright as the cell's brightest become one colour, the rest
    are transparent (the generated strip shows through)."""
    lum = [max(img.data[i], img.data[i + 1], img.data[i + 2]) if img.data[i + 3] else 0 for i in range(0, len(img.data), 4)]
    top = max(lum) or 1
    out = bytearray(len(img.data))
    for n, v in enumerate(lum):
        if v * 3 >= top and v > 0:
            out[n * 4:n * 4 + 4] = bytes(PANEL_RGB + (255,))
    return P.Image(img.w, img.h, out)


def _smooth(t):
    return 0.5 - 0.5 * math.cos(2 * math.pi * t)


def background_picture(width, height, level):
    """A seamless (top to bottom) generated far layer: a slow diagonal colour wave of the level's hue."""
    img = P.Image(width, height)
    hue = 0.55 + 0.11 * (level - 1)
    cols = [math.sin(2 * math.pi * 3 * x / width) for x in range(width)]
    for y in range(height):
        ty = y / height
        row_v = 0.28 + 0.22 * _smooth(ty)
        sy = math.sin(2 * math.pi * 2 * ty)
        for x in range(width):
            w = 0.5 + 0.5 * math.sin(2 * math.pi * (2 * ty + 3 * x / width))
            r, g, b = _hsv(hue + 0.06 * sy * cols[x], 0.55 + 0.2 * w, row_v + 0.2 * w)
            i = (y * width + x) * 4
            img.data[i:i + 4] = bytes((r, g, b, 255))
    return img


def screen_picture(width, height, name, index):
    """A generated screen: a vertical gradient with a diagonal stripe pattern and the screen's name."""
    img = P.Image(width, height)
    hue = (index * 0.137 + 0.08) % 1.0
    stripe = max(4, width // 40)
    for y in range(height):
        v = 0.22 + 0.5 * (y / height)
        for x in range(width):
            band = ((x + y) // stripe) % 2
            r, g, b = _hsv(hue + 0.05 * band, 0.6, v * (0.85 + 0.15 * band))
            i = (y * width + x) * 4
            img.data[i:i + 4] = bytes((r, g, b, 255))
    k = max(1, width // 160)
    label = "DEMO " + name.upper().replace("_", " ")
    tw, th = F.text_size(label, k)
    while tw + 8 > width and k > 1:
        k -= 1
        tw, th = F.text_size(label, k)
    if tw + 8 <= width and th + 8 <= height:
        x0, y0 = (width - tw) // 2, height // 3
        img.fill_rect(x0 - 4, y0 - 4, tw + 8, th + 8, (0, 0, 0, 255))
        F.draw_text(img, x0, y0, label, (255, 255, 255, 255), k)
    return img


def panel_background(width, height):
    img = P.Image(width, height)
    for y in range(height):
        v = 0.12 + 0.18 * (y / max(1, height - 1))
        px = bytes(_hsv(0.62, 0.5, v) + (255,))
        img.data[y * width * 4:(y + 1) * width * 4] = px * width
    return img


def preview_picture(width=160, height=100):
    img = screen_picture(width, height, "demo pack", 3)
    return img


# ----------------------------------------------------------------------------------------------------------------
# Sounds (16-bit mono PCM WAV through make_skeleton's helpers)

def wav_duration(path):
    """Duration in seconds of a PCM WAV from its header (no samples are used)."""
    import wave
    with wave.open(path, "rb") as w:
        return w.getnframes() / float(w.getframerate())


def _key_seed(key):
    return P.fnv1a64(key.encode("ascii")) & 0xFFFFFFFF


def clip_wav(key, seconds, loop, rate):
    """A chirp (or, for a loop, a pure tone with a whole number of cycles) of exactly `seconds` at `rate` Hz."""
    n = max(16, int(round(seconds * rate)))
    seed = _key_seed(key)
    f0 = 300 + seed % 900
    if loop:
        cycles = max(1, int(round(f0 * n / rate)))
        samples = [32767 * 0.4 * math.sin(2 * math.pi * cycles * i / n) for i in range(n)]
    else:
        f1 = f0 * (1.5 + (seed >> 12) % 8 / 8.0)
        phase = 0.0
        samples = []
        for i in range(n):
            phase += 2 * math.pi * (f0 + (f1 - f0) * i / n) / rate
            samples.append(32767 * 0.4 * math.sin(phase) * math.exp(-3.0 * i / n))
    return SK._wav_bytes(samples, rate)


MUSIC_NOTES = {
    "title": ([262, 330, 392, 523], [262, 330, 392, 330, 262, 196]),
    "level1": (None, [196, 247, 294, 392]),
    "level2": (None, [220, 262, 330, 440]),
    "level3": (None, [175, 220, 262, 349]),
    "level4": (None, [247, 294, 370, 494]),
    "level5": (None, [165, 208, 247, 330]),
    "level": (None, [196, 262, 294, 392, 294, 262]),
}


# ----------------------------------------------------------------------------------------------------------------
# The builder

class Pack:
    """The files and the manifest of the pack being built."""

    def __init__(self):
        self.files = {}
        self.manifest = {}

    def png(self, rel, img):
        self.files[rel] = P.encode_png(img)
        return rel

    def wav(self, rel, data):
        self.files[rel] = data
        return rel


def _read(root, rel):
    return P.read_png(P.safe_join(root, rel))


def _scale_of(entry, dump_scale):
    s = entry.get("scale", dump_scale)
    return s if isinstance(s, int) and 1 <= s <= P.MAX_SCALE else dump_scale


def _check_scale(total):
    if not 1 <= total <= P.MAX_SCALE:
        raise P.PackError("the pack scale %d is outside 1-%d (dump scale times --scale)" % (total, P.MAX_SCALE))


def _scoped(e, out):
    for k in ("games", "builds"):
        if k in e:
            out[k] = e[k]
    return out


def build_demo(dumps, scale=2, game=None, atlas=10, audio=True, music=True, name="Demo pack"):
    """Build the demo pack from the dump folders `dumps` (the first one that has an `audio` section, a `graphics` section, ...
    supplies it; merge them with merge_dumps.py for one coherent dump). Returns a `Pack`."""
    if not dumps:
        raise P.PackError("no dump folder given")
    loaded = []
    for d in dumps:
        mpath = os.path.join(d, "manifest.json")
        if not os.path.isfile(mpath):
            raise P.PackError("%s has no manifest.json (not a --dump-assets / --dump-audio folder)" % d)
        m = P.read_json_file(mpath)
        if not isinstance(m, dict) or m.get("format") != "x2-asset-pack":
            raise P.PackError("%s: not an asset pack or dump manifest" % mpath)
        loaded.append((d, m))
    dump_scale = loaded[0][1].get("scale", 1)
    for d, m in loaded:
        if m.get("scale", 1) != dump_scale:
            raise P.PackError("the dumps have different scales (%s and %s)" % (dump_scale, m.get("scale", 1)))
    if not isinstance(dump_scale, int):
        raise P.PackError("the dump's scale is not an integer")
    total = dump_scale * scale
    _check_scale(total)

    def section(key):
        for d, m in loaded:
            if m.get(key):
                return d, m[key]
        return None, None

    pk = Pack()
    out = {"format": "x2-asset-pack", "version": 2, "id": PACK_ID, "pack_version": "1.0.0", "name": name,
           "description": "Generated by make_demo_pack.py from the maker's own dump: recoloured and scaled graphics, generated screens, "
                          "chirps and loops. For testing; not for distribution.",
           "author": "make_demo_pack.py", "license": LICENSE,
           "credits": [{"name": "make_demo_pack.py", "role": "generated pictures and tones"}],
           "requires": {"x2recomp": ">=0.3.0"}, "scale": total}
    if game:
        out["games"] = [game]
    provides = []

    # --- graphics: sprites, tiles, flashes
    gdir, graphics = section("graphics")
    entries = {}
    alias_target = {}
    if graphics:
        sprite_keys = []
        by_size = {}
        for key in sorted(graphics):
            e = graphics[key]
            if not isinstance(e, dict) or not isinstance(e.get("png"), str):
                continue
            k = key[2:] if key.startswith("f:") else key
            if not P.is_gfx_key(k):
                continue
            if key.startswith("f:"):
                entries[key] = ("flash", e)
            else:
                kind = e.get("kind", "sprite")
                entries[key] = (kind, e)
                if kind == "sprite":
                    sprite_keys.append(key)
                    by_size.setdefault(P.parse_gfx_key(key)[1:], []).append(key)
        # one pair of same-sized sprites shares a picture
        for size in sorted(by_size):
            if len(by_size[size]) >= 2:
                a, b = by_size[size][0], by_size[size][1]
                alias_target[b] = a
                break
        in_alias = set(alias_target) | set(alias_target.values())
        atlas_keys = [k for k in sprite_keys if k not in in_alias][:max(0, atlas)]
        atlas_frames = {}
        gout = {}
        for key in sorted(entries):
            kind, e = entries[key]
            if key in alias_target:
                continue
            src = _read(gdir, e["png"])
            s = _scale_of(e, dump_scale)
            f = max(1, total // s)
            if key.startswith("f:"):
                img = tint_flash(src).scaled(f)
            else:
                img = recolour(src.scaled(f))
            if key in atlas_keys:
                atlas_frames[key] = img
                continue
            ext = "flashes" if key.startswith("f:") else ("tiles" if kind == "tile" else "sprites")
            rel = "%s/%s.png" % (ext, key.replace(":", "_"))
            ent = _scoped(e, {"png": pk.png(rel, img)})
            if "anchor" in e:
                ent["anchor"] = e["anchor"]
            for k2, tgt in alias_target.items():
                if tgt == key:
                    ent.setdefault("keys", []).append(k2)
            gout[key] = ent
        if gout:
            out["graphics"] = gout
        if atlas_frames:
            built = A.build_entries(atlas_frames, "sheets/demo.png", scale=total, padding=2, bleed=2)
            atl = []
            for ref, sheet, entry in built:
                pk.png(ref, sheet)
                atl.append(entry)
            out["atlases"] = atl
            out["groups"] = {"demo-atlas": sorted(atlas_frames)}
        if gout or atlas_frames:
            provides.append("graphics")

    # --- far layer strips
    fdir, far = section("far")
    if far:
        fout = []
        for i, e in enumerate(far):
            if not isinstance(e, dict) or not isinstance(e.get("png"), str) or not isinstance(e.get("lines"), list):
                continue
            s = _scale_of(e, dump_scale)
            img = recolour(_read(fdir, e["png"]).scaled(max(1, total // s)))
            fout.append({"png": pk.png("far/strip_%03d.png" % i, img), "scale": total, "lines": e["lines"]})
        if fout:
            out["far"] = fout
            if "graphics" not in provides:
                provides.append("graphics")

    # --- backgrounds, stars, palettes
    bdir, backgrounds = section("backgrounds")
    if backgrounds:
        bout = []
        for e in backgrounds:
            if not isinstance(e, dict) or not isinstance(e.get("level"), int):
                continue
            ring = e.get("ring_lines", 192)
            img = background_picture(320 * total, ring * total, e["level"])
            ent = _scoped(e, {"level": e["level"], "png": pk.png("backgrounds/level%d.png" % e["level"], img),
                              "scale": total, "ring_lines": ring})
            bout.append(ent)
        if bout:
            out["backgrounds"] = bout
            if "graphics" not in provides:
                provides.append("graphics")
    if backgrounds or far:
        out["stars"] = {"shape": "round", "size": 1.5, "softness": 0.25}
        pals = []
        for pal in loaded[0][1].get("seen_palettes") or []:
            if (isinstance(pal, list) and len(pal) == 16 and all(isinstance(c, str) and len(c) == 4 and c[0] == "#" for c in pal)
                    and any(c != "#000" for c in pal)):
                colours = []
                for c in pal:
                    r, g, b = (int(c[i], 16) * 17 for i in (1, 2, 3))
                    colours.append("#%02x%02x%02x" % (g, b, r))
                pals.append({"match": pal, "colors": colours, "layers": ["far", "stars"]})
        if pals:
            out["palettes"] = pals[-2:]

    # --- screens
    sdir, screens = section("screens")
    if screens:
        sout = {}
        for n, name_ in enumerate(sorted(screens)):
            e = screens[name_]
            if not isinstance(e, dict) or not isinstance(e.get("png"), str) or not isinstance(e.get("signature"), dict):
                continue
            ent = {"signature": e["signature"], "png": pk.png("screens/%s.png" % name_, screen_picture(320 * total, 200 * total, name_, n)),
                   "scale": total}
            for k in ("ref_palette", "fade", "bg_index", "dynamic", "anchor"):
                if k in e:
                    ent[k] = e[k]
            sout[name_] = _scoped(e, ent)
        if sout:
            out["screens"] = sout
            provides.append("screens")

    # --- status panel
    pdir, panel = section("panel")
    if isinstance(panel, dict) and isinstance(panel.get("cells"), dict) and panel["cells"]:
        ps = _scale_of(panel, dump_scale)
        pf = max(1, total // ps)
        pout = {"scale": total, "cell_w": panel.get("cell_w", 8), "max_span": panel.get("max_span", 6)}
        for k in ("ref_palette", "fade"):
            if k in panel:
                pout[k] = panel[k]
        pout["background"] = {"png": pk.png("panel/background.png", panel_background(320 * total, 8 * total))}
        cells = {}
        for key in sorted(panel["cells"]):
            e = panel["cells"][key]
            if not isinstance(e, dict) or not isinstance(e.get("png"), str) or not P.CELL_KEY_RE.match(key):
                continue
            img = flat_cell(_read(pdir, e["png"])).scaled(pf)
            cells[key] = {"png": pk.png("panel/%s.png" % key, img)}
        pout["cells"] = cells
        out["panel"] = pout
        if "screens" not in provides:
            provides.append("screens")

    # --- audio
    audio_out = {}
    adir, au = section("audio")
    if audio and isinstance(au, dict):
        for kind, folder, rate in (("sfx", "sfx", 22050), ("speech", "speech", 11025)):
            clips = au.get(kind)
            if not isinstance(clips, dict):
                continue
            cout = {}
            for key in sorted(clips):
                e = clips[key]
                if not isinstance(e, dict) or not isinstance(e.get("file"), str) or not P.CLIP_KEY_RE.match(key):
                    continue
                seconds = wav_duration(P.safe_join(adir, e["file"]))
                loop = bool(e.get("loop", False))
                ent = {"file": pk.wav("%s/%s.wav" % (folder, key), clip_wav(key, seconds, loop, rate)), "original": "mute"}
                if "ref_period" in e:
                    ent["ref_period"] = e["ref_period"]
                if loop:
                    ent["loop"] = True
                if kind == "speech" and isinstance(e.get("cd0_file"), str):
                    ent["cd0_file"] = e["cd0_file"]
                if kind == "sfx":
                    ent["gain_db"] = -3
                cout[key] = _scoped(e, ent)
            if cout:
                audio_out[kind] = cout
    if music and game != "st":
        slots = {}
        for slot in MUSIC_SLOTS:
            intro, body = MUSIC_NOTES[slot]
            files = {}
            if intro:
                files["intro"] = pk.wav("music/%s_intro.wav" % slot, SK.arpeggio_wav(intro, 0.125))
            files["file"] = pk.wav("music/%s.wav" % slot, SK.arpeggio_wav(body, 0.1875))
            ent = dict(files)
            ent["loop"] = True
            ent["gain_db"] = -6
            slots[slot] = ent
        audio_out["music"] = {"defaults": {"crossfade_ms": 250}, "slots": slots}
    if audio_out:
        out["audio"] = audio_out
        provides.append("audio")

    pk.files["preview.png"] = P.encode_png(preview_picture())
    out["preview"] = "preview.png"
    out["provides"] = [p for p in ("graphics", "screens", "audio") if p in provides]
    pk.files["README.txt"] = (
        "Demo pack generated by make_demo_pack.py from the maker's own dump of the game.\n"
        "It contains recoloured and scaled copies of the game's own graphics: for testing only. Do not share it.\n").encode("ascii")
    pk.manifest = out
    return pk


def write_pack(pk, out, force=False):
    """Write the pack to the folder `out`, or to the stored ZIP `out` when it ends in .zip."""
    files = dict(pk.files)
    files["manifest.json"] = P.json_text(pk.manifest).encode("utf-8")
    if out.lower().endswith(".zip"):
        if os.path.exists(out) and not force:
            raise P.PackError("%s exists (use --force)" % out)
        with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
            for rel in sorted(files):
                zi = zipfile.ZipInfo(rel, (2026, 1, 1, 0, 0, 0))
                zi.compress_type = zipfile.ZIP_STORED
                zi.external_attr = 0o644 << 16
                z.writestr(zi, files[rel])
        return
    if os.path.exists(out) and os.listdir(out) and not force:
        raise P.PackError("%s exists and is not empty (use --force)" % out)
    for rel, data in sorted(files.items()):
        path = P.safe_join(out if os.path.isdir(out) else _mkdir(out), rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


def _mkdir(path):
    os.makedirs(path, exist_ok=True)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build a runnable demo asset pack from your own --dump-assets / --dump-audio dump.")
    ap.add_argument("dump", nargs="+", help="dump folder(s); the first provides the graphics, screens and panel, any may provide the audio")
    ap.add_argument("out", help="the pack folder to create, or a .zip file")
    ap.add_argument("--scale", type=int, default=2, help="enlargement of every picture (default 2; dump scale times this must be at most 8)")
    ap.add_argument("--game", choices=("disk", "cdtv", "st"), help="set the pack's `games` (the ST gets no music slots)")
    ap.add_argument("--atlas", type=int, default=10, help="sprites to put into an atlas (default 10, 0 for none)")
    ap.add_argument("--no-audio", action="store_true", help="no sound effects or speech")
    ap.add_argument("--no-music", action="store_true", help="no music slots")
    ap.add_argument("--name", default="Demo pack")
    ap.add_argument("--force", action="store_true", help="write into an existing, non-empty folder or over a .zip")
    ap.add_argument("--no-check", action="store_true", help="skip validating the result with validate.py")
    args = ap.parse_args(argv)
    out = args.out
    try:
        pk = build_demo(args.dump, args.scale, args.game, args.atlas, not args.no_audio, not args.no_music, args.name)
        write_pack(pk, out, args.force)
    except (P.PackError, OSError) as e:
        print("make_demo_pack: %s" % e, file=sys.stderr)
        return 2
    m = pk.manifest
    print("demo pack: %d graphics, %d far strips, %d backgrounds, %d screens, %d panel cells, %d sfx, %d speech, %d music slots, "
          "%d files -> %s" % (len(m.get("graphics", {})) + sum(len(a.get("frames", [])) for a in m.get("atlases", [])),
                              len(m.get("far", [])), len(m.get("backgrounds", [])), len(m.get("screens", {})),
                              len(m.get("panel", {}).get("cells", {})), len(m.get("audio", {}).get("sfx", {})),
                              len(m.get("audio", {}).get("speech", {})), len(m.get("audio", {}).get("music", {}).get("slots", {})),
                              len(pk.files) + 1, out))
    if not args.no_check:
        import validate as V
        rep, code, entries, skipped = V.run_check(out, game=args.game, against=args.dump[0])
        sys.stdout.write(V.format_report(rep, code, entries, skipped, False))
        if code >= 2:
            return code
    return 0


if __name__ == "__main__":
    sys.exit(main())
