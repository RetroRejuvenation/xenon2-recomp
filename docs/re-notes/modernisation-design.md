# Modernisation: interpolation, widescreen and upgraded graphics (Phase 9a design)

Question: how can the optional modernisation goals (spec section "Modernisation") be built on this game without
touching game logic, audio or the faithful picture? The goals are high-frame-rate output, widescreen, and upgraded
colours and resolution. Which approaches does the game's own drawing allow, and at what cost?

**Short answer:**

- **Interpolation is feasible and the spike works.** The game rebuilds its whole playfield every logic step,
  in a small and regular set of drawing routines. So each step's draw list can be captured and replayed into a
  higher-resolution canvas, with each layer or object moved by a fraction of its motion between two steps.
  - Every captured draw call can be tied to an object record, a background layer or the starfield.
  - The spike checked 1,348 consecutive step pairs of the level-1 replay. All of them re-render exactly at t = 0
    and t = 1.
  - Rendering takes about 3 ms per frame at 3x and 5 ms at 4x, in plain single-threaded C++.
  - Recommended: draw-command replay (approach A1), behind `--modern interp`.
- **True widescreen is not available from the game's data.** The foreground tile map is exactly 20 tiles
  (320 px) wide, and the far layer is a 320-px bitmap. Nothing exists beyond x = 0..319.
  - Objects live from about x = -56 to 352. The game clips them to the window, and 1-3% of them appear or are
    deleted outside it, mostly within 16 px of an edge.
  - The safe extension that shows real game content without visible pop-in is **0 px** for objects.
  - Recommended: "extended surround". Synthesise background-only side areas (repeat the side-wall tile columns,
    or use artwork), clip objects at the original edge, and blend the seam. This is the documented, consistent
    handling the spec allows. Anything more needs new art, not code.
- **Upgraded graphics.** Ship in this order:
  1. integer scaling with a filter or shader (trivial);
  2. a higher-resolution canvas, which interpolation already gives;
  3. replacement asset packs keyed by a hash of the bitplane source data;
  4. a re-rendered HD status panel.

  The game uses **4 bitplanes (16 colours)**. The far layer uses only planes 0-1. Objects, tiles and the panel
  share one 16-colour palette per level.

**Status (Phases 9b and 9c done):**

- **Done.** Window filters, the Faithful switch and productised interpolation are implemented.
  `docs/BUILDING.md`, "Picture", has the user side.
- **Files.**
  - `runtime/modern/present.{h,cpp}`: `PresentOptions`, `Presenter`, `PresentClock`, `VsyncTimeline` and the
    Scale2x/Scale3x upscalers.
  - `runtime/gfx/sdl_filters.{h,cpp}`: the SDL_Renderer filter chain, with no shaders.
  - `runtime/modern/interp.cpp`: band-parallel replay, palette blending and cut detection.
- **Tests.**
  - `modern_check_<replay>`: every replay is exact at t = 0 and t = 1.
  - `modern_faithful`: Faithful pictures are byte-identical to the rendered frames.
  - `modern_midpoints`: golden hashes at t = .25/.5/.75.
  - `modern` (unit): linearity, threads, palette, cuts, the presenter, the upscalers, and pacing at 50-144 Hz.
- **Performance** (M4 Pro, 260 Hz display, scale 3). Rendering takes 0.5 ms per interpolated frame headless
  and 0.9-1.5 ms in the window.
  - Fullscreen: about 250 presents per second (CRT filter on, recompiled build).
  - Windowed: about 119 presents per second. macOS composites windows at 120 Hz, and a bare SDL present loop
    measures the same rate.

Evidence and the staged plan follow. Measurements come from `--draw-log` captures analysed by
`tools/modern/analyze_draw_log.py`. `level1_play` is natural play (no pokes). The other replays use pokes to reach
late game states, so their scroll "jumps" are cuts made by the pokes, not game behaviour.

## 1. Spike code (off by default)

