# CDTV release: what the WHDLoad slave tells us about the machine it needs

Purpose: evidence for a plan to add the CDTV release (`X2`, `docs/re-notes/cdtv.md`) as a second recompilation
target. A WHDLoad slave replaces the Amiga OS under a game and patches what still touches it, so the CDTV slave is the
best available statement of the **minimal OS and hardware environment** the CDTV program needs. Everything here is
derived from the slave (source and binary) and from a linear disassembly of the CDTV `X2` hunk made for this note
(`tools/dis68k.py`, relocations read with a few lines of Python). No game bytes are quoted.

## 1. Sources and licence

| Item | Identity | sha1 |
|---|---|---|
| WHDLoad install, CDTV | `Xenon2_v1.6_CDTV.zip` (Wepl, "Version 1.6 CDTV (11.05.2005)") | `49d5665f07a22a3fbcad019bb5e1dee0236240f0` |
| same install, other packaging | `Xenon 2 - Megablast (Bitmap Brothers)(2005)[pa Wepl][pv 1.6][CDTV][WHD].zip` | `143aeb0db65c81c195d80089eb842fd205ef4014` |
| CDTV slave binary, v1.6 | `Xenon2CDTV.Slave`, 1060 bytes | `4da8012166d2673564ec26525f41f7ae7ce4f7d8` |
| CDTV slave binary, v1.7 (JOTD, 2017) | `Xenon2CDTV.Slave` in `Xenon2-20180227.lha`, 1268 bytes | `a88a89ca24bdcb74f3530a9c50d0c0b962010bc7` |
| Slave and imager source | `Xenon2HD.s` (floppy and, with `CDTV` defined, CDTV slave), `xenon2.islave.s` (floppy imager only) in `Xenon2-20180227.lha` | n/a |
| CDTV program | `X2`, 455,288 bytes, the same file as on the disc (sha1 `f2d180efb1816e703a227f2ae50008569066dffd`) | |

Both zips hold the same 96 data files (4,822,829 bytes): `X2`, `INTRO`, `LEVEL1-5`, `PICCODE`, `PICDATA1-9,B,C` (11),
`SOUNDS00-09` (10), `SPEECH00-66` (67). The install script copies them from the disc with the pattern
`(intro|level?|piccode|picdata?|sounds??|speech??|x2)`, unmodified; the slave patches in memory only. The five disc files
that are *not* copied (`CDTV.TM`, `CDTVPREFS`, `RMTM`, `DEVS/SYSTEM-CONFIGURATION`, `S/STARTUP-SEQUENCE`) belong to the
CDTV boot environment, and the whole game runs without them.

Licence: the source headers state `:Copyright. Public Domain` (Wepl; JOTD for the 2017/2018 changes). The package ReadMe
carries only the usual no-warranty text. Read as a specification the source is safe to cite; copying its code
verbatim is also permitted by that statement, but nothing here needs it.

