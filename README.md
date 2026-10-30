# xenon2-recomp

A native port of the Amiga game **Xenon 2 Megablast** (The Bitmap Brothers, 1989), built by static
recompilation. The game's 68000 code runs on a machine model of a PAL Amiga 500: the copper, blitter,
Paula audio, CIAs and a scanline display. It runs in an SDL3 window or headless. The code runs on the Musashi
interpreter, and optionally as 68000 code translated to C++ (`-DX2_RECOMP=ON`). Both executors give the same
result, frame for frame.

## Legal position: bring your own game

This repository contains **no game data**: no disk images, code, graphics, music or translated code from the
game. To play, you need your own copy as Amiga disk images. The importer reads these on your machine and writes
what the runtime needs to your per-user data folder. Nothing from your images goes into the build tree, except
in the optional recompiled build (`-DX2_RECOMP=ON`). That build writes C++ generated from your game image into
the build folder; it is for your own use, so do not commit or share it.

## Licence

xenon2-recomp is free software under the **GNU General Public License, version 3 or later**
(GPL-3.0-or-later). See [`LICENSE`](LICENSE) for the full text.

It is GPL rather than permissive because part of the accurate-timing code is derived from WinUAE/Amiberry,
which is GPL. [`docs/PROVENANCE.md`](docs/PROVENANCE.md) records, file by file, which code that is, how
closely it follows the original, and the licence of each source; §7 has the compatibility reasoning and the
one point that remains unresolved.

Third-party components keep their own licences, all compatible with GPL-3.0-or-later:

- `vendor/musashi`: MIT-style (vendored with three local changes — two fixing an MSVC miscompilation, one a
  macOS/BSD `setjmp` optimisation — each marked in the files and described in `docs/PROVENANCE.md` §9);
- `vendor/musashi/softfloat`: SoftFloat Release 2b notice (headers only; nothing from it is linked or
  shipped, see `docs/PROVENANCE.md` §7.3);
- `vendor/puff`: zlib licence;
- SDL3, when bundled in a package: zlib licence (`packaging/licenses/`).

[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) lists every bundled and linked component.

**The game is not covered by any of this.** Xenon 2 Megablast's code, graphics, music and text are copyright
The Bitmap Brothers / Mirrorsoft. None of it is in this repository, and the GPL here grants no rights over
it. The same goes for CDTV CD audio: it comes from your own disc image. A `-DX2_RECOMP=ON` build contains
C++ translated from your game image and must not be redistributed.

## Supported release

Only the **two-disc BS1 "[t+3]" release** is supported, for example `Xenon2.zip` holding
`Xenon II - Megablast (1of2)[t+3].adf` and `(2of2)[t+3].adf`, or the same disks in TOSEC-style sets. The
importer tells the disks apart by content, not by file name. It also accepts these variants:

- the disk 1 `[a]` variant with an alternate boot block;
- the Deejay99 UAE-fix pack. Its disk 2 has 26 changed bytes, so the importer gives a warning.

This release is cracked and trained. The importer never runs the trainer loader, so the game image is used
exactly as packed on the disk: there is no trainer, and no infinite lives. The single-disk cracks (PNA, Quartex,
Black Monks, Vision Factory) are different builds of the game. They are recognised and rejected. DMS, IPF and
extended ADFs must be converted to plain ADFs first. `docs/re-notes/sources.md` catalogues the known images.

## Quick start

You need CMake 3.29 or newer, a C++20 compiler and SDL3. SDL3 can come from Homebrew `sdl3` or a distribution
package, or be downloaded and built with `-DX2_FETCH_SDL3=ON`. Without SDL3 only the headless runtime is built.
The build is exercised in CI on macOS (arm64, x86_64, universal), Linux (GCC and Clang) and Windows (MSVC
and MinGW-w64). CI runs without game data, though: the game itself has only been run on macOS and arm64
Linux (`docs/PORTING.md`).

```
tools/build_all.sh Xenon2.zip         # build, then import your disk images (ADF, ADZ, ZIP or a folder)
./build/x2recomp                      # play
```

