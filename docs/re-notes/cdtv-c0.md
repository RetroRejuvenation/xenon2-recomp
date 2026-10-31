# CDTV C0: the unknowns, settled

Phase C0 of `.omc/plans/xenon2-cdtv-target-plan.md`: an evidence study of the CDTV program `X2` and its overlays, done before any
implementation. It extends `cdtv-code.md`, `cdtv-slave.md` and `cdtv-runtime-gap.md`; read those first. No game bytes and no
disassembly listings are reproduced: addresses, counts and short instruction-level facts only. Inputs are the files named in
`cdtv-code.md` section 1 (same sha1s).

Conventions. **Hunk addresses** are offsets into the `X2` code hunk (hunk 0). If the importer pre-links hunk 0 at `$400` (the
slave's choice, and the plan's), **absolute address = hunk address + `$400`**, and hunk 1 (BSS) sits at `$6B09C`. Both tables below give
hunk addresses; the "abs" column adds `$400`. The disk build's addresses are the canonical image's.

## 0. Verdict: go for architecture 3

Everything the program needs from the OS is small and local, and every question C0 set out to answer has an answer that fits
the pre-link, bare-metal, trap-at-the-call-site design:

- the five level overlays are the disk build's level modules, recompiled with long absolute addressing and the engine variables
  moved by `-$3FA` (section 1); they touch no hardware and no OS, and contain no self-modifying code;
- the OS start-up block is one contiguous range that a trap can skip (section 2);
- the CD-audio calls are five routines with a precisely understood contract (sections 2.3 and 2.4);
- every disk-build address the runtime's hooks use has a verified CDTV equivalent (section 3);
- input is joystick plus one second button; the remote does not reach `X2` (section 4);
- speech and sound effects share one Paula effect player, polled from the vertical-blank handler (section 5);
- the 6.9 MB region after the ISO volume is not referenced by anything the game can do (section 6).

Residual risks are in section 7. None needs a different architecture.

## 1. Overlays

### 1.1 Corrections to `cdtv-code.md`

| Statement there | Correct |
|---|---|
| Relocation counts `LEVEL1..5` = 681 / 899 / 975 / 1,042 / 1,109, `INTRO` 364, `PICCODE` 13 | **682 / 900 / 976 / 1,043 / 1,110, `INTRO` 365, `PICCODE` 14.** The first fix-up is at text offset 0 (header slot `+00` is a relocated pointer); the first long of the table is that offset, not a "no relocations" marker. The earlier counts left it out. |
| "about 366 changed or added bytes in the first ~6.5 KB" | The growth is **not** new code. It is an accumulating offset caused by the code being re-assembled with long absolute addressing (section 1.2). The offset reaches 366 / 432 / 510 / 510 / 592 bytes (`LEVEL1..5`) by the end of the changed region, text offset `$1B28` / `$25F6` / `$24BE` / `$2784` / `$2AB4`. |
| RNG is `$1B56` and "not referenced" | The disk build's `$1F50` is the RNG **routine**, not a seed variable. The CDTV routine is at `$34A7A` (section 3.2). |

### 1.2 What differs from the disk build's level modules

The unmatched 4-5 % of each overlay is explained by one mechanical change. In the disk modules the engine variables, the API table
and nearby routines are addressed with **short absolute** operands (`move.w $0cea.w,d0`, `jsr $0e36.w`). Short absolute addresses cannot be
relocated, so the CDTV overlays use **long absolute** (`move.w $08f0.l,d0`, `jsr $0a3c.l`), and the engine variables sit at
(disk address `-$3FA`): `$0CEA -> $08F0`, `$0E36 -> $0A3C`. Each conversion lengthens the instruction by 2 bytes, which is why the
overlay-to-module offset grows by 2 at a time, in steps of 2-48 bytes between runs of identical code (51 / 73 / 73 / 68 / 71 shift
changes, `LEVEL1..5`). After the code region the bodies (tile maps, object tables, the bulk of each overlay) match at a constant offset.

