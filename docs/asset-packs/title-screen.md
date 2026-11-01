# Tutorial: replace the title screen

You will dump the screens of your own copy, redraw one at four times the Amiga resolution, and write the `screens` entry that makes the game
use it. The same method works for every non-gameplay screen: credits pages, the SELECT menu, high scores, the shop, game over, the ending,
and on the CDTV release and the Atari ST their own front ends.

Run commands from a source checkout; `x2recomp` is the built program (add `--assets DIR` if your game is not imported).

## 1. Dump the screens

The attract loop shows the title, credits and high scores with no input, so its replay finds most screens:

```
x2recomp --headless --input-script tests/replays/attract.txt --dump-assets my-dump
```

```
dump-assets: 9 screens (1241 transitions skipped, 2 not matchable), 0 panel cells (...)
```

`my-dump/screens/` now holds, for each screen it found, `screen_XXXXXXXX.png` and `screen_XXXXXXXX_reference.png`, and `my-dump/manifest.json` holds
a `screens` entry for each with its signature and palette. The names are the first eight hex digits of the signature's hash; rename them
in your pack (`title`, `credits1`, ...).

How a dump finds a screen: it looks at every frame, keeps the 16 by 16 blocks of colour numbers that never changed while the screen was
up, and writes up to 64 of them as the screen's **signature**. It also checks that the signature does not match any other screen or any
gameplay frame, and adds blocks that tell them apart. Parts that did change (scrolling or blinking text, scores) become `dynamic`
rectangles. Screens where everything moves are listed as "not matchable" and cannot be replaced.

## 2. See what you are replacing

Open `my-dump/screens/*.png`. `screen_XXXXXXXX.png` is the screen with its dynamic parts shown as part of the background, exactly as the
game draws it when nothing moves. `..._reference.png` is the original at its most frequent palette. Find the screen you want. Parts the
game animates (a cycling logo, blinking or scrolling text) are **dynamic**: the game keeps drawing them over your picture, so check
where they are (the `dynamic` rectangles of the manifest entry) before you paint there.

## 3. Draw the new picture

Make it **`320 * scale` by `200 * scale`** pixels. At scale 4 that is 1280 by 800. The game's window is 320 by 200 Amiga pixels (the Amiga
frame is 256 lines tall, with the window in the middle). Leave the `dynamic` rectangles clear of anything important: the original pixels
whose colour number is not `bg_index` (usually 0) are drawn over your picture there.

A stand-in, if you want to see the pipeline run before drawing anything: a blue gradient at 4x.

```python
import sys
sys.path.insert(0, "tools/assetpack")
from pack_lib import Image, write_png

S = 4
img = Image(320 * S, 200 * S)
for y in range(200 * S):
    img.fill_rect(0, y, 320 * S, 1, (20, 20 + y * 100 // (200 * S), 80 + y * 120 // (200 * S), 255))
write_png("my-pack/screens/title.png", img)
```

(Run `mkdir -p my-pack/screens` first.)

## 4. Write the manifest

Take the screen's entry from `my-dump/manifest.json` (its `signature`, `ref_palette`, `fade`, `bg_index` and `dynamic`), rename it, and point
`png` at your picture with its `scale`:

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.title",
  "name": "My title screen",
  "pack_version": "0.1.0",
  "license": "CC0-1.0",
  "provides": ["screens"],
  "screens": {
    "title": {
      "signature": {"block": 16, "min": 2, "blocks": [
        {"x": 48, "y": 16, "hash": "0123456789abcdef"},
        {"x": 112, "y": 16, "hash": "1123456789abcdef"}]},
      "png": "screens/title.png",
      "scale": 4,
      "ref_palette": ["#000", "#222", "#642", "#c62"],
      "fade": "scale",
      "bg_index": 0,
      "dynamic": [{"x": 96, "y": 160, "w": 128, "h": 16}]
    }
  }
}
```

The signature in the example is two placeholder blocks; copy your dump's real ones (a dump writes 29 to 40). A short Python script
does it (the dump manifest is plain JSON):

```python
import json
dump = json.load(open("my-dump/manifest.json"))
src = dump["screens"]["screen_8444da44"]            # the name your dump gave the screen
entry = {k: src[k] for k in ("signature", "ref_palette", "fade", "bg_index", "dynamic") if k in src}
entry["png"], entry["scale"] = "screens/title.png", 4
pack = {"format": "x2-asset-pack", "version": 2, "id": "my.title", "name": "My title screen",
        "pack_version": "0.1.0", "license": "CC0-1.0", "provides": ["screens"], "screens": {"title": entry}}
json.dump(pack, open("my-pack/manifest.json", "w"), indent=2)
```

What each field does:

- **`ref_palette`** is the screen's full-brightness palette, listed by colour number. With **`fade`: `scale`** your picture is dimmed and
  brightened with the game's own fade-ins and fade-outs, because the player compares the palette the game is using with `ref_palette`.
  With `none`, or no `ref_palette`, the picture is always at full brightness.
- **`min`** is how many signature blocks must match. A dump sets it to all of them. Lower it to tolerate a screen that differs a little
  (another language, another build), at the risk of matching a similar screen.
- **`wide_png`** fills the sides of a widescreen window (`--modern wide=16:9`); leave it out and they stay black.
- **`anchor`** lets the picture overhang the window.

## 5. Check and look

```
x2recomp --check-asset-pack my-pack --against my-dump
```

```
check: My title screen: 0 errors, 0 warnings, 1 entries used, 0 skipped: valid
```

The coverage line says how many of the dump's screens your pack replaces. To see the result in the title, the replay of the attract loop shows it
at frame 300:

```
x2recomp --headless --input-script tests/replays/attract.txt --frames 305 --asset-pack my-pack --modern interp,scale=3 --present-shot 300:shot.png
```

`shot.png` is the window picture with your screen in it. In a window, start the game normally: the title appears with your picture; Home
shows the original and the F1 menu's Faithful row the faithful frame.

## 6. If it does not match

- Look at the console when the pack loads: `asset pack: ...: screens["title"]: ...` explains a refused entry.
- The screen of another language or build has other text blocks. Dump that build and make a pack for it, or lower `min`.
- The dump's signature came from a screen the game never shows without input; replay the part that shows it (`tests/replays/` has the shop, game over and
  ending) or merge several dumps with `tools/assetpack/merge_dumps.py`.
- A screen that scrolls the whole picture has no signature; it keeps the original.
- On the Atari ST the dump cannot tell a still level from a screen: delete the screens that are not screens.