You can also skip the import step and just start `./build/x2recomp`. On the first run the window asks you to
drag and drop your disk images, a zip, or a folder onto it. On the command line, use
`x2recomp --import PATH [PATH2]`, which exits with 0 if the import worked, 2 for bad input and 3 for an
unsupported image.

To build the optional recompiled executable as well, which needs Python 3, run
`tools/build_all.sh --recomp [--test] Xenon2.zip`. `docs/BUILDING.md` has every step and option.

## Controls

| Key | macOS | Action |
|---|---|---|
| arrows, and Z, X or Ctrl to fire | arrows, and Z, X or Option to fire | joystick (port 2, the one the game plays on) |
| W A S D, and Left Shift to fire | same | joystick port 1 (the mouse port; two-player mode) |
| P / Esc / Space | same | the game's pause, back-to-menu and space keys |
| F9 or Pause | Cmd+9 | freeze |
| F12 | Cmd+R | reset |
| F11 or Alt+Enter | Cmd+F | fullscreen |
| F10 or Cmd/Ctrl+Q | Cmd+Q | quit |
| F5 / F6 | Cmd+5 / Cmd+6 | window scale down / up |
| F7 / F8 | Cmd+7 / Cmd+8 | volume down / up |
| F1 | Cmd+1 | Faithful switch (the original picture only) |
| F2 / F3 / F4 | Cmd+2 / Cmd+3 / Cmd+4 | next filter (nearest, sharp, scanlines, crt, edge) / smooth interpolated motion / 4:3 aspect |
| Home / End / Page Down | Cmd+U / Cmd+E / Cmd+D | asset packs on/off / widescreen surround (off, 16:10, 16:9, 21:9, 32:9) / HD status panel |
| Page Up | Cmd+K | music source: Amiga tracker music / CDTV CD tracks / both (needs your CDTV disc) |
| Scroll Lock | Cmd+G | keyboard grab on/off (see below) |
| Tab | Cmd+T | autofire on/off (an assist) |
| Backspace | Cmd+I | god mode on/off (a cheat) |
| Insert | Cmd+0 | add a credit (a cheat) |

`x2recomp --list-keys` prints the bindings this machine will actually use.

On macOS the defaults bind no Ctrl key: Ctrl with an arrow key is a system-wide shortcut there (Ctrl+Left and
Ctrl+Right switch Spaces, Ctrl+Up opens Mission Control, Ctrl+Down App Expose), so firing while steering used
to move the window and change the focus instead of shooting. The F-keys stay bound and work when "Use F1, F2,
etc. keys as standard function keys" is on in System Settings > Keyboard, or when you hold fn; otherwise the OS
takes F3, F4, F5, F6, F10, F11 and F12 for itself and the Cmd combinations above are the ones to use. The
Home/End/Page keys are fn + the arrow keys on a Mac laptop keyboard.

`--grab-keyboard` (or `grab_keyboard = on` in `settings.cfg`) asks the system to send its own keyboard
shortcuts to the window while it has the focus. It is off by default, and on macOS it does nothing at all:
SDL's Cocoa backend only silences the global hotkeys when SDL itself was compiled with `SDL_MAC_NO_SANDBOX`,
which no released SDL package is. The default bindings are what keeps the system shortcuts out of the way
there. The grab is always released when the window loses the focus, when the game is frozen and on exit.

**Windows: a ring around the pointer every time you fire.** That is Windows' own accessibility setting, not
the game: Control Panel > Mouse > Pointer Options > **"Show location of pointer when I press the CTRL key"**.
Turn it off there, or fire with **Z** or **X** instead, which are bound as well. Z and X are listed first in
the defaults for exactly this reason; Ctrl stays bound because that is the key the Amiga used.

The mouse cursor is hidden while you are playing and comes back when the game is frozen or the window loses
the focus — the game has no mouse input at all. `--cursor show` or `--cursor hide` (or `cursor =` in
`settings.cfg`) overrides that. The pointer is never confined to the window or captured.