Measured by greedy 16-byte-run alignment against `level{1..5}_54e00`: matched 95.8 / 94.6 / 95.2 / 95.8 / 95.2 %; of the
unmatched bytes, the relocated pointer fields are excluded and 2.1 / 3.4 / 2.9 / 3.1 / 3.2 KB remain, all inside the first
`$1B28`-`$2AB4` bytes of text (the code, which is also where the disk build keeps it). **Beyond text offset `$3000` there is no non-relocation
difference at all in any of the five overlays**: the maps, tables and object data are byte-identical to the disk modules except for the relocated pointer fields. Spot checks (`LEVEL1`) show the same instruction sequences
with `abs.w -> abs.l`, adjusted branch displacements, and the variable shift; nothing resembling new music or CD logic.
The behaviour of the level code is therefore the disk build's, with one qualification: the overlays are 95-96 % **matched**, not 100 %, and a
full behavioural-equivalence proof needs the replays (phase C6), not this static comparison.

### 1.3 No hardware, no OS, no self-modifying code

Recursive-descent decode from the six header stubs plus every relocated data pointer that targets plausible code (33-62 seeds per
level overlay) reaches 962 / 1,309 / 1,489 / 1,568 / 1,870 instructions (3.6-5.0 % of the text bytes; the rest is map and table
data, as in the disk modules). Over those instructions:

| Check | `LEVEL1..5` | `INTRO` | `PICCODE` |
|---|---|---|---|
| operands that are custom registers, CIA registers or low absolute addresses | none | none | none |
| unrelocated absolute operands | none (2 stray hits in `LEVEL4` are mis-decoded table data) | none | none |
| `jsr -N(a6)` / `trap` / `stop` / `reset` | none | none | none |
| writes into the overlay's own instruction bytes | none (two stores in `LEVEL5` at text `$1FD6C` go to a data table slot, not to instructions) | none | none |
| relocated operands | engine variables, the API table `$A12-$B7E`, the overlay's own area | same | same |

A raw byte scan for hardware-address-shaped longs finds the same counts in the disk modules (they are map data), so it proves nothing; the decoded-operand check is
the evidence. **Coverage caveat:** 3.6-5 % of the bytes is decoded code; code reachable only through computed jumps could have been missed.
The recompiler's trace workflow (phase C7) closes this.

### 1.4 Entry stubs, and what main calls

Header (60 bytes at hunk `$4A42`): relocated data pointers at `+00 +04 +08 +14 +1C +28 +30 +34 +38`, `bra.w` stubs at
`+0C +10 +18 +20 +24 +2C`, parameter words from `+3C`. The six stubs are called by `X2` through fixed hunk addresses:

| Stub (hunk) | abs | Main call sites (hunk) |
|---|---|---|
| `$4A4E` | `$4E4E` | `$35CAE`, `$35D1E`, `$35D82` |
| `$4A52` | `$4E52` | `$3621C` |
| `$4A5A` | `$4E5A` | `$36264` |
| `$4A62` | `$4E62` | `$3A32A` |
| `$4A66` | `$4E66` | `$333CC`, `$3AA90` |
| `$4A6E` | `$4E6E` | `$3A24C` |

(`LEVEL1` stubs branch to `$6440`, `$583A`, `$55D8`, `$6444`, `$5478`, `$4AA6` respectively; the last is a bare `rts`.) `INTRO` and
`PICCODE` have the same header layout; their code is a few hundred bytes and 2 bytes respectively (text, strings and pictures).
The overlay relocator is `X2`'s own routine at `$3CEEE`; nothing in the importer or runtime needs to relocate overlays if the loader trap
simply reads the file into the buffer, because the game relocates after every `Read` (`cdtv-code.md` section 6). Absolute addresses in the overlays
are all in the relocation table, so they move with whatever the game's relocator uses.

### 1.5 API table: 49 of 56 slots identical

The 56-slot `jmp abs.l` table (`$A12`) against the disk build's (`$E0C`): comparing the first up to 40 instructions of each target after
normalising operand widths and addresses, **49 slots have identical instruction shape**. The seven that differ:

| Slot | Role | Difference |
|---|---|---|
| 15, 42, 44 | blitter set-up | same instruction sequence; the differences are the call targets (absolute subroutine addresses) |
| 17 | object-list insert | same sequence; the list-head variable is `$7B4` (disk `$BAE`, the same `-$3FA` shift) |
| 28 | damage routine (`hit`) | two **extra guards** in front: `tst.w $38E5E` (a flag stored in the code hunk) and `tst.b $33940` (a state byte that the level-3 handler also tests to run a different per-frame routine and skip the effect player; **inferred** to mean front end / attract mode); then the same energy test, calling the death routine at `$38DB2` |
| 30 | sound-effect trigger | disk writes command words `#$83/#$85` to the music driver variable `$418`; CDTV writes `#$07/#$13` into effect-player command slots `$1C/$1E` (section 5) |
| 45 | random-number routine | same algorithm and state size; calls a different body (`$34A7A`) |

So the engine's behaviour is the same; the **sound command interface differs by construction** (effect player instead of the tracker driver),
which is why `--protect-music` (which mirrors tracker voices) does not apply to the CDTV build.

## 2. Start-up, interrupts and CD audio

### 2.1 The OS block to skip

Entry is hunk `$3A612` (reached from the `jmp` at hunk 0). Order of events:

| Hunk range | abs | What |
|---|---|---|
| `$3A612-$3A649` | `$3AA12-` | save SP at `$3394E` (**never read again: the program has no exit path**); stack = BSS+`$19F00`; clear `$766B` longs of BSS from offset 0; fill `$F505` longs from BSS+`$1D9AC` with `$FFFFFFFF` (**ends `$100` bytes past the BSS hunk**) |
| **`$3A64A-$3A6E7`** | **`$3AA4A-$3AAE7`** | **the OS block**: `OpenLibrary("dos.library")` stored at `$3A44E`; `AllocSignal`; `FindTask`; builds a message port at `$3A4D2` and two `IORequest`s (`$3A45E`, `$3A48E`, both pointing at the port `$3A4BE`) by hand; `OpenDevice("cdtv.device")` twice. This range is **safe to skip** if the traps below replace every user of those structures |
| `$3A6E8-$3A74F` | `$3AAE8-` | `INTENA = $4000` (master off), palette routine `$3A3CC`, buffer pointers (`$8` = BSS+0, `$C` = BSS+`$CA80`), effect-slot sentinels `$16`, `$1C` = `$FFFFFFFF`, **PAL word**: `$12.w = 3` if `ExecBase+$212 == 50`, else 4; saves the OS level-3 vector (`$6C`) into `$3A452` and `$3A456` |
| `$3A754-$3A8F0` | `$3AB54-` | installs its own level-3 handler `$34BB2` in `$6C`; deselects drives (`CIAB PRB`); display (`BPLCON0 = $4200`, DIW/DDF), `POTGO = $0C00`; DMA `$87C0`; sprites cleared; two copper lists (bitplane pointers only: **no copper interrupt**); `INTREQ = $3FFF`; `INTENA`: clear audio bits `$780`, then set master `$C000`; row-offset table; then `bsr $3A5B4` (stop CD), `bsr $3C350` (front end) |

`INTENA` is **not** written as a whole: only the audio-interrupt enables are cleared and the master bit set, so every other enable bit keeps the value the OS
left. The game needs `VERTB` (bit 5) enabled. The runtime's boot must therefore set `INTENA` to a state with `VERTB` on before the program
reaches `$3A6E8`; `COPER` and `BLIT` stay off (neither is requested: the copper lists never write `INTREQ`, and the game polls the blitter).

What the skip must also provide, because later code reads it: `$412.w = 3` (PAL word, written by the program itself in the third row above, so **no fake
`ExecBase` is needed if the range `$3A730-$3A73A`, the `cmpi.b #$32,$212(a6)`, is also trapped to take the PAL branch**); `$4.w` need not be valid
once the CD traps and the file traps replace every library call; the saved OS level-3 vector must point at a stand-in (section 2.2).

### 2.2 Level-3 interrupt and the OS chain

