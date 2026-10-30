# Porting and platform status

One codebase builds on macOS, Linux and Windows with CMake 3.29 or newer, a C++20 compiler and SDL3. This page
covers three things:

- which platforms have been built and tested;
- the portability rules the code follows;
- the risks that remain on platforms that have not been run.

`docs/BUILDING.md` covers building and running; `docs/RELEASING.md` covers packages.

Everything here is GPL-3.0-or-later (`LICENSE`), so a port carries that licence too: if you distribute a
binary for a new platform, ship the source with it. Every original source file carries an
`SPDX-License-Identifier: GPL-3.0-or-later` header — keep it when you add files. Third-party components keep
their own licences (`THIRD-PARTY-NOTICES.md`), and a `-DX2_RECOMP=ON` build is not redistributable because it
contains code translated from your game image.

One portability note follows from that: `runtime/musashi_no_fpu.c` replaces Musashi's SoftFloat, so the
`musashi` target must keep it in place of `vendor/musashi/softfloat/softfloat.c` on every platform. It is
plain C99 with no platform dependencies, so no port should need to touch it.

## Platform status

| Platform | Compiler | SDL3 | Built | Tests run | How |
|---|---|---|---|---|---|
| macOS arm64 | AppleClang | Homebrew 3.4.18 | yes | full suite with game data: 60/60 interpreter build, 80/80 `-DX2_RECOMP=ON` | developer machine |
| macOS x86_64 | AppleClang | pinned 3.4.18 | yes | full suite with game data, interpreter build, 60/60 under Rosetta 2 | `macos-x86_64` preset |
| macOS universal | AppleClang | pinned 3.4.18 | yes | suite with game data on the arm64 slice; packaged app's level1_play replay on both slices | `macos-universal` preset |
| Linux arm64, Ubuntu 24.04 | GCC 13.3 | pinned 3.4.18 | yes | game data: 60/60 interpreter build, 80/80 `-DX2_RECOMP=ON` | Docker container |
| Linux arm64, Ubuntu 24.04 | Clang 18.1 | pinned 3.4.18 | yes | game data: 60/60 | Docker container |
| Linux x86_64 | GCC, Clang | pinned 3.4.18 | in CI config only | CI (no game data) | `.github/workflows/ci.yml` |
| Windows x64 | MinGW-w64 GCC 13, cross | pinned 3.4.18 | yes, both builds | none: not run | Ubuntu cross-compiler in Docker |
| Windows x64 | MinGW-w64 GCC, MSYS2 | pinned 3.4.18 | in CI config only, may fail | CI (no game data) | `windows-mingw` preset |
| Windows x64 | MSVC 2022 | pinned 3.4.18 | in CI config only | CI (no game data) | `windows-msvc` preset |

The MSVC row is being worked on separately. What is already settled is that MSVC 2022 at `/O2` miscompiled
Musashi's table generator, which crashed every test that builds a `Machine`; that is fixed in two vendored
files and written up under "MSVC: the opcode-table miscompilation" below. Whether the suite now passes on
MSVC is not recorded here yet.

Notes on the Linux runs:

- `cdtv_game` was Skipped, because the CDTV image was not copied into the container.
- `check_no_game_data` was Skipped, because the container has no git checkout.

Two problems were fixed after the Linux runs found them:

- Clang with libstdc++ rejected a `std::pair` of an incomplete type in the asset-pack JSON parser.
- CMake's C++20 module scan needs `clang-scan-deps`, which Ubuntu's clang package does not include. Module
  scanning is now off.

The MinGW cross-build produces `x2recomp.exe`, which imports only `KERNEL32.dll`, `msvcrt.dll` and `SDL3.dll`,
and an install tree. Neither has been run on Windows or under Wine.

"In CI config only" means the job is defined in `.github/workflows/ci.yml` but has never run: GitHub Actions
cannot be run locally. The workflow was checked only as YAML with Python's `yaml` module. MSVC has only been
reviewed, never compiled (see "Windows: residual risks"). Do not claim Windows support until CI or a Windows
machine has built and tested it.

Every frame, audio and presenter hash of the replay corpus matches the recorded goldens on these platforms:

