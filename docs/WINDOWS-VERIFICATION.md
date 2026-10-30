# Windows verification with game data

Until this run, Windows support rested entirely on a no-data CI job: it proved the tree compiles and that
every test not needing the game passes, and nothing more (`docs/PORTING.md`, "Platform status"). This page
records the first run of the real game on Windows — importing the user's own disks and comparing every frame,
audio and golden-frame hash against the goldens recorded on macOS.

Short version: **the hashes match.** No emulation, audio or rendering difference between Windows and macOS was
found. Six defects were found and fixed, none of them in the emulation path. One of them would have stopped
the release from starting at all on a machine without Visual Studio.

## The machine

| | |
|---|---|
| OS | Windows 11, build 10.0.26200.8655, x64 |
| CPU / RAM | AMD Ryzen 9 7950X (16 cores), 63 GB |
| Active code page | **65001** — the system-wide "Beta: Use Unicode UTF-8" option is on. Not the default; it matters, see below |
| SDL3 | pinned 3.4.18, downloaded and built by `-DX2_FETCH_SDL3=ON` |
| Display / audio | none used: every run is `--headless` or SDL's `dummy` video and audio drivers |

Compilers:

| Tag | Toolchain | Version |
|---|---|---|
| MSVC (Enterprise) | VS 2022 Enterprise, `Visual Studio 17 2022` generator | `cl` 19.41.34120, toolset 14.41.34120 |
| MSVC (Build Tools) | VS 2022 Build Tools, Ninja | `cl` 19.41.34120 — **the same toolset**, so this adds generator coverage, not compiler coverage |
| clang-cl | standalone LLVM, Ninja | clang 22.1.2, `x86_64-pc-windows-msvc` |
| MSVC 2019 | VS 2019 Build Tools, Ninja | `cl` 19.29.30159, toolset directory 14.29.30133 |
| MinGW | Strawberry Perl's MinGW-w64 (msvcrt, not MSYS2 UCRT64), Ninja | GCC 13.1.0 |

**CMake.** Nothing on this machine's `PATH` met the project's 3.29 floor: `where cmake` finds Strawberry
Perl's 3.26.4 first, then a 3.25.2 install. The CMake that works is the one Visual Studio bundles,
3.29.5-msvc4, under
`<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`, and that is what every run below used. This is
worth knowing before blaming the project for a preset that will not configure (`docs/BUILDING.md`).

## Results

Each build used the pinned SDL3 and `X2_REQUIRE_SDL3=ON`. "with data" means the user's own disks were imported
on this machine and `X2_ASSETS_DIR` pointed at the import.

| Build | Configure + build | No-data suite | With-data suite |
|---|---|---|---|
| MSVC 19.41, `Visual Studio 17 2022` generator | 16 s + 25 s | **60/60**, 49 Skipped — identical to CI run `41041824614` | **60/60**, only `check_no_game_data` Skipped |
| MSVC 19.41, Build Tools, Ninja | — | **60/60** | not run (same toolset as the row above) |
| clang-cl 22.1.2, Ninja | — | **60/60** | not run |
| MSVC 19.29 (VS 2019), Ninja | — | **60/60** | not run |
| MinGW GCC 13.1.0, Ninja | 52 s + 15 s | — | **13/13** of a with-data subset, see below |
| MSVC 19.41, same tree converted to CRLF | 43 s | — | **9/9** of the hash-comparing tests |
| MSVC 19.41, `-DX2_RECOMP=ON` | 4 min 35 s | — | **80/80**, only `check_no_game_data` Skipped |
| MSVC 19.41, **static CRT** (the release configuration) | 15 s + 25 s | **60/60**, 49 Skipped | **60/60** |
| MSVC 19.41, static CRT, `-DX2_RECOMP=ON` | 3 min 45 s | — | **80/80** |

VS 2019 builds the tree despite its older C++20 support, which was an open question. The three Ninja builds
were run without game data only; the point of them is compiler coverage against a miscompilation like the one
`docs/PORTING.md` records, and the no-data suite is what builds and exercises Musashi's opcode tables.

`check_no_game_data` is Skipped because the source was shipped as a `git archive` tarball, so there is no git
checkout for it to inspect — the same reason it is Skipped in the Linux container runs.

