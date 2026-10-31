# CDTV release: boot and runtime services (phase C3)

Phase C3 of `.omc/plans/xenon2-cdtv-target-plan.md`: the CDTV release starts and plays in the interpreter. This note records what
the runtime does, what was found while making it run, and what has and has not been checked. It builds on `cdtv-c0.md` (the trap
sites), `cdtv-importer.md` (what the importer writes) and `cdtv-runtime-gap.md` (the architecture choice). No game bytes are quoted:
addresses, counts and short instruction-level facts only. All addresses are **absolute** (hunk offset + `$400`, the link base).

## 1. What runs

`x2recomp --game cdtv` (or plain `x2recomp` when no disk release is imported) loads `<data dir>/assets-cdtv`
(`game_0400.bin`, 809,564 bytes linked at `$400`, and `cd0/`, the 95 files of the disc) and starts the program at `$400` on a machine
with 1 MB of chip RAM. Observed in the interpreter, headless and in a window, from a cold start:

| Frames (about) | What is on screen |
|---|---|
| 20 | the program's start-up; two files loaded (`SOUNDS00`, `SPEECH63`); CD track 2 requested |
| 100-340 | "SELECT JOYPAD MODE AND PRESS BUTTON A" over the starfield; fire confirms it |
| 373-700 | `LEVEL1` loaded (probably for the attract demo; not checked), then the title pages ("XENON 2 MEGABLAST", "Designed by The Bitmap Brothers") |
| 700-2100 | the language-select page (five flags with the word "choose" in five languages, the guide's picture and text), the 1/2-player menu |
| 2105 | `LEVEL1` and `SOUNDS01` loaded for the game; CD track 3 requested (the level music) |
| 2300 on | level 1: the organic scenery scrolling, the ship, the HUD, enemies, scoring, the special weapon |

5,100 frames and 6,000 frames of scripted play run without a hang, an unmapped access or a fault (`unmapped=0`), and two recordings of
the same script give identical frame and audio hashes (`tests/replays/cdtv/`). A forced game over reaches name entry, the
high-score table is saved and the next run loads it into the game's memory.

## 2. What the runtime provides (`runtime/cdtv/cdtv_game.*`, `runtime/boot.*`)

All of it is a PC trap at the address of what it replaces (the idiom of `DiskShim` and the retail check); the image is never changed.

| Trap site (abs) | Replaces | The runtime does |
|---|---|---|
| `$3AA4A` | the OS start-up block (`OpenLibrary`, `AllocSignal`, `FindTask`, two `OpenDevice`, a hand-built message port) | continues at `$3AAE8` |
| `$3AB2C` | the ExecBase PAL check (`movea.l $4.w,a6 ; cmpi.b #$32,$212(a6)`) | continues at `$3AB40`; the PAL word `$412` the program already set to 3 stays 3 |
| ten sites each: `$3393C…`, `$33954…`, `$3395A…` | dos `Open` / `Read` / `Close` of `CD0:NAME` | `Open`: d1 = name, d0 = a handle (0 if the import lacks the file); `Read`: d1 = handle, d2 = buffer, d3 = length, d0 = bytes (a short read at the end, 0 after); `Close`; each skips its 4-byte `jsr` |
| `$3A8F6` stop, `$3A924` play (d0.w = track), `$3A95C` poll, `$3A9BA` / `$3A9E6` volume full / mute | the `cdtv.device` calls (`AbortIO`+`WaitIO`, commands 43 and 56 via `SendIO`/`DoIO`, `CheckIO`) | play and stop keep the program's own track variable `$3A8F2` and queue a cue for the CD player; the poll returns at once; volume sets the game volume |

**The poll needs nothing.** The program asks `cdtv.device` once a second whether the track has played out and restarts it if so. The runtime's
CD player already does exactly that in emulated time (`CdPlayer::tick`, 50 ticks), so the routine itself is an `rts`. A track therefore loops
with the gap of up to one second the original has, with no state in two places.

**Boot state.** The program is a normal AmigaOS program, started by the OS in user mode with interrupts open. What it relies on that the
OS left behind: the **interrupt mask at 0** (the program never touches SR; with the mask at 7 it waits forever in its first delay loop,
which is exactly what the first boot did); the **vertical-blank interrupt enabled** (the program only toggles the audio enables and the
master bit); and a **level-3 vector that clears the interrupt request** (the program saves the OS vector and chains to it at the end of
its own handler, and never clears VERTB itself). Boot stores the program's own 8-byte routine for that (`move.w #$70,INTREQ ; rte` at
`$350C4`, the one the C0 study found), enables VERTB and starts in supervisor mode with SR `$2000`. There is no restart vector (the program
never exits), so reset is a host operation: the machine is rebooted.

