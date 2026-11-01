#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Cut a sprite sheet into per-key pictures (the inverse of pack_atlas.py).

    slice.py SHEET.png --frames frames.json --out DIR          frames.json: a list of {"key", "rect": [x, y, w, h]},
                                                                or an atlas entry (with "frames" or "grid")
    slice.py SHEET.png --grid 64,48 [--columns 8] --keys keys.txt --out DIR
                                                                cells row-major; keys.txt: one key per line,
                                                                '-' (or an empty line) skips a cell
    slice.py SHEET.png --grid 64,48 --unkeyed --out DIR         name the cells cell_RRR_CCC.png, to be renamed to keys later
    slice.py --manifest manifest.json [--atlas N | --all] --out DIR
                                                                the atlases of a manifest (png paths are relative to it)

Writes `DIR/<key>.png` for every frame. Rectangles must lie inside the sheet. Standard library only (see pack_lib.py).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_lib as P  # noqa: E402


def slice_entry(sheet, entry):
    """[(key, Image)] for an atlas entry (`frames` list and/or `grid`); PackError for a rectangle outside the sheet."""
    out = []
    for f in entry.get("frames", []):
        x, y, w, h = f["rect"]
        P.parse_gfx_key(f["key"])
        out.append((f["key"], sheet.crop(x, y, w, h)))
    g = entry.get("grid")
    if g:
        cw, ch = g["cell"]
        cols = g.get("columns") or max(1, sheet.w // cw)
        for i, key in enumerate(g["keys"]):
            if key is None:
                continue
            P.parse_gfx_key(key)
            x, y = (i % cols) * cw, (i // cols) * ch
            out.append((key, sheet.crop(x, y, cw, ch)))
    if not out:
        raise P.PackError("the atlas entry has neither frames nor a grid")
    return out


def slice_grid(sheet, cell, columns=None, keys=None):
    """Cut the whole sheet into cells; with `keys` (a list, None/'-' = skip) name them, else cell_RRR_CCC. Returns
    [(name, Image)] where names are keys or `cell_RRR_CCC`."""
    cw, ch = cell
    if cw < 1 or ch < 1:
        raise P.PackError("bad cell size %dx%d" % (cw, ch))
    cols = columns or sheet.w // cw
    if cols < 1 or cols * cw > sheet.w:
        raise P.PackError("%d columns of %d pixels do not fit the %d pixel wide sheet" % (cols, cw, sheet.w))
    rows = sheet.h // ch
    out = []
    n = 0
    for r in range(rows):
        for c in range(cols):
            if keys is not None:
                if n >= len(keys):
                    return out
                key = keys[n]
                n += 1
                if key in (None, "", "-"):
                    continue
                P.parse_gfx_key(key)
                out.append((key, sheet.crop(c * cw, r * ch, cw, ch)))
            else:
                out.append(("cell_%03d_%03d" % (r, c), sheet.crop(c * cw, r * ch, cw, ch)))
    if keys is not None and len(keys) > rows * cols:
        raise P.PackError("%d keys given for a grid of only %d cells" % (len(keys), rows * cols))
    return out


def read_keys_file(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [ln.strip() for ln in f.read().splitlines()]
    except OSError as e:
        raise P.PackError("cannot read %s: %s" % (path, e))


def write_frames(frames, outdir):
    os.makedirs(outdir, exist_ok=True)
    for name, img in frames:
        P.write_png(os.path.join(outdir, name + ".png"), img)
    return len(frames)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sheet", nargs="?", help="the sheet picture (not with --manifest)")
    ap.add_argument("--out", required=True, help="folder for the per-key pictures")
    ap.add_argument("--frames", help="JSON: a frame list or an atlas entry")
    ap.add_argument("--grid", help="CELL_W,CELL_H: cut the whole sheet into equal cells")
    ap.add_argument("--columns", type=int)
    ap.add_argument("--keys", help="text file with one key per grid cell ('-' skips a cell)")
    ap.add_argument("--unkeyed", action="store_true", help="grid cells named cell_RRR_CCC.png")
    ap.add_argument("--manifest", help="slice the atlases of this manifest.json")
    ap.add_argument("--atlas", type=int, help="with --manifest: only this atlas (0-based)")
    ap.add_argument("--all", action="store_true", help="with --manifest: every atlas")
    a = ap.parse_args(argv)
    try:
        total = 0
        if a.manifest:
            if a.sheet or a.frames or a.grid:
                raise P.PackError("--manifest cannot be combined with a sheet, --frames or --grid")
            m = P.read_json_file(a.manifest)
            atlases = m.get("atlases", [])
            if not atlases:
                raise P.PackError("%s has no atlases" % a.manifest)
            if a.atlas is not None:
                if not 0 <= a.atlas < len(atlases):
                    raise P.PackError("--atlas %d: the manifest has %d atlases" % (a.atlas, len(atlases)))
                chosen = [atlases[a.atlas]]
            elif a.all or len(atlases) == 1:
                chosen = atlases
            else:
                raise P.PackError("the manifest has %d atlases: pick one with --atlas N or use --all" % len(atlases))
            root = os.path.dirname(os.path.abspath(a.manifest))
            for e in chosen:
                sheet = P.read_png(P.safe_join(root, e["png"]), P.MAX_SIDE_BACKGROUND, P.MAX_PIXELS_BACKGROUND)
                total += write_frames(slice_entry(sheet, e), a.out)
        else:
            if not a.sheet:
                raise P.PackError("give a sheet picture or --manifest")
            sheet = P.read_png(a.sheet, P.MAX_SIDE_BACKGROUND, P.MAX_PIXELS_BACKGROUND)
            if a.frames:
                data = P.read_json_file(a.frames)
                entry = {"frames": data} if isinstance(data, list) else data
                total = write_frames(slice_entry(sheet, entry), a.out)
            elif a.grid:
                try:
                    cw, ch = [int(v) for v in a.grid.split(",")]
                except ValueError:
                    raise P.PackError("--grid needs CELL_W,CELL_H")
                if not a.unkeyed and not a.keys:
                    raise P.PackError("a grid needs --keys FILE (or --unkeyed to name the cells by position)")
                keys = read_keys_file(a.keys) if a.keys else None
                total = write_frames(slice_grid(sheet, (cw, ch), a.columns, keys), a.out)
            else:
                raise P.PackError("give --frames or --grid")
        print("slice: wrote %d pictures to %s" % (total, a.out), file=sys.stderr)
    except P.PackError as e:
        print("slice: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