The with-data suite is the result that did not exist before: `game_import`, `game_boot`, `game_audio`,
`golden_frames`, `data_dir`, `cdtv_game` (the 680 MB CDTV import, 47.8 s), `modern_game`, all eighteen
`modern_check_*`, `modern_faithful`, `modern_midpoints`, `modern_upgrade`, `modern_wide`, `modern_wide_golden`
and all eighteen `replay_*` tests pass, against goldens recorded on macOS arm64.

### The import

The importer produces byte-identical assets on Windows:

| File | SHA-1 on Windows | matches macOS |
|---|---|---|
| `game_0400.bin` | `1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4` | yes |
| `disc2.bin` | `a748afc7a142335a40cc46fb7785dea820770241` | yes |

The nested `Xenon-2-Megablast_Amiga_EN.zip` accept/reject behaviour matches the macOS importer tests
(`game_import` passes, and it is the test that checks them).

## The recompiled build

The whole `-DX2_RECOMP=ON` path runs on Windows, including the two Python tools that feed it. `recomp.py`
needs a GCC-style compiler for its helper programs; Strawberry Perl's MinGW GCC 13.1.0 (`CC=`...`\gcc.exe`)
served, with no change to the scripts needed beyond the capstone fix below.

| Step | Time |
|---|---|
| `run_corpus.py`, 18 replays × 6000 frames, `--jobs 16` | 21 s |
| `recomp.py --no-lane` → 529 functions, 15,650 translated instructions, 12 files | 4 s |
| CMake configure | 14 s |
| `cmake --build --config Release` | **4 min 35 s** |

Peak compiler memory was about 490 MB in a single `cl.exe`: MSBuild compiles the generated translation units
one at a time within the `x2recomp_code` target, so `--parallel 16` does not help there and `/bigobj` does not
cost much memory. The build is the slow step precisely because it is serial.

Its suite is **80/80**, again with only `check_no_game_data` Skipped: `recomp_difftest` (36.8 s, against
76.4 s on macOS), `recomp_equivalence`, all eighteen `recomp_replay_*` and the whole interpreter suite on top.
So the translated code is bit-identical to the interpreter on Windows, frame hashes, register logs and audio
alike — the same guarantee the project already had on macOS and Linux.

## MinGW

The MinGW build had only ever been made in CI and cross-compiled on Linux; it had never been run on Windows,
nor on Wine. Strawberry Perl's bundled MinGW-w64 GCC 13.1.0 (an msvcrt toolchain, not the MSYS2 UCRT64 one the
`windows-mingw` preset assumes) builds the tree and the pinned SDL3 with the Ninja generator, and `windres`
embeds the UTF-8 manifest through `packaging/win-utf8.rc` — the exe contains `activeCodePage`, so the MinGW
half of that fix is exercised too, not just the MSVC half.

With game data it passes the subset run against it: `import`, `game_audio`, `golden_frames`, `replay_boss1`
and `replay_level1_play`, among others, 13/13. **So the frame and audio hashes match the macOS goldens under
MinGW as well as under MSVC** — two independent Windows toolchains agreeing with macOS on the emulation is
much stronger evidence than either alone. (`game_import` was Skipped: that run was not given `X2_DISK_ZIPS`.)

## Packaging

`tools/package.ps1` had only ever been read, never run: PowerShell was not available on the developer
machine. It works unchanged, and produced `x2recomp-0.1.0-windows-x64.zip` holding exactly what
`docs/RELEASING.md` promises:
`x2recomp.exe`, `SDL3.dll`, `LICENSE`, `README.md`, `THIRD-PARTY-NOTICES.md`, `PROVENANCE.md`, `BUILDING.md`,
`ASSET-PACKS.md`, `PORTING.md`, `licenses/` (four files) and `SHA256SUMS.txt`, plus the zip's own `.sha256`.
No game data, no imported assets, no generated code. Its content check passed.

Run from the unpacked package, the executable prints its version, its data directory and its help; imports the
user's disks into a fresh data directory (`game_0400.bin` SHA-1 matching); and replays `level1_play` headless
with **all 24 hashes matching** `tests/replays/level1_play.hashes`. The embedded UTF-8 manifest survives into
the packaged binary. The subsystem is `Windows CUI`, so Explorer opens a console beside the window, as
documented.

### The Visual C++ runtime: found broken, now fixed

The first package built here would not have started on a clean Windows machine. `dumpbin /dependents` on it:

```
SDL3.dll
MSVCP140.dll  VCRUNTIME140.dll  VCRUNTIME140_1.dll
api-ms-win-crt-*.dll  KERNEL32.dll
```

The `api-ms-win-crt-*` DLLs are the Universal CRT, a system component since Windows 10. The other three come
from the Microsoft Visual C++ 2015-2022 Redistributable, which a machine without Visual Studio does not have,
and the zip shipped neither the DLLs nor a note that they were needed — so the first thing such a user would
have seen is "VCRUNTIME140.dll was not found". That could not be reproduced on this machine, which has Visual
Studio installed, but an import table is not a matter of opinion.

**Fixed** by linking the CRT statically: `X2_STATIC_CRT`, on by default for MSVC, sets
`CMAKE_MSVC_RUNTIME_LIBRARY` before any target exists so that the fetched SDL3 gets it too — an `SDL3.dll`
built `/MD` would have brought the redistributable straight back through its own import table. After the
change, on the binaries inside the zip:

| | imports |
|---|---|
| `x2recomp.exe` | `SDL3.dll`, `KERNEL32.dll` — and nothing else |
| `SDL3.dll` | `KERNEL32`, `USER32`, `GDI32`, `WINMM`, `IMM32`, `ole32`, `OLEAUT32`, `VERSION`, `ADVAPI32`, `SETUPAPI`, `SHELL32` |

No `VCRUNTIME140`, no `MSVCP140`, no `ucrtbase`, and not even an `api-ms-win-crt-*`, because the Universal CRT
is statically linked as well. **The package now depends on nothing but Windows itself.** The
`-DX2_RECOMP=ON` executable, which links the translated code, comes out the same way.

Re-verified after the change: the no-data suite 60/60 with 49 Skipped, the with-data suite 60/60 with the
hashes unchanged, the recompiled build 80/80, and from the unpacked zip an import with a matching SHA-1 and a
`level1_play` replay with all 24 hashes matching. macOS is untouched — the setting is MSVC-only — and its
suite is 60/60 after it.

That leaves `x2recomp.exe` and `SDL3.dll` holding one C runtime each, which is only safe if nothing is
allocated on one side and freed on the other. An audit of every SDL call the runtime makes says it is safe:
the only memory SDL hands over is `SDL_GetPrefPath`'s string, released with `SDL_free`; every SDL object is
paired with its own destructor; `SDL_GetError`, `SDL_GetScancodeName` and `SDL_GetGamepadName` return strings
SDL owns and are never freed; and `SDL_UpdateTexture`, `SDL_PutAudioStreamData` and `SDL_RenderGeometry` copy
or read what we give them rather than taking it. The rule is written down in `docs/PORTING.md` so the next SDL
call does not quietly break it.

## Performance

Headless, `level1_flood`, 6000 frames, best of two runs, measured the same way on both machines:

| | Windows (Ryzen 9 7950X, MSVC) | macOS arm64 (AppleClang) |
|---|---|---|
| interpreter | 10.10 s → 594 fps, **11.9× real time** | 7.41 s → 810 fps, **16.2× real time** |
| `-DX2_RECOMP=ON` | 9.72 s → 617 fps, **12.3× real time** | 7.17 s → 837 fps, **16.7× real time** |

The recompiled build is only about 4% faster than the interpreter on this replay, on both platforms — it
exists for equivalence, not for speed, and `level1_flood` spends much of its time in the Paula and Denise
models rather than in translated 68000 code. Running the recompiled binary with `--interp-only` gives 12.2×,
within noise of the interpreter build, as it should.

So Windows is comfortably fast enough — nearly twelve times faster than the game needs — but a third slower
than macOS on the same work, on a CPU that is not a third slower. Whether that is MSVC's code generation for
the interpreter's dispatch loop or something about the machine was not investigated; it is a performance
observation, not a defect.

The 50 Hz pacer was measured with SDL's dummy video driver, which is the closest thing to a windowed run
available over SSH. Running four lengths separates a per-frame error from a fixed startup cost, and it is
entirely the latter:

| frames | wall | ideal | excess |
|---|---|---|---|
| 250 | 5.542 s | 5.000 s | +0.542 s |
| 500 | 10.453 s | 10.000 s | +0.453 s |
| 1000 | 20.422 s | 20.000 s | +0.422 s |
| 2000 | 40.371 s | 40.000 s | +0.371 s |

