# Asset packs: backgrounds, stars, hit flashes and the pause hold (workstream WS2b)

Status: implemented, on top of WS1b's loader: the player reads `backgrounds`, `stars`, `f:` flash keys, `flash_png`/`flash_rect`, `keys` aliases and atlas frames through
`pack::PackStack` (directory or ZIP); no change to `runtime/pack/` was needed. `x2_modern_tests` exercises that loader (`test_stack_features`) and the data tests run each pack as a
directory and as a ZIP. This is the engineering note for the gameplay-graphics half of WS2 in `docs/re-notes/asset-packs-v2-design.md` (sections 4.3, 4.6, 4.7 and the
measure items of section 10). The screens and the status panel are WS2a (`asset-packs-screens.md`); the general loader is WS1b (`asset-packs-loader.md`), which reads the
`backgrounds`, `stars` and flash entries this note describes. The user-facing text is `docs/ASSET-PACKS.md` (WS6); the format is `docs/schema/x2-asset-pack-v2.schema.json`.

A pack can now give a level one picture for its whole far layer (`backgrounds`), draw the starfield as round discs (`stars`), and replace the white flash of a hit enemy
(an explicit `flash_png` or `f:` entry, or a flash derived from the replacement). The replaced graphics also stay up during a game pause. Nothing here changes machine
state: the game runs exactly as without a pack, the Faithful switch (F1, `--faithful`, `--modern faithful`) bypasses all of it, and a run without a pack is byte for byte the
run of before (the existing interpolated-picture goldens, default and translated builds, are unchanged).

## 1. Measure items

### The far layer's ring (disk builds and the CDTV release)

The far layer is drawn from a ring buffer of **192 lines** (planes 0 and 1, 80 bytes per line, 40 per plane). Every logic step the game copies the whole ring into the
screen buffer with two blits: `far_copy_a` (disk `$1832`, CDTV `$34712`) draws `192 - ring` lines from ring line `ring` to the top of the buffer, `far_copy_b` (`$185A`,
`$3473C`) draws `ring` lines from ring line 0 below them, and `far_clear` (`$1884`, `$34766`) clears planes 2-3. So **screen line `y` shows ring line `(ring + y) mod 192`**,
with `ring` the variable `kRing` (disk `$436`, CDTV `$43C`).

| Check | Disk (level 1 replay, steps 112-172) | CDTV (level 1 replay) |
|---|---|---|
| blit heights over 120 steps: copy_a / copy_b | 192 - ring / ring, every step | 192 - ring / ring, every step |
| the ring variable at the swap equals the blit's `ring` | 120 of 120 steps | 120 of 120 steps |
| ring change per step | -1: 60, 0: 59 (falls from 172 to 112) | -1: 58, 0: 60, **+1: 1** (falls from 146 to 89) |
| distinct far lines the dump finds in a level | **192** (all different), none ever rewritten | 192 per level, except level 4 (below) |

- **Height 192**, so `CaptureLayout::far_ring_lines` is 192 on both builds (the ST is not measured: it has no capture path).
- **Scroll sign -1**: the ring falls as the level advances, so the picture moves down the screen (`far_scroll_sign = -1`). On the CDTV release the ring now and then goes
  up by one line (the layer's speed is not constant), which is why the interpolator's far-layer smoothing exists.
- **The layer is a 192-line picture that repeats.** The ring's content is written once when the level loads and never changes on the disk builds (every replay of levels
  1-5, the boss and the ending: 192 lines per level). **CDTV level 4 is the exception**: its replay finds 108 far lines more than the two levels it passes through
  have (492 against 384) and the dump sees 408 ring lines change content: in the arena of its boss the layer is nearly blank. A single picture cannot be exact for that level (section 3).
