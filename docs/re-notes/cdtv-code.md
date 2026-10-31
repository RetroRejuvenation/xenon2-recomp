# CDTV game code: `X2` and its overlays

Evidence for a plan that adds the CDTV release as a second recompilation target. It extends `cdtv.md` (disc, tracks,
files, language menu) and `cdtv-music-map.md` (CD-audio cue tables) with what the **code** needs. No game bytes and no
disassembly listings are reproduced here: offsets, counts and prose only. All offsets are **hunk offsets** of the
`X2` code hunk unless stated otherwise (file offset = hunk offset + 36).

## 0. Verdict

1. **Not bare-metal.** `X2` is a normal AmigaOS program that keeps the OS alive. It takes over the *hardware* (display,
   copper, blitter, sprite and bitplane DMA, the level-3 interrupt), but it still calls **exec**, **dos.library** and
   **cdtv.device** while the game runs: `dos` Open/Read/Close for every overlay and sample bank (10 load sites), and
   cdtv.device I/O for CD audio. It chains its level-3 handler to the OS's original handler. What is needed is a
   small OS HLE (12 library vectors plus two cdtv.device commands, section 2), not a full OS.
2. **Same engine, rebuilt.** 77.7 % of the disk build's decoded instructions (relocation-tolerant shapes, windows of 6) appear in
   `X2`; the blitter, object and main-loop code is about 95 %. Only 49.1 % of `X2`'s decoded instructions appear in the
   disk image, because the CDTV build adds menus, languages, CD audio, speech and an overlay loader (section 5).
3. **The game variable layout is the disk build's layout shifted by a constant `-$3FA`** (level `$E04`->`$A0A`, frame
   counter `$41C`->`$22`, input latches `$A00/$A01`->`$606/$607`, energy `$CC6`->`$8CC`, ships `$CCA`->`$8D0`, score `$C92`->`$898`),
   and the engine API table (56 `jmp abs.l` slots) is the same table at `$A12` instead of `$E0C`.
4. **The level code is the disk build's level modules.** `LEVEL1`..`LEVEL5` are 95-96 % byte-identical to the disk
   build's level modules (the rest is relocated pointer fields plus about 366 changed or added bytes in the first
   ~6.5 KB of each module), with the same header ABI (section 6).
5. **The load address and the overlay area are fixed by the program,** and everything is `MEMF_CHIP`: the code hunk
   (437,404 B) and the BSS hunk (372,160 B) are both flagged chip, **809,564 B in total**. The runtime's chip RAM is 512 KB
   (`kChipRamSize = 0x80000`), so a CDTV target needs a **1 MB chip RAM** machine (the CDTV has 1 MB chip).
6. **Hardest unknowns:** the CDTV hardware beyond the custom chips (none is touched by `X2`; the remote and any IR input are
   outside it), how the OS side of a faithful HLE should behave for the cdtv.device completion model, and the Paula
   effect player's interrupt use. See section 11.

## 1. Inputs

| Input | Size | sha1 |
|---|---|---|
| `X2` (AmigaDOS hunk executable, CDTV EN/FR/DE/IT disc image, Redump-style) | 455,288 | `f2d180efb1816e703a227f2ae50008569066dffd` |
| `INTRO` (0x601A overlay) | 95,816 | `ea81afd3d4e9bd2f9ce4bb16501d1b77b7fbc73b` |
| `LEVEL1` | 118,347 | `8dbb4cc9ae960dab2403d9c9335ec004b438818d` |
| `LEVEL2` | 131,487 | `0389ffcf0709fbc01b3679d6f8a46fe95fd55d17` |
| `LEVEL3` | 142,292 | `7881c9d706ed72880782df51a507874f59869dfa` |
| `LEVEL4` | 178,589 | `2982e216eef50e1efe2484c3b26ca79eb0c61b34` |
| `LEVEL5` | 161,445 | `5476e22a2d963c3c40261a9454a0227a59006071` |
| `PICCODE` | 30,203 | `a14dbe1c260baf0a137a3ef446ede7a905aa82c6` |
| disk build game image, canonical (decoded from the BS1 `[t+3]` release, `game_0400.bin`, base `$400`, `$3FC00` B) | 261,120 | `1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4` |

