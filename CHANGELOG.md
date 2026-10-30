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
- `docs/re-notes/README.md`: an index of the reverse-engineering notes.
- `docs/RELEASE-NOTES-0.1.0.md`.
- README: a "Status and known gaps" section separating what has been verified, and how, from what has not.

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
