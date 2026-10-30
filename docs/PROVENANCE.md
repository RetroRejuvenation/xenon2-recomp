# Provenance of the code in this repository

Every tracked file is accounted for below: written for this project, or derived from an external source. For
each derived piece: where it is, what it came from, how close the resemblance is, and the licence of the source.

This file exists because some of the accurate-timing code is close enough to GPL sources to constrain the
project's licence. **That is why xenon2-recomp is GPL-3.0-or-later** (see `LICENSE`): section 2 is the code
in question, and section 7 records the compatibility reasoning.

Nothing here is a legal opinion — nobody involved in writing it is a lawyer. It is a record of what was
actually written, what it was written from, and which licence statements were read in the sources. Section 7
flags the points where real uncertainty remains rather than asserting compliance.

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
`generated/ref/amiberry-src`, which is gitignored). Amiberry's own `LICENSE` is the GPL-3 text.

The exact licence statements in the files that were read are set out in §7.1 — they are **not** uniform, and
the summary "Amiberry is GPL-3, its UAE files are GPL-2-or-later" turned out to be too simple to rely on.

## 1. Summary table of everything derived

| Where | Lines | Nature | Source | Source licence |
|---|---|---|---|---|
| `runtime/machine_timing.cpp` | 229-298 | **verbatim-like transliteration** | Amiberry/WinUAE `src/blitter.cpp`: `blitter_next_cycle()`, `get_current_channel()` | **GPL-3** via Amiberry's `LICENSE`; no per-file grant (§7.1) |
| `runtime/machine.h` | 273-283 | **verbatim-like transliteration** (the state the above needs) | Amiberry/WinUAE `src/blitter.cpp` file-scope blitter state | **GPL-3** as above |
| `runtime/machine_timing.cpp` | 43-76 | **verbatim-like transliteration** | Amiberry/WinUAE `src/newcpu_common.cpp`: `getDivu68kCycles()`, `getDivs68kCycles()`, 68000 branches | **GPL-2-or-later** (explicit, §7.1) |
| `runtime/machine_timing.cpp` | 374-378 | **short-formula transliteration** | Amiberry/WinUAE `src/cia.cpp`: `get_cia_sync_cycles()` | **GPL-3** as above |
| `runtime/machine_timing.cpp` | 547-576 | **behaviour read from source** | Amiberry/WinUAE `src/newcpu.cpp`: `Exception_ce000()`, `iack_cycle()` | **GPL-3** as above |
| `runtime/machine_timing.cpp` | 21-31, 159-191 | **facts or constants only** | Amiberry/WinUAE `src/custom.cpp` constants | **GPL-3** as above |
| `runtime/machine.cpp` | 278 | **facts or constants only** | Amiberry/WinUAE `src/custom.cpp`: `blit_busy()` | **GPL-3** as above |
| `runtime/machine_timing.cpp` | 420-457, 578-612 | **facts or constants only** | Yacht cycle-hunting table; Motorola M68000UM; times confirmed by measurement against the reference emulator | tables of hardware facts |
| `runtime/modern/present.cpp` | 90-143 | **algorithm from a published description** | Scale2x/Scale3x algorithm (`scale2x.sourceforge.net`); variable names B/D/E/F/H are from that page | algorithm description (the AdvanceMAME *implementation* is GPL and was not used) |
| `runtime/import/sha1.cpp` | whole file | **algorithm from a published description** | FIPS 180-1 / RFC 3174 | public specification |
| `runtime/cdtv/inflate_stream.cpp` | 19-45, 196 | **algorithm from a published description**, structure follows `vendor/puff` `construct()` | RFC 1951; `vendor/puff/puff.c` | zlib licence |
| `runtime/import/importer.cpp` | 238-326 | **algorithm read from the game's own code** | the decrunch stub in the game's `XENON-II` CODE hunk | the game's code is copyright The Bitmap Brothers / Mirrorsoft; no game bytes are reproduced |
| `tools/unpack_xenon2_disk.py`, `tools/unpack_xenon2.py`, `tools/unpack_bs1.py`, `tools/bkunpack.py` | whole files | **algorithm read from the game's own code** (ByteKiller/ICE-family crunchers) | the decrunch stubs on the game's disks | as above |
| `tools/compare/amiberry-harness/x2ref-hooks.patch` | whole file | **original additions, with GPL context lines** | diff context from Amiberry `src/*.cpp` | GPL (context only) |
| `tools/compare/amiberry-harness/x2ref_harness.inc` | whole file | **original**, but only usable compiled into Amiberry | — | ours, GPL-3.0-or-later; the Amiberry binary built from it is a GPL combined work |
| `vendor/musashi/**` | whole tree | upstream release **with three local modifications** (`m68kmake.c`, `m68k_in.c`, `m68kcpu.h`; all marked in place, see §9) | Musashi 4.10, Karl Stenerud | MIT-style (permits modification; notice retained) |
| `vendor/musashi/softfloat/**` | whole tree | **verbatim copy** (as repackaged by MAME), unmodified; **headers only are compiled, none of it is linked or shipped** (§7.3) | SoftFloat Release 2b, John R. Hauser | SoftFloat notice (permissive, requires the notice) |
| `vendor/puff/**` | whole tree | **verbatim copy**, unmodified | puff 2.3, Mark Adler | zlib licence |
| generated `x2r_handlers.h` (build output, not tracked) | — | **verbatim copy** of Musashi opcode-handler bodies, with the instruction-stream reads specialised | `vendor/musashi` via `tools/recomp/recomp.py` | MIT-style |

