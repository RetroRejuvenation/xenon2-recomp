# Third-party notices

Components that are vendored in this repository, linked into `x2recomp`, or bundled in a release package,
with their licences. Full licence texts are in `packaging/licenses/` and are copied into every release
package.

**xenon2-recomp itself is licensed GPL-3.0-or-later** — see `LICENSE` for the full text. Every component
below is compatible with that; §"Compatibility with GPL-3.0-or-later" sets out the reasoning and the one
point that remains unresolved. Nothing in this file changes the licence of the components it lists: each
keeps its own terms.

Where code in this repository was written from an external source rather than copied from it, that is
recorded separately in **`PROVENANCE.md`** (`docs/PROVENANCE.md` in the repository, next to this file in a
release package), file by file. Anyone reviewing the licensing of this project needs both documents: this one
lists what is *bundled*, that one lists what is *derived*. The project is GPL because of what is in
`PROVENANCE.md` §2, not because of anything in this file.

## Vendored in this repository

| Component | Version | Where | Licence | Licence text |
|---|---|---|---|---|
| **Musashi** 680x0 emulator, Karl Stenerud | 4.10, **with three local modifications** (see below) | `vendor/musashi/` | MIT-style — permits modification; the notice is retained | `packaging/licenses/Musashi-LICENSE.txt`, `vendor/musashi/readme.txt` |
| **SoftFloat**, John R. Hauser | Release 2b (MAME repackaging) | `vendor/musashi/softfloat/` — **headers only are compiled; none of it is linked or shipped**, see below | SoftFloat notice — permissive, requires the notice to be reproduced | `vendor/musashi/softfloat/README.txt` and the notice in each file |
| **puff** inflate, Mark Adler | 2.3 | `vendor/puff/` | zlib licence | `packaging/licenses/puff-LICENSE.txt`, `vendor/puff/LICENSE` |

Musashi's opcode tables (`m68kops.c/h`) are generated from `vendor/musashi/m68k_in.c` by Musashi's own
`m68kmake` at build time. Those generated files are Musashi-derived and are not tracked.

**Musashi is modified.** Three files carry local changes. Two were made on 2026-10-30 to fix a Windows/MSVC
failure:

- `vendor/musashi/m68kmake.c` (+11 −2): `populate_table()` computes each opcode's mask and match bit by bit
  instead of shifting the result of a comparison. MSVC 2022 at `/O2` dropped one term of the upstream
  expression, so every generated mask and match lost bit 14 (`0xf000` became `0xb000`) and the tables
  decoded the wrong instructions.
- `vendor/musashi/m68k_in.c` (+18 −87): `m68ki_build_opcode_table()` walks the handler table by each entry's
  own free bits instead of a hard-coded chain of unbounded mask-group loops, which ran off the end of the
  array when a group was missing.

The third is older and unrelated: `vendor/musashi/m68kcpu.h` (+9 −1) uses `_setjmp`/`_longjmp` for the
bus-error trap on macOS and the BSDs, where `setjmp()` would otherwise cost two signal-mask system calls on
every CPU slice. Performance only; bus errors are never raised in this game.

All three are marked in place with an `x2recomp:` comment giving the upstream form and the reason. The
generated `m68kops.c` is byte-identical before and after on macOS arm64 (md5
`b94fcb0a43c49d6237798bcb062dfac9`), so no other platform's behaviour changes. Musashi's MIT-style licence
permits modification and asks only that its notice be retained, which it is. `docs/PROVENANCE.md` §9 has the
full account; `docs/PORTING.md` has the MSVC detail. `vendor/puff` and `vendor/musashi/softfloat` are
unmodified.

## Linked or bundled at build time

| Component | Version | How | Licence | Licence text |
|---|---|---|---|---|
| **SDL 3**, Sam Lantinga and contributors | 3.4.18 pinned for `-DX2_FETCH_SDL3=ON`; otherwise whatever `find_package(SDL3)` finds | linked; the shared library is bundled in release packages (macOS `x2recomp.app/Contents/Frameworks`, Linux `lib/`, Windows next to the `.exe`) | zlib licence | `packaging/licenses/SDL3-LICENSE.txt` |

A build without SDL3 is headless-only and bundles nothing. The first-run import screen draws text with SDL's
built-in 8x8 debug font, which is part of SDL3; no font files are bundled.

## Recompiled builds (`-DX2_RECOMP=ON`)

`tools/recomp/recomp.py` copies Musashi's opcode-handler bodies verbatim into
`generated/recomp/x2r_handlers.h`, so a recompiled executable contains Musashi-derived code under the same
MIT-style licence. It also contains `x2r_*.cpp`, translated from **your** game image: that is derived from the
game and must not be distributed. `tools/package.sh` and `tools/package.ps1` refuse to package a
`-DX2_RECOMP=ON` build.

## Developer tools, not part of any release

| Component | Used by | Licence | Note |
|---|---|---|---|
| **Amiberry** (a WinUAE port) | `tools/compare/amiberry-harness/` | GPL-3 (project `LICENSE`); per-file terms vary, see `docs/PROVENANCE.md` §7.1 | Cloned from upstream by `build.sh`; nothing is vendored. `x2ref_harness.inc` is this project's code, but the Amiberry binary it is compiled into is a GPL combined work. See `docs/PROVENANCE.md` §6. |
| **Ghidra** | `tools/ghidra/*.java`, `tools/ghidra_x2.py` | Apache-2.0 | Not vendored; the scripts run inside an installed Ghidra. |
| **Capstone** (via the `capstone` Python package) | `tools/analyze68k.py`, `tools/recomp/recomp.py` cross-check | BSD-3-Clause | Not vendored; an optional developer dependency. |

