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
- **x2recomp is free software under the GNU GPL, version 3 or later** (GPL-3.0-or-later). The full text is in
  `LICENSE`, included in the package. It is GPL rather than permissive because part of the accurate-timing
  code is derived from WinUAE/Amiberry, which is GPL; `PROVENANCE.md` (also in the package) records exactly
  which code that is and the compatibility reasoning. Third-party components keep their own licences, all
  GPL-compatible, listed in `THIRD-PARTY-NOTICES.md` and reproduced in full in `licenses/`. SoftFloat, which
  Musashi can use for its FPU, is deliberately not built: this game is a 68000 with no FPU, so none of it is
  compiled, linked or shipped. Musashi itself is vendored with three small local changes (two fixing an
  MSVC miscompilation of its opcode-table generator, one a macOS/BSD `setjmp` optimisation); they are marked in its source, its MIT-style licence permits
  modification, and its notice is retained (`PROVENANCE.md` §9).

Xenon 2 Megablast itself is copyright The Bitmap Brothers / Mirrorsoft and is covered by no licence here.

The GPL gives you the right to the complete source for what is in this package. It is the git repository
this release was built from; the commit is recorded in the release tag. If you redistribute a modified
binary, pass the source on with it. Note that a `-DX2_RECOMP=ON` build is **not** redistributable: it
contains C++ translated from your own game image, which is the game's code, not this project's.

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

**Linux** — the tarball holds `bin/x2recomp` with `lib/libSDL3.so.0` beside it, found through the executable's
`RUNPATH`. Nothing has to be installed first.

```
sha256sum -c x2recomp-0.1.0-linux-aarch64.tar.gz.sha256
tar xzf x2recomp-0.1.0-linux-aarch64.tar.gz && cd x2recomp-0.1.0-linux-aarch64
./bin/x2recomp --import ~/Xenon2.zip
./bin/x2recomp
```

Take `linux-aarch64` for 64-bit ARM (a **Raspberry Pi 4, 5 or 400 on 64-bit Raspberry Pi OS**, or any other
arm64 Linux) and `linux-x86_64` for a PC; `uname -m` says which. **Minimum system: glibc 2.35**, so Debian 12
"bookworm", Ubuntu 22.04 and anything newer. **32-bit Raspberry Pi OS (`armv7l`) is not supported by either
archive** — use the 64-bit OS image on the same Pi, or build from source.

A desktop session needs no options. Force a backend with `SDL_VIDEO_DRIVER=wayland|x11|kmsdrm` and
`SDL_AUDIO_DRIVER=pipewire|pulseaudio|alsa` (SDL3 loads them at run time, which is why one archive fits every
desktop). On weak hardware start with `--scale 2 --filter nearest`, leave `--modern interp` off, and raise
`--audio-latency 80` if the sound breaks up. `docs/PORTING.md` has a Raspberry Pi test recipe.

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
- **Builds and passes its tests on every supported platform in CI** — macOS arm64, macOS universal, macOS
  x86_64, Linux GCC, Linux Clang, Windows MSVC and Windows MinGW-w64, plus the repository hygiene check: all
  eight jobs passed in run `41041824614`. CI runs **without game data**, so this proves the code compiles and
  that every test not needing the game passes.
- **Run with the real game on** macOS arm64, macOS x86_64 (under Rosetta 2), macOS universal, Windows x64
  (MSVC and MinGW-w64), and Linux arm64 with GCC 13 and Clang 18 in a container. The Windows run is the whole suite
  with game data and every hash matches the macOS goldens (`docs/WINDOWS-VERIFICATION.md`).
- **The Linux archive itself was run, not just built.** The packaged `bin/x2recomp` starts in a clean
  `debian:bookworm-slim` container with no packages installed — it needs only libc, libm and the SDL3 beside it
  — and reproduces the `level1_play` (24 hashes), `boss1` (24) and `ending_natural` (20) replays exactly from
  the user's own game data. Its glibc floor is 2.35 and it carries no libstdc++ requirement at all.
- **Played by hand.** On macOS the maintainer played the whole game from the first level to the end of the last
  with the autofire and god-mode assists on. After the final shop the game starts again from level 1, which is how
  the original ends. On Windows the maintainer played it in a window with sound, including the smooth
  high-frame-rate modes, and reports that it matches the macOS experience; that is a hands-on impression, not a
  measurement. Playing found and fixed the macOS keyboard problems (`CHANGELOG.md`).
- **Music that goes quiet while firing is the original's behaviour** (Amiberry and FS-UAE both do the same). The
  opt-in `--protect-music` keeps the tune audible anyway (`docs/re-notes/music-dropouts.md`).

## What is not verified

- **No comparison against real Amiga hardware.** Every accuracy claim is relative to an emulator.
- **Linux has never been played.** CI builds and tests it without data and the game has been run headless with
  real data in a container on arm64, but nobody has seen a window or heard sound on any Linux machine, and
  x86_64 Linux has not run the game at all.
- **No unassisted playthrough has been done.** The full play-through used autofire and god mode, so it shows the
  game can be completed, not that it is beatable without assists. The scripted replays write coverage-only
  values into chip RAM to reach later stages quickly, and in them only the level-1 boss is beaten.
- **Drag-and-drop import is untested** — only `--import` on the command line has been exercised. Gamepads are
  untested on every platform.
- **Disc loads are instant**, where a real drive takes hundreds of frames. The game keeps calling its random
  generator while it waits, so after each level load the generator is thousands of calls behind real
  hardware and every random outcome then differs. Known and unfixed.
- **The HD status panel is an edge upscale**, not a redraw.
- **Widescreen is cosmetic**: it extends the scenery sideways. The playfield, collision geometry and
  everything the game can see are unchanged.
- **No Raspberry Pi, or any other Linux machine, has run a release by hand.** The Linux archive is verified as
  far as a container can: the packaged binary starts in a clean Debian 12 container with nothing installed and
  reproduces three replays' hashes from real game data. Its speed on Pi hardware is unmeasured, and no window
  or sound has been seen on Linux at all (`docs/PORTING.md`, "Raspberry Pi").
- **The Windows zip has not been built** for this version; `tools/package.ps1` produces it on Windows.

## Checksums

Each archive ships with a `.sha256` file next to it, and contains a `SHA256SUMS.txt` listing every file
inside it. Verify both:

```
shasum -a 256 -c x2recomp-0.1.0-macos-arm64.zip.sha256      # macOS
cd x2recomp-0.1.0-macos-arm64 && shasum -a 256 -c SHA256SUMS.txt

sha256sum -c x2recomp-0.1.0-linux-aarch64.tar.gz.sha256     # Linux
cd x2recomp-0.1.0-linux-aarch64 && sha256sum -c SHA256SUMS.txt
```

The per-file manifest is generated at packaging time from the staged contents, so it also confirms that no
game data, imported assets or translated code is present — `tools/package.sh` refuses to package a
`-DX2_RECOMP=ON` build and checks the staged files before archiving.