Everything not listed above is **original**: see sections 3-6.

## 2. The GPL-derived timing code, in detail

### 2.1 Blitter cycle sequencer — the strongest resemblance

`Machine::blitter_shift()` (`runtime/machine_timing.cpp:229-266`) and `Machine::blitter_channel()`
(`runtime/machine_timing.cpp:269-298`) are line-for-line transliterations of `blitter_next_cycle()` and
`get_current_channel()` in Amiberry's `src/blitter.cpp`. The state they run on (`struct BlitSeq`,
`runtime/machine.h:273-283`) is the same set of file-scope variables.

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
here  (machine_timing.cpp:251-262)          reference (blitter.cpp:1333-1345)
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

`divu_cycles()` and `divs_cycles()` (`runtime/machine_timing.cpp:43-76`) are the 68000 branches of
`getDivu68kCycles()` and `getDivs68kCycles()` in `src/newcpu_common.cpp`, with `mcycles` renamed to `m`, the
68010 branches and the `*extra` out-parameter dropped, `abs()` replaced by explicit negation, and the
reference's trailing `- 4` ("remove prefetch cycle") omitted because this model charges the prefetch
separately. The loop bodies are otherwise identical, down to the `mcycles--` / `--m` inside the inner `if`.

The underlying analysis is credited in both places to Jorge Cwik; the algorithm is published, but this code
was written from the reference's source, not from Cwik's write-up.

### 2.3 CIA E-clock synchronisation

`Machine::cia_cycle()` (`runtime/machine_timing.cpp:374-378`) condenses `get_cia_sync_cycles()`
(`src/cia.cpp:763-779`) into one expression with the same three-way branch:

```
here                                          reference
int div = int(t % 10);                        int div10 = c % DIV10;              // DIV10 == 10 E-cycles
int sync = div < 2   ? 2 - div                if (div10 < synccycle)      add  = synccycle - div10;
         : div > 2   ? 10 - div + 2           else if (div10 > synccycle) add  = DIV10 - div10 + synccycle;
         : 0;                                 // synccycle == e_clock_sync == 2
```

### 2.4 Interrupt exception bus-cycle sequence

