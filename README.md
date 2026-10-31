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

## Supported releases

You bring your own disks, as images or as dumps of your own disks (`docs/DUMPING.md` explains how to make one with a
KryoFlux or a DrawBridge). `x2recomp --list-variants` lists every variant the importer knows with its hashes and how
it is handled; `x2recomp --diagnose-image FILE...` prints a per-track report of an image and what the importer makes
of it, without printing any game data. The importer tells disks apart by content, not by file name.

| Release | Give the importer | Result |
|---|---|---|
| **BS1 `[t+3]` two-disc set** (the canonical image, the one the replay goldens were recorded from), its `[a]` and Deejay99 variants, the `[h Timecircle]` set, the "Xenon 2 & 4 Others" compilation | ADF, ADZ, DMS, IPF, flux, WHDLoad, zip | the canonical image; the importer never runs the trainer loader, so there is no trainer and no infinite lives |
| **Retail two-disc release** (Image Works; both pressings, SPS 0297 and SPS 2874 `[a]`) | disk 1 as an **IPF or flux dump** (SCP, KryoFlux stream folder, HFE) so the Copylock key track is kept; disk 2 in any format | imports; not canonical (the original credit text, and in the first pressing a code revision in the level 4 boss), so it has its own goldens |
| the same retail disk 1 from a plain ADF, DMS or WHDLoad `Disk.1` (no key track) | add the opt-in patch below | refused with the options unless you choose the patch |
| **Power Pack** compilation (SPS 2234, one disk) | IPF, flux or the WHDLoad `Disk.1` | imports as its own build; disc 2 rebuilt from the disk's game files (functionally equal); slightly different audio |
| **Bombuzal + Xenon 2** compilation (SPS 1762, two disks) | both disks | imports as its own build with its own disk 2; plays through, but its recorded runs differ from canonical early on (cause not investigated) |
| cracks of the retail disk 1: PNA, Quartex, Flashtro +16 (trainer) | ADF + the standard disc 2 | import as their own variants (`canonical=false`) |
| PNA `[a]`, Black Monks, and the one-disk crunches Vision Factory, VF `[a]`, Black Monks `[h][t +36]` | ADF (+ the standard disc 2 where needed) | import; trainer builds are flagged (`trainer=true`) |
| Predators +9, Flashtro +16 `[a]`, damaged disk 2 copies | | refused with the reason |
| **The CDTV release** (your own disc image: a `.cue` with its `.bin` files, a ZIP or folder holding them, or the bare data track as `.bin`/`.iso`) | the disc image | the game is **pre-linked and stored** (`assets-cdtv`, `build=cdtv`, 1 MB chip RAM; no second disk) and the CD audio is imported too. **It starts and plays in the interpreter** (`x2recomp --game cdtv`, or just `x2recomp` when it is the only game imported): title and menus, level 1 with its music, the second button, scores, assists; see "Status and known gaps" for what has and has not been checked. The recompiled build (`-DX2_RECOMP=ON`) translates it too, with the same frame-for-frame equivalence (`docs/re-notes/cdtv-recomp.md`). No interpolation or widescreen for it yet |
| the playable demo disks, the L.G.C.S.S. compilation, the WHDLoad demo and CDTV installs | | recognised and reported as different programs, not imported |

Formats read: ADF, ADZ, ZIP (nested), DMS, extended ADF, IPF, SCP, KryoFlux stream files, HFE, LHA archives, WHDLoad
installs, CDTV disc images (ISO 9660 data track), and folders of these. Exit codes of `--import`: 0 imported, 1 could not write, 2 bad input, 3 unsupported image
(including a protected retail disk 1 without its key track), 4 confirmation required.

**The retail disk and its protection.** The retail first disk is copy-protected with a Rob Northen Copylock key track
that a plain ADF cannot hold. The importer checks the key track in your IPF or flux dump, decodes the game image from the
disk unmodified, and records a receipt (`copylock.txt`, bound to the image); the runtime then answers the game's own
Copylock check (`docs/re-notes/retail-protection.md`). No key value is in the program, and without a receipt a retail
image is refused.

**The opt-in protection patch.** If you only have a plain ADF, DMS or WHDLoad `Disk.1` of a retail disk 1, you can
explicitly ask for the alternative instead:

```
x2recomp --import disk1.adf disk2.adf --apply-protection-patch --i-confirm-i-own-the-original
```

This changes 18 bytes of the game image decoded from your own retail disk 1 (the Copylock call and a check constant) so
the check is removed from the image instead of being answered at run time. It prints a disclaimer first, needs the
confirmation flag (without it the import stops with exit code 4; the import window asks for a second keypress after
showing the same text), is only applied to a catalogued retail pressing (checked by SHA-1), and is recorded in the
manifest as `patched=true`. Nothing patched is distributed by this project; use it only with a copy you own, and comply
with the law where you live. The key-track method is the default and the preferred one.

Notes: `docs/re-notes/sources.md` catalogues the known images; `cracks.md`, `single-disk-variants.md`,
`build-variants.md` and `retail-protection.md` hold the analysis.

## Quick start

You need CMake 3.29 or newer, a C++20 compiler and SDL3. SDL3 can come from Homebrew `sdl3` or a distribution
package, or be downloaded and built with `-DX2_FETCH_SDL3=ON`. Without SDL3 only the headless runtime is built.
The build is exercised in CI on macOS (arm64, x86_64, universal), Linux (GCC and Clang) and Windows (MSVC
and MinGW-w64). CI runs without game data, though: the game itself has been played on macOS and Windows, and
run headless on arm64 Linux (`docs/PORTING.md`).

```
tools/build_all.sh Xenon2.zip         # build, then import your disk images (any format listed above, or a folder)
./build/x2recomp                      # play
```

You can also skip the import step and just start `./build/x2recomp`. On the first run the window asks you to
drag and drop your disk images, a zip, or a folder onto it. On the command line, use
`x2recomp --import PATH [PATH2]`, whose exit codes are listed under "Supported releases".

**Binaries are not signed.** The project has no Apple developer account or Windows code-signing certificate, so
release packages are unsigned (the macOS app is only ad-hoc signed, which Apple Silicon needs in order to run it)
and not notarised. The first launch of a downloaded copy is therefore blocked or warned about by the operating
system, which does not mean anything is wrong with the file. Verify it with the `.sha256` file next to the
archive, then:

- **macOS:** right-click `x2recomp.app` and choose Open, or clear the quarantine flag with
  `xattr -dr com.apple.quarantine x2recomp.app`. A build you make yourself on the same Mac is not affected.
- **Windows:** SmartScreen shows "Windows protected your PC": choose More info, then Run anyway. Building from
  source avoids it.
- **Linux:** nothing is blocked.

To build the optional recompiled executable as well, which needs Python 3, run
`tools/build_all.sh --recomp [--test] Xenon2.zip` (it translates the disk release, the CDTV release, or both, whichever you imported).
`docs/BUILDING.md` has every step and option.

## Linux and Raspberry Pi

The Linux release is a tarball that needs nothing installed: `bin/x2recomp` finds `lib/libSDL3.so.0` next to it
through its own `RUNPATH`, and asks the system for nothing but the C library.

```
sha256sum -c x2recomp-0.2.0-linux-aarch64.tar.gz.sha256
tar xzf x2recomp-0.2.0-linux-aarch64.tar.gz
cd x2recomp-0.2.0-linux-aarch64
./bin/x2recomp --import ~/Xenon2.zip     # once, your own disk images
./bin/x2recomp                           # play
```

**Which archive.** `linux-aarch64` is 64-bit ARM: a Raspberry Pi 4, 5 or 400 running **64-bit** Raspberry Pi OS,
and any other arm64 Linux. `linux-x86_64` is a PC. Check with `uname -m`, which must print `aarch64` or
`x86_64`. **32-bit Raspberry Pi OS (`armv7l`, armhf) is not supported by either archive** — the same 64-bit OS
image on the same Pi is the easy fix, and otherwise the only route is building from source
(`docs/BUILDING.md`), which has never been tried on armhf.

