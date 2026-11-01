# The Atari ST release on the recompiled (translated) executor

Phase S7 of `.omc/plans/xenon2-atari-st-target-plan.md` (decision: the recompiler is in scope for 0.4.0). With `-DX2_RECOMP=ON` the Kixx ST release runs on translated
C++, bit-identical to the interpreter, exactly as the disk builds and the CDTV release do (`recompiler-design.md`, `cdtv-recomp.md`). This note is the design, the
evidence and the limits. No game bytes and no generated code are in the repository; everything below is produced locally from the user's own disk image.

Read first: `atari-st-machine.md` (the machine), `atari-st-importer.md` (what the import writes), `atari-st-hooks.md` (the coverage pokes the scripts use).

## 1. What was generalised

The translated code never touches a machine directly: every translated instruction is Musashi's own handler body, memory goes through Musashi's `m68k_read_memory_*` /
`m68k_write_memory_*` callbacks (which the ST installs through `runtime/bus_hooks.h`), and `X2R_BOUNDARY` calls the same per-instruction hook the interpreter calls. So the
ST machine's bus, its 4-cycle rounding, its device wait states and its PC hooks (god mode, the score table) work on translated code unchanged. What was needed:

| Piece | Change |
|---|---|
| Executor (`runtime/recomp`) | `Target { Disk, Cdtv, St }`, `x2r_modules_st`, and `enable_raw(ram, size, err, target)`: the dispatcher no longer needs the Amiga `Machine` (`enable(Machine&)` calls it). `print_summary_raw` / `write_profile_raw` take a RAM pointer. |
| StMachine (`runtime/st`) | `set_cpu_execute(fn)`: the slice loop calls the hybrid `execute()` instead of `m68k_execute()` (same cycle accounting, same hook at every boundary; null = Musashi). `state_digest()` (RAM, registers, cycle counter, interrupt and rounding counts). |
| Tracer (`runtime/trace`) | Generalised from the Amiga `Machine` to an `Env` (RAM, frame counter, SP and SR callbacks, interrupt vector list) and a layout with an ST mode (section 2). Disk and CDTV traces are byte-identical to before (`attract` and `level2` of both games compared against a build of the previous main). |
| Generator (`tools/recomp/recomp.py --build st`) | Images from the import, module set `game`, `frontend`, `level1`..`level5`, prefix `x2rs_`, symbol `x2r_modules_st`, `generated/recomp-st`. |
| Corpus (`tools/trace/run_corpus.py --game st`) | Runs `tests/replays/st/*.txt` and `tests/recomp/st_scenarios/*.txt` with `--trace-out` into `generated/trace-st`. |
| CLI (`--game st`) | `--interp-only`, `--recomp-stats N`, `--recomp-profile FILE`, `--trace-out`, `--trace-label` and `--st-state-hash` work as for the other games. The executor is attached by `main.cpp` (a callback in `RunOptions`), so the ST library does not depend on the translated code. |
| CMake | `X2_RECOMP_ST_DIR` (default `generated/recomp-st`), `X2_RECOMP_ST_SOURCES`, an empty stand-in module set when absent (`runtime/recomp/empty_modules_st.cpp`), and the tests of section 4. |
| Scripts, hygiene | `tools/build_all.sh` / `.ps1` translate an imported `assets-st`; `check_no_game_data.py`, `package.sh` and `package.ps1` refuse `x2rs_*` files. |

## 2. The module set and the dispatch

