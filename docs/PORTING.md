# Porting and platform status

One codebase builds on macOS, Linux and Windows with CMake 3.29 or newer, a C++20 compiler and SDL3. This page
covers three things:

- which platforms have been built and tested;
- the portability rules the code follows;
- the risks that remain on platforms that have not been run.

`docs/BUILDING.md` covers building and running; `docs/RELEASING.md` covers packages;
`docs/WINDOWS-VERIFICATION.md` records what was run on a real Windows 11 machine with the game. The maintainer
has also played it by hand there, in a window with sound, and reports that it matches the macOS experience (a
hands-on impression, not a measurement). Linux has not been played by hand.

Everything here is GPL-3.0-or-later (`LICENSE`), so a port carries that licence too: if you distribute a
binary for a new platform, ship the source with it. Every original source file carries an
`SPDX-License-Identifier: GPL-3.0-or-later` header — keep it when you add files. Third-party components keep
their own licences (`THIRD-PARTY-NOTICES.md`), and a `-DX2_RECOMP=ON` build is not redistributable because it
contains code translated from your game image.

One portability note follows from that: `runtime/musashi_no_fpu.c` replaces Musashi's SoftFloat, so the
`musashi` target must keep it in place of `vendor/musashi/softfloat/softfloat.c` on every platform. It is
plain C99 with no platform dependencies, so no port should need to touch it.

## Platform status

Two kinds of evidence appear below and they are not equivalent:

- **CI** means GitHub Actions built the tree and ran the test suite **without game data**. The tests that need
  the user's own disks report Skipped (exit 77) and SDL uses its dummy video and audio drivers. This proves the
  code compiles and that everything not requiring the game works. It does not run a single frame of the game.
- **with game data** means the real game was imported and the replay, golden-frame and audio-hash tests ran.

| Platform | Compiler | SDL3 | Built | Tests run | How |
|---|---|---|---|---|---|
| macOS arm64 | AppleClang | Homebrew 3.4.18 | yes | with game data: 60/60 interpreter, 80/80 `-DX2_RECOMP=ON`. Also CI | developer machine + CI |
| macOS x86_64 | AppleClang | pinned 3.4.18 | yes | with game data: 60/60 interpreter build, under Rosetta 2. Also CI | `macos-x86_64` preset + CI |
| macOS universal | AppleClang | pinned 3.4.18 | yes | with game data on the arm64 slice; packaged app's level1_play replay on both slices. Also CI | `macos-universal` preset + CI |
| Linux arm64, Ubuntu 22.04 | GCC 12.3 | pinned 3.4.18 | yes | no data: 67/67 in the build container. The **packaged** binary then ran three replays with game data in a clean Debian 12 container: `level1_play` 24/24 hashes, `boss1` 24/24, `ending_natural` 20/20 | `tools/release_linux.sh`, the release build |
| Linux arm64, Ubuntu 24.04 | GCC 13.3 | pinned 3.4.18 | yes | with game data: 60/60 interpreter, 80/80 `-DX2_RECOMP=ON` | Docker container |
| Linux arm64, Ubuntu 24.04 | Clang 18.1 | pinned 3.4.18 | yes | with game data: 60/60 | Docker container |
| Linux x86_64, Ubuntu 24.04 | GCC, Clang | pinned 3.4.18 | yes | CI, plus the packaged x86_64 binary ran `level1_play` with game data (24/24 hashes) in a clean Debian 12 container, emulated on an arm64 host | `.github/workflows/ci.yml` |
| Windows x64, Windows 11 26200 | MSVC 19.41 (VS 2022) | pinned 3.4.18 | yes | with game data: 60/60 interpreter, 80/80 `-DX2_RECOMP=ON`. Also CI | `windows-msvc` preset, Ryzen 9 7950X; `docs/WINDOWS-VERIFICATION.md` |
| Windows x64, Windows 11 26200 | MSVC 19.41, Build Tools, Ninja | pinned 3.4.18 | yes | no data: 60/60 | same machine |
| Windows x64, Windows 11 26200 | clang-cl 22.1.2 (standalone LLVM, Ninja) | pinned 3.4.18 | yes | no data: 60/60 | same machine. The `windows-clangcl` preset needs VS's ClangCL toolset, which a standalone LLVM does not provide |
| Windows x64, Windows 11 26200 | MSVC 19.29 (VS 2019), Ninja | pinned 3.4.18 | yes | no data: 60/60 | same machine |
| Windows x64, Windows 11 26200 | MinGW-w64 GCC 13.1 (Strawberry, msvcrt) | pinned 3.4.18 | yes | with game data: 13/13 of a subset, including `golden_frames`, `game_audio` and two replays | same machine, Ninja |
| Windows x64 | MinGW-w64 GCC, MSYS2 UCRT64 | pinned 3.4.18 | yes | CI only, no game data | `windows-mingw` preset + CI |
| Windows x64 | MinGW-w64 GCC 13, cross-compiled | pinned 3.4.18 | yes, both builds | none: never run | Ubuntu cross-compiler in Docker |

