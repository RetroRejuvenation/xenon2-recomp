# Reference comparison: runtime vs Amiberry (Phase 4)

Question: does the runtime show the same pictures, at the same rate, with the same sound as a reference emulator
running the original disks? This answers the Phase 4 open questions (logic rate, stereo balance, title/menu/level 1
parity, divergence in game state).

**Short answer:** the runtime matches the reference closely.

- **Pictures:** at every checkpoint from the title to 1,400 frames into level 1, the picture matches the reference
  except for 0.06-0.11% of pixels. All of those are in the twinkling starfield.
- **Colours and geometry:** the palette is identical, and the picture sits at a fixed offset in Amiberry's buffer.
- **Logic rate:** the game swaps pictures every 3 VBLs (16.5 pictures/s) on the reference too.
- **Stereo:** the right channel is about 2.5x louder than the left on the reference too. This is the game's own mix,
  not a runtime bug.

The remaining differences are timing differences: starfield twinkle, HUD updates, and sound effects landing 1-2 VBLs
apart. They are listed in section 5.

## 1. Reference setup

| Item | Value |
|---|---|
| Emulator | Amiberry v7.0.8 (git tag `v7.0.8`), built from source with `tools/compare/amiberry-harness/` (headless harness, see section 7) |
| Machine | A500, 68000, OCS (`chipset_compatible=A500`), 512K chip RAM, no slow or fast RAM, PAL. Cycle-exact CPU, memory and blitter. Floppy at 100% speed. |
| ROM | Kickstart 1.3 rev 34.5 (A500, CRC32 `C4F0F55F`), a user-supplied ROM file. Not in git. |
| Disks | `Xenon2.zip`: disc 1 `45e66f9c…` in DF0, disc 2 `a748afc7…` (the same disc 2 the runtime uses) in DF1 |
| Audio | Paula mix tapped before the host output: 48 kHz stereo, stereo separation 100%, interpolation off, filter "emulated / standard" (A500) |
| Video | lores, no line doubling. The native 377x288 buffer is saved as PNG, one pixel per lores pixel. |

The original boot path runs on the reference:

1. KS 1.3 boots disc 1.
2. The `startup-sequence` runs `add21k`, then the BS1 cracker intro, which needs a mouse click.
3. The BS1 **trainer menu** comes next. Its options default to ENDLESS POWER = YES and ENDLESS CREDITS = YES. The
   input script turns both off with the mouse (stub at `$70000`: the mouse Y counter `JOY0DAT` picks the item,
   and the left button toggles it). The flags at `$70232`/`$70236` were read back as 0 from a chip RAM dump, so the
   game runs unpatched, as the runtime does.
4. `XENON-II` runs.
5. At the first disc access the game asks for the game disc. The script inserts disc 2 into DF0 at frame 6400,
   before that point. This is the user's disc swap, which the runtime's disk shim makes unnecessary.

## 2. Inputs and alignment

Both runs use the same game inputs: joystick-2 fire held for 5 frames at title +460, then at +950 in the SELECT menu,
then at GET READY.

- Runtime script: `--fire 460:5 --fire 950:5 --fire 1200:5`.
- Amiberry script: `tools/compare/amiberry-harness/inputs/title_to_level1.txt`. It also holds the intro click, the
  trainer menu input and the disc swap.

The two runs boot differently, so they are aligned on game events:

| Event | Runtime frame | Amiberry frame | Offset |
|---|---|---|---|
| Game copied to `$400` by the stub (chip words `$404..` cleared) | n/a, starts there | 6354 | |
| VBL handler running (`$41C` starts counting) | | 6425 | |
| Title music first audible | 3 | 6426 | **6423 = title-phase offset** |
| Fire (SELECT) | 460 | 6883 | 6423 |
| Fire (1 PLAYER GAME, starts the level load) | 950 | 7373 | 6423 |
| GET READY zoom starts | 1136 | 8270 (±10, from 10-frame sampling) | 7134 |
| Fire at GET READY | 1200 | 8334 | 7134 |
| Level-1 music first audible | 1272 | 8407 | **7135 = level-phase offset** |

