# Asset packs: screens and the status panel (workstream WS2a)

Status: implemented. This is the engineering note for the screens and panel half of WS2 in `docs/re-notes/asset-packs-v2-design.md`
(sections 4.4, 4.5, 4.7 and the measure items of section 10). Backgrounds, stars, hit flashes and the pause hold are WS2b and are not
here. The user-facing text is `docs/ASSET-PACKS.md` (WS6); the format is the schema `docs/schema/x2-asset-pack-v2.schema.json`.

A pack replaces whole non-gameplay screens (title, credits pages, menus, loading, GET READY, high scores, the shop, game over, the
ending, the CDTV and ST front ends) and the status panel strip of the playfield. Nothing here changes machine state: the machine runs
exactly as without a pack, and the Faithful switch (F1, `--faithful`, `--modern faithful`) bypasses every pack effect.

## 1. The indexed frame

Every game draws its screens in palette indices and the colours behind the indices move (fades, copper lists), so screens are
identified by **indices** and the colours are used only to follow a fade. `modern/indexed.h` defines `IndexedFrame`: the 320 x 200
display window as colour indices, the palette each line starts with (`0x00RRGGBB`), and the faithful pixels of the same window.

| Machine | Source | Indices | Palette per line |
|---|---|---|---|
| Amiga (disk builds, CDTV) | `Video::set_index_capture(true)`: `Video::line_end` records, for each pixel, the colour register Denise picked (bitplane colour 0-31, sprite colours 16-31 where a sprite is in front, HAM/EHB: the low five bits) | 32 | the 32 colour registers when the line's first pixel is drawn (copper writes before it are in) |
| Atari ST | `StMachine::set_index_capture(true)`: `Shifter::render_line` writes the plane index and the palette the line starts with | 16 | the 16 registers at the start of the line (a write inside the line shows from its pixel in the picture, and in the next line's palette) |

The capture is off until a consumer asks for it; `front()`, `front_hash()` and `picture_hash()` do not depend on it, so every golden is
unchanged. Test `video_tests index_plane`: the indices and palettes give the rendered colours back, mid-frame palette changes
show from the right line, `front_hash` is the same with and without the capture.

## 2. Measure items

**Panel strip.** Counting the distinct 8 x 8 cells of the strip over a level-1 replay at every x offset 0-7 and every first line, the
cell count is lowest at x offset 0 and first line 192 on all three builds:

| Build | cells at x offset 0 / 1 / 2 / 3 / 4 / 5 / 6 / 7 | at first line 189 / 190 / 191 / **192** / 193 |
|---|---|---|
| disk (canonical) | **65** / 116 / 112 / 116 / 107 / 106 / 61 / 63 | 2620 / 2475 / 2115 / **65** / 64 |
| CDTV | **93** / 142 / 134 / 137 / 131 / 132 / 90 / 92 | 2186 / 2114 / 1834 / **93** / 85 |
| Atari ST | **95** / 136 / 130 / 133 / 127 / 127 / 95 / 95 | 2183 / 2093 / 1819 / **95** / 91 |

(Offsets 6 and 7 are low only because most of the strip is blank; lines below 192 are subsets of the panel, lines above are playfield.)
So the strip is **window lines 192-199, 8-pixel cells aligned at x = 0, 8, ..., 312** on every build: `PanelLayout{192, 8, 8, 6}` is now the
value of `kDiskLayout`, `kCdtvLayout` and `kStLayout` (the ST's was unmeasured). Distinct cells seen by `--dump-assets` over one replay: 94-193
(they grow with the palettes the strip is drawn with: a cell is keyed by its indices, not its colours, so a font replacement needs about 70 pictures plus
the bar and icon cells; see section 6 for what the palette does).

**Hires and odd modes.** None. Every screen of every build is lores 320 x 200 (the Amiga window 320 x 256 with the window at lines 14-213, the
ST's 320 x 200); the Amiga uses no HAM or EHB on any screen of the corpus, and the ST never leaves low resolution (`unsupported_resolution_frames` is 0).

**Palettes.** The Amiga game fades by taking a fixed step off every colour (`step * 0x22` per channel; a fade-in is the same steps in the
other order), not by scaling: a multiplier fitted to the same frames is off by up to 123 of 255 in the worst frame. The ST fades the same way in
its own steps (one of eight guns). Per-line palette changes (copper) on screens: none of the dumped screens has a palette that differs between
lines in a way the first line does not give; the screen palettes are constant within a frame, so the single `ref_palette` of the spec is enough.
Palette **animation** does happen (the ST's Bitmap Brothers logo and some Amiga title frames cycle colours): see section 6.

**Screens per build** (`--dump-assets`, one replay each; the screens below are what the dump found, named by the hash of their signature):

| Build | Screens dumped (replays) | What they are |
|---|---|---|
| Amiga disk builds (canonical, retail 2874/0297, Power Pack, 1762, PDos v5, cracks) | 6-9 per replay (attract 9, shop 7, ending 9, game over 6) | the caption and credits pages (the logo and the credit lines are one screen, the cycling text a dynamic area), the SELECT menu, the high-score table, the shop (frame and side panel; the grid of item cells and the price lines are dynamic), LOADING LEVEL and GET READY, the ending slides |
| CDTV | 3-10 (attract 3, menus 3, shop 10, game over 10, ending 8) | the same, plus the INTRO pages, the language page and the joypad-mode page |
| Atari ST | 9-10 (attract 10, shop 10, game over 10, menu 9) | the Bitmap Brothers caption, the XENON 2 MEGABLAST title (a full-window picture), the credits pages, SELECT, high scores, LOADING, GET READY, the shop |

Screens that are not dumped (and are reported as "not matchable"): the scrolling high-score table and credits of the attract loop on the
disk builds (every block changes), and the ending's scrolling text.

## 3. Matching

`ScreenMatcher` (`modern/screens.h`) compares the signature blocks of every screen of the library with the frame and takes the screen with the
highest **share** of matched blocks among those that reach their `min`; an equal share goes to the later screen of the load order, so a
later pack overrides an earlier one. Cost: at most one 16 x 16 hash per block with an early exit as soon as `min` is out of reach (the
cases that fail are the common ones), well under 0.1 ms per frame for 60 screens, once per emulated frame (matching and the fade are cached on a frame
serial, so the many presents of an interpolated run do not repeat them).

The previous screen is kept while it still matches. A signature that asks for all its blocks gets no leeway; one with `min < n` keeps the
screen through `min - 2` blocks (the spec's "cached while at least `min - 2` blocks still match"). Blocks lying outside the window never match.

Matching is not run while the presenter shows a captured gameplay step (a screen never matches play), and nothing is matched or drawn in
Faithful mode or without `upgrade`.

## 4. Fades and the dynamic parts

`fade_ratio` fits the game's fade from the palette: for the indices the frame uses, the colours the frame's lines give them against the
screen's `ref_palette`. Two models are fitted per channel, a multiplier and an amount taken off, and the one that reproduces the frame's palette better
is used (a tie goes to the subtractive one, the game's own). Within 2 % of the reference the picture is shown unchanged, and `fade: "none"` or a missing
`ref_palette` never fades. The **residual** (largest deviation of any used colour from the fitted model) tells whether a palette change is a fade at all.

With the dump as a pack the fitted fade reproduces the game's own fade exactly on the Amiga (steps of `0x22`: 98 of 98 fading frames in the ending, 75 of 75
in the shop replay are pixel-identical) and to within one colour step on the ST (8 levels; 30 of 36).

`dynamic` rectangles keep what the game still draws: inside them the original pixels whose index is not `bg_index` are drawn over the picture
(replicated or enlarged to the canvas scale), the rest shows the artwork. `wide_png` is centred on the window and drawn before the picture, so it fills
the widescreen surround; without it the sides stay black as today.

Spec deviations: `ref_palette` accepts `#rrggbb` as well as `#rgb` (the ST's guns are 3 bits and the dump writes the mean colour of an
index over the screen); the fade has the subtractive model; the dump's screen pictures hold the **dynamic parts as background** (so the
pack's own picture is exactly what the original shows where it does not change) and `reference_png` is the unmasked original.

## 5. The panel

`PackScreens::draw_panel` runs for frames in a game (`in_game`: the ship count word is not 0) on which no screen was drawn and the
strip is not blank (loading and GET READY have no panel to restyle). It hashes the strip (2560 index bytes) and re-evaluates only when the hash changes: for each column,
the longest run (up to `max_span`, 6 here) whose key `<hash of the 8n x 8 region>-<n>` is in the pack's `cells` is replaced and consumed; a column
without a replacement shows the original, upscaled with the HD-panel scaler when that is on (the scaler runs first, the cells draw over it).
A `background` picture is drawn first, and the cells without a replacement keep the original over it, so an author who uses a background
gives the blank cell a (transparent) replacement. A cell's `anchor` lets its picture overhang the strip over the playfield. The panel fades with the
game exactly as a screen does (`ref_palette`, `fade` of the `panel` section, both additions to the spec): the HUD of a pack made from a dump dims with the level's
fade-in and goes black with the fade-out.

Composition order in `Presenter::at`: gameplay playfield (or the faithful frame) -> HD-panel scaler -> panel cells or the screen -> surround -> host overlays;
when a pack has a panel, the strip always comes from the frame (the CPU draws it in place after the swap) and not from an interpolated step.

## 6. What a pack cannot follow

- **Palette changes that are not fades.** Index 1-3 of the strip are the metal colours of the level and the shop and bonus screens
  draw them differently; the cell pictures are painted with the palette the strip had in most frames. Frames with another palette show the
  cell art unchanged (the original's HUD would have changed hue). On the dump of a replay this is 0-3 % of the frames (counted in `--modern-check` as "a palette that is not a fade of the pack's").
- **Palette animation** on a screen (the ST's logo cycle): the screen's picture holds the most frequent palette of its life.
- **Signatures are exact.** A screen the dump saw is matched only when every one of its blocks matches (up to 64 blocks, 40 chosen by spread
  and the rest added to tell the screen from every other frame of the run). A frame that shows the same screen with different content in a block that never changed in
  the run falls back to the original, which is the safe failure. Authors can lower `min`.

## 7. `--dump-assets`

With screens or a panel in the run the manifest is **version 2** (the v1 sections are unchanged; a run without them still writes version 1, so every
existing dump and test is byte-identical). `screens/<name>.png` (the picture, dynamic parts as background) and `screens/<name>_reference.png` (the original at its most frequent
palette), `panel/<key>.png` for every distinct 8 x 8 cell, the sections `screens` (signature, `ref_palette`, `fade`, `bg_index`, `dynamic`, `first_frame`, `frames`, `uses`)
and `panel` (`scale`, `cell_w`, `max_span`, `ref_palette`, `fade`, `cells`). Differences from the spec's sketch, chosen so that the dump reproduces the run it was made from:

- every frame is looked at (not every fifth) and a block is in the signature only if it **never** changed during the screen's life; a block that changed at any
  time becomes part of a `dynamic` rectangle (at most 32: the nearest are merged); `min` is the number of blocks;
- a **cut** is more than 40 % of the grid blocks that are not flat in either frame changing at once (flat black blocks are ignored, so two mostly black screens do
  not run together);
- a life shorter than 12 frames (45 when the game was in play throughout) is a transition, not a screen;
- every frame of the run (screens, and a sample of the play frames the capture knows) is **checked against every finished signature**; where a signature is met by another
  frame that differs from the screen outside its dynamic parts, blocks that tell them apart are added (up to 64); a screen that play can fool is dropped with a warning;
- panel cells are dumped as single cells; runs of several cells are keyed by the same formula (`block_hash` of the 8n x 8 region) and an author adds the pictures for the
  ones they want;
- a screen seen twice with the same signature is one screen whose dynamic parts are the union.

The Atari ST has no draw capture, so its dump cannot tell a level that stands still from a screen: those come out as screens too (delete them from the pack).

## 8. The Atari ST present hook

`st_run.cpp` builds an `IndexedFrame` from `StMachine::index_plane()`/`palette_plane()`/`picture()` and, with a pack that has screens or a panel and `upgrade` on,
presents `PackScreens::render_window` (the window enlarged by `--modern scale=N`, default 3) instead of `m.picture()`; without, the ST presents exactly as before.
`--asset-pack DIR` loads the pack; `--dump-assets DIR`, `--modern-check` and `--present-shot F:FILE` work on the ST too (screens and panel only; ST sprites, backgrounds and
audio are not in this workstream). The recorded ST frame and audio hashes do not depend on any of it.

## 9. Loader

`ScreenPackLibrary` (`modern/screen_pack.h`) is a `pack::PackLibrary` that reads the `screens` and `panel` sections of a pack directory (and builds in memory for tests).
It stands in until the general loader (WS1b) installs the full library: `main.cpp` and `st_run.cpp` install it only when `pack::packs()` is empty. It
validates what it uses (signature shape, block size 8/16/32, at most 64 blocks, `min` 1..n, hash syntax, scale and anchor ranges, at most 32 rectangles, safe paths,
decodable PNGs, scope) and reports every problem as a `PackMessage`; an entry with a problem is skipped.

## 10. Evidence

Round trip (`tests/modern_screens.cmake`, ctest `screens_*`): the replay's own dump loaded as a pack gives, at canvas scales 2 and 4 (3 and 8 spot-checked), the
frames the game rendered. Per build (frames with a screen or the panel; "identical": pixel-identical; "not a fade": the strip had a palette that is not a fade of the pack's;
"other": must be 0):

@@ROUNDTRIP@@

Unit tests (`x2_screens_tests`, no game data): matching (exact, minimum, hysteresis, collisions, outside frames), the fade fits (both models, steps 1-6, `none`, dynamic
rectangles left out), compositing at scales 1, 2, 3, 4 and 8 (integer up- and downscale, anchors, `dynamic`, widescreen picture), the panel (longest run first, change detection,
anchors, background, fades), the loader (good and bad manifests, scopes, version 1, unsafe paths), the dump (screens found, fade frames, play not a screen, collisions told apart,
reproduction of the run at three scales, the panel dump and its fade), and the ST shifter's index plane.

## 11. Gaps for WS2b and later

- Backgrounds, stars, hit flashes and the pause hold are not done; `CaptureLayout::far_ring_lines`, `far_scroll_sign` and `level_var` stay unmeasured.
- The ST has no gameplay hint for the dump; the ST present hook has no interpolation or surround (ST modernisation, S8).
- `ScreenPackLibrary` loads directories only, no ZIP packs; the general loader replaces it.
- A screen picture bigger than the window (an overhang by `anchor` on a screen) is drawn at the window's corner minus the anchor; `wide_png` of a different height than the window is top-aligned.