**Minimum system:** glibc 2.35 or newer and a little-endian 64-bit CPU. That covers Debian 12 "bookworm"
(glibc 2.36, what current 64-bit Raspberry Pi OS is built on), Ubuntu 22.04, and everything newer. `x2recomp`
needs no other library: libstdc++ and libgcc are linked into it, and SDL3 opens the X11, Wayland, KMSDRM,
OpenGL/GLES, ALSA, PulseAudio and PipeWire libraries it finds at run time rather than linking them, so one
archive works across desktops. If a much older system says `GLIBC_2.35 not found`, build from source there.

**Picture and sound.** A desktop session (Wayland or X11) is the normal case and needs no options. On a Pi with
no desktop, SDL can drive the console framebuffer directly through KMSDRM, which needs the user to be in the
`video` and `render` groups. Force a choice with `SDL_VIDEO_DRIVER=wayland|x11|kmsdrm` and
`SDL_AUDIO_DRIVER=pipewire|pulseaudio|alsa` (those are SDL3's names; SDL2's `SDL_VIDEODRIVER` and
`SDL_AUDIODRIVER` still work); `--headless` opens neither a window nor an audio device at all, for
a machine with no display. The legacy Broadcom `dispmanx` backend is deliberately not built — it is gone from
current Raspberry Pi OS.

**On weak hardware,** start with `--scale 2 --filter nearest`, leave `--modern interp` off (it renders extra
interpolated frames at the display rate, which costs the most of any option), and raise `--audio-latency 80` if
the sound breaks up. `--headless --frames 6000` with `time` says how much faster than real time the machine is;
anything above about 1.5x has room to spare for a window.

**No Raspberry Pi has ever run this, and Linux has never been played by hand** — see "Status and known gaps".
`docs/PORTING.md` has a numbered test recipe for a Pi and what to report back.

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
| Delete | Cmd+Y | protect music on/off (see below) |
| C (a gamepad shoulder button) | same | the second fire button, the CDTV joypad's button B: the CDTV release's special weapon (the disk release never reads it) |

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

**`--protect-music`** (`protect_music = on`) is the optional cure, and it is **off by default because it is
not what the original does**. A sound effect takes a voice from the tune and keeps it for as long as the
effects keep coming, so holding fire through the fifteen-second passage a third of the way into the level-1
tune — where the two voices the effects take are the only ones playing — leaves nothing but shooting. Turn it
on and the emulator plays the missing voice itself, from the music driver's own copy of what it wanted to
play, so the tune carries on through the shooting. It is audio only: the game's state, its timing and every
frame of the picture are bit-identical either way, and the measurements are in §8 of the same document. The
cost is about 4 dB of volume, because one side of the mix can then carry three voices where the game only ever
drove two; F8 puts it back. `--sfx-volume` below 100 is the complementary knob — it turns the *shooting* down
rather than restoring the voice, and the two work together.

**Delete** (or **Cmd+Y** on a Mac, where Delete is fn+Backspace) switches it while you play, so you can hear
the difference in the same passage without restarting: the switch is faded over 20 ms so it does not click,
and your choice is remembered in `settings.cfg`. While it is on the window title says `[protect music]`, so a
screen recording shows which mode it was made in. With `--music cdtv` there is nothing to protect and the key
says so.

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

The interpreter build plays the game, and the recompiled build is bit-identical to it. The game has been played
through by hand from the title to the end of the last level on macOS, and played by hand on Windows. What has and
has not been checked is in "Status and known gaps" below.

The optional modernisation features are done (`docs/BUILDING.md`, "Picture"): filters, the Faithful switch,
interpolated high-frame-rate output, replacement asset packs (`docs/ASSET-PACKS.md`; no pack is shipped), a
widescreen surround of scenery and an edge-upscaled status panel. All are off by default.

Optional CDTV CD music (`docs/BUILDING.md`, "CDTV CD music"): with your own CDTV disc image
(`x2recomp --import <CDTV zip>`, which also stores the CDTV game's files for the CDTV release; about 680 MB of PCM
in the per-user data folder, or `--cdtv FILE.cue` to play the `.bin` files in place) `--music cdtv` plays the CDTV release's Bomb the Bass tracks for the title and each level
and mutes the tracker music while keeping the sound effects; `--music mix` plays both. Off by default; the disc
audio is never copied into the repository or shipped.

