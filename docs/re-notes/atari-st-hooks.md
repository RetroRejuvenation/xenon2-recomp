# Atari ST: hooks, high scores, options and coverage pokes (phase S5)

The ST release runs the real Kixx disk on the `StMachine`. This phase adds the same extras the Amiga and CDTV builds have, without
changing what the game does by default: the high-score table is kept, and the opt-in assists (god mode, extra credits, autofire)
exist. Everything is done from the host side: a RAM write at a frame boundary, or a **PC hook** (`StMachine::add_pc_hook`) that runs
when the CPU is about to execute an address. No game byte is patched. Addresses are the ST resident engine's (loaded at `$400`);
they were found by dumping RAM (`--st-ram-dump-at`), disassembling (`tools/dis68k.py`) and watching writers (`--st-watch-pc`).

With no option given nothing is hooked and no byte of RAM is touched: the `st_kixx` golden hashes are unchanged. Window runs install
the death hook (so Backspace can switch god mode on later); headless runs only with `--god-mode`.

## Game variables (same layout as the Amiga disk build, ST addresses)

| Address | Meaning |
|---|---|
| `$E04.w` / `$E02.w` | level 1-5 / loop count (the game wraps to level 1 after 5) |
| `$CC6.w` | ship energy (full = `$27`) |
| `$CCA.w` / `$CF0.w` | ships / credits (a new game sets 3 and 3; the continue code does `subq.w #1,$CF0`) |
| `$C92.l` / `$C9A.l` | score / cash |
| `$42E.w` / `$D84.b` | push-back counter / dying flag |
| `$A07.b` | level-complete flag; the main loop at `$8258` sees it and runs the end-of-level path (`$8360`) |
| `$A6C.b` | shop request (`$8250`) |
| `$B4C.l` | head of the hittable-object list (next `+$12`, hit handler `+$A`, HP `+$32`, type word 0 ends it), as on the Amiga |
| `$A00.b` / `$A01.b` | joystick held / fire edge, fed from the IKBD packets |
| `$8FAA` | high-score table: 190 bytes (10 score longs, then 10 lines of 15 characters), the Amiga layout |

## Hooks

| PC | What it is | Hook |
|---|---|---|
| `$6796` | the death routine; callers `$6838` (energy gone) and `$6AF0` (push-back sequence finished with energy left) | god mode: refill energy to `$27`, clear `$42E`, `do_rts()`; the dying flag is still clear, so the ship carries on |
| `$77CE` | just after the new-game initialiser's `move.w #3,$CF0` (`$77C6`); a continue never runs it | `--credits N`: write N |
| `$91BC` | the name entry's end (it returns into the table writer) | save the table |

The credits hook first sat at `$9340`, a *different* `move.w #3,$CF0` that only runs for the first new game from the title; the
initialiser at `$7790` is the one every new game goes through (found by watching `$CF0`: writers `$9340` once, then `$77C6`/`$81E2`).

## High scores

The original keeps none. `highscores-st.bin` (`persist.h` format: `"X2HS"` v1, FNV-1a hash of the **disk image**, CRC-32 of the 190 bytes;
214 bytes) lives in the data directory (`--persist-dir DIR` for a headless run, `--no-persist` to turn it off). One file per release, so a
Kixx table never goes into another build. Restore: the engine loads its own pristine table (`" 1. 0000000 :::"` ...) at start-up; the first
frame on which `$8FAA` is pristine, the saved table is written over it, once. A file for another image, a bad CRC or a short file is
reported and ignored. Save: at the name entry's end (`$91BC`) and once more at exit if the table changed; an unchanged table is not
rewritten. A session that used a cheat (god mode, extra credits, the Insert key) saves nothing unless `--cheat-scores` / `cheat_scores = on`.

Measured: a first run saves 5 times over one 14,000-frame script (214 bytes); the second run prints `loaded`, and the RAM table at frame 1999 shows
`" 1. 0000600 AAA"` with first score long `$00000258`. `st_hooks_scores` pins this (the restored top score equals the first run's best, and a
cheating third run leaves the file byte for byte unchanged).

