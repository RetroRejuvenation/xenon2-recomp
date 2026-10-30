# Building and running x2recomp

Phase 3b state: an interpreter-driven runtime core with the following outputs:

- a scanline renderer (`runtime/gfx/`);
- Paula audio (`runtime/audio/`);
- an SDL3 window with keyboard and gamepad input (`runtime/input/`).

It also runs headless for tests. The game boots, runs its title and attract sequence, and with fire presses
(live or scripted) loads level 1 from disc 2 and plays it.

## Requirements

- CMake 3.29 or newer. The game-data test drivers end with `cmake_language(EXIT 77)` so that CTest reports them
  as Skipped without data, and that needs 3.29. Ubuntu 24.04 ships 3.28: use `pip install cmake` or Kitware's
  apt repository.
- A C++20 compiler and a C99 compiler for Musashi. These were built and tested: AppleClang on macOS (arm64, and
  x86_64 under Rosetta), GCC 13 and Clang 18 on Linux. MSVC 2022 and MinGW-w64 are set up in CI but have not
  been run yet (`docs/PORTING.md`).
- Python 3 only for the developer tools in `tools/` and the optional recompiled build. Importing disk images
  is native (`x2recomp --import`).
- SDL3, which is optional. There are two ways to get it:
  - An installed SDL3, found with `find_package(SDL3 CONFIG)`: Homebrew `sdl3`, a distribution package, vcpkg,
    or `-DCMAKE_PREFIX_PATH=<SDL3 prefix>`.
  - `-DX2_FETCH_SDL3=ON`: CMake downloads and builds the pinned SDL 3.4.18 release, checked by its SHA-256. This
    is the default in the Windows presets, and the only way to build for x86_64 or universal on an Apple Silicon
    Mac, because Homebrew's SDL3 is arm64 only. On Linux it needs SDL's X11, Wayland and audio development
    headers (`docs/PORTING.md`).

  Without SDL3, only `--headless` is built, and the window, input and SDL audio tests are left out.
  `-DX2_REQUIRE_SDL3=ON` makes a missing SDL3 a configure error instead; the presets and CI set it.
- Your own Xenon 2 Megablast disk images. Nothing from the game is in this repository.

## Licence

xenon2-recomp is GPL-3.0-or-later (`LICENSE`). Part of the accurate-timing code is derived from
WinUAE/Amiberry, which is GPL; `docs/PROVENANCE.md` records which code and why, and
`THIRD-PARTY-NOTICES.md` lists the third-party components. The game itself is not covered: you supply your
own disk images, and a `-DX2_RECOMP=ON` build contains code translated from them and must not be shared.

## Vendored code

| Path | What | Pinned version | Licence |
|---|---|---|---|
| `vendor/musashi` | Musashi 680x0 emulator by Karl Stenerud, used as the 68000 interpreter. **Three local modifications**, each marked in place with an `x2recomp:` comment: `m68kmake.c` and `m68k_in.c` fix an MSVC 2022 `/O2` miscompilation (the generated tables are byte-identical), and `m68kcpu.h` uses `_setjmp` on macOS/BSD (`docs/PROVENANCE.md` §9, `docs/PORTING.md`) | https://github.com/kstenerud/Musashi commit `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` (2026-03-08), modified | MIT-style (permits modification), see the "LICENSE AND COPYRIGHT" section of `vendor/musashi/readme.txt` |
| `vendor/puff` | puff, Mark Adler's small reference inflate, used by the importer for ZIP and ADZ/gzip | puff 2.3 (21 Jan 2013) from zlib 1.3.2 `contrib/puff`, unmodified | zlib licence, `vendor/puff/LICENSE` |
| `vendor/musashi/softfloat` | SoftFloat by John R. Hauser, as repackaged by MAME. **Not built**: `runtime/musashi_no_fpu.c` replaces `softfloat.c` in the `musashi` target, so none of it is compiled, linked or shipped. Only its headers are still included, because `m68kcpu.h` requires them | Release 2b, as shipped with Musashi | SoftFloat notice, `vendor/musashi/softfloat/README.txt`; `docs/PROVENANCE.md` §7.3 explains the removal |

puff was chosen over miniz because the importer only needs to inflate. It is two files, about 840 lines, and
checks its input bounds. CRC-32 and SHA-1 are implemented in `runtime/import/sha1.cpp`.

Only the core sources are kept (`example/`, `test/` and `.git` removed). At build time `m68kops.c` and
`m68kops.h` are generated into `build/musashi_gen/` by the vendored `m68kmake`. The runtime's Musashi options
are in `runtime/x2_m68kconf.h`, selected with `MUSASHI_CNF`. They give a plain 68000, autovectored
interrupts, and a per-instruction hook used for PC traps.

## 0. The whole pipeline in one command

```
tools/build_all.sh [--recomp] [--test] [--build-dir DIR] [--data-dir DIR] [DISK_IMAGES...]
```

This runs the following steps:

1. It configures and builds `build/` (Release; Ninja if available).
2. If images are given, it imports them (step 1 below).
3. With `--recomp`, it generates the recompiled code from the imported image and builds `build-recomp/` (see
   "Recompiled build").

`--test` then runs `ctest` on both builds. Steps 1-3 below show what it does.

## 1. Import your disk images (once)

```
x2recomp --import Xenon2.zip                 # a zip holding both ADFs
x2recomp --import disc1.adf disc2.adz        # or the two images (ADF or gzip'd ADF), in any order
x2recomp --import ~/Amiga/Xenon2/            # or a folder holding them (one level of subfolders)
```

You can also start `x2recomp` without imported data. The window then shows an import screen where you drop the
files, a zip or a folder. If only one disc is dropped, it keeps it and asks for the other. When the import has
worked, the game starts. Esc quits.

- **Accepted inputs:** raw 880 KB ADFs, ADZ (gzip), DMS archives (every compression mode; encrypted ones are
  refused), extended ADFs (`UAE-1ADF`, and the older `UAE--ADF`, as long as every track is a plain Amiga track or a
  raw MFM track that holds standard sectors), and ZIP archives (stored or deflate, nested up to 3 deep; other
  files in them are ignored). The format is recognised by the file's content, never its name. DMS notes are in
  `docs/re-notes/dms.md`.
- **Disc detection:** discs are found by content:
  - disc 1 is the AmigaDOS (OFS/FFS) disc whose file system holds the packed `XENON-II` executable;
  - disc 2 is the raw disc with the "X2B " id in sector 0.

  When several candidates are given, the importer prefers the images it recognises by SHA-1 (see
  `docs/re-notes/sources.md`): the BS1 `[t+3]` disks, then disk 1 `[a]`, then the Deejay99 pack.
- **Rejected inputs:** each rejection gets a message.
  - The single-disk cracks are recognised by hash or by a differently packed `XENON_II`: PNA, PNA[a], QTX,
    Black Monks, VF and VF[a]. They are unsupported because they are different builds that this runtime does
    not support.
  - IPF, SCP, KryoFlux, HFE and LHA files, HD images (also as DMS or extended ADF), encrypted DMS archives, and
    extended ADFs with raw protection tracks are unsupported for now (each says which).
  - These are reported as bad input:
    - truncated or oversized images;
    - zip, gzip or file-system blocks with bad CRCs or checksums;
    - a packed stream that does not depack cleanly;
    - unrelated disks;
    - a missing disc.
- **Unrecognised images:** a disc that is structurally valid but unknown is imported with a warning, as is a
  changed `XENON-II`.
- **Trainer:** for the trained BS1 release, the log says that the trainer loader and its patches are skipped by
  design.
