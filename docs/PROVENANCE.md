# Provenance of the code in this repository

Every tracked file is accounted for below: written for this project, or derived from an external source. For
each derived piece: where it is, what it came from, how close the resemblance is, and the licence of the source.

This file exists because the licence of the project's own code has not been chosen yet, and some of the timing
code is close enough to GPL sources that it constrains that choice. Section 2 is the part that matters for the
decision; section 7 states the consequences.

Nothing here is a legal opinion. It is a record of what was actually written and what it was written from.

## 0. How the resemblance is graded

| Grade | Meaning |
|---|---|
| **verbatim-like transliteration** | Same control flow, same branch order, same identifiers (renamed but one-to-one), same constants. A reader comparing the two side by side would call one a translation of the other. |
| **short-formula transliteration** | The same, but only a handful of lines. |
| **behaviour read from source** | The order and timing of observable events was learned by reading the source. No code structure is shared; the code here does a different job (it places bus cycles for a timing model, not the operation itself). |
| **algorithm from a published description** | Implemented from a specification or an algorithm write-up, not from someone's implementation of it. |
| **facts or constants only** | A number, address, or hardware fact taken from the source. |
| **original** | Written for this project. |

Where the table says "reference emulator", the source read was Amiberry v7.0.8
(`https://github.com/BlitterStudio/amiberry`, a WinUAE port; checked out read-only under
`generated/ref/amiberry-src`, which is gitignored). Amiberry is **GPL-3**; the WinUAE files it carries are
**GPL-2-or-later** per their own headers. Amiberry's `src/newcpu_common.cpp`, `src/blitter.cpp`,
`src/custom.cpp` and `src/cia.cpp` are WinUAE files.

## 1. Summary table of everything derived

| Where | Lines | Nature | Source | Source licence |
|---|---|---|---|---|
| `runtime/machine_timing.cpp` | 227-296 | **verbatim-like transliteration** | Amiberry/WinUAE `src/blitter.cpp`: `blitter_next_cycle()`, `get_current_channel()` | GPL |
| `runtime/machine.h` | 271-281 | **verbatim-like transliteration** (the state the above needs) | Amiberry/WinUAE `src/blitter.cpp` file-scope blitter state | GPL |
| `runtime/machine_timing.cpp` | 41-74 | **verbatim-like transliteration** | Amiberry/WinUAE `src/newcpu_common.cpp`: `getDivu68kCycles()`, `getDivs68kCycles()`, 68000 branches | GPL |
| `runtime/machine_timing.cpp` | 372-376 | **short-formula transliteration** | Amiberry/WinUAE `src/cia.cpp`: `get_cia_sync_cycles()` | GPL |
| `runtime/machine_timing.cpp` | 545-574 | **behaviour read from source** | Amiberry/WinUAE `src/newcpu.cpp`: `Exception_ce000()`, `iack_cycle()` | GPL |
| `runtime/machine_timing.cpp` | 19-29, 157-189 | **facts or constants only** | Amiberry/WinUAE `src/custom.cpp` constants | GPL |
| `runtime/machine.cpp` | 276 | **facts or constants only** | Amiberry/WinUAE `src/custom.cpp`: `blit_busy()` | GPL |
| `runtime/machine_timing.cpp` | 418-455, 576-610 | **facts or constants only** | Yacht cycle-hunting table; Motorola M68000UM; times confirmed by measurement against the reference emulator | tables of hardware facts |
| `runtime/modern/present.cpp` | 88-141 | **algorithm from a published description** | Scale2x/Scale3x algorithm (`scale2x.sourceforge.net`); variable names B/D/E/F/H are from that page | algorithm description (the AdvanceMAME *implementation* is GPL and was not used) |
| `runtime/import/sha1.cpp` | whole file | **algorithm from a published description** | FIPS 180-1 / RFC 3174 | public specification |
| `runtime/cdtv/inflate_stream.cpp` | 19-45, 196 | **algorithm from a published description**, structure follows `vendor/puff` `construct()` | RFC 1951; `vendor/puff/puff.c` | zlib licence |
| `runtime/import/importer.cpp` | 236-324 | **algorithm read from the game's own code** | the decrunch stub in the game's `XENON-II` CODE hunk | the game's code is copyright The Bitmap Brothers / Mirrorsoft; no game bytes are reproduced |
| `tools/unpack_xenon2_disk.py`, `tools/unpack_xenon2.py`, `tools/unpack_bs1.py`, `tools/bkunpack.py` | whole files | **algorithm read from the game's own code** (ByteKiller/ICE-family crunchers) | the decrunch stubs on the game's disks | as above |
| `tools/compare/amiberry-harness/x2ref-hooks.patch` | whole file | **original additions, with GPL context lines** | diff context from Amiberry `src/*.cpp` | GPL (context only) |
| `tools/compare/amiberry-harness/x2ref_harness.inc` | whole file | **original**, but only usable compiled into Amiberry | — | our code; the resulting binary is GPL-combined |
| `vendor/musashi/**` | whole tree | **verbatim copy** of an upstream release | Musashi 4.10, Karl Stenerud | MIT-style |
| `vendor/musashi/softfloat/**` | whole tree | **verbatim copy** (as repackaged by MAME) | SoftFloat Release 2b, John R. Hauser | SoftFloat notice (permissive, requires the notice) |
| `vendor/puff/**` | whole tree | **verbatim copy** | puff 2.3, Mark Adler | zlib licence |
| generated `x2r_handlers.h` (build output, not tracked) | — | **verbatim copy** of Musashi opcode-handler bodies, with the instruction-stream reads specialised | `vendor/musashi` via `tools/recomp/recomp.py` | MIT-style |