| Path | What |
|---|---|
| `runtime/machine.{h,cpp}` | Two null-by-default observers. `on_blit` is called at the start of every blit, with the registers as the CPU set them. `on_cpu_write` is called for every CPU write to chip RAM. When they are null, a run is unchanged; the replay and golden hashes are unchanged. |
| `runtime/modern/draw_capture.{h,cpp}` | `DrawCapture` groups each logic step's blits, screen-buffer CPU writes and the following state (taken at the swap): object lists, scroll variables, starfield table, palette, and the finished picture. Optionally it also keeps the A/B/C words each blit consumed (`capture_data`). |
| `runtime/modern/interp.{h,cpp}` | `Interpolator`: motion analysis of a step pair, and replay of a step's draw list into an N-times canvas with per-command displacement. `InterpPresenter` maps emulated time to a step pair. |
| `runtime/main.cpp` | `--draw-log F-G:FILE`, `--modern interp[,scale=N][,nosmooth][,rate=HZ]`, `--modern-out F-G:DIR` (headless, writes host-rate PNGs), `--modern-shots F:DIR`, `--modern-check`. The window loop presents interpolated frames at the display's vsync rate when `--modern interp` is given. |
| `runtime/gfx/sdl_display.*` | `present_scaled()`: shows a picture N times the base size in the same window area. |
| `tests/modern_tests.cpp` | Synthetic tests (no game data): blit decoding, object identity from the stack, A/B/C data, step boundary, exact endpoints, object at the midpoint at scales 1 and 2, cuts, and fallback for non-gameplay steps. |
| `tests/modern_game.cmake` | Real game; Skipped without assets. Runs `--modern-check` on `level1_play`. With `X2_RECOMP`, also checks that the draw capture is byte-identical under the translated executor and `--interp-only`. |
| `tools/modern/analyze_draw_log.py` | The measurements in this document. |

## 2. How the game draws a picture (measured)

There is one picture per logic step. The main loop (`$78A4`) runs one logic step per iteration:

1. `$1F22` updates the far-layer ring row `$436`.
2. `$17D6` draws the background.
3. `$233A` draws the starfield.
4. The score and the energy bar (`$6766`, `$16BE`) are updated.
5. The level script runs.
6. `$35D4` updates and draws all object lists.
7. The palette is updated (`$6372`).
8. `$202E` swaps `$406`/`$40A` at line 258.

The draw buffer has 4 interleaved lores planes (160 bytes per line) and 200 visible lines:

- **Lines 0-191 are the playfield. It is rebuilt from scratch every step.** There is no dirty-rectangle restore and
  no scroll copy of the previous picture.
- **Lines 192-199 are the status panel.** The CPU updates it in place, in both buffers, only when a value changes.
- **The 16 lines above each buffer are a gutter.** The partly visible top tile row is drawn there. The gutter
  sits in the gap between the two buffers (`$4BD00-$4C6FF` above buffer B).