`Machine::timing_interrupt()` (`runtime/machine_timing.cpp:547-576`) shares **no code structure** with
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
| DMACONR latch delay | 1 cycle | `blit_busy()` (`machine.cpp:278`) |
| expected blit cycle counts in the test | — | `blit_cycle_diagram` (`tests/timing_tests.cpp:101`) |

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
| `import/disk_image.cpp/.h`, `import/ext_adf.cpp/.h` | — . The extended ADF layout is from WinUAE's `disk.cpp` description (a file-format fact, no code); the Amiga MFM sector format (sync, odd/even long splitting, header and data checksums) is from the Amiga Hardware Reference Manual. |
| `import/ipf.cpp/.h` | a port of MAME's `src/lib/formats/ipf_dsk.cpp` (BSD-3-Clause, Olivier Galibert) by way of `tools/ipf.py`: the IPF record layouts, the data and gap description decoding (gap types 0-3), the track splice and the per-type cell timings follow that file; the structure, bounds checking and the conversion to `DiskImage` tracks are our own. The SPS `capsimg` library is not used and its source was not read. Licence statement: `THIRD-PARTY-NOTICES.md`. |
| `import/dms.cpp/.h` | the DMS format as implemented by xDMS (public domain; reference only, no code copied): header and track layouts, CRC-16/ARC, the RLE, QUICK, MEDIUM, DEEP (LZHUF-style adaptive Huffman) and HEAVY (LHA-style canonical Huffman tables) algorithms. The two 256-entry distance tables (`kDCode`, `kDLen`) are the standard LZHUF static tables, as in xDMS. Our code is a separate implementation; see `docs/re-notes/dms.md` for how it was checked. |
| `import/flux.cpp/.h`, `import/scp.cpp`, `import/kryoflux.cpp`, `import/hfe.cpp` | — . The SCP layout is from the SuperCard Pro image format specification, the KryoFlux stream encoding from the KryoFlux stream protocol document (rev 1.1), and the HFE layout from the HxC floppy emulator file format description; these are file-format facts, no code was taken from any of them or from any other flux tool. The PLL is a standard first-order digital phase-locked loop written for this project; its gains were tuned on synthetic flux (`docs/re-notes/flux.md`). |
| `import/lha.cpp/.h` | the LHA/LZH format from its public descriptions (header levels 0-3 and the extended header chain, the -lh4- to -lh7- block structure with its temporary, code and offset tables, canonical Huffman code assignment, CRC-16/ARC) and, for -lh1-, the adaptive Huffman tree maintenance (frequency-ordered nodes in groups, tree rebuild at 32 KB, the static 64-entry offset-prefix code) as implemented in lhasa by Simon Howard (ISC licence, notice in `THIRD-PARTY-NOTICES.md`): the algorithm and the format constants follow lhasa's `lh1_decoder.c`; the code here is our own. The OS-9/68k level-2 header length quirk and the LHARK `-lh7-` marker (level 1, OS type `' '`) are lhasa's findings too. Checked against lhasa's regression archives from many archivers |
| `import/whdload.cpp/.h` | the WHDLoad slave structure (hunk header, `moveq #-1,d0 ; rts`, `WHDLOADS`, `ws_name`/`ws_copy`/`ws_info`) from the WHDLoad developer documentation as seen in the public Xenon 2 slave and imager sources (`Xenon2HD.s`, by Wepl, public domain), whose boot-block CRC-16 table gives the six floppy versions |
| `import/import_screen.cpp/.h` | — |
| `modern/present.cpp/.h` | `scale2x`/`scale3x`, §1 |
| `modern/interp.cpp/.h`, `modern/assets.cpp/.h`, `modern/draw_capture.cpp/.h` | — . PNG decoding from the PNG specification, inflate by `vendor/puff`. |
| `cdtv/inflate_stream.cpp/.h` | RFC 1951; the Huffman table construction follows `vendor/puff`'s `construct()` (zlib licence) |
| `cdtv/cd_player.cpp/.h`, `cdtv/cdtv_disc.cpp/.h`, `cdtv/cdtv_music.cpp/.h` | — . CRC-32 and ISO 9660 / cue-sheet parsing from the published formats. |
| `recomp/executor.cpp`, `recomp/recomp.h`, `recomp/x2r_api.h` | — |
| `recomp/x2r_runtime.h` | includes Musashi's internal headers and redirects its instruction-stream reads; MIT-style |
| `musashi_no_fpu.c` | — (replaces `vendor/musashi/softfloat/softfloat.c` so no SoftFloat is linked; §7.3) |
| `trace/tracer.cpp/.h` | — |

## 4. `tests/` — file by file

All original. `tests/import/container_fixtures.h` and `tests/import/flux_fixtures.h` hold test-side encoders (DMS modes, Amiga MFM, extended ADF, synthetic flux, SCP, KryoFlux stream and HFE files) and `tests/import/ipf_fixtures.h` a test-side IPF writer, all written for this project from the format descriptions. `tests/timing_tests.cpp:101-108` documents expected blit cycle counts cross-checked against the
reference's `blit_cycle_diagram` (facts only, §2.5).

`tests/replays/*.txt` are input scripts written for this project. `tests/replays/*.hashes`,
`tests/golden/*.txt` and `tests/cdtv/golden/*` are hashes and cue lists recorded from this runtime — they are
one-way digests, not game data.

## 5. `tools/` — file by file