Everything not listed above is **original**: see sections 3-6.

## 2. The GPL-derived timing code, in detail

### 2.1 Blitter cycle sequencer — the strongest resemblance

`Machine::blitter_shift()` (`runtime/machine_timing.cpp:227-264`) and `Machine::blitter_channel()`
(`runtime/machine_timing.cpp:267-296`) are line-for-line transliterations of `blitter_next_cycle()` and
`get_current_channel()` in Amiberry's `src/blitter.cpp`. The state they run on (`struct BlitSeq`,
`runtime/machine.h:271-281`) is the same set of file-scope variables.

Evidence — identifiers that exist in both, with the same meaning:

| Here | Reference (`src/blitter.cpp`) |
|---|---|
| `b.shifter[0..3]`, `b.out` | `shifter[0..3]`, `shifter_out` |
| `b.ddatuse` | `ddatuse` |
| `b.finald` | `blt_info.blit_finald` |
| `b.fill_idle` | `blitfill_idle` |
| `b.skip_b`, `b.skip_y` | `shifter_skip_b`, `shifter_skip_y` |
| `b.first` | `shifter_first` |
| `b.hcount`, `b.vcount` | `blitter_hcounter`, `blitter_vcounter` |
| `b.hsize`, `b.vsize` | `blt_info.hblitsize`, `blt_info.vblitsize` |
| `b.counter` | `blit_cyclecounter` |
| `kBlitFinished` (`-1000`) | `CYCLECOUNT_FINISHED` |
| `b.line` | `blitline` |
| `b.con0 & 0x800/0x400/0x200/0x100` | `bltcon0 & BLTCHA/BLTCHB/BLTCHC/BLTCHD` |

The shift stage is identical statement for statement:

```
here  (machine_timing.cpp:248-261)          reference (blitter.cpp:1333-1345)
if (b.skip_b) {                             if (shifter_skip_b) {
    b.shifter[2] = tmp[0];                      shifter[2] = tmp[0];
} else {                                    } else {
    b.shifter[1] = tmp[0];                      shifter[1] = tmp[0];
    b.shifter[2] = tmp[1];                      shifter[2] = tmp[1];
}                                           }
if (b.skip_y) {                             if (shifter_skip_y) {
    out = b.shifter[2];                         out = shifter[2];
} else {                                    } else {
    b.shifter[3] = tmp[2];                      shifter[3] = tmp[2];
    out = b.shifter[3];                         out = shifter[3];
}                                           }
```