Disk-build level modules: the six modules decoded from disc 2 (base `$54E00`), as described in `ghidra-code-map.md`.

Method: `tools/hunk.py` for the hunk table and relocations; a recursive-descent decode of the code hunk (seeded from
`tools/analyze68k.py`'s function and label lists and the entry point), a linear listing for reading and grepping, and
instruction-shape windows (mnemonic plus operand shape with absolute addresses, branch targets and immediates above `$FF`
masked) for the comparison with the disk image. **The descent reaches about 11,000 instructions (47 KB) of the roughly
130 KB code region `$32000-$52000`; the rest is data, jump-table targets and code only reachable indirectly.**
Percentages below are over decoded instructions, not over bytes, unless stated.

## 2. Startup and operating-system use

Hunk table: hunk 0 = CODE, 437,404 B, `MEMF_CHIP`, 4,452 `RELOC32` entries (4,367 pointing into hunk 0, 85 into the BSS
hunk 1); hunk 1 = BSS, 372,160 B (`$5ACC0`), `MEMF_CHIP`. No symbol or debug hunks.

Entry (`$3A612`, reached from a `jmp` at hunk offset 0):

1. Saves the stack pointer, sets its own stack at BSS+`$19F00`, clears BSS `[0, $1D9AC)` and fills `[$1D9AC, ...)` with
   `$FFFFFFFF` (the effect player treats a record whose data offset is `$FFFFFFFF` as an empty slot). **The fill is `$F505` longwords, which ends 256 bytes past the end of the BSS
   hunk**; the allocation must have that slack (or the fill must be bounded in an HLE).
2. `OpenLibrary("dos.library")`, `AllocSignal(-1)`, `FindTask(0)`, builds one `MsgPort` and two `IORequest`s by hand in its own data
   (`$3A4BE`, `$3A45E`, `$3A48E`), then `OpenDevice("cdtv.device", unit 0)` twice.
3. Disables interrupts (`INTENA = $4000`), saves the OS's level-3 vector (`$6C`) in `$3A452/$3A456`, installs its own
   (`$34BB2`), deselects the floppy drives (`$BFD100`), programs the display, copper and DMA directly, re-enables the master
   interrupt. It reads `$4.w` once more for `ExecBase->VBlankFrequency` (`$212(a6)`): 50 selects the PAL logic rate (every 3rd
   frame), anything else the NTSC rate (every 4th), the same PAL/NTSC split as the disk build's frame divisor at `$410`.
4. Runs from there as the main game task in **user mode**: there are no `Supervisor`, `trap`, `stop` or status-register
   instructions in the code (hits in the raw listing are data).

### 2.1 Every library vector it calls

| Library | LVO | Function | Where | When |
|---|---|---|---|---|
| exec | -552 | OpenLibrary | `$3A656` | startup only |
| exec | -330 | AllocSignal | `$3A662` | startup only |
| exec | -294 | FindTask | `$3A690` | startup only |
| exec | -444 | OpenDevice | `$3A6D0`, `$3A6E4` | startup only (two requests, same device) |
| exec | -462 | SendIO | `$3A556`, `$3A5AA` | CD audio start (every track change) |
| exec | -468 | CheckIO | `$3A584` | **from the level-3 interrupt**, every 50th tick (loop the track) |
| exec | -474 | WaitIO | `$3A514` | CD audio stop |
| exec | -480 | AbortIO | `$3A50A` | CD audio stop |
| exec | -456 | DoIO | `$3A5E0`, `$3A60C` | volume set |
| dos | -30 | Open (mode `$3ED` = `MODE_OLDFILE`) | 10 sites, section 2.2 | at every state change, **retried in a loop until it succeeds** |
| dos | -42 | Read | same sites | |
| dos | -36 | Close | same sites | |

That is all: **no `Forbid`/`Permit`/`Disable`/`Enable`, no `AllocMem`/`FreeMem`, no `Write`, no graphics, intuition, audio.device,
timer.device, input.device or playerprefs call, and no other library is opened.** (`RMTM` and `CDTVPrefs` run before `X2`
from `S/STARTUP-SEQUENCE`; they are separate executables not analysed here.) Nothing in the code writes a file, so no score or
setting is saved by `X2` itself.

cdtv.device commands (as in `cdtv.md` section 5): command 43 with `io_Offset` = track, `io_Length` = track + 1 (play a track),
command 56 with `io_Offset` = `$7FFF` (volume). Track numbers are 2..13 of the disc, chosen from the cue tables in
`cdtv-music-map.md`. The command numbers have not been checked against `CDTV.TM`.

### 2.2 The ten load sites (all `CD0:<name>`, the loop retries until `Open` returns non-zero)

| Open at | Reads into | Max bytes | Loads |
|---|---|---|---|
| `$3353C`, `$3CEB0`, `$3D47A`, `$3DCA6` | hunk+`$4A26` (overlay area) | `$2BF20` (180,000) | `LEVELn` (digit patched into the name at run time) or `INTRO` |
| `$50BDA` | hunk+`$4A26` | `$2BF20` | `PICCODE` |
| `$50B74` | the pointer held in overlay header slot `$4A42` | `$1FBD0` | `PICDATAn` |
| `$3C3EA`, `$40DB4` | the pointer variable `$40DE4` (BSS+`$4B3C0` region) | `$1A518` | `SPEECHnn` |
| `$4F934`, `$4F984` | BSS+`$1D9AC` | `$2DA14` (186,900) | `SOUNDSnn` (10 banks, chosen from the level number `$A0A`) |

Each load of an overlay is followed by the overlay relocator (section 6) and, before the load, the game swaps its level-3
vector to a bare handler (`$34B7C`, or `$34B66` in the speech path) and restores `$34BB2` afterwards, so the interrupt keeps
running while `dos` waits for the disc.

### 2.3 What an HLE has to provide

- **exec:** a valid `ExecBase` at `$4` whose `+$212` byte is 50 (PAL); `OpenLibrary` returning a non-zero base for `dos.library`;
  `AllocSignal` (any bit), `FindTask` (any non-zero pointer); `OpenDevice` for `cdtv.device` returning 0 and leaving the request
  structures valid; `SendIO`/`DoIO`/`AbortIO`/`WaitIO` and `CheckIO` with the completion model the track loop uses (the
  game re-issues the track when `CheckIO` reports the request done).
- **dos:** `Open`/`Read`/`Close` of the disc's 101 `CD0:` files (ISO 9660), reading at most the requested length; a short read is normal.
- **The level-3 chain:** `$3A456` holds the OS's original level-3 vector, and the game's handler ends with `move.l $3A456,-(a7); rts`.
  The stand-in must be a routine that acknowledges what the OS would (the game never writes `INTREQ` for VERTB; it relies on the
  OS handler to clear it) and returns with `rte`.
- Nothing needs `Supervisor` or `VBR` support: the vector table is at address 0 (68000).

## 3. Hardware the code touches itself

Custom registers (absolute addresses in the decoded code; `a4 = $DFF000` addressing is listed separately):
`DMACONR` (`$002`, 38 sites, blitter-wait loops), `VPOSR`/`VHPOSR`, `JOY1DAT` (`$00C`), `POTGOR` (`$016`, bit for the second button of port 2),
`INTREQR` (`$01F`), `POTGO` (`$034`, set to `$0C00` at startup), blitter (`BLTCON0/1` `$040/$042`, `BLTAFWM/ALWM` `$044/$046`, `BLTCPT/BPT/APT/DPT` `$048-$054`,
`BLTSIZE` `$058`, `BLTCMOD..BLTAMOD` `$060-$066`, `BLTADAT` `$074`), copper (`COP1LC` `$080`, `COPJMP2` `$088`), display
(`DIWSTRT/STOP` `$08E/$090`, `DDFSTRT/STOP` `$092/$094`, `BPLCON0..2` `$100-$104`, `BPL1MOD/BPL2MOD` `$108/$10A`, sprites `$144..$17C`, colours
`$180-$1BE` with the palette routine at `$3A3CC`), `DMACON` (`$096`), `INTENA` (`$09A`), `INTREQ` (`$09C`), `ADKCON` (`$09E`).
**Only OCS registers are used** (no `BLTSIZV/H`, `DIWHIGH`, `BEAMCON0`, `FMODE`).

Paula is reached through `a4 = $DFF000` in the effect player at `$330B6-$3321E` (offsets `$A0/$B0` for channels 0/1 and
`$D0/$C0` for 2/3, `DMACON`/`INTREQ` through `$96/$9C(a4)`); no absolute audio-register address occurs anywhere, which is why a
scan for `$DFF0A0` finds nothing. The player is driven from the level-3 handler (`jsr $330B6`), reads two command slots (`$16`, `$1C`)
and 12-byte sample records from the `SOUNDSnn` bank at BSS+`$1D9AC` (data offset, length, period, loop/one-shot flag), pans by screen X
(0..`$140`) into per-channel volume (`mulu #$CC`), and arms the audio interrupt flags. CIA-A is touched only for fire (`$BFE001`, bit 7) and the
power LED (bit 1 set at `$330CC`); CIA-B only for `$BFD100` (drive deselect). **No keyboard or serial register (`$BFEC01`, `$BFED01`) and
no CIA timer or ICR is read or written, and no CDTV-specific register (`$DC0000`, `$E90000`, `$DE0000`) is addressed.** The
other high addresses a raw scan finds are misdecoded data.

## 4. Memory map

All addresses are hunk offsets (BSS offsets marked "bss"). Everything is chip RAM.

| Range | Content |
|---|---|
| `$0-$6` | `jmp $3A612`, the entry |
| `$0-$CA80` | zero-initialised engine variables; **same layout as the disk build's `$3FA-$CE7A` range, shifted by `-$3FA`** |
| `$A12-$B7E` | engine API table: 56 `jmp abs.l` slots (`$A12..`), plus 7 pointer longs at `$B62-$B7A` |
| `$4A26-$30946` | **overlay area**: zero in the file, filled by `Read` (up to `$2BF20` bytes) |
| `$30946-$32000` | data |
| `$32000-$51000` | code (about 124 KB; menus and CD code in `$3C000-$42000`, speech/sound loaders `$4E000-$50000`, front end `$50000-`) |
| `$51000-end` | strings, tables, picture and font data |
| bss `$0`, `$CA80` | two screen buffers, `$CA80` (51,840 B) each = 4 planes x 40 B x 324 rows; the pointers are kept at `$8` and `$C` |
| bss `$19F00` | top of the game's own stack |
| bss `$1A154` | 200-entry row-offset table (stride `$A0`) |
| bss `$1D9AC-$4B3C0` | sound bank buffer (`SOUNDSnn`, `$2DA14` bytes) |
| bss `$4B3C0-` | speech buffer (`SPEECHnn`, `$1A518` bytes) |

Absolute addresses in the code are **hunk-relative and relocated by the loader** (4,452 `RELOC32` entries); the only unrelocated
absolute addresses are hardware (`$DFFxxx`, `$BFxxxx`), the exec base (`$4`) and the vector (`$6C`). A target that
fixes the load address can apply the relocations at import time or at load time, and then addresses are constant for the
recompiler (as `$400` is for the disk image). Pointer values are sometimes masked with `$FFFFFF` (`andi.l #$FFFFFF` at `$3AACC`), the same
24-bit idiom the disk build uses; the code has no top-byte flag tricks beyond that that were checked.

## 5. Relationship to the disk build

Instruction-shape windows of 6 consecutive decoded instructions, over the decoded instruction sets (X2: 11,079, disk: 7,607):

| | matched |
|---|---|
| `X2` instructions found in the disk image | 49.1 % (5,441 of 11,079); windows of 10: 46.9 % |
| disk-image instructions found in `X2` | 77.7 % (5,912 of 7,607); windows of 10: 75.4 % |
| `X2` `$32000-$3C000` (engine) | 81.5 % (5,031 of 6,172) |
| `X2` `$3C000-` (menus, CD audio, speech, loaders, front end) | 8.4 % (410 of 4,907) |
| disk `$400-$9000` (blitter, object system, main loop, menus) | about 90 % over the nine 4 KB blocks (5,878 of 6,547; e.g. `$0`: 648/668, `$4000`: 737/765, `$5000`: 756/800) |
| disk `$9000` (floppy track loader) and `$19000-$1B600` (tracker music engine) | 0 % |

Named disk routines: blitter pass `$17D6` 32/39 instructions found; blit wrapper `$10F6` 18/18; free-list ops `$0FEE` 6/12;
main init `$763C` 81/149; options menu `$8CB6` 48/73; level-3 handler `$20C2` 32/68; keyboard IRQ `$0FAC`, sector reader `$921A`, track
reader `$93F8`, music tick/command/init `$19FAC/$19FD6/$19FFC` all 0 (removed). The level-3 handler is rebuilt around the same
joystick decode (the latches, `JOY1DAT`, the fire bit) but it also runs the CD tick, the effect player and the OS chain.

**New in `X2`:** the overlay loader and relocator, `CD0:` loading, the CD-audio control (`$3A4F6-$3A5EA`), the speech narrator (`$40DEC`),
language selection and the language text tables, the music-select screen, the joypad-mode screen, the Paula effect player
(`$330B6`; it replaces the tracker engine), the picture/info front end that loads `PICCODE`/`PICDATAn`, and the
OS start-up. **Removed:** the disk shim and track loader, the keyboard interrupt, the tracker music engine, the trainer, and
all copy protection (none of the Copylock key, the `X2B ` disc id, `INSERT GAME DISK`, `NuXENON2` or `Rob Northen` strings occurs in `X2`).

The code that is shared is not just similar: the `Copyright 1988`, credits and `GET READY PLAYER 1` strings are the same, and the
variable shift in section 0 is exact for every variable checked (level, frame counter, input latches, energy, ships, score; reference
counts in `X2` of 27, 30, 46/82, 10, 7 and 8). The RNG (`$1F50` in the disk build) would be `$1B56`, which is **not referenced** in
`X2`: where the CDTV build keeps its random seed is not yet found.

## 6. The overlays (`0x601A` files)

**Format.** 28-byte Atari-ST-style program header (`$601A`, text length at `+2`, data/BSS/symbol sizes all 0), the text, then a
relocation byte table: the first long is the offset of the first fix-up, then one byte per next fix-up (`1` = skip 254, `0` = end).
Counts: `LEVEL1`..`5` = 681 / 899 / 975 / 1,042 / 1,109 relocations, `INTRO` 364, `PICCODE` 13.

**Relocator (`$3CEEE`).** It is `X2`'s own routine, called after every overlay `Read`. For every listed longword it adds the
hunk base **plus 6** (`addi.l #<hunk base + 6>`): the overlay was linked so that stored values are hunk-relative minus 6 and the
overlay lives in the same address space as the main program. The effect of the relocations on targets: every relocated field
lands either in the overlay area (about 57-59 %) or in the engine's low region `$0-$4A26` (about 41-43 %: variables, and the 56-slot API table);
**no overlay relocation points at main code** (`$32000` and up). Overlays call the engine only through the API table at `$A12`.

**Load address.** Always hunk+`$4A26` (text starts at `$4A42`), one overlay at a time; `INTRO`, `LEVEL1-5` and `PICCODE` share the area, so
a recompiler must key translated code by (overlay, address) exactly as it keys the disk build's six modules at `$54E00`.
`PICDATAn` is read into the area the loaded `PICCODE` names.

**Header ABI (identical for the five level overlays and similar for `INTRO`/`PICCODE`).** 60 bytes at the start of the text, called or read
by main code at fixed hunk addresses:

| Slot (from `$4A42`) | Kind | Used by main as |
|---|---|---|
| `+00 +04 +08 +14 +1C +28 +30 +34 +38` | relocated pointers | `movea.l`/`move.l`/`cmpa.l` (tables the main engine walks) |
| `+0C +10 +18 +20 +24 +2C` | `bra.w` entry stubs | `jsr $4A4E`, `$4A52`, `$4A5A`, `$4A62`, `$4A66`, `$4A6E` (and one `jmp`) |
| `+3C ...` | parameter words | `move.w` (level tuning values, identical to the disk module's) |

This is the disk build's level-module header (data longs that execute as harmless `ori.b`, then `bra.w` stubs from `+$0C`), with the
same parameter words. The level overlays contain **no** custom-register access, no CIA access, no OS call and 2 indirect
`jsr/jmp` sites in the entry code, like the disk modules; they reach the engine and hardware only through the API table.

**Relation to the disk modules (`level1..5_54e00`):** 95-96 % of the non-zero bytes of each overlay occur in 16-byte exact runs of the
disk module of the same level, and the reverse is 95-96 %. Overlay text is 88 / 162 / 100 / 350 / 116 bytes longer than the module.
The differing bytes are the relocated pointer fields (about 3.5 % of the text) and a block of about 366 changed or added bytes in the
first ~6.5 KB of the module (`LEVEL1`: the common body starts at module offset `$19BA`, overlay offset `$1B28`, a constant shift of 366), which
holds the CDTV differences (music is the CD player, not the tracker). The pointer and `bra.w` values in the header differ because of that
shift. `INTRO`, `PICCODE` and `PICDATAn` are text, strings and pictures with little code; the high-entropy `PICDATA1/3/5/7` (7.0 bits/byte) may be
packed or dithered images (not confirmed).

## 7. Self-modifying code, dispatch and compression

- **Self-modifying code:** none found. The six "code write" candidates in the main hunk are data inside the code hunk (the
  score-table text at `$3BF62-$3BFFC`, a three-word state at `$40338-$4033C`) and the digit patched into the `CD0:LEVELn` name
  strings (`$3CE5D`, `$3C325`, `$4F9B2`). Overlays were not scanned for self-modification beyond the decoded entry code.
- **Dispatch:** besides the 42 `jsr -N(a6)` OS calls in the listing (12 exec, 30 dos) there are six indirect sites in the main code: `jmp (a2)` at `$357AE`
  (the animation/script stream interpreter, the same idiom as the disk build's `$2C6C`), `jsr (a1)` at `$36338`/`$3634C`, `jmp (a0)` at
  `$3AAD4`, `jsr (a1)` at `$3EA00`, `jsr (a0)` at `$3F41A`; the level overlays add data-driven `jmp (an)` through object handlers.
- **Compression:** none in `X2` (a plain hunk file, no cruncher stub, no decompression routine matched). Samples are raw 8-bit PCM
  (entropy 6.4-6.9 bits/byte). Overlays compress with zlib to 43-56 % (code) so they are not packed.

## 8. Input

`JOY1DAT` plus the fire bit of CIA-A port A (bit 7) are decoded each frame into the same joystick byte as the disk build
(latched into `$606/$607`, the shifted `$A00/$A01`). The second button is `POTGOR` bit 14 of port 2, sampled about every 20 ticks
after `POTGO` is set at startup; the exact port and pin mapping of those bits was not verified. That is how the joypad button and the "select joypad mode" screen work. There is
**no keyboard path and no remote/IR register access in `X2`.** The CDTV remote and the "high-score entry with the remote"
text exist, but whatever delivers remote data runs outside `X2` (`RMTM`, started before it); how remote input reaches the game (as joystick
bits, or otherwise) is not established.

## 9. Language, music and speech (pointer to existing notes)

Language selection, music cue tables and the speech narrator are in `cdtv.md` and `cdtv-music-map.md`; this analysis confirms they sit in
the new `$3C000-$42000` and `$4E000-$50000` regions, which have only 8 % overlap with the disk build.

## 10. What the current approach already covers and what must be new

Already applicable (the runtime and recompiler were built for the disk build and the shared engine is the same code):

- Musashi interpretation of the 68000 code, the OCS machine model (copper, blitter, display, Paula effect playback, level-3 interrupt,
  joystick/fire decode), the frame hashing and replay framework, and the `--protect-music` and cheat hooks (their addresses shift).
- The recompiler's model of a resident image plus overlays keyed by (module, address) once the load address is fixed
  (`tools/recomp/recomp.py`: `MODULE_BASE`, per-module tables; here one resident image and seven overlays instead of one and six).
- Modern-mode capture, widescreen and asset-pack machinery, as far as they depend on the same blitter and object system.

New work:

1. **Loader and relocator:** read `X2` as a hunk file (both hunks chip), choose fixed load addresses for hunk 0 and the BSS, apply the 4,452
   relocations, honour the fill that overruns the BSS, then run from the entry point.
2. **A 1 MB chip RAM machine** (`kChipRamSize`, `kChipMask`, the trap-index and instruction-length tables, DMA pointer widths) with the
   CDTV's PAL timing (VBlankFrequency 50).
3. **OS HLE** for the vectors in section 2.1 and the level-3 chain; a `CD0:` virtual file system over the user's own disc image (the importer already
   reads the ISO volume for the CD audio); a cdtv.device model for track play, stop, volume and loop (the existing CD player already plays the tracks).
4. **Overlay-aware import and recompilation:** the overlay files as importable assets, relocated at load, with translated code keyed per overlay;
   the API table at `$A12` as the recompiler's entry list; per-overlay tracing and replays (the disk replays do not apply: addresses, loads and the menu
   flow differ), plus new goldens.
5. **Input and menus:** a joypad mapping for the CDTV front end (the second button, the language and music-select screens), and a decision about the remote.
6. A new importer variant (`cdtv`) writing the hunk, the overlays and sound/speech banks next to the existing CDTV CD-audio import.

## 11. Unknowns and risks

- **Interrupt model of the OS stand-in** (what exec's original level-3 handler does that the game depends on: clearing `VERTB`, running
  the other VBlank servers) is inferred from the missing `INTREQ` write, not tested.
- **cdtv.device completion semantics** (when `CheckIO` becomes true, whether the game ever relies on `WaitIO` blocking, command numbers) are inferred.
- **Overlay code was decoded only from its header stubs** (31-300 instructions each); full coverage needs traces, as for the disk modules.
  The recompiler's trace workflow for the CDTV build is not designed.
- **The RNG location** and any other per-build constants (the disk build's `$1F50` has no counterpart reference).
- **Paula effect player:** its interrupt use (the audio interrupt bits it arms) and exact record format are only read, not verified by running.
- **High-entropy `PICDATA1/3/5/7`** and the format of the `SOUNDS`/`SPEECH` headers beyond the 12-byte records seen.
- **CDTV hardware beyond `X2`:** the remote controller, the front panel, battery-backed memory and bookmarks are not used by `X2` itself; they matter only if
  the importer must reproduce `RMTM`/`CDTVPrefs` behaviour.
- **Does the CDTV Agnus address 1 MB?** The code uses only OCS registers, but its buffers lie above 512 KB; the model needs 20-bit DMA pointers
  (8372A-class behaviour) even though no ECS register is used.
