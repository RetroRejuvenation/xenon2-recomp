# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

This project contains no game data. See `README.md` for the legal position.

## [Unreleased]

### Added

- **`LICENSE`: the project is now GPL-3.0-or-later.** The accurate-timing code contains transliterations of
  GPL code from WinUAE/Amiberry, so a permissive licence was not available. Every original source file
  carries an `SPDX-License-Identifier: GPL-3.0-or-later` header.
- `docs/PROVENANCE.md`: a file-by-file record of which code was written from external sources, how closely it
  follows them, and the licence reasoning behind the GPL choice (§7), including the per-file licence
  statements found in the reference sources and the residual uncertainties.
- `THIRD-PARTY-NOTICES.md`: the bundled, vendored and linked third-party components and their licences.
  Installed and packaged alongside `README.md`.
- `docs/re-notes/music-dropouts.md`: why the music seems to stop sometimes. The answer is that it is
  authentic: the one tune has long rests in which only AUD1 and AUD2 play, and those are exactly the two
  channels the sound effects take, so holding fire through the 14.8 s rest in level 1 leaves nothing audible
  but shooting. Every rest and every music/effect channel handover appears at the same frame in the Amiberry
  reference captures (left-side envelope correlation 0.996 on `two_player`), so no emulation change was
  needed. The note also records every way the game can silence the music, corrects `$1A584` from "tempo" to
  "master volume" and `$19FE4` from "probably a fade" to "end the running effects and hand the channels back",
  and lists the live-output paths that can still cause a real dropout. Its section 7 answers the reporter's
  follow-up — that the music is silenced for as long as they keep firing — with the channel-allocation rule:
  the effect command's high byte is the voice, so effects can only ever take AUD1 or AUD2 (measured: 0 of
  12,000 frames for AUD0 and AUD3); a new command reloads the effect's duration counter at `$1B4C6` without
  handing the voice back, so chained effects hold it with no gap at all — 99.8 % of a firing block in one
  unbroken 20.47 s run when fire is tapped at 4-frame intervals, against 84 % in 288 brief holds at the game's
  own 24-frame rate. Two more Amiberry captures of the same 20 s-firing / 10 s-quiet cycle match ours to
  within a percent on both sides (envelope correlation 0.998 left, 0.994 right), so that is authentic too.
- `tools/audio/music_dropouts.py`: lists the music-silent intervals of any replay, with the game state and
  driver calls around each one, and with `--own` which voices the sound effects held and for how long without
  a break. CTest `music_gaps` pins both reference-confirmed shapes: the tune's rests, and the rule that effects
  never take AUD0 or AUD3 while AUD2 is held for most of a firing block.
- `--audio-latency MS` (settings key `audio_latency`, 10-500, default 40): the live output's target buffer,
  which is also how long the host loop may stall before the sound breaks up. The window now also says so, at
  most once a second, when the buffer does run dry, so a host-side dropout is no longer mistaken for the
  game's own silence. The emulation, `--audio-out` and every replay and golden hash are unaffected.
- `docs/re-notes/README.md`: an index of the reverse-engineering notes.
- `docs/RELEASE-NOTES-0.1.0.md`.
- README: a "Status and known gaps" section separating what has been verified, and how, from what has not.
- **Autofire** (`--autofire[=RATE]`, `autofire` / `autofire_rate` in `settings.cfg`, Tab or Cmd+T), an
  assist rather than a cheat, so high scores are still saved. The game latches a fire *press* as an edge
  (`$2144`-`$214A` sets `$A01` only on a not-down-to-down transition) and the weapon fires again only on a
  fresh press, so **holding** fire gives about 5 shots per 400 frames while tapping gives about 60 — measured
  by counting the shot sound `$0289` with `--audio-cues`. The default rate, 17 pulses a second (one every
  third emulated frame), sits exactly on the game's own ceiling of one shot per 6.6 frames. It only engages
  during a level (the ship count `$CCA` is 0 at the title and in the SELECT menu) and only after fire has
  been held for about 0.3 s, so the menus, the shop and the continue prompt — which read the same press as a
  selection — are untouched, and a tap is always delivered exactly as it came in.