## Options and window keys

`--god-mode`, `--credits N` (1-9), `--cheat-scores`, `--autofire[=RATE]`, `--persist-dir`, `--no-persist` and the `settings.cfg` keys are the
Amiga build's. In the window the shared host commands apply (`input.cfg`): autofire, god mode and the extra credit key act as there, the title
shows `[autofire]`, `[CHEAT: god mode]`, `[CHEAT used]`, and autofire uses the shared `AutofirePolicy` with "in a game" taken from the ship
count at `$CCA`. `--protect-music` prints that it has no effect. `x2recomp --game auto` picks the disk release, then CDTV, then ST.

## Coverage pokes (S6 replays)

`FRAME poke ADDR VALUE[.b|.w|.l] [N]` and `FRAME pokeobj HANDLER OFFSET VALUE[.b|.w|.l] [N]` in the shared script format now work for the ST
(`--poke`, as on the Amiga, takes the same). A run with pokes prints `coverage pokes active`. Verified on the Kixx disk
(`tests/replays/st/`, `tools/trace/st_probe.py`):

| Variable | Result |
|---|---|
| `$E04 = N-1`, `$A07 = $FF` | the main loop's own end-of-level path loads level N; checked for N = 2, 3, 4, 5, all play on with no unmapped access |
| `$CC6 = $27` held | invincibility (the energy bar stays full) |
| `$CD0`, `$CEA`, `$CCE = 0`, `$CD2`, `$CD4 = $40` | jump to the end of level 5 so its end event spawns the guardians (`$D7A = $12`) |
| `pokeobj $50360 $32 1` and `pokeobj $E48 $32 1`; `$D7A = 1` | level 5: guardian HP and the core's target held at 1; one pod finishes the set; the core dies and the game plays its ending, +1 credit, back to level 1 (`ending_l5`) |
| `$C9A.l = 5000`, `$A6C = $FF` | requests the shop; the shop code runs (`$8F0A`..`$12150`) but the exit was not confirmed with the fire pulses used, so no shop replay is recorded |

Differences from the Amiga map: `$D7A` and the level-5 handlers sit at different addresses (the level code is loaded at `$4F000` here; the guardian
hit handler is `$50360`, found by its `subq.w #1,$D7A` at `$50370`, where the Amiga build has `$561D2`), and `$8FEC` (the Amiga ending flag) is not referenced by the ST code at all.
The mid-level shop triggers (`$CD0` + ship y) were not mapped.

## Bug found on the way: a DMA that runs past the top of RAM

Loading level 2 stalled in the depacker with 8 million unmapped accesses: the loader reads 148 sectors to `$6A83C` and the last one ends 16
bytes past `$80000`. On a real ST the DMA stops at the top of the 512 KB machine's RAM (and on a 1 MB machine the bytes are real). The WD1772
model dropped the whole sector when it did not fit; it now stores as far as RAM goes (`fdc.cpp`, tested in `st_machine`). Levels 3-5 and the
level 1 to 2 transition now load.

## Tests

* `st_game` (synthetic, no game bytes): the table check, the death hook on hand-assembled code (god mode on, hotkey only, off), the credits hook, the
  score file (save, restore, other image, corrupt, cheat rules, no persist dir) and the poke walk (including a self-linked list).
* `st_hooks_god`, `_credits`, `_levels`, `_ending`, `_scores` (skipped without the Kixx dump): count and state checks from the run report.
* `st_machine`: the partial-DMA store. `st_sound`: the parser's poke syntax.

## Open

* The shop's fire/exit sequence on the ST is not scripted; the mid-level shop trigger and the boss death flags of levels 2-4 are unmapped.
* A real hand-play of the window keys (Backspace/Tab/Insert depend on the bindings in `input.cfg`) is the user's.
