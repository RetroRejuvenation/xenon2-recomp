#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Merge several `x2recomp --dump-assets` folders (one per replay or play session) into one dump.

    merge_dumps.py --out MERGED PART_DIR [PART_DIR ...]

Used by dump_all.sh. What is merged (the version 1 dump layout; folders and manifest sections it does not know are copied
or united without interpretation, first part wins):

  * sprites/ and tiles/ (manifest `graphics`): the union by key. A key dumped twice keeps the copy drawn with the brighter
    palette (a dump holds the brightest palette it saw, not a fade-in), its `uses` are added, and its `palette` index is
    remapped into the merged `seen_palettes`.
  * far/ (manifest `far`): the far layer's lines are identified by their hash, so the strips are rebuilt: every distinct line
    once, in order of first appearance, 192 lines to a strip (far/strip_NNN.png). All parts must use the same strip scale.
  * seen_palettes: the union, in order of first appearance.

The dump is the author's own data (a dump of the user's copy): never distribute it.
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_lib as P  # noqa: E402

STRIP_LINES = 192
KNOWN_SECTIONS = {"format", "version", "name", "note", "scale", "graphics", "far", "seen_palettes", "palettes"}


def _load_part(part):
    mpath = os.path.join(part, "manifest.json")
    if not os.path.isfile(mpath):
        raise P.PackError("%s has no manifest.json (not a --dump-assets folder)" % part)
    m = P.read_json_file(mpath)
    if m.get("format") != "x2-asset-pack":
        raise P.PackError("%s: not an asset pack or dump manifest" % mpath)
    return m


def merge_dumps(parts, out):
    """Merge the dump folders `parts` into the new folder `out`; returns the merged manifest."""
    if not parts:
        raise P.PackError("no dump folders given")
    manifests = [(p, _load_part(p)) for p in parts]
    scales = {m.get("scale", 1) for _, m in manifests}
    if len(scales) != 1:
        raise P.PackError("the dumps have different scales: %s" % sorted(scales))
    scale = scales.pop()

    seen = []  # merged seen_palettes
    graphics = {}  # key -> (entry, source part, brightness)
    far_lines = []  # (hash, Image of the line rows, strip scale)
    far_seen = set()
    far_scale = None
    extra = {}
    for part, m in manifests:
        pal_map = {}
        for i, pal in enumerate(m.get("seen_palettes", [])):
            tup = tuple(pal)
            if tup not in [tuple(x) for x in seen]:
                seen.append(list(pal))
            pal_map[i] = [tuple(x) for x in seen].index(tup)
        for key, e in m.get("graphics", {}).items():
            if not P.is_gfx_key(key) or "png" not in e:
                continue
            img = P.read_png(P.safe_join(part, e["png"]))
            br = img.opaque_brightness()
            ne = dict(e)
            if "palette" in ne and ne["palette"] in pal_map:
                ne["palette"] = pal_map[ne["palette"]]
            if key in graphics:
                old_e, old_part, old_br = graphics[key]
                ne["uses"] = old_e.get("uses", 0) + e.get("uses", 0)
                old_e["uses"] = ne["uses"]
                if br > old_br:
                    keep_first = old_e.get("first_frame")
                    graphics[key] = (ne, part, br)
                    if keep_first is not None:
                        ne["first_frame"] = min(keep_first, ne.get("first_frame", keep_first))
                continue
            graphics[key] = (ne, part, br)
        for strip in m.get("far", []):
            s = strip.get("scale", m.get("scale", 1))
            if far_scale is None:
                far_scale = s
            elif far_scale != s:
                raise P.PackError("the dumps use different far strip scales (%d and %d)" % (far_scale, s))
            img = P.read_png(P.safe_join(part, strip["png"]))
            for i, h in enumerate(strip.get("lines", [])):
                if h in ("-", "") or h in far_seen:
                    continue
                if (i + 1) * s > img.h:
                    raise P.PackError("%s: far strip has fewer rows than lines" % part)
                far_seen.add(h)
                far_lines.append((h, img.crop(0, i * s, img.w, s)))
        for k, v in m.items():
            if k not in KNOWN_SECTIONS and k not in extra:
                extra[k] = v

    if os.path.exists(out) and os.listdir(out):
        raise P.PackError("%s exists and is not empty" % out)
    os.makedirs(out, exist_ok=True)
    merged_graphics = {}
    for key in sorted(graphics):
        e, part, _ = graphics[key]
        dst = P.safe_join(out, e["png"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(P.safe_join(part, e["png"]), dst)
        merged_graphics[key] = e
    far = []
    if far_lines:
        width = far_lines[0][1].w
        s = far_scale or 1
        for n in range(0, len(far_lines), STRIP_LINES):
            chunk = far_lines[n:n + STRIP_LINES]
            strip = P.Image(width, STRIP_LINES * s)
            for i, (_, row) in enumerate(chunk):
                strip.paste(row, 0, i * s)
            name = "far/strip_%03d.png" % (n // STRIP_LINES)
            P.write_png(P.safe_join(out, name), strip)
            lines = [h for h, _ in chunk] + ["-"] * (STRIP_LINES - len(chunk))
            ent = {"png": name}
            if s != 1 or "scale" in manifests[0][1]:
                ent["scale"] = s
            ent["lines"] = lines
            far.append(ent)
    # folders of later dump formats (screens/, panel/, ...): copy files that are not there yet
    for part, _ in manifests:
        for name in sorted(os.listdir(part)):
            if name in ("sprites", "tiles", "far", "manifest.json") or not os.path.isdir(os.path.join(part, name)):
                continue
            for dp, dn, fns in os.walk(os.path.join(part, name)):
                dn.sort()
                for fn in sorted(fns):
                    rel = os.path.relpath(os.path.join(dp, fn), part).replace(os.sep, "/")
                    dst = P.safe_join(out, rel)
                    if not os.path.exists(dst):
                        os.makedirs(os.path.dirname(dst), exist_ok=True)
                        shutil.copyfile(os.path.join(dp, fn), dst)
    first = manifests[0][1]
    merged = {"format": "x2-asset-pack", "version": first.get("version", 1),
              "name": "Merged dump of %d runs" % len(manifests),
              "note": first.get("note", ""), "scale": scale, "graphics": merged_graphics, "far": far,
              "seen_palettes": seen, "palettes": first.get("palettes", [])}
    merged.update(extra)
    P.write_json_file(os.path.join(out, "manifest.json"), merged)
    return merged


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("parts", nargs="+", help="dump folders to merge")
    ap.add_argument("--out", required=True, help="the new, empty or missing folder for the merged dump")
    a = ap.parse_args(argv)
    try:
        m = merge_dumps(a.parts, a.out)
        print("merge_dumps: %d graphics, %d far strips (%d lines), %d palettes -> %s"
              % (len(m["graphics"]), len(m["far"]), sum(1 for s in m["far"] for h in s["lines"] if h != "-"),
                 len(m["seen_palettes"]), a.out), file=sys.stderr)
    except P.PackError as e:
        print("merge_dumps: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