- macOS arm64;
- macOS x86_64;
- Linux arm64 with GCC;
- Linux arm64 with Clang.

The recompiled build matches the interpreter on macOS arm64 and on Linux with GCC.

## Running the data-dependent tests

CI has no game data. Without it, every test that needs data exits with 77 and CTest reports it as **Skipped**,
not Passed. CI checks that `game_boot`, `golden_frames` and `replay_level1_play` are Skipped. To run the full
suite, use your own data on your own machine:

1. Import your disk images once: `x2recomp --import Xenon2.zip` (or `tools/build_all.sh Xenon2.zip`). Then
   either point CMake at the import:
   `cmake -S . -B build -DX2_ASSETS_DIR="$(build/x2recomp --print-data-dir)assets"`, or run
   `python3 -I tools/prep_assets.py` to fill `generated/assets/`, the default `X2_ASSETS_DIR`.
2. The importer test (`game_import`) looks for `Xenon2.zip` and `Xenon-2-Megablast_Amiga_EN.zip` in the source
   directory (`-DX2_DISK_ZIPS=...`). The CDTV test (`cdtv_game`) looks for the CDTV disc image
   (`-DX2_CDTV_ZIP=...`) and imports it into `X2_CDTV_TEST_DIR` (about 680 MB).
3. Run `ctest --test-dir build`. Then do the same for the recompiled build: `-DX2_RECOMP=ON`, after
   `python3 -I tools/recomp/recomp.py`, or use `tools/build_all.sh --recomp --test`.

The `data-tests` job in `.github/workflows/ci.yml` runs the same steps on a self-hosted runner, where a
maintainer has imported their own data. It must be started by hand and takes the assets directory as an input.
It uses no secrets and downloads no data, and nothing derived from the data is uploaded. Never put game data,
or anything that decrypts or downloads it, into CI.

## Portability rules followed by the code

- **No shifting the result of a comparison.** Write `if (cond) x |= bit;`, not
  `x |= (cond) << n;` — especially when `n` is loop-dependent and `x` is narrower than `int`. MSVC 2022 at
  `/O2` miscompiled exactly that idiom in Musashi's table generator and silently produced wrong opcode masks;
  see "MSVC: the opcode-table miscompilation" below. Nothing in this project's own code uses the pattern.
- **No unbounded walks over a table that another tool generates.** If the generator is wrong, an unbounded
  `while (entry->field == literal)` reads past the end of the array instead of failing. The same section
  covers the one place this bit us.
- **Endianness.** The 68000 side reads all big-endian data through explicit byte shifts (machine model,
  importer, high-score file). The RGBA framebuffers pack pixels into `uint32_t` as `R | G << 8 | B << 16 |
  A << 24` and read them back as bytes. That needs a little-endian host. `runtime/gfx/video.h` checks this with a
  `static_assert` (`std::endian`). All supported hosts (x86-64, arm64) are little-endian. CD audio PCM on disk is
  little-endian and is decoded byte by byte.
- **Integer sizes.** Fixed-width types are used throughout. `long` is used only for small values (`ftell` of
  files under 1 MB, `strtol` results, iterator offsets), so the 32-bit `long` on Windows (LLP64) is harmless.
  Every `printf` of a 64-bit value casts it to `unsigned long long` and prints it with `%llu`/`%llx`.
- **setjmp.** Musashi's bus-error trap and the recompiled executor use `m68ki_check_bus_error_trap()`. That is
  `_setjmp` on macOS and the BSDs, to avoid two signal-mask system calls per CPU slice, and plain `setjmp`
  elsewhere. glibc's `setjmp` does not save the signal mask. MSVC and MinGW have no one-argument `_setjmp`.
  Bus errors are never raised in this game (no `m68k_pulse_bus_error`, no PMMU). This is one of the three
  local modifications to vendored Musashi (`docs/PROVENANCE.md` §9).
