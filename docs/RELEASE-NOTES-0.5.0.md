# x2recomp 0.5.0 — release notes (DRAFT)

> **Draft.** 0.5.0 is not released. 0.3.0 (the CDTV release) and 0.4.0 (the Atari ST release) come first; the version in
> `CMakeLists.txt` is still the last released one, the checksums below are `TBD`, and every line marked "to be confirmed" has to be
> checked by the maintainer before the release is published. When the release is made, this file loses this paragraph, the numbers
> are filled from the release-preparation branch, and `CHANGELOG.md` gets the 0.5.0 heading.

A native port of the Amiga game **Xenon 2 Megablast** (The Bitmap Brothers, 1989), built by static recompilation. The game's own
68000 code runs on a model of a PAL Amiga 500 (and, since 0.3.0 and 0.4.0, the CDTV and the Atari ST release's machines).

**0.5.0 is the release of the window you can use instead of remembering options.** The launcher opens before a game starts and the
in-game menu opens over it; both are made of the same pages, and each page shows the command line that does the same thing. The
command line is unchanged and does everything the window does. This release also brings the asset-pack tooling that has accumulated
since 0.2.0 (see `CHANGELOG.md`).

## Downloads

| Platform | File | sha256 |
|---|---|---|
| macOS (Apple Silicon and Intel) | `x2recomp-0.5.0-macos-universal.zip` | `TBD` |
| macOS (Apple Silicon and Intel) | `x2recomp-0.5.0-macos-universal.dmg` | `TBD` |
| Windows x64 | `x2recomp-0.5.0-windows-x64.zip` | `TBD` |
| Linux aarch64 (64-bit Raspberry Pi OS) | `x2recomp-0.5.0-linux-aarch64.tar.gz` | `TBD` |
| Linux x86_64 | `x2recomp-0.5.0-linux-x86_64.tar.gz` | `TBD` |

None of the binaries is code-signed (macOS has an ad-hoc signature only); see `README.md` for what that means on first launch.

## Legal position

Unchanged from the earlier releases, and the window says it too (the About page):

- **This release contains no game data**: no disk images, no disc contents, no game code, graphics, music, speech or text, and
  nothing translated from the game. The pictures of the launcher are drawn by the program itself.
- **Bring your own copy.** The importer reads your own images on your machine; nothing is uploaded.
- **The opt-in protection patch is not in the way.** It is still only offered while importing a retail disk 1 that has no key
  track, behind the full disclaimer and a separate "I own the original disc" box that starts unticked every time.
- **x2recomp is free software under the GNU GPL, version 3 or later.** Third-party components keep their own licences
  (`THIRD-PARTY-NOTICES.md`, `licenses/`): the launcher adds the Unscii pixel fonts (public domain), dr_flac and stb_vorbis
  (asset-pack audio) are listed with their licence texts.

## What is new

- **The launcher.** `x2recomp` started without arguments (or double-clicked) opens it: one card for each release you have imported
  (Amiga disk, Amiga CDTV, Atari ST) with Play, and the import page when nothing is imported yet. Drop disk images, flux dumps, a disc
  image, a zip or a folder on it; it says what it found, shows a progress bar and a Cancel button (a cancelled import changes
  nothing), and explains errors in plain sentences. Pages: **Display**, **Audio** (with a test tone), **Controls** (key and gamepad
  bindings), **Packs** (enable, order, check, add and dump asset packs), **Advanced** and **About**. `--gui` and `--no-gui` and
  `launcher = auto | always | never` choose when it appears; any other argument starts the game exactly as before.
- **The in-game menu.** F1 (Cmd+1 on a Mac) or the gamepad's Start button pauses the game and opens the Faithful switch, Display, Audio,
  Controls, Asset packs, Assists and cheats, Reset, **Back to launcher** and Quit. Changes apply at once where the release can take them
  and are saved. **F1 no longer toggles Faithful by itself**: the switch is the menu's first row, `--faithful` is unchanged, and the
  `faithful` action in `input.cfg` can still be bound to a key.
- **Advanced.** What each import recorded, a bug report you can copy (no game data, no folder names), a check for any image, the program's
  log (`x2recomp.log` in the data folder), export, import and reset of the settings file, and the command-line-only tools.
- **Comfort.** Text size (Large), Contrast (High) and Motion (Reduced). Everything works with a keyboard or a gamepad alone. Screen
  readers are not supported; the command line is the accessible way.
- **Asset packs.** The graphics-pack cost is inside the design's budget, round stars render in parallel (`--modern threads=N`), and the
  guide, field reference and tutorials ship in the package (`ASSET-PACKS.md`, `ASSET-PACKS-REFERENCE.md`, `asset-packs/`).
- `docs/GUI.md` describes every page.

## What is verified, and how

*To be filled in when the release is prepared.* The tests that cover this release's window, all without game data and run by CI on the
required platforms: the widget kit and 64 launcher and 48 in-game-menu golden renders compared at zero tolerance; the launcher model with a
fake importer; the real window loop under SDL's dummy drivers, walked with a gamepad's buttons only; every page reachable by the d-pad in
three canvas sizes and both themes; reduced motion draws the same picture whatever the time; the settings schema against `settings.cfg`
and the command line; the log, diagnostics and test tone; the install rules against the packaging scripts. With the maintainer's own
assets: the game started as the launcher starts it, the menu walked to Quit (`gui_flow_disk`, `gui_flow_cdtv`, `gui_flow_st`), and the
same frame hashes with and without a visit to the menu.

Counts, platforms and dates: **to be confirmed by the maintainer before release.**

## What is not verified

- **Gamepad and touch on real devices**: the tests drive the same event paths with synthetic events. **To be confirmed by the maintainer.**
- **Raspberry Pi** (3, 4 and 5): idle CPU of the launcher, the feel of the menu over a running game, the launcher on a small screen. **To be
  measured and recorded in `docs/PORTING.md`.**
- **Native file dialogs** (Choose files, Add a pack, Export and Import settings) depend on the platform's dialog; every one has a
  drag-and-drop alternative. Linux needs a desktop portal or zenity for the dialog.
- "Back to launcher" starts the program again; it is not exercised by the automated tests (they check that the menu asks and calls the
  host).
- The Advanced page does not offer screenshot or audio capture buttons, and the developer options (`--timing`, `--chip-ram`, replays) have
  no buttons: they are command-line tools and the page lists them.
- Several imported builds of the same family side by side (for example two disk releases) are still one slot each.

## Known limits

- The CD audio import (about 680 MB) cannot be cancelled once it has started.
- "Hold Shift at start to see the launcher" is not possible (SDL cannot read modifiers before a window exists); use `--gui`.
- `Motion: Auto` means full motion: SDL does not report the system's reduced-motion setting.
