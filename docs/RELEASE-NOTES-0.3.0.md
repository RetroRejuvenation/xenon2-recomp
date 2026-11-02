# x2recomp 0.3.0 — release notes (DRAFT)

> **Draft.** 0.3.0 is not released. It waits for the maintainer's two hand runs: a hand-played session of the CDTV release (the "Hand play" line
> below) and a hand run of the launcher and the in-game menu on a real machine (the "Hand run" line). Until then no tag exists, the checksums are
> `TBD`, and every line marked "to be confirmed" has to be checked by the maintainer. When the release is made, this file loses this paragraph and
> the "to be confirmed" markers, the numbers are filled from the tagged commit's runs, and `CHANGELOG.md` gets its date.
>
> **Which code this describes.** `main` after the 2026 security fixes: the CDTV release, the launcher and the in-game menu, asset packs version 2,
> the importer additions and the security fixes are all in this one release. The test counts below are from earlier runs and **must be re-measured on
> the tagged commit.**


A native port of the Amiga game **Xenon 2 Megablast** (The Bitmap Brothers, 1989), built by static
recompilation. The game's own 68000 code runs on a model of a PAL Amiga 500 — copper, blitter, Paula audio,
CIAs and a scanline display — either on the Musashi interpreter or as 68000 code translated to C++.

0.2.0 read the preservation formats and most of the known disk releases. **0.3.0 adds the CDTV release of the game as a
second game:** it imports from your own CDTV disc image, starts and plays on the interpreter and on the translated
executor, plays its CD audio and speech, and takes the modernisation options. **It is also the release of the window you can use instead of
remembering options:** the launcher opens before a game starts and the in-game menu opens over it, both made of the same pages, and each page
shows the command line that does the same thing. The command line is unchanged and does everything the window does. Asset packs grow to version 2
(whole screens, the status panel, backgrounds, round stars, sound effects, speech and music), and the 2026 security audit's fixes are in. The supported games are the Amiga disk
releases and the CDTV release. The disk releases run exactly as before: the canonical image and every existing replay golden are unchanged.
Read "What is not verified" before you rely on it.

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
- **The opt-in protection patch is not in the way.** It is still only offered while importing a retail disk 1 that has no key
  track, behind the full disclaimer and a separate "I own the original disc" box that starts unticked every time.
- **x2recomp is free software under the GNU GPL, version 3 or later** (GPL-3.0-or-later). The full text is in `LICENSE`,
  included in the package. It is GPL because part of the accurate-timing code is derived from WinUAE/Amiberry
  (`PROVENANCE.md`). Third-party components keep their own GPL-compatible licences (`THIRD-PARTY-NOTICES.md`, `licenses/`): the
  launcher adds the Unscii pixel fonts (public domain), and dr_flac and stb_vorbis (asset-pack audio) are listed with their licence texts.
  The pictures of the launcher are drawn by the program itself.

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

## What is new: the launcher and the in-game menu

- **The launcher.** `x2recomp` started without arguments (or double-clicked) opens it: one card for each release you have imported
  (Amiga disk, Amiga CDTV) with Play, and the import page when nothing is imported yet. Drop disk images, flux dumps, a disc
  image, a zip or a folder on it; it says what it found, shows a progress bar and a Cancel button (a cancelled import changes
  nothing), and explains errors in plain sentences. Pages: **Display**, **Audio** (with a test tone), **Controls** (key and gamepad
  bindings), **Packs** (enable, order, check, add and dump asset packs), **Advanced** and **About**. `--gui` and `--no-gui` and
  `launcher = auto | always | never` choose when it appears; any other argument starts the game exactly as before.
- **The in-game menu.** F1 (Cmd+1 on a Mac) or the gamepad's Start button pauses the game and opens the Faithful switch, Display, Audio,
  Controls, Asset packs, Assists and cheats, Reset, **Back to launcher** and Quit. Changes apply at once where the release can take them
  and are saved. **F1 no longer toggles Faithful by itself**: the switch is the menu's first row, `--faithful` is unchanged, and the
  `faithful` action in `input.cfg` can still be bound to a key (`input.cfg` is format 6; older files are upgraded and kept as `input.cfg.bak`).
- **Advanced.** What each import recorded, a bug report you can copy (no game data, no folder names), a check for any image, the program's
  log (`x2recomp.log` in the data folder), export, import and reset of the settings file, and the command-line-only tools.