- **God mode** (`--god-mode`, `god_mode` in `settings.cfg`, Backspace or Cmd+I), a cheat: the ship's energy
  `$CC6` is refilled to `$27` every frame, so damage never kills it. This refills rather than patching the
  damage routine the way the `[t+3]` crack's trainer did (`bhi.b` to `bra.b` at `$5D84`), which still ran
  `sub.w d0,$CC6` first and left the energy word at zero or negative with the HUD bar drained; refilling
  keeps the energy display full and correct and the hit flash, sound and shield behaviour unchanged, and
  modifies no game code. It does not cover the game's one non-energy death, the `$42E` grab sequence at
  `$5FE0` that calls the death routine directly — documented, not papered over.
- **Starting credits** (`--credits N`, `credits` in `settings.cfg`; Insert or Cmd+0 adds one mid-game). The
  game has no credits option — `options_menu` `$8CB6` just sets 3, and the SELECT menu only offers 1P/2P and
  music on/off — and its display (`$8F56`) pokes a single ASCII digit into the "CREDITS n" string, so 9 is
  the highest value that can be drawn and that is the cap. `--credits` is applied once, as a new game starts,
  so a continue still costs a credit.
- Cheats are visibly flagged and do not quietly inflate the high-score table: the window title gains
  `[CHEAT: god mode]` or `[CHEAT used]`, each toggle prints a line, and a session that used one does not write
  `highscores.bin` (the in-game table still shows the score; `cheat_scores = on` saves anyway).
- `ctest -R cheats_god_mode`: runs `tests/replays/game_over.txt`, whose pokes exist purely to kill the ship,
  first unchanged — it must still reach GAME OVER with ships and credits at 0 — and then with `--god-mode`,
  where nothing may die and the energy must still be full. Skipped without game data.
- The mouse cursor is hidden while the window has the focus and the game is running, and shown again while
  frozen, on focus loss and at exit (`cursor = auto|show|hide` in `settings.cfg`, `--cursor MODE`). The game
  has no mouse input — SDL's mouse events are never read, so no click or movement can reach it — so the arrow
  was only ever in the way. Implemented with `SDL_HideCursor()`, which merely stops drawing the pointer;
  `SDL_SetWindowRelativeMouseMode()`, which would also confine and capture it, is deliberately not used. The
  decision lives in a pure `CursorPolicy` class with unit tests, and `--input-debug` logs each change.
- `x2recomp --list-keys`: prints the key bindings this machine will use, including anything `input.cfg`
  changes, and exits without touching the file.
- `x2recomp --input-debug` (`input_debug` in `settings.cfg`): logs every keyboard and focus event with a
  timestamp, scancode, keycode, modifiers, repeat flag and the action or Amiga raw key it mapped to, every
  gamepad button, and the emulated frame that carried each joystick change. A key the OS swallowed produces
  no line at all, which tells a lost keypress apart from a dropped one.
- `--grab-keyboard` / `--no-grab-keyboard`, `grab_keyboard = auto|on|off` in `settings.cfg`, and a Scroll
  Lock / Cmd+G hotkey: ask the platform to route its own keyboard shortcuts to the window while it has the
  focus. Default off, released on focus loss, while frozen and on exit. It does nothing on macOS — SDL's
  Cocoa backend only silences the global hotkeys when SDL itself is built with `SDL_MAC_NO_SANDBOX`, which no
  released package does — and that is documented rather than papered over.
- `input.cfg` version 3 adds the `autofire`, `god_mode` and `add_credit` actions. The migration now knows
  every earlier set of defaults it wrote, not just the first, so a file written by the version that fixed the
  macOS bindings picks the new actions up instead of being mistaken for one the user had edited. A file is
  only ever compared with the defaults of the version it declares — we never wrote any other combination, so
  anything else was hand written and is left alone — and a file missing the new actions simply gets their
  defaults.
- `input.cfg` now carries a `# version:` marker and is migrated on upgrade: a file you have edited is used
  exactly as it is and never rewritten, while a file holding nothing but an older version's defaults is
  replaced with the current ones and kept as `input.cfg.bak`. Without this, a Mac that already had
  `p2_fire = Left Ctrl, Right Ctrl, Z` on disk would have kept the broken bindings forever.