The v1.6 binary was decoded (patch table at slave offset `$162`) and its 17 patches match the CDTV block of the 2018
source one to one, so the 2018 source describes the 2005 behaviour for CDTV (the 2017 change was only "adapted to
WHDLoad v17").

## 2. Memory layout

| Item | Value |
|---|---|
| `ws_BaseMemSize` | `$C6000` = 811,008 bytes of chip RAM from address 0 (floppy slave: `$81000`). `ws_ExpMem` 0. The ReadMe's "1.0 MB chip + 0.5 MB other" is the install's generic text. |
| `X2` image | one load at `$400`, relocated in place to `$400` (`resload_Relocate`); 2 hunks, both flagged chip: CODE 437,404 bytes (`$6AC9C`) at `$400`..`$6B09B`, BSS 372,160 bytes (`$5ADC0`) at `$6B09C`..`$C5E5B`. 4,452 relocations: 4,367 to hunk 0, 85 to hunk 1. |
| Free slack | `$C5E5C`..`$C5FFF` = 420 bytes. The program fills the whole base memory. |
| Entry | hunk offset 0 is `jmp` to the real start (`$3AA12` after relocation): it saves SP, sets its own SSP to BSS+`$19F00` (`$84F9C`), then runs the OS setup (section 4) and takes over the machine at `$3AAE8`. |
| Overlay buffer | `$4E26` (in the code hunk), 180,000 bytes (`$2BF20`), reused for `LEVELn`, `INTRO` and `PICCODE`; `PICDATA` goes to the buffer whose pointer is at `$4E42`; speech to the 107,800-byte (`$1A518`) buffer whose pointer is at `$411E4`; `SOUNDSnn` to BSS+`$1D9AC` = `$88A48`, 186,900 bytes (`$2DA14`). |
| Replacement vector | the slave installs its own level-3 handler (acknowledges VERTB only) and enables VERTB before `jmp (a4)`; the game then installs its own at `$6C` (handler at absolute `$34FB2`). |

Hunk-relative operands in the unrelocated file may refer to **either** hunk (the sound buffer and the stack are BSS-based),
so any tool that maps `X2` must read its relocation tables rather than add `$400` blindly.

Compared with the runtime: chip RAM is `kChipRamSize = 0x80000` (512 KB) with `kChipMask` on every DMA pointer
(`runtime/machine.h`). The CDTV program needs at least 811,612 bytes (`$C5E5C`) of chip RAM, so a 1 MB chip model
with 20-bit DMA pointers (Agnus ECS style) is a prerequisite. The disk game's image is `$3FC00` bytes at `$400`.

## 3. Every patch the slave applies to `X2`

Addresses are the slave's, i.e. absolute (hunk offset + `$400`); `docs/re-notes/cdtv.md` uses hunk offsets. In a
disassembly of the unrelocated file the *operands* are hunk-relative (for example `jsr $3CEEE` is a call to absolute
`$3D2EE`) while the instruction addresses printed here are absolute. Types follow WHDLoad's patch list: P = replace the routine with a jump,
S = skip bytes, R = write `rts`, PS = call a hook before the original continues.

| Address | Type | Effect | Class |
|---|---|---|---|
| `$33930` | P | `LEVELn` loader | file loading |
| `$3D2A4` | P | `LEVELn` loader, second call site | file loading |
| `$3D86E`, `$3E09A` | P | `INTRO` loaders | file loading |
| `$3C7DE`, `$411A8` | P | `SPEECHnn` loaders | file loading |
| `$4FD28`, `$4FD78` | P | `SOUNDSnn` loaders | file loading |
| `$50F68` | P | `PICDATAn` loader | file loading |
| `$50FCE` | P | `PICCODE` loader | file loading |
| `$3A924` | R | `play_track`: starts a CD audio track (cdtv.device command `$2B`) | CD audio removed |
| `$3A9BA` | R | full volume (cdtv.device command `$38`) | CD audio removed |
| `$3AA4A` | S (to `$3AAE8`) | OS start-up: `OpenLibrary("dos.library")`, `AllocSignal(-1)`, `FindTask(0)`, message port setup, `OpenDevice("cdtv.device")` twice | OS removed |
| `$3AB2C` | S (to `$3AB40`) | reads `ExecBase.VBlankFrequency` (`$212`) and sets the PAL/NTSC word at image `+$12` (3 = PAL, 4 = NTSC) | OS removed |
| `$37A98` | P | `move.l a0,-(a7); jsr (a1); movea.l (a7)+,a0; rts`: the slave masks `a1` to 24 bits first | 32-bit fix |
| `$3A742` | S 12 | skips clearing the two cash variables on shop exit | feature (cash carry-over) |
| `$3C47E` | PS | after the `move.b #$3A,$C(a1)` it waits until fire has been released for 50 vertical-blank flips | feature (autofire highscore guard) |
| `$3C54A` | poke | `jmp` to the slave's save routine at the end of name entry (not when the trainer is on) | feature (highscore file) |
| `$3A296` | poke | replaces `move.w #3,ships` with `nop; jsr` to set ships and cash (3 and 0, or 9 and 100,000 with the trainer) | feature (trainer) |
| `$35096` | poke | operand of a `btst #6,$DFF016` read becomes `$BFE001`: with tooltype Custom2 the second button is read from the left mouse button | feature (input) |
| image `+$12` | poke | NTSC monitor: word set to 4 | PAL/NTSC |
| `$3C366-$28` | load | the `highs` file (190 bytes, XOR-obfuscated by its own length) is copied over the highscore table at start | feature |

Totals: 17 patch-list entries (10 file loaders, 2 CD-audio stubs, 2 OS skips, 1 cash skip, 1 pointer fix, 1 hook) plus
five direct pokes or loads (`$3C54A`, `$3A296`, `$35096`, the PAL/NTSC word, the `highs` file). Nothing is patched inside the overlays (`LEVELn`, `INTRO`, `PICCODE`, `PICDATA*`) and nothing in the sound or
speech drivers. One commented-out patch (`$410B2` -> `$34FB2`, "strange sound problem (does not help)") records a
sound problem the author never solved.

### 3.1 The file loaders

All ten routines are the same shape in the original: take the file name (the `SOUNDSnn` loaders format a two-digit number
into the name), `Open` (dos LVO -30, mode `$3ED` = old file), `Read` (-42) into a fixed buffer with a fixed
maximum size, `Close` (-36), then the code carries on at the instruction after `Close`. The slave replaces the call
with `LoadFileDecrunch(name+4, buffer)` and returns to the same continuation, so the following code is untouched:

File names below are as the slave labels each loader (`_load_level`, `_load_intro`, `_load_piccode`, ...); the disassembly
confirms the buffers and sizes, not which disc file a given call site names.

| Loader | File | Destination | Read size |
|---|---|---|---|
| `$33930`, `$3D2A4`, `$3D86E`, `$3E09A`, `$50FCE` | `LEVELn`, `INTRO`, `PICCODE` | `$4E26` | 180,000 |
| `$3C7DE`, `$411A8` | `SPEECHnn` | pointer at `$411E4` | 107,800 |
| `$4FD28`, `$4FD78` | `SOUNDSnn` | `$88A48` | 186,900 |
| `$50F68` | `PICDATAn` | pointer at `$4E42` | 130,000 (`$1FBD0`) |

After a `LEVEL`, `INTRO`, `PICCODE` load the original code calls its overlay relocator (absolute `$3D2EE`, hunk offset
`$3CEEE`: the Atari-ST-style `0x601A` image fix-up of `cdtv.md` section 3) and restores the game's own level-3 handler
(absolute `$34FB2`); the loaders return into that code, which is why a replacement must not return early.

### 3.2 CD audio

The slave **does not support CD music**: both audio routines are replaced by `rts`, and the ReadMe says "CD music control
from the game is currently impossible (CDTV)". There is therefore no WHDLoad reference for it. The game side is fully
documented in `docs/re-notes/cdtv.md` section 5 and `cdtv-music-map.md`: `play_track(d0.w)` stops, then `SendIO`s command
43 with `io_Offset = track`, `io_Length = track + 1`; a tick routine (every 50 VBLs) calls `CheckIO` and re-issues the track
when it has finished; stop is `AbortIO` + `WaitIO`; volume is `DoIO` command 56. Our `runtime/cdtv/` layer already
implements the playback side keyed on the same events, so a CDTV target needs only to hook `$3A924`, the stop at
`$3A8F6`, the tick `$3A95C`, and `$3A9BA`/`$3A9E6`.

## 4. What `X2` itself needs, measured on the whole hunk

A linear disassembly of the code hunk (126,694 lines) searched for instructions touching each area:

| Area | Result |
|---|---|
| exec | `OpenLibrary` x1 (-552), `AllocSignal` x1 (-330), `FindTask` x1 (-294), `OpenDevice` x2 (-444), `DoIO` x2 (-456), `SendIO` x2 (-462), `CheckIO` x1 (-468), `WaitIO` x1 (-474), `AbortIO` x1 (-480); `$4.w` read for `VBlankFrequency`. All of it sits in the start-up (`$3AA4A`-`$3AAE4`) and the audio routines. |
| dos | `Open`/`Read`/`Close` x10 each, only in the ten loaders. |
| CDTV hardware | **No instruction has an absolute operand in the CDTV DMAC/CD-controller area (`$DC0000` upwards), the CDTV ROM, NVRAM or the remote.** All CD access goes through `cdtv.device`. (A base address built at run time cannot be excluded by this scan, but the slave never emulates anything there and the game plays.) |
| CIA | `$BFE001` (CIA-A PRA, read at `$35012` for the fire button, `bset #1` at `$334CC`), `$BFD100` (CIA-B PRB, written `$FF`, `$87`, `$FF` once at start to deselect the floppy drives). No timer or serial-port (keyboard) access by absolute address; the game does not read the keyboard itself. |
| Custom chips | 47 registers addressed by absolute address: blitter (`$040`-`$064`), copper (`$080`, `$088`), display window and fetch (`$08E`-`$094`), `DMACON/INTENA/INTREQ/ADKCON`, `BPLCON0` (`$4200`: 4 bitplanes) / `BPLCON1`, modulos, sprite data (`$144`-`$17C`), `COLOR00`-`COLOR15` and the sprite palette, `JOY1DAT` (`$00C`), `POTGOR` (`$016`), `POTGO` (`$034`, written `$C00`). Audio registers are reached through a base register. No ECS-only register was seen. |
| Interrupts | only level 3 (vertical blank) at `$6C`. No CIA, copper or audio interrupt vector is installed. |
| Input | joystick port 2: `JOY1DAT` for directions, CIA-A PRA bit 7 for fire A, `POTGOR` bit 14 (high-byte bit 6) for the second button, which is what the CDTV joypad's button B drives. The remote control is not read. |

Consequence: apart from the ten file loads, the CD-audio calls and one start-up block, the CDTV program is the same kind of
bare-metal program as the disk build, and talks to the chip set the way the disk build does.

## 5. Timing sensitivity

- **None of the 17 patches is a timing fix.** There are no blitter-wait, copper, CIA-timer or disk-ready patches. (The floppy
  slave has `snoop` fixes for CIA-B and BPLCON0 writes and access-fault fixes in `lsr`/`mulu` code; none applies to CDTV.)
- The hooks that read timing: `$3AB2C` (PAL/NTSC from `ExecBase.VBlankFrequency`, needed only to pick 3 or 4 for the word at
  image `+$12`) and `_waitautofire` (polls `VPOSR` bit 0 = V8 to count 50 screen transitions while fire is held; a feature).
- Because the slave runs the game under WHDLoad's own 68000/68020 timing and never needed a delay patch, the program does not
  appear to depend on exact bus timing beyond what the disk build already needs (frame-accurate blitter and copper). This is an
  inference from the absence of patches, not a measurement.
- `jsr (a1)` with a pointer whose top byte is not zero (`$37A98`) is a 24-bit addressing dependence: on a real 68000 the byte is
  ignored. Musashi in 68000 mode masks addresses to 24 bits as the hardware does, so the runtime needs no equivalent; a
  translated `jsr (a1)` must apply the same mask.

## 6. Version history and quirks

From the ReadMe: CDTV 1.4/1.5 (2000) fixed highscore saving ("old scores are lost"), added cash carry-over and a fixed
trainer; 1.5/1.6 fixed a crash when no highscore was entered; Floppy 1.9 / CDTV 1.6 (2005) added the autofire highscore
guard; Floppy 1.10 / CDTV 1.7 (2017) adapted the slave to WHDLoad v17; Floppy 1.11 (2018) added intro skip and a 68000 quit
key for the floppy versions only. Differences between the CDTV and disk programs (language menu with EN/FR/DE/IT, speech,
music selection, joypad mode) are in the game, not the slave; see `cdtv.md` section 4 and 7.

Known quirks and limits:

- The original CDTV game keeps no highscore table across runs; the slave's `highs` file is an addition.
- No CD audio under WHDLoad; speech and effects work, and the one sound problem (`$410B2`) is unresolved.
- The game takes the whole machine (`INTENA` off, own vectors); quitting is the slave's `F10` keyboard hook (level-2 handler from
  WHDLoad's `keyboard.s`, not in the package), which the runtime replaces with its own hotkeys.
- The 6.9 MB of the data track that lies outside the ISO volume (`cdtv.md` open question 1) is not needed: the install
  copies only the 96 files and the game plays completely.
- The ReadMe and JOTD's notes warn that some **floppy** versions crash after the intro on fast WinUAE configs (self-modifying
  code); nothing similar is reported for CDTV.

