# x2recomp 0.3.0 — release notes

A native port of the Amiga game **Xenon 2 Megablast** (The Bitmap Brothers, 1989), built by static
recompilation. The game's own 68000 code runs on a model of a PAL Amiga 500 — copper, blitter, Paula audio,
CIAs and a scanline display — either on the Musashi interpreter or as 68000 code translated to C++.

0.2.0 read the preservation formats and most of the known disk releases. **0.3.0 adds the CDTV release of the game as a
second game:** it imports from your own CDTV disc image, starts and plays on the interpreter and on the translated
executor, plays its CD audio and speech, and takes the modernisation options. The disk releases run exactly as before:
the canonical image and every existing replay golden are unchanged. Read "What is not verified" before you rely on it.

## Downloads

| Platform | File | sha256 |
|---|---|---|
| macOS (Apple Silicon and Intel) | `x2recomp-0.3.0-macos-universal.zip` | `TBD` |
| macOS (Apple Silicon and Intel) | `x2recomp-0.3.0-macos-universal.dmg` | `TBD` |
| Windows x64 | `x2recomp-0.3.0-windows-x64.zip` | `TBD` |
| Linux aarch64 (64-bit Raspberry Pi OS) | `x2recomp-0.3.0-linux-aarch64.tar.gz` | `TBD` |
| Linux x86_64 | `x2recomp-0.3.0-linux-x86_64.tar.gz` | `TBD` |

None of the binaries is code-signed (macOS has an ad-hoc signature only); see "Install and quick start".

## Legal position

- **This release contains no game data.** No disk images, no disc contents, no game code, no graphics, no music, no speech,
  no text, and no code translated from the game.
- **Bring your own copy.** You need your own copy of the game as Amiga disk images or dumps, or as a CDTV disc image.
  `x2recomp --import` reads them on your machine and writes what the runtime needs to your per-user data directory.
  Nothing leaves your machine.
- **No CDTV firmware is needed or shipped.** The CDTV release runs without the CDTV ROMs: the runtime answers the few
  operating-system services the program asks for itself. The CD audio and speech are played from your own disc image.
- **The importer is not a cracking tool.** For the copy-protected retail disk it reads the key track from *your* dump and the
  runtime answers the game's own check; no key value is in the program and the image is not modified. The separate opt-in
  patch (unchanged since 0.2.0) is described in `README.md` and `docs/BUILDING.md`.
- **x2recomp is free software under the GNU GPL, version 3 or later** (GPL-3.0-or-later). The full text is in `LICENSE`,
  included in the package. It is GPL because part of the accurate-timing code is derived from WinUAE/Amiberry
  (`PROVENANCE.md`). Third-party components keep their own GPL-compatible licences (`THIRD-PARTY-NOTICES.md`, `licenses/`).

Xenon 2 Megablast itself is copyright The Bitmap Brothers / Mirrorsoft and is covered by no licence here; the music is
credited to Bomb the Bass and others and is licensed separately from the game. The GPL gives you the right to the complete
source for what is in the package: it is the git repository this release was built from, recorded in the release tag. A
`-DX2_RECOMP=ON` build contains C++ translated from your own game image (either release) and is **not** redistributable.

## What is new: the CDTV release

- **Import.** `x2recomp --import <CDTV disc image>` takes a `.cue` with its `.bin` files, a ZIP or folder holding them, or the
  bare data track as `.bin`/`.iso`. It reads the disc's ISO 9660 volume, links the game program `X2` at fixed addresses and
  stores it, with the 95 files the game loads from the disc, in `assets-cdtv` in your data folder (the disk releases stay in
  `assets`). The same command imports the CD audio tracks into the data folder (about 680 MB of PCM). `--diagnose-image`
  and `--list-variants` know the disc.
- **Play.** `x2recomp --game cdtv` (or plain `x2recomp` when it is the only game imported; with both imported, `game =
  auto|disk|cdtv` in `settings.cfg` chooses, and `auto` starts the disk release and says so). The
  front end, all five levels, bosses, the shops, two players, game over and the ending run. The window title names the
  release.
- **CD audio and speech.** The CD is the music, as in the original; the program asks the CD for a track and polls it once a
  second (every 51 ticks), so a finished track restarts up to about a second after its end (the program's own poll; the original
  drive's seek time is not modelled).
  Speech and effects use the game's own sample player. Which track plays where is in `docs/re-notes/cdtv-audio.md`. Without
  imported CD tracks the game runs and says once that the music is silent.
- **Controls.** The CDTV joypad's second button fires the special weapon (`button2`, default `C`, or a gamepad shoulder
  button). **`input.cfg` is now format 5** (version 4 files are upgraded and kept as `input.cfg.bak`). The CDTV remote control
  is not used: the game never reads it.
- **Scores.** The CDTV release saves its high-score table (the original keeps none), in its own file.
- **Recompiled executor and modernisation.** `-DX2_RECOMP=ON` translates the CDTV game too, bit-identical to the interpreter.
  Interpolation, widescreen, the HD status panel, filters, asset packs and the Faithful switch work on it.
