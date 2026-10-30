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
| **Musashi** 680x0 emulator, Karl Stenerud | 4.10 | `vendor/musashi/` | MIT-style | `packaging/licenses/Musashi-LICENSE.txt`, `vendor/musashi/readme.txt` |
| **SoftFloat**, John R. Hauser | Release 2b (MAME repackaging) | `vendor/musashi/softfloat/` | SoftFloat notice — permissive, requires the notice to be reproduced | `packaging/licenses/SoftFloat-NOTICE.txt`, `vendor/musashi/softfloat/README.txt` |
| **puff** inflate, Mark Adler | 2.3 | `vendor/puff/` | zlib licence | `packaging/licenses/puff-LICENSE.txt`, `vendor/puff/LICENSE` |

Musashi's opcode tables (`m68kops.c/h`) are generated from `vendor/musashi/m68k_in.c` by Musashi's own
`m68kmake` at build time. Those generated files are Musashi-derived and are not tracked.

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
| SoftFloat 2b | Hauser notice | **Probably — one open question** | See below. |

**SoftFloat's conditions are notice-retention** — the source must say prominently that the work is
derivative and must carry the notice's four paragraphs — and both are satisfied
(`packaging/licenses/SoftFloat-NOTICE.txt`). It is genuinely linked in: Musashi's `m68kcpu.c` includes
`m68kfpu.c`, which uses it, so SoftFloat code is present in the shipped binary even though this game is a
68000 with no FPU.

**The open question** is that one of those four paragraphs restricts use to parties who "CAN AND WILL TAKE
FULL RESPONSIBILITY" and who "EFFECTIVELY INDEMNIFY JOHN HAUSER AND THE INTERNATIONAL COMPUTER SCIENCE
INSTITUTE". Read strictly, an indemnification requirement and a restriction on who may use the code are
arguably "further restrictions" of the sort GPL §7 does not allow to be placed on downstream recipients. In
practice SoftFloat 2b travels inside many GPL projects — including MAME, which is where this copy's
packaging comes from — and has been treated as acceptable. **This is flagged, not resolved.** If it ever has
to go, the fix is small: drop `softfloat/softfloat.c` from the `musashi` CMake target and stub out Musashi's
FPU, which this game never touches. `docs/PROVENANCE.md` §7.3 has the detail.

**The GPL code this project is derived from** (WinUAE/Amiberry, `docs/PROVENANCE.md` §2) is not bundled here
— nothing from those projects is in this repository or in a release. It is nonetheless why the project is
GPL. The per-file licence statements that were actually checked, including the one file that is explicitly
"version 2 or (at your option) any later version" and the four that carry no licence sentence at all, are in
`docs/PROVENANCE.md` §7.1, with the residual uncertainty in §7.2.

**The Amiberry comparison harness** (`tools/compare/amiberry-harness/`) is a developer tool, not part of any
release. `x2ref_harness.inc` is this project's own code, but it is compiled into Amiberry, so the Amiberry
binary built from it is a GPL combined work; `x2ref-hooks.patch` carries a few unchanged context lines from
Amiberry's GPL sources. Now that this project is GPL-3.0-or-later there is no tension there.

## No game data

Xenon 2 Megablast itself — its code, graphics, music and text — is **not** in this repository and is not in
any release package. It is copyright The Bitmap Brothers / Mirrorsoft and is covered by no licence here.
`x2recomp` runs only from disk images you own (`x2recomp --import`). CDTV CD audio likewise comes from your
own disc image and is never copied into the repository or a package. `tools/check_no_game_data.py` enforces
this on every tracked file and runs in CI.
