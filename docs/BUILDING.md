# Building and running x2recomp

Phase 3b state: an interpreter-driven runtime core with the following outputs:

- a scanline renderer (`runtime/gfx/`);
- Paula audio (`runtime/audio/`);
- an SDL3 window with keyboard and gamepad input (`runtime/input/`).

It also runs headless for tests. The game boots, runs its title and attract sequence, and with fire presses
(live or scripted) loads level 1 from disc 2 and plays it.

## Requirements

- CMake 3.20 or newer. The game-data CTest uses `cmake_language(EXIT)`, which needs CMake 3.29 or newer.
- A C++20 compiler (tested: Apple clang on macOS) and a C99 compiler for Musashi.
- Python 3 for asset preparation.
- SDL3 (optional; found with `find_package(SDL3 CONFIG)`, e.g. Homebrew `/opt/homebrew`). Without it, only
  `--headless` is built, and the window, input and SDL audio tests are left out.
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

These tests are defined:

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
- `video` needs no game data. It runs synthetic chip RAM through the Machine and the renderer and checks:
  - copper palette changes, including a mid-line WAIT;
  - bitplane fetch and modulo, BPLCON1 scroll and the DIW border;
  - dual playfield priority, HAM and EHB;
  - sprite DMA, attached pairs, sprite-sprite and BPLCON2 sprite-playfield priority, and manual sprites;
  - blitter line mode: all octants, one pixel per major step within 0.5 px, XOR, SING and textures;
  - the 50 Hz pacer: no drift over 10 simulated minutes at 60/144 Hz, and rebasing after a stall;
  - the PNG/PPM writers.
- `input` is built only when SDL3 is found and needs no devices. It checks the Amiga raw-key table, the
  binding config round trip, and the key-to-JOY0DAT/JOY1DAT/CIA fire-bit path.
- `golden_frames` needs game data and is **Skipped** without it. It runs the `game_boot` input script and
  compares FNV-1a hashes of the rendered RGBA frames with `tests/golden/game_frames.txt`. The frames checked
  are:
  - the title at 150 and 300;
  - the SELECT zoom at 470;
  - GET READY at 700;
  - level 1 at 860-1400.

  Only hashes are committed. After an intended rendering change, look at the frames with `--screenshot-at`,
  then regenerate the file with:

  ```
  cmake -DX2RECOMP=build/x2recomp -DASSETS=generated/assets -DOUT=build/golden_frames \
        -DGOLDEN=tests/golden/game_frames.txt -DUPDATE=1 -P tests/golden_frames.cmake
  ```
- `paula` needs no game data. It runs on synthetic chip RAM and checks:
  - DMA start, one-shot-then-loop reload and LEN 0 = 65536 words;
  - period timing and the 124 clamp;
  - volume (bit 6 = 64);
  - DMACON master/channel gating and stop-at-end-of-sample;
  - interrupt times;
  - stereo mapping and separation;
  - filter responses (A500, LED, A1200, none, DC high-pass);
  - exact output-rate and split-invariant resampling;
  - the custom-register and CIA-A LED paths.
- `sdl_audio_smoke` is built only when SDL3 is found. It drives the SDL3 output ring buffer for 1 s on SDL's
  `dummy` audio driver, so it needs no sound device.
- `game_audio` needs game data and is **Skipped** without it. It plays title -> level 1 and holds fire for 100
  frames in level 1, with `--audio-out`/`--audio-cues`. It checks that:
  - title music starts within 10 frames;
  - the in-game music starts after the level load;
  - firing queues sound effects;
  - all four channels play for at least 1 s;
  - nothing clips;
  - with Python 3 (stdlib only, `tests/audio/check_wav.py`), the title and level-music stretches are audible on both sides and there is no DC offset.

## 3. Run

```
./build/x2recomp [--assets generated/assets] [--scale 3] [--fullscreen] [--aspect]           # window
./build/x2recomp --headless --frames 500 [--dump-dir out] [--assets generated/assets]       # no window
```

The window runs the emulated 50 Hz frames on a fixed timestep, whatever the host refresh rate:

- Frame n is due at start + n x 20 ms, in integer nanoseconds, so there is no drift.
- When the host falls more than 4 frames behind, the schedule is rebased.
- It presents with vsync, using integer-scaled square pixels, or a 4:3 fit with `--aspect`.

Default controls:

| Key | Action |
|---|---|
| arrows, Left/Right Ctrl or Z | joystick port 2 (the one the game reads) |
| W A S D, Left Shift | joystick port 1 (wired, unused by the game) |
| F11 or Alt+Enter | fullscreen toggle |
| Pause or F9 | freeze the emulation |
| F12 | hard reset |
| F10, Ctrl/Cmd+Q, closing the window | quit |
| any other key | sent to the game as an Amiga key; the game uses P (pause), ESC (back to menu) and Space |

Gamepads are hot-pluggable. The first one connected drives port 2 and the second drives port 1. The d-pad or
left stick gives directions, and any face button fires.

Bindings are stored in `input.cfg` in the SDL preferences directory; on macOS that is
`~/Library/Application Support/xenon2-recomp/x2recomp/`. The file is written with the defaults on first run.
Its format is `action = Key Name, Key Name`, using SDL scancode names.

| Option | Meaning |
|---|---|
| `--headless` | no window. Without SDL3 this is the only mode. |
| `--frames N` | number of 50 Hz frames to run (headless default 500; the window runs until quit) |
| `--no-audio` | window without sound. By default the window plays Paula through `SdlAudioOut`, with 40 ms latency and drift control. |
| `--scale N`, `--fullscreen`, `--aspect` | initial window scale (default 3), start fullscreen, 4:3 picture instead of square pixels |
| `--screenshot-at F:FILE` | save rendered frame F as FILE: PNG if it ends in `.png`, otherwise PPM. Repeatable. |
| `--hash-at F` | print `framehash F <fnv1a64>` of the rendered RGBA frame F. Repeatable; used by `golden_frames`. |
| `--dump-dir DIR` | output directory (optional; without it no register logs or dumps are written) |
| `--assets DIR` | location of `game_0400.bin` / `disc2.bin` (default `generated/assets`) |
| `--report-every N` | status line interval (default 50 frames) |
| `--ppm-every N` | dump the bitplane framebuffer every N frames as `frame_<n>.ppm` |
| `--no-reglog` | skip the per-frame register logs |
| `--trace-frame F` | disassembly and register trace of every instruction in frame F (`trace_F.txt`) |
| `--key F:CODE` | press Amiga raw key `CODE` (hex) at frame F; it is released 2 frames later |
| `--fire F[:N]` | hold joystick fire (port 2) from frame F for N frames (default 3). In a window, live input is used in all other frames. |

For example, `--fire 460:5 --fire 950:5 --fire 1200:5 --frames 1500` goes from the title to the SELECT
menu, then to "GET READY PLAYER 1", then into level 1.

Audio options (Paula, `runtime/audio/`; details in `runtime/audio/paula.h` and `docs/re-notes/audio-notes.md`):

| Option | Meaning |
|---|---|
| `--audio-out FILE` | Write the exact Paula output of the whole run as a 48 kHz stereo 16-bit WAV. It is generated from emulated time, so no audio device is involved, and it is bit-identical between runs. Paula statistics are printed at the end. |
| `--audio-filter M` | `a500` (default), `a1200` or `none` |
| `--stereo-sep X` | stereo separation from 0 (mono) to 1 (default: channels 0 and 3 hard left, 1 and 2 hard right) |
| `--audio-cues FILE` | log every music/sound driver call as `frame line event d0 caller` |

The audio model works as follows:

- **Timing:** Paula is advanced in colour clocks to each audio, DMACON, ADKCON or CIA-A PRA write, and to the end of every line.
- **Resampling:** the output is area-averaged into a 192 kHz stream, filtered, then decimated to 48 kHz by a 127-tap Kaiser-windowed sinc.
- **Filters:**
  - the A500 model has a fixed ~4.9 kHz RC low-pass;
  - the A1200 model has a ~32 kHz low-pass;
  - both add the LED filter (a 3.3 kHz 2-pole Butterworth, switched by CIA-A PRA bit 1) and a 5 Hz DC high-pass.
- **Level:** full scale on one side maps to -2.5 dBFS, which leaves headroom for filter overshoot.

The SDL3 output (`x2::SdlAudioOut` in `runtime/audio/sdl_audio.h`, library `x2audio_sdl`) works like this:

- the emulation thread pushes each frame's samples into a lock-free ring buffer;
- the device callback drains it;
- latency is held near 40 ms by adjusting the stream's frequency ratio by at most ±0.5%.

Outputs in `DIR`:

- `regs/frame_NNNNN.log`: every custom-register and CIA write in that frame, one line each:
  `frame line hpos src pc reg value`.
  - `src` is `C` for a CPU write (`pc` is the instruction's address) or `P` for a copper write (`pc` is the
    copper instruction's address).
  - CIA registers are named `CIAA_*` and `CIAB_*`.
- `chipram.bin`: the 512 KB chip RAM at the end of the run.
- `final.png`: the last frame from the renderer.
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
  **141,648 per frame** (50.08 Hz at 7.09379 MHz; the window paces at exactly 50 Hz). DMA cycle stealing is not modelled, so the CPU always
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
  - CIA-A CRA bit 7 always reads 0. It is unused on the 8520: writes are ignored and reads return 0
    (Hardware Reference Manual, Appendix F).
  - The game tests this bit at `$768A`. Reading 0, it sets `$410=5` and leaves the music's 60 Hz
    tick-skip off. Game logic and the displayed picture therefore advance once every 3 VBLs (16.67 Hz).
    `docs/re-notes/timing.md` has the code paths and the measurement.
  - CIA-A port A has /TK0 and /RDY low and the fire buttons released.
- **Copper:** runs in slots:
  - at the start of each line (h=0);
  - every 16 colour clocks during the line (`kCopperSlotCck`), between CPU time slices;
  - at the end of the line (h=`$E2`).

  So a WAIT takes effect within 16 colour clocks (32 lores pixels) of its target, but not cycle-exactly.
  - MOVE, WAIT and SKIP are implemented, as are COPCON danger handling and the `$FFFF,$FFFE` end marker.
  - The copper restarts from COP1LC at line 0 and immediately on a COPJMP1/2 strobe.
  - The blitter-finished bit in WAIT is ignored, because blits are instant.
- **Blitter:** area mode is complete: A/B/C/D, shifts, first/last-word masks, all 256 minterms, descending
  mode, inclusive and exclusive fill. It runs in zero time when BLTSIZE is written, so DMACONR BBUSY always
  reads 0. BZERO is computed, and INTREQ BLIT is set on completion.
  - Line mode is implemented: the octants come from SUD/SUL/AUL, the Bresenham error is in BLTAPTL with
    BLTAMOD/BLTBMOD, and SING, the BLTBDAT texture (BSH) and any minterm are supported.
  - The game has not used line mode so far.
- **Video** (`runtime/gfx/video.cpp`): a scanline model of Agnus display DMA plus Denise, observing every
  custom-register write together with its beam position.
  - Display state: the model keeps its own BPLxPT/SPRxPT. These are loaded by CPU or copper writes, then
    advanced by the bitplane fetch at DDFSTRT and the per-line modulo, and by sprite DMA.
  - Mid-line changes: palette and other writes are applied at the colour clock where they happened.
  - Supported features:
    - DIW/DDF, lores and hires (hires pairs are averaged into one output pixel);
    - BPLCON1 scroll;
    - dual playfield with PF2PRI;
    - HAM and EHB;
    - 8 DMA or manual sprites, with attached pairs;
    - sprite-sprite and BPLCON2 sprite-playfield priority.
  - Output: a 320x256 RGBA framebuffer for the standard PAL window, from x `$81` and line `$2C`. Each 4-bit
    gun is expanded x17.
  - Approximations:
    - single-playfield priority uses PF2P;
    - sprite H `$80` lines up with DIW `$81`;
    - sprite DMA starts at line 25;
    - collisions (CLXDAT) are not computed, and CLXDAT reads `$8000`. The game never reads it.
  - What the game uses: only a 4-plane lores single playfield and a 200-line window (lines `$3A`-`$101`).
    Its picture is double-buffered by the blitter, and the copper swaps buffers at line 258, after the
    window. It uses no copper palette changes, sprites or scroll registers.
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

- Display timing is per colour clock for register writes, but bitplane data is fetched in one go at
  DDFSTRT. Mid-line BPLxPT/BPLCON0/DDF changes, interlace, ECS/AGA features and sprite/playfield collision
  are not modelled.
- Paula does not model:
  - ADKCON attach modulation;
  - manual AUDxDAT playback;
  - DMA-slot start latency.

  The game uses none of these. Audio output has not yet been compared with a reference A500 or WinUAE capture.
- There is no blitter timing and no DMA contention, so the CPU runs somewhat faster than on
  real hardware during heavy blitting.
- The copper runs in 16-colour-clock slots, not cycle-exactly.
- The raw MFM buffer `$45600-$495FF` is not filled during loads. The original loader would leave undecoded
  flux data there.