- **Generated code (`-DX2_RECOMP=ON`).** `runtime/recomp/x2r_runtime.h` uses GNU statement expressions and
  `always_inline` on GCC and Clang. On MSVC it uses immediately invoked lambdas and `__forceinline` instead,
  with the same evaluation order. The lambda path can be tested anywhere with
  `-DCMAKE_CXX_FLAGS=-DX2R_NO_STMT_EXPR`. It passed `recomp_difftest`, `recomp_equivalence` and every
  `recomp_replay_*` test on macOS arm64. The generated sources are already split into files of about 2,500
  instructions (`PART_INSNS` in `tools/recomp/recomp.py`, about 0.9 MB each). They are compiled without warnings
  (`-w` / `/w`), and with `/bigobj` on MSVC.
- **GNU extensions in the runtime.** There are none: no VLAs, `__builtin_*`, `__attribute__` (except the
  `#if`-guarded one above), `ssize_t`, `strcasecmp`, `usleep`, POSIX headers, `fork`, `popen`, `mmap` or signals.
  The tests use `std::numbers::pi` (C++20) instead of `M_PI`.
- **Files.** Binary data is opened with `"rb"`/`"wb"` or `std::ios::binary`. Text-mode files are only logs,
  `settings.cfg`, `input.cfg` and manifests, which are read back line by line, so CRLF on Windows does not
  matter. Paths are built with `std::filesystem` or `/`, which Windows accepts.