| Layer | Routine | How it is drawn | Motion per step |
|---|---|---|---|
| Far layer | `$17D6` (blits at PCs `$1832`, `$185A`) | A ring buffer of 192 lines × 2 planes, 320 px wide (pointer at `$54E00`, ring row `$436`), copied in 1-2 blits into planes 0-1 (A→D, minterm `F0`). `$1884` then clears planes 2-3. | Half the scroll speed: `$436` moves `$CD8`/2 lines per step, with the odd half on alternate steps (`$CDC` parity). On level 1 the deltas alternate 0 and 1 (50/50 over 100 steps). |
| Foreground tiles | `draw_background_a/b` `$18B6`/`$1BD0` | A tile map 20 columns (= 320 px) by 13 rows, at map pointer `$54E04` + row offset `$CCE`, fine offset `-$CD6`. Tile id 0 is empty. A positive id is an opaque 16×16×4 copy (`09F0`). A negative id is a cookie-cut (`0FCA`, 4 blits). | 1 line per step (`$CD0` -1) in normal play. Measured by matching tile blits between steps: dy = +1 in 100 of 100 level-1 steps. |
| Stars | `$233A` (CPU, `or.w`) | 48 stars in 4 colour groups (colours 4/5/6/7). Table at `$42FDC`: {line offset, pixel mask, sub-line accumulator}. A star is plotted only where all 4 planes are 0. | Per group speed. The continuous position is line + accumulator/`$180`, so the interpolator gets exact fractional positions. |
| Objects | `run_object_list_draw` `$366A` → handler at obj+6 → `blit_bob_cookie` `$129A` (4 cookie-cut blits, one per plane, A = mask, B = plane data, C = D = screen), `$1368`/`$1438` variants (minterms `FA`/`0A`), and the tile routine `$1BD0` for large objects | 5 lists, heads `$A88`/`$AEA`/`$B4C`/`$BAE`/`$C10` (pointer to the first record; a record with +0 = 0 ends the list). Position at +`$20`/+`$24`. Clipped by `$107E` to x < `$140` and to the bottom line `$420`. | Typically -9..+9 px per step (bullets -9 px/step in y). |
| Status panel | `$16BE`, `$176E`, `$6766`, … (CPU) | Fixed positions in lines 192-199, drawn into both buffers. | None. |

Blits per step:

| Replay (gameplay steps) | Blits per step (min / median / mean / max) | Tiles | Objects | Far layer | CPU screen writes per step (stars + panel) |
|---|---|---|---|---|---|
| `level1_play` 1200-1500, natural play | 169 / 188 / 190 / 214 | 106 | 81 (≈20 bobs × 4 planes) | 3 | 46 + 28 |
| `level1_ff` | 65 / 298 / 300 / 558 | 154 | 123 + 19 via tile routine | 3 | 43 + 114 |
| `level2` | 65 / 330 / 319 / 465 | 190 | 108 + 19 | 3 | 49 + 71 |
| `level3` | 57 / 255 / 280 / 598 | 58 | 218 | 3 | 64 + 194 |
| `level4` | 65 / 299 / 309 / 619 | 142 | 151 + 13 | 3 | 50 + 103 |
| `level5` | 65 / 424 / 412 / 761 | 228 | 181 | 3 | 34 + 343 |

Other facts:

- **Destinations.** 95% of blits write into the step's own draw buffer and 5% into the gutter above it. **None
  write into the buffer on display.**
- **Minterms.** Only `CA` (cookie-cut), `F0` (copy or constant fill) and, rarely, `FA`/`0A` (bob variants at
  `$13AA`-`$1426`) occur. There is no line mode and no fill mode in gameplay.
- **Step length.** Natural level-1 play: 3 VBLs for 416 steps, 4 VBLs for 29, and 5-9 VBLs for 23 (accurate
  timing). The heavy poked levels slow down a lot: level 3 mostly takes 4-5 VBLs and level 5 takes 5-6.

### Object identity (per-step tags that survive)

- **Identity of a drawn blit.** `run_object_list_draw` pushes the next record's address before it calls the draw
  handler. So during a handler's blits, the stack holds `{$3678, next}`, and the drawn record is the one whose
  +`$12` link is `next`.
  - **100% of the object-class blits get a record in every replay.** That includes large objects drawn with the
    tile routine: 17-25 K of those per level-1/2/4 replay.
  - Background and star drawing never has `$3678` on the stack. So no background blit is misattributed.
- **Identity across steps.** The record address plus the update handler (+2):
  - **level1_play**: 2,552 of 2,726 (94%) live records are in the previous step too; there are 0 jumps over
    48 px.
  - **Poked replays**: 91-95% are in the previous step. 2-734 jumps over 48 px per replay. These are records
    reused for a new object, or warps; the interpolator treats them as cuts.
- **Spawn and despawn.** A record that is new (or dies) appears (or vanishes) at the midpoint of the interpolated
  interval.