- **Load time:** the level load from disc 2 took **897 frames** on the reference (7373 → 8270) against **186** in
  the runtime (950 → 1136). The runtime serves sectors instantly, so it is 711 frames (14.2 s) faster. This is the
  only place where the two timelines diverge, and it is expected.
- **Countdown:** the reference shows a sector countdown under "LOADING LEVEL 1" (8, 6, 3, ...). The runtime's
  load is instant, so it only shows the zoom.
- **Fire to level music:** 72 frames in the runtime and 73 on the reference.
- **Frame numbering:** a reference frame N is the picture saved when Amiberry's `vsync_counter` became N. That is
  why the frame search below tries ±3 frames.

## 3. Pictures

How the comparison was done:

- **Geometry.** The free search (scale and offset) placed the runtime's 320x256 picture at **(37,18)** in Amiberry's
  377x288 buffer, at scale 1x1. Every checkpoint below uses this fixed geometry.
- **Tolerance.** A pixel differs when any channel is more than 16 apart.
- **Frame shift.** Each checkpoint is compared at the mapped reference frame and at ±1..3 frames. The table shows
  the best shift and the difference at shift 0. A picture stays on screen for 3 VBLs, so a shift within ±2 means
  the same logic step.

| Checkpoint | Runtime / ref frame | Shift | Differing % | Exact-colour % | Differing % at shift 0 |
|---|---|---|---|---|---|
| title, logo + trainer credit | 150 / 6576 | +3 | **0.000** | 100.000 | 4.095 |
| title | 300 / 6720 | -3 | 0.089 | 99.911 | 0.090 |
| SELECT zoom-in | 470 / 6893 | 0 | 0.106 | 99.894 | 0.106 |
| SELECT menu | 700 / 7123 | 0 | 0.084 | 99.916 | 0.084 |
| GET READY PLAYER 1 | 1190 / 8325 | 0 | 0.112 | 99.888 | 0.112 |
| level 1, music +28 | 1300 / 8436 | +1 | 0.073 | 99.927 | 21.2 |
| level 1, +128 | 1400 / 8535 | 0 | 0.059 | 99.941 | 0.059 |
| level 1, +328 | 1600 / 8737 | +2 | 0.077 | 99.923 | 22.7 |
| level 1, +728 (GET READY after the first life is lost) | 2000 / 9133 | -2 | 0.107 | 99.893 | 0.110 |
| level 1, +1128 | 2400 / 9536 | +1 | 0.105 | 99.895 | 0.107 |
| level 1, +1427 | 2699 / 9832 | -2 | 0.110 | 99.890 | 0.111 |

What the results show:

- **Palette.** It is identical. Exact-colour matches equal the 12-bit matches everywhere, so both expand 4-bit
  colour with x17. No colour is remapped.
- **Remaining pixels.** All of the 0.06-0.11% are starfield dots. The diff images
  (`generated/ref/compare/diff_*.png`) show red only on stars. The playfield, enemies, the player ship, the HUD and
  the text are pixel-identical.
- **Game state.** The game state matches across 1,400 level frames with no input:
  - the scroll position, enemy waves and score;
  - the player is killed at the same point;
  - the GET READY after the first lost life comes up at the same place.

  Nothing in the tables shows a divergence.
- **Large shift-0 differences** (21-23%) appear where the two runs are one logic step apart at that frame. A shift
  of 1-2 frames removes them. See section 5.

## 4. Logic rate and cadence

Two measurements:

- From the frame log: when COP1LC changes on the reference. The game reloads COP1LC at every buffer swap, `$202E`.
- From the PNG sequences: how many VBLs each distinct picture stays on screen, for both runs.