- A pack picture taller than 192 lines (the `ring_lines: 1024` of the design's example) is therefore not supported: the ring has 192 positions, so a taller picture would
  show only its first 382 lines and jump when the ring wraps. `ring_lines` is accepted when it is 192 (or absent) and the picture is `320 x scale` by `192 x scale`; anything
  else is ignored with one message on stderr (the validator, WS1b/WS7, should reject it at load time).

### The level word

`$E04` on the disk builds and `$E0A` on the CDTV release hold the level, **1 to 5** (read at every swap into `Step::level`; it is not in `var_addr`, so the draw log is
unchanged). Checked on the five level replays and the boss replay of each build: every run shows the levels it passes through (the replays that poke the game ahead show
the levels in between for a few steps), and each level has its own far layer.

### Hit flashes

A flashed bob is not drawn through its picture. The game draws **the bob's mask as a flat colour**: four plane blits of one object (minterm `$FA` sets the plane's bit where
the mask is, `$0A` clears it, channels A (the mask), C and D, `BLTCON0` low bits `$BFA`/`$B0A`), in practice 3 x `$FA` + 1 x `$0A` = colour 7 (planes 0-2 set). Facts:

- A flash is **one logic step long** and replaces the normal draw: in the flash steps looked at, the same object has no cookie-cut group, and in the steps around it
  the object is drawn normally. Level 1 (disk): 3 groups in the 1,460-frame replay (3 mask shapes); the boss-1 replay: 18 groups, 8 shapes; CDTV level 1: 4 groups.
- The A pointer of the flash blits is the same address (`$0EBFA`) in every flash of the boss-1 replay while the graphic changes, so the **source pointer does not tie a flash
  to its graphic** as the design hoped. What can: the A data. The mask words of a flash are exactly those of the bob's own mask, so the **shape of the drawn mask** (width, height and which pixels of
  the bounding box are drawn, independent of the blitter shift) is the same for the bob and for its flash. Together with the **object record** (`BlitCmd::obj`) this ties a
  flash to the graphic its object drew last: 17 of 18 flashes of the boss-1 replay find their graphic (the other is the first thing that object ever drew), and 4 of 4 on CDTV.
- The pack key of a flash by itself is `f:<shape hash>-<w>x<h>` (`Graphic::shape`, FNV-1a 64 of 'M', width, height and one drawn/not-drawn byte per pixel of the bounding box).

### The pause hold

`P` pauses the game: the logic steps stop (level 1 replay with `P` at frame 1300 and again at 1330: the last step before is swapped in at 1299, the next at 1330) and the
display keeps the last frame; **no "PAUSED" is drawn**. The presenter, however, falls back to the faithful frame after `hold_frames` (8) without a new step, so before this
work **every replaced graphic reverted to the original, and the widescreen sides went black, for the whole pause** (it was already documented for the sides). The same
fallback is right for a disk load or a menu, where the frame on display is no step's picture.

## 2. Design as built

### The level number and the layout

`Step::level` is sampled at the swap from `CaptureLayout::level_var`. `far_ring_lines` (192) and `far_scroll_sign` (-1) are set for the disk and CDTV layouts.

### Backgrounds

`InterpOptions::packs` is the installed `pack::PackLibrary` (set by the presenter from `pack::packs()`; `assets` still serves sprites, tiles, far lines and palettes).
While planning a step, `background_for` looks up the entry of `Step::level` and checks the picture (`320 * scale` wide, `192 * scale` tall, `ring_lines` 0 or 192). For every
far line the step drew (one per screen line `y`) the plan paints picture row `(ring + y) mod 192` (`* scale`) after the last far-layer command, exactly where a `far` line
would go, and instead of it: **the background wins over `far` entries for its level** (design 4.3). It moves with the far layer's interpolated position like a far line.
Everything drawn after the far layer (tiles, stars, objects, their replacements) is drawn over it as before, and the layer's own replay into the indices is unchanged, so the
game's later tests (the star routine's "empty background" test) see what they always saw. Pixels the picture leaves transparent show the original far layer.