Gamepads are supported and hot-pluggable. Key bindings can be changed in `input.cfg` in the per-user data
folder (`docs/BUILDING.md`). Upgrading keeps a file you have edited exactly as it is; a file that still holds
nothing but older defaults is replaced with the current ones and kept as `input.cfg.bak`.

## Assists and cheats

All three are **off by default** and never touch a headless run, so the replay and golden-frame tests cannot
see them. They are in `settings.cfg`, on the command line, and on the hotkeys above.

| | What it does |
|---|---|
| **Autofire** (`--autofire[=RATE]`, `autofire = on`) | While fire is held, the fire line is pulsed instead of being held down. An **assist, not a cheat**: high scores are still saved. It only engages during a level, and only after fire has been held longer than a tap, so the title screen, the SELECT menu, the shop and the continue prompt — which all read a fire *press* as a selection — behave exactly as before. |
| **God mode** (`--god-mode`, `god_mode = on`) | **CHEAT.** The ship's energy is refilled every frame, so damage never kills it. |
| **Credits** (`--credits N`, `credits = N`) | The number of credits a new game starts with, 1 to 9. The game's own value is 3 and its display has a single digit, so 9 is the most it can show. Above 3 is a **cheat**; 3 or fewer is not. |

Autofire is worth having: the game's weapon only fires again on a fresh press, so **holding** fire gives about
5 shots in 400 frames while tapping gives about 60. The default rate, 17 pulses per second (one every third
emulated frame), is the fastest the game itself can register — asking for more does nothing.

Cheats are flagged so you always know: the window title gains `[CHEAT: god mode]` or `[CHEAT used]`, a line is
printed when you toggle one, and **a session that used a cheat does not save high scores**. The table still
shows your score while you play; it just does not outlive the run. `cheat_scores = on` overrides that.

God mode covers **both** ways the ship can die. Damage is the usual one. The other costs no energy at all:
when the ship is pushed back against the scenery the game walks a counter down for about 13 logic steps and
then kills it with the energy bar still full — that is what happens at the end of level 1's boss run, and it
is why "infinite energy" alone was not enough. Both are caught at the game's own death routine, so nothing
else about a hit changes: you still see the flash, hear it, and lose energy normally.

Autofire fires many more sound effects than holding the button, and the game's effects borrow two of the four
sound voices from the music while they play — so with autofire on you may notice the tune thinning out more
often. That is the original game's own behaviour, not a bug here; `docs/re-notes/music-dropouts.md` has the
measurements.

If a key seems to need a second press, run `x2recomp --input-debug`: it logs every key, focus change and
joystick change with a timestamp and the emulated frame that carried it.

## What is implemented

These parts are complete:

- the machine model;
- the scanline renderer;
- Paula audio;
- the 50 Hz pacing;
- keyboard and gamepad input;
- the native disk-image import.

The interpreter build plays the game, and the recompiled build is bit-identical to it. What has and has not
been checked is in "Status and known gaps" below.

The optional modernisation features are done (`docs/BUILDING.md`, "Picture"): filters, the Faithful switch,
interpolated high-frame-rate output, replacement asset packs (`docs/ASSET-PACKS.md`; no pack is shipped), a
widescreen surround of scenery and an edge-upscaled status panel. All are off by default.

Optional CDTV CD music (`docs/BUILDING.md`, "CDTV CD music"): with your own CDTV disc image
(`x2recomp --import <CDTV zip>`, about 680 MB of PCM in the per-user data folder, or `--cdtv FILE.cue` to play the
`.bin` files in place) `--music cdtv` plays the CDTV release's Bomb the Bass tracks for the title and each level
and mutes the tracker music while keeping the sound effects; `--music mix` plays both. Off by default; the disc
audio is never copied into the repository or shipped.

Cross-platform builds, CI and packages (Phase 7): see `CMakePresets.json`, `.github/workflows/ci.yml`,
`docs/PORTING.md` (platform status and Windows risks) and `docs/RELEASING.md` (version, macOS app, Linux tarball,
Windows zip). Release packages contain no game data.

## Status and known gaps

An honest account of what has been checked and what has not.

**Verified, and how:**

