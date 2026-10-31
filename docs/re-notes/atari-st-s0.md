# Atari ST S0: what the Kixx engine needs from the machine

Phase S0 of the Atari ST target plan: drive the Kixx budget release into play inside the `stemu` harness and settle the unknowns that
decide the machine design. The image is the Kixx re-release of *Xenon II: Megablast* (`.st`, 819,200 bytes, sha1
`323d6859635b6b82c86363db048b26e0f3df994c`, a public preservation dump). No game bytes, listings or dumps are in this repository;
this note records measurements and addresses only. Addresses are real memory addresses of the running game (the resident engine
is loaded at `$400`).

## 1. Method

- `tools/acq/stemu.c` (Musashi 68000 core) gained an `--st` mode: CPU-cycle timing (8,021,247 Hz PAL; 313 lines x 512 cycles =
  160,256 cycles per frame), vertical blank at level 4, the MFP 68901 (timers A-D, IER/IPR/ISR/IMR, VR, GPIP, level 6, automatic
  and software end of interrupt), the keyboard ACIA 6850 with an IKBD model (joystick packets, `$14`/`$15`/`$16`/`$1A`, one byte per
  10,267 cycles), a cycle-based video counter, a multi-sector WD1772 through the DMA registers, the TOS hand-over state (VR `$48`,
  ACIA control `$96`, Shifter low resolution, base `$78000`), and logging: interrupts, frame log, YM writes, palette writes, FDC
  commands and DMA destinations, IO summary, writer-PC census, exception/unmapped/odd-access watch, an execution-time profile.
  Without `--st` the tool behaves as before.
- `--ikbd SCRIPT` injects joystick and key events by frame (`<frame> jtap <port> <state-hex> <frames>`, `<frame> tap <scancode-hex>
  <frames>`); `--shot` writes `STSHOT` files that `tools/acq/stshot.py` turns into PNGs.
- The game was played with a fire-only script (no steering), so the ship dies and the run cycles through level 1, the continue
  screen, game over, the high-score screen and the attract loop. The longest run was 14,000 frames (about 280 s of game time).
  No exception vector target was executed, no unmapped or odd-address access happened, no TRAP was executed, and no write
  command was sent to the disk controller.
