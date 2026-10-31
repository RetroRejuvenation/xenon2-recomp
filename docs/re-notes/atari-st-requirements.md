# Atari ST build: what it needs from the machine

Evidence for the plan to add an Atari ST target. Analysis only: no runtime code changed, no game bytes here. The
subject is the Kixx 1993 re-release (public Atarimania dump, `.st`, sha1 `323d6859635b6b82c86363db048b26e0f3df994c`,
see `docs/re-notes/acquisitions-flux-acorn-st.md`), whose loader, resident engine and front-end were unpacked in
`docs/re-notes/atari-st-analysis.md`. Addresses below are ST addresses; `$FF8xxx` etc. are the memory-mapped I/O.

## 1. Method, and what it can and cannot show

`tools/acq/stemu.c` (a minimal ST around the project's Musashi core: WD1772 + DMA, a video counter, a level-4 VBL
interrupt, the YM2149 write log) gained an off-by-default `--io-summary` option: at exit it lists every I/O register
touched (reads, writes, the first PC that read or wrote it, the last value written), every write below `$600`
(vectors and system variables) and every `TRAP` instruction executed. Run on the Kixx disk with a 50 Hz VBL
(`--sides 2 --boot-at 0x70000 --vbl 90000`), 1.5 billion instructions (16,666 VBLs, about 5.5 minutes of ST time) ran
without a fault and ended in the engine's frame-pacing loop.

Limits: the harness has no MFP timer interrupts, no IKBD input and no cycle timing, so the title screen idles and no
game is started: the **level modules were never loaded** in this run and anything only used in gameplay or by a
timer interrupt is invisible to it. A static scan of the resident for `$FF8xxx` operands was also tried; a linear
sweep decodes data as code and produced about a hundred spurious addresses, so only the dynamic result is used here.

## 2. Hardware actually touched (dynamic run)

| Area | Registers (hits in 1.5 G instructions) | What the program does |
|---|---|---|
| Shifter video | `$FF8201/03` video base (4,268 writes each); `$FF8207` video counter, mid byte (16.1 M reads); `$FF820A` sync mode (2 reads); `$FF8240-5F` palette (192 full 16-colour loads); `$FF8260` resolution (2 writes, low) | Double-buffered page flip each game frame; **waits on the video counter** (`cmp.b $ff8207` against the target page, equality loop) before flipping: a scan-line-granular raster wait. Palette is loaded as a whole, by the CPU, not mid-frame. 50/60 Hz read from the sync mode to pick the frame cadence (below). Not touched: `$FF8205/09`, hardware scroll registers (STe). |
| MFP 68901 | `$FFFA07/09/13/15` IER/IMR A and B; `$FFFA0B/0D/0F/11` IPR/ISR; `$FFFA17` VR; `$FFFA19` TACR, `$FFFA1F` TADR (front-end: TACR 1, TADR `$4D`); `$FFFA1D` TCDCR; `$FFFA01` GPIP (800 k reads) | Init masks **every** interrupt except GPI4 (the keyboard/MIDI ACIA); the resident stops timer D (`TCDCR` low nibble cleared, TOS's baud-rate timer). **The front-end installs a Timer A handler and enables it**: delay mode, divide by 4, data 77, so 2,457,600 / 4 / 77 = 7,979 Hz. GPIP bit 5 (FDC) is polled during loads. Timers B, C and D have no handler (their vectors point at the default one). |
| Floppy | `$FF8604/06/09/0B/0D` (3,932 mode writes, 2,424 command/data writes, 754 sector DMAs) | Raw WD1772 commands through the DMA chip: restore, seek, read sector. No BIOS, no GEMDOS. Side select through YM port A (`$FF8800` register 14), read-modify-write. |
| IKBD ACIA | `$FFFC00` status, `$FFFC02` data (3 writes: IKBD init; reads in the interrupt handler) | Vector `$118` is real. The handler decodes IKBD packets (header `$F6-$FF`, lengths from a table), joystick packets into a buffer at `$A6D`, key make codes into `$9FF`. The start-up sends IKBD commands `$08` and `$14` (relative mouse, joystick event reporting). |
| YM2149 | `$FF8800` (122,968 writes, 11,183 reads), `$FF8802` (122,967 writes) | About 7.4 register writes per VBL from a VBL-driven music player. The front-end sets the mixer register to `$7F` (all tone and noise off) for its Timer A sample player. |
| Not touched | blitter `$FF8A00`, STe DMA sound `$FF8900`, MIDI ACIA `$FFFC04`, RTC `$FFFC21`, TT/Falcon registers | The target is a **stock ST (no STe, no blitter)**. |

**Digitised sound.** The Timer A handler (front-end) decrements a counter, fetches a sample byte from an address that is
patched into the instruction itself (**self-modifying code**), looks up eight bytes of YM volume values and writes
YM registers 8-10 with `movep` (a 4-bit-sample-to-YM-volume DAC trick), at 7,979 Hz. The earlier note that the engine
has no sample effects was true of the register log only because the harness never raised the timer interrupt: the
sample player exists and needs MFP Timer A with CPU-cycle scheduling.

## 3. TOS: not used after loading

- **Zero `TRAP` instructions** executed in the whole run (no GEMDOS, BIOS or XBIOS).
- Every CPU exception vector (`$08-$7C` except VBL), every TRAP vector and every MFP vector `$100-$13C` is overwritten
  with one default handler at `$7CF8`, which is also **the engine's entry point** (the first instruction at `$400` is a
  branch to it): an unexpected exception restarts the engine. Only **VBL (`$70` = `$2984`)**, the **ACIA (`$118` =
  `$0F7C`)** and the front-end's **Timer A (`$134`)** are real handlers; the init at `$7CF8` sets all of these.
