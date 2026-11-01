# Asset packs: the guide

An asset pack replaces parts of the game with your own: sprites and tiles at up to 8 times the Amiga resolution and in any
colours, whole backgrounds, the title and menu screens, the status panel, sound effects, speech and music. Packs are optional and off
by default. The Faithful switch (F1, `--faithful`) always gives the original picture and the original sound back, bit for bit.

This page explains how packs work and how to make one. Pages that go with it:

- `docs/ASSET-PACKS-REFERENCE.md`: every manifest field, type, default and limit, and the checker's messages.
- `docs/asset-packs/`: step by step tutorials ([2x sprites](asset-packs/sprites-2x.md), [a title screen](asset-packs/title-screen.md),
  [music](asset-packs/music.md), [a sound effect](asset-packs/sound-effect.md), [a status panel font](asset-packs/hud-font.md)).
- `tools/assetpack/README.md`: the authoring tools. `examples/asset-pack-skeleton/`: a complete pack with a placeholder for every section.
- `docs/re-notes/asset-packs-v2-design.md`: the design, for people who want to know why it is shaped this way.

## The legal rule first

**x2recomp contains no game data and ships no asset pack. A pack must contain only artwork and audio that you made, or that is
licensed to you for this.**

- Everything `--dump-assets` and `--dump-audio` write comes from your own copy of the game. It is a working reference for making your
  pack. Never put it, or an edited copy of it, in a pack you share. `--check-asset-pack --against DUMPDIR` warns when a picture or a
  sound in your pack is a byte-for-byte copy of a dump file.
- The keys in a manifest (`0123456789abcdef-16x12` and so on) and a screen's signature blocks are hashes. They contain no picture and no
  sound; they say which original graphic a replacement stands in for. They are what lets a pack made by you work for someone else's copy.
- Say what your pack is licensed under in `license`, and who made what in `credits`. A pack with no `license` is a warning in the
  checker and an error with `--strict`.
- This is a description of how the project works, not legal advice. The game, its graphics and its music belong to their rights holders.

## What a pack can change

| What | Section | Disk builds | CDTV release | Atari ST |
|---|---|---|---|---|
| Sprites and tiles | `graphics`, `atlases` | yes | yes | no |
| Far layer (strips by line) | `far` | yes | yes | no |
| A level's far layer as one picture | `backgrounds` | yes | yes | no |
| Level palettes | `palettes` | yes | yes | no |
| Round stars | `stars` | yes | yes | no |
| Hit flashes | `flash_png`, `f:` keys | yes | yes | no |
| Title, menus, shop, ending screens | `screens` | yes | yes | yes |
| Status panel and HUD | `panel` | yes | yes | yes |
| Sound effects | `audio.sfx` | yes, see the notes below | yes | no |
| Speech | `audio.speech` | no (the game has none) | yes | no |
| Music | `audio.music` | title and levels 1-5 | title and levels 1-5, or by CD track | no |

Per build of the disk game:

| Build | Graphics, screens, panel | Sound effects | Music |
|---|---|---|---|
| Canonical image, retail pressings, cracks | yes | yes | yes |
| Power Pack | yes | yes (its samples are lower precision, so most keys differ: list both keys in `keys`) | yes |
| 1762 compilation | yes | yes | not yet: its music cue addresses are not mapped |
| PDos v5 | expected (same engine), not checked on real data | not yet: the sound code is not mapped | not yet |

The retail pressings, the Power Pack and the cracks use the canonical image's tables; the cracks were not each measured. Every build is
covered by recorded replays for screens and the panel; sound and music were checked on the canonical image and the CDTV release (and the effects of the 1762
compilation).

Things a pack cannot do: change gameplay or timing, replace the tune's individual instruments or notes, give the shop, bosses or ending
their own music (the game has no music cue there, so the tune continues), replace graphics the CPU draws itself apart from screens and
the status panel, or touch the Atari ST's sound. The machine always runs exactly as it does without a pack; a pack only changes what
you see and hear.

## Quick start