| Source | Window | VBLs per picture or swap (count) | Rate |
|---|---|---|---|
| Amiberry COP1LC, level 1 | 8435-9834 (1,400 frames) | 3: 459, 4: 3, 5: 1 | **16.54 swaps/s** |
| Amiberry PNG, level 1 | 8435-9834 | 3: 440, 4: 3, 5: 1, 2: 20, 1: 20 | 17.33 changes/s |
| Runtime PNG, level 1 | 1300-2699 | 3: 460, 4: 4 | 16.62 pictures/s |
| Amiberry COP1LC, title | 6573-6872 | 3: 69, 4: 10, 5: 6, 6: 1, 7: 2 | 14.67 swaps/s |
| Amiberry PNG, title + SELECT | 6423-7200 | 3: 198, 4: 15, 5: 11, 6: 4, 7: 4, 9: 1 | 15.06 changes/s |
| Runtime PNG, title + SELECT | 0-777 | 3: 207, 4: 16, 5: 8, 6: 4, 7: 2, 8: 1 | 15.38 pictures/s |

**Answer (a):** yes. On the reference the game advances one logic step, with one buffer swap, every **3 VBLs**:
16.67 Hz nominal and 16.5 Hz measured in level 1. The few 4- and 5-VBL steps are load slowdowns. The title has more
of them because of the logo zoom. This confirms `timing.md` (`$410 = 5`, CIA-A CRA bit 7 reads 0); the frame log
shows `$410 = 0005` on the reference. The runtime has the same distribution, with slightly fewer slow steps.

The 1- and 2-VBL "pictures" on the reference are not extra logic steps; COP1LC does not change. They come from HUD
changes in the displayed buffer one VBL after a swap (section 5).

## 5. Differences found

1. **Starfield twinkle (all checkpoints, 0.06-0.11% of pixels).** The star dots differ between the runs, and
   nothing else does. Stars are probably drawn from a pseudo-random source or a beam-position value whose state
   differs between the two runs. Not yet traced (follow-up: find the star routine and its seed).
2. **Logic-step phase drifts by 1-2 VBLs during level 1.** The best frame shift moves between -2 and +2 while
   the content still matches. On the reference a step sometimes takes 4-5 VBLs, under load, at moments where the
   runtime's step still fits in 3, and the other way round. The runtime has no CPU/DMA bus contention (see
   `timing.md` §5), so its step times differ slightly. The game state is not affected, because it moves per step,
   but the step boundaries do not land on the same VBLs.
3. **HUD updates land one VBL later on the reference.** Around reference frames 8689-8748 (runtime ≈1554-1613,
   while score and energy change), the score digits and energy bar change one VBL *after* the buffer swap, inside
   the displayed buffer (3-51 pixels, HUD rows 225-229). In the runtime they change together with the swap. This
   is the same CPU-speed effect: on the reference the main loop is still running when the next VBL starts. A
   cycle-faithful CPU/DMA timing model would be needed to reproduce it.
4. **Sound-effect timing.** Over 100-frame windows, the level-1 audio envelope correlates at 0.98-0.999 while only
   music plays. It drops to 0.68-0.91 in the windows with the player's death or explosion effects (1560-1660,
   2060-2160, 2460). That is consistent with item 2: effects are queued per logic step, so they start 1-2 VBLs
   apart.
5. **Output level.** The runtime's output is 0.755-0.759x the reference level. This is the output gain chosen in
   `runtime/audio/paula.h`, against Amiberry's mixer. It is not a game difference.
6. **Low-frequency balance.** The reference has slightly more energy below 100 Hz: 18.1-18.6% against 15.8-16.0%,
   with correspondingly less at 100-500 Hz. This points to a difference in the DC high-pass or the A500 filter
   model. It is small, and has not been followed up.

## 6. Audio and stereo balance

| Segment | Envelope correlation | Right/left RMS, reference | Right/left RMS, runtime |
|---|---|---|---|
| Title music (447 frames) | **0.9988** (relative envelope error 0.026) | 2.449 (median 2.568) | 2.441 (median 2.535) |
| Level-1 music and effects (1,418 frames) | 0.907 (0.98-0.999 in music-only windows) | 2.660 (median 2.607) | 2.678 (median 2.683) |

How the audio was aligned: the reference WAV was cut on the emulated frame grid using the per-frame sample
positions in `frames.log`. Amiberry's host-synced sound clock gives 961-965 samples per frame. The runtime WAV was
cut at its constant 958.46 samples per frame. Both were then resampled to 960 samples per frame, with no further lag
from cross-correlation.

