# Tutorial: make a 2x sprite pack

You will dump the graphics of your own copy, redraw a few sprites at twice the Amiga resolution, pack them into a sprite sheet, check the pack
and see it in the game. About 20 minutes with a picture editor; the steps that need no art take a minute.

Commands are run from a source checkout of x2recomp. `x2recomp` is the built program (`build/x2recomp`); add `--assets DIR` if your game is
not imported into the data directory. The guide (`docs/ASSET-PACKS.md`) explains the ideas; the reference lists every field.

## 1. Dump the graphics

```
x2recomp --headless --input-script tests/replays/level1_play.txt --dump-assets my-dump
```

This plays the committed level 1 replay without a window and writes everything it draws into `my-dump/`:

```
my-dump/manifest.json    a valid pack: the original graphics at scale 1
my-dump/sprites/         one picture per sprite, named by its key
my-dump/tiles/           the same for tiles
my-dump/far/, far_full/  the far layer
my-dump/flashes/, screens/, panel/
```

The summary line tells you how much it found: `dump-assets: 231 sprites, 83 tiles, 192 far lines, 11 flashes, ...`. The dump covers only what
the replay drew. Run `tools/assetpack/dump_all.sh -o my-dump` to cover every level, boss and shop, or play a part by hand with
`--dump-assets my-dump` and quit normally. Dumps are for you: never share them or edited copies of them.

## 2. Find the graphics you want

```
python3 tools/assetpack/contact_sheet.py my-dump --out sheet.png --scale 3 --kind sprite
```

`sheet.png` shows every sprite, enlarged and labelled with its key (`0048848ccf64d966-16x12`: the hash, then the size in Amiga pixels).
Open it and note the keys of the sprites you will redraw. Add `--group NAME` to see one named group once your manifest has `groups`.

## 3. Redraw at 2x

Copy the pictures you chose to a working folder, named by key, and redraw each at **twice its size**: a `16x12` sprite becomes `32x24`
pixels. Keep the transparent parts transparent.

```
mkdir work
cp my-dump/sprites/0048848ccf64d966-16x12.png work/
```

If you only want to see the pipeline work, this stand-in doubles every picture and swaps its red and blue so the change is obvious. It
uses the PNG code of the tools, so it needs nothing installed:

```python
import os, sys
sys.path.insert(0, "tools/assetpack")
from pack_lib import read_png, write_png

for name in os.listdir("work"):
    if not name.endswith(".png"):
        continue
    img = read_png("work/" + name).scaled(2)          # nearest-neighbour 2x
    d = img.data
    for i in range(0, len(d), 4):
        d[i], d[i + 2] = d[i + 2], d[i]               # swap red and blue
    write_png("work/" + name, img)
```

## 4. Pack the sheet

Start the pack with a manifest that says what it is and its scale, then let `pack_atlas.py` put the pictures on one sheet and add the
`atlases` entry for you:

```
mkdir my-pack
```

Write `my-pack/manifest.json`:

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.sprites-2x",
  "name": "My 2x sprites",
  "pack_version": "0.1.0",
  "license": "CC0-1.0",
  "provides": ["graphics"],
  "scale": 2
}
```

then:

```
python3 tools/assetpack/pack_atlas.py work --out-png my-pack/sheets/sprites.png --into my-pack/manifest.json --scale 2 --padding 2 --bleed 2
```

`--scale 2` makes the tool refuse a picture that is not twice the key's size. `--padding 2 --bleed 2` leave a 2 pixel margin around each
frame and repeat its edge pixels into it, so smoothing at larger canvas scales does not pull in the neighbour's colour. The tool writes
`my-pack/sheets/sprites.png` and adds this to the manifest, one frame per key:

```json
"atlases": [
  {"png": "sheets/sprites.png", "scale": 2,
   "frames": [{"key": "0048848ccf64d966-16x12", "rect": [2, 2, 32, 24]}]}
]
```

(To go the other way and get the pictures back out of a sheet, run `python3 tools/assetpack/slice.py --manifest my-pack/manifest.json --all --out sliced`.)

If you prefer one file per graphic, skip the sheet and list `graphics` entries instead (`{"png": "sprites/ship.png"}`); both work and can be mixed.

## 5. Check it

```
x2recomp --check-asset-pack my-pack --against my-dump
```

```
info: asset pack: My 2x sprites: against: coverage of the dump: graphics 1/314 (0%), far lines 0/192 (0%), ...
check: My 2x sprites: 0 errors, 0 warnings, 1 entries used, 0 skipped: valid
```

`--against` compares your keys with the dump: a key the dump never saw is a typo or another build's graphic, and a picture that is a
byte-for-byte copy of a dump file is a warning. `python3 tools/assetpack/validate.py my-pack --against my-dump` gives the same report
without the game. The exit code is 0 when the pack is valid.

## 6. See it in the game

```
x2recomp --headless --input-script tests/replays/level1_play.txt --frames 1700 --modern interp,scale=2 --asset-pack my-pack --present-shot 1650:shot.png
```

writes the window picture of frame 1650 with your sprites in it. Run the same command without `--asset-pack` and compare the two. To play,
drop `--headless`, `--frames` and the script: `x2recomp --asset-pack my-pack --modern interp,scale=4`. Home turns the pack off and on;
F1 shows the original.

Everything else about drawing is unchanged: the game still makes its collision tests on the original sprite, so a bigger picture does not
make you easier to hit. Use `anchor` if a picture should extend past its original's corner (a glow), and `flash_rect` or `flash_png` for
the picture shown when the enemy is hit (otherwise it is your picture blended 70 % toward white).

## Where to go next

- Hit flashes, animation groups and a full list of sheet options: the guide's "Graphics" section.
- Recolouring a level: `palettes`. A whole painted background: `backgrounds`.
- If nothing changed, see "Troubleshooting" in the guide: it is nearly always a key that the dump never saw.