1. **Dump** what the game draws and plays (your own copy, a headless run):
   ```
   x2recomp --headless --input-script tests/replays/level1_play.txt --dump-assets my-dump --dump-audio my-dump
   ```
   Replay scripts are in `tests/replays/`; each runs to the length its own `frames` line gives (put `--frames N` **after**
   `--input-script` to stop sooner: the last one on the line wins). To cover every level, boss, shop and the ending in one dump, run all of them:
   `tools/assetpack/dump_all.sh -o my-dump` (add `-g cdtv` for the CDTV release). You can also play by hand with `--dump-assets DIR` on a
   normal run; the dump is written when you quit the game.
2. **Look** at what you have: `python3 tools/assetpack/contact_sheet.py my-dump --out sheet.png` draws every graphic with its key.
3. **Make a pack folder** with a `manifest.json` and your pictures and sounds. Start from the skeleton
   (`python3 tools/assetpack/make_skeleton.py my-pack`) or from the pieces of a dump.
4. **Check it**: `x2recomp --check-asset-pack my-pack --against my-dump`.
5. **Play it**: `x2recomp --asset-pack my-pack --modern interp,scale=4`, or put the folder in the `packs/` folder of the data directory
   (`x2recomp --print-data-dir`) and enable it (see "Loading").
6. **Share it**, if you wish, as a folder or a ZIP with your own artwork only.

The smallest pack that does anything:

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.first-pack",
  "name": "My first pack",
  "pack_version": "0.1.0",
  "license": "CC0-1.0",
  "scale": 4,
  "graphics": {
    "0123456789abcdef-16x12": {"png": "sprites/ship.png"}
  }
}
```

## Pack layout

A pack is a folder, or a ZIP file holding the same files at its root or inside one top folder:

```
my-pack/
  manifest.json
  sprites/ship.png
  sheets/enemies.png
  backgrounds/level1.png
  screens/title.png
  panel/digit_0.png
  sfx/fire.wav
  music/level1.ogg