## Compatibility with GPL-3.0-or-later

Nobody who wrote this is a lawyer. This records what the licence texts say and where doubt remains.

| Component | Licence | Compatible | Reasoning |
|---|---|---|---|
| Musashi | MIT-style | **Yes** | Permissive with notice retention; the standard GPL-compatible case. Notice shipped. |
| puff | zlib | **Yes** | Permissive, no restrictions beyond notice and marking modifications. |
| SDL 3 | zlib | **Yes** | Same. Linked, and bundled as a shared library. |
| SoftFloat 2b | Hauser notice | **Not applicable — removed from the build** | See below. |

**SoftFloat was removed from the build on 2026-10-30.** Musashi can use it for its FPU, and this project
used to compile and link it — 142 SoftFloat symbols were in `x2recomp` — even though the game is a 68000
with no FPU. One paragraph of its notice restricts use to parties who "CAN AND WILL TAKE FULL
RESPONSIBILITY" and who "EFFECTIVELY INDEMNIFY JOHN HAUSER AND THE INTERNATIONAL COMPUTER SCIENCE
INSTITUTE"; read strictly, an indemnification requirement and a restriction on who may use the code are
arguably "further restrictions" of the sort GPL §7 does not allow to be placed on downstream recipients.
Many GPL projects do carry SoftFloat 2b (including MAME, this copy's source) and treat it as fine, so it was
never a settled problem — but the code was doing nothing for us, so it was deleted rather than argued about.

`runtime/musashi_no_fpu.c` now stands in for `softfloat.c`, defining only the 15 symbols `m68kfpu.c` names.
They are unreachable: Musashi enters its FPU only under `CPU_TYPE_IS_030_PLUS` and this is a 68000, so an
F-line opcode takes the 1111 trap exactly as real hardware does. Verified with `nm`: all 88 symbols unique
to SoftFloat's implementation are absent from the binary, and the 15 that remain are defined in
`musashi_no_fpu.c.o`.

The `vendor/musashi/softfloat/` **headers** stay in the repository because `m68kcpu.h` includes them
unconditionally and removing them would mean editing a vendored upstream file. They contribute type
declarations only; the 10 `static inline` functions in `softfloat-macros` are referenced by nothing, so the
compiler emits none of them. Those files keep their own unmodified notices, which covers the parts this
*source tree* retains. `packaging/licenses/SoftFloat-NOTICE.txt` was deleted because a binary package
contains no SoftFloat code. `docs/PROVENANCE.md` §7.3 has the full detail.

**The GPL code this project is derived from** (WinUAE/Amiberry, `docs/PROVENANCE.md` §2) is not bundled here
— nothing from those projects is in this repository or in a release. It is nonetheless why the project is
GPL. The per-file licence statements that were actually checked, including the one file that is explicitly
"version 2 or (at your option) any later version" and the four that carry no licence sentence at all, are in
`docs/PROVENANCE.md` §7.1, with the residual uncertainty in §7.2.

**The Amiberry comparison harness** (`tools/compare/amiberry-harness/`) is a developer tool, not part of any
release. `x2ref_harness.inc` is this project's own code, but it is compiled into Amiberry, so the Amiberry
binary built from it is a GPL combined work; `x2ref-hooks.patch` carries a few unchanged context lines from
Amiberry's GPL sources. Now that this project is GPL-3.0-or-later there is no tension there.

**IPF reader (`tools/ipf.py`)** is a developer tool, not part of any release. Its record layouts, gap and data
description decoding and track timing rules are ported from MAME's `src/lib/formats/ipf_dsk.cpp`,
copyright Olivier Galibert, licensed BSD-3-Clause, which is compatible with GPL-3.0-or-later. The file carries
`SPDX-License-Identifier: BSD-3-Clause AND GPL-3.0-or-later` and this notice. It does not use or link the SPS
`capsimg` library, whose licence is not GPL-compatible.

> Copyright (c) Olivier Galibert and the MAME contributors. All rights reserved.
> Redistribution and use in source and binary forms, with or without modification, are permitted provided that
> the following conditions are met: (1) redistributions of source code must retain the above copyright notice,
> this list of conditions and the following disclaimer; (2) redistributions in binary form must reproduce the
> above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other
> materials provided with the distribution; (3) neither the name of the copyright holder nor the names of its
> contributors may be used to endorse or promote products derived from this software without specific prior
> written permission. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
> EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
> FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
> LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
> (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

**Copylock analysis (`tools/copylock.py`)** is an independent re-implementation, written from the public
description of the track layout and the LFSR in keirf's disk-utilities (`libdisk/format/amiga/copylock.c`,
public domain). No code was copied.

## No game data

Xenon 2 Megablast itself — its code, graphics, music and text — is **not** in this repository and is not in
any release package. It is copyright The Bitmap Brothers / Mirrorsoft and is covered by no licence here.
`x2recomp` runs only from disk images you own (`x2recomp --import`). CDTV CD audio likewise comes from your
own disc image and is never copied into the repository or a package. `tools/check_no_game_data.py` enforces
this on every tracked file and runs in CI.