- **Exit codes** of `--import`: 0 imported, 2 bad input, 3 unsupported image or format, 1 the output could
  not be written. Nothing is written unless the import works.

What is written (to `<data dir>/assets/`, never next to the executable):

- `game_0400.bin`: the native port of `tools/unpack_xenon2.py`'s decruncher applied to `XENON-II`, cut to the
  `$3FC00`-byte image the game runs from at `$400`.
- `disc2.bin`: disc 2 unchanged, 1760 linear sectors.
- `manifest.txt`: the importer version, the source paths, the SHA-1 of the source disks and the executable, and
  the SHA-1 of both output files.

Each file is written to a temporary file and then renamed. The old manifest is removed first and the new one is
written last, so an interrupted import never looks valid.

At start-up, the runtime checks the manifest's importer version and output hashes. If they no longer match, it
re-imports from the recorded source paths when they still exist. Otherwise it shows the import screen (window)
or exits with code 3 and the reason (headless).

The data directory is `SDL_GetPrefPath("xenon2-recomp", "x2recomp")`; `--data-dir DIR` overrides it:

- macOS: `~/Library/Application Support/xenon2-recomp/x2recomp/`
- Linux: `$XDG_DATA_HOME` or `~/.local/share/xenon2-recomp/x2recomp/`
- Windows: `%APPDATA%\xenon2-recomp\x2recomp\`

Builds without SDL use the same paths.

**Asset lookup order:**

1. `--assets DIR`;
2. `./generated/assets/` in the current directory, if it holds `game_0400.bin` (a developer checkout; the
   CTests pass `--assets` explicitly);
3. `<data dir>/assets/`.

`--import ... --assets DIR` imports into DIR instead, for example `--assets generated/assets` for the
developer layout.

The cracker intro (BS1), the depacker stub and the trainer stub are never run, so the trainer patches are not
applied. With the BS1 `[t+3]` disks the SHA-1 hashes are:

- `game_0400.bin`: `1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4`
- `disc2.bin`: `a748afc7a142335a40cc46fb7785dea820770241`

The `game_import` test checks these.

`python3 -I tools/prep_assets.py --zip Xenon2.zip` (or `--disc1 d1.adf --disc2 d2.adf`) is the original
Python path for developers. It writes the same two files to `generated/assets/` without a manifest.

## 2. Build and test

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Or use a preset from `CMakePresets.json`. The build goes into `build-<preset>/`, which is gitignored:

```
cmake --preset macos-arm64            # also macos-x86_64, macos-universal, linux-gcc, linux-clang,
cmake --build --preset macos-arm64    #      windows-msvc, windows-clangcl, windows-mingw
ctest --preset macos-arm64
```

On Windows, `tools\build_all.ps1` does what `tools/build_all.sh` does. Two things to check there first:

- **CMake 3.29.** Several Windows installs ship something older and put it on `PATH` — Strawberry Perl's 3.26
  and the standalone CMake installer's 3.25 were both ahead of anything newer on the machine in
  `docs/WINDOWS-VERIFICATION.md`, and `cmake --preset` then fails on the preset version. Visual Studio bundles
  a current one at `<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`; run `cmake --version` and use
  that path if what you have is older. `build_all.ps1` and `package.ps1` call `cmake` by name, so whichever one
  is first on `PATH` is the one they get.
- **`windows-clangcl`** uses Visual Studio's ClangCL toolset, which comes from the "C++ Clang tools for
  Windows" VS component. A standalone LLVM install does not provide it; to use one of those, configure with
  `-G Ninja -DCMAKE_CXX_COMPILER=<llvm>/bin/clang-cl.exe` from a developer command prompt instead.
- **`-DX2_STATIC_CRT=OFF`** if you would rather link the MSVC C runtime dynamically. It is on by default so
  that a package runs on a machine with no Visual C++ redistributable, which is what a user has
  (`docs/PORTING.md`). It applies to the fetched SDL3 as well, so changing it needs a clean build directory.

`cmake --install build --prefix DIR` installs the following:

- `x2recomp`;
- the SDL3 shared library;
- `LICENSE`, the documents (including `THIRD-PARTY-NOTICES.md` and `PROVENANCE.md`) and the third-party
  licence texts in `licenses/`.

`tools/package.sh` and `tools/package.ps1` make release archives from that (`docs/RELEASING.md`).

Every test runs without game data and without a display or sound device: SDL uses its dummy drivers in tests.
The tests that need your data report **Skipped** without it. Two tests are about the repository itself:

- `check_no_game_data` fails if git tracks game data, disk or CD images, audio, ROMs, picture dumps, build
  output or generated code (`tools/check_no_game_data.py`).
- `check_no_game_data_selftest` runs that tool's own unit tests.

These tests are defined:

- `import` needs no game data. It builds synthetic inputs in memory and checks how each is handled:
  - an OFS disc 1 holding a fake `XENON-II` packed in the game's cruncher format, and a fake "X2B " disc 2;
  - wrong, truncated, oversized and damaged images;
  - ADZ;
  - ZIP: stored, deflate, nested, CRC-corrupted, encrypted and empty archives;
  - DMS (header, CRC, checksum, encrypted, partial, truncated, modes none, simple, quick, medium and heavy),
    extended ADF (normal and raw MFM tracks, protection tracks), IPF, SCP, KryoFlux, HFE, LHA and HD images;
  - folders;
  - the asset writer, manifest verification and re-import;
  - the `--import` exit codes;
  - about 270 truncated, bit-flipped and garbage inputs, which must not crash.
- `game_import` needs your own `Xenon2.zip` and/or `Xenon-2-Megablast_Amiga_EN.zip` in the source tree (or
  `-DX2_DISK_ZIPS=...`) and is **Skipped** without them. It does the following:
  - imports each archive whole, every nested image of the Amiga_EN set alone, and the BS1 and Deejay99 pairs;
  - checks the accept/reject verdict and the message for each, and prints a table;
  - checks that the imported `game_0400.bin` is byte-identical to the known-good image and to
    `generated/assets/game_0400.bin` when that exists;
  - boots to level 1 (as `game_boot`) from the per-user data directory alone, in an empty working directory
    without `--assets`.
- `smoke` needs no game data. It checks the bus and mirroring, custom-register SET/CLR semantics, the beam
  counters, CIA timers and TOD, VBL and CIA interrupt delivery, the keyboard serial handshake, copper
  MOVE/WAIT, the blitter (shift, masks, fill) and the disk shim.
- `game_boot` needs `generated/assets/` (or set `-DX2_ASSETS_DIR=...`, for example to your imported
  `<data dir>/assets`). It runs 1500 frames, pressing fire
  every 50 frames from frame 400. It passes only if all of these happen:
  - the VBL handler runs;
  - the disc-id check passes;
  - level 1's main loop runs;
  - bitplanes are displayed;
  - nothing restarts through `$7032`.

  Without assets this test is reported as **Skipped**, not Passed.
- `timing` needs no game data. It checks the accurate timing model on synthetic programs and blits: the
  313-line frame and LOF; blit times per channel combination and fill mode (against the reference's
  measured times); bitplane DMA slowing the blitter; BBUSY and the blitter interrupt; BLTPRI; the 68000 table
  fixes and exact DIVU/DIVS times; CPU wait states with 4 and 6 lores planes; the CIA E-clock wait.
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
- `replay_<name>`, one per `tests/replays/<name>.txt`, needs game data and is **Skipped** without it. It plays
  the input script headless and compares the frame hashes and cumulative audio hashes (every 500 frames and
  at the last frame) with `tests/replays/<name>.hashes`; a restart through `$7032` also fails it. The corpus
  and its coverage-only pokes are described in `tests/replays/README.md`. After an intended change, re-record
  with `python3 -I tools/trace/run_corpus.py --record-hashes --no-trace`.

### Recompiled build (optional, Phase 5a)

`-DX2_RECOMP=ON` links the statically recompiled game code: 68000 code translated to C++ by
`tools/recomp/recomp.py`. The hybrid executor runs it and falls back to the Musashi interpreter at any
instruction boundary. Translated code is exactly equivalent to the interpreter, frame for frame, register write
for register write and sample for sample. See `docs/re-notes/recompiler-design.md` and
`docs/re-notes/recompiler-report.md`.

The recompiled build is **optional**. The interpreter build is fully functional, and the translated build
must, and does, give bit-identical results. Translation needs seeds, meaning the entry points of the game's
code. There are two sources, and neither can be shipped, because both are derived from the game:

- **The Ghidra code map**, `generated/lane-d2/`. This is a developer-only analysis product that needs Ghidra
  (`tools/ghidra_x2.py`).
- **Coverage traces.** These are recorded by replaying the committed input scripts (`tests/replays/*.txt`)
  on your own imported game: `tools/trace/run_corpus.py`. Only the scripts are committed, so anyone with the
  disks can re-create the traces.

End users take the second path, which is what `tools/build_all.sh --recomp` runs:

```
A="$HOME/Library/Application Support/xenon2-recomp/x2recomp/assets"     # your imported assets
python3 -I tools/trace/run_corpus.py --exe build/x2recomp --assets "$A" --out build/x2-trace
python3 -I tools/recomp/recomp.py --no-lane --assets "$A" --trace build/x2-trace --out build/x2-recomp-src
cmake -S . -B build-recomp -G Ninja -DX2_RECOMP=ON -DX2_RECOMP_DIR=build/x2-recomp-src -DX2_ASSETS_DIR="$A"
cmake --build build-recomp
ctest --test-dir build-recomp --output-on-failure
```

How `recomp.py` gets its seeds:

- **Without a code map** (no `--lane` directory, the default being `generated/lane-d2`, or with `--no-lane`,
  which `build_all.sh` always passes so that every machine builds the same thing), it seeds only from
  `--trace`/`--profile`. It uses the level-module sector table and interrupt vectors built into the script.
  `run_corpus.py` skips its Ghidra comparison report when there is no code map.
- **In a developer checkout** with `generated/lane-d2`, the code map and the traces are combined:

  ```
  python3 -I tools/recomp/recomp.py --trace generated/trace   # generated/assets + lane-d2 -> generated/recomp
  cmake -S . -B build-recomp -G Ninja -DX2_RECOMP=ON
  ```

Either way, the translated C++ is derived from your game image. Keep it in the build or `generated/` folders,
which are gitignored, and do not distribute it.

- **CLI options.** In this build `x2recomp` uses translated code by default, plus these options:

  | Option | Meaning |
  |---|---|
  | `--interp-only` | run everything on the interpreter, for A/B comparison |
  | `--recomp-stats N` | print the translated/interpreted instruction counts and the N most-interpreted PCs |
  | `--recomp-profile FILE` | write the PCs that were still interpreted (`recomp.py --profile FILE` translates them) |
  | `--hash-every N` | print `framehash` every N frames (available in every build) |

- **Extra tests:**
  - `recomp_difftest`: each translated function is run from random states on both executors, and all CPU,
    memory, chip and bus state is compared.
  - `recomp_equivalence`: a scripted 2000-frame title, menu and level 1 run in both modes must give
    identical per-frame framebuffer hashes, register-write logs, WAV and chip RAM, with at least 90% of
    executed instructions translated.
  - `recomp_replay_<name>`: the same comparison and coverage check for each replay in `tests/replays/`.

  Both tests are **Skipped** without game data.

## 3. Run

```
./build/x2recomp [--assets generated/assets] [--scale 3] [--fullscreen] [--aspect]           # window
./build/x2recomp --headless --frames 500 [--dump-dir out] [--assets generated/assets]       # no window
./build/x2recomp --version | --help | --print-data-dir                                     # print and exit (0)
```

The window runs the emulated 50 Hz frames on a fixed timestep, whatever the host refresh rate:

- Frame n is due at start + n x 20 ms, in integer nanoseconds, so there is no drift.
- When the host falls more than 4 frames behind, the schedule is rebased.
- It presents with vsync, using integer-scaled square pixels, or a 4:3 fit with `--aspect`.

Default controls:

| Key | macOS | Action |
|---|---|---|
| arrows, Z, X or Left/Right Ctrl | arrows, Z, X, Left/Right Option | joystick port 2 (the one the game reads) |
| W A S D, Left Shift | same | joystick port 1 (wired, unused by the game) |
| F11 or Alt+Enter | Cmd+F | fullscreen toggle |
| Pause or F9 | Cmd+9 | freeze the emulation |
| F12 | Cmd+R | hard reset |
| F10, Ctrl/Cmd+Q, closing the window | Cmd+Q | quit |
| F1 | Cmd+1 | Faithful switch on/off (the original picture only) |
| F2 | Cmd+2 | next window filter: nearest, sharp, scanlines, crt, edge |
| F3 | Cmd+3 | interpolation on/off |
| F4 | Cmd+4 | 4:3 aspect on/off |
| F5 / F6 | Cmd+5 / Cmd+6 | window scale down / up |
| F7 / F8 | Cmd+7 / Cmd+8 | volume down / up |
| Home | Cmd+U | asset packs (replacement graphics) on/off |
| End | Cmd+E | next widescreen surround: off, 16:10, 16:9, 21:9, 32:9 |
| Page Down | Cmd+D | HD status panel on/off |
| Page Up | Cmd+K | next music source: amiga, cdtv, mix (CDTV CD music, see below) |
| Scroll Lock | Cmd+G | keyboard grab on/off |
| Tab | Cmd+T | autofire on/off (assist) |
| Backspace | Cmd+I | god mode on/off (cheat) |
| Insert | Cmd+0 | add a credit (cheat) |
| Delete | Cmd+Y | protect music on/off |
| any other key | same | sent to the game as an Amiga key; the game uses P (pause), ESC (back to menu) and Space |

`x2recomp --list-keys` prints the bindings this machine will use, including anything `input.cfg` changes, and
exits without touching the file.

Gamepads are hot-pluggable. The first one connected drives port 2 and the second drives port 1. The d-pad or
left stick gives directions, and any face button fires.

#### macOS keys

The macOS defaults bind no Ctrl key. Ctrl with an arrow key is a system-wide shortcut there (Ctrl+Left and
Ctrl+Right move between Spaces, Ctrl+Up opens Mission Control, Ctrl+Down App Expose), so holding fire while
steering moved windows between screens and changed the focus instead of shooting. Fire is Z, X or either
Option key; Left Shift still fires for port 1.

The F-keys are still bound and are the right keys when "Use F1, F2, etc. keys as standard function keys" is on
in System Settings > Keyboard, or while you hold fn. Without that, macOS keeps F3 (Mission Control), F4
(Spotlight/Launchpad), F5/F6, F7-F9 (media), F10-F12 (volume) for itself and the key never reaches the game,
which looks like a keypress that needs a second try. The Cmd combinations in the table are not claimed by the
system and always work. Cmd+Q, Cmd+H, Cmd+M, Cmd+W and Ctrl+Cmd+F belong to the application menu SDL installs
(quit, hide, minimise, close, native fullscreen) and are deliberately not used for anything else.

Two SDL hints are set before the window opens, on macOS only. `SDL_MAC_PRESS_AND_HOLD=0` (which SDL turns into
the `ApplePressAndHoldEnabled` user default) makes holding a letter key repeat it, instead of the macOS default
of suppressing the repeat and offering the accent menu for A, S, Z and the other accent-capable letters — here
those are steering and fire keys, not text. `SDL_MAC_OPTION_AS_ALT=both` stops the Option fire keys starting a
dead-key composition. Both can be overridden with the environment variables of the same name.

#### Assists and cheats

Off by default, and never applied unless asked for, so the replay and golden-frame tests cannot see them.
They are applied in the host frame loop at the same point as the coverage pokes — before `m.run_frame()`, not
in an interpreter hook — so the interpreter and the translated executor behave identically.

| Option | Setting | Hotkey | What it does |
|---|---|---|---|
| `--autofire[=RATE]`, `--no-autofire` | `autofire`, `autofire_rate` | Tab / Cmd+T | pulse the fire line while fire is held. RATE is pulses per second, 1..25, default 17. |
| `--god-mode` | `god_mode` | Backspace / Cmd+I | refill the ship's energy (`$CC6`) to `$27` every frame |
| `--credits N` | `credits` | Insert / Cmd+0 adds one | credits (`$CF0`) a new game starts with, 1..9 |
| `--cheat-scores` | `cheat_scores` | — | save high scores even after a cheat was used |

**Autofire.** The game latches a fire *press* as an edge in its VBL handler (`$20DE` builds the state in
`$208E`, and `$2144`-`$214A` sets the latch `$A01` only on a not-down-to-down transition), and the player's
weapon fires again only on a fresh press plus its own cooldown. Measured with `--audio-cues`, counting the
shot sound `$0289` over 400 frames of level 1: fire **held** gives 5 shots, tapped every 2 frames gives 60,
every 3 frames 61, every 4 frames 44, every 6 frames 31. So a pulse every 2-3 frames reaches the game's
ceiling of about one shot per 6.6 frames, and the default rate of 17/s (every third frame) sits on it.

Autofire makes the game fire far more sound effects, and the game's effects take AUD1 and AUD2 from the
music while they play, so the tune thins out more often with it on. That is the original's own behaviour;
`docs/re-notes/music-dropouts.md` has the measurements.

Autofire would break the menus if it pulsed there, because the title screen, SELECT menu, shop and continue
prompt all read the same press as a selection. Two things prevent it: it only engages while a game is in
progress (the ship count `$CCA` is 0 at the title and in the SELECT menu, and non-zero only during a level),
and only after fire has been held for 15 frames (about 0.3 s), so the first press and any tap are delivered
exactly as they came in. `AutofirePolicy` in `runtime/input/input.h` is the whole decision and is unit tested.

**God mode** covers both of the game's death paths, and does it with a PC trap rather than by patching code.

The usual death is energy: refilling `$CC6` to `$27` every frame means `sub.w d0,$CC6` in the damage routine
never reaches zero, so the death call at `$5D86` is never made.

The other death costs no energy. `$42E` is a push-back counter: when the ship is forced back along its own
recorded trail the handler sets it to -1 (`$6042`) or -`$F` (`$6142`) and decrements it once per logic step
(`$5FE6`), and when it reaches -`$11` the code at `$5FE0` calls the death routine directly with the energy
bar still full. This is not theoretical — `tests/replays/boss1.txt` hits it at frame 2381, after 13 steps,
and `--pc-log 2300-2400:5FE0,5D00,5FD6:LOG` shows the whole countdown and the call. It is the only
energy-independent death: with god mode on, `$5D00` is reached from nowhere else across the `level1_play`,
`game_over`, `two_player` and `boss1` replays, and `$5D00` has exactly two callers in the listing.

So god mode also installs a trap on the death routine `$5D00` itself, which catches both callers at once. The
trap refills the energy, clears `$42E` and calls `Machine::do_rts()`, the same mechanism the disk shim uses:
the routine returns as if it had done nothing, the dying flag `$D84` is never set, and the ship's handler
carries on as though the hit had been survived. Clearing `$42E` is what stops the sequence sitting on its last
step and dying again immediately — measured, the hook fires **once** in boss1, the push-back then restarts
from -1 and is released normally, and the run goes on to level 2 with the same scroll position as the
baseline. The trap is only installed when god mode could be used (`--god-mode`, or any windowed run so the
hotkey works), so a headless run without it is left completely alone.

A trap and not a byte patch because the translated executor verifies each instruction's bytes before running
its translation: patching `$5D00` or the `bhi.b` at `$5D84` would push that routine back to the interpreter and
change what the recomp build executes. A trap that moves the PC ends the translated run and the executor
continues at the new PC exactly as `m68k_execute()` would (`runtime/recomp/x2r_runtime.h`, `X2R_BOUNDARY`),
so the two agree; `ctest -R cheats_god_mode_equivalence` compares god-mode frame hashes from the translated
executor with `--interp-only` and requires them identical.

Refilling energy rather than patching the damage branch also matters for how it looks. The `[t+3]` crack's own trainer patched the
damage routine's `bhi.b` at `$5D84` to `bra.b`, which skips the death call but still runs `sub.w d0,$CC6`
first, so the energy word goes to zero or negative and the HUD bar drains or draws from a wrapped value.
Refilling instead keeps the energy HUD full and correct, leaves the hit flash, the sound and the shield
behaviour (`$D83` halves damage) exactly as they were, and modifies no game code. The largest single hit is
`$10` out of `$27` (`$5F98`/`$5FA0` pass 8 or 16 to the damage routine), so a frame's worth of hits cannot
empty the bar before the next refill.

Three tests cover it, all skipped without game data: `cheats_god_mode` runs `tests/replays/game_over.txt` —
a replay whose pokes exist purely to kill the ship — first unchanged (it must still reach GAME OVER with
`$CCA` and `$CF0` both 0, and the test fails loudly if it stops doing so) and then with `--god-mode`, where
nothing dies and the energy is still `$27`; `cheats_god_mode_crush` does the same for the push-back death in
`boss1`, requiring the baseline to execute `$5FE0` and god mode to report skipping a death called from
`$5FE4` and still finish on the same level; and `cheats_god_mode_equivalence` checks the translated executor
against `--interp-only`.

`$CA4` looked like a ready-made invulnerability flag — both the damage routine and the death routine return
early when it is set — but it is a count of live objects (incremented at `$2220`/`$228C`, decremented at
`$466E`/`$473C`, and reaching 0 sets the shop request `$A6C`), and the main loop at `$7B72` skips the shop
check, the level-complete check `$A07` and the whole end-of-level path while it is non-zero. Holding it would
stop a level ever finishing, so it is not used.

**Credits.** The game has no credits option: `options_menu` `$8CB6` and `$6BB4` both just `move.w #3,$CF0`,
and the SELECT menu's only items are 1 PLAYER / 2 PLAYER GAME and MUSIC ON / OFF. One extra credit is awarded
at the end of a level (`$7C84 addq.w #1,$CF0`), `$7B1C` spends one on death, and 0 means GAME OVER (`$7B4C`)
instead of the continue prompt (`$8E88`). The display `$8F56` adds `$30` to the count and pokes the single
resulting character into the "CREDITS n" string, so **9 is the highest value that can be drawn** and both the
option and the hotkey stop there. `--credits N` is applied once, when a new game starts (the frame the ship
count goes from 0 to non-zero), so a continue still costs a credit as usual.

**Cheat flagging.** Using god mode or adding a credit, or starting with more than the game's own 3 credits,
marks the session. The window title gains `[CHEAT: god mode]` or `[CHEAT used]`, each toggle prints a line,
and the high-score file is not written for the rest of the run — the in-game table still shows the score, it
just does not persist. `cheat_scores = on` saves anyway. Autofire is an assist and never sets the flag.

#### Windows: a ring around the pointer when you hammer Ctrl

Windows has an accessibility setting, **Control Panel > Mouse > Pointer Options > "Show location of pointer
when I press the CTRL key"** (`main.cpl`, or Settings > Bluetooth & devices > Mouse > Additional mouse
settings). With it on, Windows draws an expanding ring at the pointer on every Ctrl press, which is constant
while firing. It is the operating system doing that, not the game, and the fix is to turn that setting off.

Z and X are bound as fire too, and are listed first in the Windows and Linux defaults for this reason, so
firing without touching Ctrl needs no configuration. Ctrl stays bound because it is the key the Amiga used.
Hiding the mouse cursor (below) is **not** known to suppress the ring: Windows draws it from the pointer
position regardless of whether the application draws a cursor there, and that has not been tested here.

#### Mouse cursor

The game has no mouse input — what the Amiga called the mouse port is joystick port 1 here, and SDL's mouse
events are never read, so no click or movement can reach the game. The cursor is therefore only in the way
while playing.

| `cursor` / `--cursor` | Behaviour |
|---|---|
| `auto` (default) | hidden while the window has the focus and the game is running; shown again while frozen (F9/Pause), when the window loses the focus, and at exit |
| `show` | always visible |
| `hide` | hidden whenever the window has the focus, frozen or not |

Fullscreen behaves the same way. This uses `SDL_HideCursor()`, which only stops drawing the cursor: the
pointer still moves normally and still belongs to the window manager. `SDL_SetWindowRelativeMouseMode()`,
which would additionally confine and capture the pointer, is deliberately not used — there is nothing to
capture it for. The first-run import screen runs before the cursor policy exists, so the cursor is always
visible there (you need it to drag disk images onto the window). `--input-debug` logs each change as
`[input] <ms> cursor hidden|shown (<why>)`.

#### Keyboard grab

`--grab-keyboard` / `--no-grab-keyboard`, `grab_keyboard = auto|on|off` in `settings.cfg`, or the Scroll Lock /
Cmd+G hotkey: ask the platform to route its own keyboard shortcuts to the window while it has the focus. It is
released on focus loss, while the game is frozen, and on exit, and `auto` means off on every platform today,
so you have to ask for it.

On macOS it does nothing: `SDL_SetWindowKeyboardGrab` only disables the global hotkeys when SDL itself was
compiled with `SDL_MAC_NO_SANDBOX`, which neither Homebrew's SDL3 nor the pinned `X2_FETCH_SDL3` build
defines, and it reports success either way. The safe default bindings above are the fix there. On Windows and
X11/Wayland the grab does work; quit (F10, Cmd/Ctrl+Q, closing the window) always stays available.

#### Diagnosing a key that needs a second press

`--input-debug` (or `input_debug = on`) prints the active bindings and then, to stdout, one line per keyboard
and focus event (timestamp in ms, scancode name and number, keycode, modifiers, the repeat flag, and the
action or Amiga raw key it mapped to), one line per gamepad button, and one line for every emulated frame
whose joystick state changed, naming the frame number and any press that only arrived as a tap:

```
[input] 41231 down Z    sc=29 key=0000007A mod=0000 rep=0 p2_fire
[input] 41248 up   Z    sc=29 key=0000007A mod=0000 rep=0 p2_fire
[input] 41250 frame 2061 p2=----F p1=----- keys=0 tap=p2_fire
[input] 41270 frame 2062 p2=----- p1=----- keys=0
[input] 44102 focus lost: dropped 1 held, released 0 amiga
```

A key the OS swallowed produces no line at all, which separates "the window never saw it" from "the window saw
it and dropped it".

#### Where the files live

Bindings are stored in `input.cfg` in the per-user data directory (`--data-dir DIR`, by default the SDL
preferences directory; on macOS that is `~/Library/Application Support/xenon2-recomp/x2recomp/`). `settings.cfg`
and `highscores.bin` are kept in the same directory, so `--data-dir` moves all of them together with the
imported assets. Headless runs (tests, replays) never read or write any of them unless `--persist-dir` is given. The file is written with the defaults on first run.
Its format is `action = Key Name, Key Name`, using SDL scancode names.

The file carries a `# version:` marker. A file you have edited, or one from a newer version, is used exactly as
it is and never rewritten. A file that holds nothing but the defaults of an older version is replaced with the
current defaults and kept as `input.cfg.bak`, which is how a Mac that already had `p2_fire = Left Ctrl, Right
Ctrl, Z` on disk picks up the macOS bindings. Deleting `input.cfg` always restores the current defaults.

| Option | Meaning |
|---|---|
| `--headless` | no window. Without SDL3 this is the only mode. |
| `--frames N` | number of 50 Hz frames to run (headless default 500; the window runs until quit) |
| `--no-audio` | window without sound. By default the window plays Paula through `SdlAudioOut`, with 40 ms latency and drift control. |
| `--audio-latency MS` | target output buffer, 10-500 ms (default 40; settings key `audio_latency`). The emulation tops the buffer up whenever it falls below this, so it is also how long the host loop may stall before the sound breaks up. If you hear it break up, raise it; `docs/re-notes/music-dropouts.md` §5 lists what else can cause a dropout. |
| `--list-keys` | print the key bindings this run would use and exit (0); reads `input.cfg` without changing it |
| `--grab-keyboard`, `--no-grab-keyboard` | send the system's own keyboard shortcuts to the window while it has the focus ("Keyboard grab" above). Default off. |
| `--cursor MODE` | mouse cursor: `auto` (hidden while playing, the default), `show` or `hide` ("Mouse cursor" above) |
| `--input-debug` | log every key, focus change, gamepad button and joystick change to stdout ("Diagnosing a key that needs a second press" above) |
| `--autofire[=RATE]`, `--no-autofire` | pulse the fire line while fire is held, during a level only ("Assists and cheats" above) |
| `--god-mode` | CHEAT: refill the ship's energy every frame |
| `--credits N` | credits a new game starts with, 1..9 (the game's own value is 3; above that is a cheat) |
| `--cheat-scores` | save high scores even in a session that used a cheat |
| `--scale N`, `--fullscreen`, `--aspect` | initial window scale (default 3), start fullscreen, 4:3 picture instead of square pixels |
| `--screenshot-at F:FILE` | save rendered frame F as FILE: PNG if it ends in `.png`, otherwise PPM. Repeatable. |
| `--hash-at F` | print `framehash F <fnv1a64>` of the rendered RGBA frame F. Repeatable; used by `golden_frames`. |
| `--dump-dir DIR` | output directory (optional; without it no register logs or dumps are written) |
| `--assets DIR` | location of `game_0400.bin` / `disc2.bin` (default: `./generated/assets` if present, else the import in the data directory; see step 1) |
| `--import PATH...` | import disk images (ADF, ADZ, DMS, extended ADF, ZIP, folder) into `<data dir>/assets` (or `--assets DIR`) and exit: 0 ok, 2 bad input, 3 unsupported. A single CDTV disc image (a `.cue`, or a ZIP or folder holding one) goes to the CD-audio import instead ("CDTV CD music" below). |
| `--data-dir DIR` | per-user data directory: the imported assets and, in window mode, `settings.cfg`, `input.cfg` and `highscores.bin` (default: the SDL preferences directory) |
| `--report-every N` | status line interval (default 50 frames) |
| `--ppm-every N` | dump the bitplane framebuffer every N frames as `frame_<n>.ppm` |
| `--no-reglog` | skip the per-frame register logs |
| `--trace-frame F` | disassembly and register trace of every instruction in frame F (`trace_F.txt`) |
| `--key F:CODE` | press Amiga raw key `CODE` (hex) at frame F; it is released 2 frames later |
| `--fire F[:N]` | hold joystick fire (port 2) from frame F for N frames (default 3). In a window, live input is used in all other frames. |
| `--joy F:DIRS[:N]` | hold joystick directions from frame F for N frames (default 1); DIRS uses the letters `U D L R F` (F = fire) |
| `--input-script FILE` | read frame-indexed input (and `frames N`) from FILE; the format is in `tests/replays/README.md` |
| `--poke F[-G]:ADDR:VALUE[.b\|.w\|.l]` | **coverage runs only**: write VALUE to chip RAM at ADDR at the start of frames F to G. Nothing is ever poked by default; a run with pokes prints `coverage pokes active`. Input scripts also have `pokeobj` (a write into every object with a given hit handler, `tests/replays/README.md`). |
| `--trace-out FILE` | record execution coverage as JSON (see "Coverage tracing" below) |
| `--trace-label NAME` | label stored in the trace (default: the file name without extension) |
| `--audio-hash` | with `--hash-at`, also print `audiohash F <fnv1a64>` over all Paula output up to the end of frame F |

For example, `--fire 460:5 --fire 950:5 --fire 1200:5 --frames 1500` goes from the title to the SELECT
menu, then to "GET READY PLAYER 1", then into level 1.

### Picture: Faithful switch, filters, interpolation, upgraded graphics (Phases 9b-9f)

The picture options live in `settings.cfg` (keys below), can be overridden on the command line, and are switched
at run time with F1-F4, Home, End and Page Down (bindable in `input.cfg` as `faithful`, `filter`, `interp`,
`aspect`, `asset_packs`, `wide`, `hd_panel`). Every key but F1 changes the custom picture and switches Faithful
off. The window title shows the active mode. The defaults give exactly the faithful picture.

| Option | settings.cfg | Meaning |
|---|---|---|
| `--faithful` | `modern = faithful` | **The Faithful switch.** Forces no interpolation, nearest-neighbour integer scaling and square pixels, and ignores every option below. The pictures shown are the rendered frames themselves, byte for byte (test `modern_faithful`). `modern = custom` uses the options. |
| `--filter NAME` | `filter = NAME` | `nearest` (default), `sharp` (integer pre-scale, then bilinear), `scanlines`, `crt` (scanlines, aperture-grille mask, bloom), `edge` (Scale2x/Scale3x edge-preserving upscale, then sharp) |
| `--aspect` / `--no-aspect` | `aspect = on/off` | 4:3 display (PAL lores pixels 16:15 wide) or square pixels |
| `--integer` / `--no-integer` | `integer = on/off` | integer scale factors only (default on; with 4:3 the vertical factor) |
| `--curvature` / `--no-curvature` | `curvature = on/off` | `crt` only: curved screen with a vignette |
| `--modern interp[,scale=N][,nosmooth]` | `interp`, `interp_scale`, `smooth_far` | interpolated frames at the display refresh rate, drawn into a canvas N times the Amiga resolution (default 3). `nosmooth`: the far layer moves exactly as in the step pictures (0 or 1 line per step) rather than at constant speed. `--modern nointerp` turns it off. |
| `--asset-pack PATH` (repeatable), `--modern upgrade` | `upgrade = on/off`, `asset_packs = A;B` | replacement graphics and palette overrides from asset packs (directories or ZIP files; `docs/ASSET-PACKS.md`). Later packs override earlier ones; `asset_packs` come before `--asset-pack`. `--asset-pack` implies `upgrade`. Without packs, `upgrade` changes nothing (test `modern_upgrade`). Works with and without interpolation. |
| `--modern wide=W:H` or `wide=N` | `wide = off / 16:10 / 16:9 / 21:9 / 32:9 / N` | widescreen surround: the picture is made W:H wide (the 256-line frame with square pixels, or 4:3 pixels with `--aspect`), or N Amiga pixels wider on each side (at most 320). The extra areas show scenery only; see below. |
| `--modern hdpanel` | `hd_panel = on/off` | the status panel drawn with the Scale2x/Scale3x edge-preserving upscale at the canvas scale (2, 3, 4, 6, 8) instead of pixel replication |
| `--dump-assets DIR` | - | headless or window: write every distinct original graphic drawn in gameplay as PNGs, with `DIR/manifest.json`, for pack authors (`docs/ASSET-PACKS.md`). These are your game's graphics: keep them to yourself. |

`--modern` also accepts `--modern=SPEC`, with comma-separated items, e.g. `--modern=upgrade,interp,wide=21:9`.
Any of `--filter`, `--aspect`, `--integer`, `--curvature`, `--asset-pack` or `--modern interp`, `upgrade`, `wide=`,
`hdpanel` on the command line selects the custom picture, unless `--faithful` is also given. `--faithful` turns
all of them off: the shown pictures stay the rendered frames byte for byte (`modern_faithful`).

**How the filters are drawn.** They use the SDL_Renderer API only: render-target textures, nearest and linear
sampling, MOD and ADD blending, tiled textures and `SDL_RenderGeometry` for the curvature. This runs unchanged
on every SDL3 back end (Metal, Direct3D, OpenGL, Vulkan and the software renderer). There are no shader sources
and no shader cross-compiler. `SDL_GPU` was not used, because it would need SPIR-V, MSL and DXIL copies of each
shader, and the build has no tooling for that. The edge upscaler is the only CPU pass. With interpolation, the
edge filter draws the interpolated picture at 1x, so motion moves in whole Amiga pixels.

**Interpolation** (`runtime/modern/`, design in `docs/re-notes/modernisation-design.md`):

- **What is shown.** Each logic step's draw list is replayed with its objects and layers moved part of the way
  towards the next step. The window shows the pair (N-1, N) while step N+1 is being computed.
- **Latency.** This adds one logic step (normally 3 frames, 60 ms) plus the presentation clock's lag, about
  1 to 1.5 frames (20 to 30 ms). The total is about 80 to 90 ms more than the faithful picture.
- **Timing.** The emulation runs on the audio clock, unchanged. The presentation clock (`PresentClock`) turns
  the bursty frame completions into a smooth emulated time. The vsync grid (`VsyncTimeline`) gives each present
  the display time of its refresh, measured from the vsync waits. On macOS a window is composited at 120 Hz even
  on a faster display; fullscreen runs at the display rate.
- **Fallbacks.**
  - Menus, the shop, the title and other non-gameplay screens show the faithful frames.
  - Cuts are not interpolated: a scroll jump, a cleared playfield, or most objects jumping more than 48 px at
    once.
  - A single object that jumps more than 48 px switches at the midpoint.
  - A step held for more than 8 frames (game pause, disk loads) falls back to the faithful frame.
  - Host pause (F9) holds the last picture.
  - Reset (F12) drops the captured steps.
- **Palette.** Palette fades are blended per channel between the two steps.

**Upgraded graphics** (Phase 9d, `runtime/modern/assets.{h,cpp}`, format in `docs/ASSET-PACKS.md`):

- **How.** The replay knows which draw command every pixel comes from. Each original graphic it draws (a bob or
  masked tile merged from its 4 plane blits, an opaque tile, a far-layer line) is identified by a hash of its
  content. A pack maps those keys to RGBA PNGs at 1-8x, with anchors, and can override the level palette per
  layer (far layer, tiles, objects, stars, other). The canvas keeps each pixel's colour index and layer and an
  RGBA colour on top, composited in the game's drawing order, so replacements may use any colours.
- **Without interpolation** the newest step is replayed in place: with the dump as a pack the pictures are the
  rendered frames exactly (`modern_upgrade`). The status panel then comes from the frame.
- **Bottom-clipped tiles and objects** (fewer rows drawn from the same source) use the replacement of their full
  graphic, clipped to the rows drawn. Graphics clipped at the left or right edge, and the hit-flash variants
  (minterms `FA`/`0A`), keep the original picture.

**Widescreen surround** (Phase 9e). The game's world is 320 pixels wide and objects are deleted just outside it
(`docs/re-notes/modernisation-design.md`, section 4), so the extra width shows scenery only:

- the outermost 16 pixels of each side of the playfield, as the far layer and the tiles left them before the
  stars and objects were drawn, mirrored outwards (back and forth, so the seam is continuous) and darkened with
  the distance from the playfield (to about 30% at the outer edge);
- extra stars, continuing the starfield: each is a copy of one of the game's 48 stars (same line and speed,
  moving with interpolation), at an x chosen by a fixed permutation;
- objects, bullets, explosions and the game's own stars are clipped at the original 320-pixel edge exactly as
  in the game. The status panel stays centred with black sides. Non-gameplay screens (menus, shop, title)
  and a step held for more than 8 frames (game pause) have black sides.

Without interpolation the centre is the rendered frame exactly (`modern_wide`). Visual notes: walls repeat
visibly (the wall decorations of the edge column, e.g. the level-1 pods, appear every 32 pixels), and level 3,
whose edge columns are often empty, shows mirrored far layer there.

**HD status panel** (Phase 9f). The panel (lines 192-199) is upscaled with Scale2x/Scale3x instead of pixel
replication at canvas scales 2, 3, 4, 6 and 8: digits, icons and the frame's slanted edges keep sharp edges and
get smoothed diagonals. It is the panel as presented (with interpolation, the step picture's panel; without, the
frame's). A re-drawn panel with an embedded font is not implemented (`docs/ASSET-PACKS.md`, "Out of scope").

Headless tools: `--modern-out F-G:DIR` writes the presented pictures at `--modern rate=HZ`. With DIR `-` it
prints `presenthash H F <hash>` instead, and with the surround also `centerhash H F <hash>` of the centre 320
columns. `--present-shot F:FILE` renders the window picture, filter included, with SDL's software renderer.

Audio options (Paula, `runtime/audio/`; details in `runtime/audio/paula.h` and `docs/re-notes/audio-notes.md`):

| Option | Meaning |
|---|---|
| `--audio-out FILE` | Write the exact Paula output of the whole run as a 48 kHz stereo 16-bit WAV. It is generated from emulated time, so no audio device is involved, and it is bit-identical between runs. Paula statistics are printed at the end. |
| `--audio-filter M` | `a500` (default), `a1200` or `none` |
| `--stereo-sep X` | stereo separation from 0 (mono) to 1 (default: channels 0 and 3 hard left, 1 and 2 hard right) |
| `--audio-cues FILE` | log every music/sound driver call as `frame line event d0 caller` |

### CDTV CD music (Phase 8, optional, off by default)

The CDTV release replaced the tracker tune with Bomb the Bass tracks on its CD. With your own CDTV disc image the
runtime can play those tracks where the disk game starts its music, and silence the tracker music while keeping
every sound effect. Without the disc nothing changes: the game boots and plays with Paula audio only, and the
default build's audio and replay hashes are the same as before.

Import the disc once (the Redump-style ZIP with a `.cue` and one `.bin` per track, a folder holding them, or the
`.cue` itself; the disc is recognised by its ISO volume `XENONII` and the twelve audio tracks):

```
x2recomp --import Xenon-2-Megablast_CDTV_EN-FR-DE-IT_Disc-Image.zip     # or --import-cdtv PATH
```

- **Storage:** the programme audio of tracks 2-13 as raw 16-bit stereo 44.1 kHz PCM, one file per track, in
  `<data dir>/cdtv/` (`--cdtv DIR` to choose another directory), with `manifest.txt` (sizes and CRC-32s). This
  takes **about 680 MB**; the import checks the free space first. ZIP members are inflated as a stream (memory
  stays bounded, about 64 KB of window plus buffers) and checked against the ZIP's CRC-32; each file is written
  under a temporary name and renamed, and the manifest is written last. At start-up the runtime checks each
  file's size and the CRC of its first MB. Exit codes: 0 ok, 1 cannot write, 2 bad input (missing, short or
  corrupted track), 3 not the Xenon 2 CDTV disc.
- **In place instead:** `--cdtv PATH` with a `.cue` or a folder holding the `.cue` and `.bin` files plays them
  where they are, without copying (settings key `cdtv`). A ZIP cannot be played in place.
- **Programme start:** the `.cue` says INDEX 01 at 137/138 sectors, which the data contradicts. Measured: sector
  0 of each audio file is the previous track's last sector, sectors 1-150 are silence, and the music starts at
  sector 151 (150 for tracks 12 and 13). The importer takes the first non-silent sector at or after INDEX 01
  and prints the start it found for every track (`runtime/cdtv/cdtv_disc.h`).

Which track plays where is in `docs/re-notes/cdtv-music-map.md`: title track 2, level 1 track 3, level 2 track 5,
level 3 track 2, level 4 track 3, level 5 track 5 (the CDTV game's defaults); the CD stops where the disk game
stops its music (leaving the title, the end of a level, the shop), and a track loops the way the CDTV game
loops it (re-issued when its 50-tick poll finds it finished, so there is a gap of up to a second).

| Option | Settings key | Meaning |
|---|---|---|
| `--music amiga\|cdtv\|mix` | `music` | `amiga` (default): tracker music. `cdtv`: CD tracks, tracker music muted, sound effects kept. `mix`: both. Without CD audio, `cdtv` and `mix` warn once and play the Amiga music. Page Up cycles the source in the window. |
| `--music-volume N` | `music_volume` | percent, CD tracks and tracker music (default 100) |
| `--sfx-volume N` | `sfx_volume` | percent, sound effects (default 100). `--volume` / F7 / F8 stay the master volume of the window output. |
| `--protect-music`, `--no-protect-music` | `protect_music` | **Off by default, and not what the original does.** The game's sound effects take AUD1/AUD2 from the music and the driver then writes nothing to Paula for that channel, so heavy firing silences a voice of the tune — on real hardware too (`docs/re-notes/music-dropouts.md` §7). On, Paula also plays that voice from the driver's own shadow registers, so the tune keeps going through the shooting. Audio only: the game's state, timing and every frame hash are bit-identical either way, and the option costs 3.9 dB of output headroom because one side can then carry three voices (worst peak measured over the corpus 0.85, nothing clipped). A no-op with `--music cdtv`. Delete, or Cmd+Y on macOS, toggles it while the game runs: `CdtvMusic::set_protect_music(bool)` fades the mirror voices and the headroom together over 20 ms so the switch cannot click, the new value is written back to `settings.cfg` on exit like the volume and scale hotkeys, and the window title carries `[protect music]` while it is on. The console line says when the toggle is inert because the CDTV disc is the music. `docs/re-notes/music-dropouts.md` §8.4 has the toggle's measurements. |
| `--cd-crossfade MS` | `cd_crossfade` | 0-500: loop seamlessly (the restart overlaps the end) and crossfade track changes (default 0 = as the CDTV game) |
| `--cd-pause keep\|duck\|stop` | `cd_pause` | during the game's own pause (P): keep playing (default, as the disk game's music does), play at a quarter, or hold the CD |
| `--cdtv PATH` | `cdtv` | the imported directory, or a `.cue` / folder used in place (default `<data dir>/cdtv`) |
| `--cdtv-tracks T,L1,L2,L3,L4,L5` | `cdtv_tracks` | the CD track (2-13) for the title and each level, like the CDTV game's SELECT TITLES screen (default `2,3,5,2,3,5`) |
| `--cdtv-log FILE` | | log each cue: `frame line event track caller` |

How it works: PC traps on the resident driver's `music_init`/`music_stop` and on the pause loop (the same trap
mechanism as the disk shim, so the translated executor and `--interp-only` give the same result) issue the cues;
the CD audio is resampled to the Paula output rate (32-tap windowed sinc) and added into Paula's output at the
output frame of each cue, so `--audio-out` renders and `--audio-hash` are exact and repeatable. The tracker music
is muted by Paula's per-writer output gains: each channel's sample pointer is tagged with the driver code that
wrote it (music or effect code), and only the output mix uses the tag. The game's state, timing and RNG are
unaffected (`cdtv_game` test). The CDTV speech clips and language selection are not used: they belong to
CDTV-only menus (`docs/re-notes/cdtv-music-map.md` section 4).

Tests: `cdtv` (synthetic CD images, no game data) and `cdtv_game` (your CDTV ZIP at `X2_CDTV_ZIP`, default the
repository root; Skipped without it; it imports into `X2_CDTV_TEST_DIR`, default `generated/cdtv_test`, about
680 MB shared by the build directories).

Timing and reference-comparison options (`docs/re-notes/timing.md` §8, `docs/re-notes/reference-compare.md`
section 10):

| Option | Effect |
|---|---|
| `--timing accurate\|fast` | the timing model (default `accurate`); `--timing=fast` also works |
| `--watch-log FILE` / `--watch A,B,...` | per frame: chip words (default the logic-step and RNG words `$404 $41C $1FEE-$1FF4`) at the start of the frame, the lines whose end found the CPU in the swap wait `$2034-$2041`, and the lines whose end found the blitter busy; the same columns as the reference harness's `frames.log` |
| `--pc-trace F-G:FILE` | `frame line pc waits` at the end of every line of frames F..G |
| `--pc-log F-G:PC,...:FILE` | `frame line hpos cycle pc` at the start of every instruction at one of the PCs |
| `--insn-log F-G:FILE` | `frame line hpos cycle pc opcode` for every instruction of frames F..G |
| `--blit-log F-G:FILE` | each blit's start (`S w h bltcon0 bltcon1`) and BBUSY clear (`E`) with its beam position |

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

In the window, the audio device is the master clock while sound is on:

- **Frame timing:** a frame runs whenever the queue falls below the 40 ms target, so the queue never grows and the 50 Hz wall-clock pacer is used only with `--no-audio`.
- **Stalled device:** if the device stops consuming audio for 100 ms, frames are run by wall-clock time again.
- **Pause and reset:** F9 (pause) stops the device and drops queued audio; F12 (reset) drops it too.
- **Quit:** closes the stream.
- **Exit report:** the program prints a `live audio:` line with underruns, dropped frames and the maximum queue size.

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

## Coverage tracing (Phase 5b)

`--trace-out FILE` attaches `x2::Tracer` (`runtime/trace/`). Without it the machine only checks a null pointer
in the instruction hook and the CPU write path. The tracer records:

- every executed instruction (PC, count, length) and its role: call target, taken-branch target, indirect
  target, interrupt entry, HLE trap (`$921A`, whose body the disk shim replaces);
- taken and not-taken counts for every Bcc/DBcc/BRA site;
- the targets of every `jmp`/`jsr` through a register or `d8(PC,Xn)`, and of every `rts` that does not return
  to the address its call pushed (a shadow stack), for example the push/rts at `$5924`;
- interrupt entries (vector target at the matching mask level);
- CPU and blitter writes into bytes of already-executed instructions, and opcode changes at executed PCs.

PCs in `$54E00-$7FFFF` belong to the module the disk shim loaded there. A module is identified by a hash of its
first 8 KB and named from the level table at `$91EA` (`level1`..`level6`; `level6` is the shop and ending).
Everything below `$54E00` is `game`.

```
python3 -I tools/trace/run_corpus.py           # replays -> generated/trace/runs/*.json, then merge + report
python3 -I tools/trace/merge_traces.py --out generated/trace generated/trace/runs   # coverage.json, indirect_targets.json
python3 -I tools/trace/report.py                # report.json / report.md vs generated/lane-d2 (Ghidra)
```

Results and their interpretation are in `docs/re-notes/trace-coverage.md`.

## Machine model and timing budget

The model is a PAL A500 (OCS, 512 KB chip RAM, 68000 at 7.09379 MHz).

- **Timing model** (`--timing`, `docs/re-notes/timing.md` §8): `accurate` (the default) models the OCS chip
  bus per colour clock: refresh, audio and bitplane DMA slots, the blitter's channel cycle sequence (BBUSY and
  the blitter interrupt when its last cycle has run, BLTPRI), the CPU's wait for a free slot on every bus
  cycle, E-clock CIA accesses, and exact 68000 times where Musashi's table is off (DIVU/DIVS, some `.L`
  ALU forms, ADDQ/SUBQ to An, bit operations). `fast` is the Phase 3 model: a zero-time blitter and no bus
  contention; it is kept for A/B comparison.
- **Frame:** non-interlaced, 227 colour clocks per line. The CPU gets 2 cycles per colour clock, so **454 CPU
  cycles per line**. Accurate timing has **313 lines** per frame, like the reference A500 (long fields only,
  LOF=1): **142,102 cycles, 49.92 Hz**. Fast timing keeps the Phase 3 frame of 312 lines (141,648 cycles,
  50.08 Hz). The window paces at the emulated frame rate when there is no audio device, and by the audio
  queue otherwise. Musashi's overshoot past a line's budget is carried into the next line.
- **Beam:** VPOSR/VHPOSR give the current line plus `hpos = cycles into the line / 2`, so polling loops see
  the beam move within a line (with accurate timing, the colour clock of the read's bus cycle). VPOSR bit 0 is
  V8, bit 15 is LOF (1 with accurate timing); the Agnus id reads 0.
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
  - The blitter-finished bit in WAIT is ignored (the game's copper lists never wait for the blitter).
- **Blitter:** area mode is complete: A/B/C/D, shifts, first/last-word masks, all 256 minterms, descending
  mode, inclusive and exclusive fill. The memory operation is done when BLTSIZE is written. With accurate
  timing the blit then runs its channel cycles in the free bus slots and DMACONR BBUSY and INTREQ BLIT follow
  them; with fast timing BBUSY always reads 0 and INTREQ BLIT is set at once. BZERO is computed.
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

  The game uses none of these. Audio output has been compared with an Amiberry capture (envelope correlation
  0.98-0.999 per 100-frame window, matching stereo balance; `docs/re-notes/reference-compare.md` §6), but not
  against real A500 hardware.
- The accurate timing model places a 68000 instruction's bus cycles by rule, not by its microcode, and
  leaves out copper and disk DMA slots and the interrupt-acknowledge timing (`docs/re-notes/timing.md`
  §8.6). Disc loads stay instant, so the game's timeline differs from a real A500 during and after loads.
- The copper runs in 16-colour-clock slots, not cycle-exactly.
- The raw MFM buffer `$45600-$495FF` is not filled during loads. The original loader would leave undecoded
  flux data there.