| Path | Derived from |
|---|---|
| `adf.py`, `hunk.py`, `iso9660.py` | the published AmigaDOS OFS/FFS, AmigaDOS hunk and ISO 9660 formats |
| `bkunpack.py`, `unpack_bs1.py`, `unpack_xenon2.py`, `unpack_xenon2_disk.py` | the decrunch stubs on the game's own disks (ByteKiller- and ICE-family crunchers). The algorithms were read out of the game's 68000 code; no game bytes are reproduced. |
| `ipf.py` | a port of MAME's `src/lib/formats/ipf_dsk.cpp` (BSD-3-Clause), see the `import/ipf` row in §3 and `THIRD-PARTY-NOTICES.md` |
| `mfm.py`, `copylock.py`, `retail_game_image.py` | the Amiga MFM sector format (Amiga Hardware Reference Manual) and the Rob Northen Copylock track structure as publicly documented; our code |
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
`packaging/Info.plist.in`, `packaging/make_icon.py` and `packaging/licenses/README.txt` are original.
`packaging/licenses/*` are verbatim licence texts of the third-party components. The app icon
(`packaging/make_icon.py`) is a geometric arrow drawn by that script — no game art.

`.github/workflows/ci.yml` calls three published GitHub Actions (`actions/checkout@v4`,
`actions/upload-artifact@v4`, `msys2/setup-msys2@v2`, all MIT). They run on GitHub's infrastructure and are
not part of the repository or of any build output.

What a built binary contains:

- **Default build (`x2recomp`)**: this project's code, plus Musashi (MIT-style), `vendor/puff` (zlib), and
  SDL3 (zlib) linked or bundled. Also `runtime/machine_timing.cpp`, which carries the GPL-derived code of §2.
  **No SoftFloat** — `runtime/musashi_no_fpu.c` replaces it (§7.3).
- **`-DX2_RECOMP=ON` build**: the above, plus `generated/recomp/x2r_handlers.h` — Musashi's opcode-handler
  bodies copied verbatim by `tools/recomp/recomp.py` with the instruction-stream reads specialised — and
  `x2r_*.cpp`, which is translated from *your* game image. That build is never packaged (`tools/package.sh`
  refuses it) and must not be distributed.
- **The Amiberry harness** (`tools/compare/amiberry-harness`) is a developer tool. `x2ref_harness.inc` is our
  own code, but it is compiled into Amiberry, so the *resulting Amiberry binary* is a GPL combined work. It is
  not part of any release and `build.sh` fetches Amiberry from upstream rather than vendoring it.

`vendor/musashi/m68k_in.c` is processed by `m68kmake` at build time into `m68kops.c/h`; those outputs are
Musashi-derived and gitignored. Both of those files carry local modifications — see §9.

## 7. The licence, and why

**xenon2-recomp is licensed GPL-3.0-or-later** (`LICENSE`, the unmodified FSF text of GNU GPL version 3).
Every original source file carries `SPDX-License-Identifier: GPL-3.0-or-later`.

The reason is §2: the accurate-timing code contains transliterations of GPL code from WinUAE/Amiberry. A
binary built from this tree is a combined work with that code, so it has to be distributed under
GPL-compatible terms. A permissive licence was considered and rejected; §7.4 records what it would have cost.

### 7.1 Licence statements actually found in the sources that were read

This matters, so it was checked file by file rather than assumed. Amiberry carries no uniform per-file
licence header.

| Reference file | Derived here as | Licence statement in the file | Usable under GPL-3-or-later? |
|---|---|---|---|
| `src/newcpu_common.cpp`, the block immediately above `getDivu68kCycles` (lines ~680-699) | `divu_cycles`, `divs_cycles` (§2.2) | **Explicit**: "Copyright (c) 2005 by Jorge Cwik, pasti@fxatari.com … under the terms of the GNU General Public License … **either version 2 of the License, or (at your option) any later version**" | **Yes** — "or any later version" permits use under GPL-3 |
| `src/blitter.cpp` | `blitter_shift`, `blitter_channel`, `BlitSeq` (§2.1) | **None.** Header is `UAE - The Un*x Amiga Emulator / Custom chip emulation / (c) 1995 Bernd Schmidt, Alessandro Bissacco / (c) 2002-2021 Toni Wilen`. No licence sentence anywhere in the file. | Yes, via Amiberry's project `LICENSE` (GPL-3) — but see §7.2 |
| `src/cia.cpp` | `cia_cycle` (§2.3) | **None** — copyright lines only (Bernd Schmidt, Alessandro Bissacco, Stefan Reinauer, Christian Schmitt, Toni Wilen) | as above |
| `src/newcpu.cpp` | `timing_interrupt` ordering (§2.4) | **None** — copyright lines only | as above |
| `src/custom.cpp` | constants (§2.5) | **None** — copyright lines only | as above |