```

Put audio in a ZIP **stored, not compressed**, so the player can stream it from the archive; a compressed music file is unpacked into
memory once and the checker warns about it. `zip -r -n .wav:.flac:.ogg ../my-pack.zip .` (run inside the folder) stores those
file types and compresses the rest.

## Loading, layering and switches

- `--asset-pack PATH` (a folder or a `.zip`, repeatable) loads packs in the order given.
- **The packs folder.** Folders and ZIP files in `<data dir>/packs/` are found at start. They are loaded only when their `id` is listed
  in `packs_enabled = id1;id2` in `settings.cfg`, and before any other pack. `x2recomp --list-asset-packs` shows what is there and
  whether it is enabled (`--json` for scripts). `x2recomp --print-data-dir` prints the data directory.
- **`settings.cfg`** (`asset_packs = A;B`, `packs_enabled`, `upgrade`, `audio_upgrade`) is read by the window, not by headless runs.
  Headless runs take `--asset-pack` and `--modern`.
- **Order.** Packs from `packs_enabled` first, then `asset_packs`, then each `--asset-pack`. A later pack replaces an earlier one per key
  (a graphic, a screen, a panel cell, a clip, a music slot); for palettes the latest one that matches wins. The loader reports the
  first 20 replacements of each pack so you can see conflicts.
- **Switches.** Home (Cmd+U on a Mac) turns packs on and off for graphics and screens (`--modern upgrade` and `noupgrade`). Pack sound
  is on when a loaded pack has sound; `--modern noaudio`, or `audio_upgrade = off`, turns it off. F1 (Cmd+1) and `--faithful` turn
  everything off at once, in the middle of a tune too.
- **Volumes.** `--sfx-volume`, `--speech-volume` and `--music-volume` scale pack sounds the way they scale the originals.

The loader prints one line per problem and one summary per pack:

```
asset pack: My pack: graphics["0123456789abcdef-16x8"]: missing file 'sprites/old.png'
asset pack: My pack: loaded: 106 graphics, 192 far lines, 1 palettes (all packs: 323 KB of pictures)
```

## How the game's graphics are identified

The modernisation layer records every blit and screen write of each logic step and replays them into a larger picture. It recognises each
original graphic by a key computed from its content, so a key does not depend on a memory address, the disk version or the level.

| Kind | What the game draws | Key |
|---|---|---|
| sprite | a bob: four cookie-cut blits, one per plane, sharing the mask | `<hash>-<w>x<h>` |
| tile | a masked tile or an opaque tile of the foreground map | `<hash>-<w>x<h>` |
| far line | one 320-pixel line of the far layer | `<hash>` |
| hit flash | a bob's mask drawn as a flat colour | `f:<hash>-<w>x<h>` |
| panel cell | a run of 1 to 8 eight-pixel cells of the status panel | `<hash>-<n>` |
| sound | the bytes of a sample, at the moment its channel starts | `<hash>-<bytes>` |

`<hash>` is the FNV-1a 64 hash, 16 lowercase hex digits, of the graphic as drawn: its size, then for each pixel of the mask's bounding
box whether it is drawn and its colour number (0 to 15). The bounding box makes the key independent of the blitter shift, so a sprite
has one key at every x position. `<w>x<h>` is that box in Amiga pixels. Tiles and objects cut off at the bottom of the playfield use the
replacement of the full graphic, clipped to the rows drawn. Graphics cut off at the left or right edge, the CPU-drawn parts and
anything not listed above keep the original picture.

Both disk and CDTV releases draw through the same routines, so a graphic usually has the same key in both: 108 of the 109 graphics of
the disk release's level 1 have the same key in the CDTV release's. A graphic only one release draws needs its own replacement.

## Graphics

### Sprites and tiles

`graphics` maps a key to a picture. The picture is normally `w * scale` by `h * scale` pixels for the key's `w` and `h`, aligned with the
original's top-left corner; any PNG that is not interlaced (RGBA, RGB, grey, grey with alpha, or palette, with transparency) works.

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.ship",
  "license": "CC0-1.0",
  "scale": 4,
  "graphics": {
    "0123456789abcdef-16x12": {"png": "sprites/ship.png"},
    "1123456789abcdef-16x15": {"png": "tiles/wall.png", "scale": 2, "anchor": [2, 0]},
    "2123456789abcdef-16x12": {"png": "sprites/ship.png", "keys": ["3123456789abcdef-16x12"]}
  }
}
```

- `scale` is the number of picture pixels per Amiga pixel (1 to 8), for one entry or for the whole pack.
- `anchor` places picture pixel `[x, y]` on the original's top-left corner, so a picture can extend past the original (a glow, a shadow).
- `keys` lists more keys that use the same picture, for a graphic that has a different hash in another build.
- A replaced sprite or masked tile is drawn over what is underneath it; its transparent parts show the background. A replaced opaque
  tile or far line is drawn over the original; its transparent parts show the original. Whatever the game draws later on top (objects
  over tiles, explosions, stars) still covers the replacement, and the game's own collision tests run on the original graphics.
- Pictures are resampled once when the window's canvas scale (`--modern scale=N`, default 3) differs from the picture's scale.
- Animation frames switch at the game's step rate; interpolation does not invent frames.

### Sprite sheets