### Changed

- `.github/workflows/ci.yml`: `push` and `pull_request` now carry `paths-ignore` for documentation-only
  changes (`**.md`, `docs/**`, `LICENSE`, `_specs/**`), so a docs commit no longer spends macOS and Windows
  minutes on this private repository. `workflow_dispatch` is unaffected, and any non-docs change still runs
  every job including hygiene.
- `docs/PORTING.md`, `README.md`, `docs/RELEASE-NOTES-0.1.0.md` and `docs/RELEASING.md` now state the real CI
  status and separate "builds and passes the no-data suite in CI" from "has been run with the real game",
  which are not the same claim.
- Packages now carry `LICENSE`, `THIRD-PARTY-NOTICES.md` and `PROVENANCE.md` next to `README.md`, plus
  `licenses/README.txt` indexing the third-party texts. `CPACK_RESOURCE_FILE_LICENSE` is set.
- `_specs/xenon2-recomp.md`: the open licensing question is annotated as answered (2026-10-30); the rest of
  the spec is left as a historical record.
- `README.md`, `docs/BUILDING.md`, `docs/RELEASING.md`, `docs/PORTING.md` and
  `docs/RELEASE-NOTES-0.1.0.md` state the licence instead of saying it is undecided. `docs/PORTING.md` also
  notes that a port carries the licence and that new files need the SPDX header.
- `tools/check_no_game_data.py`: deny list extended with further floppy-image, picture, audio, tracker-music
  and release-archive extensions; one more self-test case.
- README: the controls table now lists the port-1 joystick keys and the scale and volume hotkeys.
- `docs/BUILDING.md`: corrected a stale note that audio had not been compared with a reference emulator — it
  was, in Phase 4; only real A500 hardware remains uncompared. Added `vendor/musashi/softfloat` to the
  vendored-code table — it was linked in but undocumented.

### Fixed

- **macOS: the keyboard defaults fought the operating system.** `p2_fire` was bound to Left/Right Ctrl, and
  Ctrl with an arrow key is a system-wide shortcut on macOS (Ctrl+Left/Right move between Spaces, Ctrl+Up
  opens Mission Control, Ctrl+Down App Expose), so firing while steering moved windows between screens and
  changed the focus instead of shooting. On macOS fire is now Z, X or either Option key and no Ctrl key is
  bound at all; Windows and Linux keep Ctrl, where it collides with nothing. Space is still deliberately
  unbound — the game reads it itself. A unit test asserts the platform defaults bind neither Ctrl (on
  macOS) nor any key the game reads.
- **macOS: host actions on F-keys the operating system claims first.** Without "Use F1, F2, etc. keys as
  standard function keys" the OS takes F3, F4, F5, F6 and F7-F12 and the keypress never reaches the game,
  which read as a key needing a second press. Every host action now also has a Cmd combination that the
  system does not claim: Cmd+1..Cmd+9 for F1..F9, Cmd+F fullscreen, Cmd+R reset, Cmd+U/E/D for the asset
  packs, surround and HD panel, Cmd+K the music source and Cmd+G the keyboard grab. Cmd+Q, Cmd+H, Cmd+M,
  Cmd+W and Ctrl+Cmd+F are left to the application menu SDL installs.
- **A key tapped between two emulated frames was dropped.** The joystick was sampled once per emulated
  frame from the set of currently held keys, so a press and release that both arrived in one SDL event batch
  left no trace — the other half of "sometimes needs a second press". A press is now latched and delivered
  for one emulated frame even if the key is already up, for keyboard and gamepad buttons alike. The latch
  lives only in the live SDL path, so scripted input, replays and golden hashes are untouched.
- Losing the window focus now drops the held keys and sends the game a release for any Amiga key it was told
  was down, so a key held while Cmd+Tabbing away no longer sticks.
- macOS: `SDL_MAC_PRESS_AND_HOLD=0` and `SDL_MAC_OPTION_AS_ALT=both` are set before the window opens, so
  holding a letter key repeats it instead of suppressing the repeat and offering the accent menu (what macOS
  does by default for A, S, Z and the rest — steering and fire keys here, not text), and the Option fire keys
  cannot start a dead-key composition. Either environment variable still overrides the default.
