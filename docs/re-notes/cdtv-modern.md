# CDTV release: the modernisation layer and the user interface (phase C8)

The picture options of the disk build (interpolated frames at the display rate, the widescreen surround, filters, the HD status
panel, replacement asset packs, the Faithful switch) work on the CDTV release too, on the interpreter and on the translated
executor. Faithful stays the default; `--faithful` (or the Faithful row of the F1 menu) switches everything off. No game bytes or frames are reproduced here: addresses,
counts and hashes only.

## 1. What had to change

The capture (`runtime/modern/draw_capture.*`) watches the engine's drawing: the two screen-buffer pointers the frame swap exchanges,
the five object-list heads, the sampled game variables, the star table, and the PCs of the routines that write BLTSIZE or a star pixel
(far layer, tiles, stars). The disk build's addresses were constants. They are now a `CaptureLayout` (`runtime/modern/layout.h`), one per
game build, carried by each captured `Step`; the interpolator, the asset dumper and the presenter read the layout from the step.
`kDiskLayout` holds exactly the old values (the disk, retail, Power Pack and crack builds share them; the 1762 compile only moved code after
`$D000`). `kCdtvLayout` holds the CDTV addresses.

### 1.1 How the CDTV layout was found

The CDTV engine is the disk build's code re-assembled with long absolute addressing, so its addresses drift while the instruction
sequences stay the same (`cdtv-c0.md`). `tools/modern/map_cdtv.py` maps addresses by instruction shape: both images are swept, each
instruction is reduced to its mnemonic and register operands (numbers and absolute-address sizes dropped), and every window of eight
instructions that occurs exactly once in each image anchors the mapping. `--vars` votes a variable address from the absolute operands
of every aligned instruction pair. Every value below got between 3 and 38 agreeing votes.

| What | Disk | CDTV |
|---|---|---|
| draw / show screen pointers | `$406` / `$40A` | `$408` / `$40C` |
| object list heads | `$A88 $AEA $B4C $BAE $C10` | `$A8E $AF0 $B52 $BB4 $C16` |
| ring row, scroll, map row, fine scroll, speed, frame counter, `$CEA`, `$CE8` | `$436 $CD0 $CCE $CD6 $CD8 $CDC $CEA $CE8` | `$43C $CD6 $CD4 $CDC $CDE $CE2 $CF0 $CEE` |
| star table (48 x 3 words) | `$42FDC` | `$85380` (hunk 1) |
| `run_object_list_draw` return address | `$3678` | `$3674E` |
| far layer blits (two ring copies, the clear of planes 2-3) | `$1832 $185A $1884` | `$34712 $3473C $34766` |
| tile routine range | `$18A0-$1EA2` | `$34784-$34DC6` |
| star routine range, group starts 2/3/4 | `$233A-$23EE`, `$2370 $2398 $23C2` | `$35260-$35318`, `$3529A $352C2 $352EC` |

Checked by running the game: in level 1 the CDTV run and the disk run capture the same kind of step (the same top blit PCs after
mapping, 188 against 208 blits per step, 24 against 27 object records, the draw buffer holding nearly all of them, the ring row and
scroll variables moving the same way), and the interpolation endpoints reproduce the real pictures exactly.

### 1.2 One rule that had to change

The interpolator replays the captured CPU writes of the star routine (`$233A`, "object_pass_a") with a per-pixel occupancy rule: a star is
only drawn on empty background, like the routine's own word-wide test. At an unshifted position the game has already made that test, so
the captured write was executed and must be applied as it is. The CDTV release's level 4 has star-table entries with multi-pixel
masks (the table at `$85380` holds other data there), where the per-pixel rule refused pixels the game had drawn; about 200 of the 842
interpolable step pairs of the level-4 replay were not exact at an endpoint. The rule now applies only where interpolation moved the star, and only for the CDTV layout
(`CaptureLayout::star_writes_exact`): the first version applied it to every build and changed four of the disk build's recorded
interpolated-picture hashes, which the goldens caught. All 842 pairs are exact on the CDTV release, and the disk build's goldens are unchanged.

## 2. Evidence

All on the real disc, interpreter and translated executor, no game data in the repository (hashes and scripts only).

- **Interpolation endpoints** (`--modern-check`, no far-layer smoothing): every interpolable step pair re-renders exactly at t = 0 and
  t = 1 on all 21 replays of the corpus that have gameplay (`tests/replays/cdtv/`; 235 to 1265 pairs each); `attract` and `menus` are
  title and menu pages only and have no pair to interpolate.