Instead of one file per graphic, `atlases` lists frames of a sheet. Each frame becomes an ordinary `graphics` entry at load time.

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.sheets",
  "license": "CC0-1.0",
  "scale": 4,
  "atlases": [
    {"png": "sheets/ship.png",
     "frames": [{"key": "0123456789abcdef-16x12", "rect": [0, 0, 64, 48], "anchor": [0, 0]}]},
    {"png": "sheets/enemies.png",
     "grid": {"cell": [64, 48], "columns": 8, "keys": ["0123456789abcdef-16x12", null, "1123456789abcdef-16x12"]}}
  ],
  "groups": {"enemies": ["0123456789abcdef-16x12", "1123456789abcdef-16x12"]}
}
```

`rect` is `[x, y, w, h]` in sheet pixels and must be the key's size times the scale. A `grid` lays cells out row by row; `null` skips one.
`groups` is only for the tools (contact sheets, completeness checks). `tools/assetpack/slice.py` cuts a sheet into per-key pictures and
`tools/assetpack/pack_atlas.py` packs per-key pictures into a sheet and writes its `atlases` entry; see the tutorial
[2x sprites](asset-packs/sprites-2x.md).

### Hit flashes

When an enemy is hit, the game draws its mask as a flat white-ish shape for one step. A pack can supply that picture:

- **`flash_png`** on a graphic, or **`flash_rect`** on an atlas frame: the picture drawn when that graphic flashes.
- **An `f:` key** in `graphics` (`"f:<hash>-<w>x<h>": {"png": "..."}`): a picture for a mask shape, wherever it is used. A dump writes these
  as `flashes/*.png`.
- Without either, the flash is the replacement picture blended 70 % toward white, with its transparency kept.

A flash is tied to its graphic through the object that drew it and the shape of the mask, so an enemy that is hit the first moment it
appears keeps the original flash once.

### The far layer: `far` and `backgrounds`

The far layer (the scrolling backdrop) is a ring of 192 lines that repeats. Two ways to replace it:

- **`far`** strips are keyed by the content of each line. Identical original lines get the same replacement, so you cannot draw a
  seamless painting this way.
- **`backgrounds`** gives a level's whole layer as one picture, `320 * scale` wide and `192 * scale` tall, and wins over `far` for that
  level. A dump writes the original as `far_full/levelN.png` and lists it under `backgrounds`; start from it.

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.level1-background",
  "license": "CC0-1.0",
  "backgrounds": [{"level": 1, "png": "backgrounds/level1.png", "scale": 4}]
}
```

The picture wraps vertically, so make its top and bottom edges meet. Everything the game draws after the far layer (tiles, objects, stars)
is drawn over it. Pixels you leave transparent show the original far layer. On the CDTV release level 4 rewrites its far layer during
the level, so a dump writes `far_full/level4.png` but does not list it. Backgrounds are shown at full brightness while the game fades.

### Palettes

`palettes` recolours a level. `match` is the level's 16 original colours (`#rgb`, as listed in a dump's `seen_palettes`), `colors` the 16
replacements (`#rrggbb`), `layers` which layers get them (`far`, `tiles`, `objects`, `stars`, `other`). With a layer list the far layer
can have colours of its own, which the game's single shared palette does not allow. The override applies only while the game's palette is
exactly `match`, so during a fade the original colours show.

### Stars

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.round-stars",
  "license": "CC0-1.0",
  "stars": {"shape": "round", "size": 1.5, "softness": 0.25}
}
```

`round` draws an anti-aliased disc of `size` Amiga pixels over each star the game plots, coloured through the palette's `stars` layer.
The game still plots its own pixels, so its own tests are unaffected. Round stars draw on one thread (about 6 ms a frame at 4x).

### Screens

`screens` replaces whole screens that are not gameplay: the caption and credits pages, the SELECT menu, the high-score table, the shop,
the loading and GET READY screens, game over, the ending, the CDTV front end and the ST front end. A screen is recognised by a
**signature**: hashes of up to 64 blocks of the screen's colour numbers (not its colours, so fades do not matter). A dump writes the
signature for you.

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.title",
  "license": "CC0-1.0",
  "screens": {
    "title": {
      "signature": {"block": 16, "min": 2, "blocks": [
        {"x": 48, "y": 16, "hash": "0123456789abcdef"},
        {"x": 112, "y": 16, "hash": "1123456789abcdef"}]},
      "png": "screens/title.png",
      "scale": 4,
      "ref_palette": ["#000", "#fff", "#f80", "#08f"],
      "fade": "scale",
      "bg_index": 0,
      "dynamic": [{"x": 96, "y": 160, "w": 128, "h": 16}],
      "wide_png": "screens/title_wide.png"
    }
  }
}
```

- The picture is `320 * scale` by `200 * scale` pixels, the game's window.
- `ref_palette` is the screen's full-brightness palette. With `fade` `scale` (the default) the picture follows the game's fade-ins and
  fade-outs; without it, or with `none`, the picture stays at full brightness.
- `dynamic` rectangles are parts the game keeps drawing itself (scores, names, shop prices). Inside them the original pixels whose
  colour number is not `bg_index` are drawn over your picture. A dump finds these rectangles by seeing what changes while the screen is up.
- `wide_png` fills the sides when widescreen is on (`--modern wide=16:9`); without it the sides stay black.
- The screen is matched when at least `min` blocks match, and it keeps matching while at least `min - 2` do. A screen the dump saw is
  matched only when all of its blocks match; lower `min` to be more forgiving. Blocks are pixel-exact, so a screen that changes in a
  block that never changed during the dump falls back to the original, which is the safe failure.
- A dump cannot write signatures for screens where every block changes (the scrolling credits and high-score table of the attract loop,
  the ending's scrolling text). Those keep the original picture.

The dump's `screens/NAME.png` holds the dynamic parts as background, so it reproduces the original exactly; `NAME_reference.png` is the
original at its most frequent palette. Both come from your copy: use them to see what to draw, never to distribute.

### The status panel and the HUD

The status panel (window lines 192 to 199) is written by the game's CPU as finished glyph bytes, so a pack replaces it **cell by cell**:
the strip is cut into 8-pixel cells, and each run of 1 to `max_span` cells whose content matches a key is replaced. A dump writes every
distinct cell it saw as `panel/<key>.png` (about 70 for a font, plus the bar and icon cells).

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.hud",
  "license": "CC0-1.0",
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

At each column the longest matching run is tried first. A cell with no replacement shows the original (upscaled by the HD panel scaler
when it is on). `background` is drawn first, so replacement cells can be transparent; a cell with a `background` needs a (transparent)
replacement for the blank cell, or the original blank cell is drawn over it. `anchor` lets a picture overhang the strip over the
playfield. The panel fades with the game like a screen does. See [a status panel font](asset-packs/hud-font.md).

The panel's colours 1 to 3 change between the levels and the shop and bonus screens. A cell picture is painted for the palette most
frames use; in the few percent of frames with another palette it is shown unchanged.

## Audio

Pack audio changes only the **output mix**. The emulated machine plays the original sound exactly as before, with the same DMA, interrupts
and timing, and the pack's sound is mixed in at the exact output frame of the event it replaces. The original is muted in the mix by
default. So `--audio-hash` and `--audio-out` stay repeatable, and every replay hash is unchanged.

Supported files are **WAV** (PCM 8, 16, 24 and 32 bit, float), **FLAC** and **Ogg Vorbis**, one or two channels, 8000 to 192000 Hz. Files
at other rates are resampled once. MP3 and Opus are not supported.

### Sound effects

The game starts a sound by enabling a Paula channel on a sample. The key of a sound is the hash and length of that sample's bytes, so it is the
same on every build that has the same sample. `--dump-audio DIR` writes every distinct effect (and speech clip) of a run as
`sfx/<key>.wav` with a `manifest.json` that is already a valid pack.

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.fire-sound",
  "license": "CC0-1.0",
  "audio": {
    "sfx": {
      "0123456789abcdef-4160": {
        "file": "sfx/fire.ogg",
        "ref_period": 357,
        "gain_db": -3,
        "pan": "channel",
        "pitch": "follow",
        "original": "mute",
        "cut": "channel",
        "fade_out_ms": 5,
        "loop": false
      }
    }
  }
}
```

- **`ref_period`** is the Paula period at which your file sounds like the original played normally; the dump writes it. The voice's speed is
  `ref_period` divided by the period the game is using now, so pitch bends and sweeps carry over. A file redone at the same duration
  and pitch as the original needs the same `ref_period`. `pitch: "fixed"` plays the file at its own rate.
- **Volume** follows the game's own channel volume, then `gain_db` (-60 to 24). A full-scale mono file plays at the level of one Paula channel
  at full volume; a dump plays back at the original's level with `gain_db` 0. Raise your file with `gain_db` if it is quiet.
- **`pan`** `channel` puts a mono file on the side of the channel that started it (left: channels 0 and 3, right: 1 and 2) with the
  stereo separation setting; a number from -1 to 1 fixes the position. Stereo files play as they are. A mono replacement is played once per
  start, so a sound the game starts on two channels at once is heard on both sides as the original is.
- **`cut`** `channel` (the default) ends the voice, with `fade_out_ms`, when the channel gets a new sound or goes idle, as the original is cut.
  `never` lets it ring out. **`loop`** repeats the file while the channel keeps replaying the same sample (a dump sets it for
  sounds the game loops, so a looped sweep does not stop early).
- **`original`**: `mute` (default), `mix` (keep the original at the same level) or `mix:-12` (keep it 12 dB below).
- Up to 16 pack voices play at once; a seventeenth fades out the oldest.
- The A500 low-pass filter does not apply to pack voices (it is part of Paula's mix): give the final sound you want.
- On the **disk builds** only effects started by the game's effect code are replaced; the tune's instrument notes are never touched, so
  the shop and ending, which use their own player, list instrument samples as "effects" in a dump and can be replaced one by one.
  A dump can list one stale start (a channel enabled before its registers were written; 1 use, the sample bank's first bytes): ignore it.

### Speech (CDTV release)

The narrator's clips are loaded into a fixed buffer, so every start from that buffer is speech. They go under `audio.speech`, with the same
fields as effects, and may be named by the disc file they come from (`"cd0_file": "SPEECH63"`) instead of by hash, which is independent of
the disc's language. `--dump-audio` writes both the key and `cd0_file`. Speech has its own volume (`--speech-volume`, `speech_volume`).

### Music

Music is keyed by **slot**, the moment the game starts a tune:

| Slot | Disk builds | CDTV release |
|---|---|---|
| `title` | the title tune | the title and attract sequence and the SELECT screen |
| `level1` to `level5` | a level start, restart after a death, return from the shop (the level word) | the same points, by level |
| `level` | any level with no slot of its own | the same |
| `cd2` to `cd13` | not used | the CD track the game asks for (the exact key; it depends on SELECT TITLES) |

Resolution is exact slot first (`cd5`), then the role (`title`, `level3`), then `level`. A slot that cannot be opened is skipped with one
warning and the next is tried; with none left the original music plays. On the disk build, a pack slot comes before an imported CD
track (`--music cdtv`), which comes before the tracker.

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.music",
  "license": "CC0-1.0",
  "audio": {
    "music": {
      "defaults": {"crossfade_ms": 250, "loop_mode": "pack"},
      "slots": {
        "title":  {"file": "music/title.ogg", "loop": {"start_s": 4.0, "end_s": 80.5}},
        "level3": {"intro": "music/l3_intro.flac", "file": "music/l3_loop.flac", "loop": true},
        "level":  {"file": "music/level.ogg", "loop": true},
        "level4": {"file": null, "mute": true}
      }
    }
  }
}
```

- **Loops.** `loop: true` repeats the whole file, `false` plays it once and then silence, and `{start_s, end_s}` repeats those points of the
  file (seconds, after the intro; `end_s` defaults to the end). With none of these the file's own loop is used (a WAV `smpl` chunk, or
  `LOOPSTART` and `LOOPLENGTH` tags) and, with no loop at all, the game's rule. Pack loops are sample-exact at any file rate.
- **`intro`** plays once before `file`.
- **`crossfade_ms`** (0 to 5000) overlaps tracks and seamless loops; **`fade_in_ms`** and **`fade_out_ms`** (0 to 10000) shape starts and stops.
- **`retrigger`**: `restart` (the default) starts the slot again when its cue comes while it is playing, as the game does after a death;
  `continue` leaves it running.
- **`loop_mode`**: `pack` (the default) loops by the pack's points; `game` keeps the original rule, where the game's own poll restarts a
  finished track from its start up to a second late.
- **`mute`** silences the original and plays nothing; `file` may be `null` only with `mute: true`.
- A level's tune is the same through the boss fight, the shop and the ending have no cue, so a pack track simply continues through
  them. `--sfx-volume` does not touch music; `--music-volume` does.

To find which slot a moment belongs to, log the game's cues. On the disk builds `--audio-cues FILE` writes one line per call of the music
driver, with the return address that tells the title (`008236`) from a level start (`00789C`); on the CDTV release `--cdtv-log FILE` logs
the CD calls with the track and the caller.

## The tools

| Tool | What it does |
|---|---|
| `x2recomp --dump-assets DIR` | Writes every distinct graphic of a run: `sprites/`, `tiles/`, `far/` (strips of 192 lines), `far_full/` (the far layer of each level), `flashes/`, `screens/`, `panel/`, and `manifest.json` (a version 2 manifest, itself a valid pack at scale 1 that gives back the original picture). Each graphic is written once, at the brightest palette it was drawn with, transparent where not drawn. |
| `x2recomp --dump-audio DIR` | Writes `sfx/` and `speech/` as WAV (the original, for reference only) and merges entries into the manifest. It merges with what is there: entries and files you edited are never touched. The Atari ST release plays no sampled sound; the command says so. |
| `tools/assetpack/dump_all.sh` | Runs every committed replay of a game and merges the dumps: `dump_all.sh -o my-dump [-g disk\|cdtv\|st] [-a ASSETS] [-x x2recomp] [-f FRAMES]`. |
| `tools/assetpack/merge_dumps.py` | Merges dumps: `merge_dumps.py --out merged dump1 dump2` (the brighter copy of a picture wins). |
| `tools/assetpack/contact_sheet.py` | An overview picture of a dump or pack, labelled with keys: `contact_sheet.py my-dump --out sheet.png --scale 3 --kind sprite`. |
| `tools/assetpack/slice.py`, `pack_atlas.py` | Cut a sheet into per-key pictures, and pack pictures into a sheet and its `atlases` entry. |
| `tools/assetpack/make_skeleton.py` | Writes a complete pack of placeholders: `make_skeleton.py my-pack --scale 4`. |
| `x2recomp --check-asset-pack PATH` | The checker: `[--game disk\|cdtv\|st] [--build TAG] [--against DUMPDIR] [--full] [--strict] [--json]`. `tools/assetpack/validate.py` does the same without the game. Exit codes in the reference. `--game` and `--build` check the pack's scoping; they do not know whether that game supports a section. With `--against`, a screen is identified by its first signature block, so screens that share one are counted once. |
| `x2recomp --list-asset-packs [--json]` | Lists the packs in the data directory's `packs/` folder with id, name, version, licence, games and whether they are enabled. |

Looking at the result:

- `--modern-shots F:DIR` (headless) writes the two real pictures of the first step pair at or after frame F and the interpolated pictures between.
- `--present-shot F:FILE` writes the window picture of frame F as a PNG, with the filter on.
- `--modern-out F-G:DIR` writes the presented pictures of frames F to G (`rate=HZ` in `--modern` sets the rate).
- `--modern-check` checks that the pack's pictures are consistent with the game's own and prints a summary.
- `--audio-out FILE.wav` writes the sound of the whole run; `--audio-hash --hash-at F` prints its hash. Run with and without the pack (and with `--faithful`) to hear and
  measure the difference. Faithful and no-pack hashes are identical.
- Windowed: `--asset-pack my-pack --modern interp,scale=4`, then Home to toggle the pack and F1 for the faithful picture.

A shell hint: `$F:dir` in zsh is a modifier, so write `${F}:dir` or a plain number when you build these arguments in a script.

## Limits

- Pictures: at most 4096 pixels a side and 8 million pixels (backgrounds 16384 and 32 million); manifest and each PNG at most 32 MB; all
  decoded pictures together 512 MB; at most 65,536 entries and 256 screens in a pack; ZIP files up to 4 GB without ZIP64.
- Clips (effects and speech): at most 120 seconds and 32 MB decoded, 256 MB for all packs together; music tracks up to two hours, streamed.
- Paths are relative and inside the pack: no absolute paths, drive letters, backslashes, `.` or `..`, control characters or Windows device
  names, at most 240 bytes. In a ZIP two names that differ only in case refuse the pack.
- A pack with no readable manifest, bad JSON, another `format` or a version above 2 is not used. A bad entry is skipped and reported; the
  rest of the pack is used. The full list is in the reference.
- Not replaced: graphics cut off at the left or right edge, the CPU-drawn parts of the playfield, and the scrolling credits, high-score table
  and ending text. Replacements do not follow the palette fades (they show at full brightness while the game fades, except screens and the
  panel, which follow with `ref_palette`). Round stars cost about 5 ms a frame at 4x. The Atari ST gets screens and the panel only.

## Troubleshooting

**A key never matches (nothing changes).** Run `--check-asset-pack my-pack --against my-dump`. A key the dump never saw is reported ("not in
the dump: a typo, or a graphic of another build"). The dump covers only what the replay drew: play or replay the part you want, or run
`dump_all.sh`. The same graphic can have another key on another build: add it under `keys`.

**"is AxB, a graphic of this key at this scale is expected to be CxD".** The picture is the wrong size for the key and `scale`. Redraw at
`w * scale` by `h * scale`, fix `scale`, or add an `anchor` if the overhang is meant.

**"missing file".** The path is relative to the pack, uses `/`, and in a ZIP is case-sensitive (and two names that differ only in case
refuse the pack). Check the folder structure inside the ZIP: the manifest may be at the root or inside one top folder.

**A screen is not replaced.** The signature must match: replay the same screen that the dump saw, or lower `min`. A different language or
build draws different text blocks. The screen may be one of the attract-loop scrolls, which have no signature.

**Black or dimmed screens during fades.** That is `fade: "scale"` following the game's fade. Without `ref_palette` the picture would stay at
full brightness. If it fades the wrong way or never, check that `ref_palette` is the screen's full-brightness palette (`#rrggbb` or
`#rgb`, as the dump wrote it).

**The HUD digits are in the wrong colours.** Index 1 to 3 of the strip are the level's metal colours and change in the shop and bonus
screens; the cell art is painted for the common palette.

**Music is silent.** The slot name does not match the cue: check `--audio-cues` (disk) or `--cdtv-log` (CDTV). A level slot is tried for the
level word; a `level` slot catches the rest. A slot whose file cannot be opened is skipped with one warning on the console. On the 1762
compilation and PDos builds music slots do nothing yet. Sound is off with `--modern noaudio` or `audio_upgrade = off`, and F1 (the Faithful
switch) turns all of it off.

**A sound is too quiet or clips.** `gain_db` raises or lowers it. A file whose peak is above full scale is a warning in the checker with
`--full`. Pack voices bypass the A500 filter, so a file that was filtered to sound like the original may sound duller than needed.

**Sound effects stop early or play twice.** Set `loop` for a sound the game loops, and keep the file at least as long as the original. A
mono replacement plays once per start; if the original is started on two channels you hear it from both.

**Deflated audio in a ZIP.** The checker warns: the member is inflated into memory instead of streamed. Store it (`zip -n .wav:.flac:.ogg`).

**Loop clicks.** Cut the loop at a zero crossing, or set exact `start_s` and `end_s`; pack loops are sample-exact, so a click is in the file.

**Nothing from the pack in a headless run.** `settings.cfg` is read only by the window; pass `--asset-pack` (and `--modern upgrade`) to a
headless run.

## Compatibility at a glance

| Feature | Disk (canonical, retail, cracks) | Power Pack | 1762 | PDos v5 | CDTV | Atari ST |
|---|---|---|---|---|---|---|
| Sprites, tiles, atlases | yes | yes | yes | expected | yes | no |
| `far`, `backgrounds`, `palettes`, `stars`, hit flashes | yes | yes | yes | expected | yes | no |
| `screens`, `panel` | yes | yes | yes | yes | yes | yes |
| Sound effects | yes | yes (other keys) | yes | no | yes | no |
| Speech | no | no | no | no | yes | no |
| Music | yes | yes | no | no | yes | no |

"Expected" means the build uses the same engine but the only public PDos dump is damaged, so a pack has not been run on it. The ST has
no draw capture, so its dump cannot tell a level that stands still from a screen: delete screens that are really gameplay.
