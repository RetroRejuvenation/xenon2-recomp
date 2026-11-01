# Tutorial: a complete status panel font

The strip at the bottom of the playfield (score, lives, energy, credits) is drawn by the game's CPU as already-rendered glyph bytes, so a pack
replaces it **cell by cell**: the strip is cut into 8-pixel cells, and every cell that matches a key is replaced by your picture. In this
tutorial you replace all of them with 4x pictures. The same applies on the CDTV release and the Atari ST.

Run commands from a source checkout; `x2recomp` is the built program (add `--assets DIR` if your game is not imported).

## 1. Dump the panel

```
x2recomp --headless --input-script tests/replays/level1_play.txt --dump-assets my-dump
```

```
dump-assets: 2 screens (558 transitions skipped, 0 not matchable), 153 panel cells (the strip had 9 palettes)
```

`my-dump/panel/` holds one 8 by 8 picture for every distinct cell the strip showed (153 here), named by key (`03f65971b188026a-1`: the hash of the
cell's colour numbers, then the number of cells the key covers, 1 to 8). `my-dump/manifest.json` has a `panel` section:

```json
"panel": {"scale": 1, "cell_w": 8, "max_span": 6, "fade": "scale",
          "ref_palette": ["#000000", "#222222", "..."],
          "cells": {"03f65971b188026a-1": {"png": "panel/03f65971b188026a-1.png", "uses": 3}}}
```

To cover digits that only appear later (higher scores, other weapons, the bonus screens, two players) dump more replays: `tools/assetpack/dump_all.sh` and
`tools/assetpack/merge_dumps.py` merge them. A font replacement needs about 70 pictures (the digits and letters in each of the strip's states), plus the energy bar
segments and icons; the others (the metal frame) can stay original.

## 2. Look at the strip

Open `my-dump/panel/` in an image viewer or editor that can enlarge small pictures. Each picture is **8 pixels wide and 8 tall** (a key
ending in `-1`; a key ending in `-3` covers three cells side by side and is 24 pixels wide, if you want to replace a word as one picture).

Cells are matched by their content as drawn: a digit in the score has a different key from the same digit in the credits if the
background or colours differ. That is why a font needs several pictures per glyph; do the common ones first and look at the result.

## 3. Redraw them at 4x

Each replacement is `8 * n * scale` by `8 * scale` pixels for a key of `n` cells: 32 by 32 at scale 4 for a single cell. Transparent parts
show whatever is under the cell: the `background`, or the original cell if you give none.

A stand-in that inverts every dumped cell at 4x, so you can see the whole pipeline work:

```python
import json, sys
sys.path.insert(0, "tools/assetpack")
from pack_lib import read_png, write_png

dump = json.load(open("my-dump/manifest.json"))
src = dump["panel"]
cells = {}
for key, entry in src["cells"].items():
    img = read_png("my-dump/" + entry["png"]).scaled(4)
    d = img.data
    for i in range(0, len(d), 4):
        d[i], d[i + 1], d[i + 2] = 255 - d[i], 255 - d[i + 1], 255 - d[i + 2]
    write_png("my-pack/panel/%s.png" % key, img)
    cells[key] = {"png": "panel/%s.png" % key}

panel = {k: src[k] for k in ("cell_w", "max_span", "ref_palette", "fade") if k in src}
panel["scale"] = 4
panel["cells"] = cells
pack = {"format": "x2-asset-pack", "version": 2, "id": "my.hud", "name": "My HUD", "pack_version": "0.1.0",
        "license": "CC0-1.0", "provides": ["screens"], "panel": panel}
json.dump(pack, open("my-pack/manifest.json", "w"), indent=1)
```

(Run `mkdir -p my-pack/panel` first.) Your manifest, whatever you draw, looks like this:

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.hud",
  "name": "My HUD",
  "pack_version": "0.1.0",
  "license": "CC0-1.0",
  "provides": ["screens"],
  "panel": {
    "scale": 4,
    "cell_w": 8,
    "max_span": 6,
    "ref_palette": ["#000", "#222", "#642", "#c62"],
    "fade": "scale",
    "background": {"png": "panel/background.png"},
    "cells": {
      "0123456789abcdef-1": {"png": "panel/digit_0.png"},
      "1123456789abcdef-3": {"png": "panel/lives_icon.png", "anchor": [0, -2]}
    }
  }
}
```

- **`max_span`** is the longest run of cells a key may cover; at each column the longest matching run is tried first, so a word you draw as one picture wins over the letters in it.
- **`background`** (`320 * scale` by `8 * scale`) is drawn first over the whole strip. Cells you do not replace keep the original over it, so if you use a
  background, give the blank cell a (transparent) replacement.
- **`anchor`** lets a picture overhang the strip, upwards over the playfield for an icon that rises above the bar.
- **`ref_palette` and `fade`** make the panel dim and brighten with the level's fade-in and fade-out, as screens do. A pack made from the dump
  as above keeps them.

## 4. Check and look

```
x2recomp --check-asset-pack my-pack --against my-dump
```

```
info: asset pack: My HUD: against: coverage of the dump: ..., panel cells 153/153, ...
check: My HUD: 0 errors, 0 warnings, 153 entries used, 0 skipped: valid
```

```
x2recomp --headless --input-script tests/replays/level1_play.txt --frames 1700 --modern interp,scale=3 --asset-pack my-pack --present-shot 1650:shot.png
```

`shot.png` shows the strip in your cells (inverted, with the stand-in). `--modern-check` (same command, no `--present-shot`) checks the pack's pictures against the game's own and prints a summary line.
Keys that your pack lacks show the original cell, so you can work font by font.

## 5. Things to know

- The strip is window lines 192 to 199 on every build, aligned to 8-pixel cells from the left edge (the cell grid is where the game writes its bytes).
- The strip is re-examined only when its bytes change, so a busy panel costs nothing.
- The panel's colours 1 to 3 change between the levels and the shop and bonus screens. Cell art is painted for the common palette; in the few percent
  of frames where the strip's palette is not a fade of it the cell is shown unchanged.
- The original panel upscaled with edge preservation (`--modern hdpanel`) is a separate option, used for the cells that your pack does not replace.
- Digits and letters can differ between builds in colour or position, so the same glyph may have another key on the CDTV release or the ST: dump each
  build, or add the other keys under `keys`.