- TOS system variables are not maintained: `_vbclock`, `_frclock`, `_hz_200`, `_v_bas_ad`, `nvbls`/`_vblqueue`,
  `phystop` all stay 0 in the harness, and the engine uses `$400-$41F` as **its own variables**: `$404` last-frame
  stamp, `$406` / `$40A` the two screen bases, `$410` minimum VBLs per game frame, `$414`/`$418` deferred-job flags,
  `$41C` VBL counter. The engine's game variables sit in `$9FC-$A6D` (compare the Amiga build's `$9xx-$Axx` block).
- Consequence: after the boot sector the machine is the game's. A **minimal TOS is not needed**; what is needed is
  the state TOS leaves behind at hand-over (supervisor mode, SR `$2300`, a stack, the boot device byte `$446`, the
  video base TOS had set), which the existing `stemu` already reproduces well enough to reach the engine.

## 4. Loading model and memory map

- The boot sector (at `$70000` in the harness) reads **tracks 1-25 side 0** (250 sectors, 128,000 bytes, RNC ProPack
  method 1) to `$10000`, unpacks them to **`$40000`** (the 159,108-byte front-end), then reads **tracks 34-79 side 0**
  (460 sectors) straight to **`$400`**: the 235,520-byte resident engine. All by direct FDC commands. The five RNC level
  modules (111,900-171,638 bytes) and five 12,000-byte blocks live on side 1 and were not loaded in this run.
- Non-zero RAM at the end: `$000000-$039FFF` (resident, up to `$39C00`), `$03D000-$066FFF` (screen buffers at `$3E200`
  and `$46900`, 34,560 bytes apart = 216 lines of 160 bytes, plus the front-end), `$070000-$073FFF` and
  `$078000-$07CFFF` (loader leftovers, stack). **Everything is below `$80000`: a 512 KB ST is enough.**
- The screens overlap the front-end's load area (`$40000-$66E04`), so the front-end and the engine's play phase
  cannot be resident together; how and when it is replaced is for phase S0 (the level modules were not reached).

## 5. Timing

- ST CPU 8 MHz, **512 cycles per line, 313 lines, 160,256 cycles per frame**, 50 Hz PAL (60 Hz mode exists: the
  engine reads it).
- Frame cadence is software: the game advances when `$41C - $404 >= $410` (VBL count since the last game frame), where
  `$410` is set to **4 at 50 Hz and 3 at 60 Hz** (from the sync mode bit): game logic at 12.5 or 20 steps per
  second. (The disk Amiga build steps every 3 VBLs at 50 Hz.) To be confirmed in gameplay (phase S0).
- The page flip waits for the Shifter counter to reach the end of the shown buffer: **line-granular video counter
  needed** (a counter that advances 160 bytes per displayed line is enough, and must hit every mid-byte value).
- No HBL (`$68`) or timer B (`$120`) vectors: **no mid-frame palette or border tricks in what was observed.**
- Timer A digi playback at 7,979 Hz needs interrupts scheduled in CPU cycles, and the sample write timing decides the
  audio quality (the YM output must be sampled finer than the sample rate).

## 6. Open questions (settled by phase S0 of the plan)

1. What the play phase loads and where (level-module slot, the five 12,000-byte blocks), and how it replaces the front-end.
2. The input path in gameplay (IKBD joystick packets parsed at `$F7C`; keyboard) and the game variables for level,
   energy, lives, score (the engine shares code with the CDTV build: derive by shape matching).
3. Whether the cadence of 4 VBLs is real in play, whether hardware scroll or sprites are used (CPU drawing only is
   expected: no blitter), and whether timer B or HBL appear in later phases.
4. The sample source of the Timer A player and whether the engine (not only the front-end) plays samples.
5. The Image Works release (Copylock ST) uses the same engine; its payload is still encrypted.

## 7. Reference emulator

Hatari 2.6.1 (GPL) is available as a Homebrew bottle but is **not installed here**; installing it changes the machine's
packages, so the plan treats it as a decision. Hatari runs with EmuTOS (GPL) as its ROM, so no Atari ROM would be needed,
but whether the Hatari package bundles an EmuTOS image was **not verified**.