The excess does not grow with the run: the slope of wall against ideal is **0.9951**, and the implied fixed
cost is 0.567 s, which is start-up — SDL initialisation, loading the assets, booting the machine. So
`SDL_GetTicksNS` and `SDL_DelayNS` hold 50 Hz on Windows to within 0.5%, and Windows' timer granularity is not
the problem `docs/PORTING.md` worried it might be. Pacing against a real display's vsync is a different
mechanism and is still unmeasured.

## What is left: the interactive session

Everything above was produced over SSH, so nothing has been drawn on a Windows display or played through a
Windows sound card. That is the one thing no amount of hashing can replace, and it needs a person at the
machine. A ready-to-run folder was prepared for it, built from the final static-CRT package:

```
C:\Users\stoo\x2verify\PLAYTEST\
```

It holds the unpacked package, the game and the CDTV disc audio already imported into `PLAYTEST\userdata`
(so nothing touches the tester's real profile), a `PLAYTEST-CHECKLIST.txt` of 32 items with the expected result
for each, a `playtest-notes-template.txt` to type into, and one `.bat` per configuration worth looking at:

| | |
|---|---|
| `play.bat` | the plain default — windowed, scale 3, square pixels |
| `play-filters.bat` | `--filter crt --modern interp` |
| `play-wide.bat` | `--modern interp,wide=21:9` |
| `play-faithful.bat` | `--faithful` |
| `play-cdtv.bat` | `--music cdtv` |
| `play-firstrun.bat` | an empty data directory, for the drag-and-drop import; `reset-firstrun.bat` empties it again |

Every `.bat`'s command line was parsed back out and run headlessly with the dummy drivers, and every flag it
uses was checked against this build's own `--help`: all six are good. Nothing was launched windowed from here,
and the two imports that filled `userdata` could not have opened a window either — `--import` and
`--import-cdtv` return from `main` before any `SDL_Init`.

The checklist asks the questions a replay cannot: does the window open at the right size and aspect, does
fullscreen work, is the picture right on the title screen and in level 1, is the scroll even, is there sound
and is the right channel louder as it should be, does five minutes pass without a crackle, does the game feel
like 50 Hz, does interpolation look smooth on a high-refresh display, does a gamepad work when hot-plugged,
does drag-and-drop import work, and do high scores survive a restart.

## Windows-specific findings

### 1. Non-ASCII paths were broken, and the fix the docs proposed would not have worked

`docs/PORTING.md` listed this as a risk and suggested converting at the file boundaries with
`std::filesystem::path(std::u8string)`. That could not have worked, because the damage happens earlier.

This machine has the system-wide UTF-8 code page turned on, so the first test of a data directory named
`日本語` passed and looked like good news. It was not: it was a property of the machine. Building the same
probe program three ways shows what a default Windows does. With `activeCodePage=Legacy`, which forces the
legacy ANSI code page even on this machine:

```
GetACP=1252
argv1_bytes=...5c3f3f3f5c...        "\???\"  — the three CJK characters, as received by main()
fopen=FAIL
fs_exists=FAIL
```

and with `activeCodePage=UTF-8`:

```
GetACP=65001
argv1_bytes=...5ce697a5e69cace8aa9e5c...    the same path, intact UTF-8
fopen=OK
fs_exists=OK
```

The characters are replaced with `?` **in `argv` itself**, before `main` is entered. No amount of care at the
`fopen` call can recover them, because the bytes are gone. The same applies to the default data directory,
which `SDL_GetPrefPath` returns as UTF-8 and which contains the user's account name.

The same experiment on the real executable, with the whole tree built twice:

```
UTF-8 manifest   rc=0   assets_written=True
Legacy manifest  rc=1   assets_written=False
      | error: cannot create C:\Users\stoo\x2verify\cjk\Legacy\???-?ml?ut/assets: unknown error
```

**Fix:** `packaging/win-utf8.manifest` declares `activeCodePage=UTF-8` (Windows 10 1903 and later) and is
embedded in every executable — through the linker on MSVC (`/MANIFEST:EMBED /MANIFESTINPUT`), through
`packaging/win-utf8.rc` as an `RT_MANIFEST` resource on MinGW. The `x2_win_utf8()` helper in `CMakeLists.txt`
does nothing off Windows.

The regression test is in `tests/import/import_tests.cpp`: it imports from, and writes assets into, a
directory named `Ümläut-日本語` on every platform, and on Windows also asserts `GetACP() == 65001`. That
assertion is the one that catches a missing manifest, because the path round-trip passes on a machine like this
one whether or not the manifest is there.

### 2. The replay tools rewrote committed test data with CRLF

`tools/trace/run_corpus.py --record-hashes` and `tools/trace/make_replays.py` write into `tests/replays/`,
which `.gitattributes` keeps at LF on every platform so that the hash lists compare byte for byte. Both opened
those files in Python's text mode, so running either tool on Windows rewrote every file with CRLF and turned a
hash refresh into a whole-corpus diff. Both now pass `newline="\n"`. No other tool writes a committed file.

### 3. MSVC's two warnings were a silent narrowing

`inflate_raw` assigned `size_t` lengths to puff's `unsigned long` parameters, which are 32-bit on Windows
(C4267, twice — MSVC's only warnings in the project's own code). Nothing was truncated in practice, because
`kMaxFile` is 64 MB and `kMaxUnpacked` 16 MB, but the truncation would have been silent if a cap ever moved.
It now refuses a length above 4 GB and casts where the narrowing is intended.

### 4. The recompiler's capstone cross-check could only ever run on one developer's Mac

`recomp.py` checks every translated instruction's length against capstone's and records the result in
`manifest.json`. `tools/dis68k.py` loaded capstone from a single pinned path,
`/opt/homebrew/Cellar/capstone/5.0.9/lib/libcapstone.dylib`. Off macOS the load always failed, and the failure
is indistinguishable in the manifest from "capstone is not installed" — so a cross-check that exists to raise
confidence in the recompiler was quietly doing nothing on two of the three platforms, and would have stopped
working on macOS at the next Homebrew bump. Running the recompiler on Windows made it visible, because the
error named `C:\opt\homebrew\Cellar\...dylib`. It now tries `X2_CAPSTONE`, the platform loader, the usual
library names and Homebrew's unversioned path, and reports what it tried.

Capstone is not installed on this Windows machine and nothing was installed to make it so, so the cross-check
is still reported unavailable there — but now for the true reason, and with a message that says so.

### 5. `build_all.ps1` could never have taken the disk images it documents

The script's whole point is `build_all.ps1 [options] DISK_IMAGE...`: build, then import those disks. `$Images`
collects them with `ValueFromRemainingArguments`, but PowerShell binds positional parameters first, so the
first image went to `-BuildDir` — or, when that was given by name, onward to `-DataDir` and then `-Generator`.
The first real run said

```
CMake Error: Could not create named generator C:\Users\stoo\x2verify\data\Xenon2.zip
```

`[CmdletBinding(PositionalBinding = $false)]` makes the options named-only and lets the remaining arguments be
the images. With that, the script builds, imports, and writes an asset whose SHA-1 matches. This is the kind of
defect only running a script finds: it is invisible to review, because the parameter block reads correctly.

## Warnings

MSVC 19.41 is warning-free in the project's own code after finding 3. The older MSVC 19.29 (VS 2019) adds five
C4244 narrowing warnings, all inside `vendor/musashi/softfloat/softfloat-macros`, which is a vendored header;
no SoftFloat code is compiled or linked (`docs/PROVENANCE.md` §7.3) and the 68000 has no FPU path, so they are
noise from an older compiler on third-party code. clang-cl 22 reports none.

`cl : command line warning D9025: overriding '/W1' with '/w'` appears twice in every MSVC build. That is
deliberate: `m68kmake` and `musashi` are compiled without warnings.

## Risks that turned out not to be real

- **CRLF test data.** A second source tree with every `tests/replays/*.txt`
  and `*.hashes` converted to CRLF passes the hash-comparing tests. CMake's `file(STRINGS)` strips the `\r`
  itself, and the replay-script parser reads through a text-mode `std::ifstream`, where the CRT does it. Note
  that a normal Windows git checkout keeps these files at LF anyway, because of `.gitattributes`; this tested
  the case where something else converted them.
- **Musashi's opcode tables under MSVC.** The tables were verified only indirectly before,
  by the no-data suite. Now the whole game runs on them for 18 replays with matching frame hashes, which is as
  direct a check as exists.
