# Asset packs v2: design and format specification

Status: design, written 2026-11-01 against `main` at `6fc6f45`. Nothing here is implemented. It is the input for the work
breakdown in section 10; the implementers turn sections 3-7 into the user guide (section 8). Items marked **(measure)** are
facts the implementing workstream has to establish first; the spec says what to do with each answer.

Companion files: `docs/schema/x2-asset-pack-v2.schema.json` (the manifest schema, checked against the two example manifests and
14 bad manifests while writing this note) and `docs/schema/examples/{v1-graphics,v2-full}.example.json` (placeholder keys, no artwork).

## 0. Summary of the decisions

| # | Decision | Why |
|---|---|---|
| 1 | One manifest format, `version: 2`, a superset of v1. v1 packs load unchanged. | No migration: packs made for v1 keep working. |
| 2 | Three kinds in one pack: **graphics**, **screens**, **audio**. Each switchable at run time; the single Faithful switch (the first row of the F1 menu since 0.5.0; first designed as the F1 key) turns all of them off. | The project promise: one switch restores the original presentation. |
| 3 | Packs only change what is *presented* (picture, output mix). The machine runs exactly as without a pack. | Replay goldens and determinism; the same rule the modernisation layer and `--protect-music` already follow. |
| 4 | Screens are matched by a **block signature** (hashes of stable 16x16 blocks of the colour-index image, at least `min` of them), not by a whole-frame hash and not by a per-build state id. | Survives animated or dynamic parts, fades and different builds; carries no original pixels. |
| 5 | The status panel is replaced **cell by cell by content hash** (8-pixel cells, spans of 1-8 cells, longest match first). No variable-bound panel in 2.0. | Works on every build, needs no glyph-to-character mapping, reuses the existing key idea; bindings are reserved for 2.x. |
| 6 | Sound effects and speech are replaced by the **content hash of the sample bytes at DMA start** and played as a *host voice* in the output mix; the original channel is muted in the mix only. Pitch follows the game's period changes relative to a reference period. | Content-stable across builds, no change to chip RAM, DMA, interrupts or timing. |
| 7 | Music is replaced per **slot** (`title`, `level1`..`level5`, `levelN` fallback, `cd2`..`cd13`), using the cue points the CDTV layer already detects. Tracker instruments are not replaceable. | Whole-track replacement covers the use case; instrument loops and per-note restarts make sample-level music replacement fragile. |
| 8 | Audio formats: WAV (own parser), FLAC (dr_flac), Ogg Vorbis (stb_vorbis). No MP3, no Opus. | Public-domain / MIT-0 decoders, small, GPL-compatible. Needs the user's go-ahead (section 11). |
| 9 | Pack voices are rendered in emulated time, in the same stage that renders the CD player, so `--audio-hash` stays exact and repeatable with a pack on. | Same determinism contract as the CD layer. |
| 10 | Atari ST in 2.0: screens and panel only (they work from the indexed frame). Sprites, tiles, backgrounds, stars and ST audio are later phases. | The ST draws with the CPU: there is no blit capture, and its YM effects are register patterns, not samples. |

## 1. Goals, non-goals, constraints

Goals: higher-resolution sprites and tiles, backgrounds, whole-screen artwork (title, menus, shop, ending, loading, game over,
the CDTV INTRO pages and language page, the ST front end), the status panel, replacement sound effects, speech and music, for all
releases and builds, with tools and documentation good enough that someone can make a pack without reading the source.

Non-goals: recolouring or filters (`palettes` stays; CRT and scanline options stay in the presenter, not in packs); changing game
logic, timing or the level data; any mechanism that needs original game data in a pack; a pack download service.

Constraints, all inherited from what exists:

- **Determinism.** Frame and audio hashes of the replay corpora must not change with no pack loaded, and must be repeatable with a
  pack loaded. Pack effects live in `Presenter` and in the output mix, never in the machine.
- **Legal.** No game-derived content in a pack and none produced by the project except the author-side dumps, which carry the same
  warning as `--dump-assets`: for making your pack, never for distributing. Signatures and keys are hashes.
- **Safety.** Packs are untrusted files: decode bombs, path tricks, zip bombs (sections 3.5 and 6).
- **Cost.** Pack off: zero measurable cost. Pack on: at most 5 % more frame time (graphics) and 3 % (audio) on `level1_flood`.

## 2. What exists and what is missing

`docs/ASSET-PACKS.md` (v1) covers graphics only: `manifest.json` with `graphics` (sprite and tile keys `<fnv1a64>-<w>x<h>`),
`far` (strips of far-layer lines by line hash), `palettes`, a pack-wide `scale` of 1-8, PNG pictures with `anchor`, directory or ZIP
packs, `--asset-pack` (repeatable), `asset_packs` in `settings.cfg`, `--dump-assets`, the `upgrade` toggle (Home, Cmd+U), limits in
`PackLimits` (`runtime/modern/assets.h`). The loader (`AssetLibrary`, `runtime/modern/assets.cpp`, 956 lines) also holds the PNG
decoder, the resampler and the dumper.

| Wish | State | Reason it is missing |
|---|---|---|
| Sprite sheets | per-key PNGs only | convenience, no run-time need |
| Backgrounds | far layer by line hash | identical lines share one replacement row; no position binding |
| Title, menu, shop, ending, loading, game over, INTRO pages | **none** | `Presenter::at()` shows the faithful frame for every non-gameplay moment |
| Status panel | edge upscale (`hd_panel`) only | the panel is CPU-written glyph bytes at lines 192-199 |
| Starfield, hit flashes | original pixels | stars are CPU pixel writes; flashes are partial blit groups (minterms `FA`/`0A`), not graphics |
| Sound effects, speech, music | **none** | no replacement hook; the CDTV layer plays imported CD tracks only |
| Per-release scoping | none (keys are content hashes, so packs apply across builds by accident of identical art) | |
| Validation, discovery, metadata | load-time messages only | |

## 3. The format (v2)

### 3.1 Layout and conventions

A pack is a directory or a ZIP (stored or deflate) with `manifest.json` at its root or inside one top folder, as in v1. Suggested
folders (any relative path works): `sprites/ tiles/ far/ backgrounds/ sheets/ screens/ panel/ sfx/ speech/ music/`.

- Paths: relative, `/` separators, UTF-8, at most 240 bytes, no empty/`.`/`..` components, no leading `/`, no backslash, no drive
  letter, no control characters, no Windows reserved names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`, with or without an
  extension). In a ZIP, two members whose names collide after case folding are an error. Directory packs: the resolved path must
  stay inside the pack (no symlink escape), as in v1.
- Numbers in JSON are plain decimals; colours are `#rgb` (Amiga 12-bit, as in dumps) or `#rrggbb`.
- Unknown fields are ignored by the loader and reported by the validator (a dump adds descriptive fields on purpose).

### 3.2 Top-level fields

| Field | Type, default | Meaning |
|---|---|---|
| `format` | `"x2-asset-pack"`, required | |
| `version` | `1` or `2`, required | `2` enables the new sections. A version above the loader's maximum: pack refused with "needs a newer x2recomp". |
| `name` | string <= 80 | shown in messages and `--list-asset-packs` (default: folder or file name) |
| `id` | `^[a-z0-9][a-z0-9._-]{1,63}$` | stable identifier for `requires` and `packs_enabled` (default: slug of `name`) |
| `pack_version` | semver `x.y.z[-pre]` | author's version of the pack |
| `description`, `author`, `url` | strings | metadata |
| `license` | string <= 200 | SPDX id or free text; missing -> validator warning (error with `--strict`) |
| `credits` | `[{name, role?, url?}]` | |
| `preview` | relative path | optional picture (<= 1 MB PNG) for front ends; no run-time use |
| `provides` | `["graphics","screens","audio"]` | optional summary; the loader recomputes it |
| `games` | subset of `disk`, `cdtv`, `st` (default all three) | pack-level scope; `disk` covers the canonical image, the retail pressings, the Power Pack, the 1762 compilation, the PDos build and the cracks (one engine, one capture layout); `cdtv` the CDTV disc; `st` the Atari ST |
| `builds` | `[string]` (default: all) | optional narrowing by manifest `build=` tag (`power-pack-2234`, `compilation-1762`, `pdos-v5`, `cdtv`, `atari-st-kixx`; the canonical image has no tag and is named `canonical`) |
| `requires` | `{x2recomp: ">=0.5", packs: [{id, version?}]}` | version constraints are `>=`, `>`, `=` followed by `x.y[.z]` |
| `scale` | 1-8, default 1 | default picture scale for the entries (v1 meaning) |