## 3. A. Interpolation

### Approaches compared

1. **Draw-command capture and replay (chosen).** Capture every blit and screen write per step and replay the
   step's list into a canvas `scale` times larger, moving each command by its layer's or object's motion.
   - Works because the playfield is rebuilt every step, layers are distinguishable by drawing routine,
     and every object blit is tied to its record.
   - It needs no knowledge of graphics formats: it replays the game's own blitter operations, minterms
     included. That is why the endpoints are bit-exact and odd cases (large objects made of tiles, the `FA`/`0A`
     bob variants) come out right.
   - Game-specific knowledge needed: 6 PCs/ranges (far layer, tile routines, star plotter), the object-list
     layout, the star table and the ring-row variable. All are in `interp.cpp`.
2. **Game-state re-render (a parallel renderer).** Draw the level from the map, tile graphics and object
   records ourselves. It would need decoders for every draw handler: the resident bobs, three bob variants,
   level-module handlers through the API slots `$E12`… and tile-built bosses. It would also need the clipping
   rules and palette effects. That is a large amount of format work, with a risk of visible differences from the
   original. Worth it only for a full remaster; it is not needed for interpolation.
3. **Framebuffer motion interpolation (block matching or optical flow).** No game knowledge, but the motion is
   ambiguous:
   - parallax layers move at different speeds (far layer 0.5 px/step, foreground 1 px, stars at 4 speeds);
   - many identical bullets;
   - sprites move up to 9 px per 60 ms;
   - the source image changes only every 3 VBLs, and objects animate between steps.

   Expect halos and ghosting. It is useful only as a generic fallback for non-gameplay screens, and those
   (menus, shop, text) do not move enough to need it.

### Spike results

- **Endpoints.** `--modern-check` on the whole `level1_play` replay:
  - 1,348 interpolable step pairs. **t = 0 is exact in 1,348 and t = 1 is exact in 1,348**. This is
    pixel-for-pixel RGBA against the captured pictures, with far-layer smoothing off.
  - The captured picture, converted with the palette at the swap, equals the frame the video model rendered for
    1,909 of 1,939 steps. The 30 others come in pairs at screen transitions, where `$6372` fades the palette
    after the swap, while that picture is on display.
- **Continuity.** Step k's picture ends pair (k-1, k) at t = 1 and starts pair (k, k+1) at t = 0. So there is no
  jump at step boundaries for objects present in both steps.
- **Linear motion.** Objects move linearly between their +`$20`/+`$24` positions by construction. The synthetic
  test checks that a 16×8 bob moved by (+32, -8) covers exactly x 80..95, lines 46..53 at t = 0.5, at scales 1
  and 2.
- **Host rate.** `--modern interp,scale=3 --modern-out 1200-1260:DIR` at 60 Hz writes 74 frames for 61 emulated
  frames, and **no two consecutive output frames are identical**. The faithful 60 Hz presentation repeats each
  16.7 Hz picture 3-4 times.
- **Cost.** Software canvas, single thread, Apple Silicon, `RelWithDebInfo`: 0.64 ms per frame at 1x, 3.1 ms at
  3x, 5.3 ms at 4x. That is fine for 60 Hz, and for 120 Hz at 3x. The capture adds the A/B/C words (≈60 KB per
  step) and the step picture.
- **Screenshots.** In `generated/modern/shots1/` (not committed; regenerate with `--modern scale=3
  --modern-shots 1300:DIR` on `level1_play`): `real_a.png`, `interp_t25/50/75.png` and `real_b.png`.
  - At t = 0.5, the ship, the player's bullets (moving 9 px/step) and the "S" pickup are halfway.
  - The tiles have moved half a line, and the stars have moved by their fractional speeds.
  - The panel is unchanged.

### Timing model

- **When a picture appears.** Step k's picture is swapped in at T_k = swap frame + 258/313 (`InterpPresenter`
  `step_time`).
