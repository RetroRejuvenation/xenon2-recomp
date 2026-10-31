# x2recomp 0.2.0 — release notes

A native port of the Amiga game **Xenon 2 Megablast** (The Bitmap Brothers, 1989), built by static
recompilation. The game's own 68000 code runs on a model of a PAL Amiga 500 — copper, blitter, Paula audio,
CIAs and a scanline display — either on the Musashi interpreter or as 68000 code translated to C++.

0.1.0 imported one release of the game: two cracked ADFs. **0.2.0 reads the preservation formats (IPF, flux dumps,
DMS, WHDLoad) and most of the known releases, including the original copy-protected retail disks.** The game itself
runs exactly as before: the canonical image and every existing replay golden are unchanged. Read "What is not
verified" before you rely on it.

## Downloads

| Platform | File | sha256 |
|---|---|---|
| macOS (Apple Silicon and Intel) | `x2recomp-0.2.0-macos-universal.zip` | `e33760da6334f6eda3b6733d6422fac800020c3f33e8fcd1a38561e49c2c8304` |
| macOS (Apple Silicon and Intel) | `x2recomp-0.2.0-macos-universal.dmg` | `38a1d590abfdbdde41bb6d4f56f1a995b4336c62cc95f5613ecd7d1d9d8e9e00` |
| Windows x64 | `x2recomp-0.2.0-windows-x64.zip` | `af483345b01a951379a094becaf8c753a449cbf120762eadaaa746ea23c45999` |
| Linux aarch64 (64-bit Raspberry Pi OS) | `x2recomp-0.2.0-linux-aarch64.tar.gz` | `d831cbc25c06ddc3157ce36d90993c7de27bd09a135dace4d5b0510d1ab9297b` |
| Linux x86_64 | `x2recomp-0.2.0-linux-x86_64.tar.gz` | `c5e0b61fa7a46d3a24c3525e4ad6b40cede82d1584fc703e804f99c14ede3e26` |

None of the binaries is code-signed (macOS has an ad-hoc signature only); see "Install and quick start".

## Legal position

- **This release contains no game data.** No disk images, no game code, no graphics, no music, no text, and
  no code translated from the game.
- **Bring your own disks.** You need your own copy of the game as Amiga disk images or dumps. `x2recomp --import`
  reads them on your machine and writes what the runtime needs to your per-user data directory. Nothing leaves
  your machine.
- **The importer is not a cracking tool.** For the copy-protected retail disk it reads the key track from *your* dump
  and the runtime answers the game's own check; no key value is in the program and the image is not modified. The
  separate opt-in patch is described below.
- **CDTV music is user-supplied.** The optional CD-audio layer plays tracks from your own CDTV disc image.
- **x2recomp is free software under the GNU GPL, version 3 or later** (GPL-3.0-or-later). The full text is in
  `LICENSE`, included in the package. It is GPL because part of the accurate-timing code is derived from
  WinUAE/Amiberry (`PROVENANCE.md`). Third-party components keep their own GPL-compatible licences
  (`THIRD-PARTY-NOTICES.md`, `licenses/`). New in this release: the IPF reader is a port of the BSD-3-Clause MAME
  reader (attribution in `THIRD-PARTY-NOTICES.md`); the SPS `capsimg` library is not used or shipped; the DMS and LHA
  decoders are our own code written from the public format descriptions.

Xenon 2 Megablast itself is copyright The Bitmap Brothers / Mirrorsoft and is covered by no licence here. The GPL gives
you the right to the complete source for what is in the package: it is the git repository this release was built from,
recorded in the release tag. A `-DX2_RECOMP=ON` build contains C++ translated from your own game image and is **not**
redistributable.

## What you can import

`x2recomp --list-variants` prints this table with the hashes; `x2recomp --diagnose-image FILE...` tells you what the
importer makes of a particular image.

