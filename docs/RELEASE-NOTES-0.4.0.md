# x2recomp 0.4.0 — release notes (DRAFT)

> **Draft.** 0.4.0 is not released. It waits for the maintainer's hand-played session of the Atari ST release. Until then the version in
> `CMakeLists.txt` is the last released one, the checksums below are `TBD`, and every line marked "to be confirmed" has to be checked by the
> maintainer. When the release is made, this file loses this paragraph and `CHANGELOG.md` gets the 0.4.0 heading (`docs/RELEASING.md`,
> "Cutting 0.3.0, 0.4.0 and 0.5.0").

A native port of the Amiga game **Xenon 2 Megablast** (The Bitmap Brothers, 1989), built by static recompilation. The game's own 68000 code runs
on a model of the machine it was written for, either on the Musashi interpreter or as 68000 code translated to C++.

0.2.0 read the preservation formats of the Amiga disk releases and 0.3.0 added the CDTV release. **0.4.0 adds the Atari ST release as a third
game:** it imports from your own copy of the Kixx budget re-release, boots the disk's own boot sector on a model of the ST (68000, WD1772 floppy
controller and DMA, MFP 68901, IKBD, Shifter, YM2149), plays with sound, picture and input, and runs on the translated executor too. The
Amiga releases run exactly as before: the canonical image and every earlier replay golden are unchanged. Read "What is not verified" before you
rely on it.

## Downloads

| Platform | File | sha256 |
|---|---|---|
| macOS (Apple Silicon and Intel) | `x2recomp-0.4.0-macos-universal.zip` | `TBD` |
| macOS (Apple Silicon and Intel) | `x2recomp-0.4.0-macos-universal.dmg` | `TBD` |
| Windows x64 | `x2recomp-0.4.0-windows-x64.zip` | `TBD` |
| Linux aarch64 (64-bit Raspberry Pi OS) | `x2recomp-0.4.0-linux-aarch64.tar.gz` | `TBD` |
| Linux x86_64 | `x2recomp-0.4.0-linux-x86_64.tar.gz` | `TBD` |

None of the binaries is code-signed (macOS has an ad-hoc signature only); see "Install and quick start".

## Legal position

- **This release contains no game data.** No disk images, no game code, no graphics, no music, no sound, no text, and no code translated from
  the game. No TOS, EmuTOS or other ST firmware is included or needed: the program boots the disk's own boot sector on the emulated controller
  and the game does the rest itself.
- **Bring your own copy.** You need your own Atari ST disk image of the game. `x2recomp --import` reads it on your machine and writes what the
  runtime needs to your per-user data directory. Nothing leaves your machine.
- **Copy-protected ST releases are not supported.** The Image Works and Mirror Image ST releases use an ST Copylock; they are recognised and
  refused with the reason (`--diagnose-image` shows the findings). Nothing here defeats a protection.
- **x2recomp is free software under the GNU GPL, version 3 or later** (GPL-3.0-or-later). The full text is in `LICENSE`, included in the package.
  Third-party components keep their own GPL-compatible licences (`THIRD-PARTY-NOTICES.md`, `licenses/`). The YM2149 level table is a set of
  physical measurements of a real ST taken from the sc68 player and Hatari and credited in `runtime/st/ym2149.cpp`.

Xenon 2 Megablast itself is copyright The Bitmap Brothers / Mirrorsoft and is covered by no licence here. A `-DX2_RECOMP=ON` build contains C++
translated from your own game image (any release) and is **not** redistributable.

## What is new: the Atari ST release

- **Import.** `x2recomp --import <image>` takes a `.st`, an `.msa` or a Pasti `.stx` disk image, also inside a ZIP or a folder. It accepts the
  **Kixx budget re-release** (1993, one double-sided 80 x 10 disk of 819,200 bytes, no copy protection, SHA-1
  `323d6859635b6b82c86363db048b26e0f3df994c`), normalises it to one logical image and records what it found in `assets-st` (the Amiga
  releases stay in `assets` and `assets-cdtv`). `--diagnose-image` and `--list-variants` know the disk. Details: `docs/re-notes/atari-st-importer.md`.
- **Play.** `x2recomp --game st` (or plain `x2recomp` when it is the only game imported; `game = auto|disk|cdtv|st` in `settings.cfg` chooses when
  several are). The game's own boot sector runs on an emulated WD1772: boot, the depacker, the front end, the credits and the title, then the
  game from the title with the fire button; all five levels, the shop, game over, the ending. The window title names the release.