Edge strip: when the far layer is shown moved by part of a line (interpolation), the top or bottom canvas rows the move uncovers keep the step's own picture, with or
without a pack (they are the step's finished picture). With a background this is a strip of up to half a line (2 pixels at 4x) at the very top or bottom of the playfield on
some pictures. Painting those rows from the picture would be better looking, but it would make the dump loaded as a pack differ from the original there, and that identity
is the contract (section 4); a later change can opt in.

The playfield's panel lines (192-199) are never painted by the layer.

### Stars

`stars.shape: round` (`size` Amiga pixels, `softness` 0-1) replaces the square pixel of each plotted star. The star routine's CPU writes are replayed as before (the game's own
"empty background" test still sees the original pixels); while replaying, each star block records the canvas bytes under it. After the replay the stars that are still
visible (the block's centre still belongs to the star layer) are put back to what was under them and a disc is composited over the picture: `alpha = clamp((R - d) / edge +
0.5)` with `R = size * scale / 2` canvas pixels and an edge `softness * R` wide (at least one pixel). The colour is the star's palette index through the palette's `stars` layer,
so `palettes` overrides still apply. Discs do not cover tiles or objects (`layer` of the canvas pixel). At scale 1 and size 1 the disc is exactly the original pixel. The
widescreen surround's extra stars use the same disc. A disc reaches across band edges, so a render with round stars uses **one band** (one thread): about 5.7 ms per frame at
4x against 0.6 ms (section 5). The `pixel` shape (default) is the original, byte for byte.

### Hit flashes

`find_graphics` returns four consecutive flash blits as a `GfxKind::Flash` graphic (footprint from the mask, colour from the plane minterms, `shape` hash). While planning a
step the interpolator learns, per object record and mask shape, the graphic that object drew normally (`ties_`), and for a flash group looks up, in this order:

1. an explicit entry for the mask shape (`f:<shape>-<w>x<h>` in `graphics`; `PackLibrary::flash`): picture, scale and anchor of its own;
2. the `flash_png` of the graphic the object drew (`GraphicEntry::flash`, `flash_scale`; the anchor is the graphic's, rescaled);
3. the replacement picture of that graphic, **blended 70 % toward white** with its alpha kept (premultiplied: `c' = 0.3 c + 0.7 a`), when the pack replaces that graphic;
4. otherwise the original flash stays.