The counts in the table are from the runs named, at the time they were made (the suite has grown since: 67 tests in
the 0.1.0 default build, 128 in 0.2.0's, which also covers the disk importer and the retail, Power Pack, 1762 and
crack variants; the data-dependent ones are skipped without images). Row by row, only the evidence kind matters.

**CI status.** At 0.1.0 run `41041824614`, on commit `95d9d78`, passed every job: repository hygiene, macOS arm64
(Homebrew SDL3), macOS universal, macOS x86_64, Linux GCC, Linux Clang, Windows MSVC and Windows MinGW-w64.
The manual data-dependent job is skipped by design — it only runs from `workflow_dispatch` with an
`assets_dir` pointing at a self-hosted runner's game data. The previous run, `41034182594` on `e1db672`, had
MSVC failing; the cause and fix are under "MSVC: the opcode-table miscompilation" below.

Two caveats when reading a future green run: **`macOS x86_64` and `Windows MinGW-w64` are
`continue-on-error`**, so they report failure without failing the workflow. A green tick on the run does not
by itself mean those two passed — check the job results. In run `41041824614` they did pass. On `main` of the current
repository the six other jobs are required checks (`docs/RELEASING.md`).

**What CI does not establish.** Every job ran without game data, so the whole emulation path is untested
wherever the table above does not say "with game data". Windows is no longer in
that position: `docs/WINDOWS-VERIFICATION.md` records a full with-data run. What is still missing there is a
window and a sound device, and no platform has had a human play session (`README.md`, "Status and known gaps").

Notes on the Linux runs:

- `cdtv_game` was Skipped, because the CDTV image was not copied into the container.
- `check_no_game_data` was Skipped, because the container has no git checkout.

Two problems were fixed after the Linux runs found them:

- Clang with libstdc++ rejected a `std::pair` of an incomplete type in the asset-pack JSON parser.
- CMake's C++20 module scan needs `clang-scan-deps`, which Ubuntu's clang package does not include. Module
  scanning is now off.

The MinGW cross-build produces `x2recomp.exe`, which imports only `KERNEL32.dll`, `msvcrt.dll` and `SDL3.dll`,
and an install tree. Neither has been run on Windows or under Wine; a MinGW build made natively on Windows
has (`docs/WINDOWS-VERIFICATION.md`).

Every frame, audio and presenter hash of the replay corpus matches the recorded goldens on these platforms:

- macOS arm64;
- macOS x86_64;
- Linux arm64 with GCC;
- Linux arm64 with Clang;
- Windows x64 with MSVC 19.41, and the subset run under MinGW-w64 GCC 13.1 (`docs/WINDOWS-VERIFICATION.md`).

The recompiled build matches the interpreter on macOS arm64, on Linux with GCC and on Windows with MSVC.

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
  The one place a `size_t` meets a `long` is `inflate_raw`, because puff counts in `unsigned long`: it refuses
  a length above 4 GB rather than narrowing it silently. Those were MSVC's only two warnings (C4267).