- **Comfort.** Text size (Large), Contrast (High) and Motion (Reduced). Everything works with a keyboard or a gamepad alone. Screen
  readers are not supported; the command line is the accessible way.
- `docs/GUI.md` describes every page.

## What is new: asset packs version 2 and the importer additions

- **Asset packs, version 2.** Beyond sprites and tiles a pack can now replace whole **screens** and the **status panel**, give the far background
  layer **backgrounds**, **round stars** and **hit flashes**, and replace the game's **sound effects**, **speech** and **music** (WAV, FLAC and Ogg
  Vorbis, with loop points). A version 1 pack loads unchanged; packs layer, can be ZIPs, and are found in a `packs` folder. The tools: `--dump-assets`
  and `--dump-audio` write what to replace, `--check-asset-pack` validates against a dump, `--list-asset-packs` lists, and `tools/assetpack/`
  slices, packs sprite sheets, validates and builds a demo pack from your own dump (`make_demo_pack.py`). The graphics cost is inside the
  design's 5% budget (1.3 to 1.6% of a 60 Hz frame on the flood replays), round stars render in parallel (`--modern threads=N`), and the guide,
  field reference, five tutorials and a complete skeleton ship in the package (`ASSET-PACKS.md`, `ASSET-PACKS-REFERENCE.md`, `asset-packs/`).
- **Importer additions since 0.2.0.** FluxEngine `.flux` files are read, two captures of one disk given together are merged sector by sector, the
  flux decoder retries unreadable sectors with a PLL limited to valid MFM, and the Rob Northen PDos release (WHDLoad imager v5) is a known release
  (recognised; not importable until a second capture of the same disk exists: `docs/re-notes/pdos-v5.md`).

## Security

