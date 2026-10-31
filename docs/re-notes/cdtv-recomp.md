# The CDTV release on the recompiled (translated) executor

Phase C7 of `.omc/plans/xenon2-cdtv-target-plan.md` (decision D3: in scope for the same release as the interpreter). With
`-DX2_RECOMP=ON` the CDTV release runs on translated C++, bit-identical to the interpreter, exactly as the disk builds do
(`recompiler-design.md`, `trace-coverage.md`). This note is the design, the evidence and the limits. No game bytes and no
generated code are in the repository; everything below is produced locally from the user's own disc image.

Read first: `cdtv-c0.md` (overlay facts), `cdtv-importer.md` (what the import writes), `cdtv-boot.md` (the traps), `cdtv-replays.md`
(the 23-replay corpus used as seeds and as the test).

## 1. What was generalised

The disk recompiler assumed one resident image plus six level modules loaded at `$54E00` by the disk shim. The CDTV release has one
resident image (X2, pre-linked at `$400` by the importer) plus seven **overlays** (`LEVEL1-5`, `INTRO`, `PICCODE`) that X2 itself reads
into one 180,000-byte area at `$4E26` and relocates. The pieces:

| Piece | Change |
|---|---|
| Tracer (`runtime/trace`) | `Tracer::Layout`: the overlay window (`$4E26`-`$30D46`) and the extent of the resident ("game") context. The disk layout is the old one (resident below `$54E00`); the CDTV layout makes the resident context all of chip RAM except the window, because X2's main code lies above the overlay area. A new `on_file_load` hook (called from the CDTV file service when a `Read` lands in the window) starts a module named after the disc file, keyed by a hash of the first 8 KB, exactly as `on_disk_load` does for the disk build's modules. |
| Generator (`tools/recomp/recomp.py --build cdtv`) | A build descriptor: the resident image (`game_0400.bin` at `$400`) with the overlay window as a **hole** (never decoded, never seeded: its contents change at run time), and one image per overlay file: the file as read, with `$406` added to every longword its relocation table lists (the way X2's own relocator `$3CEEE` does; section 2). Modules are `game`, `level1`..`level5`, `intro`, `piccode`. Outputs go to `generated/recomp-cdtv` with `x2rc_` file and function names and the module set `x2r_modules_cdtv`, so the disk and CDTV translations coexist in one build. The disk output is unchanged (regenerated and diffed against the previous translation: identical). |
| Seeds | From traces (`tools/trace/run_corpus.py --game cdtv`: the 23 replays) and statically, with no code map: the entry (`$400`), the level-3 handler (`$34FB2`) and the stand-in routine (`$350C4`), every slot of the 56-entry engine API table (`$E12`) that points into code, and the `bra.w` entry stubs in the 60-byte header of each overlay. |
| Executor (`runtime/recomp`) | `Target { Disk, Cdtv }`: `enable()` dispatches from the chosen module set. The dispatch itself was already per-instruction and byte-verified: the table maps each PC to a list of candidate translations (one per module that has an instruction there), the first whose instruction words match memory runs, and the module that matched is tried first next time. Overlays that share the window therefore need no explicit key at run time: an instruction is only executed translated if its opcode and extension words (relocated pointers included) are the bytes in memory. |
| Front end (`main.cpp`) | The target follows the game being run. A build that carries only the other game's translation runs this one on the interpreter and says so. |
| CMake | `X2_RECOMP_DIR` (disk) and `X2_RECOMP_CDTV_DIR`; at least one must have been generated; an absent set is an empty stand-in (`runtime/recomp/empty_modules_*.cpp`). The disk equivalence tests are only registered when the disk set is linked, `recomp_cdtv_*` only when the CDTV set is. |
| Scripts | `tools/build_all.sh` and `build_all.ps1` `--recomp` translate whichever releases were imported (`assets`, `assets-cdtv`) and link what was generated. |

## 2. The overlay image the generator builds is the one in memory

Translated code verifies every instruction word against memory, so a wrong static overlay image would not break anything: it would
silently fall back to the interpreter. The generator's image was therefore checked against the running game
(`tools/recomp/test_cdtv_build.py --assets ... --exe ...`, ctest `recomp_cdtv_overlay_image`): `--dump-ram-at` a little after each overlay
loads, compare the overlay area with the statically relocated file.

| Overlay | Relocations (table, = C0) | Bytes differing from the running game |
|---|---:|---:|
| LEVEL1 | 682 | 0 of 118,347 |
| LEVEL2 | 900 | 0 of 131,487 |
| LEVEL3 | 976 | 0 |
| LEVEL4 | 1,043 | 0 of 178,589 |
| LEVEL5 | 1,110 | 0 of 161,445 |
| INTRO | 365 | 0 of 95,816 |
| PICCODE | 14 | not loaded in the corpus: checked by relocation count only |

Notes. (a) The first fix-up is at text offset 0, so a leading zero long means "fix-up at 0", not the Atari "no relocations": the first
version of the generator skipped it, every relocated long in LEVEL1 was `$406` short, and the comparison caught it. (b) Later in play
the game writes its own state into the data at the end of the text (LEVEL2 49 bytes, LEVEL4 6, LEVEL5 2 or 21 depending on the frame): data, not
code, so the test compares a few frames after the load. (c) 28-byte file header included: the overlay is read to `$4E26` as a whole and its
text starts at `$4E42`.

## 3. Evidence

All with the CDTV import of the user's own disc, generated and built locally.

**Equivalence (interpreter against translated, per replay).** `recomp_cdtv_<replay>` runs each of the 23 replays twice (`--interp-only`
and hybrid) and compares framebuffer and audio hashes of every frame, the per-frame register-write logs and the final chip RAM, and requires
at least 90% of executed instructions to run as translated code. All 23 pass. The 23 recorded-hash tests (`cdtv_replay_<replay>`) also pass on
the translated build, so the translated code reproduces the manifests recorded from the interpreter.

**Differential test (`recomp_cdtv_difftest`, `x2_recomp_difftest --game cdtv`).** Every translated function, and random mid-function entry
points, run from random machine states (registers, SR, interrupts pending, random data bytes in the images, stack) on both executors,
comparing the whole CPU context, all 1 MB of chip RAM, the custom chip and CIA state, the register-write log, cycles and every data bus
access. A short pass (4 trials, 2 mid-function entries): 563 functions, 3,378 cases, 0 differences, every function ran translated code
(138,443 translated instructions). The default pass is larger (see the test counts in the changelog).

**Coverage** (`tools/recomp/coverage_report.py`; instructions executed, from `--recomp-stats`):

| | |
|---|---|
| Corpus, all 23 replays | 1,226,355,619 instructions, **100.00% translated**, 2 interpreted (`$000EEA` x2 in `level4_flood`) |
| Translated | 563 functions, 17,360 instructions (resident 10,663; LEVEL1 1,052; LEVEL2 1,254; LEVEL3 1,439; LEVEL4 1,355; LEVEL5 1,583; INTRO 7; PICCODE 7) |
| Distinct instructions executed in the corpus | resident 7,417; LEVEL1 918; LEVEL2 1,103; LEVEL3 1,275; LEVEL4 1,282; LEVEL5 1,457; INTRO 0 |

The translation covers more than was executed because a translated function includes every arm its control-flow graph reaches (and the static seeds add
entry points no replay called).

**Coverage on code the translation was not traced on.** A hold-out experiment: traces from 18 replays only (without `boss3`, `level5`,
`shop5`, `two_player`, `game_over`), a translation generated from them (16,659 instructions), and the five held-out replays run on it:
**99.84% translated** overall (`boss3` 100%, `shop5` 100%, `two_player` 100%, `game_over` 100%, `level5` 99.22%: 446,911 interpreted instructions in
object handlers the 18 traced replays never reached). The residual risk the disk build documents applies unchanged: code no trace and no seed
reached runs on the interpreter, which costs speed and never changes a result.

**The interpreter fallback is exact.** A deliberately partial translation (traces of `attract` and `menus` only, no static seeds: 6,222 instructions,
LEVEL1 50 instructions, levels 2-5 none) still gives identical frame/audio hashes every frame, register logs and chip RAM against the interpreter on `level1_play`
(95.6% translated), `level2` (89.7%), `level4_boss` (83.3%), `ending_natural` (92.8%) and `shop` (98.7%).

**One game or both.** Builds with only the CDTV translation (disk game falls back with a message; every `replay_*` of the disk game still matches its hashes) and with only the disk
translation (CDTV game falls back; `cdtv_replay_*` match; the disk `recomp_replay_*` pass) were built and tested.

**Speed.** `level1_flood` (6,000 frames, headless): 7.68 s interpreted against 7.49 s translated (best of three; about 800 frames/s, 16x real time either way): a gain of about
2.5%. As for the disk build, the machine model (blitter, copper, video, accurate timing) dominates the time, not the CPU core: the translated executor is
about equivalence and portability, not speed.

## 4. Coverage gaps (what the corpus does not exercise)

- **INTRO** is loaded 22 times in the corpus (once in most replays, at frame 636 in the menu replays) but no instruction of it is executed in any replay; its 7 translated
  instructions come from the entry stubs. **PICCODE** is not loaded in any replay (what triggers it was not determined): its translation is the 7 stub instructions, and its
  image was checked by relocation count only, not against memory.
- **Level code not reached.** The level-2 and level-3 bosses are reached but not defeated (their death and bonus code is untraced), the level-4 eyes/head
  sweep does not line up in the replay, and the CDTV-only front-end code (language selection, the speech narrator's less common paths) is traced only as far as the 23
  replays go. Those paths interpret until a replay or a profile run (`--recomp-profile`) seeds them.
- **Only one disc.** The translation is keyed to the bytes of this release; another CDTV disc image, language build or pressing (none is known) would translate nothing
  until regenerated, and the code-byte verification makes that safe (interpreter fallback), not wrong.
- **Hand play** is not part of the evidence; the corpus is scripted. The generator and the hold-out experiment are the answer to "what if a player reaches code
  no replay did": it runs on the interpreter.

## 5. Reproducing

```
tools/build_all.sh --recomp --test <disk images> <CDTV disc image>     # imports, traces, generates and builds both translations
# or by hand, with the CDTV import in $C (<data dir>/assets-cdtv):
python3 -I tools/trace/run_corpus.py --game cdtv --exe build/x2recomp --assets "$C" --out build/x2-trace-cdtv
python3 -I tools/recomp/recomp.py --build cdtv --assets "$C" --trace build/x2-trace-cdtv --out build/x2-recomp-cdtv-src
cmake -S . -B build-recomp -G Ninja -DX2_RECOMP=ON -DX2_RECOMP_CDTV_DIR=build/x2-recomp-cdtv-src \
      -DX2_RECOMP_DIR=build/none -DX2_CDTV_GAME_ASSETS="$C"            # a missing X2_RECOMP_DIR: CDTV translation only
python3 -I tools/recomp/coverage_report.py --exe build-recomp/x2recomp --assets "$C" --trace build/x2-trace-cdtv \
      --recomp build/x2-recomp-cdtv-src                                  # per-replay coverage table
```

The generated sources are derived from the disc and are not redistributable (like every `-DX2_RECOMP=ON` build); `tools/check_no_game_data.py` guards
`generated/` and the build folders.