| Release | Give the importer | Result |
|---|---|---|
| BS1 `[t+3]` two-disc set (the canonical image), its `[a]` and Deejay99 variants, the `[h Timecircle]` set, the "Xenon 2 & 4 Others" compilation | ADF, ADZ, DMS, IPF, flux, WHDLoad, zip | the canonical image: all replay goldens apply |
| **Retail two-disc release**, Image Works, both pressings (SPS 0297, SPS 2874 `[a]`) | disk 1 as an **IPF or flux dump** (SCP, KryoFlux stream folder, HFE) so the Copylock key track is kept; disk 2 in any format | imports; not canonical (original credit text; the first pressing has a code revision in the level 4 boss); own goldens |
| the same retail disk 1 from a **plain ADF, DMS or WHDLoad `Disk.1`** (no key track) | add `--apply-protection-patch --i-confirm-i-own-the-original` | refused with the options unless you choose the opt-in patch |
| Power Pack compilation (SPS 2234, one disk) | IPF, flux or the WHDLoad `Disk.1` | imports as its own build (`build=power-pack-2234`); disc 2 rebuilt from the disk's six game files (functionally equal; 61 unused sectors differ); slightly different audio (lower-precision samples) |
| Bombuzal + Xenon 2 compilation (SPS 1762, two disks) | both disks (IPF, flux, WHDLoad) | imports as its own build (`build=compilation-1762`, own disk 2); plays levels 1-5, the shop and the ending; its recorded runs differ from canonical early on and the cause is not investigated |
| cracks of the retail disk 1: PNA, Quartex, Flashtro +16 | ADF + the standard disc 2 | import; own variants (`canonical=false`), Flashtro +16 is a trainer build |
| PNA `[a]`, Black Monks, and the one-disk crunches Vision Factory, VF `[a]`, Black Monks `[h][t +36]` | ADF (+ the standard disc 2 where needed) | import; trainer builds flagged `trainer=true`; Black Monks' image tail is repaired from a duplicate block, the crunches are decrunched and disc 2 rebuilt from their data files |
| Predators +9, Flashtro +16 `[a]`, bad disk 2 copies | | **refused** with the reason (a trainer replaced the entry code; an unanalysed transformation; a copier's filler in the level data) |
| the playable demo disks, the L.G.C.S.S. compilation, WHDLoad demo and CDTV installs | | **recognised and reported**, not imported: different programs |

Formats read: ADF, ADZ, ZIP (nested), DMS, extended ADF, IPF, SCP, KryoFlux stream files (any DTC file prefix), HFE,
LHA archives, WHDLoad installs, and folders of these. CDTV discs (for the music layer) as before.

### The opt-in protection patch

The retail first disk is protected; the default way to import it keeps the protection check intact and answers it in
the runtime from a receipt that your own key track verified. If you only have a plain ADF, DMS or WHDLoad `Disk.1` of
that disk (no key track), you can ask for the alternative explicitly:

```
x2recomp --import disk1.adf disk2.adf --apply-protection-patch --i-confirm-i-own-the-original
```

This changes 18 bytes of the game image decoded from your retail disk 1 (the Copylock call and a check constant), in
your own copy on your own machine. It prints a disclaimer first, stops with exit code 4 unless you pass the
confirmation flag (the import window asks for a second keypress), applies only to a catalogued retail pressing
(checked by SHA-1), and is recorded in the manifest as `patched=true`. Nothing patched is distributed by this
project. Use it only with a copy you own, and comply with the law where you live. The key-track method is the default
and the preferred one.

## Install and quick start

Unpack the archive for your platform, then import your disks. The first-run window also accepts them dropped onto it.

**macOS** — the archive holds `x2recomp.app` with SDL3 bundled. The app is ad-hoc signed, not notarised, so Gatekeeper
will object on first launch: right-click the app and choose Open, or clear the quarantine flag with
`xattr -dr com.apple.quarantine x2recomp.app`.

```
./x2recomp.app/Contents/MacOS/x2recomp --import Disk1.ipf Disk2.ipf     # or your own dumps, ADFs, a zip, a folder
open x2recomp.app
```

**Windows** — unzip anywhere and run `x2recomp.exe` from the folder (SDL3 is beside it, and the C runtime is linked in,
so nothing has to be installed). The exe is unsigned, so SmartScreen may warn on first run: choose "More info", then
"Run anyway".

```
x2recomp.exe --import Disk1.ipf Disk2.ipf
x2recomp.exe
```

**Linux** — `bin/x2recomp` finds `lib/libSDL3.so.0` beside it through its `RUNPATH`; nothing has to be installed.
Take `linux-aarch64` for 64-bit ARM (a Raspberry Pi 3, 4, 5 or 400 on **64-bit** Raspberry Pi OS) and `linux-x86_64` for a
PC; `uname -m` says which. Minimum system: glibc 2.35 (Debian 12, Ubuntu 22.04 and newer). 32-bit Raspberry Pi OS
(`armv7l`) is not supported by either archive.

```
tar xzf x2recomp-0.2.0-linux-aarch64.tar.gz && cd x2recomp-0.2.0-linux-aarch64
./bin/x2recomp --import ~/Disk1.ipf ~/Disk2.ipf
./bin/x2recomp
```

`x2recomp --import` exits 0 imported, 1 could not write, 2 bad input, 3 unsupported image (including a protected retail
disk 1 without its key track), 4 confirmation required. Making your own dump: `docs/DUMPING.md`. Controls, assists and
every option are in `README.md` and `BUILDING.md`. Imports made by 0.1.0 are redone automatically from their recorded
sources the first time you start this version.

## What is verified, and how

- **128 tests** pass in the default interpreter build, run without game data on every CI platform (macOS arm64, macOS
  universal, Linux GCC and Clang, Windows MSVC are required checks; macOS x86_64 and Windows MinGW-w64 also run). The tests
  that need real images are reported as Skipped there. With the maintainer's images present they all pass locally.
  The translated-executor (`-DX2_RECOMP=ON`) suite is 150 tests; run with the maintainer's images, all pass except `game_import`, whose expectations for the cracks were still the 0.1.0 ones (fixed in this release, then re-run), and four folder-driven local tests (`flux_files`, `dms_files`, `lha_files`, `ipf_files`) that are skipped without a folder.
- **Replay goldens** (frame and audio hashes, the whole scripted corpus: title, five levels, shop, two-player, game over,
  ending) exist for the canonical image, both retail pressings, the retail-patched variant, the Power Pack, the 1762
  compilation and the cracks. The retail pressings reproduce the canonical frames except where the credit text and, in
  the first pressing, the level 4 boss code genuinely differ; the translated executor reproduces the retail goldens.
- **Readers, against real files.** IPF: every record and payload CRC verifies on all seven staged SPS images, and the
  disk 2 of both retail pressings decodes to the canonical disc 2. DMS: of 500 files from public Amiga archives, 497
  decode with every checksum verified (the other three are rejected at the same tracks by an independent decoder);
  output matched that decoder byte for byte per cylinder on 237 of them, across every mode. LHA: 238 members of the
  lhasa regression archives (about twenty archiver releases) and 252 members of WHDLoad archives decode with matching
  CRC-16. Flux: a 168-file five-revolution KryoFlux set and a three-revolution SCP of other Amiga disks, both from
  public archives, decode byte-identically to their reference ADFs.
- **Flux decoder against noise.** Synthetic flux with jitter: every sector recovers up to about 10% of a bit cell on one
  revolution, and spindle speed errors of ±4% are fine; noisy tracks are reported unreadable, never decoded wrongly.
- **The importer is fuzzed** (random and mutated input for every reader and decruncher) and the importer tests run clean
  under ASan and UBSan. An independent review of the new readers found two denial-of-service paths (ZIP expansion, SCP
  revolution aliasing) and a shift bug in the LHA decoder; all were fixed with regression tests.
- The WHDLoad `Disk.1` of the retail pressings decodes to the same game image as the IPF of the same pressing; the
  patch route from it gives the same image as the IPF route.
- Everything from 0.1.0 (compared against Amiberry, played by hand on macOS and Windows) is unchanged; see
  `README.md`, "Status and known gaps".

## What is not verified

- **No dump of the maintainer's own retail disks has been imported yet.** The retail import is verified against SPS
  preservation images (IPF) of both pressings, and the flux readers against other disks. A real KryoFlux or DrawBridge
  dump of the retail first disk has not been run through the importer; `docs/DUMPING.md` describes what a good one
  looks like. DrawBridge's SCP output for the long Copylock track is the least certain path (its own notes call accurate
  flux extraction experimental).