- **What is shown.** At emulated time τ, the newest known step is k. The presenter shows the pair (k-1, k) at
  t = (τ - T_k)/(T_k - T_{k-1}), clamped to [0, 1]. That is "render N-1→N while N+1 computes".
- **Latency.** One logic step: normally 60 ms, 3 VBLs. This is inherent: the next position is not known
  before the next step finishes, and extrapolation would overshoot on every direction change. The input-to-photon
  delay grows from about 1 step (faithful) to 2 steps.
- **Slowdown.** The step after a slow one is shown at the speed of the previous interval. If a step takes longer
  than the previous one, the picture holds briefly at t = 1. That is the original's own slowdown, just smoothed.
  (A two-step latency would know every interval exactly, but 120 ms is too much for a shooter.)
- **Pause, menus, shop, title, loading, level start.**
  - `analyse()` only interpolates pairs of consecutive *gameplay* steps. A gameplay step has the far-layer copy
    at `$1832` and its data captured.
  - The pair must also be 2-8 frames apart, with a far-layer move of 4 lines or less and a foreground move of
    8 lines or less.
  - Anything else presents the faithful frame (`frame()` returns false). Pause has no swaps, so t clamps at 1
    and the paused picture is exact.
  - Screen changes are cuts in the original, so switching between latency 0 (faithful) and 1 step at a cut is
    not visible.
- **Reset.** The capture restarts with the machine. The first pair after a reset is a cut.
- **Far layer.** The far layer moves 0 or 1 line on alternate steps, so exact endpoints would make it move in
  60 ms bursts. `smooth_far_layer` (default on) shows the two-step moving average instead, which is constant
  speed. The price is that at t = 0 or t = 1 that layer is up to half a line from the real picture (718 of 1,348
  pairs are still exact at t = 1, 5,076 differing pixels on average). `nosmooth` restores exact endpoints.
- **Edges.** A moved playfield leaves at most 1 line uncovered at the top or bottom. It keeps the step's own
  picture there. The top tile row has the gutter; the far layer has no data above line 0.

### Limits found

- **Animation frames switch at the midpoint.** The object's gfx pointer changes per step, so this is the
  original animation rate.
- **Palette fades** use the palette of the step being replayed. They could be blended per channel.
- **Stars over moving tiles** are re-tested against the canvas ("only on empty background"), so a star can blink
  at a tile edge for one interpolated frame.
- **Static objects drawn by the level script outside the object lists**, if any, are not moved. None were seen
  in level 1.

## 4. B. Widescreen

What the game provides beyond x = 0..319:

- **Level art: nothing.**
  - The foreground map row is 20 tiles (`$18AA` `moveq #$13,d0`; map row stride 40 bytes) = 320 px.
  - The far-layer ring is 80 bytes per line = 2 planes × 320 px.
  - Side walls are part of the map: tiles in the leftmost and rightmost 16-px columns.

  How often the edge columns hold wall tiles:

  | Replay | Column x=0, mean coverage of tile rows | Fully covered | Empty | Column x=304, mean coverage | Fully covered | Empty |
  |---|---|---|---|---|---|---|
  | level1_ff | 90% | 81% | 1% | 91% | 74% | 2% |
  | level2 | 68% | 56% | 26% | 62% | 48% | 27% |
  | level3 | 35% | 21% | 59% | 35% | 18% | 59% |
  | level4 | 82% | 68% | 8% | 87% | 77% | 8% |
  | level5 | 78% | 52% | 0% | 79% | 52% | 0% |