The game's handler `$34BB2` tests `INTREQR` bit 5 (`VERTB`). If clear it chains at once. If set it increments the frame counter (`$22`), decodes
the joystick (section 4), runs the front-end colour-cycling and effect-player hooks, calls the effect player `$330B6` (section 5) and the CD tick `$3A55C`
(section 2.3), and then **chains to the saved OS vector** (`move.l $3A456,-(a7); rts`). It never writes `INTREQ` for `VERTB` itself.
In the original the OS handler clears the interrupt request and executes `rte`; so the stand-in must: **clear `INTREQ` bits `$0070` (COPER, VERTB, BLIT)
and `rte`**. The program already contains exactly that routine, the 8-byte `$34CC4` (`move.w #$70,$dff09c; rte`), used elsewhere as a bare handler; the importer or
boot can store its absolute address (`$350C4`) in `$6C` before the program runs, so the program's own `move.l $6C,$3A452` saves it. During file loads the game
swaps `$6C` to a bare handler (`$34B7C`, or `$34B66` in the speech path) that runs the vertical-blank work but still chains the same way, then restores `$34BB2`.
The game runs in user mode in the original (no `Supervisor`); under Musashi it can run in supervisor mode with `A7` as its own stack, because nothing reads `SR`, and the
interrupt frame is pushed on the same stack.

### 2.3 CD-audio control routines (the trap sites)

Five small routines drive the whole CD interface; all run in hunk 0. Each is a trap site that replaces the exec calls with the runtime's existing `cd_player`:

| Hunk | abs | Routine | Original behaviour | Trap emulates |
|---|---|---|---|---|
| `$3A4F6` | `$3A8F6` | stop | if the "current track" variable `$3A4F2` is `>= 0`: `AbortIO` then `WaitIO` on the play request, set `$3A4F2 = -1` | stop the CD player, set the variable to `-1` |
| `$3A524` | `$3A924` | play track `d0.w` | stop (above), store the track in `$3A4F2`, fill the request (`io_Command = 43`, `io_Offset` = track, `io_Length` = track + 1, `io_Data` = 0), `SendIO` | start the track on the CD player (non-blocking) |
| `$3A55C` | `$3A95C` | tick (called from the level-3 handler every VBL) | if track `>= 0`: decrement the counter `$3A4F0` from 50 (once per second at 50 Hz); on zero reload it and `CheckIO` the play request; **if the request has completed, re-issue the same track** | if the player reports the track finished, restart it (loop) |
| `$3A5BA` | `$3A9BA` | volume full | `DoIO` of command 56 with `io_Offset = $7FFF`, `io_Length = 2` | set volume to full |
| `$3A5E6` | `$3A9E6` | volume mute | the same with `io_Offset = 0` | set volume to zero |
| `$3A5B4` | `$3A9B4` | `bsr $3A4F6; rts` | wrapper for stop | (covered by the stop trap) |

The traps can sit at the routine entries (one `rts`-style replacement per routine), which also removes every `SendIO`/`CheckIO`/`WaitIO`/`AbortIO`/`DoIO` call. The
program calls them at: stop from `$3A5B4` (itself called at start-up, `$3A89E`); play with `d0` = track at the game-start path (`$3A966`, level 1) and, by the same pattern, at level changes and the music-select screen;
volume full from the game-start path (`$3A96A`). Other call sites were not enumerated: the traps sit at the routine entries, so they are not needed.

### 2.4 What `cdtv.device` commands 43 and 56 are

Resolved from the device driver on the disc (`CDTV.TM`, `cdtv.device 35.2 (6.2.91)`, linked for the extended ROM at `$F04800`; Romtag at offset `$98`; `BeginIO`
at `$F04E84` validates the command (1..60) against a flag table and dispatches through the library vector table at index command + 7):

| Command | Vector | Meaning |
|---|---|---|
| **43** (`$2B`) | `$F0521E` | **play audio tracks**. Builds the drive's play command `$0B`: start track = low byte of `io_Offset` (start index = the high word, default 1), end track = `io_Length` (zero means start + 1; capped at 99). The request stays in progress while the audio plays and **completes when the drive reports the end of the play range** (or on `AbortIO`) |
| **56** (`$38`) | `$F054C6` | **set attenuation (volume)**: if `io_Length` is non-zero, stores `io_Offset` as the new level (full = `$7FFF`) and returns the previous level in `io_Actual`; applies it to the drive's volume |

