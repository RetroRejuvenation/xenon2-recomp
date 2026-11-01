#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Pack a folder of per-key pictures into a sprite sheet (an `atlases` entry of an asset pack manifest).

    pack_atlas.py FRAMES_DIR --out-png sheets/ship.png [--out-json ship.atlas.json] [--scale 4]
                  [--padding 1] [--bleed 1] [--max-size 4096] [--pot] [--width W] [--split]
                  [--layout frames|grid] [--columns N] [--grid-order cells.txt] [--anchors anchors.json]
                  [--pack-root DIR] [--png-ref sheets/ship.png] [--into manifest.json]

FRAMES_DIR holds `<key>.png` files (searched recursively: a dump's sprites/ and tiles/ folders work), where
`<key>` is a graphics key `<16 hex digits>-<w>x<h>`. Each picture should be `w*scale` by `h*scale` pixels (the size the
manifest validator expects); a mismatch is an error unless --allow-size-mismatch.

The packing is deterministic: frames are placed by shelves, tallest first (ties: widest, then key), and the sheet width
is the one giving the smallest sheet. `--layout grid` writes the `grid` form instead (equal-sized frames, row-major,
no padding). `--split` starts another sheet (`name-2.png`, ...) when the limit of --max-size pixels a side is reached.
The inverse is slice.py. Output: the sheet(s) and an `{"atlases": [...]}` fragment (or merged into a manifest with --into).

Standard library only (see pack_lib.py).
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_lib as P  # noqa: E402


def collect_frames(root, scale=None, allow_mismatch=False):
    """{key: Image} for every `<key>.png` under `root` (recursively). Duplicate keys with different pictures are an
    error. With `scale`, pictures must be `w*scale` x `h*scale` unless `allow_mismatch`."""
    frames = {}
    sources = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            stem, ext = os.path.splitext(fn)
            if ext.lower() != ".png" or not P.is_gfx_key(stem):
                continue
            path = os.path.join(dirpath, fn)
            img = P.read_png(path)
            if stem in frames and frames[stem] != img:
                raise P.PackError("%s and %s hold the same key with different pictures" % (sources[stem], path))
            frames[stem] = img
            sources[stem] = path
    if scale is not None and not allow_mismatch:
        bad = []
        for key, img in sorted(frames.items()):
            _, w, h = P.parse_gfx_key(key)
            if (img.w, img.h) != (w * scale, h * scale):
                bad.append("%s is %dx%d, expected %dx%d for scale %d" % (key, img.w, img.h, w * scale, h * scale, scale))
        if bad:
            raise P.PackError("picture size does not match the key and scale (use --allow-size-mismatch to pack anyway):\n  "
                              + "\n  ".join(bad[:20]) + ("\n  ... %d more" % (len(bad) - 20) if len(bad) > 20 else ""))
    return frames


def _order(keys, frames):
    return sorted(keys, key=lambda k: (-frames[k].h, -frames[k].w, k))


def shelf_pack(order, frames, width, max_height, padding):
    """Place the frames of `order` on shelves in a sheet `width` wide. Every frame takes (w + 2 padding) x (h + 2 padding).
    Returns ({key: (x, y)}, used height, [keys that did not fit within max_height])."""
    placed = {}
    left = []
    x = 0
    y = 0
    shelf_h = 0
    for key in order:
        img = frames[key]
        fw, fh = img.w + 2 * padding, img.h + 2 * padding
        if fw > width:
            left.append(key)
            continue
        if x + fw > width:
            y += shelf_h
            x = 0
            shelf_h = 0
        if y + fh > max_height:
            left.append(key)
            continue
        placed[key] = (x + padding, y + padding)
        x += fw
        shelf_h = max(shelf_h, fh)
    used = y + shelf_h if placed else 0
    return placed, used, left


def _round_up(v, step):
    return (v + step - 1) // step * step


def _next_pot(v):
    p = 1
    while p < v:
        p *= 2
    return p


def choose_width(order, frames, max_side, padding, pot, width=None):
    """The sheet width (candidates: powers of two with --pot, else steps of 16 or 64) with the smallest sheet area."""
    min_w = max(frames[k].w for k in order) + 2 * padding
    if width is not None:
        if width < min_w:
            raise P.PackError("--width %d is smaller than the widest frame plus padding (%d)" % (width, min_w))
        return width
    if min_w > max_side:
        raise P.PackError("a frame (%d pixels wide with padding) does not fit a sheet of at most %d pixels" % (min_w, max_side))
    if pot:
        cands = []
        w = _next_pot(min_w)
        while w <= max_side:
            cands.append(w)
            w *= 2
    else:
        step = 16 if max_side <= 1024 else 64
        cands = list(range(_round_up(min_w, 16), max_side + 1, step))
        if not cands or cands[-1] != max_side:
            cands.append(max_side)
        cands = [c for c in cands if c >= min_w]
    if not cands:
        raise P.PackError("no sheet width within %d pixels fits the frames" % max_side)
    best = None
    for w in cands:
        _, used, left = shelf_pack(order, frames, w, 1 << 30, padding)
        hh = _next_pot(used) if pot else used
        area = w * hh
        key = (area, w)
        if best is None or key < best[0]:
            best = (key, w)
    return best[1]


def pack_sheets(frames, max_size=4096, padding=1, pot=False, width=None, split=False):
    """Pack `frames` ({key: Image}) into one or more sheets. Returns a list of (sheet Image, {key: (x, y)}). More than one
    sheet only with `split`; otherwise exceeding `max_size` is an error."""
    if not frames:
        raise P.PackError("no frames to pack")
    remaining = _order(frames.keys(), frames)
    sheets = []
    while remaining:
        w = choose_width(remaining, frames, max_size, padding, pot, width)
        placed, used, left = shelf_pack(remaining, frames, w, max_size, padding)
        if left and not split:
            raise P.PackError("the frames do not fit one %dx%d sheet (%d of %d left over): use --split or a larger --max-size"
                              % (max_size, max_size, len(left), len(frames)))
        if not placed:
            raise P.PackError("a frame is larger than the sheet limit of %d pixels" % max_size)
        sw = max(x + frames[k].w + padding for k, (x, _) in placed.items())
        sh = max(y + frames[k].h + padding for k, (_, y) in placed.items())
        if pot:
            sw, sh = _next_pot(sw), _next_pot(sh)
        sheet = P.Image(sw, sh)
        for k, (x, y) in placed.items():
            sheet.paste(frames[k], x, y)
        sheets.append((sheet, placed))
        remaining = left
    return sheets


def bleed_sheet(sheet, placed, frames, bleed):
    if bleed <= 0:
        return
    for k, (x, y) in placed.items():
        sheet.bleed(x, y, frames[k].w, frames[k].h, bleed)


def grid_sheet(frames, columns=None, max_size=4096, order=None):
    """Equal-sized frames in a row-major grid without padding: (sheet, cell (w, h), columns, ordered keys).
    `order` lists the cells row-major (None or '-' leaves a cell empty); default: the keys sorted."""
    keys = sorted(frames)
    sizes = {(frames[k].w, frames[k].h) for k in keys}
    if len(sizes) != 1:
        raise P.PackError("--layout grid needs frames of one size, found %s" % ", ".join("%dx%d" % s for s in sorted(sizes)))
    if order is not None:
        order = [None if k in (None, "-", "") else k for k in order]
        listed = [k for k in order if k is not None]
        if sorted(listed) != keys:
            raise P.PackError("the grid order must list every frame exactly once (plus '-' for empty cells)")
        cells = order
    else:
        cells = keys
    cw, ch = next(iter(sizes))
    cmax = max_size // cw
    if cmax < 1:
        raise P.PackError("a frame is wider than the sheet limit of %d pixels" % max_size)
    cols = columns or min(cmax, max(1, int(math.ceil(math.sqrt(len(cells))))))
    if cols > cmax:
        raise P.PackError("%d columns of %d pixels exceed the sheet limit of %d" % (cols, cw, max_size))
    rows = (len(cells) + cols - 1) // cols
    if rows * ch > max_size:
        raise P.PackError("the grid would be %d pixels tall: over the limit of %d" % (rows * ch, max_size))
    sheet = P.Image(cols * cw, rows * ch)
    for i, k in enumerate(cells):
        if k is not None:
            sheet.paste(frames[k], (i % cols) * cw, (i // cols) * ch)
    return sheet, (cw, ch), cols, cells


def sheet_names(base_png, count):
    """Sheet file names: the base name, or name-2.png, name-3.png... for further sheets of a split."""
    stem, ext = os.path.splitext(base_png)
    return [base_png] + ["%s-%d%s" % (stem, i, ext) for i in range(2, count + 1)]


def build_entries(frames, png_ref, scale=None, layout="frames", padding=1, bleed=0, max_size=4096, pot=False, width=None,
                  columns=None, split=False, anchors=None, grid_order=None):
    """Pack and return [(png reference, sheet Image, manifest atlas entry)]."""
    anchors = anchors or {}
    if bleed > padding:
        raise P.PackError("--bleed (%d) cannot exceed --padding (%d)" % (bleed, padding))
    out = []
    if layout == "grid":
        sheet, cell, cols, keys = grid_sheet(frames, columns, max_size, grid_order)
        entry = {"png": png_ref}
        if scale is not None:
            entry["scale"] = scale
        entry["grid"] = {"cell": [cell[0], cell[1]], "columns": cols, "keys": keys}
        out.append((png_ref, sheet, entry))
        return out
    sheets = pack_sheets(frames, max_size, padding, pot, width, split)
    names = sheet_names(png_ref, len(sheets))
    for (sheet, placed), name in zip(sheets, names):
        bleed_sheet(sheet, placed, frames, bleed)
        flist = []
        for k in sorted(placed):
            x, y = placed[k]
            f = {"key": k, "rect": [x, y, frames[k].w, frames[k].h]}
            if k in anchors:
                f["anchor"] = list(anchors[k])
            flist.append(f)
        entry = {"png": name}
        if scale is not None:
            entry["scale"] = scale
        entry["frames"] = flist
        out.append((name, sheet, entry))
    return out


def merge_into_manifest(manifest, entries):
    """Add (or replace by `png`) atlas entries; a version 1 manifest becomes version 2."""
    m = dict(manifest)
    if m.get("version", 1) < 2:
        m["version"] = 2
    atlases = list(m.get("atlases", []))
    for e in entries:
        atlases = [a for a in atlases if a.get("png") != e["png"]]
        atlases.append(e)
    m["atlases"] = atlases
    return m


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames_dir", help="folder of <key>.png pictures (searched recursively)")
    ap.add_argument("--out-png", required=True, help="the sheet to write (further sheets of --split get -2, -3... suffixes)")
    ap.add_argument("--out-json", help="write the {\"atlases\": [...]} fragment here (default: print it)")
    ap.add_argument("--into", help="merge the atlas entries into this manifest.json instead of writing a fragment")
    ap.add_argument("--pack-root", help="the pack folder: the png path recorded is relative to it "
                                        "(default: the folder of --into, else of --out-png)")
    ap.add_argument("--png-ref", help="the png path to record (default: --out-png relative to --pack-root)")
    ap.add_argument("--scale", type=int, help="picture scale of the frames (1-8); also checks picture sizes")
    ap.add_argument("--layout", choices=("frames", "grid"), default="frames")
    ap.add_argument("--columns", type=int, help="grid layout: columns (default: about square)")
    ap.add_argument("--grid-order", help="grid layout: text file, one key per cell row-major, '-' for an empty cell")
    ap.add_argument("--padding", type=int, default=1, help="pixels around every frame (default 1)")
    ap.add_argument("--bleed", type=int, default=0, help="repeat each frame's edge pixels into the padding (<= padding)")
    ap.add_argument("--max-size", type=int, default=P.MAX_SIDE, help="largest sheet side (default 4096, the picture limit)")
    ap.add_argument("--pot", action="store_true", help="round the sheet size up to powers of two")
    ap.add_argument("--width", type=int, help="fix the sheet width instead of choosing the smallest sheet")
    ap.add_argument("--split", action="store_true", help="use more sheets when one would exceed --max-size")
    ap.add_argument("--anchors", help="JSON file {key: [x, y]} of frame anchors")
    ap.add_argument("--allow-size-mismatch", action="store_true")
    a = ap.parse_args(argv)
    try:
        if a.scale is not None and not 1 <= a.scale <= P.MAX_SCALE:
            raise P.PackError("--scale must be 1-%d" % P.MAX_SCALE)
        if a.max_size > P.MAX_SIDE:
            raise P.PackError("--max-size above %d exceeds the picture limit" % P.MAX_SIDE)
        frames = collect_frames(a.frames_dir, a.scale, a.allow_size_mismatch)
        anchors = P.read_json_file(a.anchors) if a.anchors else None
        root = a.pack_root or os.path.dirname(os.path.abspath(a.into or a.out_png))
        ref = a.png_ref or os.path.relpath(os.path.abspath(a.out_png), os.path.abspath(root)).replace(os.sep, "/")
        if not P.is_safe_rel_path(ref):
            raise P.PackError("the sheet path %r is not a safe relative path inside the pack (use --pack-root/--png-ref)" % ref)
        order = None
        if a.grid_order:
            with open(a.grid_order, encoding="utf-8") as f:
                order = [ln.strip() for ln in f.read().splitlines()]
        entries = build_entries(frames, ref, a.scale, a.layout, a.padding, a.bleed, a.max_size, a.pot, a.width, a.columns,
                                a.split, anchors, order)
        for name, sheet, _ in entries:
            target = os.path.join(os.path.abspath(root), *name.split("/"))
            P.write_png(target, sheet)
            print("wrote %s (%dx%d, %d frames)" % (target, sheet.w, sheet.h, len(frames)), file=sys.stderr)
        e = [x[2] for x in entries]
        if a.into:
            m = merge_into_manifest(P.read_json_file(a.into), e)
            P.write_json_file(a.into, m)
            print("updated %s" % a.into, file=sys.stderr)
        elif a.out_json:
            P.write_json_file(a.out_json, {"atlases": e})
        else:
            sys.stdout.write(P.json_text({"atlases": e}))
    except P.PackError as e:
        print("pack_atlas: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