- **Objects.**
  - Object x (+`$20`) ranges from about -56 to 352 (level 5 has a few at -172..527).
  - `$107E` clips drawing to x 0..319, so objects off the sides exist but are not drawn.
  - Spawns and despawns whose position is outside 0..319:

    | Replay | Spawns outside / inside | Despawns outside / inside | x of those outside |
    |---|---|---|---|
    | level1_play | 3 / 275 | 0 / 262 | spawns at x = -32 |
    | level1_ff | 69 / 2,474 | 11 / 2,424 | spawns -56..-8, 320..352; despawns -8, 320 |
    | level2 | 54 / 3,723 | 28 / 3,706 | despawns mostly x ∈ [-8, 0) and [320, 328) |
    | level3 | 147 / 5,030 | 104 / 4,963 | -48..-8, 320..359 |
    | level4 | 51 / 3,400 | 34 / 3,356 | -32..-8, 320..415 |
    | level5 | 167 / 5,284 | 158 / 5,248 | mostly [-16, 0) and [320, 344) |

  - Objects start being drawn through a side edge 2-8% of the time (most enter from the top, or are bullets and
    explosions born inside). The analyser counts first drawn touching x < 16 or x > 288: level 3 144 right / 9
    left of 2,611.
- **Enemy spawning.**
  - The level script (`$54E24`) and `level_event_scan` `$3554` fire on scroll position: event lists `$54E38` and
    `$54E14`, cursors `$CD2`/`$CD4`.
  - Spawn x comes from the event data. Side spawns are placed just outside the window, 8-56 px out.
  - Objects are deleted soon after they leave the window: their last position is mostly in [-8, 0) or
    [320, 328). This is measured; the per-handler deletion tests were not read.

Consequences:

- **Real game content in an extended view.**
  - The background would need invented columns.
  - Objects would have to be drawn unclipped. That is possible: the replay could draw a bob's full width, or
    re-blit from obj+`$16`.
  - But then objects visibly vanish 0-8 px past the old edge, and side spawns appear from nothing 8-56 px out.
  - **The safe extension with no pop-in or pop-out of objects is 0 px.** At +8 px, the few objects deleted at
    [320, 328) are still cut. Beyond +16 px, every side spawn and despawn is visible.
- **Gameplay change.** None if only the presentation changes. The game still runs its 320-px world. Showing
  off-screen enemies early would give the player information the original does not, which is a mild gameplay
  change.

Recommendation, in two stages:

- **W1, "extended surround".** Background-only side areas:
  - repeat or mirror the outermost tile column (walls are present 35-94% of the time; where a column is empty,
    the far layer shows through);
  - extend the far layer by mirroring its edge 16-32 px;
  - clip all objects, bullets and stars at the original 320-px edge;
  - darken or blur the extension slightly, so it reads as surround rather than playfield.

  The extended area is then consistently scenery and never shows objects. That meets the spec's "documented,
  consistent way". Default width: with square pixels (as the runtime shows them), 320×200 is already 16:10, and 16:9 needs
  356 px, so +18 px per side. With 4:3-corrected pixels (`--aspect`), 16:10 needs +40 px and 16:9 needs +53 px
  per side. Anything up
  to about +64 px per side is consistent.
- **W2, artwork bezels.** Optional per-level side artwork (asset pack), in place of synthesised walls.

Showing real objects in the extension needs the game's spawn and delete limits moved outwards. That changes game
code and gameplay, and is not recommended.

## 5. C. Upgraded graphics

1. **Integer scaling plus shader filtering** (scanlines/CRT, xBR/hqx, or a 4:3 aspect correction) of the
   faithful framebuffer, or of the interpolated canvas.
   - Trivial and independent of everything else. Do it in the SDL renderer (an SDL_GPU shader pass).
   - It does not change the faithful pixels; only the presentation filters.
   - **Ship first.**
2. **Higher internal resolution.** The interpolator's canvas already renders at N× with sub-pixel motion, which
   gives smooth scrolling at 3-4×. A smooth filter for the replacement art would need an RGBA canvas (stage 3).