- **The machine** (`runtime/st`, `docs/re-notes/atari-st-machine.md`): 68000 on a 4-cycle bus with Hatari's device wait states, the WD1772 with
  its DMA at real drive speed (`--st-fdc-speed N` to hurry it), the MFP's four timers and interrupts (including the 7,979 Hz timer the
  digitised speech uses), the IKBD with a joystick on port 1 and the keyboard, and a Shifter that draws line by line with the palette the beam
  had at the start of each line (low resolution; the game uses no other).
- **Sound.** A YM2149 model at 250 kHz (tone, noise, envelope, mixer, the measured level table), sample-accurate register writes and the game's
  digitised speech and effects through the volume register (`docs/re-notes/atari-st-sound.md`).
- **Input.** A joystick or keyboard, scripted or live. Controls are as for the other releases; the pad's Start button and F1 open the menu.
- **Scores and options.** The original keeps no high-score table; the ST release saves one, in its own file. God mode, extra credits and
  autofire work as options on this release (`docs/re-notes/atari-st-hooks.md`); with none of them given no byte of the game is touched.
- **Recompiled executor and modernisation.** `-DX2_RECOMP=ON` translates the ST release too (11 RNC streams, seven modules), bit-identical to the
  interpreter on every scripted run (`docs/re-notes/atari-st-recomp.md`). Asset packs can replace the ST release's screens and status panel (the
  screens and panel parts of the pack format; the other parts are Amiga features).
- **Atari ST in the window.** The launcher shows an Atari ST card once an ST disk is imported; the in-game menu and the pages work as for the
  other games (`docs/GUI.md`), where 0.5.0 describes them.

## What you can import

`x2recomp --list-variants` prints the table with the hashes; `x2recomp --diagnose-image FILE...` tells you what the importer makes of an image.
The Amiga rows are as in `RELEASE-NOTES-0.3.0.md`.

| Release | Give the importer | Result |
|---|---|---|
| **Atari ST, Kixx budget re-release** (1993) | `.st`, `.msa` or `.stx` (also in a ZIP or a folder) | **imports and plays** (`build=atari-st-kixx`) |
| Atari ST, Image Works release | | recognised, **refused**: ST Copylock (loader layer 3 not reached) |
| Atari ST, Mirror Image release | | recognised, **refused**: Copylock-encrypted |
| Atari ST Format preview disk | | recognised, **refused** with the reason |

Exit codes of `--import`: 0 imported, 1 could not write, 2 bad input, 3 unsupported image, 4 confirmation required.

## Install and quick start

Unpack the archive for your platform, then import your disk image. The launcher window also accepts it dropped onto it.

**macOS** — `x2recomp.app` with SDL3 bundled; ad-hoc signed, not notarised, so Gatekeeper objects on first launch: right-click the app and
choose Open, or `xattr -dr com.apple.quarantine x2recomp.app`.

```
./x2recomp.app/Contents/MacOS/x2recomp --import Xenon2-ST.zip     # your Kixx .st/.msa/.stx, or a zip or folder holding it
./x2recomp.app/Contents/MacOS/x2recomp --game st
```

**Windows** — unzip anywhere and run `x2recomp.exe`; nothing has to be installed. The exe is unsigned: choose "More info", then "Run anyway".

```
x2recomp.exe --import Xenon2-ST.zip
x2recomp.exe --game st
```

**Linux** — `bin/x2recomp` finds `lib/libSDL3.so.0` beside it; glibc 2.35 or newer (Debian 12, Ubuntu 22.04 and newer). `linux-aarch64` is for
64-bit Raspberry Pi OS, `linux-x86_64` for a PC (`uname -m` says which).

```
tar xzf x2recomp-0.4.0-linux-x86_64.tar.gz && cd x2recomp-0.4.0-linux-x86_64
./bin/x2recomp --import ~/Xenon2-ST.zip
./bin/x2recomp --game st
```

Imports made by earlier versions are redone automatically from their recorded sources the first time you start this version.

## What is verified, and how

- **Tests.** *To be filled from the release-preparation run:* the default interpreter build with no game data, on every CI platform, and with the
  maintainer's images attached. The ST tests among them: `st`, `st_machine`, `st_sound`, `st_game`, `st_files`, `st_hooks_god`, `st_hooks_credits`,
  `st_hooks_scores`, `st_hooks_levels`, `st_hooks_ending` and `st_kixx` (all six Skipped without the Kixx dump), 21 `replay_st_*` (Skipped without the dump),
  `st_replay_scripts` (no game data), `gui_flow_st`, and, on a translated build, the `recomp_st_*` tests.