## 7. Verdict: the minimal environment for a CDTV target

A recompiled or interpreted CDTV `X2` needs the following, and nothing else from the machine:

1. **Chip RAM of at least `$C5E5C` bytes (use 1 MB) with 20-bit DMA pointers** (bitplane, sprite, copper, blitter, audio), a
   24-bit CPU address space, supervisor-mode start with `SR = $2700`, and the image loaded once at `$400` with both hunks
   relocated (hunk 0 at `$400`, hunk 1 at `$6B09C`).
2. **Start-up shortcut**: enter at the image's own entry and **skip `$3AA4A`..`$3AAE7`** (set PC to `$3AAE8` with the stack
   already set to `$84F9C`), and store the PAL word (3) at `$412` instead of running `$3AB2C`. These are the only OS calls before
   the takeover.
3. **File service for ten call sites**: the replacement of `Open`/`Read`/`Close` by whole-file reads of `CD0:<NAME>` from the
   user's disc image (an ISO9660 reader over the data track, or files in the import directory), delivered to the buffers in
   section 3.1, continuing at the original post-`Close` address. Files: `LEVEL1-5`, `INTRO`, `PICCODE`, `PICDATA1-9,B,C`,
   `SPEECH00-66`, `SOUNDS00-09` (about 4.4 MB) plus `X2`.