- Under the hood: a selectable 1 MB chip RAM model (the CDTV program needs about 800 KB; the disk builds keep their 512 KB
  behaviour exactly).

## What you can import

`x2recomp --list-variants` prints this table with the hashes; `x2recomp --diagnose-image FILE...` tells you what the importer
makes of a particular image.

| Release | Give the importer | Result |
|---|---|---|
| BS1 `[t+3]` two-disc set (the canonical image), its `[a]` and Deejay99 variants, the `[h Timecircle]` set, the "Xenon 2 & 4 Others" compilation | ADF, ADZ, DMS, IPF, flux, WHDLoad, zip | the canonical image: all replay goldens apply |
| **Retail two-disc release**, Image Works, both pressings (SPS 0297, SPS 2874 `[a]`) | disk 1 as an **IPF or flux dump** (SCP, KryoFlux stream folder, HFE) so the Copylock key track is kept; disk 2 in any format | imports; not canonical; own goldens |
| the same retail disk 1 from a **plain ADF, DMS or WHDLoad `Disk.1`** (no key track) | add `--apply-protection-patch --i-confirm-i-own-the-original` | refused with the options unless you choose the opt-in patch |
| Power Pack compilation (SPS 2234), Bombuzal + Xenon 2 compilation (SPS 1762) | IPF, flux or WHDLoad | import as their own builds (`build=power-pack-2234`, `build=compilation-1762`) |
| cracks of the retail disk 1 (PNA, Quartex, Flashtro +16), PNA `[a]`, Black Monks, the one-disk crunches (Vision Factory, VF `[a]`, Black Monks `[h][t +36]`) | ADF (+ the standard disc 2 where needed) | import as their own variants; trainer builds are flagged |
| Predators +9, Flashtro +16 `[a]`, damaged disk 2 copies | | **refused** with the reason |
| **The CDTV release** (EN/FR/DE/IT) | your CDTV disc image | **imports and plays** (`build=cdtv`): see above |
| PDos one-disk release (WHDLoad imager "v5") | | recognised by its boot block in a WHDLoad install; **not importable** yet (a real flux dump of it was analysed: `docs/re-notes/acquisitions-flux-acorn-st.md`) |
| the playable demo disks, the L.G.C.S.S. compilation, the WHDLoad demo and CDTV installs | | **recognised and reported**, not imported: different programs |

Formats read: ADF, ADZ, ZIP (nested), DMS, extended ADF, IPF, SCP, KryoFlux stream files, HFE, LHA archives, WHDLoad
installs, CDTV disc images (ISO 9660 data track), and folders of these. Exit codes of `--import`: 0 imported, 1 could not
write, 2 bad input, 3 unsupported image (including a protected retail disk 1 without its key track), 4 confirmation
required. Making your own dump: `docs/DUMPING.md` (disks); a CDTV disc is dumped to a Redump-style BIN/CUE set with any CD
ripper that keeps the data track and the audio tracks as separate files.

## Install and quick start

Unpack the archive for your platform, then import your disks or your disc. The first-run window also accepts them
dropped onto it.

**macOS** — the archive holds `x2recomp.app` with SDL3 bundled. The app is ad-hoc signed, not notarised, so Gatekeeper
will object on first launch: right-click the app and choose Open, or clear the quarantine flag with
`xattr -dr com.apple.quarantine x2recomp.app`.

```
./x2recomp.app/Contents/MacOS/x2recomp --import Xenon2-CDTV.zip     # your CDTV disc image; or disk images, dumps, a folder
open x2recomp.app
```

**Windows** — unzip anywhere and run `x2recomp.exe` from the folder (SDL3 is beside it, and the C runtime is linked in, so
nothing has to be installed). The exe is unsigned, so SmartScreen may warn on first run: choose "More info", then "Run anyway".

```
x2recomp.exe --import Xenon2-CDTV.zip
x2recomp.exe --game cdtv
```

**Linux** — `bin/x2recomp` finds `lib/libSDL3.so.0` beside it through its `RUNPATH`; nothing has to be installed. Take
`linux-aarch64` for 64-bit ARM (a Raspberry Pi 3, 4, 5 or 400 on **64-bit** Raspberry Pi OS) and `linux-x86_64` for a PC;
`uname -m` says which. Minimum system: glibc 2.35 (Debian 12, Ubuntu 22.04 and newer). 32-bit Raspberry Pi OS (`armv7l`) is
not supported by either archive.

```
tar xzf x2recomp-0.3.0-linux-aarch64.tar.gz && cd x2recomp-0.3.0-linux-aarch64
./bin/x2recomp --import ~/Xenon2-CDTV.zip
./bin/x2recomp --game cdtv
```

Controls, assists and every option are in `README.md` and `BUILDING.md`. Imports made by earlier versions are redone
automatically from their recorded sources the first time you start this version.

## What is verified, and how

- **192 tests** in the default interpreter build. Run without game data (as on every CI platform: macOS arm64, macOS
  universal, Linux GCC and Clang, Windows MSVC are required checks; macOS x86_64 and Windows MinGW-w64 also run) all
  192 pass, 24 of them executed and 168 reported as Skipped because they need real images. WITHDATA