- **ST replay corpus.** 21 scripted replays (front end, levels 1-5, the shop, the level 1 boss, the ending reached naturally, game over, two
  players, menus) with recorded frame and audio hashes; each matches a fresh run, 0 unmapped accesses in all. On the translated executor all 21
  give identical frame hash, audio hash, a state digest of all RAM and registers at **every frame**, the frame log and the final RAM to the
  interpreter (92.2% to 96.8% of the executed instructions ran translated); the sanitizer builds run a selection of them clean.
- **Independent reference.** Hatari 2.6.1 with EmuTOS 1.4 UK, built locally, driven by the same scripts (`docs/re-notes/atari-st-replays.md`,
  `atari-st-video.md`, `atari-st-sound.md`). Where the two runs are in the same state the pictures agree: the Bitmap Brothers caption and logo
  match at 100%, the title screen at a median of 100%, the attract loop at a median of 99.88%; the menu, shop, name entry, continue, GAME OVER and
  the ending screens at 99.8% to 100%; the first ~1,000 frames of a played level at 88% to 99%; shop purchases leave identical cash values; the
  game-over path gives identical ships, credits and scores at every checked event; the scroll position agrees to within a few units over
  thousands of frames of fast-forward; game events happen at constant offsets with the same variable values. YM audio: where events line up the
  envelope correlates at 0.91 and the spectrum at 0.99; the digitised speech's waveform at 0.955 to 0.96.
- **Amiga releases and importer.** Everything from 0.2.0 and 0.3.0 is unchanged and its goldens identical.
- **Hand play: [to be confirmed by the maintainer before release]**

## What is not verified

- **The ST release has not been played by hand** (see the line above). The evidence is scripted replays, the Hatari comparison and the unit tests.
- **Bosses of levels 2 to 4.** They are reached but not defeated by script; their death flags are not mapped, and in level 2's arena the ship stops
  answering the joystick after a fast-forward. Level 5's guardians are shot down to 14, and the ending is reached through the game's own end-of-level
  event, so it works; whether a human can finish levels 2 to 4 is the open item for the hand-played session.
- **Played levels part from Hatari after about 1,000 frames** (the scroll position, then the enemies; the pictures match again after each
  re-synchronisation). Whether the gap is in this program's timing model or in Hatari's was not settled. The front end's animation runs about
  1.3% fast against Hatari (the remainder after the bus rounding and the device wait states, not attributed yet).
- **One resolution only.** The game uses low resolution; medium and high resolution give a black line, and no ST video modes beyond what the
  Kixx build needs are modelled. No STE hardware, no TOS, no GEM.
- **Other ST releases are not importable** (Copylock, see above), and only the Kixx image has been used.
- **The sound** matches the reference where events line up; the percussion is noise-based and correlates only at 0.44 to 0.5 sample by sample
  (envelope and spectrum agree). The output level is set to Hatari's, not to a real ST's.
- **Windows and Linux** ran the ST tests only in CI without the disk; the ST import and play have been run on macOS arm64 only. No Raspberry Pi
  has run it (speed unmeasured). Gamepads and drag-and-drop import are untested on real devices.
- **No comparison against a real Atari ST.** Every accuracy claim is relative to an emulator.
- **Security audit.** The 2026 audit of the source (`CHANGELOG.md`, "Security") fixed its eleven confirmed findings; its coverage was partial:
  five areas were not examined (the GUI's threading and callback lifetimes, the ST device models' memory safety under a hostile disk,
  record-count amplification in the archive readers, the Amiga machine core and the CDTV guest services), and four low-severity leads remain
  open (archive and image names printed with control characters, the startup re-import trusting `source=` lines in `manifest.txt`, control
  characters in an asset pack's name, and the command-line footer's shell quoting). None involves the network or code execution.
- No unassisted playthrough has been done; disc loads are instant on the Amiga releases; widescreen is cosmetic (`README.md`, "Status and known gaps").

## Checksums

The sha256 of each download is in the table above. Each archive also contains a `SHA256SUMS.txt` listing every file inside it. Verify both:

```
shasum -a 256 x2recomp-0.4.0-macos-universal.zip            # macOS: compare with the table
cd x2recomp-0.4.0-macos-universal && shasum -a 256 -c SHA256SUMS.txt

sha256sum x2recomp-0.4.0-linux-aarch64.tar.gz               # Linux: compare with the table
cd x2recomp-0.4.0-linux-aarch64 && sha256sum -c SHA256SUMS.txt
```

The per-file manifest is generated at packaging time from the staged contents, so it also confirms that no game data, imported assets or
translated code is present — `tools/package.sh` and `tools/package.ps1` refuse to package a `-DX2_RECOMP=ON` build and check the staged files
before archiving.
