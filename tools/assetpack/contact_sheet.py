#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Render a labelled contact sheet of the graphics of a dump or an asset pack, so an artist can see what is what.

    contact_sheet.py PACK_OR_DUMP_DIR --out sheet.png [--scale 3] [--columns N] [--cell 128]
                     [--kind sprite|tile|all] [--group NAME] [--sort size|key] [--max-height 4096]

PACK_OR_DUMP_DIR is a folder with a manifest.json (a `--dump-assets` folder or a pack: `graphics` entries and the frames of
`atlases` are shown; entries of other pictures are not) or a plain folder of `<key>.png` files. Each picture is drawn on a
checkerboard (so transparency shows) enlarged by --scale and labelled with the first eight digits of its key and the
original size (WxH). Pictures larger than --cell pixels are shrunk by an integer step to fit; the sheet is split into
pages (`sheet-2.png`, ...) if it would be taller than --max-height. Only the standard library is used.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bitmapfont as F  # noqa: E402
import pack_lib as P  # noqa: E402
import slice as S  # noqa: E402

BG_A = (60, 60, 68, 255)
BG_B = (84, 84, 92, 255)
SHEET_BG = (32, 32, 38, 255)
LABEL = (225, 225, 120, 255)
CELL_FRAME = (110, 110, 130, 255)


def load_items(root, kind="all", group=None):
    """[(key, Image, kind label)] from a manifest folder or a folder of key-named PNGs."""
    items = {}
    mpath = os.path.join(root, "manifest.json")
    if os.path.isfile(mpath):
        m = P.read_json_file(mpath)
        for key, e in sorted(m.get("graphics", {}).items()):
            if not P.is_gfx_key(key) or not isinstance(e, dict) or "png" not in e:
                continue
            k = e.get("kind", "sprite")
            if kind != "all" and k != kind:
                continue
            try:
                items[key] = (P.read_png(P.safe_join(root, e["png"])), k)
            except (P.PackError, OSError) as err:
                print("contact_sheet: skipped %s: %s" % (key, err), file=sys.stderr)
        for e in m.get("atlases", []):
            if kind not in ("all", "sprite"):
                continue
            try:
                sheet = P.read_png(P.safe_join(root, e["png"]), P.MAX_SIDE_BACKGROUND, P.MAX_PIXELS_BACKGROUND)
                for key, img in S.slice_entry(sheet, e):
                    items.setdefault(key, (img, "sprite"))
            except (P.PackError, OSError, KeyError, ValueError) as err:
                print("contact_sheet: skipped atlas %s: %s" % (e.get("png"), err), file=sys.stderr)
        if group:
            names = m.get("groups", {}).get(group)
            if names is None:
                raise P.PackError("the manifest has no group %r" % group)
            items = {k: v for k, v in items.items() if k in set(names)}
            order = {k: i for i, k in enumerate(names)}
            return [(k, items[k][0], items[k][1]) for k in sorted(items, key=lambda k: order[k])]
    else:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for fn in sorted(filenames):
                stem, ext = os.path.splitext(fn)
                if ext.lower() == ".png" and P.is_gfx_key(stem):
                    items[stem] = (P.read_png(os.path.join(dirpath, fn)), "sprite")
        if group:
            raise P.PackError("--group needs a manifest.json with groups")
    return [(k, v[0], v[1]) for k, v in sorted(items.items())]