A source audit was run in November 2026 (a standard-profile pass with a source-first method, sandboxed local runs of the harnesses that
confirmed its findings, and independent re-checks of every record). It confirmed **eleven findings**; **ten affect code in this release and are fixed in it**
(`CHANGELOG.md`, "Security"): two out-of-bounds heap writes and four more defects (an out-of-bounds read, two null-pointer crashes, a division by zero and endless loop) in the
vendored Ogg Vorbis decoder, reachable from a downloaded asset pack's `.ogg` (local patches 5 to 10 in `vendor/audio/README.md`), an out-of-bounds read in the modern
compositor's picture cache, path traversal in the developer scripts `tools/adf.py` and `tools/iso9660.py`, and an index check in the developer analysis tool
`tools/variants/stubrun.c`; the eleventh was in code that was removed before release. It found no network exposure and no code-execution path, and the project has no
server. **The audit was partial:** four areas were not examined (the GUI's threading and callback lifetimes, record-count amplification in the archive readers, the Amiga machine core
and the CDTV guest services), and **four low-severity leads are open**: archive and image names are printed to the terminal and log with control characters, the startup re-import trusts `source=` lines in
`manifest.txt` (traced in the source but not run: a hostile AmigaDOS volume name may be able to add one, which would make the next start re-import
from a path you did not choose, or on Windows look up a network path; the audit's reviewers disagreed on whether this crosses a boundary), an asset pack's `name` can carry control characters into the console, and the copy-able command line is quoted with
double quotes only. Treat asset packs and disk images from strangers like any downloaded program data.

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
| PDos one-disk release (WHDLoad imager "v5") | | recognised by its boot block in a WHDLoad install; **not importable** yet (a real flux dump of it was analysed: `docs/re-notes/acquisitions-flux-acorn.md`) |
| the playable demo disks, the L.G.C.S.S. compilation, the WHDLoad demo and CDTV installs | | **recognised and reported**, not imported: different programs |

Formats read: ADF, ADZ, ZIP (nested), DMS, extended ADF, IPF, SCP, KryoFlux stream files, HFE, LHA archives, WHDLoad
installs, CDTV disc images (ISO 9660 data track), and folders of these. Exit codes of `--import`: 0 imported, 1 could not
write, 2 bad input, 3 unsupported image (including a protected retail disk 1 without its key track), 4 confirmation
required. Making your own dump: `docs/DUMPING.md` (disks); a CDTV disc is dumped to a Redump-style BIN/CUE set with any CD
ripper that keeps the data track and the audio tracks as separate files.

## Install and quick start

Unpack the archive for your platform and start `x2recomp`: the launcher opens, and its import page takes your disks or your disc
dropped onto it (or chosen with its buttons). The command line does the same.

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

- **Tests.** The default interpreter build runs the whole suite without game data on every CI platform (macOS arm64, macOS universal, Linux GCC and
  Clang and Windows MSVC are required checks; macOS x86_64, Windows MinGW-w64, g++ 12 on Ubuntu 22.04 and the sanitizer and fuzz jobs also run); the tests that need real images are
  reported as Skipped there. With the maintainer's images attached (the disk releases, both retail pressings, the Power Pack, 1762 and the CDTV disc)
  they run too. **The counts (registered, passed, skipped, per platform) are to be confirmed by the maintainer from the tagged commit's runs before release.**
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
- **The launcher and the in-game menu.** All without game data and run by CI on the required platforms: the widget kit and the launcher and in-game-menu golden
  renders compared at zero tolerance; the launcher model with a fake importer; the real window loop under SDL's dummy drivers, walked with a gamepad's buttons
  only; every page reachable by the d-pad in three canvas sizes and both themes; reduced motion draws the same picture whatever the time; the settings schema
  against `settings.cfg` and the command line; the log, diagnostics and test tone; the install rules against the packaging scripts. With the maintainer's own
  assets: the game started as the launcher starts it, the menu walked to Quit (`gui_flow_disk`, `gui_flow_cdtv`), and the same frame hashes with and without a visit
  to the menu (`menu_pause_disk`, `menu_pause_cdtv`).
- **Asset packs.** The pack loader, manifest parser, ZIP reader and the PNG, WAV, FLAC and Ogg Vorbis decoders have fuzz targets that also run as plain tests, and run
  under AddressSanitizer and UBSan in CI; with the maintainer's assets a pack made from a dump reproduces the pictures exactly and the graphics cost is inside the 5% budget.
- **Hand play: [to be confirmed by the maintainer before release]**
- **Hand run: [to be confirmed by the maintainer before release]** (launcher, import by drop and by file dialog, the menu over a running game, rebinding a key and a gamepad button, Back to launcher, on at least macOS and one of Windows or Linux).

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
- **Gamepad and touch on real devices**: the tests drive the same event paths with synthetic events. **To be confirmed by the maintainer.**
- **Raspberry Pi** (3, 4 and 5): idle CPU of the launcher, the feel of the menu over a running game, the launcher on a small screen. **To be
  measured and recorded in `docs/PORTING.md`.**
- **Native file dialogs** (Choose files, Add a pack, Export and Import settings) depend on the platform's dialog; every one has a
  drag-and-drop alternative. Linux needs a desktop portal or zenity for the dialog.
- "Back to launcher" starts the program again; it is not exercised by the automated tests (they check that the menu asks and calls the host).
- The Advanced page does not offer screenshot or audio capture buttons, and the developer options (`--timing`, `--chip-ram`, replays) have no
  buttons: they are command-line tools and the page lists them. Several imported builds of the same family side by side (for example two disk
  releases) are still one slot each.
- **Security audit coverage:** partial, with four open low-severity leads (see "Security").
- **No dump of the maintainer's own retail disks has been imported yet** (as in 0.2.0): the retail import is verified
  against preservation images, and the flux readers against public dumps; a real flux dump of the retail disk 1 of this
  game was not available. A real Amiga flux dump of the PDos release read 1,901 of 1,908 sectors directly; it cannot be
  imported yet.
- **The crack variants and the 1762 build** are as described in `RELEASE-NOTES-0.2.0.md`; budget (floppy version 4) has no image.
- **HFE v3 support** is best effort and untested; flux decoding was validated on synthetic flux and public dumps.
- **No comparison against real Amiga or CDTV hardware.** Every accuracy claim is relative to an emulator.
- No unassisted playthrough has been done; disc loads are instant (the game's random generator drifts after each load);
  widescreen is cosmetic. All as in 0.1.0 (`README.md`, "Status and known gaps").

## Known limits

- The CD audio import (about 680 MB) cannot be cancelled once it has started.
- "Hold Shift at start to see the launcher" is not possible (SDL cannot read modifiers before a window exists); use `--gui`.
- `Motion: Auto` means full motion: SDL does not report the system's reduced-motion setting.

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