**No file that was read states "version 2 only."** That was specifically checked, because a GPL-2-only
source could not be combined into a GPL-3 work. The files in the tree that do say version 2 without an "or
later" grant are all under `src/ppc/pearpc/**` (the PearPC PowerPC core) plus a few unrelated files; none of
them were read and none contributed anything here. That is an Amiberry-internal question, not one for this
project.

### 7.2 Where real uncertainty remains

Flagged rather than papered over:

1. **The four files with no licence header.** `blitter.cpp`, `cia.cpp`, `newcpu.cpp` and `custom.cpp` carry
   copyright notices but no licence grant. The grant therefore comes from Amiberry's project-level `LICENSE`,
   which is the GPL-3 text with **no "or later" sentence of its own**. So the safe reading is that this
   material reaches us as **GPL-3**, not GPL-3-or-later. That is still compatible with distributing the
   combined work under GPL-3-or-later — a recipient can always rely on the version 3 terms — but it means
   the *effective floor* for a binary of this project is GPL-3, and a hypothetical future GPL-4 relicensing
   of the whole work could not be done on the strength of our own "or later" grant alone.
2. **WinUAE's own terms were not verified.** These files originate in WinUAE, and only the Amiberry
   redistribution was available here (`generated/ref/amiberry-src`). WinUAE's own distribution terms for
   them were not read, so the statement "the WinUAE files are GPL-2-or-later" — which an earlier draft of
   this document asserted — is **not supported by anything checked** and has been removed. If the upstream
   terms matter for some future decision, read WinUAE's own release rather than inferring from Amiberry.
3. **Whether §2.4 and §2.5 are copyrightable at all.** An ordering of bus cycles and a handful of hardware
   constants may well be uncopyrightable fact. Nothing here depends on that being true: they are licensed as
   GPL material anyway, which costs nothing now that the project is GPL.
4. ~~SoftFloat's indemnification wording~~ — **resolved by removal**: SoftFloat is no longer compiled,
   linked or shipped (§7.3). No uncertainty remains from it.

### 7.3 Third-party components: compatibility with GPL-3.0-or-later

| Component | Licence | Compatible? | Note |
|---|---|---|---|
| Musashi 4.10 (`vendor/musashi`) | MIT-style (`readme.txt`, "LICENSE AND COPYRIGHT") | **Yes** | Permissive with notice retention; standard GPL-compatible. The notice is shipped (`packaging/licenses/Musashi-LICENSE.txt`). |
| puff 2.3 (`vendor/puff`) | zlib | **Yes** | Permissive, GPL-compatible. |
| SDL 3 | zlib | **Yes** | Permissive, GPL-compatible. Bundled as a shared library in packages. |
| SoftFloat 2b (`vendor/musashi/softfloat`) | Hauser notice | **Not applicable — removed from the build** | See below. |

#### SoftFloat was removed rather than relied on

Musashi can use SoftFloat for its FPU, and until 2026-10-30 this project compiled and linked it: `softfloat.c.o`
was in `libmusashi.a` and 142 SoftFloat symbols were present in `x2recomp`, even though the game is a 68000
with no FPU at all.