The ST has no module loader to hook. What runs, and where (all from `atari-st-s0.md` and the importer's manifest):

| Module | Image the generator builds | Address | Notes |
|---|---|---|---|
| `game` | side 0, cylinders 34-79, raw (235,520 bytes) | `$400` | the engine: entry `$7CF8`, VBL handler `$2984`, ACIA handler `$0F7C` |
| `frontend` | `rnc.0`, RNC-unpacked (159,108 bytes) | `$40000` | title, menus, the Timer A handler `$40C94` |
| `level1`..`level5` | `rnc.1`..`rnc.5`, RNC-unpacked | `$4F000` | one at a time, over the front end's area |

The boot sector, the stage-1 loader and the RNC depacker (what runs before the engine starts, from low RAM) are not translated: they run once, about 4.26 million
instructions, mostly the loader polling the disk controller (section 5).

**Dispatch** is the one the other builds use: a table maps each PC to the candidate translations (one per module with an instruction there), the first whose opcode and
extension words match memory runs, and the module that matched is tried first next time. The front end and the level modules share addresses and the engine's code
sits under the stage-1 loader's, so nothing needs to know which phase the program is in: an instruction is only run translated if its words are the bytes in RAM.

**The tracer** needs to tell the same things apart, to attribute the traced PCs to the right image (`Tracer::Layout::atari_st()`): before the engine's first instruction
(`$7CF8`), low RAM is the context `stage1` and the window `$40000`-`$7FFFF` holds `frontend`; afterwards low RAM is `game` and the window holds a level module named
`mod_<key>`. The key is the FNV-1a hash of 2 KB of the module. For the front end that is the first 2 KB (equal to the unpacked file's), for a level module the 2 KB
from byte 16: the program patches pointers in the module's first bytes after unpacking it (offsets 5-7 differ from the unpacked file), and the key must equal the one the generator
computes from the disk. The key is taken at the first entry after the program wrote the module's first bytes, so a second module unpacked over the first is a new
context. The generator maps each `mod_<key>` to the image with the same key (a trace module that matches none is reported and skipped).

## 3. Self-modifying code

The front end's Timer A handler (`$40C94`, 187,479 interrupts in the front end's part of every run) patches the immediate of one of its own instructions (`$40CA2`, the word at `$40CA4`) on
every interrupt. Translated code verifies every instruction's opcode and extension words against RAM before it executes, so that one instruction fails its
verification and the executor interprets it (`X2R_UNVERIFIED`); the rest of the handler runs translated. Nothing special is generated for it, and nothing else patches
code in the corpus (0 opcode changes in the traces). Any other self-modification, anywhere, is safe for the same reason: it costs speed, never exactness.

## 4. Evidence

All with the Kixx import, generated and built locally (`-DX2_RECOMP=ON`, macOS arm64).

**Translation.** 406 functions, 12,324 instructions (game 8,977; frontend 669; level1 507; level2 605; level3 549; level4 212; level5 805), 13 files, 492 handlers. The
capstone length cross-check: 18,104 instructions, 0 mismatches. Generated from traces of the 10 scripts below plus the static seeds (the engine's entry, the VBL and ACIA
handlers, the front end's entry and its Timer A handler; a trace flags the front end's first instruction only as a branch target, so it is seeded by hand).

**Equivalence, interpreter against translated** (`recomp_st_<script>`, `tests/recomp/st_equivalence.cmake`): each script is run twice, `--interp-only` and hybrid, and
must give the same framebuffer hash, audio hash and **state digest** (all RAM, D0-A7, PC, SR, USP/ISP, cycle counter, VBL/MFP/rounding counts) at **every frame**, the
same frame log (PC, SR and video base at each frame boundary) and the same final RAM. 10 of 10 pass, 84,000 frames, 252,000 per-frame hashes:

| Script | Frames | What it covers | Translated |
|---|---:|---|---:|
| `attract` | 4,000 | boot, stage-1 loader, front end title and attract loop (no pokes) | 90.63% |
| `keys` | 6,000 | level 1 with keyboard events through the IKBD (pause, space, escape, number and function keys) | 93.91% |
| `level1_play` | 9,000 | level 1, fire and sweeps (no pokes) | 95.95% |
| `level2` (S5) | 6,000 | the end-of-level path loads level 2 | 93.90% |
| `level3`, `level4`, `level5` | 8,000 each | the same path loads levels 3-5 (energy held full) | 95.50%, 95.47%, 95.46% |
| `ending_l5` (S5) | 7,000 | level 5 to its end event, the ending, the wrap to level 1 | 94.95% |
| `game_over` | 14,000 | fragile ship: deaths, continues, game over, the high-score entry, the title again | 97.47% |
| `hold_fire` (S5) | 14,000 | the same through one continue, the score table | 97.46% |

The existing ST goldens (`st_kixx`, `st_hooks_*`) pass on the translated build as well, and an unmodified build's hashes for `hold_fire` are the same as before this
change (the seams are inert when nothing attaches).

**Differential test** (`recomp_st_difftest`, `x2_recomp_difftest --game st`, `tests/recomp/difftest_st.cpp`). Every translated function, and random mid-function entry points,
run from random states (registers, SR, a pending interrupt, random data bytes over the images, stack) on a bare 512 KB bus with the ST's 4-cycle rounding, interpreter against
translated, comparing the whole CPU context (raw lazy flags included), all 512 KB of RAM, cycles, the rounding count and every data bus access with its cycle stamp. The
default pass: **406 functions, 6,090 cases, 0 differences**, every function ran translated code (295,958 translated instructions). Under AddressSanitizer and UBSan (short pass,
1,218 cases) and for `st_machine` (143 checks, with the new executor and tracer seams): clean.

**The disk and CDTV builds are unaffected.** A recompiled build carrying all three translations passes the full suite: 264 tests, 0 failures (10 skipped: the folder-driven import
tests and the PDos release, which need dumps this machine does not have), including the disk `recomp_*` and the 23 `recomp_cdtv_*` equivalence tests and both difftests.

**Coverage on code the translation was not traced on.** Traces of 5 scripts only (`attract`, `level1_play`, `level2`, `hold_fire`, `keys`: no level 3-5 module was traced, so
none of them has a translated instruction): the other five scripts, run on that translation, are **identical** in every hash, log and RAM, at 94.8-97.5% translated (level 3 94.97%, level 4
95.36%, level 5 95.28%, `ending_l5` 94.76%, `game_over` 97.47%). The level modules are small (about 200-800 distinct instructions each); the engine does the work.

**The interpreter fallback is exact.** A deliberately partial translation (the trace of `attract` alone, no static seeds: 1,844 instructions, nothing in any level) gives identical hashes,
logs and RAM on `level1_play` (47.76% translated), `level2` (45.22%), `level4` (42.09%), `ending_l5` (40.56%) and `hold_fire` (52.16%).

**Speed.** `level1_play`, 9,000 frames, headless, best of three: 1.73 s interpreted, 1.38 s translated (5,200 against 6,520 frames per second, about 100 and 130 times real time): the
translated executor is about 25% faster. As on the other builds, the executor is about equivalence and portability; the machine model is the rest of the time.

## 5. What is interpreted, and the gaps

- **The boot** (4,257,932 instructions in every run, constant): the boot sector, the stage-1 loader and its RNC depacker. About 2.5 million of them are two instructions of the
  loader's wait for the disk controller (`$928`, `$92E`); the depacker runs from `$2ECE4`-`$2ED24` and a few more. They are not translated because they exist once, in memory
  that the engine overwrites, and are not worth a module of their own. They make the share on a short script lower (`attract`, 4,000 frames, 90.63%). Everything else that is
  interpreted in the corpus is the Timer A instruction of section 3 (187,479 per run, 0.4% of `attract`) and 621 instructions; excluding the boot, every script is above 99.5% translated.
- **Level code no script reached.** The scripts do not defeat the bosses of levels 1-4 (the poke recipe for level 5's end event, `atari-st-hooks.md`, does not carry over; their death
  and bonus code is untraced), do not use the shop (its exit is not scripted) and do not play the second player. That code interprets until a trace or a profile run
  (`--recomp-profile`, `recomp.py --profile`) seeds it. The hold-out result above is the measure of what that costs: speed only.
- **Only the Kixx release** is translated: it is the only ST release the importer accepts. Another ST release or pressing would translate nothing until regenerated, and the byte
  verification makes that safe.
- **Hand play** is not part of the evidence; the scripts are, and some of them (the level 2-5, `hold_fire`, `ending_l5`, `game_over`) use coverage pokes, so they are not faithful playthroughs.
  A translation derived from them is still exact on any play, since it is verified instruction by instruction.
- The translation is derived from the user's disk and is not redistributable (like every `-DX2_RECOMP=ON` build). CI builds the generator test and the executor and tracer changes; it
  does not build a translation.

## 6. Reproducing

```
S=generated/assets-st                                  # the import (<data dir>/assets-st)
python3 -I tools/trace/run_corpus.py --game st --exe build/x2recomp --assets "$S" --out build/x2-trace-st
python3 -I tools/recomp/recomp.py --build st --assets "$S" --trace build/x2-trace-st --out build/x2-recomp-st-src
cmake -S . -B build-recomp -G Ninja -DX2_RECOMP=ON -DX2_RECOMP_ST_DIR=build/x2-recomp-st-src -DX2_ST_GAME_ASSETS="$S" \
      -DX2_RECOMP_DIR=build/none -DX2_RECOMP_CDTV_DIR=build/none            # the ST translation only
cmake --build build-recomp && ctest --test-dir build-recomp -R recomp_st
build-recomp/x2recomp --game st --headless --input-script tests/recomp/st_scenarios/level1_play.txt --recomp-stats 10   # coverage and the hottest interpreted PCs
```

`tools/build_all.sh --recomp` (and `.ps1`) does all of it for whichever releases were imported.