- **The MSVC C runtime is linked statically** (`X2_STATIC_CRT`, on by default; `-DX2_STATIC_CRT=OFF` to
  opt out). A dynamic CRT makes `x2recomp.exe` import `VCRUNTIME140.dll` and `MSVCP140.dll` from the Visual
  C++ redistributable, which a clean Windows machine does not have, so the release would not start. It is set
  before any target is created so that the fetched SDL3 gets it too — an `SDL3.dll` built `/MD` would pull the
  redistributable back in on its own. The Universal CRT (`api-ms-win-crt-*`) is a system component on
  Windows 10 and later and is linked statically as well, so a package depends on nothing but system DLLs.

  That leaves `x2recomp.exe` and `SDL3.dll` with one C runtime each, which is only safe if no allocation is
  freed across the boundary. It is not: the whole runtime takes exactly one allocation from SDL,
  `SDL_GetPrefPath`, and releases it with `SDL_free`. Every other SDL object is paired with its own SDL
  destructor (`SDL_CreateWindow`/`SDL_DestroyWindow`, `SDL_CreateSurface`, `SDL_CreateTexture`,
  `SDL_CreateRenderer`, `SDL_OpenGamepad`, `SDL_OpenAudioDeviceStream`); `SDL_GetError`, `SDL_GetScancodeName`
  and `SDL_GetGamepadName` return strings SDL owns and are never freed; and nothing of ours is handed to SDL to
  free — `SDL_UpdateTexture`, `SDL_PutAudioStreamData` and `SDL_RenderGeometry` all copy or read. **Keep it
  that way:** any new SDL call that returns memory must be freed with `SDL_free`, never `free` or `delete`.
- **The UTF-8 code page on Windows.** Paths are UTF-8 `std::string`s, which is what SDL returns, so every
  executable embeds `packaging/win-utf8.manifest` (`activeCodePage=UTF-8`, Windows 10 1903 and later). Without
  it the process runs in the legacy ANSI code page, which replaces every byte it cannot represent with `?`
  **in `argv` itself**, before `main` is entered — so converting at the file boundaries would be too late. See
  "Windows: residual risks" and `docs/WINDOWS-VERIFICATION.md`.
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
  matter. Paths are built with `std::filesystem` or `/`, which Windows accepts. The same holds for the
  committed test data: a whole source tree converted to CRLF passes the hash-comparing tests on Windows, so
  neither CMake's `file(STRINGS)` nor the replay-script parser is confused by `\r`
  (`docs/WINDOWS-VERIFICATION.md`). Tools that *write* committed test data pass `newline="\n"`, so a Windows
  developer does not turn a hash refresh into a whole-corpus CRLF diff.
- **Keyboard defaults differ by platform** (`runtime/input/input.cpp default_bindings()`, the only
  `#ifdef __APPLE__` in the input code). macOS reserves Ctrl + the arrow keys for Spaces, Mission Control and
  App Expose, so fire there is Z, X or either Option key and no Ctrl key is bound; Windows and Linux keep
  Left/Right Ctrl, where only Ctrl+Alt+arrow (workspace switching on some Linux desktops) is taken and Ctrl
  alone is free, but list Z and X first because of the Windows pointer-ring setting below. macOS also gets Cmd combinations for every host action, because the OS claims most
  F-keys unless the "standard function keys" setting is on. A unit test asserts the per-platform invariants.
  `input.cfg` is per-machine, not portable between platforms, and carries a `# version:` marker so a file
  written by an older release that still holds its defaults can be replaced with the current ones.
- **Mouse cursor.** The window hides it while the game is running (`CursorPolicy` in `runtime/input/input.h`,
  `cursor = auto|show|hide`) with `SDL_HideCursor()`, which is portable and does nothing else:
  `SDL_SetWindowRelativeMouseMode()` would also confine and capture the pointer and is not used. No mouse
  event is read anywhere, on any platform. One platform quirk worth knowing is not ours to fix: Windows'
  "Show location of pointer when I press the CTRL key" accessibility setting draws a ring on every Ctrl press,
  which is why Z and X lead the Windows and Linux fire bindings. Whether hiding the cursor suppresses that
  ring is untested.
- **Assists and cheats** (autofire, god mode, starting credits) are host-side only and portable: autofire is
  pure logic in `AutofirePolicy`, and god mode and the credits are `Machine::write16` calls made in the host
  frame loop at the same point as the coverage pokes, never in an interpreter hook, so the interpreter and
  the translated executor see the same thing on every platform. None of them is applied unless asked for, so
  no test hash can move. God mode additionally traps the game's death routine and returns through
  `Machine::do_rts()`; a trap that moves the PC ends a translated run and the executor continues at the new PC
  exactly as the interpreter would (`X2R_BOUNDARY` in `runtime/recomp/x2r_runtime.h`), which is why the cheat
  is a trap rather than a patched instruction -- a patch would fail the executor's byte check and push the
  routine back to the interpreter. `ctest -R cheats_god_mode_equivalence` pins the two together.
