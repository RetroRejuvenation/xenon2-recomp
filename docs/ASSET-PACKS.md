# Asset packs: replacement graphics

An asset pack replaces original graphics of the game with your own pictures, at up to 8 times the Amiga
resolution and in any colours. Packs are optional and off by default. The Faithful switch (F1, `--faithful`)
always shows the original picture.

```
x2recomp --dump-assets my-dump --input-script tests/replays/level1_play.txt --headless --frames 3000
x2recomp --asset-pack my-pack --modern interp,scale=4
```

No pack is shipped with x2recomp. A dump (below) holds your own copy's graphics: it is for making your pack,
never for distributing. Share only artwork you made yourself.

## How graphics are identified

The modernisation layer captures every blit and screen write of each logic step and replays them into a larger
canvas (`docs/re-notes/modernisation-design.md`). During the replay it recognises each original graphic and
looks it up by a key computed from its content, so the key does not depend on memory addresses, the disk
version or the level module:

| Kind | What the game draws | Key |
|---|---|---|
| sprite | a bob: 4 cookie-cut blits, one per plane, sharing the mask (`blit_bob_cookie` `$129A` and the object draw handlers) | `<hash>-<w>x<h>` |
| tile | a masked tile (4 cookie-cut blits) or an opaque tile (one 4-plane copy) of the foreground map, also when an object draws tiles | `<hash>-<w>x<h>` |
| far line | one 320-pixel line of the far layer (planes 0-1 of the ring copy at `$1832`/`$185A`) | `<hash>` |

- `<hash>` is the FNV-1a 64 hash (16 lowercase hex digits) of the graphic as drawn: its width and height, then
  for each pixel of the drawn mask's bounding box whether it is drawn and its colour index (0-15). The bounding
  box makes the key independent of the blitter shift, so a bob has one key at every x position.
- `<w>x<h>` is the size of that bounding box in Amiga pixels. Two graphics with the same hash but different
  sizes are different keys.
- Tiles and objects crossing the bottom of the playfield are drawn with fewer rows. They use the replacement of
  the full graphic (drawn earlier from the same source data), clipped to the rows drawn, so a pack only needs
  the full graphic.
- Not replaced (the original is shown): graphics clipped at the left or right edge, the hit-flash variants
  (minterms `FA`/`0A`), the starfield, the status panel and anything drawn by the CPU, and all non-gameplay
  screens (title, menus, shop, ending).

## Pack layout

A pack is a directory, or a ZIP file (stored or deflate) holding the same files either at its root or inside a
single top folder:

```
my-pack/
  manifest.json
  sprites/ship.png
  tiles/wall_03.png
  far/level1.png
```

### manifest.json

```json
{
  "format": "x2-asset-pack",
  "version": 1,
  "name": "My pack",
  "scale": 4,
  "graphics": {
    "0048848ccf64d966-16x12": {"png": "sprites/ship.png"},
    "01a6be94d61de0e7-16x15": {"png": "tiles/wall_03.png", "scale": 2, "anchor": [2, 0]}
  },
  "far": [
    {"png": "far/level1.png", "scale": 4, "lines": ["aec612394d830079", "dd9b44d08d36e3d1", "-"]}
  ],
  "palettes": [
    {"match": ["#000", "#222", "#444", "#665", "#888", "#aaa", "#ccc", "#fff",
               "#300", "#600", "#900", "#c00", "#030", "#060", "#090", "#0c0"],
     "colors": ["#000000", "#1a2b3c", "#28405a", "#665544", "#888888", "#aaaaaa", "#cccccc", "#ffffff",
                "#330000", "#660000", "#990000", "#cc0000", "#003300", "#006600", "#009900", "#00cc00"],
     "layers": ["far"]}
  ]
}
```

| Field | Meaning |
|---|---|
| `format`, `version` | must be `"x2-asset-pack"` and `1` |
| `scale` | default picture scale for the entries, 1-8 (picture pixels per Amiga pixel; default 1) |
| `graphics` | key -> entry. `png`: the picture, a relative path inside the pack. `scale`: its scale (default: the pack's). `anchor`: `[x, y]`, the picture pixel placed on the original's top-left corner (default `[0, 0]`; lets a picture extend beyond the original, e.g. for a glow). |
| `far` | strips of far-layer lines. The picture is `320 * scale` pixels wide; row block `i` (`scale` rows) replaces the line whose hash is `lines[i]` (`"-"` or `""`: unused row). Identical original lines get the same replacement row. |
| `palettes` | palette overrides. `match`: the level's 16 original colours (`#rgb`, Amiga 12-bit, as listed in a dump's `seen_palettes`). `colors`: 16 replacement colours (`#rrggbb`). `layers`: where they apply, any of `far`, `tiles`, `objects`, `stars`, `other` (default: all). With a layer list the far layer, for example, can get colours of its own, which the game's shared 16-colour palette does not allow. |

