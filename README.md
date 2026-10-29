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
in the optional recompiled build (step 3 below). That build writes C++ generated from your game image into the
build folder; it is for your own use, so do not commit or share it.

The project's own licence has not been chosen yet. Vendored code keeps its own licence:

- `vendor/musashi`: MIT-style;
- `vendor/puff`: zlib licence.

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

You need CMake 3.20 or newer, a C++20 compiler and SDL3 (for example Homebrew `sdl3`). Without SDL3 only the
headless runtime is built.

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

| Key | Action |
|---|---|
| arrows, and Ctrl or Z to fire | joystick |
| P / Esc / Space | the game's pause, back-to-menu and space keys |
| F9 or Pause | freeze |
| F12 | reset |
| F11 or Alt+Enter | fullscreen |
| F10 or Cmd/Ctrl+Q | quit |

Gamepads are supported and hot-pluggable. Key bindings can be changed in `input.cfg` in the per-user data
folder (`docs/BUILDING.md`).

## Status

These parts are complete:

- the machine model;
- the scanline renderer;
- Paula audio;
- the 50 Hz pacing;
- keyboard and gamepad input;
- the native disk-image import.

The interpreter build plays the game. Regression replays cover the title, all five levels, the shop,
two-player mode, game over and the ending, and are checked against recorded frame and audio hashes. To reach
the later stages within 2 minutes, most replays write coverage-only values into chip RAM, so they are not
unassisted play-throughs (`tests/replays/README.md`). The recompiled build runs the same replays and is
bit-identical to the interpreter.

Still open:

- comparison against a real A500 or a WinUAE capture;
- CDTV CD audio;
- the optional modernisation features (higher frame rates, widescreen);
- packaging and CI for Windows and Linux.

Reverse-engineering notes are in `docs/re-notes/`.