So `io_Offset` = track and `io_Length` = track + 1 means "play track N to the start of N+1", and `CheckIO` becoming true means "the track played to its end";
that is the whole loop mechanism, and it matches the `cdtv-music-map.md` cue tables (tracks 2..13). The existing `cd_player` already has start, stop, loop and volume.

## 3. Variables and hook points

All variable addresses are verified by code equivalence (identical instruction sequences around each use, and reference counts that match). Hunk 0 holds the engine variables
(zero-initialised front of the code hunk, relocated into hunk 0); hunk 1 holds buffers. **The block shifts by exactly `-$3FA`** from the disk build; the first
`$3FA` bytes of the disk image are the disk build's vectors and loader variables, which have no counterpart.

### 3.1 Variables

| Role | Disk | CDTV hunk | CDTV abs (base `$400`) | Evidence |
|---|---|---|---|---|
| frame counter (increments each VBL) | `$CDC` | `$8E2` | `$CE2` | main loop `addq.w #1` in both |
| level number | `$E04` | `$A0A` | `$E0A` | compared with 5 at the ending test in both; 28 CDTV refs vs 11 disk |
| loop counter (game cycles) | `$E02` | `$A08` | `$E08` | set to 1 at game start, `addq` once at the end, 2 refs each |
| level-complete flag | `$A07` | `$60D` | `$A0D` | `sf` at start, `st` on completion, `tst` in the loop |
| shop request | `$A6C` | `$672` | `$A72` | `st`/`sf` pattern, 6 refs each |
| energy | `$CC6` | `$8CC` | `$CCC` | `sub.w d0` then death call; `$27` refilled on respawn (`$3AD20`) |
| ships | `$CCA` | `$8D0` | `$CD0` | `subq` at respawn, `tst` for game over, 7 vs 5 refs |
| credits | `$CF0` | `$8F6` | `$CF6` | reset to 3 (`$39EAC`, `$3C648`), `subq` at continue (`$3AD4E`), `addq` at `$3AF08`, displayed at `$3CA04` |
| score | `$C92` | `$898` | `$C98` | `add.l d0` at the same two scoring sites, 8 refs each |
| push-back counter (crush at `<= -$11`) | `$42E` | `$34` | `$434` | `cmpi.w #$ffef` at `$3913E` |
| guard word `$CA4` (non-zero blocks damage and death; meaning not established) | `$CA4` | `$8AA` | `$CAA` | first test in the damage and death routines |
| death-state byte | `$D84` | `$98A` | `$D8A` | `st` at the start of the death routine |
| PAL/NTSC logic divisor | `$410` | `$12` | `$412` | written by the OS block; 3 = every 3rd frame (PAL) |

New in the CDTV build (no disk counterpart): the front-end/attract state byte `$33940` (read by the damage guard and the level-3 handler), the CD track variable `$3A4F2` and
counter `$3A4F0`, the effect-player command slots `$16` and `$1C` (hunk), and the ending flag below.