- **Flux decoding was validated on synthetic flux and two public dumps**; HFE v3 support is best effort and untested.
- **The crack variants are other people's builds.** PNA `[a]`, Black Monks and the Vision Factory family are trainer or
  re-packed builds: they run and have goldens, but their gameplay is not the original's (infinite ships, trainer hooks),
  and VF `[a]` differs from VF by one frame near frame 500. Predators +9 and Flashtro +16 `[a]` cannot be imported.
- **The 1762 build** boots and plays, but its recorded runs differ from the canonical ones early on; the cause (probably
  timing) is not investigated. The **Power Pack's** audio differs slightly from the other releases (lower-precision
  samples); its disc 2 is rebuilt, not byte-identical.
- **Budget and PDos releases** (floppy versions 4 and 5 of the WHDLoad imager) are known only from the imager's source;
  no image of either was available to test.
- **Windows and Linux have not been played by hand on this release;** the Windows executable was not run on a clean
  machine without Visual Studio; no Raspberry Pi has run it (speed unmeasured). Gamepads and drag-and-drop import are
  untested. The macOS and Windows builds of 0.1.0 were played by hand; nothing in 0.2.0 touches the game's runtime
  paths for the canonical image.
- **No comparison against real Amiga hardware.** Every accuracy claim is relative to an emulator.
- The CDTV game executable (a different program) is recognised and reported, not run; it is planned as a later target.
- No unassisted playthrough has been done; disc loads are instant (the game's random generator drifts after each load);
  widescreen is cosmetic. All as in 0.1.0 (`README.md`, "Status and known gaps").

## Checksums

The sha256 of each download is in the table above. Each archive also contains a `SHA256SUMS.txt` listing every file
inside it. Verify both:

```
shasum -a 256 x2recomp-0.2.0-macos-universal.zip            # macOS: compare with the table
cd x2recomp-0.2.0-macos-universal && shasum -a 256 -c SHA256SUMS.txt

sha256sum x2recomp-0.2.0-linux-aarch64.tar.gz               # Linux: compare with the table
cd x2recomp-0.2.0-linux-aarch64 && sha256sum -c SHA256SUMS.txt
```

The per-file manifest is generated at packaging time from the staged contents, so it also confirms that no game data,
imported assets or translated code is present — `tools/package.sh` and `tools/package.ps1` refuse to package a
`-DX2_RECOMP=ON` build and check the staged files before archiving.