- **`input.cfg` could not always be read back after it was written.** SDL keeps a single mutable
  scancode-name table and each video backend renames entries in it at init — macOS turns "Left Alt" into
  "Left Option", Windows "Left GUI" into "Left Windows", X11 and Wayland "Application" into "Menu" — and
  `SDL_GetScancodeFromName` searches that same table. So the file the game wrote with a window open said
  `p2_fire = Z, X, Left Option, Right Option`, which `--list-keys` (which never calls `SDL_Init`) rejected as
  two unknown keys, and a file written on one platform could not be read on another. The file now always
  carries SDL's platform-independent spelling, and reading accepts every platform's spelling, so existing
  files keep working either way. A test pins both directions.
- `--list-keys` now says what the next start will do with the file instead of silently showing bindings that
  are about to be replaced: a file still holding an older version's defaults prints "this file holds the old
  defaults, so the game will upgrade it to version N on its next start" followed by the bindings that *will*
  apply. It still reads and writes nothing. The listing also notes that "Left Alt" and "Right Alt" are the
  Option keys on a Mac keyboard.
- Windows and Linux now list **Z and X first** among the fire keys (Ctrl stays bound; the macOS bindings are
  unchanged). Windows' "Show location of pointer when I press the CTRL key" accessibility setting (Control
  Panel > Mouse > Pointer Options) draws a ring at the pointer on every Ctrl press, which is constant while
  firing. That is the operating system, not the game, and the documentation now names the setting and says how
  to switch it off; binding a second Ctrl-free fire key means it can also just be avoided. Whether hiding the
  cursor suppresses the ring is untested and not claimed.
- **MSVC 2022 at `/O2` miscompiled Musashi's opcode-table generator**, dropping bit 14 from every
  generated mask and match (`0xf000` as `0xb000`). That crashed `m68ki_build_opcode_table()` — an unbounded
  loop walked off the end of the handler table — in every MSVC test that builds a `Machine`, and would
  otherwise have decoded every opcode against the wrong mask. Fixed in two vendored files:
  `vendor/musashi/m68kmake.c` computes each mask and match bit by bit instead of shifting a comparison, and
  `vendor/musashi/m68k_in.c` walks the handler table by each entry's own free bits instead of a hard-coded
  chain of unbounded mask-group loops. Both are marked in place with an `xenon2-recomp:` comment. **Musashi
  is therefore no longer a verbatim upstream copy** — and documenting this turned up a third, older local
  change that had never been recorded as one: `vendor/musashi/m68kcpu.h` uses `_setjmp` on macOS/BSD
  (commit `a0f4dcb`). All three are now listed in `docs/PROVENANCE.md` §9. Musashi's MIT-style licence
  permits modification and its notice is retained (`docs/PORTING.md` has the MSVC detail). The generated
  `m68kops.c` is byte-identical on macOS arm64 (md5 `b94fcb0a43c49d6237798bcb062dfac9`), and all replay and golden hashes
  are unchanged.

### Removed

- **SoftFloat is no longer compiled, linked or shipped.** Musashi can use it for its FPU, but this game is a
  68000 with no FPU, and one paragraph of SoftFloat's notice imposes an indemnification obligation that reads
  awkwardly under the GPL. `runtime/musashi_no_fpu.c` replaces `softfloat.c` in the `musashi` target,
  defining only the 15 symbols `m68kfpu.c` names; they are unreachable because Musashi enters its FPU only
  for 68030+ CPU types. Verified by `nm`: all 88 symbols unique to SoftFloat's implementation are gone from
  the binary (142 SoftFloat symbols before). No vendored file was edited, and the vendored headers stay
  because `m68kcpu.h` requires them. `packaging/licenses/SoftFloat-NOTICE.txt` removed, since no SoftFloat
  code is in a package. All replay and golden hashes unchanged.
- `packaging/LICENSE-TODO.txt`, the placeholder that stood in for a licence; `LICENSE` replaces it in
  packages.

## [0.1.0] — unreleased

First collected version. The history below is grouped by the development phase that produced it; these were
never released separately.

### Added

#### Analysis (phases 0-2)