3. **Expanded colour.**
   - The game has 4 planes (BPLCON0 `$4000`), 16 colours, one palette per level (header +`$1C`).
   - Sprites and tiles share it; there is no hardware-sprite colour sharing to remove.
   - The far layer is limited to colours 0-3, and stars to 4-7.
   - The replay knows which object or layer every pixel came from, so it can:
     - render through a per-layer or per-object palette override;
     - blend palette fades per channel;
     - add alpha edges (a mask-derived anti-aliased edge on cookie-cut bobs).

   This needs the canvas to become RGBA with per-command compositing. A cookie-cut blit (`CA`) is "draw B
   where A". Copies (`F0`) are opaque. `FA`/`0A` are OR/clear effects; treat them as tints.
4. **Replacement asset packs.**
   - **Key.** A bob is drawn as 4 per-plane cookie-cut blits that share the mask (A) source and use consecutive
     B sources. Merge them into one sprite: key = SHA-1 of (width words, height, the mask words and the 4 plane
     words as read). The capture already records them (`BlitCmd::data`).
     - Tiles are keyed the same way (16×16×4, plus the mask for masked tiles).
     - The far layer is keyed per level, by hash of the ring bitmap.
     - The key depends on the graphics content, not on addresses, so it is stable across level modules and
       disk versions.
     - Level 1 has 80 distinct object mask sources and 34 tile sources in its first 1,500 frames.
   - **Format.** A directory or ZIP holding `pack.json` plus PNGs:
     ```json
     {"format": "x2-pack", "version": 1, "scale": 4,
      "sprites": {"<sha1>": {"png": "ship_03.png", "hotspot": [0, 0]}},
      "tiles":   {"<sha1>": {"png": "l1_wall_12.png"}},
      "far":     {"<sha1-of-ring>": {"png": "l1_far.png"}},
      "palette": {"level1": ["#000000", "..."]}}
     ```
     Each PNG is RGBA at `scale` × the original size, aligned with the original's top-left corner.
   - **Loader.** At pack load time, build a hash map from key to texture. At replay, for each merged sprite or
     tile, look up the key; when it is found, draw the replacement at the (moved) position. Otherwise use the
     bitplane path.
   - **Authoring.** A `--dump-assets DIR` mode writes every key seen, with its original PNG, for artists.
   - **Partial matches.** Partially clipped bobs (left and top clipping changes the masks and the first row) fall
     back to the original.
5. **HD UI and text.** The panel is drawn by the CPU at fixed positions from the score (`$C92`/`$C96`), energy
   (`$CC6`), lives (`$CF0`) and weapon icons. An HD panel can be re-drawn from those variables with vector or
   high-resolution glyphs. Menus and shop text go through `print_string_blit` `$15CE` (font `$B666`). Glyph blits
   can be keyed like sprites and replaced by an HD font.

**Ship in this order:** (1) the shader/scaler, then (2) the interpolated N× canvas, then (3) the RGBA canvas
with asset packs and palette overrides, then (4) the HD panel.

## 6. D. Architecture

- **Where it plugs in.**
  - `runtime/modern/` (library `x2modern`) observes the machine through the two hooks and the frame loop.
  - It only reads chip RAM and registers. It never writes them, and it has no PC traps.
  - Game logic, the CPU, Paula and the timing model run exactly as in faithful mode. Audio stays on the emulated
    50 Hz clock.
  - Output is a separate image. `Video::front()` is still produced and hashed.
- **Translated code.**
  - The hooks sit in `Machine::do_blit` and the bus write path. Musashi's handlers and the recompiled executor
    share them.
  - The recompiled code keeps Musashi's CPU state (REG_PPC, A7), so PCs, registers and object identity are the
    same.
  - **Measured:** the 18.5 MB draw log of `level1_play` frames 900-1500 is byte-identical between the translated
    executor, `--interp-only`, and the non-recomp build. `modern_game` asserts this in recomp builds.