- **A harness defect found and fixed on the way:** Musashi burns every remaining cycle of a slice on a branch to itself. The
  harness ran 100,000-cycle slices, so a program waiting in `bra.b *` lost every Timer A and keyboard event inside the slice (the
  front end's sample player ran at about 1/100 of its rate in the first runs). Slices are now bounded to the next timed event and
  to one scanline. Numbers below are from the fixed harness; the cadence and busy-fraction figures of the first runs were wrong
  and are superseded. The lesson for the machine core: time-driven devices must be scheduled by cycle, never by slice.

## 2. Verdict

| Question | Verdict |
|---|---|
| Option A (new `StMachine`, emulated boot of the real loader on an emulated WD1772, no pre-link) | **Go.** The game boots and plays with a boot sector, 846 single-sector reads, the MFP, the ACIA and the YM. No TOS call, no TRAP, no exception, no GEMDOS/BIOS dependency. |
| Line-granular video model (Shifter state sampled per scanline) | **Go.** Palette writes land at arbitrary lines in some frames (section 5); the base address is written mid-frame and latched at the vertical blank; no HBL, Timer B or sync-mode tricks. |
| Cycle budget | The engine uses on average 44% of its 3-VBL step (481,000 cycles) at nominal 68000 timing, p99 91%, max 100%. Stock-ST bus-slot rounding costs roughly another 10-15%; the Hatari reference overruns to 4 VBLs in 2.7% of steps against 0.3% here, so the machine needs wait-state rounding (S3) to reproduce the reference. |
| Recompiler (S7) | Feasible: no TRAPs, one self-modifying handler in the front end (section 6), all I/O at fixed absolute addresses. |
| Modernisation capture (D4) | The CPU draws through about 14 routines carrying 99.7% of the written bytes (section 7), under the plan's threshold of 20; interpolation stays a later decision, filters and scaling are free. |

## 3. Answers to the open questions (plan sections 9 and 11.1)

| # | Question | Answer (evidence) |
|---|---|---|
| 1 | Logic rate | One logic step every **3 VBLs on 50 Hz** (16.67 Hz), **4 VBLs on 60 Hz** (15 Hz). The word at `$410` is set to 3 at engine start (`$7D50`) and to 4 if bit 1 of `$FF820A` is clear (`$7D58`). Measured in `stemu` with `--sync 2` and `--sync 0`, and independently in Hatari with EmuTOS UK (50 Hz: 3) and US (60 Hz: 4). Pacing: the step waits until `$41C - $404 >= $410` (loop at `$28E4`), then waits for the video counter to reach the other screen (`$2912`), swaps the buffers and writes the base registers. |
| 2 | RNG | Routine at **`$286C`**: saves D1/D2, loads a 64-bit state from `$28A0`/`$28A4`, runs two rounds of add/addx on words, stores it back, returns D0. Same algorithm as the Amiga build. Seeds `$63141592`/`$35358979` are present from load and not changed by the program at start, so the game is deterministic from the input; nothing is seeded from the clock or the keyboard. |
| 3 | Palette effects | Palette words are written 16 at a time, by the engine at **line 263** (just below the 200 visible lines 63-262; the fade routine at `$8BF8`, 12,096 words in the window) and at **line 255** (the death flash at `$8194`, which falls inside the last 8 visible rows), plus a few sets at other lines (95, 174, 175, 10, 265, 274 in the sampled run, from `$7CBA`, `$8B58`, `$8F9A`, `$8D4C`). No Timer B, no HBL, no raster-timed colour changes; the front end writes its fades from its own code. Per-line capture of the palette reproduces all of it; a per-frame latch would not. |
| 4 | Draw method and cost | The CPU draws; no blitter. Double-buffered screens at `$3E200` and `$46900` (buffer pointers `$406`/`$40A`, swapped at `$291C`). Each step starts with a full-screen clear (a movem store loop at `$8580`: 12% of the CPU time outside the wait loops, 37% of the bytes written to the buffers); the sprite and tile blits sit at `$1F42-$219C` (51% of the bytes). Over a 2,000-frame window of play: 24 writer clusters, 14 carry at least 0.1% of the bytes, 7 at least 1%. Busy fraction of a 3-VBL step: mean 0.44, median 0.35, p95 0.71, p99 0.91, max 1.00 (990 steps of 3 VBLs, 3 of 4 VBLs). |
| 5 | Loader and disk layout | 80 tracks x 2 sides x 10 sectors x 512 bytes. The boot sector loads side 0 tracks 1-25 (250 sectors, 128,000 bytes) to `$10000` in a packed form (the front end), which unpacks itself. The front end then loads side 0 tracks 34-79 (460 sectors, 235,520 bytes) to `$400`: the resident engine, which keeps its place for the whole run. A level load reads side 1 tracks 0-13 (132 sectors, 67,584 bytes) to `$6C994` (the packed level module, unpacked by the program to the module slot), and four sectors of side 1 track 77 (2,048 bytes) to `$7F714` (the packed map block). All loads are single-sector read commands (`$80`) after a restore (`$03`) and seeks (`$13`); one force-interrupt (`$D0`). 846 sector reads in total in the longest run, which reached level 1 only (levels 2-5 were not reached; see the unknowns). |
| 6 | High-score persistence | **None.** No write-sector or write-track command in 14,000 frames covering game over, the continue screen and the high-score screen. The table lives in RAM only; the original does not save scores. The default for the port (D6: save by default) is a modernisation, not original behaviour, and should be described as such. |
| 7 | Controls | **Joystick port 1 only**: the IKBD joystick-1 packet (`$FF`) is decoded by the ACIA handler (`$0F7C`) into the state byte `$A00` (bit 7 fire, bits 3-0 right, left, down, up) and the fire-press edge flag `$A01`. Joystick-0 packets are accepted and ignored. Keys: the handler stores the last make code in `$9FF` (break codes are ignored) and a table at `$7FE2` maps three keys: **P (`$19`) pause** (waits for any key or fire, at `$805A`), **Esc (`$01`) abort the game** (jumps to the engine's re-entry code at `$7E54`), **Space (`$39`) special action** (decrements the counter at `$CA0` and sets the request words `$414`/`$430`: probably the smart bomb, to be confirmed in S5). The IKBD receives `$13` at start and `$14` when the engine starts. |
| 8 | YM and sampled sound | Two uses. **Front end (frames 143-2681 in the run, the intro and title):** a Timer A handler at `$40C94` (vector `$134`) fires at 7,979 Hz (TADR `$4D`, TACR 1 = divide by 4) and writes the YM channel A and C volume registers (8 and 10) through `movep`, about 160 interrupts and 320 YM writes per frame: a digitised sample played through the volume registers, with self-modifying code (the sample pointer is patched in the handler). **Engine (gameplay):** no Timer A; the music and effects driver is called from the VBL handler once per frame, writing the eleven registers 0-10 each tick (about 11 writes per frame), no envelope registers (11-13 are never written). |
| 9 | Music tune count | Not measured: the tune number is a variable in the engine; the Amiga build has the same structure. To do in S4 with the register logs from `--ym-log`. |
| 10 | Vectors | All 62 vectors from `$08` to `$FC` and all MFP vectors except two point at one default address (`$7CF8`, the engine's own entry, i.e. a restart if anything unexpected fires). Real handlers: VBL level 4 at `$70` -> `$2984`, ACIA via the MFP (VR `$40`, interrupt 6 = vector `$118`) -> `$0F7C`, and Timer A (`$134`) in the front end only. IERA/IMRA end at 0 after the front end; IERB/IMRB at `$40` (ACIA). No Timer B, C, D, HBL, level 3, TRAPs. |
| 11 | Odd addresses and bus errors | None (`--excwatch`: 0 exception-target executions, 0 unmapped accesses, 0 odd word or long accesses). The 68000 address-error path is not needed for this game. |

## 4. Variables and hooks

All variables the Amiga disk build uses are at the same addresses in the ST resident (checked in play).

| Item | Address | Notes |
|---|---|---|
| Level (1-5) | `$E04` (word) | incremented at `$8426`; wraps from 6 to 1 at `$8436` |
| Loop count | `$E02` (word) | incremented at `$843E` when the level wraps |
| Energy | `$CC6` (word) | full is `$27` (39), set at `$81B4` |
| Ships | `$CCA` (word) | decremented at `$81C2` |
| Credits | `$CF0` (word) | 3 at start; decremented when ships reach 0 (continue) |
| Score | `$C92` (long) | 200 per kill in the test run |
| Death routine | `$8190`-`$81C8` | the request flag is `$A02`; sets energy, flags `$A6B`, decrements ships, calls `$1C7A`; the flash writes the palette at `$8194` |
| Level advance | `$8426`-`$844C` | after the map blocks are processed |
| Logic stamp / VBL counter / step length | `$404`, `$41C`, `$410` | words; the counter is incremented by the VBL handler |
| Screen buffers | `$3E200`, `$46900` | pointers at `$406` and `$40A`; base registers `$FF8201/03` |
| VBL handler | `$2984` | counts `$41C`, calls the sound driver through the pointer at `$294A` |
| ACIA handler | `$0F7C` | packets into `$A6D`, joystick state `$A00`, fire edge `$A01`, last key `$9FF` |
| RNG | `$286C`, state `$28A0`/`$28A4` | |
| Pacing and flip | `$28E4` (wait for the step), `$2912` (wait for the beam) | idle ranges for the profiler |
| Key table | `$7FE2` | three entries (P, Esc, Space) |
| Screen clear | `$8580` | per step |

## 5. Timing evidence for the video model

- Frame structure: vertical blank every 160,256 cycles; the visible lines are 63-262 in the harness (a model assumption, to be
  confirmed against Hatari's line timing in S3); the base-address registers are written at each flip (4,032 writes in the
  14,000-frame run) and take effect at the next vertical blank.
- `$FF8207` (video counter, mid byte) is read 25 million times by the flip wait: the counter must follow the beam at cycle
  resolution, not the frame. Implementation: derive it from the CPU cycle count (as `stemu` does); no lines of lookahead needed.
- Palette: see answer 3. The frame-latched alternative fails for the death flash (a mid-screen split on the bottom 8 rows) and for
  the few mid-frame fades.
- Reference cross-check (Hatari 2.6.1 with EmuTOS 1.4, the harness from `st/hatari-reference`): the logic step is 3 VBLs in 1,784
  of 1,835 steps (97.2%) and 4 VBLs in 50 (2.7%) at 50 Hz; at 60 Hz 4 VBLs in 1,187 steps and 5 in 22 (1.8%). The variable map
  matched at the same addresses. The 4-VBL steps are what a cycle model without bus-slot rounding under-counts (0.3% here); the
  harness's busy fraction at p99 (0.91) times the 10-15% rounding crosses 1.0.

## 6. Hardware used, summary

| Device | Use |
|---|---|
| Shifter | low resolution (`$FF8260` written twice at start), palette `$FF8240-5E`, base `$FF8201/03`, counter `$FF8207`, sync mode `$FF820A` read (the 50/60 Hz selection at `$7D58`, and a routine near `$2BBF8` that also reads it to choose a constant, probably the music tempo) |
| MFP | VBL is not an MFP interrupt; GPIP read for the FDC (bit 5) and the ACIA line (bit 4), bit 7 (monochrome monitor detect) read by the same routine near `$2BBF8`; Timer A in the front end; Timer D untouched; VR `$40` (automatic end of interrupt) |
| ACIA/IKBD | one handler; joystick 1 packets and key make codes; commands `$13`, `$14` out |
| YM2149 | registers 0-10, never 11-13; reads of `$FF8800` are the driver's mixer read-modify-write |
| WD1772 via DMA | restore, seek, read sector, force interrupt; registers `$FF8604/06/09/0B/0D`; the loaders poll GPIP bit 5 |
| Self-modifying code | the front end's Timer A handler (sample pointer and counters patched in its body) |

## 7. Modernisation (D4) assessment

- Writer-PC census over 2,000 frames of play: 24 clusters, 14 above 0.1% of the bytes; the blit area `$1F42-$219C` and the clear
  (`$8580`) account for 88%. The Amiga/CDTV capture works on blitter calls and object lists; the ST has neither, so a capture
  would hook the 5-7 blit entry points and the clear. Feasible, uncertain size: left for after the machine core.
- Filters and scaling work on any framebuffer: no S0 dependency.

## 8. Hatari reference: design (the harness is the `st-hatari` worker's; not duplicated here)

Use `tools/compare/hatari-harness/run.sh` with `WATCH=404,41c,410,cc6,cca,e04,c92` and `X2REF_RAM` dumps, UK image for PAL, US
image for NTSC. Compare **game events, not VBL numbers** (EmuTOS boot offset):
1. cadence: the histogram of `$404` steps (done: section 5);
2. variables at "get ready" and death events (RAM dumps at the VBL where energy first reaches `$27`, and after each ship loss);
3. the first frame of level 1 and the frame after the first death (palette flash) as PNGs, for the S3 picture check;
4. the YM output for the front end (`WAV=1`, `FF=off`) against our register-log render, for the Timer A sample (S4);
5. the 60 Hz music tempo (US image): the routine near `$2BBF8` picks a different constant.
Our side needs `StMachine` replays with the same fire script: the script in `stemu` (`jtap 1 80 3` every 8 frames from the title)
and the harness's `inputs/kixx-title-to-level1.txt` both reach level 1.

## 9. Unknowns

- Whether a real IKBD obeys `$13` (pause output) from frame 0 until `$14`: the model ignores `$13` and Hatari delivered the fire press,
  so the front end's title screen works either way; settle in S4 against Hatari's IKBD.
- What Space does exactly (smart bomb vs. weapon change): the key table is certain, the effect is inferred from the variables.
- Music tune count and the per-tune data; whether any tune uses the YM envelope (none in the run).
- The interaction of bus-slot rounding with the pacing loop for the step lengths at the p99 (S3 measurement).
- Level 2-5 loads were not reached (the fire-only script dies on level 1); each should be the same two-block load, to be confirmed
  with a steering script in S6.
- Hatari timing was measured with the fire script only; a steering script would give a fuller histogram.

## 10. Reproducing

```
cc -O2 -w -DMUSASHI_CNF=\"st_m68kconf.h\" -Itools/acq -Ivendor/musashi -I<musashi_gen> tools/acq/stemu.c vendor/musashi/m68kcpu.c \
   <musashi_gen>/m68kops.c runtime/musashi_no_fpu.c -o stemu
./stemu IMAGE.st --st --sides 2 --boot-at 0x70000 --frames 6000 --max 6000000000 --ikbd SCRIPT --frame-log f.log --ym-log ym.txt \
   --pal-log pal.txt --irq-log --fdc-log --io-summary --excwatch --idle 0x28e4:0x2918 --shot 3300:s.stshot
python3 tools/acq/stshot.py s.stshot s.png
```
`tools/acq/test_st.py` holds synthetic tests only (boot-sector programs and generated screens; set `X2_MUSASHI_GEN` to the
generated Musashi tables to run the `stemu` ones).