`blitter_channel()` returns the same channel codes (0 idle, 1 A, 2 B, 3 C, 4 D, 5 and 6 for line mode) in the
same order, with the same conditions, including the two non-obvious ones the reference comments as "normally
would be D but becomes C" and "D is disabled if position A is non-zero, even if A is disabled".

What is **not** carried over: the mid-blit `BLTCON0`/`BLTCON1` change handling (`blitchanged`), the debug
logging, `blitfc`, and the actual data path (the reference also computes the blitted words; this code only
decides which cycles take the bus).

### 2.2 DIVU/DIVS exact times

`divu_cycles()` and `divs_cycles()` (`runtime/machine_timing.cpp:41-74`) are the 68000 branches of
`getDivu68kCycles()` and `getDivs68kCycles()` in `src/newcpu_common.cpp`, with `mcycles` renamed to `m`, the
68010 branches and the `*extra` out-parameter dropped, `abs()` replaced by explicit negation, and the
reference's trailing `- 4` ("remove prefetch cycle") omitted because this model charges the prefetch
separately. The loop bodies are otherwise identical, down to the `mcycles--` / `--m` inside the inner `if`.

The underlying analysis is credited in both places to Jorge Cwik; the algorithm is published, but this code
was written from the reference's source, not from Cwik's write-up.

### 2.3 CIA E-clock synchronisation

`Machine::cia_cycle()` (`runtime/machine_timing.cpp:372-376`) condenses `get_cia_sync_cycles()`
(`src/cia.cpp:763-779`) into one expression with the same three-way branch:

```
here                                          reference
int div = int(t % 10);                        int div10 = c % DIV10;              // DIV10 == 10 E-cycles
int sync = div < 2   ? 2 - div                if (div10 < synccycle)      add  = synccycle - div10;
         : div > 2   ? 10 - div + 2           else if (div10 > synccycle) add  = DIV10 - div10 + synccycle;
         : 0;                                 // synccycle == e_clock_sync == 2
```

### 2.4 Interrupt exception bus-cycle sequence

`Machine::timing_interrupt()` (`runtime/machine_timing.cpp:545-574`) shares **no code structure** with
`Exception_ce000()` — the reference function performs the exception (stack frame, vector fetch, SR, PC),
while this one only places seven bus cycles and three idle runs on the chip-bus timeline so the CPU's wait
states come out right. What was taken from `src/newcpu.cpp` is the *order and timing* of those cycles:
`start = 6` idle, PC low word, `iack_cycle()` (a 4-cycle read outside chip RAM then 4 idle), SR, PC high word,
the two vector words, prefetch 1, 2 idle, prefetch 2. That ordering is a statement of how the 68000 behaves,
read off a GPL implementation rather than measured on hardware or read from Motorola's documentation.

### 2.5 Constants

Taken as values, with no surrounding code:

| Here | Value | Reference name |
|---|---|---|
| `kRefreshFirst` | 3 | `REFRESH_FIRST_HPOS` |
| `kAudioFirst` | `11 + 3*2` | `DMAL_FIRST_HPOS` + 3 disk slots |
| `kCopperPipeline` | 2 | `RGA_PIPELINE_OFFSET_COPPER` |
| `kNastySteal` | 3 | `BLIT_NASTY_CPU_STEAL_CYCLE_COUNT` |
| copper runs on even clocks | — | `COPPER_CYCLE_POLARITY 0` |
| DMACONR latch delay | 1 cycle | `blit_busy()` (`machine.cpp:276`) |
| expected blit cycle counts in the test | — | `blit_cycle_diagram` (`tests/timing_tests.cpp:99`) |