- **60/60 tests** pass in the default interpreter build and **80/80** with `-DX2_RECOMP=ON`
  (`ctest --test-dir build`). The extra 20 are the recompiler difftest and the replay-equivalence runs.
- **The recompiled and interpreted builds agree bit for bit** on every replay: the equivalence tests compare
  frame and audio hashes between the two executors, not against a stored expectation.
- **Replay coverage**: the title, all five levels, the shop, two-player mode, game over and the ending, each
  checked against recorded frame and audio hashes (`tests/replays/`).
- **Compared against a reference emulator**: pictures, palette, logic rate, stereo balance and game state
  were compared against Amiberry (a WinUAE port) running the original disks, from the title to 1,400 frames
  into level 1. Pictures match except for 0.06-0.11% of pixels, all in the twinkling starfield
  (`docs/re-notes/reference-compare.md`). Per-instruction and per-blit timing was calibrated the same way
  (`docs/re-notes/timing.md`).
- **Builds and passes its tests on every supported platform in CI.** Run `41041824614` (commit `95d9d78`)
  passed all eight jobs: repository hygiene, macOS arm64, macOS universal, macOS x86_64, Linux GCC, Linux
  Clang, Windows MSVC and Windows MinGW-w64. **CI runs without game data**, so what this proves is that the
  code compiles and that every test not needing the game passes; the data-dependent tests report Skipped.
- **Run with the real game on**: macOS arm64, macOS x86_64 (under Rosetta 2), macOS universal, and Linux
  arm64 with GCC 13 and Clang 18 in a container. `docs/PORTING.md` has the full matrix and explains the
  difference between the two kinds of evidence.

**Not verified:**

- **No comparison against real Amiga hardware.** Every timing claim is relative to a reference emulator, not
  to an A500.
- **Nothing has been seen or heard on Windows**, though the game itself now runs there: a Windows 11 machine
  passes the whole suite with real game data, interpreter and recompiled, with every frame and audio hash
  matching the macOS goldens (`docs/WINDOWS-VERIFICATION.md`). What has not happened is a window on a Windows
  display or a sample through a Windows sound card — every run was headless or used SDL's dummy drivers.
- **The same is true of x86_64 Linux**: CI builds and tests it without data, but the game has only been run
  on arm64 Linux.
- **Nobody has played this by hand for any length of time.** The replays are scripted input, and most of them
  write coverage-only values into chip RAM to reach later stages within two minutes, so they are not
  unassisted play-throughs (`tests/replays/README.md`). **Only the level-1 boss has been defeated**; the
  later bosses are reached by replay but not beaten in normal play.
- **Disc loads are instant**, where a real drive takes hundreds of frames. The game keeps calling its random
  generator while it waits, so after every level load the generator is thousands of calls behind where it
  would be on real hardware, and every random outcome after that point differs. This is a known, unfixed
  divergence. It also bounds the reference comparison: past the first disc load, a run can only be compared
  with the generator re-seeded to the reference's state (`tools/compare/resync_rng.py`, comparison only —
  the committed replays do not do this).
- **Drag-and-drop import is untested.** The first-run window offers it and the code path exists, but only
  `--import` on the command line has actually been exercised.
- **The HD status panel is an upscale, not a redraw**: it runs the original panel through the edge filter.
- **Widescreen is cosmetic.** The surround extends the scenery to the sides; the playfield, the collision
  geometry and everything the game can see are unchanged. It is not real widescreen.
- **Linux and Windows release packages have not been built on this machine** — only the macOS arm64 and
  universal archives in `dist/`. `tools/package.sh` produces the Linux tarball when run on Linux.

**Open decisions:**

- Nothing outstanding. The licence is settled (GPL-3.0-or-later, see above), and the one loose end that
  remained — whether SoftFloat's indemnification wording sits comfortably under the GPL — was closed by
  removing SoftFloat from the build entirely (`docs/PROVENANCE.md` §7.3).

Reverse-engineering notes are in `docs/re-notes/` (see `docs/re-notes/README.md` for an index). The version
history is in `CHANGELOG.md`, and the notes for this version in `docs/RELEASE-NOTES-0.1.0.md`.