**Answer (b):** the reference has the same about 2.5x louder right channel: 2.45x on the title and 2.66x in level 1.
The runtime matches it to within 1-3%. The imbalance is the game's own mix, so it should stay. The driver plays
AUD1/AUD2 (right) at volume 64 and AUD0/AUD3 (left) at 32 (`audio-notes.md` §4). The music starts on the same frame
in both, measured from the game's first VBL.

## 7. Reproducing

Prerequisites:

- Your own `Xenon2.zip` and an A500 Kickstart 1.3 ROM file.
- Amiberry v7.0.8 installed in `/Applications`. Its bundled SDL2, SDL2_image and SDL2_ttf dylibs are copied, not
  changed.

```
# 1. build the harnessed Amiberry under generated/ref (clone, patch, configure, build)
sh tools/compare/amiberry-harness/build.sh
# 2. disks: unzip Xenon2.zip into generated/ref/adf/; ROM: KICK=/path/to/kick13.rom (default
#    ~/Amiberry/Roms/x2recomp-kick13-34.5-a500.rom)
sh tools/compare/amiberry-harness/run.sh "$PWD/generated/ref/runs/p3" 9834 6423-7200,8300-9834 \
   "$PWD/tools/compare/amiberry-harness/inputs/title_to_level1.txt"
# 3. the runtime side (frames 0-777 and 1180-2700, WAV)
./build/x2recomp --headless --frames 2701 --no-reglog --fire 460:5 --fire 950:5 --fire 1200:5 \
   --audio-out generated/ref/ours/full/run.wav --screenshot-at <F>:generated/ref/ours/full/f<F>.png  # one per frame
# 4. compare (writes generated/ref/compare/{results.jsonl,generated.md,diff_*.png})
python3 -I tools/compare/phase4.py
```

How long it takes:

- The reference runs in real time (50 fps), about 3.5 minutes to frame 9834. Most of that is booting through
  Kickstart, the intro and the trainer menu, plus the real disk load.
- `phase4.py` takes about 40 s.

The harness (`x2ref-hooks.patch` + `x2ref_harness.inc`) adds environment-variable hooks to Amiberry and does
nothing when `X2REF_DIR` is unset. It covers:

- PNGs at listed frames;
- a per-frame log of COP1LC, the WAV sample position and chip words `$404 $41C $410 $406 $40A $700E`;
- chip-RAM dumps;
- a WAV tap of the Paula mix;
- a frame-indexed input script: mouse/joystick buttons and directions, relative mouse motion, raw keys, and the
  DF1→DF0 disc swap;
- quit at a frame;
- the SDL dummy video and audio drivers with the software renderer, so nothing appears on the desktop or speakers.

The rest of the patch makes v7.0.8 build on macOS without libserialport (one missing `#ifdef SERIAL_PORT`),
without dylibbundler, and against the app bundle's SDL dylibs.

`tools/compare/compare.py` (stdlib only) does the individual measurements:

- `frame`: geometry search, tolerance diff, colour remaps, diff PNG;
- `cadence`;
- `audio`;
- `wavinfo`;
- `cutwav`;
- `report`.

## 8. Limitations

- **One run, no input in the level.** Only one reference run was captured. The level-1 comparison covers 1,400
  frames with no player input, during which the ship is killed. Steering, shooting, the shop, bosses and later
  levels are not covered yet.
- **Alignment.** The runs are aligned on events (music starts and fire presses), not by cycle. Inside a logic step
  the frame mapping is only good to ±2 VBLs (section 5, item 2). GET READY timing on the reference was read from
  10-frame samples (±10).
- **The reference is an emulator.** Amiberry is not real hardware. Its cycle-exact A500 model is widely trusted,
  but "matches Amiberry" is not proof of matching an A500.
- **Mid-step palette writes.** Fades and the pickup flash write the palette from the main loop at any beam
  position. They are only compared at the sampled checkpoints, not line by line.