In the CDTV release itself the CD *is* the music, and it plays by default in a window when the CD tracks were imported (the same `x2recomp --import
<CDTV disc>` imports them; without them the game runs with silent music and says so). The program asks the CD for a track and polls once a
second, as on the original, so a finished track starts again at the next poll (up to a second of silence); the drive's own latency is not modelled.
The music key mutes and unmutes the CD. `docs/re-notes/cdtv-audio.md` has the track table and what was checked.

Cross-platform builds, CI and packages (Phase 7): see `CMakePresets.json`, `.github/workflows/ci.yml`,
`docs/PORTING.md` (platform status and Windows risks) and `docs/RELEASING.md` (version, macOS app, Linux tarball,
Windows zip). Release packages contain no game data.

## Status and known gaps

An honest account of what has been checked and what has not.

**Verified, and how:**

- **Every test passes** in the default interpreter build and with `-DX2_RECOMP=ON` (`ctest --test-dir build`;
  128 and 150 tests at the time of writing). The extra ones are the recompiler difftest and the replay-equivalence
  runs.
- **The recompiled and interpreted builds agree bit for bit** on every replay: the equivalence tests compare
  frame and audio hashes between the two executors, not against a stored expectation.
- **Replay coverage**: the title, all five levels, the shop, two-player mode, game over and the ending, each
  checked against recorded frame and audio hashes (`tests/replays/`).
- **Compared against a reference emulator**: pictures, palette, logic rate, stereo balance and game state
  were compared against Amiberry (a WinUAE port) running the original disks, from the title to 1,400 frames
  into level 1. Pictures match except for 0.06-0.11% of pixels, all in the twinkling starfield
  (`docs/re-notes/reference-compare.md`). Per-instruction and per-blit timing was calibrated the same way
  (`docs/re-notes/timing.md`).
- **Builds and passes its tests on every supported platform in CI** (repository hygiene, macOS arm64, macOS universal,
  macOS x86_64, Linux GCC, Linux Clang, Windows MSVC and Windows MinGW-w64; the six main jobs are required checks on
  `main`). **CI runs without game data**, so what this proves is that the code compiles and that every test not needing
  the game passes; the data-dependent tests report Skipped.
- **The disk importer** (0.2.0) is verified against real files, not only synthetic ones: IPF against the SPS preservation
  images of both retail pressings, DMS against 500 public archives, LHA against 490 members, flux against two public
  dumps, and the retail, Power Pack and 1762 imports against replay goldens (`docs/RELEASE-NOTES-0.2.0.md`, "What is
  verified, and how"). It is fuzzed and the new readers were reviewed for memory and decompression-bomb safety.
- **Played by hand, macOS:** the maintainer played the whole game through to the end with the autofire and god-mode
  assists on (`Assists and cheats` above), from the first level to the last. The game ends the way the original
  does: after the final shop it starts again from level 1, which the maintainer confirmed is how the original
  behaves. Playing it also found and fixed the macOS keyboard problems described in `CHANGELOG.md` (Ctrl+arrow
  triggering Spaces and Mission Control, F-keys taken by the OS, quick taps lost between frames).
- **Played by hand, Windows:** the maintainer played it in a window with sound on a Windows 11 PC, including
  the smooth high-frame-rate modes, and reports that it matches the macOS experience. That is a hands-on
  impression, not a measurement. The headless runs on the same machine are the measured evidence: the whole
  suite with real game data passes on MSVC and every frame and audio hash matches the macOS goldens
  (`docs/WINDOWS-VERIFICATION.md`).
- **Music that goes quiet while you fire is the original's behaviour.** Sound effects can only take two of the
  four voices, and the tune's quiet passages leave exactly those two carrying it. Amiberry and FS-UAE running the
  original disks both do the same (`docs/re-notes/music-dropouts.md`). The opt-in `--protect-music` plays the
  taken voice anyway; it is off by default because it is not the original's behaviour.
- **Run with the real game on**: macOS arm64, macOS x86_64 (under Rosetta 2), macOS universal, Windows x64
  (MSVC and MinGW-w64, headless and played by hand), and Linux arm64 with GCC 13 and Clang 18 in a container
  (headless). `docs/PORTING.md` has the full matrix and explains the difference between the two kinds of
  evidence.

**The CDTV release, and what has been checked of it:** it starts from your own disc image and plays on the interpreter and on the
translated executor. Checked by running it: the front end (joypad-mode page, title and guide pages, language and 1/2-player menus), all five
levels, the level-1 boss defeated by the game's own death code, the other bosses reached, the mid-level and level-5 shops, two players, game
over with the high-score entry, and the level-5 ending reached naturally, as 23 scripted replays (`tests/replays/cdtv/`) that reproduce
their recorded hashes on every run, on both executors. Level 1 and the front end were compared with an independent reference (the game under
the WHDLoad slave on an emulated A500: a median 99.9% of the pixels equal, the HUD identical, the game variables equal;
`docs/re-notes/cdtv-reference-whd.md`); the picture of the later levels, the speech and the audio mix have no such reference. The
modernisation options work on it as on the disk release (`docs/re-notes/cdtv-modern.md`). **Not checked:** a hand-played session on any
platform, the level-2 and -3 bosses and level 4's eyes and head defeated, speech and music by ear, and the CDTV remote (it never reaches
the game).