- **Settings and CLI.**
  - Spike: `--modern interp,scale=N,nosmooth,rate=HZ`; `faithful` switches it off.
  - Planned, persisted in `settings.cfg`:
    - `modern = faithful | custom` (the single Faithful switch: `faithful` ignores all the others);
    - `interp = on|off`, `scale = 1..8`, `wide = 0..64` (px per side), `filter = none|scanline|crt|xbr`, and
      `pack = PATH`.
  - A hotkey toggles Faithful at runtime.
  - The settings menu is a host overlay (SDL), not in the game, because the game has no free option screen.
- **Tests.**
  - **Faithful hashes unchanged.** All replay and golden-frame tests pass with the hooks compiled in.
  - **Interpolation endpoints exact.** `modern_game` (real game) and `modern` (synthetic) check this.
  - **Capture identical** across executors (`modern_game`, recomp builds).
  - **Golden images** for the modern path come next: hashes of `--modern-out` frames at fixed τ for a replay.
    They are only stable within one renderer version, so keep them separate from the faithful goldens.

## 7. Staged plan

What 9d and 9e plug into:

- **Presenter output.** `Presenter::at()` returns a `Canvas`: RGBA, any width and height, with a `scale` in
  canvas pixels per Amiga pixel. The filter chain takes any canvas size, so a wider (9e) or higher-resolution
  RGBA (9d) canvas needs no presenter or window changes.
- **Per-layer hooks.** `layer_of()` classifies each draw command (far, tiles, object, other; stars are CPU
  writes) where the replay already moves it. That is where per-layer palettes, sprite replacement (9d) and side
  extension or clipping (9e) go.
- **Canvas format.** The replay canvas is still 4-bit colour indices, converted to RGBA per band. 9d makes it
  RGBA with per-command compositing.

| Stage | Content | Effort | Risk |
|---|---|---|---|
| 9b (done) | Scaler and filter output (integer scale, 4:3, sharp, scanlines, CRT, Scale2x/3x) through the SDL_Renderer API (no SDL_GPU, no shader sources); Faithful switch, settings, hotkeys | S | Low |
| 9c (done) | Interpolation productised: GPU or SIMD canvas (or a dirty-only replay), presenter in the window loop with proper vsync timing, palette fade blending, settings and hotkey, modern golden images | M (1-2 weeks) | Low-medium. The capture and replay are proven exact. Risks: pacing against the audio clock, and level-module draw handlers in later levels not yet visually reviewed. |
| 9d | RGBA canvas plus per-command compositing; merged-sprite keys; asset-pack loader; `--dump-assets` | M (1-2 weeks) | Medium (bobs clipped at edges, hit-flash minterms) |
| 9e | Widescreen "extended surround" (W1): edge-column repetition, far-layer mirroring, object clip at 320, seam blending | S-M (1 week) | Medium, aesthetic: walls repeated on levels with sparse side walls (level 3: edges empty 59% of the time) |
| 9f | HD status panel and HD font replacement | S-M | Low |
| (no) | Real widescreen with objects visible off the old edge | L (game-code patches to spawn and delete limits) | High: changes gameplay, which the spec forbids |

## 8. What could not be determined

- **Visual review of every level's special draw handlers.** The level-module bosses and the level-5 objects
  far off-screen (x up to 527) were not reviewed visually. The capture links them to records, but whether all of
  them interpolate well was only checked by the statistics (jumps > 48 px are cuts).
- **The live window path** was measured in 9c (presents per second, missed refreshes, clock snaps; see the
  status at the top), but visual quality was judged from headless shots (`--modern-shots`, `--present-shot`),
  not by watching motion on the display.
- **The title zoom, menus and shop** are not gameplay steps, so they are shown at the original rate. Interpolating
  them would need their own drawing code identified.
- **Exact spawn rules.** The spawn x of side enemies comes from the level event data. The ranges above are
  measured, not read from the data format.
- **True intermediate frames.** These do not exist: the game computes no state between steps. The quality check
  is endpoint exactness plus linear motion by construction, not comparison with a ground truth.