It is composited as a cookie-cut replacement is (over what is underneath, before its own blits, opaque on the original's footprint). The ties are learned from the steps that
are rendered, in order, so the result of a flash depends on the normal steps shown before it (deterministic for a replay, which renders every step).

### The pause hold

`InterpPresenter::frame` remembers the newest step when it refused it only because of the hold (`held()`); `Presenter::at` then keeps the composed picture
(`frame_held`: that step's own picture with replacements and surround) **if the frame on display still is that step's picture**: the 320 x 192 playfield of the faithful
frame compared pixel for pixel with the step rendered at 1x, cached per step and frame content. A pause passes; a load, a menu or a shop does not, and shows the faithful frame as
before. Only when packs or the surround are on.

### The dump (`--dump-assets`)

- `far_full/levelN.png`: the level's far layer, the ring unrolled (ring line `r` at row `r`, 320 x 192 at scale 1, the brightest palette seen, lines never seen transparent),
  and a `backgrounds` entry in the version 2 manifest (`scale: 1`, `ring_lines: 192`).
- A level whose far layer the game rewrites during the level (CDTV level 4) is written to `far_full/` as the last picture of each line but **not listed** under
  `backgrounds`, with a warning, so the dump stays exact (its content-keyed `far` lines are).
- `flashes/<shape>-<w>x<h>.png`: the flash silhouette in its own colour, and `f:` entries in `graphics` (kind `flash`).
- The summary line is now `dump-assets: S sprites, T tiles, F far lines, N flashes, L far layers, P palettes -> ...`; a run with packs that used these features ends with
  `modern packs: B steps with a background, N hit flashes replaced, R round stars` (the counts are per plan or render, so lower bounds; the data tests use them).

## 3. Limits (to document for authors)

- A background replaces the far layer of the whole level, including where the game itself blanks or rewrites it. On the disk builds that never happens in the replays; on
  the CDTV release level 4 does it for its boss (the original far layer is nearly empty there; a background shows over it).
- The picture is the repeating 192-line layer. Painting a taller continuous scene means working on the 192-line cycle (the original art does the same).
- The edge strip, above.
- Replaced graphics do not follow the level's palette fades (an existing limit of every replacement), so a background is shown at full brightness while the game fades.
- Flash ties need the graphic to have been drawn normally by that object earlier in the run; the first appearance of an enemy that is hit at once keeps its original flash.
- Round stars render on one thread. `size` is the loader's `size_px` (0.25-8 Amiga pixels, fractions allowed).
- The schema and the loader accept `ring_lines` up to 4096; only 192 can work (section 1), so the interpolator ignores a background whose `ring_lines` is not 192 (one message on
  stderr); the loader itself already rejects a picture whose height is not `ring_lines x scale`.
- Atari ST: backgrounds, stars and flashes are not available (no capture path), as the design says.

## 4. Evidence

- `x2_modern_tests` (synthetic chip RAM, no game data): picture row `(ring + y) mod 192` at scales 1, 2 and 4 on two steps including the wrap and the panel line; other level
  and wrong-size pictures ignored; a quarter-line move (edge strip, then the picture one pixel lower); round stars (centre, corner, soft halo at size 3, pixel shape equal to
  plain, threads equal to one band); flash detection and its `shape` against the bob's, derived flash colour, `flash_png` over derived, `f:` over both, replication at 2x, no
  tie on first sight; the dump (`far_full`, `backgrounds`, `flashes`, `f:` keys, warnings); the pause hold (composed while the frame is the step's picture, faithful when it
  is not).
- `test_stack_features` (real `PackStack`): an alias in `keys`, an atlas frame with its `flash_rect`, an explicit `f:` entry layered above it, a background and round stars, the last
  three from a directory and from a ZIP; a background of the wrong height is refused by the loader.
- `modern_backgrounds` and `modern_cdtv_backgrounds` (data, Skipped without the assets), level-1 replays, each pack also run as a ZIP archive with identical pictures: the dump's far layer **alone** as a `backgrounds` pack gives exactly
  the 362 interpolated pictures of the plain replay (101 and 101 steps used it) on both builds, which fixes the ring binding and the sign; a background declared with another ring height
  is not used; the `pixel` stars change nothing; round stars change every picture, repeatably (8,438 / 8,994 stars drawn); the whole dump as a pack reproduces the pictures with
  3 / 4 hit flashes replaced by their own silhouettes; without the `f:` entries the flashes are derived (2 / 4 replaced) and only 7 / 16 of 362 pictures differ, the flash
  frames. On a translated build the round-star pictures are identical under the translated executor and `--interp-only`.
- Across a level change (boss-1 replay, 2,881 pictures) the dump as a pack differs from the plain replay in 301 pictures, 414 without the dump's flash entries and the same 414 without
  its backgrounds: the backgrounds add no difference; the rest is the existing fade and boss-graphics limit of every replacement. CDTV level 4 (whole replay): the dump
  without a listed background differs in 276 of 7,201 pictures against 288 when the far_full picture is listed, which is what the "rewritten layer" exclusion avoids.
- Existing goldens (`modern_*`, `replay_*`, the screens tests, `modern_check_*`): unchanged, default and translated builds.

## 5. Cost

Render time per interpolated frame (level-1 replay, translated build, 8 threads where banded):

| Canvas | no pack | background | round stars | the whole dump |
|---|---|---|---|---|
| 2x | 0.35 ms | 0.58 ms | 2.87 ms | 0.58 ms |
| 4x | 0.61 ms | 1.06 ms | 5.68 ms | 1.24 ms |

A run without these features has the same cost as before (a null check per render).