**Ending flag.** The disk build's `$8FEC` lives in the image's code area; the CDTV equivalent is the byte at **hunk `$3CE4E`** (abs `$3D24E`): cleared (`sf`) at `$3AE3E`, set (`st`) at `$3AE58` when
the level is 5 and the loop counter and ship counter allow it (the same test as the disk build's `$7BF0-$7C00`).

### 3.2 Routines and the runtime's hooks

| Runtime hook (`BuildInfo` field or use) | Disk | CDTV hunk | CDTV abs | Evidence |
|---|---|---|---|---|
| level-3 (vertical blank) handler (`vbl_pc`) | `$20C2` | **`$34BB2`** | `$34FB2` | installed in `$6C` at start-up; tests `VERTB` first |
| main loop back-edge (`main_loop_pc`) | `$78CE` | **`$3AA38`** | `$3AE38` | same setup instructions precede it (`clr.b` / `sf.b` of the two input latches); a second copy of the same loop body at `$33374` is the attract/front-end variant |
| death routine (`death_pc`) | `$5D00` | **`$38DB2`** | `$391B2` | first instructions equal (`tst.w` guard, `st`, `clr.w energy`); called by the damage routine when energy reaches 0 |
| push-back crush site | `$5FE0` | `$3914A` | `$3954A` | `cmpi.w #$ffef`, `subq.w #1` on the push-back counter |
| respawn refill (`$27` to energy) | `$7AF6` | `$3AD20` | `$3B120` | `move.w #$27,$8CC` followed by the ship decrement |
| random-number routine | `$1F50` | `$34A7A` | `$34E7A` | identical algorithm; the 64-bit state is two longs at hunk **`$34AAE` / `$34AB2`** (disk `$1FEE` / `$1FF2`), initial values `$63141592` / `$35358979` in both; they are in the code hunk, not cleared by start-up |
| high-score name entry end (`hiscore_saved_pc`) | `$8B1A` | `$3C14C` | `$3C54C` | same shape; the table code is `$3BF38` (disk `$8918`) |
| options menu | `$8CB6` | `$3C648` | `$3CA48` | resets ships and credits to 3 |
| restart vector (`restart_pc`) | `$7032` | none | none | `X2` never returns or restarts; **reset is a host operation** (re-run the pre-link entry) |

The `--protect-music` mirror voices read the tracker driver's shadow registers; the CDTV build has none (music is the CD), so the option is not applicable there. The high-score table location in
`X2` is data at about `$3BF62-$3BFFC` (the "score-table text"); the original keeps no table across runs, so persistence is the runtime's own (decision D4).
The replay harness's coverage pokes (`tools/trace/make_replays.py`, addresses like level and boss HP) need the same remap, with the `-$3FA` rule for variables and a per-handler search for object handlers.

## 4. Input

- **Joystick:** `JOY1DAT` decoded in the level-3 handler (`$34BCE-$34C26`) into the bit layout the disk build uses, latched into `$606`/`$607`; fire is CIA-A `PRA` bit 7 (`$BFE001`).
- **Second button:** `POTGOR` bit 14 (the high byte's bit 6 at `$DFF016`), sampled once every 20 VBLs (`$33943`), after `POTGO = $0C00` at start-up. A press stores **`$40`** in the command byte `$605`.
- **Command byte `$605`:** consumed by the main loop at `$3AAA6` and by several menus (`cmpi.b #$40` at `$3DFEC`, `$3E804`, `$4F6D4`) as a "key": a 4-byte-entry dispatch table at `$3AAD6` matches codes `$72`, `$73`, `$45`, `$40`. **Only `$40` is ever stored in the CDTV build** (there is no keyboard interrupt); the other three entries are dead code inherited from the disk build.
- **No keyboard path, no serial or CIA-timer register, no CDTV register** (`$DC0000`, `$E90000`, `$DE0000`) is touched. **The CDTV remote does not reach `X2`**: whatever turns remote buttons into input runs outside it (`RMTM`, started from the startup sequence); on the real machine the joystick port 2 lines and the second button are driven by the controller hardware, which is consistent with what `X2` reads, but this was not verified here.
- **Consequence for the runtime:** map pad/keyboard to the same joystick bits and fire, add a second-button source for `POTGOR` bit 14 (the runtime's `POTGOR` is a constant today), and treat the CDTV remote as out of scope (a "remote" is just the pad). Pause/quit/reset are host functions: the program has no exit path (the saved stack pointer is written once and never read).

## 5. Speech, sound banks and the Paula effect player

- **One player for effects and speech.** The effect player `$330B6-$3321E` is **polled from the level-3 handler** (`jsr $330B6` once per vertical blank) and is not interrupt-driven: the game clears the audio interrupt-enable bits at start-up (`INTENA` audio bits `$780`), and the player writes `INTREQ` itself to clear the per-channel audio request bits. It owns channels 0/1 (command slot `$16`) and 2/3 (slot `$1C`), uses `DMACON` set/clear for each pair, and reloads the period/length/volume registers directly (`a4 = $DFF000`). It waits with `dbra` loops (`$C7` and `$63` iterations) between starting the DMA and writing the loop pointers: a **timing dependence on Paula's DMA latch that the runtime's accurate-timing mode already models** for the disk build's own audio code, and which should be checked once in C4.
- **Banks.** `SOUNDSnn` (10 banks, index = level, `$A0A`) loads to BSS+`$1D9AC` (`$2DA14` bytes); the effect records are 12 bytes (data offset, length, period, volume, loop/one-shot), looked up by the command slot's index; an offset of `$FFFFFFFF` marks an empty slot.
- **Speech.** `SPEECHnn` loads to the speech buffer (pointer `$40DE4`, normally BSS+`$4B3C0`, `$1A518` bytes). The narrator `$40DEC` does not use any extra hardware: it **writes a sample record into the same effect table** (data offset = speech buffer minus the bank base, period from the table at `$40C2C`) and triggers the effect player through command slot `$16` (index 1, pan `$12C`). Two special indices (`$33`, `$18`) additionally patch in extra sample data from fixed addresses (`$547DF`, `$503E3`, inside the code hunk). It also does text word-wrapping for the subtitle display. No non-Paula and no non-vertical-blank dependence.
- **Front-end effects.** `$500E8` and `$50F90` (run from the bare handler during loads) are colour-cycling effects that write copper-list colour words and `COLOR16-30`; CPU register writes only.
- **Music interplay.** Music is CD audio only: the play trap is called with the cue-table track for the level (`$3A950-$3A966`: level `$A0A`, byte table `$3EE27` indexed by level-1, track in `d0`), and the music-select screen can override it (cue tables in `cdtv-music-map.md`).

## 6. The 6.9 MB region after the ISO volume

`X2` opens only files **by name** (the strings `CD0:INTRO`, `CD0:LEVEL1`, `CD0:SOUNDS00`, `CD0:SPEECH00` plus the digits it patches in, and the picture files) and never issues a raw CD read:
its only `cdtv.device` commands are 43 and 56 (section 2.4). The overlays contain no OS calls at all. A scan of `X2` and the seven overlays for 2048-aligned values inside the region's byte range finds
only constants such as `$640000` and `$900000` that are screen and table values (448 hits in `X2`, hundreds in the overlays, none with a plausible sector context). Every file on the volume is accounted for by a
load site or by the start-up sequence (`CDTV.TM`, `CDTVPREFS`, `RMTM`, `DEVS/SYSTEM-CONFIGURATION`, `S/STARTUP-SEQUENCE`). **Recommendation: ignore the region.** (What it actually is - the leftover of the
mastering process or an unused sample set - is not determined; it does not matter for the target.)

## 7. The trap list, and go/no-go

### 7.1 Importer and boot (one-time, at pre-link)

| Item | Hunk | abs | Action |
|---|---|---|---|
| load hunk 0 at `$400`, hunk 1 at `$6B09C` | | | relocate all 4,452 longs (4,367 into hunk 0, 85 into hunk 1) by the chosen bases; allocate `hunk1 + $5ACC0 + $100` bytes (the start-up fill overruns) |
| set `$6C` to the stand-in | | `$6C` | store the absolute address of the program's own `$34CC4` routine (`$350C4`) before entry |
| set `INTENA` | | | `VERTB` enabled (the program sets the master bit itself); enable bits for `COPER`/`BLIT` stay off |
| entry | `$3A612` | `$3AA12` | start the CPU here with a valid stack (the program loads its own) |

### 7.2 Runtime PC traps

| Trap site | Hunk | abs | Emulate |
|---|---|---|---|
| OS block skip | `$3A64A-$3A6E7` | `$3AA4A-$3AAE7` | continue at `$3A6E8` (abs `$3AAE8`); nothing in the range's outputs is read once the CD and file traps exist |
| PAL branch | `$3A730` (`cmpi.b #$32,$212(a6)`) | `$3AB30` | take the equal branch, so `$12.w = 3` (or write `3` to `$412` and skip the three-instruction block) |
| CD stop | `$3A4F6` | `$3A8F6` | stop player, `$3A4F2 = -1`, `rts` |
| CD play (`d0.w` = track) | `$3A524` | `$3A924` | start track; store it in `$3A4F2`; `rts` |
| CD tick | `$3A55C` | `$3A95C` | if a track is current and the player reports it finished, restart it; `rts` |
| CD volume full / mute | `$3A5BA` / `$3A5E6` | `$3A9BA` / `$3A9E6` | set volume; `rts` |
| file loads (10 sites) | see below | | in place of `Open`, `Read`, `Close` |

The file-load sites (each is `Open` then `Read` then `Close`; the name pointer is in `d1` at the `Open` call, the buffer in `d2` and the maximum length in `d3` at the `Read` call; a short read is normal; resume after the `Close`):

| `Open` | `Read` | `Close` | resume | Loads (buffer, max bytes) |
|---|---|---|---|---|
| `$3353C` | `$33554` | `$3355A` | `$3355E` | overlay (`$4A26`, `$2BF20`) |
| `$3CEB0` | `$3CEC8` | `$3CECE` | `$3CED2` | overlay (`$4A26`, `$2BF20`) |
| `$3D47A` | `$3D492` | `$3D498` | `$3D49C` | overlay (`$4A26`, `$2BF20`) |
| `$3DCA6` | `$3DCBE` | `$3DCC4` | `$3DCC8` | overlay (`$4A26`, `$2BF20`) |
| `$50BDA` | `$50BF2` | `$50BF8` | `$50BFC` | `PICCODE` (`$4A26`, `$2BF20`) |
| `$50B74` | `$50B8C` | `$50B92` | `$50B96` | `PICDATAn` (pointer `$4A42`, `$1FBD0`) |
| `$3C3EA` | `$3C402` | `$3C408` | `$3C40C` | `SPEECHnn` (pointer `$40DE4`, `$1A518`) |
| `$40DB4` | `$40DCC` | `$40DD2` | `$40DD6` | `SPEECHnn` (pointer `$40DE4`, `$1A518`) |
| `$4F934` | `$4F94C` | `$4F952` | `$4F956` | `SOUNDSnn` (BSS+`$1D9AC`, `$2DA14`) |
| `$4F984` | `$4F99C` | `$4F9A2` | `$4F9A6` | `SOUNDSnn` (BSS+`$1D9AC`, `$2DA14`) |

(The absolute address of each is the hunk address plus `$400`.) The `Open` call is retried in a loop until it returns non-zero; the trap should return a non-zero handle for a file that exists and report a hard error otherwise rather than loop forever.

### 7.3 Residual risks

1. **Interrupt chain** is read, not run. The stand-in of section 2.2 follows from the code; a hang at the first VBL, or no frame counter increment, would be the symptom (C3).
2. **Completion model for the CD tick**: the trap decides "finished"; the original behaviour is that `CheckIO` is polled once a second, so a track that ends is restarted within about a second. The runtime's player should mimic a request that completes at the track end, not immediately (C4).
3. **Overlay code coverage**: 3.6-5 % of the text bytes decoded; the rest is data or reached indirectly; traces (C7) settle it. The replays (C6) cover behaviour.
4. **Effect-player DMA timing** (`dbra` delays) assumes Paula's DMA latch; check once (C4).
5. **Remote input** and the second button's port are inferred (section 4).
6. **The `$12.w` PAL word and the `$412` timing**: set to 3 for PAL; the NTSC value 4 is the program's own alternative, not needed.
7. The `INTENA` bits other than `VERTB` that the original leaves set are unknown; harmless unless some other interrupt source (CIA) is wired to it. (`X2` writes no CIA interrupt-control register, and CIA interrupts are level 2/6, not 3.)

### 7.4 Go/no-go

**Go for architecture 3.** No finding argues against pre-link at a fixed base and bare-metal execution with trap-site HLE: the OS use is a contiguous start-up block plus five CD routines plus ten file loads;
the engine is the disk build's with a variable shift and longer addressing; the overlays are clean; every runtime hook has an equivalent. The recompiler
phase (C7) is the largest piece of work and is independent of this verdict: the overlays load into one fixed buffer (`$4A26`), exactly like the disk build's `$54E00` slot.