## 3. Findings while making it run

1. **The interrupt mask** (above) was the only boot problem. After the fix the first vertical blank is handled at frame 20 and the poll
   counter runs at 50 Hz.
2. **The PAL check must be skipped, not just the OS block.** With `$4` empty, `a6` is 0 and the check reads byte `$212` of chip RAM; it is
   not 50, so the program would set the NTSC word 4 and run its game logic every fourth frame. The WHDLoad slave skips the same range.
3. **The variable map of C0 is right.** Read from a live level 1: level `$E0A` = 1, energy `$CCC` = `$27`, ships `$CD0` = 3, credits `$CF6` = 3,
   PAL word `$412` = 3, track variable `$3A8F2` = 3 (the level music). The high-score table (190 bytes: ten longs then ten 15-character lines) is at
   `$3C33E`, and the save point `$3C54C` has the shape of the disk build's (`bsr.w ; rts`).
4. **The assists work on the CDTV addresses.** A 6,000-frame level-1 replay with `--god-mode --credits 9` ends at full energy, 3 ships and 9
   credits; the same replay without them ends at energy 7 with 2 ships. The death routine trap (`$391B2`) sits on the program's own routine.
5. **The second button is the special weapon.** The program samples POTGOR bit 14 once every 20 vertical blanks (`$33D43` counts down from
   `$14`), stores `$40` in the command byte `$A05`, and the main loop's dispatch sends `$40` to a routine that spends one of the special weapons
   counted at `$CA6` and starts the effect. With none carried it does nothing, which is why a press at the start of level 1 changes no frame; with
   three poked in (a coverage poke, `tests/replays/cdtv/level1_special`) the first press changes the picture at once. The button must be held for
   at least 20 frames for the sample to catch it, as on the hardware.
6. **Names.** The program builds `CD0:LEVEL1` and the like; the runtime matches case-insensitively, drops any device prefix and a `;version` suffix,
   and logs a name the import lacks once. An `Open` that fails returns 0 and the program's retry loop waits, which is what a CD that is not in the
   drive looks like; the front end prints the name and `GameStats::failed_opens` counts it.

## 4. Not done, and what is not known

- **Translated executor.** Added in phase C7: `-DX2_RECOMP=ON` builds run this release on its own translation when `generated/recomp-cdtv` is linked in
  (`cdtv-recomp.md`), on the interpreter otherwise (printed at start-up).
- **Interpolation, widescreen, asset packs and the HD panel** hook the disk build's draw routines at fixed addresses; they are off for this
  release (the faithful picture is forced, with a message) until phase C8 maps the CDTV equivalents.
- **`--protect-music`** is a tracker-music feature; the CDTV release's music is the CD, so the option and its hotkey say so and do nothing.
- **Only the front end and level 1 were exercised.** Levels 2-5, the bosses, the shop, the speech screens, two players and the ending are
  untested here; the CDTV program has not been compared with a reference.
- **The language-select page shows noise in most item cells** (the first row shows its pictures after a while). This was not investigated: it may
  be the program's own "static" effect for items that are not available, or graphics the overlays have not drawn yet. It needs a look against a
  reference (phase C5).
- **The Amiberry run of the same disc** (`cdtv-reference.md`) showed dotted vertical streaks where level 1's scenery should be. Our level 1 draws
  the full scenery. Why the emulator's picture differed is not known; the WHDLoad-slave route is the independent comparison planned for C5.
- **CD end-of-track timing** is the player's, not the drive's: a track restarts within a second of ending, as the program's poll implies, but the
  exact frame depends on the player's loop poll, not on `cdtv.device`.
- **The CDTV remote** never reaches `X2` (confirmed again: no keyboard or CDTV register is read), so the pad is the only input.