- **CDTV replay corpus.** 23 scripted replays (front end, five levels, floods, shops, bosses 1-3 and level 4, two players,
  game over, ending reached naturally) with recorded frame and audio hashes; each matches a fresh run. On the translated
  executor all 23 give identical frame hashes, audio hashes, register logs and final chip RAM to the interpreter (100.00% of
  1.23 billion executed instructions ran translated; a differential test of 563 translated functions from random states
  found no difference). The recompiled suite (236 tests with the images attached, two long differential tests excluded)
  was run during development on the CDTV phases' final head.
- **Independent reference.** The CDTV game was run under the WHDLoad CDTV slave on an A500+ in Amiberry (and from the
  disc in Amiberry with a user-supplied CDTV ROM). Against that reference, the front end and level 1 match at a median
  99.89% of pixels inside the 320x256 picture (the HUD is identical in every frame; the enemy positions agree, so the
  random sequence does; the differences are the twinkling starfield); the game variables are equal at matched moments;
  chip RAM above 512 KB differs by 0.1%, direct evidence for the 1 MB model.
- **CD audio and speech.** The observed state-to-track table comes from running all 23 replays; the program's own poll
  routine runs unmodified; speech plays whole and mono at 11,902 Hz and correlates 0.99 with its file; CD audio goldens
  cover four replays and the no-tracks fallback.
- **Modernisation on the CDTV release.** Interpolation endpoints are exact on all 21 gameplay replays; the centre of every
  widescreen picture is the rendered frame at 16:10, 16:9, 21:9 and 32:9; the asset dump reproduces the pictures; the
  pictures with a pack, the surround and the HD panel are identical on the translated executor.
- **Disk releases and importer.** Everything from 0.2.0 is unchanged and its goldens identical (`RELEASE-NOTES-0.2.0.md`).
- **Hand play: [to be confirmed by the maintainer before release]**

## What is not verified

- **The CDTV release has not been played by hand** (see the line above). The evidence is scripted replays, the reference
  comparison and the importer tests.
- **Bosses and later content.** The level-2 and level-3 bosses and level 4's eyes and head are reached but not defeated in
  the replays (the disk build's corpus has the same limit on levels 2-3); the CDTV `INTRO` program's code is not executed by
  any replay, `PICCODE` was never loaded in one, and the music-select and information pages (CD tracks 8-13) are read from the
  code only. Code no replay reached runs on the interpreter in the recompiled build: slower, never a different result.
- **The reference covers the front end and level 1 only.** The picture of later levels, the speech and the audio mix have no
  independent reference. The CD's level against effects and speech is a choice (0.75, the same headroom as the Amiga
  audio, adjustable with `--music-volume` and `--sfx-volume`); the original's analogue mix is unknown. The gap before a
  looping track restarts follows the program's own 51-tick poll; the original drive's seek latency is not modelled and no
  evidence for it was found. `MUSIC OFF` in the menu does not stop anything, as the code reads; this was not compared
  with the reference.
- **Windows and Linux** ran the CDTV tests only in CI without the disc; the CDTV import and play have been run on macOS
  arm64 only (the recompiled translation likewise). The Windows executable was not run on a clean machine without Visual
  Studio; no Raspberry Pi has run it (speed unmeasured). Gamepads and drag-and-drop import are untested.
- **No dump of the maintainer's own retail disks has been imported yet** (as in 0.2.0): the retail import is verified
  against preservation images, and the flux readers against public dumps; a real flux dump of the retail disk 1 of this
  game was not available. A real Amiga flux dump of the PDos release read 1,901 of 1,908 sectors directly; it cannot be
  imported yet.
- **The crack variants and the 1762 build** are as described in `RELEASE-NOTES-0.2.0.md`; budget (floppy version 4) has no image.
- **HFE v3 support** is best effort and untested; flux decoding was validated on synthetic flux and public dumps.
- **No comparison against real Amiga or CDTV hardware.** Every accuracy claim is relative to an emulator.
- No unassisted playthrough has been done; disc loads are instant (the game's random generator drifts after each load);
  widescreen is cosmetic. All as in 0.1.0 (`README.md`, "Status and known gaps").

## Checksums

The sha256 of each download is in the table above. Each archive also contains a `SHA256SUMS.txt` listing every file
inside it. Verify both:

```
shasum -a 256 x2recomp-0.3.0-macos-universal.zip            # macOS: compare with the table
cd x2recomp-0.3.0-macos-universal && shasum -a 256 -c SHA256SUMS.txt

sha256sum x2recomp-0.3.0-linux-aarch64.tar.gz               # Linux: compare with the table
cd x2recomp-0.3.0-linux-aarch64 && sha256sum -c SHA256SUMS.txt
```

The per-file manifest is generated at packaging time from the staged contents, so it also confirms that no game data,
imported assets or translated code is present — `tools/package.sh` and `tools/package.ps1` refuse to package a
`-DX2_RECOMP=ON` build and check the staged files before archiving.
