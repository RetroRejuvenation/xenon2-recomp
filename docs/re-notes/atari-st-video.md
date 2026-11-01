# Atari ST video and CPU timing (S3)

How the ST target draws its picture and counts its cycles, and how close that comes to Hatari 2.6.1 (EmuTOS 1.4 UK, 50 Hz) on the
Kixx release. Companion to `atari-st-machine.md`; the comparison tools are `tools/compare/hatari-harness` (the patched Hatari) and
`tools/compare/st_compare.py`. Nothing here is game data: the numbers are from the maintainer's own dump, identified by its sha1 in
`tests/st/st_kixx_tests.cpp`.

## The model

A frame is 313 lines of 512 CPU cycles (160,256 cycles, 50.0 Hz at 8,021,247 Hz). The machine runs the CPU a line at a time.

- **Vertical blank** (level 4) is raised at line 0.
- **Video counter** (`$FF8205/07/09`). The GLUE reloads it from the base register (`$FF8201/03`, plus `$FF820D` on the STE) at
  line 310, cycle 56, so a base written after that point shows one frame later. During the display (lines 63-262, line cycles 56-376, one
  pixel a cycle) it advances one 16-bit word every 4 cycles, 160 bytes a line; before and after the window it holds. Reads see it
  8 cycles after the reading instruction began (the bus read is late in the instruction; Hatari's own read adjustment), and a byte of the
  counter is read as part of the whole counter at that moment. The engine's page-flip wait polls `$FF8207`.
- **Picture.** Each display line is decoded once its 512 cycles have run, from the 160 bytes at the counter's value for that line
  (`counter_base + 160 * y`), through the palette the beam had at the start of the line; every palette write is logged with its cycle
  and applied from the pixel the beam had reached (pixel x is drawn at line cycle 56 + x). Low resolution only: medium and high resolution
  give a black line and a counter (`unsupported_resolution_frames`), nothing in the Kixx build selects them.
- **Bus rounding.** The ST's memory bus gives the CPU a slot every 4 cycles, so every instruction ends on a 4-cycle boundary
  (Hatari's default, "cycle exact" CPU). Musashi's table times are rounded up in the instruction hook with
  `m68k_modify_timeslice`; the burnt cycles are kept in `slice_pad_`, outside the core's own count, so `now()` inside an instruction is
  the instruction's start plus the pad. `StConfig::bus_rounding` turns it off. In the Kixx run it adds 5.6% to the cycles.
- **Device wait states** (`StConfig::io_waits`; Hatari `psg.c`, `mfp.c`, `acia.c`): the first YM2149 access of an instruction costs 4 cycles
  (and every 4th access of a `movem`), every MFP register access 4, every keyboard ACIA access 6 plus the wait for the next E-clock
  (a multiple of 10 cycles). The front-end's digitised-speech handler writes the YM with `movep` about 160 times a frame, so this alone is
  1.2% of the cycles and the difference between a 3.7% and a 1.3% drift in the front end's animation speed.
- **Not modelled:** the shifter/palette 4-cycle bus sync (every instruction already ends on the boundary it would wait for), HBL and
  Timer B (the Kixx build uses neither), contention between the CPU and the video fetch, the STE's blitter and DMA sound, borders,
  overscan, sync-mode and shift-mode tricks.

## Measuring against Hatari

`tools/compare/hatari-harness/run.sh` runs the patched Hatari headless with a scripted joystick (`inputs/*.txt`: `<vbl> JOY1_FIRE 1`) and
writes `frames.log` (one line per VBL: pc, sr, video base, watched RAM words), screenshots (832x552, the 320x200 picture at (48, 29) of a
416x276 raster at 2x) and RAM dumps. Ours writes the same with `--st-frame-log`, `--st-shots-dir DIR --st-shots 100,200-260,1000-2000/50`
and `--st-watch`; the same script drives both (`--input-script`, `--st-input-offset N` = the VBLs of Hatari's EmuTOS boot, 436 here).
`st_compare.py HATARI_DIR OURS_DIR --offset 436 --search W` compares every Hatari shot with our frames within W of the aligned one (guns
as 3-bit indices: Hatari draws a gun g as 34g, we as 255g/7), reports the best, and the shift it needed, because the two machines
run their busy loops at slightly different speeds.

## Results (Kixx, Hatari 2.6.1)

| scene (Hatari VBL) | frames compared | pixel match, best frame | frames at least 99.99% |
|---|---|---|---|
| Bitmap Brothers caption typing, logo, "GAME" (780-1100) | 320 | 1.0000 (min 1.0000) | 100% |
| Xenon 2 title screen (1100-1410) | 301 | 1.0000 (min 0.9275) | 99.7% |
| Level 1, ship idle, started with one fire pulse (3600-3980; `inputs/kixx-level1-nofire.txt`) | 381 | 0.9992 (min 0.9985) | 0.8% (96% at 99.9%) |

In level 1 about 50 of 64,000 pixels differ in each frame, scattered over the picture and not yet traced to a cause (small
sprites or the starfield, probably a step out of phase with Hatari's); the background, status bar, ship and enemies are identical.
The scenes between the title and the level (credits, menu, loading) wait for fire pulses, so their frame depends on the phase of the
pulse in the animation, not on the video model; they match when the pulse lands in the same phase and are not tabulated.

Pacing, which is what the video model is for: the engine runs its logic in steps of 3 VBLs and takes 4 when a step overruns. In level 1
the 4-VBL steps are 2.77% of the steps (41 of 1,479) against Hatari's 2.87% (35 of 1,219), mean step 3.0304 VBLs against 3.0320; with the
bus rounding alone they were 2.70%, and the front-end's fade ran 3.7% faster than Hatari's. With the wait states too, the front end still
runs about 1.3% fast (the best frame shift drifts from -4 to -16 frames over 900). The remainder is not attributed yet (interrupt entry and
exception timing are the first suspects: Musashi counts them from its own table); it does not change what is drawn.

## Re-recording the goldens

`X2_ST_PRINT_GOLDENS=1 ctest -R st_kixx -V` prints the video and audio hashes the checks compare against. The video goldens (frames 800,
1300, 2999) changed with S3 because the picture is now decoded line by line from the counter's base, and the audio goldens because every
YM write moved by the new cycle counts; the first level frame (2766 with the project's pulses) moved with them.

## Open

- The 1.3% above: a comparison of Hatari's cycle counter with ours over the front end's busiest loops would settle it.
- The shifter's palette write sync and a palette write that lands exactly on the 4-cycle bus boundary after a wait state.
- Image Works' Copylock reads the DMA address in its timing check (S7); whether it reads the video counter too is not established.