4. **CD audio events** (play track, stop, full volume, end-of-track poll) mapped onto the existing `runtime/cdtv` player, with
   the two ioreq blocks and the `CheckIO` result faked (nothing is polled except through `$3A95C`).
5. **Input**: `JOY1DAT`, CIA-A PRA bit 7, `POTGOR` bit 14 and `POTGO` writes, with the second button mapped to a key.
6. **Chip-set behaviour** as for the disk game (copper, blitter, sprites, Paula, VERTB); `CIA-B PRB` writes accepted.
7. **Persistence**: highscores through the runtime's own store; the slave's `highs` file format (190 bytes at `$3C33E`, XOR with
   the length) is only needed to share files with WHDLoad.
8. **Static translation inputs**: the relocated image plus the overlays (`LEVEL1-5`, `INTRO`, `PICCODE`) loaded at `$4E26` and fixed
   up by the routine at `$3D2EE`, which are code and therefore need trace coverage like the disk build's level modules.

### Unknowns

- What the `0x601A` overlays do at run time (they are not patched by the slave, which suggests they use no OS services, but
  they were not disassembled here).
- Whether the speech and sound drivers use anything beyond Paula DMA and the VERTB tick (audio registers are reached through
  a base register, so the absolute-address scan does not list them).
- The unsolved sound problem at `$410B2`.
- Exact semantics of the `$3A296` poke site beyond "ships and cash initial values".
- Whether CDTV remote or button C/D events matter for the language and music-select menus (the code reads only port 2 and the
  second button).