Sections (all optional): graphics kind: `graphics`, `far`, `backgrounds`, `atlases`, `groups`, `stars`, `palettes`, `panel`;
screens kind: `screens`; audio kind: `audio` with `sfx`, `speech`, `music`. A v1 manifest (`version: 1`) may only use `graphics`,
`far`, `palettes`, `scale` and metadata; the schema forbids the v2 sections under `version: 1`.

### 3.3 Scoping, layering and precedence

- **Scoping.** Every entry may carry `games` and `builds` (same meaning as at pack level). An entry applies when the current game
  family is in the intersection of the pack's and the entry's `games`, and the current build tag matches `builds` when given. A
  pack none of whose entries apply is loaded with an informational line, not an error.
- **Layering.** Packs load in this order: `packs_enabled` (from the data dir's `packs/` folder, in the listed order), then
  `asset_packs` paths from `settings.cfg`, then `--asset-pack` arguments. A later pack overrides an earlier one **per key**
  (graphics, screens, panel cells, clips, music slots); palettes: the latest matching one, as in v1. The loader reports the first
  override of each key once per run (info level) so authors see conflicts.
- **Dependencies.** `requires.packs[i]` must have been loaded earlier in the order, with a satisfying `pack_version`, or the
  requiring pack is skipped with one message. No automatic fetching or reordering: the order is the user's.
- **Toggles.** Graphics and screens obey `upgrade` (Home / Cmd+U, `--modern upgrade|noupgrade`, `upgrade =` in `settings.cfg`).
  Audio obeys the new `audio_upgrade` (default on when a pack with an audio section is loaded; `--modern audio|noaudio`; hotkey
  Page Up / Cmd+J proposed, to be checked against `--list-keys` for collisions). `--faithful` and F1 override both: the original
  picture and the original mix, bit for bit.

### 3.4 Keys and hashing

All new hashes use the same primitive as the graphics keys: FNV-1a 64 (offset `cbf29ce484222325`, prime `100000001b3`), printed as
16 lowercase hex digits (`hash_hex`). Inputs are fixed byte sequences so that every implementation (C++, the Python tools) agrees.

| Key | Where | Input bytes |
|---|---|---|
| sprite, tile | v1, unchanged | as `find_graphics` today |
| far line | v1, unchanged | as today |
| **block hash** | screens, panel cells | `w` (u16 LE), `h` (u16 LE), then `w*h` bytes of colour index (0-31), row-major, of the region of the indexed frame |
| **sample key** | sfx, speech | `<fnv1a64>-<bytes>`; input: `bytes` (u32 LE) then the `bytes` sample bytes as stored (signed 8-bit), read from chip RAM at DMA start (`AUDxLEN * 2` bytes from the latched `AUDxLC`) |

The **indexed frame** is the picture before the palette is applied: one colour-index byte per pixel (0-31; the Amiga window is
320 x 200 starting at `kFrameTop` of the 320 x 256 faithful frame; the ST picture is 320 x 200, indices 0-15). The video models
already decode planes into per-line index data, so `Video::index_frame()` and the ST equivalent are small additions **(measure:
hires screens, if any exist, are out of scope in 2.0)**.

### 3.5 Limits (extends `PackLimits`)

| Limit | v1 | v2 |
|---|---|---|
| manifest / one PNG | 32 MB | unchanged |
| picture side / pixels | 4096 / 8 M | unchanged; **backgrounds**: 16384 / 32 M |
| entries per pack (every kind, atlas frames included) | 65,536 | unchanged |
| decoded graphics, all packs | 512 MB | unchanged |
| ZIP file | 256 MB, all members unpacked | **4 GB** (no ZIP64), at most 65,536 members, any member <= 512 MB, central directory read first, members extracted on demand; deflated member ratio above 1000:1 refused unless it unpacks to <= 1 MB |
| audio file (one clip or track) | n/a | 512 MB |
| clip (sfx, speech) | n/a | <= 120 s, <= 32 MB decoded (int16), 1-2 channels, 8000-192000 Hz |
| decoded sfx + speech, all packs | n/a | 256 MB |
| music | n/a | streamed; at most 3 decoders open at once; <= 2 h per track |
| signature blocks per screen / screens per pack | n/a | 64 / 256 |
| polyphony of pack voices | n/a | 16 (oldest stolen with a 5 ms fade) |

Decode-bomb rules: every header is checked against the limits before any allocation; FLAC and Vorbis may declare an unknown
length, so the produced frame count is counted while decoding and decoding aborts at the cap; PNG and WAV chunk sizes are checked
against the file; a member that decodes to a different size than it declared is an error.

### 3.6 Errors

As in v1: a pack without a readable `manifest.json`, with invalid JSON, another `format`, or a version above the maximum is not
used; a bad entry (bad key, missing or undecodable file, wrong size, out-of-range value) is skipped and reported, the rest of the
pack is used. One line per problem, `asset pack: <name>: <section>["<key>"]: <problem>`, then a summary per pack.

## 4. Graphics

### 4.1 Kept as is

`graphics` (sprite and tile keys), `far`, `palettes`, `anchor`, `scale`, compositing rules (a replaced bob or masked tile is drawn
over what is underneath; a replaced opaque tile or far line over the original; later draws still cover it). New optional entry
fields: `keys` (extra keys sharing this picture: the same artwork for a graphic that exists in two builds under different hashes),
`flash_png` (section 4.6), `games`, `builds`.

### 4.2 Sprite sheets and animation (`atlases`, `groups`)

Convenience only; both expand to ordinary `graphics` entries at load time.

```json
"atlases": [
  {"png": "sheets/ship.png", "scale": 4,
   "frames": [{"key": "0048848ccf64d966-16x12", "rect": [0, 0, 64, 48], "anchor": [0, 0]}]},
  {"png": "sheets/enemies.png", "scale": 4,
   "grid": {"cell": [64, 48], "columns": 8, "keys": ["<key>", null, "<key>"]}}
]
```

`rect` is `[x, y, w, h]` in atlas pixels (must lie inside the picture; the validator also checks `w x h` against `key` and
`scale`). `grid` lays frames out row-major, `null` skips a cell. The decoded atlas counts once toward the memory limit; frames are
views or small copies (implementation choice, equal behaviour). `groups` (`{"ship": [keys...]}`) names sets of keys for the tools
(slicing, contact sheets, completeness checks); no run-time meaning. Animation frames are separate keys because the game switches
graphics at the logic step rate; interpolation does not invent frames.

### 4.3 Backgrounds (`backgrounds`)

```json
"backgrounds": [{"level": 1, "png": "backgrounds/level1.png", "scale": 4, "ring_lines": 1024}]
```

One picture for a level's whole far layer, wrapped vertically, positioned by the layer's scroll. It replaces `far` entries for that
level wherever both exist. It exists because `far` rows are keyed by line *content*: identical original lines cannot be given
different art, so a seamless painted background needs position, not content, as the key.

- **Position binding.** The far layer is drawn from a ring buffer whose current row is variable `kRing` (`$436` on the disk builds,
  `+6` on the CDTV release). The picture's row `r * scale` is the row the layer draws when the ring row is `r`; the canvas line
  `y` of a step shows picture row `((ring + y) mod ring_lines) * scale` (**measure**: the ring's wrap height and the scroll sign;
  the dump tool writes `far_full/levelN.png` unrolled by the same function, so a pack built from the dump is consistent by
  construction, and a test compares a dump loaded as a `backgrounds` pack at scale 1 with the original picture, exactly).
- **Level.** The level number is a new `Var` (`kLevel`; `$E04` on the disk builds, `$E0A` on the CDTV release: the variable block there is the disk addresses plus 6) added to
  `CaptureLayout::var_addr` and sampled at each swap with the others.
- `ring_lines` is optional (default: the build's table value); the validator checks the picture height equals `ring_lines * scale`.
- Limits in section 3.5 apply (a 1024-line ring at 8x is 8192 pixels tall). The picture must be `320 * scale` wide.
- The tile layer needs nothing new: tiles are already per-key.

### 4.4 Screens (`screens`)

Whole-screen replacements for the non-gameplay moments the presenter shows as the faithful frame: caption/credits, title, attract,
menus, the shop, the ending, loading, game over and high score, the CDTV INTRO pages and language page, the ST front end.

```json
"screens": {
  "title": {
    "signature": {"block": 16, "min": 14,
                  "blocks": [{"x": 32, "y": 48, "hash": "0123456789abcdef"}]},
    "png": "screens/title.png", "scale": 4,
    "ref_palette": ["#000", "#fff"],
    "fade": "scale",
    "dynamic": [{"x": 96, "y": 160, "w": 128, "h": 16}],
    "bg_index": 0,
    "wide_png": "screens/title_wide.png"
  }
}
```

**Identity: evaluation.**

| Option | Verdict |
|---|---|
| Hash of the whole indexed frame | Breaks as soon as anything on the screen moves, blinks or is typed (high-score names, shop prices, scrolling credits, animated parts of a screen). |
| Screen id from game state (variable or PC hook per screen) | Stable, but needs reverse engineering of every screen of every build (the ST has no map, the shop and ending live in a separate module, the CDTV pages are loaded overlays). Reserved for later as an optional `state` selector where a build already identifies a screen. |
| **Block signature (chosen)**: hashes of up to 64 stable blocks (default 16 x 16) of the indexed frame; the screen matches when at least `min` blocks match at their positions | Tolerates dynamic and animated parts, ignores palette and fades (indices are matched, not colours), works on every build with an indexed frame, and contains no original pixels (hash only). |

The signature is written by the dump tool: sample every 5th frame of the screen's lifetime, keep grid blocks identical in at least
90 % of samples with at least two distinct indices (flat blocks identify nothing), choose up to 24 by greedy farthest-point spread,
`min = ceil(0.7 * n)`. A screen with fewer than 6 stable blocks (a scrolling credits roll) is reported as "not matchable" and left
out. The dump also checks that the new signature does not match another dumped screen or any sampled gameplay frame, and warns if
it does. A screen's lifetime is the run of presented non-gameplay frames in which the stable blocks persist; a cut (more than 40 %
of grid blocks change at once) starts the next screen.

**Matching at run time.** For each presented non-gameplay frame, for each registered screen (an index by the first block's hash
keeps this to a few lookups), compare up to 64 blocks of 256 bytes at fixed positions: well under 0.1 ms. The last match is cached
while at least `min - 2` blocks still match, so a screen does not flicker when a block changes briefly.

**Rendering.** The picture is `320 * scale` by `200 * scale` (the window area; `anchor` lets it overhang), composited over the
faithful window at the canvas scale, below host overlays. Palette handling:

- `ref_palette`: the full-brightness palette the dump recorded (`#rgb`, 2-32 entries). With `fade: "scale"` (default) the picture is
  multiplied per RGB channel by `sum(current palette over used indices) / sum(ref_palette over the same indices)`, clamped to
  0..1, so the game's fade-ins and fade-outs fade the replacement too. Without `ref_palette`, or with `fade: "none"`, the picture
  stays at full brightness while the game fades.
- Per-scanline palette effects of the original (copper gradients) are baked into the artwork.

`dynamic` rectangles (Amiga pixels, in the window) mark parts the game still draws itself: inside them, pixels of the original frame
whose index is not `bg_index` (default 0) are drawn, scaled, over the replacement; elsewhere the replacement covers everything. This
keeps live text (scores, names, shop lines) while the artwork changes. `wide_png` (any width, `wide_scale` default the pack scale)
is used instead of black sides when widescreen is on; without it the sides stay black as today.

Dump: `--dump-assets` also writes `screens/<name>.png` (the original at the brightest palette seen, for the author to redo; never to
distribute), the signature, `ref_palette` and detected dynamic rectangles (blocks that changed during the lifetime), with names
`screen_<first 8 hex of FNV(signature)>` that authors rename.

### 4.5 Status panel and HUD (`panel`)

```json
"panel": {"scale": 4, "cell_w": 8, "max_span": 6,
  "background": {"png": "panel/bg.png"},
  "cells": {"0123456789abcdef-1": {"png": "panel/digit3.png"},
            "89abcdef01234567-3": {"png": "panel/lives_icon.png", "anchor": [0, -2]}}}
```

The panel (window lines 192-199) is written in place by the CPU as finished glyph bytes. Two designs were weighed:

| Option | Verdict |
|---|---|
| Variable-bound panel: layout + font + the score/lives/energy/credits variables (known per build: score `$C92`/`$C96`, energy `$CC6`, ships `$CCA`, credits `$CF0`; CDTV `+6`; ST the disk addresses) | Needs a glyph-to-character mapping and the geometry of the weapon icons and energy bar, per build; the HUD cannot show what the engine does not hold in a variable. **Deferred to 2.x** (`panel.bindings` is reserved). |
| **Cell keys (chosen)**: split the panel into byte-aligned cells, key each cell run by the content of the indexed pixels as drawn | Works on all builds and on the ST (it needs only the indexed frame), no per-build RE, a font replacement is ~40 small pictures. |

Cells are `cell_w` x 8 pixels (default 8; the grid is byte-aligned because the game writes bytes). A run of `n` cells (1 to
`max_span`, default 6) at a column has key `<block hash of the 8n x 8 region>-<n>`. At each column the presenter tries the longest
span first; a matched run is replaced and consumed; unmatched cells show the original, upscaled with the HD-panel scaler when that is
on. `background` (`320 * scale` x `8 * scale`) is drawn first so replaced cells can be transparent; replacement pictures may overhang
the strip through `anchor` (drawn last, over the playfield picture). The strip is re-evaluated only when it changes (its raw bytes
hash). The per-build `PanelLayout {first_line, lines, cell_w, max_span}` goes in the capture layout table (disk and CDTV: 192, 8, 8, 6
**(measure)**; ST: the same lines of its 200-line picture **(measure)**). Energy bar segments and weapon icons are cells
like digits. The dump writes `panel/<key>.png` for every distinct cell run seen.

### 4.6 Stars and hit flashes

- **`stars`**: `shape` `pixel` (default, today's behaviour) or `round` (an anti-aliased disc of `size` Amiga pixels, `softness` 0-1),
  coloured by the star's palette index, so `palettes` layer `stars` still applies. The captured star writes are replayed as now (the
  game's own tests still run on the original pixels); only the drawn shape changes. Amiga family only.
- **Hit flashes.** Today a flashed bob is a partial blit group (fewer than four planes, other minterms) and always replays as the
  original. v2: an entry may carry `flash_png` (the replacement picture drawn as a white silhouette, or any flash look), used when
  the flash variant of that graphic is drawn. Without it the flash is **derived**: the replacement picture's alpha, blended to white
  at 70 %. The flash is tied to its graphic through the blit source pointer (the `SourceTracker` already follows sources) **(measure:
  if the flash blits cannot be tied to the normal graphic reliably, the derived flash is dropped and only an explicit `flash_png`
  keyed by a mask-shape hash `f:<hash>-<w>x<h>` is supported)**.

### 4.7 Order of composition (per presented frame)

1. gameplay: the replayed playfield as today (backgrounds and `far`, tiles, objects, stars with their replacements and palette
   overrides);
2. the panel pass (HD-panel upscale, then `panel` cells);
3. non-gameplay: the screens pass (match, then composite) instead of the plain faithful frame;
4. the widescreen surround (screens may bring `wide_png`);
5. host overlays.

Pause and disk loads: the presenter shows the faithful frame when a step is held longer than `hold_frames` (game pause, loads). That
makes every replaced graphic revert to the original for the length of a pause today **(measure; likely an existing v1 limitation)**:
WS2 keeps the last composed picture instead when the hold is a pause, and the screens pass covers the loading screens.

### 4.8 Author tools for graphics

`--dump-assets DIR` (extended: `graphics`, `tiles`, `far`, `far_full/` per level, `flashes/`, `screens/`, `panel/`, and a v2
`manifest.json`); a dump is itself a valid pack at scale 1 and loads to the original picture exactly (test `modern_upgrade`
extended to screens and panel). `tools/assetpack/slice.py` cuts dump sprites into an atlas plus frame map and `pack_atlas.py` does the
reverse; `contact_sheet.py` renders a dump or pack as a contact sheet for review; `dump_all.sh` runs the committed replay scripts of a
build into one dump directory so every level, boss, shop and the ending are covered.

## 5. Audio

### 5.1 Architecture

Pack audio changes the **output mix** only, in emulated time:

- Paula, the CD player and the ST YM generator keep running exactly as now (DMA, interrupts, timing, chip RAM, statistics). A pack
  never writes machine state. This is the rule `--protect-music`'s mirror voices follow, and it keeps replays and the accurate timing
  model intact.
- A **pack voice** is started by an event the machine already has (a Paula DMA start, a CD-play cue) at an exact Paula output frame
  and rendered into the stream the machine produced for that frame, in the same stage that renders the CD player
  (`CdtvMusic::after_frame`). Positions use exact rational arithmetic as `CdPlayer` does, and the resampler is built with
  `FP_CONTRACT` off, so Debug and Release builds hash identically.
- The original voice is muted **in the mix** (a per-channel gate in Paula's output stage, like the source gains), never in the
  channel state.

Rejected: replacing the sample bytes in chip RAM or changing `AUDxLEN`/`AUDxPER` (changes machine state, DMA slot accounting and
interrupt timing, and breaks the loop-after-one-shot pattern the driver depends on); a host audio thread that mixes in wall-clock time
(not reproducible; no `--audio-hash`).

### 5.2 Sound effects and speech (`audio.sfx`, `audio.speech`)

```json
"audio": {"sfx": {"0123456789abcdef-640": {
   "file": "sfx/fire.ogg", "ref_period": 214, "gain_db": -3,
   "pan": "channel", "pitch": "follow", "original": "mute",
   "cut": "channel", "fade_out_ms": 5, "loop": false}}}
```

**Identity.** The key is the sample key (section 3.4): the bytes the channel reads from the latched `AUDxLC` for `AUDxLEN` words, hashed
when the channel starts. Hashing is only done when some replacement has that byte length (a set lookup), so a pack costs nothing for
the many starts it does not replace. Samples are content, so the same sound has the same key on every build that has the same
bytes; where a build's sample differs (the Power Pack's samples are lower-precision) the author lists the other key in `keys`. Effect
identification by game command word (`$414`/`$418` writes) was rejected as a primary key: a command can start several samples, and the
numbering is per build.

**Which starts count.**

- Disk builds: starts whose sample pointer was written by the effect code (the CDTV layer's `AudioSource::Effect` tag, from the PC range
  `$1B26A-$1B6CF`); music instrument starts are never replaced (section 5.3).
- CDTV release: every start (the program has no tracker); `speech` when the pointer lies in the speech buffer (`$B645C` range) and
  `sfx` otherwise. A `speech` entry may instead name the disc file the clip came from (`"cd0_file": "SPEECH63"`; the file trap that
  loaded the buffer remembers the name), which is language-independent for authors who work from the disc's numbering.
- ST: none in 2.0 (section 7).

**Playback (the host voice).**

- *Pitch.* `ref_period` is the Paula period at which the replacement file sounds like the original sample played normally (a dump
  writes it, with the original sample resampled to `colour clock / ref_period` Hz, so a file redone at the same duration matches). The
  voice's speed is `ref_period / current AUDxPER` and is re-evaluated at every period change the game makes, so pitch bends and
  frequency sweeps carry over; `"pitch": "fixed"` plays at the file's own rate.
- *Volume.* `AUDxVOL` (0-64) is followed through the voice's life as a multiplier `vol / 64` (the game fades channels by writing volume),
  then `gain_db`. A full-scale file maps to Paula full scale at the configured output gain, as the CD player does.
- *Pan.* Mono files go to the side of the channel (AUD0, AUD3 left; AUD1, AUD2 right, with `stereo_separation`) when `pan` is
  `"channel"`, or a constant-power position -1 (left) to 1 (right). Stereo files play as they are, scaled by `gain_db`.
- *Original voice.* `"original": "mute"` (default) removes the channel's contribution from the mix from the start to the channel's next DMA
  start or DMA off; `"mix:-12"` keeps it at -12 dB under the replacement; `"mix"` keeps it at 0 dB.
- *End and cut.* The voice ends when its file ends, or, with `"cut": "channel"` (default), when the same channel gets a new DMA start or
  a DMA stop (the game's own effect priority: a newer sound takes the channel), with a `fade_out_ms` (default 5) fade; `"cut": "never"`
  lets it ring out. `"loop": true` loops the file while the original channel keeps playing the same sample (its buffer reloads) and
  fades out when the channel changes. Voices are polyphonic up to 16.
- *Category.* `sfx` and `speech` have their own volumes (`--sfx-volume`, new `--speech-volume`, `settings.cfg` `speech_volume`); the
  final mix saturates to int16 as today.

Paula needs three additions, all output-side: an *event sink* (DMA start with channel, latched `LC`/`LEN`, current `PER`/`VOL`, source
tag and output frame; DMA stop; period and volume writes, timestamped), a *replace decision* callback consulted at DMA start (returns
"mute until next start" and a voice id), and per-channel mute ranges applied when Paula generates the frames. With no sink installed
Paula is bit-identical to today.

### 5.3 Music (`audio.music`)

```json
"music": {"defaults": {"crossfade_ms": 250, "loop_mode": "pack"},
  "slots": {"title":  {"file": "music/title.ogg", "loop": {"start_s": 4.0, "end_s": 80.5}},
            "level3": {"intro": "music/l3_intro.flac", "file": "music/l3_loop.flac", "loop": true},
            "level":  {"file": "music/level.ogg", "loop": true},
            "cd5":    {"file": "music/c5.flac", "gain_db": -2},
            "level4": {"file": null, "mute": true}}}
```

**Slots and cues, per game.**

| Game | Slots | Cue source (exists) |
|---|---|---|
| disk | `title`, `level1`-`level5`, `level` (fallback for any level) | the CDTV layer's PC traps on the tracker driver: `music_init` called from `$8230` (title) and at the in-game starts `$7896 $7C50 $7CDA $7D8C` (level = `$E04`), `music_stop` at `$19FBA` (docs/re-notes/cdtv-music-map.md). The disk game has one tune; replacing a slot silences the tracker and plays the pack track. Shop, ending and bosses have no cue (the tune simply continues or the module plays its own), so no slot. |
| cdtv | `cd2`-`cd13` by CD track, aliases `title` (track 2 by default) and `level1`-`level5` (the track the game plays at the cue while `$E0A` is that level), `level` | the game's own CD calls (the `cd_play` trap at `$3A924`, hunk offset `$3A524`, and its siblings). The CD track the game asks for depends on the user's SELECT TITLES choice, so `cdN` is the faithful key; the aliases follow the default table `{2,3,5,2,3,5}`. |
| st | `title`, `level1`-`level5`, `level` | **phase 2**: the ST music driver's start/stop entry points are not yet mapped (S0 left the tune count unknown); WS4 maps them first. YM effects share the chip with the music, so muting the YM for a pack track also silences YM effects unless the writer tag can separate them (`ym_mute: all` until then). |

**Behaviour.** A cue for slot X with an entry for X (exact slot first, then alias, then `level`): the original music is silenced
(disk: the tracker's source gain to 0 as `MusicSource::Cdtv` does; CDTV: the CD player's voice not started) and the pack track starts at
the cue's output frame; with `mute: true` and no file the music is simply silent. `file` may be `null` only with `mute`. Order of
precedence on the disk build: pack slot, then an imported CD track (`--cdtv`), then the tracker.

- *Loop.* `loop` is `true` (whole file), `false`, or `{start_s, end_s?}` (seconds in the file after `intro` is played; `end_s` defaults
  to the file end). Fallbacks when the manifest says nothing: WAV `smpl` chunk loop, then Vorbis/FLAC comments `LOOPSTART` and
  `LOOPLENGTH` (in samples). `loop_mode`: `"pack"` (default when a loop is declared) loops seamlessly by the pack's own points;
  `"game"` keeps the original rule (the CDTV program's once-a-second `CheckIO` poll restarts a finished track, up to a second late,
  intro included; the disk tracker loops by itself).
- *Transitions.* `crossfade_ms` (default 0, 0-5000) between tracks and at seamless loops (equal-power, as `CdPlayer`), `fade_in_ms`,
  `fade_out_ms`; `retrigger`: `"restart"` (default: a cue for the slot that is already playing restarts it, as the game's driver does when
  a level restarts after a death) or `"continue"`.
- *Pause.* The game's pause handling is reused (`--cd-pause keep|duck|stop` semantics apply to pack tracks); `mute` applies at the host.
- Implementation: `CdPlayer`'s reader generalises to a `StreamSource` interface (CD track, or decoder over a pack file) behind the same voice
  engine, so loops, crossfades, volume ramps and determinism are shared, not rewritten.

### 5.4 Formats and decoders

| Format | Decoder | Licence | Notes |
|---|---|---|---|
| WAV | own parser (`wav_writer.cpp` already writes them) | project | PCM 8 (unsigned), 16, 24, 32 bit, float 32/64; 1-2 channels; `RF64`, `WAVE_FORMAT_EXTENSIBLE` with more than 2 channels refused; chunk sizes checked; `smpl` loop read |
| FLAC | **dr_flac** (single header) | public domain or MIT-0 | 1-2 channels, 8-32 bit; streaming over a memory block or a file |
| Ogg Vorbis | **stb_vorbis** (single file, version 1.22 or later) | public domain or MIT | 1-2 channels; historically memory-unsafe: only called after the size checks above, a libFuzzer target and ASan/UBSan runs are part of WS7 |

Rejected: MP3 (a dr_mp3 header exists and the patents have expired, but the format adds nothing for this use and has no reliable
gapless or loop semantics), Opus (needs libopus or a large decoder), anything needing a system library (the Linux archive must stay
dependency-free beyond libc). Vendored sources go under `vendor/audio/` with their licence texts, and into `THIRD-PARTY-NOTICES.md`
with the version recorded. Channel counts above 2 are refused with a clear message; sample rates are resampled at voice start with the
existing Kaiser-sinc kernel; a clip's resampled copy is cached per host rate.

Memory: sfx and speech clips decode at load into int16 (limits in 3.5). Music streams: files in a ZIP should be **stored** (the
validator warns about deflated audio); a deflated music member is inflated into memory once (counted against the audio budget).

### 5.5 `--dump-audio DIR`

Records, during a run (window or headless, with or without an input script), every distinct effect and speech sample at DMA start and
every music cue, and writes:

```
DIR/sfx/<key>.wav        the ORIGINAL sample, mono 16-bit at round(colour clock / ref_period) Hz  (author reference: never to distribute)
DIR/speech/<key>.wav     (CDTV)
DIR/music_cues.txt       frame, slot, game state, caller: the cue log (slot discovery)
DIR/manifest.json        a valid v2 pack: audio.sfx / audio.speech entries pointing at those files, plus dump-only fields:
                         bytes, uses, first_frame, channels (bitmask), periods [min, max], peak, source (effect|speech), cd0_file
```

Dump and `--dump-assets` may write the same directory: each merges its sections into an existing v2 manifest. Loaded unchanged, the dump
plays the original samples through the host-voice path at the right pitch and level, without the A500 output filter (the A500 low-pass is
part of Paula's mix, which pack voices bypass: authors supply the final sound; `audio_filter` does not apply to pack voices). A test
compares each dumped clip with the sample read independently from the chip RAM.

### 5.6 Determinism and the switches

- Packs off (no pack, `--faithful`, or the audio toggle off): audio hashes are bit-identical to today's goldens.
- Packs on: deterministic, so `--audio-hash` and `--audio-out` are repeatable; a synthetic pack (generated in the tests) has its own
  recorded hashes.
- `--modern audio|noaudio`, `audio_upgrade =`, the proposed hotkey, and F1 / `--faithful` (overrides all pack effects). `--asset-pack`
  keeps implying `upgrade` as in v1 and implies audio when the pack has an audio section.
- Latency: pack voices are mixed in the same frame blocks as the machine's own audio; no added latency and no extra underrun risk beyond
  decode and resample time, which happens at voice start (sfx are pre-decoded; the first music buffer is decoded at the cue, so tracks are
  opened when a slot is first resolved at load: the first buffer of each slot is pre-decoded).

## 6. Loader, selection, validation

**Loader.** `assets.cpp` splits: manifest and layering (`pack_manifest.*`), ZIP (`pack_zip.*`, central directory first, lazy extraction,
case-fold collision and ratio checks), paths (`pack_paths.*`), limits (`pack_limits.h`), PNG and resampling stay; the audio decoders and
voices live under `runtime/audio/`. The library is a read-only snapshot after load, shared with the presenter and the audio stage.
Everything is checked from headers before allocation; decode errors skip the entry and are reported.

**Selection.**

- `--asset-pack PATH` (repeatable) as today; `settings.cfg` `asset_packs = A;B` as today.
- **New:** `<data dir>/packs/` is scanned at start; each child directory or `.zip` with a manifest is a *discovered* pack. Discovered packs
  are enabled by `packs_enabled = id1;id2` in `settings.cfg` (ids from `id`, else the folder or file stem). `--list-asset-packs` prints
  every discovered pack with id, name, version, author, licence, games, provides, enabled state and any problem, and exits; `--json` for
  front ends.
- Keys: Home / Cmd+U (graphics and screens, exists), a new key for audio (proposed Page Up / Cmd+J), F1 / Cmd+1 overrides.
- No in-game menu: the project has no settings UI beyond host hotkeys and `settings.cfg`; one would be a separate feature.

**Validator.** `x2recomp --check-asset-pack PATH [--game disk|cdtv|st] [--build TAG] [--against DUMPDIR] [--full] [--strict] [--json]` and
`tools/assetpack/validate.py` (pure Python, same rules except the run-time scoping, no game data needed):

- schema (the JSON schema above); version, id and `requires` syntax; unknown fields (warning); missing `license` (warning, error with `--strict`);
- every referenced file exists and its header decodes; with `--full`, fully decoded (PNG CRCs, audio frame counts and caps);
- picture sizes: a sprite or tile picture must be `w*scale` x `h*scale` for the `w`x`h` in its key; far strips `320*scale` wide and a
  multiple of `scale` high; atlas rects inside the picture and matching their keys; backgrounds `320*scale` x `ring_lines*scale`; panel cells
  `8n*scale` x `8*scale` (anchored pictures may differ: warning only);
- audio: format, channels, rate, duration within limits, clipping (peak above 0 dBFS), loop points inside the file, `ref_period` range, deflated
  audio in a ZIP (warning);
- key syntax, duplicates across sections, scoping consistency (`games`/`builds` values), dependency order;
- `--against DUMPDIR`: **coverage** (how many of the dump's keys the pack replaces, per kind), **typos** (pack keys the dump never saw), and
  **unchanged copies** (a pack picture or clip byte-identical to the dump's original, with a warning that a dump must not be redistributed);
- the memory estimate (decoded graphics and audio) against the limits.

Exit codes: 0 valid (warnings allowed unless `--strict`), 1 warnings with `--strict`, 2 the pack is unusable or has invalid entries, 3 the
path cannot be read. The report is plain text, one line per finding in the loader's message format, then a summary; `--json` gives the same
as an array.

## 7. Compatibility matrix

Y = supported in 2.0, P = planned phase 2 (design reserved, needs analysis named in the cell), - = not possible or not meaningful.

| Feature | disk (canonical, retail 0297/2874, cracks) | Power Pack | 1762 | PDos v5 | CDTV | Atari ST |
|---|---|---|---|---|---|---|
| sprites, tiles (`graphics`, `atlases`) | Y | Y | Y | Y (**measure**: confirm the disk capture layout) | Y (108 of 109 level-1 keys equal the disk build's) | - (CPU-drawn, no blit capture) |
| `far`, `backgrounds` | Y | Y | Y | Y | Y | - |
| `stars`, hit flashes | Y | Y | Y | Y | Y | - |
| `palettes` | Y | Y | Y | Y | Y | - (CPU-drawn; palette per layer needs the capture) |
| `screens` | Y | Y | Y | Y | Y | Y (indexed frame from the Shifter) |
| `panel` cells | Y | Y | Y | Y | Y | Y (same lines **(measure)**) |
| sfx replacement | Y | Y (own sample keys) | Y | Y | Y (plus `speech`) | P (YM effects are register patterns; the front end's Timer A digi clips could be keyed by sample address: needs S0-style analysis) |
| music slots | Y (tracker cues) | Y | Y | Y | Y (CD track cues) | P (music start/stop entries unmapped) |
| `speech` | - (none in the game) | - | - | - | Y | - |
| validator, dump tools, discovery | Y | Y | Y | Y | Y | Y (screens, panel; audio later) |

Not possible and why: tracker **instrument** replacement (instruments loop inside the sample and restart per note with the note's period;
a host voice cannot reproduce the loop semantics without modelling the driver); replacing the original tune per *note*; a re-drawn
panel from the game's variables (2.x); graphics of CPU-drawn builds beyond screens and the panel; anything that changes gameplay or
timing; Hires screens (none known).

Per-build tables the implementation adds (all next to the existing `CaptureLayout`, one entry per game family, build overrides only where
the build moves an address):

- `CaptureLayout`: `kLevel` variable address; `far_ring_lines` and scroll sign **(measure)**; `panel {first_line, lines, cell_w, max_span}`;
  the `window` rectangle of the indexed frame.
- `ScreenGeometry` per game: indexed-frame provider (Amiga `Video`, ST Shifter), window origin, size, colour index range.
- `AudioLayout` per game: the effect-writer PC range (disk `$1B26A-$1B6CF`), the speech buffer range (CDTV `$B645C`), the channel to side map,
  the cue table (disk: the music_init call-site list and `$E04` level word; CDTV: the CD trap addresses, `$3A8F2` track variable and the level word `$E0A`;
  ST: to be mapped), and `kMusicShadow` where relevant.

## 8. Documentation plan

Deliverables (WS6), written alongside the code and finalised last:

1. `docs/ASSET-PACKS.md` rewritten as the guide: overview (what a pack can change, per release, the legal rule), quick start, pack layout,
   loading and layering, graphics (sprites, tiles, sheets, backgrounds, screens, panel and HUD, stars and flashes) each with a worked example,
   audio (sfx, speech, music, loop points, formats) each with a worked example, the tools (dump, slice and pack, contact sheet, validate,
   discovery, preview with `--modern-shots` and `--audio-out`), troubleshooting (a key never matches, picture too small, black screens during
   fades, silent music, clipping, ZIP deflated audio), limits, and the compatibility matrix.
2. `docs/ASSET-PACKS-REFERENCE.md`: every manifest field, type, default, limit and error, kept in step with the schema (a test compares the
   field list in the doc with the schema).
3. Tutorials under `docs/asset-packs/`: *make a 2x sprite pack*, *replace the title screen*, *replace the music*, *replace a sound effect*,
   *a complete HUD font*.
4. Legal section (in the guide and the README): only your own artwork and audio, or content licensed to you for this; no game-derived
   material (dumps, signatures built from your copy are for you); the project ships no packs; use the `license` and `credits` fields.
5. `examples/asset-pack-skeleton/`: a manifest with every section and placeholder keys (the schema example), folder structure, README, and
   procedurally generated placeholder pictures and tones (no game content). It validates with warnings only (the keys match nothing).
6. `tools/assetpack/make_demo_pack.py`: builds a runnable demo pack **from the user's own dump** (it needs a dump to know the real keys):
   recolours and box-scales sprites and tiles, replaces screens with generated gradients that keep `dynamic` rectangles, redraws panel cells
   in a flat font, swaps every sfx for a synthesised chirp of the same duration and `ref_period`, and gives each music slot a synthesised
   loop. It exercises every feature, is used by the data tests, and is the quickest check that a build is pack-ready.
7. README: "Asset packs" section updated; `CHANGELOG` entry; `docs/PORTING.md` (what a new platform needs for packs: an indexed frame, a capture layout,
   an audio layout); `THIRD-PARTY-NOTICES.md` (dr_flac, stb_vorbis); `docs/BUILDING.md` options.

## 9. Test strategy

- **Synthetic first, no game data in the repo.** C++ test helpers generate packs on the fly (PNGs through the existing encoder helpers, WAVs by
  a small writer, FLAC/OGG fixtures from tiny hand-built streams or encoded once offline and checked in as bytes of a *synthetic* tone,
  which is not game content); synthetic `Step`s as the 414-check modernisation test does; synthetic indexed frames for screens and panel;
  synthetic Paula DMA sequences for sfx.
- **v1 compatibility:** every existing v1 test (`modern_upgrade`, `modern_*`) unchanged; the docs example pack loads identically.
- **Goldens:** pack-off frame and audio hashes identical for every build's corpus (the gate); pack-on synthetic hashes recorded; two runs identical.
- **Data tests (exit 77 without assets):** dump a build (`--dump-assets`, `--dump-audio` over its replay scripts), build the demo pack, replay with it
  and check: a dump loaded unchanged reproduces the original picture exactly (graphics, screens, panel, backgrounds); the demo pack changes the
  expected regions; the audio dump equals chip RAM samples; pack off equals the golden. Disk, CDTV and ST (screens, panel).
- **Fuzz and sanitizers:** libFuzzer targets for the manifest parser, ZIP reader, PNG, WAV, FLAC and Vorbis decoders (smoke minutes per PR, long runs
  nightly), ASan/UBSan runs of the pack tests; allocation-bomb tests (huge declared sizes, zip ratio, 255-revolution-style aliasing for atlases:
  frames pointing at overlapping rects are fine but the entry cap and memory cap hold).
- **Windows:** MSVC-safe filesystem use (non-throwing overloads, close files before removal), UTF-8 paths; ZIP names with backslashes in old
  Windows-made zips are normalised by the loader with a warning.
- **Performance gates:** `level1_flood` interpolated at scale 3 with a full demo pack: at most 5 % over no pack; audio with a pack of 200 sfx and two
  streaming tracks: at most 3 %.

## 10. Work breakdown

Estimates are focused person-days; "concurrent" means no shared files once WS1a has landed.

| WS | Content | Owns (files) | Depends on | Est. | Gate |
|---|---|---|---|---|---|
| **WS1a** contract | `runtime/pack/pack_api.h` (the read-only interfaces the other workstreams consume: `PackLibrary`, screen/panel/background/clip/music sets), all new CLI flags and `settings.cfg` keys as plumbing (`--check-asset-pack`, `--list-asset-packs`, `--dump-audio`, `--modern audio`, `--speech-volume`, `audio_upgrade`, `packs_enabled`, `speech_volume`) so later work never touches `main.cpp`/`persist.*` again; `kLevel` and the new `CaptureLayout`/`PanelLayout`/`AudioLayout` fields with their per-build values; the schema (done in this design) | `runtime/pack/*`, `runtime/main.cpp` (options), `runtime/persist.*`, `runtime/modern/layout.*` | this note | 2 | builds, all tests unchanged |
| **WS1b** format | manifest v2 parser, scoping, layering, `requires`, discovery, atlas expansion, ZIP lazy reader and limits, path rules, `--check-asset-pack`, `--list-asset-packs`, `tools/assetpack/validate.py`, v1 compatibility | `runtime/modern/assets.*` (split into `runtime/pack/*`), `tests/modern/*`, `tools/assetpack/validate.py` | WS1a | 6 | v1 identical; schema and validator tests; zip/path tests |
| **WS2** graphics | indexed-frame providers (Amiga, ST), `ScreenMatcher` and compositing with fades and `dynamic`, panel cells, backgrounds (ring binding), stars, hit flashes, pause-hold fix, ST present hook, dump extensions (`screens/`, `panel/`, `far_full/`, `flashes/`), per-build tables | `runtime/modern/{interp,present,draw_capture,layout}.*`, new `runtime/modern/{screens,panel,background}.cpp`, `runtime/gfx/video.*` (index frame), `runtime/st/st_run.*` (present hook only) | WS1a | 12 | dump-as-pack exact for every kind; goldens unchanged; 5 % budget |
| **WS3a** decoders | vendor dr_flac and stb_vorbis (notices), WAV parser, `AudioDecoder` interface, resampler reuse, limits | `vendor/audio/*`, `runtime/audio/decode.*`, `THIRD-PARTY-NOTICES.md`, `CMakeLists.txt` | WS1a | 3 | decoder unit tests, fuzz smoke |
| **WS3b** sfx | Paula event sink, replace decision, mute ranges, pack voice mixer (pitch/volume follow, pan, cut, loop, polyphony), speech classification and `cd0_file`, categories and `--speech-volume`, `--dump-audio` | `runtime/audio/paula.*`, new `runtime/audio/pack_audio.*`, `runtime/cdtv/cdtv_game.*` (file-name memory only) | WS3a, WS1b (clip set) | 8 | Paula bit-identical without a sink; dump equals chip RAM samples; determinism |
| **WS4** music | `StreamSource` generalisation of `CdPlayer`, slot resolution per game (disk cues, CDTV tracks and aliases), loop/intro/crossfade/retrigger, tracker mute (`MusicSource::Pack`), ST cue mapping analysis (notes only unless the entries are found) | `runtime/cdtv/{cd_player,cdtv_music}.*`, new `runtime/audio/music_slots.*` | WS3a, WS1b | 7 | CD goldens unchanged; pack slot tests; seamless loop test |
| **WS5a/5b** tools | `slice.py`, `pack_atlas.py`, `contact_sheet.py`, `dump_all.sh`, `make_skeleton.py` (5a, needs only the format); `make_demo_pack.py` (5b, needs the dump formats of WS2 and WS3b) | `tools/assetpack/*`, `examples/asset-pack-skeleton/` | 5a: WS1a; 5b: WS2, WS3b | 5 | tools tests; demo pack validates and loads |
| **WS6** docs | guide, reference, tutorials, legal text, README and changelog | `docs/*`, `examples/*` | spec now; final text after WS2-4 | 5 | every schema field documented; examples validate |
| **WS7** integration | synthetic pack helpers, goldens, data tests for disk/CDTV/ST, fuzz targets and nightly job, MSVC pass, performance gates | `tests/*`, `.github/workflows/*` (a fuzz job) | WS1b, WS2, WS3b, WS4 | 5 | all gates in section 9 |

Total about 53 person-days (42 with less documentation overhead, 60 with slips in the measure items).

**Concurrency.** After WS1a lands: WS1b, WS2, WS3a and WS5a run in parallel (disjoint files). WS3b and WS4 start when WS3a (decoder
interface) and WS1b (clip and music sets in the library) are merged; WS3b and WS4 are disjoint (`paula.*` versus `cdtv/*`, `music_slots.*`).
WS5b waits for the dump formats. WS7 grows with each merge and closes after WS4. WS6 starts after WS1a and is finalised last.
**Merge order:** WS1a, then WS1b, WS2, WS3a and WS5a in any order, then WS3b and WS4, then WS5b, WS7, and WS6 final. With four workers about
five to six weeks of wall-clock time.

**Measure items first** (each is a half-day at the start of its workstream, results recorded in the workstream's note): far-layer ring height
and scroll sign (WS2); panel lines and cell grid on the CDTV release and the ST (WS2); flash-blit to graphic correlation (WS2); whether the
pause hold reverts replaced graphics today (WS2); ST music start/stop entries and tune count (WS4); hires screens, if any (WS2).

## Implementation status

- **WS1a (API contract and plumbing): landed.** `runtime/pack/pack_api.h` fixes the interfaces the other workstreams build on: the game scope, the
  key and hash helpers (FNV-1a 64 with reference vectors in `tests/pack_tests.cpp`), the entry types for graphics, screens, panel, audio and music,
  the `PackReport` message format, the abstract `PackLibrary` with its null implementation, and `AssetPackSettings` with the global accessors.
  `runtime/modern/layout.h` gained the per-game screen geometry, audio layout and the capture-layout fields (an ST stub with every address unknown).
  The flags `--dump-audio`, `--check-asset-pack` (with `--against`, `--build`, `--full`, `--strict`, `--json`), `--list-asset-packs`,
  `--speech-volume` and `--modern audio|noaudio` parse and the settings keys `audio_upgrade`, `packs_enabled` and `speech_volume` persist; the three
  entry points print "not implemented yet" and exit 3 until WS1b and WS3b replace them. No existing behaviour changed.
- **WS2a (screens and the status panel): landed** (`docs/re-notes/asset-packs-screens.md` has the measurements, the design as built and the evidence).
  The Amiga video model and the ST shifter can record an indexed frame (`modern/indexed.h`); `ScreenMatcher`, the fade fit, the compositor, the dynamic rectangles, the
  panel's cell runs and the ST present hook are in `modern/screens.*`, the version 2 dump of screens and panel cells in `modern/screen_dump.*`, and an in-memory
  library for tests in `modern/screen_pack.*` (packs are loaded by WS1b's loader, directory and ZIP). Measure items: the panel is window lines 192-199 in 8-pixel cells
  on the disk, CDTV and ST builds (`PanelLayout{192, 8, 8, 6}` everywhere), no hires screens exist, and the games **fade by subtraction**, not by scaling. Deviations from sections 4.4
  and 4.5 as written: the fade fits a subtractive model as well as the multiplier; `ref_palette` takes `#rrggbb` and the `panel` section has `ref_palette` and `fade`; the dump
  looks at every frame, signatures contain only blocks that never changed (and up to 64 blocks, with the ones that tell the screen from other frames of the run added), and
  its pictures hold the dynamic parts as background with the unmasked original beside them.
- **WS1b (format, loader, validator): landed.** Manifest v2 parser, scoping, layering per key, `requires`, atlas expansion, `keys` aliases, lazy ZIP reader with the
  section 3.5 limits and the path rules, discovery (`<data dir>/packs/`, `packs_enabled`), `PackStack` (the real `PackLibrary`), `--check-asset-pack`, `--list-asset-packs` and
  `tools/assetpack/validate.py`. `modern::AssetLibrary` is now an adapter over the stack; every version 1 test runs unchanged. Graphics, far lines and palettes of any pack
  (including atlas frames and aliases) are applied by the interpolator as before; screens, panel, backgrounds, stars, flashes and all audio are **loaded and validated but not yet
  applied** (WS2, WS3b, WS4). Sound files are read through an `AudioCodec` hook that WS3b registers. How it is built, the decisions taken where this note left room (`f:` flash keys,
  panel cell schema, stars `size_px`, ...) and the additions to `pack_api.h` are in [`asset-packs-loader.md`](asset-packs-loader.md).
- **WS4 (music slots): landed** (`docs/re-notes/asset-packs-music.md` has the cue maps, the engine as built and the evidence). `CdPlayer` plays any `StreamSpec`
  (per-voice source rate and kernel, gain, fades, wrapped sample-exact loops, crossfaded loops) and the CD is one of its sources, with every CD hash unchanged;
  `audio/music_slots.*` turns the library's slots into decoder streams (first buffer pre-decoded, intro plus body, loop points from the manifest or the file); `CdtvMusic`
  resolves the slot at the disk and CDTV cues (the CDTV callers are classified as title, level or other; `cdN` is exact), mutes the tracker in the mix from the trap, keeps
  `retrigger: continue` across the CDTV game's stop-then-play, and follows F1 and the audio toggle. Deviations from section 5.3: `mute` wins over a `file` left in the slot; with no
  loop declared and none in the file the original's rule applies (`loop_mode: "game"` behaviour) rather than a one-shot; a crossfaded loop restarts at `loop_start`, overlapping the
  end; `loop` points are seconds in the body file (the intro is not counted). **Not done:** the Atari ST (the music driver's entries were not mapped; decision U5 keeps ST audio out of
  this release), and slots for the shop, bosses and ending (no cue exists).
- **WS3b (sound effects, speech and `--dump-audio`): landed** (`docs/re-notes/asset-packs-sfx.md` has the measurements and the design as built). Paula has an event
  sink and per-channel gates (bit-identical without them); `runtime/audio/pack_audio.*` classifies starts (disk: the effect code's AUDxLC writes, tagged per build;
  CDTV: every start, speech by the buffer `$B645C-$D0973` or by disc file name), mixes pack voices at exact Paula frames and ends them on the Faithful switch;
  `runtime/audio/audio_dump.*` is `--dump-audio`. Measure items: effects are one buffer then a park loop, pairs are two channel starts, the 1762 compilation's driver
  is `$5DE` lower, PDos's effect code is not mapped, the CDTV speech buffer ends at `$D0973`. Deviations from section 5.2: a mono file at full scale is one Paula channel at
  full volume (so a dump replays at the original level with `gain_db` 0), voices are shifted 16 frames to line up with Paula's filtered output, a write of the minimum period reaches a voice 3 ms
  late, a stereo file started on a channel pair plays once, and the dump never overwrites an entry or file that exists. The dump loaded back as a pack (directory and ZIP) reproduces
  the original audio (correlation 0.99+, tests `dump_roundtrip_*`). Not done: music cues in the dump (WS4).
- **WS2b (backgrounds, stars, hit flashes, the pause hold): landed** (`docs/re-notes/asset-packs-backgrounds.md` has the measurements, the design as built and the evidence).
  Measure items: the far layer is a **192-line ring** on the disk builds and the CDTV release (screen line `y` shows ring line `(ring + y) mod 192`, the ring variable falls as the level
  advances, `far_ring_lines = 192`, `far_scroll_sign = -1`), the level word is 1-5, a hit flash is the bob's **mask drawn as a flat colour** (four plane blits, minterms `$FA`/`$0A`) that
  the **source pointer cannot tie to its graphic** but the object record plus the mask shape can, and the **pause does revert replaced graphics** (the presenter's hold falls back to the
  faithful frame). Built as in 4.3, 4.6 and 4.7 with these deviations: `backgrounds` take a 192-line picture only (`ring_lines` other than 192 is ignored: the ring has 192 positions, so the
  design's `ring_lines: 1024` example cannot be bound), the top or bottom edge strip a sub-line move uncovers keeps the original (so the dump loads back exactly), a level whose far layer the
  game rewrites during play (CDTV level 4) is dumped to `far_full/` but not listed under `backgrounds`, flash ties use the object record and the mask shape (a `shape` hash in the key,
  `f:<shape>-<w>x<h>`) instead of the source pointer, and the pause hold keeps the composed picture only while the frame on display is the held step's own picture. Round stars render on
  one thread (about 5.7 ms per frame at 4x), and read the loader's `size_px`. Not done: the user guide text (WS6) and a load-time rejection of a wrong-height background (WS1b's validator).
- **WS5b (demo pack generator): landed** (`tools/assetpack/make_demo_pack.py`). Takes the dump(s) and writes a pack that validates with no warning, covers every dumped key and loads
  through the real `PackStack` as a folder or a stored ZIP; the data tests `demo_pack_disk` and `demo_pack_cdtv` dump, build and play it (all 362 presented pictures of the window
  change, audio differs, ZIP equals folder, `--faithful` restores the original). Deviations from section 8 item 6: panel cells are dumped one cell wide, so each is redrawn as a
  flat two-colour glyph of the cell's own shape (runs of several cells cannot be keyed without the faithful frame, so `max_span` runs are not exercised); music slots are the
  aliases `title`, `level1`-`level5` and `level`, which resolve on both Amiga builds (no `cdN` slots); the ST gets screens only (no sprites, panel cells or audio in its dump, and `--dump-audio`
  refuses it). Found on the way: `--check-asset-pack --against` counted a dump's `f:` hit flashes in its graphics denominator but never as hits (fixed; the Python validator
  never counted them).
- **WS6 (documentation): landed.** `docs/ASSET-PACKS.md` (the guide), `docs/ASSET-PACKS-REFERENCE.md` (every field, kept in step with the schema by the test `assetpack_reference`), the tutorials in
  `docs/asset-packs/`, the legal text in the guide and the README, and a skeleton with every section. Where the code differs from this design: there is **no hotkey for pack audio** (Page Up is the music
  source key; use `--modern audio|noaudio` or `audio_upgrade`), `settings.cfg` (and so `packs_enabled`) is read by the window only, `--dump-audio` does not write `music_cues.txt` (nothing feeds
  `note_cue`; use `--audio-cues FILE` on the disk builds and `--cdtv-log FILE` on the CDTV release to find cues), the music slots do nothing on the 1762 and PDos builds (their cue addresses are
  not mapped) and the PDos effect code is not mapped, `--frames` given before `--input-script` is overridden by the script's `frames` line, and `--check-asset-pack --against` identifies a screen by
  its first signature block only (screens that share one count once). The schema gained `games` and `builds` on `far` entries; `--help` no longer calls the pack commands unimplemented.
- **WS7 (integration: fuzzing, sanitizers, integration tests, cost gates): landed** (`docs/re-notes/asset-packs-fuzzing.md`). Six libFuzzer targets (manifest, ZIP, PNG, WAV, FLAC, Vorbis) that
  also run as plain tests over a generated synthetic corpus; `X2_SANITIZE` and `X2_FUZZ` options; `fuzz.yml` (sanitizer tests and fuzzing on pull requests that touch the pack or audio code, nightly 30
  minutes per target; not a required check); `x2_pack_integration_tests`; the data tests `pack_full_*`; shared helpers in `tests/support/synth.h`. The manifest fuzzer found a float-to-integer overflow in the
  "version needs a newer x2recomp" message (fixed). Deviations from section 9: the seed corpus is generated by `x2_fuzz_seeds` instead of checked in (no new binary directory for the hygiene check to
  allow); the synthetic cost gates are set wider than the budgets (4 times), and the real-game gate first measured whole runs and found the graphics pack over the 5 % budget on the flood replays
  (see the note; the figures were partly the test's own hashing, and the rest came from round stars forcing one render band, the overlay clearing in the replay and the frames outside the game: all fixed in the performance pass below);
  the audio budget is met. Not done: the Windows run of the data tests (self-hosted macOS only), a nightly with the dump as seed corpus.
  **Performance pass (graphics cost).** The real-game gate now times the presenter alone (the run's "present time" line, hashing excluded) and its limit is the design's 5 % (833 us per presented picture).
  Round stars render in parallel bands (per-band blocks merged in single-band order; pictures bit-identical, checked on 13 configurations of 7,201 pictures and by `modern_backgrounds` with
  `--modern threads=1|3`), the replay skips clearing the overlay where nothing was written, and frames outside the game are expanded once per line. Measured on the flood replays at scale 3:
  +216 us (disk) and +209 us (CDTV) with the dumps, +263 us and +238 us with the demo pack, 1.3 to 1.6 % of a 60 Hz frame (`docs/re-notes/asset-packs-fuzzing.md`).

## 11. Decisions needed from the user

| # | Decision | Recommendation |
|---|---|---|
| U1 | Vendor FLAC (dr_flac) and Ogg Vorbis (stb_vorbis) decoders, WAV always, no MP3/Opus? | **Yes.** Public-domain/MIT-0, no system dependency, the only way to ship music packs of a sane size. Fallback if declined: WAV only (packs 10x larger). |
| U2 | When `--asset-pack` loads a pack with an audio section, is audio applied by default (the Faithful switch still restores the original)? | **Yes**, with a separate off toggle; it matches what `--asset-pack` does for graphics today. |
| U3 | Raise the pack caps to allow multi-hundred-megabyte music packs (ZIP 4 GB, audio members 512 MB, streamed)? | **Yes**; music is streamed so the memory cost is small. |
| U4 | Keep an example skeleton and a demo-pack generator in the source repository (no game content, not in the release archives)? | **Yes.** |
| U5 | Atari ST in the first release of packs: screens and panel only, ST audio and sprites later? | **Yes**; the rest needs analysis that is not done. |
| U6 | The validator warns on a missing `license`, errors with `--strict`; the project runs no pack registry or download service? | **Yes**: the project documents the legal rule and leaves distribution to authors. |

## 12. Risks

1. **Block signatures can collide or miss.** Mitigation: the dump verifies uniqueness against other screens and sampled gameplay; at least 6 stable
   blocks required; the validator's `--against` coverage report; a manual `min`/block override is possible.
2. **Far-layer binding and flash correlation**: measured by WS2b (the ring is 192 lines, the source pointer does not tie a flash but the object and the mask shape do); the dump-as-pack
   exactness tests (`modern_backgrounds`, `modern_cdtv_backgrounds`) hold the binding. A far layer the game rewrites mid-level (CDTV level 4) cannot be one picture.
3. **Pause reverts replaced graphics today**: confirmed and fixed by WS2b (the held step's composed picture stays while the frame on display is that step's picture); the hold logic's goldens
   are unchanged.
4. **Audio pitch follow** depends on a faithful period timeline; Paula records period writes at colour-clock resolution, the voice reads them per output block.
   Rapid arpeggios are rare in effects (periods change at note boundaries), and `pitch: fixed` is the escape hatch.
5. **stb_vorbis security history.** Mitigations in 5.4; if the fuzzing is not clean the fallback is to drop OGG (decision U1).
6. **ST** has no capture path: only screens and the panel work, and the ST path presents `m.picture()` directly (`runtime/st/st_run.cpp`), so WS2 adds a shared post pass **(measure)**.
7. **Key stability across builds**: sample keys differ where samples differ (Power Pack), graphics keys for the 1 of 109 that differs on CDTV; `keys` lists cover
   it, and the dump of each build shows the differences.
8. **Memory** at 8x with many screens (each 2560 x 1600 RGBA is 16 MB): the 512 MB cap holds, and the validator prints the estimate; screens decode on first use
   if they exceed 25 % of the budget.
9. **Scope.** 53 days is a lot; the order of merge makes graphics (WS1b, WS2) and audio (WS3, WS4) independently shippable.

## 13. Alternatives rejected (summary)

| Idea | Why not |
|---|---|
| Replace samples in chip RAM, or change `AUDxLEN`/`AUDxPER` | Changes machine state and DMA timing; breaks replays and the loop pattern. |
| Replace tracker instruments | Instrument loops and per-note restarts need the driver modelled; whole-track music covers the use. |
| Key effects by command word (`$414`/`$418`) | Not content-stable, several samples per command, numbering per build. |
| Whole-frame hash for screens | Breaks on any animation or dynamic text. |
| Per-screen state ids as the primary identity | Per-build, per-screen reverse engineering; reserved as an optional selector. |
| Embed original pixels in a pack as a similarity reference | Legal: a pack must never contain game material. |
| Variable-bound HUD in 2.0 | Needs glyph mapping and geometry per build; deferred (reserved `panel.bindings`). |
| Host audio thread mixing | Not reproducible; no `--audio-hash`. |
| YAML or TOML manifests | JSON already has a parser and a schema ecosystem; v1 is JSON. |
| MP3 or Opus decoders | Patent history (MP3), size or system dependency (Opus), no loop semantics. |
| New file extension or a binary pack format | A directory or ZIP of PNG/WAV/OGG/JSON is editable with any tool, which is the point. |