- Specification, the reverse-engineering notes in `docs/re-notes/`, and the analysis tools in `tools/`:
  ADF and AmigaDOS hunk readers, the disk depackers, a capstone-based 68000 recursive-descent analyser and a
  disk-image catalogue.
- A Ghidra headless code map of the resident game image and the six level modules
  (`tools/ghidra/`, `docs/re-notes/ghidra-code-map.md`).

#### The runtime (phase 3)

- Headless runtime core: the Musashi 68000 interpreter on a PAL Amiga 500 machine model — custom chip
  registers, copper, blitter, CIAs, interrupts and a disk shim.
- Paula audio: four DMA channels, the A500/A1200 output filter models, a band-limited resampler, and WAV or
  SDL3 output.
- Scanline renderer, SDL3 window, keyboard and gamepad input, blitter line mode.
- Device-clocked frame pacing in the window, with pause and reset flushing the audio queue.

#### Reference comparison (phase 4)

- An Amiberry harness (`tools/compare/amiberry-harness/`) and the comparison tooling behind
  `docs/re-notes/reference-compare.md`: pictures, palette, logic rate, stereo balance and game state against
  a reference emulator running the original disks.

#### Static recompiler (phase 5)

- The recompiler (`tools/recomp/recomp.py`) and the hybrid executor: the game's 68000 code translated to C++
  that is exactly equivalent to the interpreter, with `-DX2_RECOMP=ON`.
- Execution tracer (`runtime/trace/`), the replay corpus, and translation seeded from trace coverage.
- Per-replay interpreter-versus-hybrid equivalence tests.

#### Playability and import (phase 6a-6b)

- Native disk-image import (ADF, ADZ, ZIP or a folder) with a first-run import screen and drag-and-drop,
  writing to a per-user data directory.
- Persistent high scores and settings; volume and window-scale hotkeys.
- Boss-defeat and level-transition replays.

#### Accurate timing (phase 6c-6d)

- The accurate OCS bus model: blitter cycle sequencing, fixed DMA slots, CPU wait states and 313-line frames
  (`--timing accurate`, the default; `--timing fast` for the old zero-cost model).
- Copper DMA slots, the interrupt exception's bus-cycle placement, and MOVEM's extra read.
- `docs/re-notes/timing.md`, and timing-comparison tooling against the reference harness.

#### Portability, CI and packaging (phase 7)

- Portable build for macOS, Linux and Windows, with `CMakePresets.json`, a project version, and the
  `check_no_game_data` hygiene check.
- GitHub Actions CI covering macOS arm64/universal/x86_64, Ubuntu with GCC and Clang, and Windows with MSVC
  and MinGW-w64, plus a repository hygiene job. All eight jobs pass without game data (run `41041824614`,
  commit `95d9d78`); the data-dependent job is manual and self-hosted.
- Release packaging: a macOS `.app` in a zip or dmg, a Linux tarball and a Windows zip, each with `LICENSE`,
  the third-party licence texts and `SHA256SUMS.txt` (`tools/package.sh`, `tools/package.ps1`,
  `docs/RELEASING.md`).

#### Optional CDTV CD music (phase 8)

- A CD-audio music layer that plays the CDTV release's tracks in place of the tracker music, from your own
  CDTV disc image. Off by default (`--music cdtv` / `--music mix`).

#### Optional modernisation (phase 9)

- Window filters (nearest, sharp, scanlines, crt, edge) and the Faithful switch.
- Interpolated presentation at the display's refresh rate, from captured draw commands.
- Replacement asset packs on an RGBA-composited canvas, plus `--dump-assets` for pack authors
  (`docs/ASSET-PACKS.md`).
- Widescreen surround (16:10, 16:9, 21:9, 32:9) and an edge-upscaled HD status panel.

All modernisation features are off by default and none change game logic, audio or the faithful picture.

### Known limitations

See "Status and known gaps" in `README.md`. In summary: no comparison against real Amiga hardware; Windows
never built; CI never run; disc loads are instant, which diverges the game's random generator after every
level load. The licence question is now settled (GPL-3.0-or-later); the one open point is whether
SoftFloat's indemnification wording sits comfortably under the GPL (`docs/PROVENANCE.md` §7.3).