- **Captured picture against the rendered frame:** on the level-1 replay they are equal on every frame from 2372 on, i.e. through all of its
  gameplay; before that, in the front end, the program fades and colour-cycles through the copper and they differ. The other replays also
  show some differing frames, at level changes, shops and fades (the last one of each is listed by `--modern-check`), as the disk build's
  replays do; they were not examined one by one.
- **Faithful switch:** with `--faithful`, also together with a filter, interpolation and `--aspect`, all 302 presented pictures of a
  window of level 1 hash like the rendered frames.
- **Widescreen:** at 16:10, 16:9, 21:9 and 32:9 the centre 320 x 256 of every presented picture of that window is the rendered frame
  exactly, and with interpolation (21:9) the centre is exact at both endpoints of all 303 step pairs. By eye, the surround continues
  the scenery on levels 1 to 5 and the bosses without a visible seam or edge defect; the front-end pages (title, menus, shop) are shown
  faithfully, as in the disk build.
- **HD status panel:** the HUD is the disk build's and the edge-preserving upscale is clean.
- **Asset dump:** `--dump-assets` of the level-1 replay writes 105 sprites, 30 tiles and 192 far lines. Of the 109 graphics the disk build's
  level-1 dump holds, 108 have the same key in the CDTV dump (the key is a content hash of the graphic as drawn), the far layer and the
  palettes are identical: **an asset pack made for one release applies to the other.** The dump loaded back as a pack reproduces the
  interpolated pictures and, without interpolation, the rendered frames exactly.
- **Golden hashes** of interpolated pictures at t = 0, .25, .5, .75, 1 and of surround pictures: `tests/golden/modern_cdtv_midpoints.txt`,
  `tests/golden/modern_cdtv_wide.txt`.
- **Translated executor:** with the CDTV translation linked, the draw capture of a 300-frame window is byte-identical to `--interp-only`, and
  the presented pictures with a replacement pack, a 21:9 surround and the HD panel are identical to `--interp-only` (100.00% of the executed
  instructions ran as translated code).

CTest: `modern_cdtv_check_<replay>` (23), `modern_cdtv_game`, `modern_cdtv_faithful`, `modern_cdtv_wide`, `modern_cdtv_upgrade`,
`modern_cdtv_midpoints`, `modern_cdtv_wide_golden`, all skipped without the CDTV assets; the scripts are the disk build's
(`tests/modern_*.cmake`) with the assets guard widened (a CDTV import has `cd0/` and no `disc2.bin`) and the frame window passed in.

## 3. The interface

- **Which release starts.** `--game disk|cdtv|auto`, and the new `game = auto|disk|cdtv` key in `settings.cfg` for a double-clicked
  application with nowhere to type an option; the command line wins. `auto` starts the disk release when it is imported, else the CDTV
  release; with both imported in a window it prints one line saying which started and how to start the other. A disc dropped on
  the import window is imported and started (`runtime/import/import_screen.cpp`, C3).
- **Window title.** `Xenon 2 Megablast (CDTV) - ...` for the CDTV release, then the picture mode and the active assists, as before.
- **Settings are per user, not per build.** `settings.cfg` (picture, audio, volumes, assists) and `input.cfg` (bindings) are shared by
  both releases; the high-score tables are separate files (`highscores.bin`, `highscores-cdtv.bin`: a table file carries the hash of its game image, so a shared name would make each release discard the other's table). The CDTV-only second button is the `button2` binding.
- **Bindings.** The same keys and pad buttons as the disk release; `button2` (default `C`, a gamepad shoulder button) fires the CDTV
  release's special weapon and is ignored by the disk release. The CDTV remote control never reaches the game (`cdtv-c0.md` section 4), so
  nothing is mapped to it; a joypad does what the keys do. `--list-keys` says both.
- **Language select.** The program's own page, driven by the same joystick and fire input as its other menus (the keys or a pad); not checked
  by hand. The noise in the shop's empty item cells is the game's own effect (`cdtv-reference-whd.md`).

## 4. Not covered

- Hand-playing with modernisation on (nobody has played the CDTV release through yet).
- Interpolation quality on the level-2 and level-3 bosses and on the INTRO pages: the check is exact at the endpoints, the midpoints are
  golden-hashed on levels 1, 2, 3, 5 and the level-1 boss, and looked at, but the motion model was built for the disk build's objects.
- The CRT, scanline and sharp filters are applied by the window (SDL), so they are checked by eye only, not by a headless test.
- A pack made from the other release's graphics matches 108 of 109 level-1 keys; the one that differs and any graphic only the CDTV
  release draws (its own pages, which are not gameplay) need their own replacements.