Unknown fields are ignored (a dump adds `kind`, `w`, `h`, `uses`, `first_frame`, `palette`, `seen_palettes` and
`note`).

### Pictures

- PNG, not interlaced: RGBA, RGB, grey, grey+alpha (8 or 16 bits) or palette (1-8 bits), with `tRNS`
  transparency. Alpha is honoured (premultiplied, with clean edges).
- Normally `w * scale` by `h * scale` pixels, aligned with the original's top-left corner. Other sizes are
  allowed (use `anchor`).
- Drawn on the canvas scale of the window (`--modern scale=N`, default 3): a picture at another scale is
  area-resampled once (exact pixel replication for integer up-scales).
- A replaced sprite or masked tile is drawn over what is underneath it: its transparent parts show the
  background. A replaced opaque tile or far line is drawn over the original: its transparent parts show the
  original.
- Whatever the game draws later on top (objects over tiles, explosions, the starfield) still covers the
  replacement as the original would be covered. The game's own tests run on the original graphics, so
  gameplay and the starfield behave exactly as without the pack.

## Loading

`--asset-pack PATH` (repeatable) or `asset_packs = A;B` in `settings.cfg` loads packs in order; later packs
override earlier ones (by key; palettes: the latest matching one). Home toggles them at run time (`upgrade`).
The loader prints one line per problem and a summary per pack:

```
asset pack: my-pack: graphics["0123456789abcdef-16x8"]: missing file 'sprites/old.png'
asset pack: my-pack: loaded: 106 graphics, 192 far lines, 1 palettes (all packs: 323 KB of pictures)
```

Validation and limits (`PackLimits` in `runtime/modern/assets.h`):

- **Paths.** Only relative paths inside the pack: no absolute paths, drive letters, backslashes, `.` or `..`
  components. In a directory pack the resolved path must also stay inside the pack (no symlink escapes).
- **Sizes.** A picture is at most 4096 pixels on a side and 8 M pixels, checked from its header before it is
  decoded; a PNG or manifest at most 32 MB; a ZIP file, and all its members unpacked, at most 256 MB; all
  decoded pictures of all packs together at most 512 MB; at most 65,536 entries per pack.
- **Errors.** A pack without a readable `manifest.json`, with invalid JSON or another format or version is not
  used. A bad entry (bad key, missing or corrupt PNG, picture too large, wrong strip size) is skipped and
  reported; the rest of the pack is used. PNG chunk CRCs are checked.

## Making a pack

1. **Dump.** `--dump-assets DIR` (window or headless) writes every distinct graphic drawn in gameplay:
   `DIR/sprites/<key>.png`, `DIR/tiles/<key>.png`, the far layer as `DIR/far/strip_NNN.png` (192 lines per
   strip, in order of appearance) and `DIR/manifest.json`. Each graphic is written once, in the brightest
   palette it was drawn with (not the black of a fade-in), transparent where it is not drawn. Bottom-clipped
   draws of a graphic already written are counted as uses of it. Play (or replay) the parts you want to cover;
   the dump only writes into DIR.
2. **Copy** the manifest, keep the entries you redraw, and point `png` at your pictures. A dump is itself a valid
   pack at scale 1: loaded unchanged it reproduces the original picture exactly (test `modern_upgrade`).
3. **Draw** each picture at `w * scale` by `h * scale`; set `scale` in the manifest.
4. **Recolour** a level with `palettes`: copy its colours from `seen_palettes`.
5. **Check** with `--asset-pack my-pack --modern interp,scale=4` (window) or `--modern-shots F:DIR` (headless).

## Out of scope and known limits

- **Both releases.** The CDTV release's engine draws through the same routines, and the key of a graphic is a content hash of the
  pixels as drawn, so a pack applies to the disk release and the CDTV release alike: 108 of the 109 graphics of the disk release's level-1
  dump have the same key in the CDTV release's (`docs/re-notes/cdtv-modern.md`). A graphic that only one release draws (a page of the
  CDTV program, say) needs its own replacement. `--dump-assets` works the same on `--game cdtv`.
- Graphics clipped at the left or right edge, hit flashes, the starfield, the status panel and screen text keep
  the original pictures. Menu, shop and title screens are not replayed, so packs do not apply there.
- Palette overrides match the level's exact colours, so during a palette fade (level start, death) the original
  colours are used.
- **HD status panel** (`--modern hdpanel`) is an edge-preserving upscale (Scale2x/Scale3x) of the panel as
  drawn. A panel re-drawn from the score, lives and energy variables with an embedded font was not done: the
  panel is written by the CPU (`$16BE`, `$176E`, `$6766`) as already-rendered glyph bytes, so re-drawing it
  needs the glyph sources mapped to characters (or the game's font, which cannot be shipped). Replacing the
  panel with pack pictures would need the same mapping.
- Animation frames switch at the step rate, as in the original.