That was removed, because the notice carries a condition that does not sit comfortably under the GPL. Its
requirements for derivative works are ordinary notice-retention — "(1) the source code for the derivative work
includes prominent notice that the work is derivative, and (2) the source code includes prominent notice with
these four paragraphs" — but one of those four paragraphs says "USE OF THIS SOFTWARE IS RESTRICTED TO PERSONS
AND ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ALL LOSSES … AND WHO FURTHERMORE EFFECTIVELY
INDEMNIFY JOHN HAUSER AND THE INTERNATIONAL COMPUTER SCIENCE INSTITUTE … AGAINST ALL LOSSES". Read strictly,
an indemnification obligation and a restriction on *who* may use the software are arguably "further
restrictions" of the kind GPL §7 does not permit to be imposed on downstream recipients. Many GPL projects do
carry SoftFloat 2b (including MAME, from which this copy's packaging comes) and treat it as acceptable, so this
is not a settled problem — but the code was doing nothing for us, so the cheapest resolution was to delete the
question rather than answer it.

**How it was removed, without editing any vendored file.** `runtime/musashi_no_fpu.c` replaces
`vendor/musashi/softfloat/softfloat.c` in the `musashi` CMake target. It defines the 15 SoftFloat symbols that
`m68kfpu.c` names, and nothing else. They are unreachable: Musashi enters its FPU only from the `040fpu0` and
`040fpu1` opcode handlers, both guarded by `CPU_TYPE_IS_030_PLUS(CPU_TYPE)` (`vendor/musashi/m68k_in.c`), and
this runtime calls `m68k_set_cpu_type(M68K_CPU_TYPE_68000)` with `M68K_EMULATE_010/EC020/020/030/040` all
`OPT_OFF`. An F-line opcode therefore takes `m68ki_exception_1111()`, which is what a real 68000 does. The
stubs `abort()` rather than return a plausible value, so enabling a 68030+ CPU type in future fails loudly
instead of silently computing wrong results.

**What this leaves, precisely.** No SoftFloat code is compiled, linked or shipped:

- `softfloat.c` is not compiled, and `softfloat.c.o` is no longer in `libmusashi.a`.
- `softfloat-specialize` is included only by `softfloat.c`, so it is gone entirely.
- All 88 symbols that only SoftFloat's implementation defines are absent from `x2recomp` (verified with `nm`);
  so are its internal helpers (`roundAndPackFloat32`, `normalizeFloatx80Subnormal`, `estimateDiv128To64`,
  `float_exception_flags`, `propagateFloatx80NaN`, …).
- The 15 symbols that remain in the binary carry SoftFloat's *names* — they have to, that is what a stub is —
  but they are defined in `musashi_no_fpu.c.o`, which `nm -o` confirms.

**Why `vendor/musashi/softfloat/` is still in the repository.** `m68kcpu.h` includes `softfloat/milieu.h` and
`softfloat/softfloat.h` unconditionally, so those headers are still needed to compile Musashi at all; removing
the directory would mean editing a vendored upstream file, which was deliberately avoided. What survives from
them is type declarations. `softfloat.h` also includes `softfloat-macros`, which does contain code — 10
`static inline` functions — but none of them is referenced by `m68kfpu.c`, `m68kcpu.c` or the stub, so the
compiler emits nothing for them. The vendored files keep their own unmodified notices
(`vendor/musashi/softfloat/README.txt` and the notice at the top of each file), which satisfies the notice
condition for the parts of SoftFloat this *source tree* retains.

`packaging/licenses/SoftFloat-NOTICE.txt` was therefore deleted: a binary package contains no SoftFloat code,
and shipping a notice for absent code would be misleading. To restore real FPU support, reverse the one CMake
line and put the notice back.

### 7.4 What a permissive licence would have cost

Kept for the record, since the question may come back.

| Piece | What a rewrite needs | Effort | Do goldens change? |
|---|---|---|---|
| `divu_cycles`, `divs_cycles` (34 lines) | Re-derive from Motorola's published DIVU/DIVS timing or Cwik's public write-up without reading the reference. Verifiable: the result is a function of two operands, so an exhaustive equivalence test against the current code proves the rewrite is behaviour-identical. | small | **No**, if behaviour-identical |
| `cia_cycle` (5 lines) | Re-derive the E-clock sync from the 68000/6800 bus documentation; the behaviour is fully specified by "wait for E phase 2". | trivial | **No** |
| `blitter_shift`, `blitter_channel`, `BlitSeq` (~90 lines) | **The hard one.** A clean-room re-derivation from the Hardware Reference Manual plus measurement. The reference's shift-register model encodes behaviour the HRM does not state: the C-instead-of-D case, the A-suppresses-D case, line-mode channels 5 and 6, and fill-mode idle cycles. | **large**, and risky: a different model changes which cycles the blitter takes → CPU wait states → RNG → frame hashes | **Likely yes** — would shift `tests/replays/*.hashes`, `tests/golden/*` and `tests/cdtv/golden/*`, all of which would need re-recording, plus a re-run of `docs/re-notes/reference-compare.md` to show the new model is no worse |
| `timing_interrupt` (§2.4) | Weakest of the four — only an ordering is shared. Re-statable from Motorola's exception-processing documentation. | small | **No** if the ordering is unchanged |
| Constants (§2.5) | Individual hardware numbers, re-citable from the HRM or measurement. | trivial | **No** |

Splitting the blitter sequencer into a separately-licensed file behind `--timing fast` / `--timing accurate`
would not have helped: the default build still links it, so the binary is still a combined work. It would
only help if accurate timing became an opt-in build — which would mean shipping a runtime that does not
match the recorded goldens.

The decision was that rewriting the blitter sequencer would risk the project's main correctness claim in
exchange for a licensing preference, so GPL-3.0-or-later it is.

### 7.5 What the GPL means for users of this project

- The game itself is **not** covered. Xenon 2 Megablast's code, graphics, music and text are copyright
  The Bitmap Brothers / Mirrorsoft. This repository contains none of it, and the GPL here grants no rights
  over it. You supply your own disk images.
- Likewise CDTV CD audio: it comes from your own disc image and is never shipped.
- **A `-DX2_RECOMP=ON` build must not be redistributed.** It contains C++ translated from your game image.
  The GPL applies to this project's code, not to the game's; distributing that build would be distributing
  the game. `tools/package.sh` and `tools/package.ps1` refuse such a build.
- Third-party components keep their own licences (§7.3); the GPL does not replace them.

## 8. How this inventory was produced

- `git ls-files` enumerated all 239 tracked files; every one is in §3-§6.
- Every tracked source file was grepped for the names of other projects (`winuae`, `amiberry`, `uae`,
  `musashi`, `mame`, `vice`, `cwik`, `ghidra`, `puff`, `softfloat`, `stb_`, `public domain`, `transliterat`).
  Only the files listed above mention any.
- For every timing, blitter, copper, CIA, Paula and bus function, the corresponding reference function was
  read in `generated/ref/amiberry-src/src` and compared side by side. The identifier tables and line pairs in
  §2 are that comparison.
- `vendor/` notices were read from the vendored trees themselves
  (`vendor/musashi/readme.txt`, `vendor/musashi/softfloat/README.txt`, `vendor/musashi/softfloat/softfloat.h`,
  `vendor/puff/LICENSE`).
- Every reference file that contributed anything was grepped for its own licence sentence, and the whole
  reference tree was surveyed for `version 2` / `either version 2 … or any later version` wording, to check
  that nothing read was GPL-2-only. §7.1 is that survey.
- SoftFloat's **absence** from the shipped binary was verified rather than assumed: `ar t libmusashi.a` no
  longer lists `softfloat.c.o`, and of the 103 symbols SoftFloat defines, the 88 that only its implementation
  provides are all absent from `nm build/x2recomp`; the 15 that remain resolve to `musashi_no_fpu.c.o`
  (`nm -o`). Before the removal there were 142 SoftFloat symbols in the binary.
- `LICENSE` was checked against two independent GPL-3 copies on the build machine (Homebrew `poppler`
  `COPYING3` and `xz` `COPYING.GPLv3`); see §8.1.
- Vendored trees were checked for local modifications rather than assumed pristine, two ways: `git log` per
  tracked file under `vendor/` for anything with more than its introducing commit, and `grep -rln 'x2recomp:'
  vendor/` for the marker convention. Both agree on the same three files in `vendor/musashi` (§9);
  `vendor/puff` and `vendor/musashi/softfloat` have none. The generated `m68kops.c` md5 was compared before
  and after the MSVC fix to show the tables are unaffected.
- No comment in the tree claims derivation from MAME, VICE, FS-UAE, or any emulator other than WinUAE and
  Amiberry; the only MAME connection is that `vendor/musashi/softfloat` is MAME's repackaging of SoftFloat and
  that Musashi has historically been MAME's 68000 core.

### 8.1 How `LICENSE` was verified

`LICENSE` is the FSF text of GNU GPL version 3, SHA-256
`8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903`, 674 lines, header "GNU GENERAL PUBLIC
LICENSE / Version 3, 29 June 2007", ending with the "GNU Lesser General Public License" paragraph. That hash
is shared by two independent copies shipped on the build machine (Homebrew `poppler`'s `COPYING3` and one
other), which is why that text was used.

**It was not copied from the reference checkout.** `generated/ref/amiberry-src/LICENSE` is *not* the pristine
FSF text: its operative part (the Preamble and all of TERMS AND CONDITIONS, lines 1-621) is byte-identical,
but the non-operative "How to Apply These Terms to Your New Programs" appendix uses GitHub's licence-template
placeholders — `{one line to give the program's name…}`, `{year}`, `{name of author}`, `{project}`,
`{fullname}` — where the FSF text has `<one line…>`, `<year>`, `<name of author>` and `<program>`. The
difference is cosmetic and confined to the appendix, but since this project's own licence text should be the
canonical one, the verified FSF copy was used instead.

## 9. Local modifications to vendored code

`vendor/puff` and `vendor/musashi/softfloat` are unmodified. **`vendor/musashi` is not**: three of its files
carry local changes. Each is marked in place with an `x2recomp:` or `xenon2-recomp:` comment explaining what
upstream wrote and why it was changed, so nothing here is a silent divergence — and that marker is how this
list was compiled (`grep -rln 'x2recomp:' vendor/` returns exactly these three files).

| File | Change | Size | Why | When |
|---|---|---|---|---|
| `vendor/musashi/m68kmake.c` | `populate_table()`: compute `op_mask`/`op_match` bit by bit instead of shifting the result of a comparison | +11 −2 | Works around an MSVC 2022 `/O2` miscompilation (§9.1) | 2026-10-30 |
| `vendor/musashi/m68k_in.c` | `m68ki_build_opcode_table()`: walk the handler table by each entry's own free bits instead of a hard-coded chain of mask-group loops | +18 −87 | Removes the out-of-bounds walk that turned the miscompilation into an access violation (§9.1) | 2026-10-30 |
| `vendor/musashi/m68kcpu.h` | `m68ki_check_bus_error_trap()`/`m68ki_bus_error_longjmp()` use `_setjmp`/`_longjmp` on macOS and the BSDs | +9 −1 | `setjmp()` there saves the signal mask with two system calls on every `m68k_execute()` slice; the address-error trap already avoided this, the bus-error trap did not. Performance only — bus errors are never raised in this game | earlier (commit `a0f4dcb`, Phase 9) |

The third was already described in `docs/PORTING.md` under "Portability rules" as a behaviour of the build,
but had not been recorded here as a modification to vendored code. It is listed now for completeness; §9.1
and §9.2 below concern the two MSVC fixes only.

### 9.1 The MSVC miscompilation

Upstream `m68kmake.c` builds each opcode's mask and match like this:

```c
op->op_mask  |= (bitpattern[i] != '.') << (15-i);
op->op_match |= (bitpattern[i] == '1') << (15-i);
```

MSVC 2022 at `/O2` dropped the `i == 1` term. Every generated mask and match came out with bit 14 clear:
`0xf000` became `0xb000`, `0xff00` became `0xbf00`, `0xffff` became `0xbfff`, and `bra.8`'s match `0x6000`
became `0x2000`. The group *counts* were otherwise identical to a macOS build's, which is what pinned the
fault on that one expression.

It had two consequences, and the second is the dangerous one:

1. The missing bit removed the whole `mask == 0xff00` group that `m68ki_build_opcode_table()`'s first loop
   scans for. That loop is `while(ostruct->mask == 0xff00)`, with no bound, so it ran off the end of the
   array — an access violation in every MSVC test that constructs a `Machine`.
2. Even with the crash fixed, **every opcode would have been decoded against the wrong mask**. An MSVC build
   that merely stopped crashing would have silently run the wrong instruction handlers.

The fix names the bit and branches, which says the same thing but leaves the optimiser nothing to
reassociate. The table walk was rewritten as well, so a malformed or reordered handler table can no longer
walk off the end: enumerating each entry's own free bits is order-independent and needs no sentinel group.

### 9.2 Why this does not change behaviour anywhere else

The generated `m68kops.c` is **byte-identical** before and after on macOS arm64 — md5
`b94fcb0a43c49d6237798bcb062dfac9`, confirmed independently in both this project's build directories. The
table walk produces the same jump table and cycle table for a well-formed table, because a mask's zero bits
are exactly the opcode bits its handler ignores, so `match | <subset of free bits>` visits precisely the
opcodes satisfying `(opcode & mask) == match`. Every replay and golden hash is unchanged.

### 9.3 Licence position

Musashi's licence is MIT-style and permits modification; its only condition is that the copyright notice and
permission notice be retained in copies and substantial portions. They are: `vendor/musashi/readme.txt` is
untouched, and `packaging/licenses/Musashi-LICENSE.txt` reproduces the notice in every release package. The
modified files are marked as modified, which also satisfies the courtesy expectation that a changed copy not
be passed off as the original. Musashi's MIT-style terms are GPL-compatible, so the modified files sit under
this project's GPL-3.0-or-later distribution without conflict.

**These two changes would be worth offering upstream** (`kstenerud/Musashi`). Both are general: the
`m68kmake.c` expression is a latent portability trap for any MSVC user, and the unbounded table walk is a
real robustness bug independent of the miscompilation. Neither depends on anything specific to this project.
This is a note, not a commitment — nobody has contacted upstream.
