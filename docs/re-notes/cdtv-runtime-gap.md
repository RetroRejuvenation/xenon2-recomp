# CDTV target: what the runtime lacks, and three ways to close the gap

Purpose: evidence and a recommendation for the plan to add the CDTV release (`X2`, `docs/re-notes/cdtv.md`) as a second
recompilation target. Read with `docs/re-notes/cdtv-slave.md` (what the WHDLoad slave says the program needs from the
machine; its verdict is the starting point here) and `docs/re-notes/cdtv-reference.md` (reference emulation).
Counts and addresses below come from reading the runtime at the time of writing (`main` after 0.2.0).

## 1. What the runtime is today

| Area | State |
|---|---|
| Memory | `kChipRamSize = 0x80000` (512 KB), `kChipMask` on every access: CPU reads/writes below `$200000` are `chip[addr & mask]`, copper, blitter, bitplane/sprite/audio DMA pointers and the trap index (`trap_index_[(addr & kChipMask) >> 1]`) use the same mask. Lines mentioning the constant or its mask: 23 in `machine.cpp`, 7 in `trace/tracer.cpp`, 6 in `machine_timing.cpp`, 5 in `recomp/executor.cpp` (translation lists indexed by `pc >> 1` below the chip size), 4 in `main.cpp`, 1 in `gfx/video.cpp`, plus `paula.attach(chip, mask)`. There is no fast RAM, no ROM, no autoconfig |
| Boot | `boot_game`: zero chip, copy `game_0400.bin` (`$3FC00` bytes) to `$400`, start the CPU at `$400` (or the build's `entry_pc`) with `SSP = kGameInitialSsp`, `SR = $2700`. The runtime runs **no operating system**: the disk game is bare-metal after its boot block |
| Builds | `BuildInfo` table in `boot.cpp` (canonical, Power Pack, 1762): restart vector, disk-loader PC, variable addresses, selected by the image's entry word and the manifest's `build=` line |
| HLE | PC traps (`Machine::add_trap(pc, fn)`): `DiskShim` replaces the game's own track loader at `$921A` (`m.do_rts()` returns to the caller after copying sectors); `retail_protection` answers the retail Copylock routine by moving the PC. Image bytes are never patched |
| Chip set | OCS: copper (line-granular), blitter, Paula (4 channels), two CIAs, sprites, 313-line accurate timing. The register table names some ECS registers (`BPLCON3`, `DIWHIGH`, ...); whether any has behaviour was not checked |
| CD audio | `runtime/cdtv/`: `cdtv_disc` (disc recognition, import of the audio tracks as raw PCM), `cd_player` (streams, resamples, adds to Paula's output, loop/crossfade, volume, pause modes), `cdtv_music` (maps events of the **disk** game's music calls to tracks) |
| Recompiler | `tools/recomp/recomp.py` + `runtime/recomp/executor.cpp`: translation keyed by absolute PC, per **module**: the resident image plus six level modules that the game loads to the single address `$54E00`; a module is identified at run time by an FNV-1a hash of its first 8 KB; seeds come from Ghidra code map + execution traces (`docs/re-notes/trace-coverage.md`); anything not translated falls back to Musashi |

## 2. What the CDTV program needs (summary of cdtv-slave.md §4 and §7)

- 811,612 bytes (`$C5E5C`) of chip RAM from address 0: CODE hunk 437,404 B at `$400`, BSS hunk 372,160 B at `$6B09C`, both
  flagged chip, 4,452 relocations (4,367 to hunk 0, 85 to hunk 1) — i.e. **more than the runtime's 512 KB**.
- Only 12 OS calls, all in one start-up block that the WHDLoad slave skips (`OpenLibrary`, `AllocSignal`, `FindTask`,
  `OpenDevice` x2, a read of `ExecBase.VBlankFrequency`) plus the CD-audio calls (`SendIO`, `DoIO`, `CheckIO`, `WaitIO`,
  `AbortIO` on `cdtv.device`, commands 43/56) and `Open`/`Read`/`Close` in ten file loaders. **No instruction addresses the
  CDTV hardware** (DMAC, drive controller, NVRAM, remote, front panel); all CD access goes through `cdtv.device`.
- After start-up the program takes the whole machine: level-3 vector at `$6C`, own copper/blitter use, `INTENA` off at the
  start; CIA-A `$BFE001` (fire), CIA-B `PRB` write once, joystick port 2, `POTGOR` bit 14 for the second button.
- Overlays (`LEVEL1-5`, `INTRO`, `PICCODE`) load into one fixed buffer of 180,000 bytes inside the code hunk and are
  relocated by the program's own routine; `PICDATAn`, `SPEECHnn`, `SOUNDSnn` are data going to fixed buffers.

## 3. Gap list, by what must change

| # | Gap | Needed by | Size | Notes |
|---|---|---|---|---|
| G1 | **1 MB chip RAM with 20-bit DMA pointers**, runtime-selectable, 512 KB behaviour (including mirroring at `$80000`) unchanged for the disk builds | all architectures | M | Turn `kChipRamSize/kChipMask` into per-machine values (or a template/constant per build target). Check the mirror: a 512 KB OCS machine mirrors chip RAM; the disk game may rely on it. `video.cpp`, `paula.attach`, copper `cop_pc_`, blitter pointer wrap, `trap_index_`, the executor's per-PC lists and the tracer all index by the mask. Pointer high-word width matters (OCS Agnus decodes 3 bits of the high word, ECS 5): 1 MB needs a 20-bit pointer. All 128 existing tests and every replay hash must stay unchanged |
| G2 | Loader for the CDTV program (hunk reader + relocator at a **fixed** base `$400`) and for the files on the disc | 1, 3 | M | `docs/re-notes/cdtv-slave.md` §2: hunk 0 at `$400`, hunk 1 at `$6B09C`, 2 hunks, relocation entries in hunk-relative form (either hunk). The runtime has no hunk loader; the importer has an OFS/FFS reader but only a volume-detection stub for ISO 9660 (`cdtv_disc.cpp` recognises the `XENONII` volume; no file reader) |
| G3 | File service for ten call sites (`LEVELn`, `INTRO`, `PICCODE`, `PICDATAn`, `SPEECHnn`, `SOUNDSnn`) reading from the disc image | all | S | Same pattern as `DiskShim`: a trap at each loader PC, copy to the buffer, `rts`/continue at the post-`Close` address. Data total about 4.4 MB; read from the imported disc (no ISO reader needed if the importer extracts the 96 files, or an ISO 9660 level-1 reader of ~150 lines) |
| G4 | Start-up shortcut: enter at the program's entry, skip the OS block (`$3AA4A..$3AAE7`) with a PC trap, store the PAL word | 1, 3 | S | Same idiom as `retail_protection` (trap that sets the PC); `SSP` set to `$84F9C` by the program itself |
| G5 | CD-audio events -> `cd_player`: `play_track` (`$3A924`), stop (`$3A8F6`), tick (`$3A95C`), volume (`$3A9BA`/`$3A9E6`) | all | S-M | The player already does play/stop/volume/loop/crossfade keyed on emulated time; the existing `cdtv_music` cue layer is for the *disk* game and is bypassed: here the game itself says which track. The faked `IORequest`/`CheckIO` result is the one subtle part (end-of-track poll every 50 VBLs, loop gap up to 1 s as on hardware) |
| G6 | Input: `JOY1DAT`, CIA-A PRA bit 7, `POTGOR` bit 14 / `POTGO` writes for button 2; CIA-B PRB writes accepted | all | S | `POTGOR` is a constant (`$FF00`, buttons released, `machine.cpp`), so button 2 has no input source yet; `POTGO` writes need a check |
| G7 | Recompiler generalised to a second program: module table, base address, overlay identification, code-map-less seeding | recompiled build | L | See §5 |
| G8 | Persistence (highscores), language/menu behaviour, speech and sound drivers | all | S-M | The CDTV original keeps no table across runs; the runtime's own store can be used. The speech/sound drivers address Paula through a base register: not yet checked for anything beyond DMA and VERTB |
| G9 | Replay corpus, goldens, CI, importer (`--import-cdtv` writing the relocatable image's inputs), UX | all | M | New `tests/replays/cdtv/`, asset format, manifest `build=cdtv`, variant registry entry |
| G10 | Reference for goldens | all | S-L | `docs/re-notes/cdtv-reference.md`: the disc does not yet boot in Amiberry with the only CDTV ROM held; WHDLoad route is available |

**CDTV hardware the Machine lacks** (needed only by architecture 2): the DMAC / drive controller and its command
protocol (the cdtv.device-to-drive interface), the 256 KB extended ROM at `$F00000` (and `$E00000` alias), the 64 KB
NVRAM card and battery-backed clock, the front-panel / infra-red remote (keyboard-port style input from a CD1401-class
controller), CDTV-specific autoconfig, 1 MB chip RAM. None of it is used by the program itself.

## 4. Three architectures

### 4.1 (1) HLE mini-OS inside the runtime

Load `X2` with a runtime `LoadSeg`-style loader and provide the OS pieces it touches: an `ExecBase` at `$4`, library
bases and jump tables with trap stubs for `exec`, `dos`, `cdtv.device`, a heap for `AllocMem`.
- Facts: the program makes about a dozen exec/dos calls, all but the file loaders in a start-up block that no one needs
  to run. A real mini-OS (message ports, `OpenDevice`, IO requests, tasks) is far more than that.
- It gains nothing over (3) for this program: everything it would emulate is skipped or replaced by a trap anyway.
- It *loses* determinism: a runtime `AllocMem` chooses load addresses, which the recompiler's absolute-PC translation
  cannot tolerate unless the loader is pinned to a fixed base, which is exactly (3).
- Effort: about 35-40 working days (15-20 more than (3), for a mini-OS nothing uses); risk: medium; legal: clean. **Not recommended.**

### 4.2 (2) Emulate CDTV hardware and run the real CDTV ROMs

Add the CDTV motherboard to `Machine`: extended ROM and Kickstart mapping, the DMAC and drive-controller command
interface that `cdtv.device` speaks, NVRAM/RTC, the remote/front-panel controller, enough CIA/serial/keyboard behaviour
for the real OS to boot, then boot the disc and let `rmtm`, `CDTVPrefs` and `x2` run exactly as on a CDTV.
- Needs **user-supplied ROMs** (Kickstart 1.3 plus a CDTV extended ROM). The project could not ship or test them in CI;
  the importer and every test would need an optional ROM path. GPL is not an obstacle (nothing of the ROMs is
  copied), but the user's legal ROM supply (e.g. a Cloanto package) becomes a prerequisite for playing.
- Largest surface: the OS reads and writes a long list of hardware the game never touches; timing sensitivity of a real
  boot; the disc did not boot in Amiberry with ROM v1.0 (`cdtv-reference.md`), so even the reference is unproven.
- The OS chooses the program's load address (`AllocMem`/`LoadSeg`); the recompiler's absolute-PC keyed translation then
  depends on the OS's allocation being deterministic, which is not guaranteed across ROM versions.
- It does give faithful CD-audio and speech behaviour for free (the real `cdtv.device` talks to a modelled drive), and the
  CDTV remote and language menus exactly as shipped.
- Effort: 35-50 working days; risk: high; legal: needs user-supplied ROMs. **Not recommended** except as a long-term
  addition after (3) exists, if exact OS-level behaviour ever matters.

### 4.3 (3) Pre-link at import, bare-metal start, HLE only at the call sites

The importer reads the user's disc, relocates `X2` to the fixed base `$400` (hunk 0 at `$400`, hunk 1 at `$6B09C`) —
what the WHDLoad slave does with `resload_Relocate` — and writes a memory image plus the 96 data files into the data
directory, like `game_0400.bin` and `disc2.bin` for the disk build. The runtime starts the image bare-metal and installs
PC traps (no byte of the program is modified):
- skip the OS block and set the PAL word (G4);
- the ten file loaders (G3);
- the four CD-audio call sites (G5).
This is the disk build's design extended: `DiskShim` and `retail_protection` already show both idioms.
- Facts that make it work: the program does not use the OS after start-up, never touches CDTV hardware and takes over
  the machine like the disk build; the WHDLoad slave runs it in 811,008 bytes of chip RAM with only these 17 patches and
  none of them timing-related. Fixed-base relocation is also what keeps the recompiler's absolute-address translation
  valid.
- Risks: the unknowns in `cdtv-slave.md` §7 (what the `0x601A` overlays and the speech/sound drivers do at run time); G1 is
  the one invasive change to existing code (mask constants in 7 files); the CD-audio `CheckIO` fake; the relocation
  entries use either hunk (must read the tables, not add `$400`).
- Legal: no ROMs; the image derives from the user's disc and stays in the user's data directory (like the disk
  build's), and a recompiled build of it is as non-redistributable as the disk one.
- Effort (working days, single developer): G1 3-4, G2 2-3, G3 1-2, G4 0.5, G5 2-3, G6 1, G8 2-3, G9 3-4, plus G7 below;
  the interpreter-only playable target is **about 15-20 days**; with the recompiler about **25-30 days**. Testability is
  as good as the disk build's: replay hashes recorded from our own runtime, picture compared with the WHDLoad run
  (`cdtv-reference.md` §5), CD events compared as logs. **Recommended.**

| | (1) mini-OS | (2) real ROMs | (3) pre-link + traps |
|---|---|---|---|
| Effort | ~35-40 d | ~35-50 d | ~15-20 d interpreter, ~25-30 d recompiled |
| User-supplied ROMs | no | yes | no |
| Recompiler fit | needs fixed load (= 3) | fragile (OS picks addresses) | direct |
| CD audio fidelity | event-level | real drive protocol | event-level (as `cdtv_music`) |
| Risk | medium | high | low-medium |

## 5. How the recompiler would cope (G7)

- **Relocatable code.** Translation is keyed by absolute PC, so the load address must be fixed: pre-linking at `$400`
  makes `X2` just another resident image (its entry, vectors and relocated absolute operands are fixed). The
  constants that are per-target today are `GAME_BASE = 0x400`, `MODULE_BASE = 0x54E00`, `CHIP_END = 0x80000`, the
  `MODULES` list and the 8 KB FNV overlay key (all in `tools/recomp/recomp.py`); they become per-target data, and
  `CHIP_END` follows G1.
- **Overlays.** The disk build loads six level modules to `$54E00` (`MODULE_BASE`), each identified by a hash of its first
  8 KB, and keeps one executed-byte map per module. The CDTV program loads seven (`LEVEL1-5`, `INTRO`, `PICCODE`) into one
  fixed 180,000-byte buffer inside the code hunk and relocates them itself (the Atari-ST-style `0x601A` fix-up routine at
  `$3D2EE`) — so after the load the overlay is code at a fixed address, the same situation as the disk build. The module
  key and the "which module is mapped at the buffer" attribution in `trace/tracer.cpp` work as they are, with a new table.
  The overlay's *base* is inside the program image: the translation of the resident code and of an overlay must not overlap
  in the dispatch lists, which the per-module scheme already guarantees.
- **Seeds and coverage.** The disk build's functions come from a Ghidra code map and traces of 19 replays (83 % of the
  decoded instructions executed). The CDTV program has neither yet: a replay corpus and a code map (REA/Ghidra) are
  prerequisites; until then everything falls back to the interpreter, which is slower but correct. Indirect jumps through
  tables, self-modifying code and `jsr (a1)` with a non-zero top byte (needs a 24-bit mask in translated code) are the
  usual checks (`cdtv-slave.md` §5).
- **Disk-loader HLE.** The disk build's trace marks the disk shim's redirect; the CDTV traps are redirects of the same
  kind (`m.do_rts()` or a PC change), which the tracer already records.

## 6. What the CD-audio layer needs

`CdPlayer` plays a `TrackSource` (an imported raw PCM track) from emulated-time events. For the CDTV build the game
calls `cdtv.device` itself; the traps at the four call sites translate: command 43 with `io_Offset = track` -> `play(track)`;
`AbortIO`+`WaitIO` -> `stop()`; command 56 -> `set_volume`; the tick's `CheckIO` -> "finished" when `CdPlayer`'s track
has ended, so the game restarts it (loop gap as on hardware, or crossfade). The current `cdtv_music` cue table (disk
game state -> track) is not used for this target. The data track's speech and sound banks are *files* (G3), played by
Paula; only the Bomb-the-Bass tracks (2-13) come from the CD-audio path.

## 7. Recommendation

Architecture (3): pre-link at import, bare-metal start, PC traps at the OS and file call sites. Order of work: G1
(1 MB chip, behaviour-neutral), then G2-G4 (a loader, ten traps, start-up shortcut: the program should reach its title
screen under the interpreter), then G6, G5, G8, then goldens and CI (G9), then the recompiler (G7). Before committing to
G7, a small time-boxed disassembly of one overlay (is it free of OS calls and self-modification?) removes the biggest
unknown. For reference output use the WHDLoad slave route (`cdtv-reference.md` §5). Keep (2) as a possible later
addition if exact CD-drive behaviour or the remote/menu flow ever needs to be reproduced.