`kLoresSlot` / `kHiresSlot` (the bitplane fetch order inside a fetch unit) are from the Amiga Hardware
Reference Manual figure 6-9, not from the reference emulator.

## 3. `runtime/` — file by file

Original unless the "derived" column says otherwise.

| Path | Derived from |
|---|---|
| `main.cpp`, `boot.cpp/.h`, `persist.cpp/.h`, `debug_dump.cpp/.h`, `disk_shim.cpp/.h` | — |
| `machine.cpp` | one constant, §2.5 |
| `machine.h` | `struct BlitSeq`, §2.1 |
| `machine_timing.cpp` | §2.1-2.5 (the rest of the file — the DMA slot maps, copper slots, `chip_cycle`, `bus_shape`, `insn_words`, `div_delta`, `insn_delta`, `timing_insn` — is original) |
| `cia.cpp/.h`, `custom_regs.cpp/.h` | — (register behaviour from the Hardware Reference Manual and from measurement against the reference emulator's observable output) |
| `x2_m68kconf.h` | Musashi's configuration contract (option names only); MIT-style |
| `audio/paula.cpp/.h` | — . The A500/A1200 filter models are an independent implementation: an RBJ-cookbook biquad for the LED filter against the reference's cascade of one-pole RC sections. Only the hardware facts are shared (fixed ~5 kHz RC low-pass on A500, switchable LED filter, CIA-A PRA bit 1 selects it). |
| `audio/sdl_audio.cpp/.h`, `audio/wav_writer.cpp/.h`, `audio/audio_cues.cpp/.h` | — |
| `gfx/video.cpp/.h`, `gfx/sdl_display.cpp/.h`, `gfx/sdl_filters.cpp/.h`, `gfx/image_io.cpp/.h`, `gfx/pacer.h` | — . PNG writing/reading follows the PNG specification and uses `vendor/puff` to inflate. |
| `input/input.cpp/.h` | — |
| `import/importer.cpp/.h` | the game's decrunch stub (§1, last-but-four row); ZIP and ADF/OFS parsing from the published formats |
| `import/sha1.cpp/.h` | FIPS 180-1 / RFC 3174 |
| `import/import_screen.cpp/.h` | — |
| `modern/present.cpp/.h` | `scale2x`/`scale3x`, §1 |
| `modern/interp.cpp/.h`, `modern/assets.cpp/.h`, `modern/draw_capture.cpp/.h` | — . PNG decoding from the PNG specification, inflate by `vendor/puff`. |
| `cdtv/inflate_stream.cpp/.h` | RFC 1951; the Huffman table construction follows `vendor/puff`'s `construct()` (zlib licence) |
| `cdtv/cd_player.cpp/.h`, `cdtv/cdtv_disc.cpp/.h`, `cdtv/cdtv_music.cpp/.h` | — . CRC-32 and ISO 9660 / cue-sheet parsing from the published formats. |
| `recomp/executor.cpp`, `recomp/recomp.h`, `recomp/x2r_api.h` | — |
| `recomp/x2r_runtime.h` | includes Musashi's internal headers and redirects its instruction-stream reads; MIT-style |
| `trace/tracer.cpp/.h` | — |

## 4. `tests/` — file by file

All original. `tests/timing_tests.cpp:99-106` documents expected blit cycle counts cross-checked against the
reference's `blit_cycle_diagram` (facts only, §2.5).

`tests/replays/*.txt` are input scripts written for this project. `tests/replays/*.hashes`,
`tests/golden/*.txt` and `tests/cdtv/golden/*` are hashes and cue lists recorded from this runtime — they are
one-way digests, not game data.

## 5. `tools/` — file by file

| Path | Derived from |
|---|---|
| `adf.py`, `hunk.py`, `iso9660.py` | the published AmigaDOS OFS/FFS, AmigaDOS hunk and ISO 9660 formats |
| `bkunpack.py`, `unpack_bs1.py`, `unpack_xenon2.py`, `unpack_xenon2_disk.py` | the decrunch stubs on the game's own disks (ByteKiller- and ICE-family crunchers). The algorithms were read out of the game's 68000 code; no game bytes are reproduced. |
| `analyze68k.py`, `dis68k.py`, `recomp/m68kdecode.py`, `recomp/isa_report.py` | — (the 68000 instruction encodings are from Motorola's M68000 Programmer's Reference Manual) |
| `recomp/recomp.py`, `recomp/opdump.c` | — . They *generate* code containing Musashi handler bodies: see §6. |
| `ghidra/X2Prep.java`, `ghidra/X2Export.java`, `ghidra_x2.py` | — (they call Ghidra's scripting API; Ghidra itself is Apache-2.0 and is not vendored here) |
| `compare/amiberry-harness/x2ref_harness.inc` | — , but see §6 |
| `compare/amiberry-harness/x2ref-hooks.patch` | our additions; carries three unchanged context lines per hunk from Amiberry's GPL sources |
| `compare/amiberry-harness/build.sh`, `run.sh`, `x2ref.uae.in`, `inputs/*` | — (`build.sh` clones Amiberry from upstream; nothing is vendored) |
| `compare/*.py`, `trace/*.py`, `modern/analyze_draw_log.py` | — |
| `build_all.sh`, `build_all.ps1`, `package.sh`, `package.ps1`, `check_no_game_data.py`, `prep_assets.py`, `catalog_sources.py`, `cdtv_analyze.py`, `cdtv_tracks.py`, `build_entrypoints.py`, `merge_entrypoints.py` | — |

## 6. Build system, packaging, CI, and what ends up in a binary

`CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/ci.yml`, `.gitattributes`, `.gitignore`,
`packaging/Info.plist.in`, `packaging/make_icon.py` and `packaging/LICENSE-TODO.txt` are original.
`packaging/licenses/*` are verbatim licence texts of the third-party components. The app icon
(`packaging/make_icon.py`) is a geometric arrow drawn by that script — no game art.

`.github/workflows/ci.yml` calls three published GitHub Actions (`actions/checkout@v4`,
`actions/upload-artifact@v4`, `msys2/setup-msys2@v2`, all MIT). They run on GitHub's infrastructure and are
not part of the repository or of any build output.

What a built binary contains:

- **Default build (`x2recomp`)**: this project's code, plus Musashi (MIT-style), SoftFloat (notice licence,
  reached through Musashi's FPU code), `vendor/puff` (zlib), and SDL3 (zlib) linked or bundled. Also
  `runtime/machine_timing.cpp`, which carries the GPL-derived code of §2.
- **`-DX2_RECOMP=ON` build**: the above, plus `generated/recomp/x2r_handlers.h` — Musashi's opcode-handler
  bodies copied verbatim by `tools/recomp/recomp.py` with the instruction-stream reads specialised — and
  `x2r_*.cpp`, which is translated from *your* game image. That build is never packaged (`tools/package.sh`
  refuses it) and must not be distributed.
- **The Amiberry harness** (`tools/compare/amiberry-harness`) is a developer tool. `x2ref_harness.inc` is our
  own code, but it is compiled into Amiberry, so the *resulting Amiberry binary* is a GPL combined work. It is
  not part of any release and `build.sh` fetches Amiberry from upstream rather than vendoring it.

`vendor/musashi/m68k_in.c` is processed by `m68kmake` at build time into `m68kops.c/h`; those outputs are
Musashi-derived and gitignored.

## 7. What this means for the project's licence

The project's own code is not yet licensed (see `README.md` and `packaging/LICENSE-TODO.txt`). The GPL-derived
code in §2 is the constraint on that choice.

**As it stands, `x2recomp` binaries are a combined work with GPL code.** Shipping them under a permissive
licence would not be honest about §2.1-2.3. The two defensible positions are:

1. **License the project GPL-3-or-later** (compatible with both Amiberry's GPL-3 and WinUAE's GPL-2-or-later).
   Costs nothing in code. Note Musashi, SoftFloat, puff and SDL3 are all GPL-compatible, so nothing else
   blocks this.
2. **Rewrite §2.1-2.3 so no GPL-derived code remains**, then license permissively.

### Cost of option 2

| Piece | What has to happen | Effort | Do goldens change? |
|---|---|---|---|
| `divu_cycles`, `divs_cycles` (34 lines) | Re-derive from Motorola's published DIVU/DIVS timing or from Cwik's public write-up, without reading the reference. The *results* must stay identical or replay hashes move. | small, and verifiable: the cycle counts are a function of two operands, so an exhaustive or randomised equivalence test against the current code proves the rewrite is behaviour-identical | **No**, if the rewrite is behaviour-identical |
| `cia_cycle` (5 lines) | Re-derive the E-clock sync from the 68000/6800 bus documentation. Trivially small and the behaviour is fully specified by "wait for E phase 2". | trivial | **No** |
| `blitter_shift`, `blitter_channel`, `struct BlitSeq` (~90 lines) | **The hard one.** This would have to be rebuilt from the Hardware Reference Manual's blitter description plus measurement — the reference's shift-register model encodes behaviour (the C-instead-of-D case, the A-suppresses-D case, line-mode channels 5/6, fill-mode idle cycles) that the HRM does not state. Realistically this means a clean-room re-derivation driven by the blit cycle-count tests. | **large**, and the risk is real: a different but not identical model changes which cycles the blitter takes, which changes CPU wait states, which changes RNG and so frame hashes | **Likely yes** — any behavioural difference shifts `tests/replays/*.hashes`, `tests/golden/*`, and `tests/cdtv/golden/*`. They would have to be re-recorded, and the Amiberry comparison (`docs/re-notes/reference-compare.md`) re-run to show the new model is no worse. |
| `timing_interrupt` (§2.4) | Weakest claim of the four — no code structure is shared, only an ordering of bus cycles. Could be re-stated from Motorola's exception-processing documentation. | small | **No** if the ordering is unchanged |
| Constants (§2.5) | Individual hardware numbers; re-citable from the HRM or from measurement. Not a practical obstacle. | trivial | **No** |

A third option exists and is worth naming: keep the GPL-derived blitter sequencer but put it behind the
`--timing fast` / `--timing accurate` split that already exists, in a separately-licensed file. That does not
actually help — the default build still links it, so the binary is still a combined work. It would only help
if accurate timing became an opt-in build, which would mean shipping a runtime that does not match the
recorded goldens.

**Recommendation: option 1 (GPL-3-or-later).** Option 2's only genuinely expensive item is the blitter
sequencer, and that sequencer is why the timing model matches the reference emulator closely enough for the
replay hashes to be stable. Rewriting it to be permissively licensable risks the project's main correctness
claim for a licensing preference. This is the maintainer's call, not a technical one.

## 8. How this inventory was produced

- `git ls-files` enumerated all 239 tracked files; every one is in §3-§6.
- Every tracked source file was grepped for the names of other projects (`winuae`, `amiberry`, `uae`,
  `musashi`, `mame`, `vice`, `cwik`, `ghidra`, `puff`, `softfloat`, `stb_`, `public domain`, `transliterat`).
  Only the files listed above mention any.
- For every timing, blitter, copper, CIA, Paula and bus function, the corresponding reference function was
  read in `generated/ref/amiberry-src/src` and compared side by side. The identifier tables and line pairs in
  §2 are that comparison.
- `vendor/` notices were read from the vendored trees themselves
  (`vendor/musashi/readme.txt`, `vendor/musashi/softfloat/README.txt`, `vendor/puff/LICENSE`).
- No comment in the tree claims derivation from MAME, VICE, FS-UAE, or any emulator other than WinUAE and
  Amiberry; the only MAME connection is that `vendor/musashi/softfloat` is MAME's repackaging of SoftFloat and
  that Musashi has historically been MAME's 68000 core.
