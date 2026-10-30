# x2recomp 0.1.0 — release notes

A native port of the Amiga game **Xenon 2 Megablast** (The Bitmap Brothers, 1989), built by static
recompilation. The game's own 68000 code runs on a model of a PAL Amiga 500 — copper, blitter, Paula audio,
CIAs and a scanline display — either on the Musashi interpreter or as 68000 code translated to C++.

This is the first collected version. It is a working port, not a finished product: read "What is not verified"
before you rely on it.

## Legal position

- **This release contains no game data.** No disk images, no game code, no graphics, no music, no text, and
  no code translated from the game.
- **Bring your own disks.** You need your own copy of the game as Amiga disk images. `x2recomp --import`
  reads them on your machine and writes what the runtime needs to your per-user data directory. Nothing
  leaves your machine.
- **CDTV music is user-supplied.** The optional CD-audio layer plays tracks from your own CDTV disc image.
  No disc audio is shipped.
- **The project's licence has not been chosen yet.** Packages carry `licenses/LICENSE-TODO.txt`, which grants
  no licence to the project's own code. Third-party components keep their own licences, listed in
  `THIRD-PARTY-NOTICES.md` and reproduced in full in `licenses/`. `docs/PROVENANCE.md` in the repository
  records which code was written from external sources — some accurate-timing code is derived from GPL
  sources, which is why the licence is still open.

Xenon 2 Megablast itself is copyright The Bitmap Brothers / Mirrorsoft and is covered by no licence here.

## Which game release is supported

Only the **two-disc BS1 "[t+3]" release**, for example a `Xenon2.zip` holding
`Xenon II - Megablast (1of2)[t+3].adf` and `(2of2)[t+3].adf`, or the same disks in a TOSEC-style set. The
importer identifies disks by content, not by file name, and also accepts:

- the disk 1 `[a]` variant with an alternate boot block;
- the Deejay99 UAE-fix pack (its disk 2 differs by 26 bytes, so the importer warns).

The single-disk cracks (PNA, Quartex, Black Monks, Vision Factory) are different builds and are recognised and
rejected. DMS, IPF and extended (`UAE-1ADF`) images must be converted to plain 880 KB ADFs first.

The supported release is cracked and trained, but the importer never runs the trainer loader: the game image
is used exactly as packed, so there is no trainer and no infinite lives.

## Install and quick start

Unpack the archive for your platform, then import your disks.

**macOS** — the archive holds `x2recomp.app` with SDL3 bundled. The app is ad-hoc signed, not notarised, so
Gatekeeper will object on first launch: right-click the app and choose Open, or clear the quarantine flag with
`xattr -dr com.apple.quarantine x2recomp.app`.

```
./x2recomp.app/Contents/MacOS/x2recomp --import ~/Xenon2.zip
open x2recomp.app
```

**Linux** — the tarball holds `bin/x2recomp` and, when SDL3 was bundled, `lib/libSDL3.so.0`, found through the
executable's `RUNPATH`. If SDL3 is not in the archive you need your distribution's SDL3 package.

```
./bin/x2recomp --import ~/Xenon2.zip
./bin/x2recomp
```

You can also skip the import step and just start `x2recomp`: the first-run window asks you to drag your disk
images, a zip, or a folder onto it. On the command line, `--import` exits 0 on success, 2 for bad input and 3
for an unsupported image. `--print-data-dir` prints where imports and settings go.

Default controls: arrow keys and Ctrl or Z to fire; P, Esc and Space are the game's own keys; F11 or Alt+Enter
for fullscreen, F10 or Cmd/Ctrl+Q to quit, F12 to reset. Gamepads work and are hot-pluggable. The full list is
in `README.md`, and bindings can be changed in `input.cfg` in the data directory.

Everything optional is off by default. `BUILDING.md` in the archive documents every flag.

## What is verified, and how

- **60/60 tests** pass in the interpreter build and **80/80** with `-DX2_RECOMP=ON`.
- **The interpreted and recompiled executors agree bit for bit** across the whole replay corpus — compared
  against each other, not against a stored expectation.
- **Replays cover** the title, all five levels, the shop, two-player mode, game over and the ending, each
  checked against recorded frame and audio hashes.
- **Compared against a reference emulator** (Amiberry, running the original disks): pictures match except for
  0.06-0.11% of pixels, all in the twinkling starfield; palette identical; the logic rate, stereo balance and
  game state agree. Audio envelopes correlate at 0.98-0.999 per 100-frame window.
- **Built and tested on** macOS arm64, macOS x86_64 (under Rosetta 2), and Linux arm64 with GCC 13 and
  Clang 18.

## What is not verified

- **No comparison against real Amiga hardware.** Every accuracy claim is relative to an emulator.
- **Windows has never been built or run.** The presets and CI jobs exist but are untested.
- **CI has never run.**
- **Nobody has played this by hand for any length of time.** The replays are scripted, and most write
  coverage-only values into chip RAM to reach later stages quickly, so they are not unassisted
  play-throughs. **Only the level-1 boss has been defeated** in normal play.
- **Drag-and-drop import is untested** — only `--import` on the command line has been exercised.
- **Disc loads are instant**, where a real drive takes hundreds of frames. The game keeps calling its random
  generator while it waits, so after each level load the generator is thousands of calls behind real
  hardware and every random outcome then differs. Known and unfixed.
- **The HD status panel is an edge upscale**, not a redraw.
- **Widescreen is cosmetic**: it extends the scenery sideways. The playfield, collision geometry and
  everything the game can see are unchanged.
- **Only the macOS packages have been built** for this version. The Linux tarball and Windows zip are
  produced by `tools/package.sh` and `tools/package.ps1` on those platforms.

## Checksums

Each archive ships with a `.sha256` file next to it, and contains a `SHA256SUMS.txt` listing every file
inside it. Verify both:

```
shasum -a 256 -c x2recomp-0.1.0-macos-arm64.zip.sha256      # or sha256sum -c
cd x2recomp-0.1.0-macos-arm64 && shasum -a 256 -c SHA256SUMS.txt
```

The per-file manifest is generated at packaging time from the staged contents, so it also confirms that no
game data, imported assets or translated code is present — `tools/package.sh` refuses to package a
`-DX2_RECOMP=ON` build and checks the staged files before archiving.