**Not verified:**

- **No comparison against real Amiga hardware.** Every timing claim is relative to a reference emulator, not
  to an A500.
- **Linux has never been played.** CI builds and tests it without data, and the game has been run headless with
  real data in clean Debian 12 containers (arm64 natively; x86_64 emulated, one replay), but nobody has seen a
  window or heard sound on any Linux machine.
- **No unassisted playthrough has been done.** The full play-through used autofire and god mode, so it shows the
  game can be completed end to end, not that it is balanced or beatable without assists. The scripted replays
  write coverage-only values into chip RAM to reach later stages quickly (`tests/replays/README.md`); in them
  only the level-1 boss is beaten, the other bosses are reached but not defeated.
- **Disc loads are instant**, where a real drive takes hundreds of frames. The game keeps calling its random
  generator while it waits, so after every level load the generator is thousands of calls behind where it
  would be on real hardware, and every random outcome after that point differs. This is a known, unfixed
  divergence. It also bounds the reference comparison: past the first disc load, a run can only be compared
  with the generator re-seeded to the reference's state (`tools/compare/resync_rng.py`, comparison only —
  the committed replays do not do this).
- **Drag-and-drop import is untested.** The first-run window offers it and the code path exists, but only
  `--import` on the command line has actually been exercised.
- **Gamepads are untested** on every platform, and so is SmartScreen behaviour for the unsigned Windows
  executable.
- **The HD status panel is an upscale, not a redraw**: it runs the original panel through the edge filter.
- **Widescreen is cosmetic.** The surround extends the scenery to the sides; the playfield, the collision
  geometry and everything the game can see are unchanged. It is not real widescreen.
- **No Raspberry Pi has run this**, and the Linux archive's speed on Pi hardware is unmeasured. What is
  checked is that the packaged binary starts in a clean Debian 12 container with nothing installed and
  reproduces three replays' hashes from real game data (`docs/PORTING.md`, "Raspberry Pi").
- **No real dump of the retail first disk has been imported yet.** The retail import is verified against IPF
  preservation images; the flux readers against other Amiga disks. A KryoFlux or DrawBridge dump of your own disks
  is the first real test, and `docs/DUMPING.md` says what a good one looks like. HFE v3 is untested.
- **The crack, Power Pack and 1762 variants are not the original's gameplay in every respect:** the trainer builds
  change it on purpose, the Power Pack's samples are lower precision, and the 1762 build's recorded runs differ from
  the canonical ones early on (not investigated). **Windows executables are built by CI and by a Windows PC that is
  sometimes offline**; the 0.2.0 Windows archive has not been run on a machine without Visual Studio.

**Open decisions:**

- Nothing outstanding. The licence is settled (GPL-3.0-or-later, see above), and the one loose end that
  remained — whether SoftFloat's indemnification wording sits comfortably under the GPL — was closed by
  removing SoftFloat from the build entirely (`docs/PROVENANCE.md` §7.3).

Reverse-engineering notes are in `docs/re-notes/` (see `docs/re-notes/README.md` for an index). The version
history is in `CHANGELOG.md`, and the notes for this version in `docs/RELEASE-NOTES-0.2.0.md` (0.1.0's are in `docs/RELEASE-NOTES-0.1.0.md`).
