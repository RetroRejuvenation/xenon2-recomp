# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

This project contains no game data. See `README.md` for the legal position.

## [Unreleased]

### Added

- **The Atari ST (Kixx) release runs on the recompiled executor** (`docs/re-notes/atari-st-recomp.md`). `tools/recomp/recomp.py --build st` translates the engine, the front
  end and the five level modules (the front end and the levels are unpacked from the disk's own RNC streams) into a third module set, `x2r_modules_st`
  (`generated/recomp-st`, `x2rs_` files, `-DX2_RECOMP_ST_DIR`), seeded from traces of ten scripts (`tools/trace/run_corpus.py --game st`: `tests/replays/st` and the new
  `tests/recomp/st_scenarios`) and the program's entry points. On every script the translated build gives the interpreter's framebuffer hash, audio hash and a new
  per-frame **state digest** (all RAM, registers, cycle counter; `--st-state-hash`), frame log and final RAM (84,000 frames, 90.6-97.5% of instructions translated; the
  rest is the boot loader, which runs once, and one self-modifying instruction of the front end's Timer A handler, which the executor interprets because its bytes change).
  A differential test (`x2_recomp_difftest --game st`) ran 6,090 cases over all 406 translated functions with no difference; a translation traced on half the scripts runs the
  other half identically, and a deliberately partial one (a single trace) runs at 40-52% translated and is still exact. About 25% faster than the interpreter. The tracer was
  generalised to any machine's RAM (disk and CDTV traces are byte-identical), `StMachine` gained `set_cpu_execute`, and `--game st` takes `--interp-only`,
  `--recomp-stats`, `--recomp-profile` and `--trace-out`. New tests `recomp_st_<script>`, `recomp_st_difftest` (need the Kixx import and `-DX2_RECOMP=ON`) and `recomp_st_tools`
  (synthetic, in the default build); `st_machine` checks the executor seam, the state digest and the tracer's ST phases. The hygiene check, `package.sh` and `package.ps1`
  refuse `x2rs_*` files; `tools/build_all.sh` and `.ps1` translate an imported `assets-st`.
- **Asset pack authoring tools** (`tools/assetpack/`, standard-library Python only; workstream WS5a of `docs/re-notes/asset-packs-v2-design.md`). `pack_atlas.py` packs per-key pictures into sprite sheets (shelf or grid layout, padding and edge bleed, power-of-two sizes, splitting, merging the atlas entry into a manifest), `slice.py` cuts sheets back into per-key pictures, `contact_sheet.py` renders labelled contact sheets of a dump or pack, `merge_dumps.py` merges several `--dump-assets` folders, `dump_all.sh` runs every replay of a build into one merged dump, and `make_skeleton.py` writes (and checks) a tiny synthetic example pack with every v2 section, committed as `examples/asset-pack-skeleton/` (the hygiene check allows that one folder, 64 KB per file). New test `assetpack_tools`.
- **Atari ST disk images are read, and the Kixx budget release is imported** (`docs/re-notes/atari-st-importer.md`). `.st`, `.msa` and Pasti `.stx`
  (also from a ZIP or folder) are decoded to one logical image and identified by its SHA-1, so the three containers of one disk agree. The
  unprotected Kixx release imports as `disk.bin` plus a `build=atari-st-kixx` manifest in `assets-st` (checked by `verify_assets`, rebuilt by
  `ensure_imported`); the Image Works and Mirror Image releases (Copylock ST) and the ST Format preview disk are recognised and refused with the
  reason. `--diagnose-image` reports geometry, the boot sector, copy-protection findings (key sector, weak-bit mask, slow sector), a FAT12
  listing and the RNC ProPack blocks; `--list-variants` and the import window know the ST disks. New tests `st` (synthetic) and `st_files` (your dumps).
- **The Atari ST machine, first step (`x2recomp --game st`, in development).** `runtime/st` models an ST around
  the Musashi core: the bus, the WD1772 floppy controller with its DMA chip, the MFP (timers and interrupt controller), the keyboard
  ACIA and IKBD, the Shifter and a YM2149 register file, scheduled by CPU cycle (313 lines of 512 cycles, VBL on level 4, MFP on
  level 6). The Kixx dump's real boot sector runs on the emulated controller, loads the front-end and the resident engine (710 sector
  reads) and the front-end runs to the title screen with no fault (`docs/re-notes/atari-st-machine.md`). The runtime core gained one
  seam for it (`runtime/bus_hooks.h`; the Amiga and CDTV paths pay a null-pointer test and are bit-identical: every replay golden is
  unchanged). `--game st` starts the imported disk (`assets-st`; `--st-disk FILE.st` a plain image). Tests: `st_machine` (synthetic, 112 checks before the picture work, 131 now) and `st_kixx`
  (skipped without the dump; it also checks the importer's output boots the same picture). No sound or game play yet.
- **The Atari ST build has sound and takes input** (`docs/re-notes/atari-st-sound.md`). A YM2149 generator (tone, noise, envelope, the measured
  32-step output table, sample-accurate volume writes for the front-end's digitised speech) feeds the speakers or `--audio-out` / `--audio-hash`;
  against Hatari's own capture the title music, the digi-sample player and level 1 match within a few percent in level and 0.96-1.0 in envelope
  and timbre (`tools/compare/st_audio_compare.py`). The IKBD follows Hatari (reset `$F1`, pause and resume keep what was typed, joystick packets);
  the joystick bindings drive port 1, keys reach the game as scancodes, `--input-script` takes the project's script format and Hatari's, and
  `--st-watch` prints RAM words as they change. With Hatari's fire pulses the game starts level 1 with full energy and 3 ships. New tests
  `st_sound` (synthetic) and, in `st_kixx`, golden audio hashes and the input chain.
- **The Atari ST picture follows Hatari line by line** (`docs/re-notes/atari-st-video.md`). The display is decoded one line at a time with every
  palette write applied from the pixel the beam had reached, the video counter is reloaded where the ST's GLUE reloads it and counts the way
  Hatari's does, every instruction ends on the 4-cycle bus boundary and the YM2149, MFP and keyboard ACIA stall the CPU as on the machine.
  Against Hatari's own frames the front end matches pixel for pixel and level 1 within 0.1% of its pixels, and the engine's 4-VBL steps are
  2.8% of all steps against Hatari's 2.9%. `tools/compare/st_compare.py` compares the two frame sets; `--st-frame-log`, `--st-shots-dir`,
  `--st-input-offset`, `--st-profile` and `--st-ram-dump-at` produce what it reads. The `st_kixx` golden pictures and audio hashes were re-recorded.
- **The Atari ST build keeps high scores and has the assists** (`docs/re-notes/atari-st-hooks.md`). The high-score table (`$8FAA`, the Amiga
  layout) is saved at the name entry and put back at the next start, in `highscores-st.bin` keyed to the disk image; `--god-mode`, `--credits N`,
  `--autofire`, `--cheat-scores`, `--persist-dir` and the window's Backspace/Tab/Insert commands work as in the other builds (all off by default:
  the goldens are unchanged). Done with PC hooks, not patches. The shared input script's `poke` and `pokeobj` lines work for the ST (coverage
  only). Fixed: the floppy DMA dropped a sector that ran past the top of RAM, which stalled the load of level 2 (levels 2-5 now load).
  New tests `st_game` (synthetic) and `st_hooks_*` (skipped without the dump); `tools/trace/st_probe.py`; `--st-watch-pc`.
- Research: Atari ST loader, packer and engine comparison (`docs/re-notes/atari-st-analysis.md`) with analysis tools in
  `tools/acq/` (`stemu`, `sttrace.py`, `stshape.py`, `ymrender.py`) and synthetic tests. No runtime change.
- **The Rob Northen PDos release (WHDLoad imager version v5) is a known release** (`docs/re-notes/pdos-v5.md`). The importer reads its
  twelve-sector PDos tracks, unpacks the game and its eleven files with a new RNC method 1 decoder (both CRC-16s must match) and
  imports it as `build=pdos-v5`, from a flux dump, an extended ADF or the 982,528-byte WHDLoad `Disk.1`; the runtime starts that build
  with its own loader address, a one-sector bias and twelve sectors a track. **It imports only from a complete dump**: a dump with
  unreadable sectors inside the game or its files is refused with the files and `track:sector` pairs named. The one public dump has seven
  unreadable sectors (inside the first three level files), so this release has not been imported from real data end to end; the boot to
  the title, attract and menus was checked on development assets (`tests/replays/pdos-v5/attract.hashes`, the only replay that loads none of the
  damaged files).
- **FluxEngine `.flux` files** are read (FluxEngine's protobuf format, `runtime/import/fluxengine.cpp`; bounded, unknown fields skipped).
- **Two captures of one disk, given together, are merged** (SCP and `.flux`, in any mix): a sector valid in any revolution of either capture is kept and
  nothing is overridden.
- **A retry pass in the flux decoder:** sectors still unreadable after the first pass are decoded again with a PLL limited to valid MFM
  run lengths at eleven loop gains, and raw tracks keep alternate bit streams for custom-format decoders. On synthetic flux the
  practical limit moves from about 12 % to about 14 % jitter (one revolution at 12 %: 200 of 264 sectors, now 242; three revolutions at 14 %: 93, now 161); the real
  Frontier Elite II and SimCity dumps still decode byte-identical to their reference ADFs (`docs/re-notes/flux.md`).

## [0.3.0] - unreleased

0.3.0 adds the **CDTV release** of Xenon 2 Megablast as a second game: it imports from your own disc image, starts and plays
on the interpreter and on the recompiled executor, plays its CD audio and speech, and takes the modernisation options. The
disk releases are unchanged: the canonical image and every existing replay golden are identical.

### Added

- **The CDTV release (`x2recomp --game cdtv`).**
  - *Import.* `x2recomp --import <CDTV disc image>` (a `.cue`, a ZIP or folder holding it, or the bare data track as
    `.bin`/`.iso`) reads the disc's ISO 9660 volume (`runtime/import/iso9660`, bounds-checked, loop-safe), parses `X2` as an AmigaDOS
    hunk file (`runtime/import/hunk`) and links it at fixed addresses (hunk 0 at `$400`, hunk 1 at `$6B09C`). It stores the linked
    image (`game_0400.bin`, 809,564 bytes) and the 95 files the game loads from the disc (`cd0/`) in `<data dir>/assets-cdtv`, with a
    manifest (`build=cdtv`, `chip_kb=1024`, hashes, relocation facts, file table) that `verify_assets` and the automatic re-import
    check. The same command imports the CD audio when the input has the audio tracks (`--import-cdtv` still imports the audio
    alone). `--diagnose-image`, `--list-variants` and the import window know the disc (`docs/re-notes/cdtv-importer.md`).
  - *Runtime.* The runtime answers what the program still asks of the operating system with PC traps, the idiom of the disk shim
    and the retail check (`runtime/cdtv/cdtv_game.*`, `docs/re-notes/cdtv-boot.md`): its OS start-up block and ExecBase PAL check
    are skipped, the ten dos Open/Read/Close loaders read the imported `cd0/` files, and the five CD-audio routines drive the CD
    player. Boot sets what the OS left behind (the program's own interrupt-clearing routine as the level-3 vector, the
    vertical-blank enable, interrupt mask 0). The boot table (`runtime/boot.*`) carries the build's variable map, which the
    assists use: god mode, `--credits` and the cheat flag work. The image is never modified.
  - *CD audio and speech.* The program's own once-a-VBL CD poll runs as the game's (its counter lives in game memory and comes round
    every 51 ticks), and the CD player answers the `CheckIO`: a finished track starts again at the first poll after its end, up to a
    second late, in the original's phase. The cue log (`--cdtv-log`) names the game's calling address. The CD is the music by
    default in a window (a headless run stays silent unless asked; without imported CD tracks the game runs and says once that the
    music is silent). The music key mutes and unmutes the CD. Measured: speech plays whole and mono at period 298 (11,902 Hz),
    correlating 0.99 with its file, with the pan and volume arithmetic exact (`docs/re-notes/cdtv-audio.md`: the observed
    state-to-track table, the MUSIC ON/OFF finding, pause and mix-level findings).
  - *Second button.* The CDTV joypad's button B (POTGOR bit 14, the special weapon): the `button2` binding (default `C`, a gamepad
    shoulder button; `B` in `joy` script lines). **`input.cfg` is now format version 5**; version 4 files are upgraded as before
    (kept as `input.cfg.bak`). `--list-keys` explains `button2` and that the CDTV remote, which never reaches `X2`, is unused.
  - *Choosing the release.* `--game disk|cdtv|auto` and `game = auto|disk|cdtv` in `settings.cfg` (with both imported, `auto`
    starts the disk release and says so); the window title names the release (`Xenon 2 Megablast (CDTV)`); the import window
    starts the CDTV release after a disc is dropped.
  - *Scores.* The CDTV release saves its high-score table by default (the original keeps none) in its own file,
    `highscores-cdtv.bin`: a table file carries the hash of its game image, so a shared file made each release overwrite the other's.
  - *Replay corpus.* 23 replays (`tests/replays/cdtv/`, `docs/re-notes/cdtv-replays.md`): the front end and guide pages, the SELECT
    menu and a 2-player game, all five levels, fast-forwards and floods, the mid-level and level-5 shops (the CDTV shop is paced by
    shopkeeper speech), the level-1 boss defeated by the game's own death code and the path into level 2, the level-2, -3 and -4
    bosses reached but not defeated, game over with the high-score entry, and the level-5 ending reached both through a shop
    request and naturally. Frame and audio hashes are recorded for all of them and match a fresh run. CD audio goldens
    (`NAME.cd.hashes`, `NAME.cues`) for four of them. New tools: `tools/trace/make_cdtv_replays.py`, `cdtv_probe.py`,
    `tools/cdtv_audio_check.py`.
- **The CDTV release on the recompiled executor (`docs/re-notes/cdtv-recomp.md`).** `-DX2_RECOMP=ON` translates the CDTV game too,
  from its own traces, bit-identical to the interpreter. `tools/recomp/recomp.py --build cdtv` describes the pre-linked `X2` (the
  overlay area as a hole) and the seven overlays (`LEVEL1-5`, `INTRO`, `PICCODE`, relocated statically the way `X2`'s own
  relocator does: checked byte for byte against the running game), seeds from the entry points, the engine API table, the
  overlay stubs and the traces of the 23 replays, and writes `generated/recomp-cdtv` next to the disk translation. A recompiled
  build carries the disk translation, the CDTV one or both (`X2_RECOMP_DIR`, `X2_RECOMP_CDTV_DIR`), and a game without a linked
  translation runs on the interpreter. Results: frame and audio hashes of every frame, register logs and final chip RAM identical
  to the interpreter on all 23 replays (100.00% of 1.23 billion executed instructions translated; 99.84% on five replays held out
  of the traces; exact with a deliberately partial translation), the CDTV differential test (563 functions from random states, no
  difference), and about 2.5% more speed. The disk translation regenerates byte for byte. New tool: `tools/recomp/coverage_report.py`.
- **Modernisation on the CDTV release (`docs/re-notes/cdtv-modern.md`).** Interpolated frames, the widescreen surround, the HD
  status panel, filters, replacement asset packs and the Faithful switch work on `--game cdtv`, on the interpreter and on the
  translated executor. The capture's disk-build addresses became a per-build `CaptureLayout` (`runtime/modern/layout.h`); the CDTV
  layout was mapped from the disk build by instruction shape (`tools/modern/map_cdtv.py`) and confirmed by running the game.
  Interpolation endpoints are exact on all 21 gameplay replays, the centre of every surround picture is the rendered frame, and
  108 of the 109 level-1 graphics have the same asset-pack key in both releases (a pack applies to both).
- **Selectable chip RAM size.** `Machine` takes its chip RAM size at construction (512 KB by default, 1 MB with `--chip-ram 1024` or a
  build's `chip_kb`); the mask comes from it, so every CPU access, copper list, blitter, bitplane, sprite and audio DMA pointer
  follows it. The 1 MB model has real RAM at `$80000` and 4-bit pointer high words; the 512 KB model keeps its mirror and 3 bits. The
  disk builds are unchanged and reproduce their frame and audio hashes in the 1 MB model too (hot path +1.4%).
- **Reference evidence for the CDTV release** (`docs/re-notes/cdtv-reference.md`, `cdtv-reference-whd.md`; harness
  `tools/compare/amiberry-harness/run-cdtv.sh`, `run-cdtv-whd.sh`). The CDTV disc boots to gameplay in Amiberry (ROM v1.0, v2.30 or
  v2.35, user-supplied, with extra fast RAM and an ECS Agnus). Against the WHDLoad CDTV slave run on an A500+ the front end and
  level 1 match at a median 99.89% of pixels, the HUD is identical and the game variables are equal at matched moments.
- **Analysis notes**: the CDTV `X2` program and overlays (`cdtv-code.md`, `cdtv-c0.md`), the WHDLoad CDTV slave (`cdtv-slave.md`),
  the runtime gap (`cdtv-runtime-gap.md`), the 6.9 MB region after the ISO volume (`cdtv-data-region.md`: another CDTV title's
  narration, prompts and effects; unreferenced, ignored), other versions and possible development sources
  (`other-versions-and-sources.md`).
- **Acquisition tools (`tools/acq/`).** Readers written to analyse the first real Amiga flux dump of Xenon 2 and other ports:
  SuperCard Pro and FluxEngine (`.flux`) flux readers with a phase-tracking PLL, the Rob Northen PDos track format and its RNC
  streams, Pasti (`.stx`) Atari ST images, Acorn ArcFS archives and a small ProTracker replayer, with 18 synthetic tests.
  `docs/re-notes/acquisitions-flux-acorn-st.md` records the results, notably that the Internet Archive "Powerplay" flux is the
  previously unseen **PDos release** (WHDLoad imager v5). The importer recognises a WHDLoad install of it by its boot block but
  does not import it, and does not read these tools' formats.

### Changed

- The recompiler and the executor are per game build (the tracer takes a layout, the executor takes a target, module sets are named per game).
- The modernisation capture reports the last frame whose captured picture differs from the rendered one, and replays the star
  routine's writes as the game did them for the CDTV layout (the disk layout keeps its rule; its goldens are unchanged).
- `pokeobj` follows the build's object-list head (`BuildInfo::var_objlist`: `$B4C` for the disk builds, `$B52` for the CDTV release).
- `--cd-pause` and `--cdtv-tracks` say they do not apply to the CDTV release; `--protect-music` is a clear no-op there.

### Fixed

- The Windows release zip stored backslash path separators (`Compress-Archive` in Windows PowerShell 5.1), so `unzip` on macOS and Linux warned and flattened the folders; `tools/package.ps1` now writes forward-slash entry names. Its `SHA256SUMS.txt` is written with LF line endings so `sha256sum -c` works on every platform (the 0.1.0 and 0.2.0 Windows zips have the old layout).

- `tools/recomp/recomp.py` failed to build its Musashi table-dump helper on Linux (the linker could not find `sin`/`cos`; macOS links
  libm implicitly). Found by the new synthetic generator test in CI.
- CD audio hashes differed between Debug and Release builds because of fused multiply-add; the CD resampler no longer contracts
  floating-point operations (`FP_CONTRACT OFF`). The goldens were recorded on arm64 macOS.
- The Amiberry CDTV harness config showed the level-1 playfield as vertical streaks: an OCS Agnus wraps DMA pointers at 512 KB
  while the game's buffers sit above `$80000`; `chipset=ecs_agnus` fixes it.
- The hygiene check refuses committed `x2rc_` (generated CDTV translation) files.

### Security

- The new parsers (ISO 9660, AmigaDOS hunk files) are bounds-checked and loop-safe, draw on the shared import budget, and are covered by
  synthetic damage tests and a mutation fuzz; the importer tests run clean under ASan and UBSan.

## [0.2.0] - 2026-10-31

The importer grew from "two cracked ADFs" into a reader for the Amiga disk preservation formats and for most of the
known releases of the game, including the original copy-protected retail disks. The runtime changed only where a
release needed it. The canonical image and every existing replay golden are unchanged.

### Added

- **Disk image formats.** The importer decodes containers into one disk model (`runtime/import/disk_image`, up to 168
  tracks, each decoded sectors or raw MFM) and recognises them by content: ADF, ADZ, ZIP (nested), **DMS** (our own
  decoder: every mode, every per-track checksum; encrypted and high-density archives are refused), **extended ADF**
  (`UAE-1ADF`, `UAE--ADF`, raw tracks included), **IPF** (our own C++ port of the BSD-3-Clause MAME reader; the SPS
  `capsimg` library is not used), flux dumps (**SCP**, **KryoFlux stream** folders, **HFE**; a phase-locked loop and
  a sector vote over the revolutions, failures named by track and sector), **LHA** archives (our own decoder, levels 0-3,
  `-lh0-` to `-lh7-`) and **WHDLoad** installs (slave detection, floppy version by boot-block CRC). Notes:
  `docs/re-notes/dms.md`, `flux.md`, `whdload.md`. `docs/DUMPING.md` explains how to make your own dump with a
  KryoFlux or a DrawBridge and how to check it.
- **The retail two-disc release imports** (SPS 0297 and SPS 2874 `[a]`), from an IPF, a flux dump or an extended ADF that
  keeps disk 1's Rob Northen Copylock key track. The importer verifies the key track, decodes the game image under its
  word-XOR chain unmodified, identifies the pressing by the image's SHA-1, takes disc 2 from the second disk, and writes
  `copylock.txt` (receipt, version 2, bound to the image) beside `game_0400.bin` and `disc2.bin`. The runtime recognises
  the game's own Copylock routine at `$70F6` and, only with a matching receipt, continues at its exit `$763A`; the image
  bytes are never modified, nothing in the program is a key, and without a receipt the image is refused
  (`docs/re-notes/retail-protection.md`, `retail-disk1.md`). Retail imports are not the canonical image and have their
  own replay manifests (`tests/replays/retail/`).
- **Opt-in protection patch.** `--apply-protection-patch --i-confirm-i-own-the-original` applies the 18-byte protection
  removal to a catalogued retail pressing from any source that decodes disk 1 (a plain ADF, DMS or a WHDLoad `Disk.1`
  has no key track). It prints a disclaimer, exits with code 4 without the confirmation (the import window asks for a
  second keypress), checks the decoded image by SHA-1 and records `patched=true`. The key-track method stays the
  default; the patched variant has its own goldens (`tests/replays/retail-patched/`).
- **A variant registry** (`runtime/import/variants`) behind `x2recomp --list-variants` and `--diagnose-image FILE...`
  (a per-track report without game data), manifest version 2 (`variant`, `pressing`, `container`, `canonical`,
  `patched`, `copylock`, `build`, `trainer`, `disc2_rebuilt`) and exit code 4.
- **More releases and builds.**
  - Canonical-equivalent: the BS1 `[h Timecircle]` set and the "Xenon 2 & 4 Others" compilation (its disk 2 differs in two
    unused sectors; the `[a]` copy of that disk 2 is damaged and refused).
  - Own variants, `canonical=false`, own replay goldens (`tests/replays/cracks`): PNA, Quartex, Flashtro +16, PNA `[a]`,
    Black Monks, and the one-disk crunches Vision Factory, VF `[a]` and Black Monks `[h][t +36]` (native decrunchers,
    each checked against its expected SHA-1 and fuzzed; disc 2 rebuilt from the six data files). Trainer builds are flagged
    `trainer=true` with a warning.
  - **Different builds of the game** that the runtime starts from a table (`runtime/boot.h`, `build=` in the manifest):
    the Power Pack (SPS 2234, from an IPF, flux or the WHDLoad `Disk.1`) and the Bombuzal + Xenon 2 compilation (SPS 1762, which
    needs its own disk 2). Goldens: `tests/replays/power-pack-2234`, `tests/replays/compilation-1762`.
  - Recognised and refused with the reason: Predators +9, Flashtro +16 `[a]`, the playable demo disks, the L.G.C.S.S.
    compilation (the demo, not the game), WHDLoad CDTV and demo installs, bad disk 2 copies.
  - Analysis: `docs/re-notes/sources.md`, `cracks.md`, `single-disk-variants.md`, `build-variants.md`.
- **Release workflows.** Manual `Release (Windows x64)` and `Release (macOS universal)` workflows build the archives from
  a `git archive` of a ref on hosted runners (for when no local Windows machine or Mac is at hand), next to the existing
  `Release (Linux x86_64)` one, which now starts after CI succeeds on a push to `main` as well.
- **Tests.** 128 in the default build (67 in 0.1.0): `disk_image`, `dms`, `ipf`, `flux`, `lha`, `retail_import`,
  `retail_protection`, `build_table`, `builds_import`, `variants_import` and the per-variant replay tests; those that need
  real images are skipped without them. The importer test binaries also run clean under ASan and UBSan.

### Changed

- The importer version is now 3 (manifest version 2, receipt version 2): imports made by 0.1.0 are redone automatically
  from their recorded sources. A hand-made assets folder (`tools/retail_assets.py`) must be written again.
- A retail disk 1 given without its key track is refused with the options instead of a wrong "single-disk crack"
  message (PNA, Quartex and Black Monks are the retail disk 1 with the Copylock removed, not one-disk releases).
- The Xenon 2 executable test bounds the unpacked size, so another program packed the same way (the L.G.C.S.S.
  compilation's demo) is reported as another program, not as a corrupt disc.
- Archive members may be 96 MB and folders 1024 files, because flux dumps are large. The `--import` help and the
  no-game-data message list every accepted format.
- CI runs on every pull request (the six main jobs are required checks on `main`), and its arm64 macOS jobs use `macos-15`.

### Fixed

- **KryoFlux stream files made with any DTC file prefix are recognised.** DTC names the files after whatever `-f` gave
  (`dumpfile00.0.raw` for `-fdir/dumpfile`); the importer insisted on `track`, so a dump made with the manual's own example
  command line was not read. The track number now comes from the `NN.S.raw` suffix.
- The `game_import` data test now expects what the importer does with the cracks (the retail-disk cracks need their disc 2;
  the one-disk crunches import alone) instead of the 0.1.0 "single-disk crack" refusals.
- Windows: the importer tests no longer remove a directory while a file in it is open (the throwing `remove_all` ended in
  `std::terminate` on MSVC), and the audio smoke test's throughput floor no longer fails on a starved runner.

### Security

- **Importer hardening.** The importer reads untrusted files that users drop onto a window, so every new reader is bounds-checked
  and size-limited. ZIP archives are read in two passes (the member count is capped at 4096 and the declared sizes are
  summed against the import's unpack budget before anything is inflated); gzip, ZIP, LHA and every decoded disk image draw on
  one 512 MB budget per import, across nested archives. An SCP file may hold at most 32 revolutions per track and its flux
  data ranges must be disjoint; KryoFlux flux held across track files is capped. LHA `-lh4-` to `-lh7-` offset tables are
  capped per method and the bit reader no longer shifts out of range. One-past-the-end `&v[v.size()]` accesses became
  `data()` arithmetic. Output folders named on the command line are created with the non-throwing overloads.
- **The key-track receipt names its image** (`image-sha1`); the runtime refuses a receipt written for another image or an old
  version 1 receipt, and `verify_assets` refuses a `copylock.txt` beside a manifest that records none.

## [0.1.0] - 2026-10-30

### Added

- **`LICENSE`: the project is now GPL-3.0-or-later.** The accurate-timing code contains transliterations of
  GPL code from WinUAE/Amiberry, so a permissive licence was not available. Every original source file
  carries an `SPDX-License-Identifier: GPL-3.0-or-later` header.
- `docs/PROVENANCE.md`: a file-by-file record of which code was written from external sources, how closely it
  follows them, and the licence reasoning behind the GPL choice (§7), including the per-file licence
  statements found in the reference sources and the residual uncertainties.
- `THIRD-PARTY-NOTICES.md`: the bundled, vendored and linked third-party components and their licences.
  Installed and packaged alongside `README.md`.
- `docs/re-notes/music-dropouts.md`: why the music seems to stop sometimes. The answer is that it is
  authentic: the one tune has long rests in which only AUD1 and AUD2 play, and those are exactly the two
  channels the sound effects take, so holding fire through the 14.8 s rest in level 1 leaves nothing audible
  but shooting. Every rest and every music/effect channel handover appears at the same frame in the Amiberry
  reference captures (left-side envelope correlation 0.996 on `two_player`), so no emulation change was
  needed. The note also records every way the game can silence the music, corrects `$1A584` from "tempo" to
  "master volume" and `$19FE4` from "probably a fade" to "end the running effects and hand the channels back",
  and lists the live-output paths that can still cause a real dropout. Its section 7 answers the reporter's
  follow-up — that the music is silenced for as long as they keep firing — with the channel-allocation rule:
  the effect command's high byte is the voice, so effects can only ever take AUD1 or AUD2 (measured: 0 of
  12,000 frames for AUD0 and AUD3); a new command reloads the effect's duration counter at `$1B4C6` without
  handing the voice back, so chained effects hold it with no gap at all — 99.8 % of a firing block in one
  unbroken 20.47 s run when fire is tapped at 4-frame intervals, against 84 % in 288 brief holds at the game's
  own 24-frame rate. Two more Amiberry captures of the same 20 s-firing / 10 s-quiet cycle match ours to
  within a percent on both sides (envelope correlation 0.998 left, 0.994 right), so that is authentic too.
- `tools/audio/music_dropouts.py`: lists the music-silent intervals of any replay, with the game state and
  driver calls around each one, and with `--own` which voices the sound effects held and for how long without
  a break. CTest `music_gaps` pins both reference-confirmed shapes: the tune's rests, and the rule that effects
  never take AUD0 or AUD3 while AUD2 is held for most of a firing block.
- **`--protect-music`** (`protect_music` in `settings.cfg`), **off by default and a deliberate deviation from
  the original**: the tune keeps its voices through the sound effects that take their Paula channels.
  The obvious design — neutralise the driver's ownership test at `$B(a4)` and send the effects to extra voices
  — is not available, and not because of the driver: `$B(a4)` is read only to decide whether to write Paula and
  the music's bookkeeping is unconditional. It is bus timing. `machine.cpp` builds `audio_dma_` each line from
  `paula.dma_fetches()` and `Machine::fixed_dma()` charges the CPU for those slots, so changing which channels
  fetch, or at what period, moves the logic-step boundaries and with them the random generator and the whole
  game state. Instead Paula runs four mirror voices that are not on the bus at all, reading the driver's own
  shadow records at `$1B762 + ch*$C` — which the music tick keeps current on every tick whether or not an
  effect owns the channel (`$1A1EE`, `$1A1F2`, `$1A278`, `$1A27E`, `$1A28C`, `$1A2BE`, `$1A3A2`, `$1A3D4` are
  all unguarded and each writes the value the guarded Paula write after it would have written). A mirror starts
  a note on the driver's own note-start DMACON write at `$1A2E0`, which it issues whether or not a channel is
  held, and is audible only while the real channel is playing an effect — so with nothing firing the mix is
  bit-identical to the faithful one (`attract`, protect on against protect off scaled by the headroom: 0.84 LSB
  peak difference, which is int16 rounding).
  The note start has to be the write that *names* the channel, not a rising edge: on the hardware `$1A2E0`
  retriggers only because the note-off at `$1A240` cleared the bit a tick earlier, and `$1A240` is exactly what
  the driver skips while an effect owns the channel, so a rising-edge mirror holds one note and never plays the
  melody (measured: 10 % low, per-frame correlation against the unobstructed tune 0.78). Paula is therefore
  given the word the CPU wrote as well as the register's new value, and a named set restarts the mirror while
  the driver's `$B(a4)` flag says the channel is owned — and only then, so that its tied notes (`$83`) still
  play on when the channel is free. With that, correlation is 0.98 over the run and 0.95 inside the long rest,
  against a 0.995 ceiling set by the busy-waits that shift the music tick inside the VBL.
  Measured: inside the tune's 14.8 s rest the music goes from 174 to 2984 rms with fire tapped and 406 to 2852
  with it held, against 2610 with no firing at all, and the time with no music at all over 218 s of level 1
  falls from 41.8 s to 13.1 s against the tune's own floor of 14.7 s. It runs 8-14 % above the unobstructed
  level because a mirror still misses the one-tick cut `$1A240` would have made, so a protected voice is
  slightly more legato; reproducing that would mean reimplementing the driver rather than mirroring its
  registers. The effects are the same waveform at the same sample positions (max 0.84 LSB difference,
  correlation 0.99999999). Game state is untouched: all frame hashes and all 34,525 watched frames identical
  across six replays, and the full 512 KB of chip RAM byte-identical, so none of the driver's variables move —
  the mirrors only read. Identical audio under the default build, the translated executor and `--interp-only`.
  Protect mode carries a 0.64 output headroom because one side can then hold three voices where the game drove
  two (worst peak 0.852 on `level5` over all 17 replays, nothing clipped, against 0.78 for the faithful mix),
  so it is about 3.9 dB quieter and F8 puts it back; it is a complete no-op with `--music cdtv` and protects
  the tracker voices with `--music mix`. It cannot leak into a test: the whole `settings.cfg` read is behind
  `if (!opt.headless)`, verified by running a headless replay against a settings file that asks for it. New
  tests: the Paula unit test `protect_music` (including the retrigger both ways), and
  `protect_music_level1_play` / `protect_music_boss1`, which check the faithful frame hashes against new audio
  hashes in `tests/audio/<name>.protect.hashes`. With autofire on this is what keeps the tune playing, and
  `--sfx-volume` is the complementary knob.
  It can be switched while the game runs: `Paula::set_protect_music(bool)` fades the mirror voices and the
  headroom in or out together over 20 ms, so a toggle slides instead of stepping by 4 dB (measured: the largest
  sample-to-sample step at the switch is 38 of a level of 2560, against about 8,600 unfaded), keeps the mirrors
  running until the fade is over so no note hangs, and snaps when nothing has been output yet so starting with
  it on is unchanged. The unit test `protect_music_toggle` pins that, and that the DMA, interrupt and statistics
  counters are identical to a run that never toggled.
- `--audio-latency MS` (settings key `audio_latency`, 10-500, default 40): the live output's target buffer,
  which is also how long the host loop may stall before the sound breaks up. The window now also says so, at
  most once a second, when the buffer does run dry, so a host-side dropout is no longer mistaken for the
  game's own silence. The emulation, `--audio-out` and every replay and golden hash are unaffected.
- `docs/re-notes/README.md`: an index of the reverse-engineering notes.
- `docs/RELEASE-NOTES-0.1.0.md`.
- README: a "Status and known gaps" section separating what has been verified, and how, from what has not.
- **A hotkey for `--protect-music`**: **Delete**, or **Cmd+Y** on macOS where Delete is fn+Backspace
  (Backspace itself is god mode). It toggles the option while the game runs, so the same passage can be heard
  both ways without restarting; the switch is faded over 20 ms by `CdtvMusic::set_protect_music(bool)` so it
  cannot click, the new value is written back to `settings.cfg` on exit like the volume and scale hotkeys, and
  the console line says when the toggle is inert because the CDTV disc is the music. Audio only, as before:
  toggling cannot move a frame hash or any game state.
- The window title now advertises the modes a run is in — `[keyboard grabbed]`, `[autofire]`,
  `[protect music]`, `[CHEAT: god mode]`, `[CHEAT used]` — composed by one pure
  `x2::input::window_mode_suffix()` so there is a single place to change and it can be tested. A screen
  recording therefore shows which modes were on, which matters now that the picture and the sound both have
  non-faithful options.
- `input.cfg` is version 4, adding the `protect_music` action. The migration recognises the version 3 defaults
  as ours, so a file written by the version that added the assists picks the new key up; a file the user
  edited is still kept, and one that does not mention the action simply gets its default.
- **Autofire** (`--autofire[=RATE]`, `autofire` / `autofire_rate` in `settings.cfg`, Tab or Cmd+T), an
  assist rather than a cheat, so high scores are still saved. The game latches a fire *press* as an edge
  (`$2144`-`$214A` sets `$A01` only on a not-down-to-down transition) and the weapon fires again only on a
  fresh press, so **holding** fire gives about 5 shots per 400 frames while tapping gives about 60 — measured
  by counting the shot sound `$0289` with `--audio-cues`. The default rate, 17 pulses a second (one every
  third emulated frame), sits exactly on the game's own ceiling of one shot per 6.6 frames. It only engages
  during a level (the ship count `$CCA` is 0 at the title and in the SELECT menu) and only after fire has
  been held for about 0.3 s, so the menus, the shop and the continue prompt — which read the same press as a
  selection — are untouched, and a tap is always delivered exactly as it came in.
- **God mode** (`--god-mode`, `god_mode` in `settings.cfg`, Backspace or Cmd+I), a cheat: the ship's energy
  `$CC6` is refilled to `$27` every frame, so damage never kills it. This refills rather than patching the
  damage routine the way the `[t+3]` crack's trainer did (`bhi.b` to `bra.b` at `$5D84`), which still ran
  `sub.w d0,$CC6` first and left the energy word at zero or negative with the HUD bar drained; refilling
  keeps the energy display full and correct and the hit flash, sound and shield behaviour unchanged, and
  modifies no game code.
  It also covers the game's **other** death, which costs no energy at all. `$42E` is a push-back counter: when
  the ship is forced back along its recorded trail the handler sets it to -1 and decrements it once per logic
  step, and at -`$11` the code at `$5FE0` calls the death routine with the energy bar still full.
  `tests/replays/boss1.txt` reaches it at frame 2381 after 13 steps, so this is a real death in ordinary play
  and not a theoretical one. God mode therefore also traps the death routine `$5D00` at its entry, which
  catches both of its callers at once, refills the energy, clears `$42E` and returns through
  `Machine::do_rts()` — the same mechanism the disk shim uses — so the dying flag `$D84` is never set and the
  ship's handler carries on as if the hit had been survived. Clearing `$42E` is what stops the sequence
  sitting on its last step and dying again at once: measured, the hook fires once in `boss1`, the push-back
  restarts and is released normally, and the run reaches level 2 with the same scroll position as the
  baseline. A trap and not a byte patch, because the translated executor verifies each instruction's bytes and
  a patched routine would fall back to the interpreter; a trap that moves the PC behaves identically in both
  (`runtime/recomp/x2r_runtime.h`, `X2R_BOUNDARY`). It is only installed when god mode could be used, so a
  headless run without it is untouched. `$CA4` was rejected as a ready-made invulnerability flag: the damage
  and death routines do honour it, but it is a count of live objects that gates the whole end-of-level path at
  `$7B72`, so holding it would stop a level ever finishing.
- **Starting credits** (`--credits N`, `credits` in `settings.cfg`; Insert or Cmd+0 adds one mid-game). The
  game has no credits option — `options_menu` `$8CB6` just sets 3, and the SELECT menu only offers 1P/2P and
  music on/off — and its display (`$8F56`) pokes a single ASCII digit into the "CREDITS n" string, so 9 is
  the highest value that can be drawn and that is the cap. `--credits` is applied once, as a new game starts,
  so a continue still costs a credit.
- Cheats are visibly flagged and do not quietly inflate the high-score table: the window title gains
  `[CHEAT: god mode]` or `[CHEAT used]`, each toggle prints a line, and a session that used one does not write
  `highscores.bin` (the in-game table still shows the score; `cheat_scores = on` saves anyway).
- Three god-mode tests, all skipped without game data. `cheats_god_mode` runs `tests/replays/game_over.txt`,
  whose pokes exist purely to kill the ship, first unchanged — it must still reach GAME OVER with ships and
  credits at 0, and the test fails loudly if it ever stops, because then it would prove nothing — and then
  with `--god-mode`, where nothing may die and the energy must still be full. `cheats_god_mode_crush` does
  the same for the energy-independent push-back death in `boss1`: the baseline must execute `$5FE0`, god mode
  must report skipping a death called from `$5FE4`, and the game must still finish on the same level, which is
  what shows the sequence was ended rather than left stuck. `cheats_god_mode_equivalence` (recompiled builds)
  requires identical god-mode frame hashes from the translated executor and from `--interp-only`, since the
  hook moves the PC.
- The mouse cursor is hidden while the window has the focus and the game is running, and shown again while
  frozen, on focus loss and at exit (`cursor = auto|show|hide` in `settings.cfg`, `--cursor MODE`). The game
  has no mouse input — SDL's mouse events are never read, so no click or movement can reach it — so the arrow
  was only ever in the way. Implemented with `SDL_HideCursor()`, which merely stops drawing the pointer;
  `SDL_SetWindowRelativeMouseMode()`, which would also confine and capture it, is deliberately not used. The
  decision lives in a pure `CursorPolicy` class with unit tests, and `--input-debug` logs each change.
- `x2recomp --list-keys`: prints the key bindings this machine will use, including anything `input.cfg`
  changes, and exits without touching the file.
- `x2recomp --input-debug` (`input_debug` in `settings.cfg`): logs every keyboard and focus event with a
  timestamp, scancode, keycode, modifiers, repeat flag and the action or Amiga raw key it mapped to, every
  gamepad button, and the emulated frame that carried each joystick change. A key the OS swallowed produces
  no line at all, which tells a lost keypress apart from a dropped one.
- `--grab-keyboard` / `--no-grab-keyboard`, `grab_keyboard = auto|on|off` in `settings.cfg`, and a Scroll
  Lock / Cmd+G hotkey: ask the platform to route its own keyboard shortcuts to the window while it has the
  focus. Default off, released on focus loss, while frozen and on exit. It does nothing on macOS — SDL's
  Cocoa backend only silences the global hotkeys when SDL itself is built with `SDL_MAC_NO_SANDBOX`, which no
  released package does — and that is documented rather than papered over.
- `input.cfg` version 3 adds the `autofire`, `god_mode` and `add_credit` actions. The migration now knows
  every earlier set of defaults it wrote, not just the first, so a file written by the version that fixed the
  macOS bindings picks the new actions up instead of being mistaken for one the user had edited. A file is
  only ever compared with the defaults of the version it declares — we never wrote any other combination, so
  anything else was hand written and is left alone — and a file missing the new actions simply gets their
  defaults.
- `input.cfg` now carries a `# version:` marker and is migrated on upgrade: a file you have edited is used
  exactly as it is and never rewritten, while a file holding nothing but an older version's defaults is
  replaced with the current ones and kept as `input.cfg.bak`. Without this, a Mac that already had
  `p2_fire = Left Ctrl, Right Ctrl, Z` on disk would have kept the broken bindings forever.

### Changed

- `.github/workflows/ci.yml`: `push` and `pull_request` now carry `paths-ignore` for documentation-only
  changes (`**.md`, `docs/**`, `LICENSE`, `_specs/**`), so a docs commit no longer spends macOS and Windows
  minutes on this private repository. `workflow_dispatch` is unaffected, and any non-docs change still runs
  every job including hygiene.
- `docs/PORTING.md`, `README.md`, `docs/RELEASE-NOTES-0.1.0.md` and `docs/RELEASING.md` now state the real CI
  status and separate "builds and passes the no-data suite in CI" from "has been run with the real game",
  which are not the same claim.
- Packages now carry `LICENSE`, `THIRD-PARTY-NOTICES.md` and `PROVENANCE.md` next to `README.md`, plus
  `licenses/README.txt` indexing the third-party texts. `CPACK_RESOURCE_FILE_LICENSE` is set.
- `_specs/xenon2-recomp.md`: the open licensing question is annotated as answered (2026-10-30); the rest of
  the spec is left as a historical record.
- `README.md`, `docs/BUILDING.md`, `docs/RELEASING.md`, `docs/PORTING.md` and
  `docs/RELEASE-NOTES-0.1.0.md` state the licence instead of saying it is undecided. `docs/PORTING.md` also
  notes that a port carries the licence and that new files need the SPDX header.
- `tools/check_no_game_data.py`: deny list extended with further floppy-image, picture, audio, tracker-music
  and release-archive extensions; one more self-test case.
- README: the controls table now lists the port-1 joystick keys and the scale and volume hotkeys.
- `docs/BUILDING.md`: corrected a stale note that audio had not been compared with a reference emulator — it
  was, in Phase 4; only real A500 hardware remains uncompared. Added `vendor/musashi/softfloat` to the
  vendored-code table — it was linked in but undocumented.

### Fixed

- **The Linux release archive would not have started on a Raspberry Pi.** glibc is not bundled, so a tarball
  built in the Ubuntu 24.04 container the earlier Linux runs used (glibc 2.39) refuses to start on Debian 12
  "bookworm" or 64-bit Raspberry Pi OS (2.36) with nothing but `GLIBC_2.38 not found`. The release build is now
  `tools/release_linux.sh`, which builds in `ubuntu:22.04` (glibc 2.35) with GCC 12 and links libstdc++ and
  libgcc statically into the executable, so the archive needs only glibc 2.35, libm and the SDL3 beside it —
  no `GLIBCXX_` or `CXXABI_` requirement at all. The packaged binary was then run in a clean
  `debian:bookworm-slim` container with no packages installed: it reproduces the `level1_play`, `boss1` and
  `ending_natural` replay hashes exactly from the user's own game data. `README.md`, `docs/RELEASING.md`,
  `docs/PORTING.md` and `docs/RELEASE-NOTES-0.1.0.md` now give the minimum system, how to pick a video and
  audio backend, settings for weak hardware, and a numbered Raspberry Pi test recipe. 32-bit Raspberry Pi OS
  is not supported by the archive and this is now said explicitly.
- **`runtime/gfx/video.h` used `size_t` without including `<cstddef>`**, which GCC 13's headers happened to
  provide and GCC 12's do not. Found by building the release on the older baseline; it is now `std::size_t`
  with the include.
- **macOS: the keyboard defaults fought the operating system.** `p2_fire` was bound to Left/Right Ctrl, and
  Ctrl with an arrow key is a system-wide shortcut on macOS (Ctrl+Left/Right move between Spaces, Ctrl+Up
  opens Mission Control, Ctrl+Down App Expose), so firing while steering moved windows between screens and
  changed the focus instead of shooting. On macOS fire is now Z, X or either Option key and no Ctrl key is
  bound at all; Windows and Linux keep Ctrl, where it collides with nothing. Space is still deliberately
  unbound — the game reads it itself. A unit test asserts the platform defaults bind neither Ctrl (on
  macOS) nor any key the game reads.
- **macOS: host actions on F-keys the operating system claims first.** Without "Use F1, F2, etc. keys as
  standard function keys" the OS takes F3, F4, F5, F6 and F7-F12 and the keypress never reaches the game,
  which read as a key needing a second press. Every host action now also has a Cmd combination that the
  system does not claim: Cmd+1..Cmd+9 for F1..F9, Cmd+F fullscreen, Cmd+R reset, Cmd+U/E/D for the asset
  packs, surround and HD panel, Cmd+K the music source and Cmd+G the keyboard grab. Cmd+Q, Cmd+H, Cmd+M,
  Cmd+W and Ctrl+Cmd+F are left to the application menu SDL installs.
- **A key tapped between two emulated frames was dropped.** The joystick was sampled once per emulated
  frame from the set of currently held keys, so a press and release that both arrived in one SDL event batch
  left no trace — the other half of "sometimes needs a second press". A press is now latched and delivered
  for one emulated frame even if the key is already up, for keyboard and gamepad buttons alike. The latch
  lives only in the live SDL path, so scripted input, replays and golden hashes are untouched.
- Losing the window focus now drops the held keys and sends the game a release for any Amiga key it was told
  was down, so a key held while Cmd+Tabbing away no longer sticks.
- macOS: `SDL_MAC_PRESS_AND_HOLD=0` and `SDL_MAC_OPTION_AS_ALT=both` are set before the window opens, so
  holding a letter key repeats it instead of suppressing the repeat and offering the accent menu (what macOS
  does by default for A, S, Z and the rest — steering and fire keys here, not text), and the Option fire keys
  cannot start a dead-key composition. Either environment variable still overrides the default.
- **`input.cfg` could not always be read back after it was written.** SDL keeps a single mutable
  scancode-name table and each video backend renames entries in it at init — macOS turns "Left Alt" into
  "Left Option", Windows "Left GUI" into "Left Windows", X11 and Wayland "Application" into "Menu" — and
  `SDL_GetScancodeFromName` searches that same table. So the file the game wrote with a window open said
  `p2_fire = Z, X, Left Option, Right Option`, which `--list-keys` (which never calls `SDL_Init`) rejected as
  two unknown keys, and a file written on one platform could not be read on another. The file now always
  carries SDL's platform-independent spelling, and reading accepts every platform's spelling, so existing
  files keep working either way. A test pins both directions.
- `--list-keys` now says what the next start will do with the file instead of silently showing bindings that
  are about to be replaced: a file still holding an older version's defaults prints "this file holds the old
  defaults, so the game will upgrade it to version N on its next start" followed by the bindings that *will*
  apply. It still reads and writes nothing. The listing also notes that "Left Alt" and "Right Alt" are the
  Option keys on a Mac keyboard.
- Windows and Linux now list **Z and X first** among the fire keys (Ctrl stays bound; the macOS bindings are
  unchanged). Windows' "Show location of pointer when I press the CTRL key" accessibility setting (Control
  Panel > Mouse > Pointer Options) draws a ring at the pointer on every Ctrl press, which is constant while
  firing. That is the operating system, not the game, and the documentation now names the setting and says how
  to switch it off; binding a second Ctrl-free fire key means it can also just be avoided. Whether hiding the
  cursor suppresses the ring is untested and not claimed.
- **MSVC 2022 at `/O2` miscompiled Musashi's opcode-table generator**, dropping bit 14 from every
  generated mask and match (`0xf000` as `0xb000`). That crashed `m68ki_build_opcode_table()` — an unbounded
  loop walked off the end of the handler table — in every MSVC test that builds a `Machine`, and would
  otherwise have decoded every opcode against the wrong mask. Fixed in two vendored files:
  `vendor/musashi/m68kmake.c` computes each mask and match bit by bit instead of shifting a comparison, and
  `vendor/musashi/m68k_in.c` walks the handler table by each entry's own free bits instead of a hard-coded
  chain of unbounded mask-group loops. Both are marked in place with an `xenon2-recomp:` comment. **Musashi
  is therefore no longer a verbatim upstream copy** — and documenting this turned up a third, older local
  change that had never been recorded as one: `vendor/musashi/m68kcpu.h` uses `_setjmp` on macOS/BSD
  (commit `a0f4dcb`). All three are now listed in `docs/PROVENANCE.md` §9. Musashi's MIT-style licence
  permits modification and its notice is retained (`docs/PORTING.md` has the MSVC detail). The generated
  `m68kops.c` is byte-identical on macOS arm64 (md5 `b94fcb0a43c49d6237798bcb062dfac9`), and all replay and golden hashes
  are unchanged.

### Removed

- **SoftFloat is no longer compiled, linked or shipped.** Musashi can use it for its FPU, but this game is a
  68000 with no FPU, and one paragraph of SoftFloat's notice imposes an indemnification obligation that reads
  awkwardly under the GPL. `runtime/musashi_no_fpu.c` replaces `softfloat.c` in the `musashi` target,
  defining only the 15 symbols `m68kfpu.c` names; they are unreachable because Musashi enters its FPU only
  for 68030+ CPU types. Verified by `nm`: all 88 symbols unique to SoftFloat's implementation are gone from
  the binary (142 SoftFloat symbols before). No vendored file was edited, and the vendored headers stay
  because `m68kcpu.h` requires them. `packaging/licenses/SoftFloat-NOTICE.txt` removed, since no SoftFloat
  code is in a package. All replay and golden hashes unchanged.
- `packaging/LICENSE-TODO.txt`, the placeholder that stood in for a licence; `LICENSE` replaces it in
  packages.

## [0.1.0] — 2026-10-30

First collected version. The history below is grouped by the development phase that produced it; these were
never released separately.

### Added

#### Analysis (phases 0-2)

- Specification, the reverse-engineering notes in `docs/re-notes/`, and the analysis tools in `tools/`:
  ADF and AmigaDOS hunk readers, the disk depackers, a capstone-based 68000 recursive-descent analyser and a
  disk-image catalogue.
- A Ghidra headless code map of the resident game image and the six level modules
  (`tools/ghidra/`, `docs/re-notes/ghidra-code-map.md`).

#### The runtime (phase 3)

- Headless runtime core: the Musashi 68000 interpreter on a PAL Amiga 500 machine model — custom chip
  registers, copper, blitter, CIAs, interrupts and a disk shim.
- Paula audio: four DMA channels, the A500/A1200 output filter models, a band-limited resampler, and WAV or
  SDL3 output.
- Scanline renderer, SDL3 window, keyboard and gamepad input, blitter line mode.
- Device-clocked frame pacing in the window, with pause and reset flushing the audio queue.

#### Reference comparison (phase 4)

- An Amiberry harness (`tools/compare/amiberry-harness/`) and the comparison tooling behind
  `docs/re-notes/reference-compare.md`: pictures, palette, logic rate, stereo balance and game state against
  a reference emulator running the original disks.

#### Static recompiler (phase 5)

- The recompiler (`tools/recomp/recomp.py`) and the hybrid executor: the game's 68000 code translated to C++
  that is exactly equivalent to the interpreter, with `-DX2_RECOMP=ON`.
- Execution tracer (`runtime/trace/`), the replay corpus, and translation seeded from trace coverage.
- Per-replay interpreter-versus-hybrid equivalence tests.

#### Playability and import (phase 6a-6b)

- Native disk-image import (ADF, ADZ, ZIP or a folder) with a first-run import screen and drag-and-drop,
  writing to a per-user data directory.
- Persistent high scores and settings; volume and window-scale hotkeys.
- Boss-defeat and level-transition replays.

#### Accurate timing (phase 6c-6d)

- The accurate OCS bus model: blitter cycle sequencing, fixed DMA slots, CPU wait states and 313-line frames
  (`--timing accurate`, the default; `--timing fast` for the old zero-cost model).
- Copper DMA slots, the interrupt exception's bus-cycle placement, and MOVEM's extra read.
- `docs/re-notes/timing.md`, and timing-comparison tooling against the reference harness.

#### Portability, CI and packaging (phase 7)

- Portable build for macOS, Linux and Windows, with `CMakePresets.json`, a project version, and the
  `check_no_game_data` hygiene check.
- GitHub Actions CI covering macOS arm64/universal/x86_64, Ubuntu with GCC and Clang, and Windows with MSVC
  and MinGW-w64, plus a repository hygiene job. All eight jobs pass without game data (run `41041824614`,
  commit `95d9d78`); the data-dependent job is manual and self-hosted.
- Release packaging: a macOS `.app` in a zip or dmg, a Linux tarball and a Windows zip, each with `LICENSE`,
  the third-party licence texts and `SHA256SUMS.txt` (`tools/package.sh`, `tools/package.ps1`,
  `docs/RELEASING.md`).

#### Optional CDTV CD music (phase 8)

- A CD-audio music layer that plays the CDTV release's tracks in place of the tracker music, from your own
  CDTV disc image. Off by default (`--music cdtv` / `--music mix`).

#### Optional modernisation (phase 9)

- Window filters (nearest, sharp, scanlines, crt, edge) and the Faithful switch.
- Interpolated presentation at the display's refresh rate, from captured draw commands.
- Replacement asset packs on an RGBA-composited canvas, plus `--dump-assets` for pack authors
  (`docs/ASSET-PACKS.md`).
- Widescreen surround (16:10, 16:9, 21:9, 32:9) and an edge-upscaled HD status panel.

All modernisation features are off by default and none change game logic, audio or the faithful picture.

### Known limitations

See "Status and known gaps" in `README.md`. In summary: no comparison against real Amiga hardware; Windows
never built; CI never run; disc loads are instant, which diverges the game's random generator after every
level load. The licence question is now settled (GPL-3.0-or-later); the one open point is whether
SoftFloat's indemnification wording sits comfortably under the GPL (`docs/PROVENANCE.md` §7.3).
