# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

This project contains no game data. See `README.md` for the legal position.

## [Unreleased]

### Added

- `docs/PROVENANCE.md`: a file-by-file record of which code was written from external sources, how closely it
  follows them, and what that means for the project's licence choice.
- `THIRD-PARTY-NOTICES.md`: the bundled, vendored and linked third-party components and their licences.
  Installed and packaged alongside `README.md`.
- `docs/re-notes/README.md`: an index of the reverse-engineering notes.
- `docs/RELEASE-NOTES-0.1.0.md`.
- README: a "Status and known gaps" section separating what has been verified, and how, from what has not.

### Changed

- `tools/check_no_game_data.py`: deny list extended with further floppy-image, picture, audio, tracker-music
  and release-archive extensions; one more self-test case.
- README: the controls table now lists the port-1 joystick keys and the scale and volume hotkeys.
- `docs/BUILDING.md`: corrected a stale note that audio had not been compared with a reference emulator — it
  was, in Phase 4; only real A500 hardware remains uncompared.

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
  and MinGW. **Never executed yet.**
- Release packaging: a macOS `.app` in a zip or dmg, a Linux tarball and a Windows zip, each with the licence
  texts and `SHA256SUMS.txt` (`tools/package.sh`, `tools/package.ps1`, `docs/RELEASING.md`).

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
level load; the project's licence is not yet chosen.