- **Keyboard grab** (`SDL_SetWindowKeyboardGrab`, off by default) works on Windows and X11/Wayland. On macOS
  SDL's Cocoa backend only disables the global hotkeys when SDL is compiled with `SDL_MAC_NO_SANDBOX`, which
  no released package defines, and returns success regardless — so the failure cannot be detected at run time
  and the feature is documented as a no-op there rather than relied on.
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

MSVC 19.41 at `/W3` reported two C4267s, both on one line of the importer; that line now refuses an
over-long length instead of narrowing it (see "Integer sizes"), so MSVC is warning-free too.

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

With both fixes in, the MSVC job passes the no-data suite (run `41041824614` on commit `95d9d78`), and the
whole game has since been run on Windows with real data: 18 replays with matching frame hashes, which checks
the generated tables far more directly than the no-data suite can (`docs/WINDOWS-VERIFICATION.md`).

## Windows: residual risks

The game has now been run on Windows with real data, so most of what used to be listed here is settled —
`docs/WINDOWS-VERIFICATION.md` has the detail. What remains:

1. **No window, no sound, no gamepad.** Every Windows run so far has been headless or used SDL's dummy video
   and audio drivers, because the machine was reached over SSH. Nothing has drawn a pixel on a Windows display
   or produced a sample on a Windows audio device, and no gamepad has been touched. Frame pacing against a real
   display's vsync is likewise unmeasured; against the dummy driver the pacer holds 50 Hz to within 0.5% over
   2000 frames, so timer granularity is not a concern, but vsync is a different mechanism.
2. **Console subsystem.** `x2recomp.exe` is a console program (`Windows CUI`, confirmed), so Explorer opens a
   console next to the window. A GUI (`WIN32`) build would need `SDL_main` or a `WinMain` wrapper and would
   lose stdout. Left as it is.
3. **SmartScreen.** The executable is not code-signed, so SmartScreen warns on first start. Not tested, since
   that needs a download through a browser.
4. **MinGW.** `x2recomp.exe` links libstdc++ and libgcc statically. SDL3 comes from the pinned build. The CI
   job is `continue-on-error`, so it does not gate the workflow — it passed in run `41041824614`, but a green
   tick on a later run does not prove it still does.
5. **Musashi on a new toolchain.** The miscompilation above was silent: a build that merely started would have
   run the wrong instruction handlers. The tables are now verified on MSVC 19.41, MSVC 19.29 and clang-cl, but
   treat them as something to verify on each new toolchain rather than to assume.
6. **One machine.** All of the above is one Windows 11 build on one AMD CPU. Note in particular that it has
   the system-wide UTF-8 code page enabled, which is *not* the default — which is exactly why the non-ASCII
   path bug needed a deliberate experiment to see (`docs/WINDOWS-VERIFICATION.md`).
7. **PowerShell.** Both `tools/package.ps1` and `tools/build_all.ps1` have now been run on Windows.
   `package.ps1` worked unchanged; `build_all.ps1` needed a parameter-binding fix before it would accept the
   disk images at all (`docs/WINDOWS-VERIFICATION.md`). Both call `cmake` by name, so they get whatever is
   first on `PATH`. Neither is covered by a test, so both can rot again without anyone noticing.

## Linux notes

- SDL3 is not packaged in Ubuntu 24.04. Use `-DX2_FETCH_SDL3=ON`, which needs the X11, Wayland and audio
  development headers listed in SDL's `docs/README-linux.md` (the CI job installs them). Distributions that
  package SDL3 (Fedora, Arch, Debian 13 / Ubuntu 25.04 and later) can use `find_package`.
- Ubuntu 24.04 ships CMake 3.28. Use `pip install cmake` (in a venv) or Kitware's apt repository for 3.29 or
  newer.
- Tests run with SDL's dummy video and audio drivers, so no display or sound server is needed.
- **The release archive's glibc floor is the build machine's**, because glibc is not bundled. That is the one
  thing a Linux release gets wrong most easily: a tarball built on Ubuntu 24.04 (glibc 2.39) will not start on
  Debian 12 or Raspberry Pi OS, with `GLIBC_2.38 not found` and nothing else to go on.
  `tools/release_linux.sh` therefore builds in `ubuntu:22.04` (glibc 2.35) and links libstdc++ and libgcc
  statically into the executable, so `x2recomp` asks the system for nothing but libc, libm and the bundled
  SDL3. `docs/RELEASING.md` has the measured floors and how to re-measure them.