- **Per-user data directory.** In order of use:
  1. `--data-dir`;
  2. `SDL_GetPrefPath("xenon2-recomp", "x2recomp")`;
  3. without SDL, the same layout by hand: `%APPDATA%\xenon2-recomp\x2recomp\`,
     `~/Library/Application Support/...` or `$XDG_DATA_HOME` / `~/.local/share/...`.

  `x2recomp --print-data-dir` prints the directory for scripts (`tools/build_all.sh` and `tools/build_all.ps1`
  use it).
- **Python tools** run as `python3 -I` on macOS and Linux and `py -3 -I` on Windows. `tools/recomp/recomp.py`
  needs a GCC-style host compiler (`CC=cc`, `gcc` or `clang`), not MSVC's `cl`. It appends `.exe` to the helper
  programs on Windows.
- **Cross-compiling.** `m68kmake` generates Musashi's opcode tables at build time. When cross-compiling, build it
  for the host first and pass `-DX2_M68KMAKE=/path/to/m68kmake`, or set `CMAKE_CROSSCOMPILING_EMULATOR`.

## Warnings

The project's own code is compiled with `-Wall -Wextra` (GCC, Clang) or `/W3` (MSVC). Vendored Musashi and the
generated code are compiled without warnings, and Musashi's headers are included as system headers. GCC 13
(Linux and MinGW), AppleClang and Clang 18 report no warnings.

AppleClang and Clang 18 used to report one `-Wtautological-bitwise-compare` in
`runtime/machine_timing.cpp`: the mask of the "MOVEM to registers" extra read was `0xFB80`, which cleared the
direction bit and never matched, so the read was never charged. The mask is now `0xFF80`, applied in Phase 6d
together with the other fine-timing fixes so the goldens were re-recorded once (`docs/re-notes/timing.md` §9.4).

## MSVC: the opcode-table miscompilation

The first MSVC build did not merely fail to compile — it crashed, and the cause is worth recording because the
fix lives in vendored code and the failure mode was quiet.

**Symptom.** An access violation in every MSVC test that constructs a `Machine`, inside
`m68ki_build_opcode_table()`.

**How it was found.** From CI crash diagnostics rather than a debugger: the generated handler table was dumped
and its mask groups counted, then compared against a macOS build's. The group *counts* matched, but every mask
and match had bit 14 clear — `0xf000` appeared as `0xb000`, `0xff00` as `0xbf00`, `0xffff` as `0xbfff`, and
`bra.8`'s match `0x6000` as `0x2000`. A single wrong bit across every entry points at one expression, not at
table data.

**Cause.** `populate_table()` in `vendor/musashi/m68kmake.c` built each mask and match with

```c
op->op_mask |= (bitpattern[i] != '.') << (15-i);
```

Bit 14 is the `i == 1` term, and MSVC 2022 at `/O2` dropped exactly that term. Shifting the 0-or-1 result of a
comparison by a loop-dependent amount, straight into an `unsigned short` bitfield, is an expression a compiler
may reassociate freely.

**Two consequences, the second worse than the first.** The missing bit removed the whole `mask == 0xff00`
group that `m68ki_build_opcode_table()`'s first loop scans for; that loop is `while(ostruct->mask == 0xff00)`
with no bound, so it ran off the end of the array and the process died. Fixing only the crash would have left
**every opcode decoded against the wrong mask**, so an MSVC build that merely started would have run the wrong
instruction handlers.

**Fix**, in two vendored files, both marked in place with an `xenon2-recomp:` comment
(`docs/PROVENANCE.md` §9):

- `m68kmake.c`: name the bit and branch, leaving the optimiser nothing to reassociate.
- `m68k_in.c`: walk the handler table by each entry's own free bits instead of a hard-coded chain of unbounded
  mask-group loops, so a missing or reordered group can no longer walk off the end.

The generated `m68kops.c` is byte-identical before and after on macOS arm64 (md5
`b94fcb0a43c49d6237798bcb062dfac9`), so no other platform's behaviour changed.

**The rule this leaves behind:** avoid shifting the result of a comparison, especially by a loop-dependent
amount into a narrow type. Write `if (cond) x |= bit;` instead. It is no longer about MSVC specifically — the
idiom is simply harder for a compiler to get right than it looks, and a wrong answer here is silent.

## Windows: residual risks

These have been reviewed in the code but not compiled or run:

1. **Barely compiled with MSVC.** The MSVC build now gets as far as running tests (see above), but the port is
   not finished. The code avoids GNU extensions, but MSVC may still reject something that Clang and GCC accept,
   such as header transitivity (`<array>`, `<cstring>` and others that happen to be included indirectly on
   libc++ and libstdc++), conversion warnings that become errors under `/permissive-`, or `constexpr`
   differences. Expect more one-line fixes.
2. **Musashi under MSVC C.** Upstream Musashi claims MSVC support, and `m68kmake.c` uses only standard C — but
   see the miscompilation above: "standard C" was not enough. Treat the generated tables as something to verify
   on each new toolchain, not to assume.
3. **Non-ASCII paths.** Paths are kept as UTF-8 `std::string`s, which is what SDL returns. `fopen` and
   `std::filesystem::path(std::string)` on Windows read narrow strings in the ANSI code page, so a user name or
   data path with non-ASCII characters may fail to open. The fix is UTF-8 to UTF-16 path conversion at the file
   boundaries (`std::filesystem::path(std::u8string)`) or the UTF-8 code page in an application manifest
   (`activeCodePage`). Neither is done yet.
4. **Console subsystem.** `x2recomp.exe` is a console program, so Explorer opens a console next to the window.
   A GUI (`WIN32`) build would need `SDL_main` or a `WinMain` wrapper and would lose stdout. Left as it is.
5. **Stdout CRLF.** On Windows, the hash lines on stdout (`framehash`, `presenthash`) end in CRLF. The CTest
   drivers extract them with regular expressions such as `[0-9a-f]+`, which stop before the `\r`.
   `.gitattributes` keeps the test data (`*.hashes`, replay scripts, goldens) LF in Windows checkouts. This has
   not been checked on Windows.
6. **High-resolution timing and pacing.** These use SDL (`SDL_GetTicksNS`, `SDL_DelayNS`). Windows timer
   granularity may make frame pacing less even than on macOS. Not measured.
7. **MinGW.** `x2recomp.exe` links libstdc++ and libgcc statically. SDL3 comes from the pinned build. The CI job
   is allowed to fail.
8. **Packaging.** `tools/package.ps1` and `tools/build_all.ps1` have only been reviewed: PowerShell was not
   available on the developer machine.
9. **SmartScreen.** The executable is not code-signed, so SmartScreen warns on first start.

## Linux notes

- SDL3 is not packaged in Ubuntu 24.04. Use `-DX2_FETCH_SDL3=ON`, which needs the X11, Wayland and audio
  development headers listed in SDL's `docs/README-linux.md` (the CI job installs them). Distributions that
  package SDL3 (Fedora, Arch, Debian 13 / Ubuntu 25.04 and later) can use `find_package`.
- Ubuntu 24.04 ships CMake 3.28. Use `pip install cmake` (in a venv) or Kitware's apt repository for 3.29 or
  newer.
- Tests run with SDL's dummy video and audio drivers, so no display or sound server is needed.
