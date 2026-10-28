# Building and running x2recomp

Phase 3a state: headless, interpreter-driven runtime core. There is no window, video or audio output yet
(Phase 3b). The game boots, runs its title and attract sequence, and with scripted fire presses loads
level 1 from disc 2 and plays it.

## Requirements

- CMake 3.20 or newer. The game-data CTest uses `cmake_language(EXIT)`, which needs CMake 3.29 or newer.
- A C++20 compiler (tested: Apple clang on macOS) and a C99 compiler for Musashi.
- Python 3 for asset preparation.
- Your own Xenon 2 Megablast disk images. Nothing from the game is in this repository.

## Vendored code

| Path | What | Pinned version | Licence |
|---|---|---|---|
| `vendor/musashi` | Musashi 680x0 emulator by Karl Stenerud, used as the 68000 interpreter | https://github.com/kstenerud/Musashi commit `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` (2026-03-08) | MIT-style, see the "LICENSE AND COPYRIGHT" section of `vendor/musashi/readme.txt` |

Only the core sources are kept (`example/`, `test/` and `.git` removed). At build time `m68kops.c` and
`m68kops.h` are generated into `build/musashi_gen/` by the vendored `m68kmake`. The runtime's Musashi options
are in `runtime/x2_m68kconf.h`, selected with `MUSASHI_CNF`. They give a plain 68000, autovectored
interrupts, and a per-instruction hook used for PC traps.

## 1. Prepare assets (once)

```
python3 -I tools/prep_assets.py --zip Xenon2.zip                  # zip holding both ADFs
python3 -I tools/prep_assets.py --disc1 d1.adf --disc2 d2.adf     # or the two ADFs
```

This writes `generated/assets/game_0400.bin` and `generated/assets/disc2.bin`. `generated/` is gitignored.

- `game_0400.bin` is the depacked `XENON-II` executable from disc 1, cut to the `$3FC00`-byte image that the
  game runs from at `$400`.
- `disc2.bin` is disc 2 unchanged: 1760 linear sectors with the "X2B " id.

The cracker intro (BS1), the depacker and the trainer stub are never run, so the trainer patches are not
applied.

With the reference `Xenon2.zip` the SHA-1 hashes are:

- `game_0400.bin`: `1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4`
- `disc2.bin`: `a748afc7a142335a40cc46fb7785dea820770241`

## 2. Build and test

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

There are two tests:

- `smoke` needs no game data. It checks the bus and mirroring, custom-register SET/CLR semantics, the beam
  counters, CIA timers and TOD, VBL and CIA interrupt delivery, the keyboard serial handshake, copper
  MOVE/WAIT, the blitter (shift, masks, fill) and the disk shim.
- `game_boot` needs `generated/assets/` (or set `-DX2_ASSETS_DIR=...`). It runs 1500 frames, pressing fire
  every 50 frames from frame 400. It passes only if all of these happen:
  - the VBL handler runs;
  - the disc-id check passes;
  - level 1's main loop runs;
  - bitplanes are displayed;
  - nothing restarts through `$7032`.

  Without assets this test is reported as **Skipped**, not Passed.

## 3. Run

```
./build/x2recomp --headless --frames 500 --dump-dir out [--assets generated/assets]
```

| Option | Meaning |
|---|---|
| `--frames N` | number of 50 Hz frames to run |
| `--dump-dir DIR` | output directory |
| `--assets DIR` | location of `game_0400.bin` / `disc2.bin` (default `generated/assets`) |
| `--report-every N` | status line interval (default 50 frames) |
| `--ppm-every N` | dump the bitplane framebuffer every N frames as `frame_<n>.ppm` |
| `--no-reglog` | skip the per-frame register logs |
| `--trace-frame F` | disassembly and register trace of every instruction in frame F (`trace_F.txt`) |
| `--key F:CODE` | press Amiga raw key `CODE` (hex) at frame F; it is released 2 frames later |
| `--fire F[:N]` | hold joystick fire (port 2) from frame F for N frames (default 3) |

For example, `--fire 460:5 --fire 950:5 --fire 1200:5 --frames 1500` goes from the title to the SELECT
menu, then to "GET READY PLAYER 1", then into level 1.

Outputs in `DIR`:

- `regs/frame_NNNNN.log`: every custom-register and CIA write in that frame, one line each:
  `frame line hpos src pc reg value`.
  - `src` is `C` for a CPU write (`pc` is the instruction's address) or `P` for a copper write (`pc` is the
    copper instruction's address).
  - CIA registers are named `CIAA_*` and `CIAB_*`.
- `chipram.bin`: the 512 KB chip RAM at the end of the run.
- `final.ppm` and `frame_<n>.ppm`: a crude low-res planar decode of the bitplanes. It uses the pointers,
  BPLCON0, modulos, DDF/DIW and palette latched at the first line of the display window. There is no
  scrolling, sprites, hires or HAM. It is a debugging aid, not the renderer.
- stdout prints the following:
  - Every `--report-every` frames: PC, SR, INTENA/INTREQ/DMACON, counters, the hottest PCs (sampled once per
    line, with names from the code map), and the most-polled registers.
  - At the end, the milestone frames and the disk-shim statistics.

## Machine model and timing budget

The model is a PAL A500 (OCS, 512 KB chip RAM, 68000 at 7.09379 MHz).

- **Frame:** 312 lines, non-interlaced, with no long/short field alternation. Each line is 227 colour clocks;
  the real line is 227.5. The CPU gets 2 cycles per colour clock, so **454 CPU cycles per line** and
  **141,648 per frame** (49.9 Hz at 7.09379 MHz). DMA cycle stealing is not modelled, so the CPU always
  gets its full budget. Musashi's overshoot past a line's budget is carried into the next line.
- **Beam:** VPOSR/VHPOSR give the current line plus `hpos = cycles into the line / 2`, so polling loops see
  the beam move within a line. VPOSR bit 0 is V8. LOF and the Agnus id read 0.
- **Interrupts:** Paula INTENA/INTREQ drive the IPL lines. Levels: TBE/DSKBLK/SOFT=1, PORTS=2,
  COPER/VERTB/BLIT=3, AUD0-3=4, RBF/DSKSYNC=5, EXTER=6.
  - CIA-A /IRQ sets PORTS and CIA-B /IRQ sets EXTER. Both are level-sensitive: clearing INTREQ while the CIA
    still asserts its line sets the bit again.
  - VERTB is raised at the start of line 0.
  - The game enables only INTEN, VERTB, PORTS and DSKBLK (`INTENA=$C02A`). Its handlers are VBL `$20C2`
    (L3), keyboard `$0FAC` (L2) and disk-block `$95E4` (L1; never raised, because the shim replaces the
    loader).
- **CIAs:** clocked at E = CPU/10, which is 45.4 ticks per line, applied once per line with the remainder
  carried.
  - CIA-A TOD counts frames (50 Hz) and CIA-B TOD counts lines.
  - Keyboard bytes arrive in CIA-A SDR as `~rol(code,1)` with ICR SP set. The next byte is held until the
    game's SPMODE handshake pulse (CRA bit 6 high, then low), or a 10-frame timeout.
- **Initial CIA state:** as Kickstart leaves it.
  - CIA-A CRA has TODIN=1 (50 Hz mains). The game reads this at `$768A` to choose its frame pacing: with
    TODIN=1 it sets `$410=4` and runs game logic every 2 VBLs (25 Hz), because the VBL handler adds 2 to
    `$41C` per interrupt. **This is an assumption about Kickstart's PAL setup.** It disagrees with the note
    "4 NTSC / 5 PAL" in `docs/re-notes/memory-map.md`. Real-hardware timing should settle it.
  - CIA-A port A has /TK0 and /RDY low and the fire buttons released.
- **Copper:** runs twice per line, at the start (h=0) and the end (h=`$E2`) of the line. So a WAIT takes
  effect within one line of its target position, but not cycle-exactly.
  - MOVE, WAIT and SKIP are implemented, as are COPCON danger handling and the `$FFFF,$FFFE` end marker.
  - The copper restarts from COP1LC at line 0 and immediately on a COPJMP1/2 strobe.
  - The blitter-finished bit in WAIT is ignored, because blits are instant.
- **Blitter:** area mode is complete: A/B/C/D, shifts, first/last-word masks, all 256 minterms, descending
  mode, inclusive and exclusive fill. It runs in zero time when BLTSIZE is written, so DMACONR BBUSY always
  reads 0. BZERO is computed, and INTREQ BLIT is set on completion. **Line mode is not implemented.** It
  is counted, and the game has not used it so far.
- **Disk shim:** a PC trap at `$921A` (`load_sectors`: a0=dest, d0.w=start sector, d1.w=count) copies
  `count*512` bytes from `disc2.bin`, then does the following:
  - copies the last track touched into the loader's staging buffer at `$44000`, as the real loader would;
  - sets a0 to the end of the data, d0=0 and Z;
  - emulates RTS.

  The disc-id check `$8F9A` needs no hook. It calls `$921A` for sector 0 and then compares the real "X2B "
  id.
- **Boot:**
  - chip RAM is zeroed and the image is loaded at `$400`;
  - the 68000 is reset into supervisor mode with SR=`$2700`, SSP=`$435FC` and PC=`$400`;
  - `$400` is `bra.w $7032`, the same path as the crack stub's `jmp $400`, and the game then sets
    everything up itself.

  Later entries to `$7032` mean an exception or a deliberate restart. They are counted and reported as
  `restarts-via-$7032`.

## Known gaps (Phase 3b and later)

- There is no real renderer and no audio. Paula registers are logged but produce no sound, and audio DMA
  interrupts are never raised (the game does not enable them).
- There is no blitter timing or line mode, and no DMA contention, so the CPU runs somewhat faster than on
  real hardware during heavy blitting.
- The copper is line-granular. Sprites are not implemented; the game does not use hardware sprites.
- The raw MFM buffer `$45600-$495FF` is not filled during loads. The original loader would leave undecoded
  flux data there.
- Only the headless driver exists. Without `--headless` the program exits with an error.