- GCC 12 is the oldest compiler the tree has been built with. The C++20 the code uses (`std::endian`,
  `std::numbers`, designated initialisers, `<bit>`) all works there. It found one real bug that newer
  libstdc++ headers had hidden: `runtime/gfx/video.h` used `size_t` without including `<cstddef>`.

## Raspberry Pi

The `linux-aarch64` archive is the Raspberry Pi archive: 64-bit Raspberry Pi OS is Debian 12 "bookworm"
(glibc 2.36), above the archive's 2.35 floor. A Pi 4, 5 or 400 on the 64-bit OS image is the target; a Pi 3 on a
64-bit image should start but is expected to be borderline for speed. **32-bit Raspberry Pi OS (`armv7l`,
armhf) is not supported by either archive**, and no armhf build has ever been attempted — the practical answer
is the 64-bit OS image on the same hardware.

Nothing about this has been tested on a Pi. What *is* established is the part a Pi shares with any Debian 12
machine: the packaged binary runs in a clean `debian:bookworm-slim` container with no packages installed, and
reproduces the `level1_play`, `boss1` and `ending_natural` replay hashes bit for bit from the user's own game
data. What is untested is the hardware, the display and the sound — a Pi's CPU is several times slower than
anything this has run on, its GPU path is KMSDRM or Wayland rather than X11 on a desktop, and Linux has never
been played by hand anywhere.

Speed is the open question, and the container measurement cannot answer it: the packaged binary running
`level1_flood` headless, 6000 frames, best of two runs in the aarch64 container on an Apple M-series host takes
7.19 s — 835 fps, **16.7x real time**, within noise of the same measurement made natively on macOS
(`docs/WINDOWS-VERIFICATION.md`, "Performance"). That is the host CPU's number, not a Pi's. **Pi performance is
unmeasured.** The game needs 50 fps, so anything above about 1.5x real time headless has room for the window,
the filter and the audio mixing on top.

### Test recipe for a Raspberry Pi

For someone with a Pi and their own disk images. Each step says what to report back.

1. On the Pi, check the architecture: `uname -m`. It must print `aarch64`. If it prints `armv7l` the OS is
   32-bit and this archive cannot run — stop here and report that.
2. Copy the archive and one replay script across (the script is a text file of joystick inputs, not game data):
   `scp x2recomp-0.2.0-linux-aarch64.tar.gz* tests/replays/level1_flood.txt pi@raspberrypi:~/`
3. Check the download: `sha256sum -c x2recomp-0.2.0-linux-aarch64.tar.gz.sha256` — must say `OK`.
4. Unpack: `tar xzf x2recomp-0.2.0-linux-aarch64.tar.gz && cd x2recomp-0.2.0-linux-aarch64`
5. `./bin/x2recomp --version` must print `x2recomp 0.2.0`. If it fails with `GLIBC_... not found`, report that
   line together with `ldd --version | head -1` — that is the floor being wrong and nothing else works until it
   is fixed.
6. Import your disks: `./bin/x2recomp --import ~/Xenon2.zip` (exit status 0), then
   `./bin/x2recomp --print-data-dir` to see where they went.
7. Benchmark, which is the main number wanted:
   `time ./bin/x2recomp --headless --report-every 0 --input-script ~/level1_flood.txt` — 6000 frames. Report the
   wall time. The multiple of real time is `6000 / seconds / 50`, and it needs to be above about **1.5x**; 7.2 s
   (16.7x) is what a desktop CPU does.
8. Play it in the desktop session: `./bin/x2recomp --scale 2 --filter nearest`. Report whether a window appears,
   whether there is sound, whether the title music plays, and whether the arrow keys and Z respond.
9. If no window appears, try `SDL_VIDEO_DRIVER=wayland ./bin/x2recomp`, then `=x11`, and from a text console with
   no desktop running `SDL_VIDEO_DRIVER=kmsdrm ./bin/x2recomp` (which needs your user in the `video` and `render`
   groups). Report which one worked and the error from the ones that did not.
10. If there is no sound, try `SDL_AUDIO_DRIVER=pipewire`, then `=pulseaudio`, then `=alsa`, and add
    `--audio-latency 80` if it is present but breaks up. Report which worked.
11. If it crashes, re-run `./bin/x2recomp --headless --frames 300` and report the last ten lines of output.
12. Report the machine with it: `cat /proc/device-tree/model`, `cat /etc/os-release`, `uname -m`,
    `ldd --version | head -1`, `echo $XDG_SESSION_TYPE`, and how much memory the Pi has.