def _shrink_factor(img, cell):
    return max(1, -(-max(img.w, img.h) // cell))


def _shrunk(img, k):
    if k == 1:
        return img
    w, h = max(1, img.w // k), max(1, img.h // k)
    out = P.Image(w, h)
    for y in range(h):
        for x in range(w):
            out.put(x, y, img.get(x * k, y * k))
    return out


def _blend(dst, src, ox, oy):
    for y in range(src.h):
        for x in range(src.w):
            r, g, b, a = src.get(x, y)
            if a == 0:
                continue
            if a == 255:
                dst.put(ox + x, oy + y, (r, g, b, 255))
            else:
                dr, dg, db, _ = dst.get(ox + x, oy + y)
                dst.put(ox + x, oy + y, ((r * a + dr * (255 - a)) // 255, (g * a + dg * (255 - a)) // 255,
                                         (b * a + db * (255 - a)) // 255, 255))


def render_cell(key, img, scale, cell, label_scale=1):
    """One labelled cell: checkerboard, the picture enlarged by `scale` (shrunk to fit `cell`), two label lines."""
    big = img.scaled(scale)
    k = _shrink_factor(big, cell)
    big = _shrunk(big, k)
    h16, w, h = P.parse_gfx_key(key)
    label1 = h16[:8]
    label2 = "%dx%d" % (w, h) + ("" if k == 1 else " /%d" % k)
    lw = max(F.text_size(label1, label_scale)[0], F.text_size(label2, label_scale)[0])
    cw = max(cell, lw) + 4
    lh = F.GLYPH_H * label_scale
    ch = cell + 4 + 2 * lh + 6
    out = P.Image(cw, ch, bytes(SHEET_BG) * (cw * ch))
    out.fill_rect(0, 0, cw, cell + 4, CELL_FRAME)
    out.fill_rect(1, 1, cw - 2, cell + 2, BG_A)
    sq = 4
    for y in range(cell + 2):
        for x in range(cw - 2):
            if ((x // sq) + (y // sq)) & 1:
                out.put(1 + x, 1 + y, BG_B)
    _blend(out, big, 2 + (cw - 4 - big.w) // 2, 2 + (cell - big.h) // 2)
    F.draw_text(out, 2, cell + 6, label1, LABEL, label_scale)
    F.draw_text(out, 2, cell + 6 + lh + 2, label2, LABEL, label_scale)
    return out


def layout_pages(cells, columns, max_height, gap=4):
    """Paginate rendered cells: [(page Image)]; all cells share one size per page row (rows are as tall as their tallest)."""
    cw = max(c.w for c in cells)
    ch = max(c.h for c in cells)
    rows_per_page = max(1, (max_height - gap) // (ch + gap))
    per_page = columns * rows_per_page
    pages = []
    for p in range(0, len(cells), per_page):
        chunk = cells[p:p + per_page]
        rows = (len(chunk) + columns - 1) // columns
        page = P.Image(columns * (cw + gap) + gap, rows * (ch + gap) + gap, bytes(SHEET_BG) * ((columns * (cw + gap) + gap) * (rows * (ch + gap) + gap)))
        for i, c in enumerate(chunk):
            page.paste(c, gap + (i % columns) * (cw + gap), gap + (i // columns) * (ch + gap))
        pages.append(page)
    return pages


def build_pages(items, scale=3, columns=None, cell=128, sort="size", max_height=4096):
    if not items:
        raise P.PackError("no graphics found")
    if sort == "size":
        items = sorted(items, key=lambda t: (t[1].w * t[1].h, t[0]))
    cells = [render_cell(k, img, scale, cell) for k, img, _ in items]
    cw = max(c.w for c in cells)
    if columns is None:
        columns = max(1, min(len(cells), 1920 // (cw + 4)))
    return layout_pages(cells, columns, max_height)


def page_names(out, n):
    stem, ext = os.path.splitext(out)
    return [out] + ["%s-%d%s" % (stem, i, ext) for i in range(2, n + 1)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", type=int, default=3, help="enlargement of every picture (1-8, default 3)")
    ap.add_argument("--columns", type=int)
    ap.add_argument("--cell", type=int, default=128, help="largest picture size drawn, pixels (default 128)")
    ap.add_argument("--kind", choices=("all", "sprite", "tile"), default="all")
    ap.add_argument("--group", help="only the keys of this group of the manifest, in its order")
    ap.add_argument("--sort", choices=("size", "key"), default="size")
    ap.add_argument("--max-height", type=int, default=4096)
    a = ap.parse_args(argv)
    try:
        if not 1 <= a.scale <= P.MAX_SCALE:
            raise P.PackError("--scale must be 1-%d" % P.MAX_SCALE)
        items = load_items(a.folder, a.kind, a.group)
        pages = build_pages(items, a.scale, a.columns, a.cell, a.sort, a.max_height)
        for name, page in zip(page_names(a.out, len(pages)), pages):
            P.write_png(name, page)
            print("contact_sheet: wrote %s (%dx%d)" % (name, page.w, page.h), file=sys.stderr)
    except P.PackError as e:
        print("contact_sheet: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
