# End of game: level 5, the ending, and the loop back to level 1

Question (from a full playthrough on macOS with autofire and god mode): after the final shop / end sequence the
game **restarts from level 1**. Is that the original game's behaviour or a bug in this runtime?

**Answer: it is the original game's behaviour.** Xenon 2 Megablast has no "you have won" state. Finishing
level 5 shows a three-page ending text in the shop module, grants an extra credit, increments a loop counter
`$E02` and sets `$E04` back to 1. Every level's init multiplies `$E02` into two difficulty words, so the second
loop is harder than the first. There is no high-score entry and no congratulations screen on completion; the
high-score check only runs when the last ship is lost.

The code path is reproduced end to end in this runtime by `tests/replays/ending_natural.txt`, and the same
sequence of state words and screens was captured on the Amiberry reference (section 5).

## 1. Flow table

Addresses are run-time addresses. "game" = the resident image at `$400`; `level5` / `level6` = disc-2 overlays
loaded at `$54E00` (`disk-layout.md` section 2). Everything below is from the Ghidra listings in
`generated/lane-d2/` plus a linear disassembly of the level-5 blob for the parts Ghidra did not decode
(`tools/dis68k.py`; see section 2).

| # | Address | Module | What happens |
|---|---|---|---|
| 1 | `$62EFE` | level5 | Event-list record (list B, pointer at `$54E14`): **position `$0000`, handler index 6**. `level_event_scan` `$35A0` fires it when the scroll `$CD0` reaches 0, i.e. at the very end of the level. |
| 2 | `$551AA` → `$55BFA` | level5 | End event. `st $D99` (fire once), respawn point `$CE6=$1A0`/`$CE2=$A0`/`$CE4=$B0`, poisons the event-list head `$7491C` and the level data, then `$55C70`: `$CCE=$410`, **`$CD0=$CEA=$1A0`** (the scroll jumps back up to the final arena and is clamped there). |
| 3 | `$55C82`-`$55D10` | level5 | Spawns **22 objects** from the table at `$5594E` onto list `$B48`: one structure (type `$0134`, frame handler `$55D7E`, hit handler `$E48` = ignore, HP 20), two side pieces (type `$0144`, hit `$F2C`, HP 240), **18 guardian pods** (types `$0114`/`$013C`/`$0140`/`$0138`, hit handler **`$561D2`**, HP 5-40) and the **core** (type `$0050`, frame handler `$55D6E`, hit handler **`$562A6`**, `+$56` → the structure). |
| 4 | `$55D14` | level5 | `move.w #$12,$D7A` — **18 guardians to destroy**. |
| 5 | `$55D7E` | level5 | The structure's frame handler, every frame: `move.w #$1A0,$CEA` keeps the scroll pinned to the arena. |
| 6 | `$55D6E`-`$55D78` | level5 | The core's frame handler: `tst.w $D7A; bne rts` — it only calls `$E4E` (`$34EA`, register the hit box) **once `$D7A` is 0**, so the core is invulnerable until all 18 pods are gone. (`$560D2` likewise doubles the pods' speed at `$D7A == 0`.) |
| 7 | `$561D2`-`$561E6` | level5 | Guardian hit handler: `sub.w d0,$32(a1)`; on death `subq.w #1,$D7A` and `addi.l #$C8,$C92` (+200 points), then the explosion. |
| 8 | `$562A6`-`$562BE` | level5 | Core hit handler: `movea.l $56(a1),a2` (the structure), `sub.w d0,$32(a2)`; while it survives, `$562C0` picks the damage sprite. |
| 9 | `$562DC`-`$5631E` | level5 | **The game is beaten.** Clears the level data, `jsr $E6C` with `d7=9` → `$21B6` spawns **20 shop capsules** (types `$60`/`$18`, handler `$4634`) and adds 20 to `$CA4`; `jsr $E72` with `d7=$27` = the full-screen explosion; then **`st $A07`** (level complete). |
| 10 | `$7B72` | game | Main loop: `tst.w $CA4; bne $78CE` — the end of the level is held back until every shop capsule has been collected or has fallen off the bottom. |
| 11 | `$467A` / `$4742` | game | `powerup_dispatch`: each capsule decrements `$CA4`; the one that takes it to 0 does **`st $A6C`** (shop request). |
| 12 | `$7B7A`-`$7BE6` | game | `$A6C` set → stop the music (`$19FBA`), top the cash up to the level's minimum (`$54E3C`/`$54E3E`), `sf $D83`, `sf $8FEC`. |
| 13 | `$7BF0`-`$7C00` | game | `cmpi.w #$3C0,$CD0; bgt` / `cmpi.w #5,$E04; bne` → on level 5 with the scroll at or below `$3C0`: **`st $8FEC`** (ending flag). The arena is at `$1A0`, so this is always true after step 2. |
| 14 | `$7C06`-`$7C24` | game | Two-player only: if the other player is still active and has not finished the level, the shop is skipped (`$7C30`) and it is their turn instead. |
| 15 | `$7C26` | game | `jsr $8FF2` = `level_load_and_run`: fade, music off, save `$54E04` to `$8FEE`, disc-id check, load disc-2 **slot 6** (sectors 1476, count 248) to `$54E00`, `jsr $54E00`. |
| 16 | `$54E00`-`$54F42` | level6 | Slot 6 starts: own music driver (`$2090=$6477C`, `$2094=$647A6`), draws the shop backdrop and the shopkeeper, cursor defaults. |
| 17 | `$54F46` | level6 | `tst.b $8FEC` → set: `$55512` fills all 20 stock cells with item `$1A` / sprite `$56114` (the "?" cells). Clear: `$55AFC` builds the real stock from the player's list `$AEA`. |
| 18 | `$54F64` | level6 | `tst.b $8FEC; bne $5505A` → **the shop is skipped entirely and the ending runs**. |
| 19 | `$5505A`-`$55098` | level6 | Ending text, printed into the shopkeeper's panel: `$55138` "WELL, THAT'S IT VIEWERS.", `$55153` "DON'T FORGET TO TURN OFF YOUR SET...", `$5517A` "WAS IT GOOD ENOUGH FOR YOU ?" |
| 20 | `$550BC`-`$550F6` | level6 | 150 frames of the screen collapsing to a single white dot (`$55198` draws it) with the colour table at `$55118` strobed into `$DFF180` — the TV switching off. |
| 21 | `$550F8`-`$55114` | level6 | Fade (`$885E` d3=4), slot-6 music off (`$647B4`), 80 frames, `bra $55016`. |
| 22 | `$55016`-`$5504C` | level6 | Common exit (shared with a normal shop): `$9618`, `$420=$BF`, `jsr $5540` + `jsr $3534`, slot-6 music off, restore the resident music API (`$2090=$19FAC`, `$2094=$19FD6`), `$CF2=$C`, `rts`. **Slot 6 never touches `$A07`.** |
| 23 | `$9060`-`$90EC` | game | Back in `level_load_and_run`: `$A07` is set, so it reloads the **current** level's module — the table count is reduced by `$18` when it is `$12C` or more, which is the documented "level 5 reloaded with 288 of its 312 sectors" — restores `$54E04`, and `$9116` returns at once because `$A07` is set. |
| 24 | `$7C3E` | game | `tst.b $A07; bne $7C62` → the end-of-level path. |
| 25 | `$7C62`-`$7C84` | game | `sf $A07`; level 5 only: drain the object list `$AEA` through `$5C9E`, `bsr $5540` and `bsr $352E` (the end-of-level bonus), **`addq.w #1,$CF0`** — finishing level 5 gives one extra credit. |
| 26 | `$7C88`-`$7CEC` | game | Two-player hand-over (`$428`, `$A03`, `$A05`); in a one-player game it falls straight through to `$7CEC`, which clears the object lists. |
| 27 | `$7D0C`-`$7D2A` | game | **The loop.** `addq.w #1,$E04`; `cmpi.w #6,$E04; blt $7D30`; otherwise `move.w #1,$E04`, **`addq.w #1,$E02`**, `move.w #1,$8F94`, `clr.w $8F96`. |
| 28 | `$7D30`-`$7D3E` | game | "LOADING LEVEL 1" digit into `$8C41`, `bsr $917E` loads level 1 from disc 2 (sectors 11, count 229), GET READY, play. |
| 29 | level inits | level1-5 | Each level's start slot reads `$E02` and writes two difficulty words: L1 `$567A2` `$424=$E02*1`, `$426=$E02*2`; L2 `$57224` `*3`/`*5`; L3 `$570A8` `*6`/`*8`; L4 `$5736E` `*9`/`*B`; L5 `$5764C` `*C`/`*E`. `$E02` is set to 1 only by `$6BF8` (a new game), so the second loop runs with `$E02=2`. |

## 1a. What carries over into the next loop

Measured from chip-RAM dumps of `ending_natural` at game frames 3560 (still in step 25) and 3650 (level 1
loaded), and from the code that touches each one.

| State | Across the loop | Why |
|---|---|---|
| score `$C92.l` | **kept** (`$190` → `$190`) | only `$7B32` clears it, on a continue |
| ships `$CCA.w` | **kept** (3 → 3) | nothing between `$7C62` and `$7D3E` touches it |
| credits `$CF0.w` | **+1** (3 → 4) | `$7C84`, level 5 only |
| weapons / owned items, list `$AEA` | **lost** (head `$3F268` → the empty head `$AD8`) | the `$7C6E` loop frees every entry through `$5C9E`, level 5 only |
| cash `$C9A.l` | **cleared** | `$6F7C clr.l $C9A` in the level-start init `$6F1E`, which `$6F94` calls at `$7D50`. Not special to the loop: cash is per level |
| difficulty `$424`/`$426` | **raised** | each level's init multiplies the new `$E02` into them (step 29) |
| level `$E04.w` | **1** | `$7D18` |
| ending flag `$8FEC` | stays set until the next shop request clears it at `$7BEA` | nothing else writes it |
| high-score table | **not offered** | `$8A16` is reached only from `$7B14`, when ships reach 0 |

So the second loop starts with the score and the ships, one extra credit, no weapons, no cash, and harder
enemy parameters. That is the same state a fresh level-1 start has, except for the score, the credit and
`$E02`.

Not in the path, for completeness:

- **High-score entry** (`$8A16`) is called only from `$7B14`, when `$CCA` (ships) reaches 0. Beating the game
  does not offer it; the score simply carries into the next loop.
- `$8F94`/`$8F96` are written by `$7836`/`$783E` (new game) and `$7D22`/`$7D2A` (the wrap) and **never read** in
  the shipped image. They sit in the four bytes just after the `"  INSERT GAME DISK  "` string at `$8F80`.
- `$6F0C`/`$6F18` is a second route to `$A07`/`$A6C`, in `$6ECA`, reachable only in a two-player game.

## 2. A correction to the earlier notes

`trace-coverage.md` and `tests/replays/README.md` said that *"level 5 has no boss of its own; the level end
comes from its shop request at `$CD0 <= $3C0`"*. That is wrong in both halves:

- Level 5 **does** have an end boss: the structure, its 18 guardian pods and the core described above.
- Nothing requests the shop on level 5 until the core dies. `$A6C` is only ever set by level 1's `$5581C`,
  by `powerup_dispatch` (`$467A`/`$4742`, when the last shop capsule is gone) and by `$6F18` (two-player).
  The capsules come from `$E6C`/`$21B6`, which on level 5 is called only from `$562FE`, in the core's death
  code.

The reason the earlier pass missed it: Ghidra never decoded `$5631E` (`st $A07`), `$561E2` (`subq.w #1,$D7A`)
or `$55D72`/`$560D2` (`tst.w $D7A`). They are inside level 5's undecoded object-handler ranges
(`trace-coverage.md` section 3). They were found by scanning the raw level-5 blob for the encodings of
`st (xxx).w` / `subq.w #1,(xxx).w` / `tst.w (xxx).w` against `$A07`, `$A6C` and `$D7A`, and then disassembling
around the hits with `tools/dis68k.py`.

## 3. New variable

| Address | Meaning | Evidence |
|---|---|---|
| `$D7A.w` | level-5 guardians left | Set to `$12` by level 5's end event (`$55D14`). `subq.w #1` on each guardian death (`$561E2`, in the hit handler `$561D2` the spawn table gives 18 of the 22 objects). `$55D72` gates the core's hit box on it being 0; `$560D2` doubles the pods' speed at 0. Cleared only by the level's own code. |

## 4. What this runtime does

`tests/replays/ending_natural.txt` (4,600 frames) reaches the end of level 5 through the game's own code. The
only pokes are the usual coverage ones plus a `$D7A` poke, which plays the same role as the boss-HP pokes in
`boss1`/`level4_boss`: the frame budget is not enough to fly to all 18 guardians, so `$D7A` is set to 1 and
the one guardian that is reachable finishes the set.

State words over the run (`--watch e04,e02,a06,a6c,8fec,cf0,cca,c92,c94,cd0,d7a,ca4`, only the changes):

| Frame | What |
|---|---|
| 1220 | coverage poke: scroll to `$0` with the event cursors at `$40` |
| 1283 | the end event fires: `$CD0 = $19F`, **`$D7A = $12`** |
| 1301 | coverage poke: `$D7A = 1` |
| 1612 | a guardian dies: `$D7A = 0`, score +200 (`$C94 = $C8`) |
| 1651 | the core dies: **`$CA4 = $14`** (20 shop capsules spawned by `$E6C`) |
| 1652 | **`$A07 = $FF`** (`$5631E`) |
| 1652-1983 | the capsules are collected or fall off: `$CA4` `$13` → 0 |
| 1983 | **`$A6C = $FF`** (`$467A`/`$4742`), scroll `$CD0 = $CB` |
| 1985 | **`$8FEC = $FF`** (`$7C00`); slot 6 is loaded and runs |
| 1985-3577 | the ending: shop backdrop, the three text pages, the dot-collapse strobe |
| 3577 | `$A07` and `$A6C` cleared, **`$CF0` 3 → 4** (the extra credit at `$7C84`) |
| 3578 | **`$E04 = 1`, `$E02 = 2`** (`$7D18`/`$7D1E`) — the loop |
| 3630 | disc-2 sectors 11 count 229: level 1 loaded |
| ~3700 | GET READY PLAYER 1, then level 1 again |

No restart through `$7032`, no unmapped access, no write into executed code.

## 5. The Amiberry reference

The same replay was played on the Amiberry reference with the original disks
(`reference-compare.md` sections 1, 7 and 9), converted with
`tools/compare/to_harness.py --step-sync` and run to vsync 17500. **The reference does exactly the same
thing**, within the known 1-2 logic-step drift:

| Event | This runtime (game frame) | Reference (game frame) |
|---|---|---|
| level-5 end event, `$D7A = $12` | 1283 | 1284 |
| a guardian dies, `$D7A = 0`, +200 points | 1612 | 1616 |
| the core dies, `$CA4 = $14` (20 shop capsules) | 1651 | 1655 |
| **`$A07 = $FF`** | 1652 | 1656 |
| last capsule gone, **`$A6C = $FF`** | 1983 | 1990 |
| **`$8FEC = $FF`**, slot 6 loaded | 1985 | 1992 |
| ending over, `$CF0` 3 → 4 | 3577 | 3577 |
| **`$E04 = 1`, `$E02 = 2`** | 3578 | 3578 |
| level 1 running again, `$CD0 = $1200` | 3700 | 3700 |

Pictures (`tools/compare/compare_replay.py`, 17 checkpoints, fixed geometry, best of shifts -2..+2):

- the dot-collapse and the black screen after it (game frames 3100, 3350, 3500, 3570) are
  **pixel-identical, 0.000 % differing**;
- the shop backdrop before and after the text (2200, 2800) and the level-1 restart (3600, 3700) are within
  0.12 %;
- the ending-text page at 2500 differs by 2.27 %: the text itself is identical, and what differs is the
  REALCASH digits (`006500` here, `006400` on the reference, the random top-up above) and one of the shop's
  TV-static cells, which comes from the same generator (`reference-compare.md` section 9);
- the guardian fight (1600-1700), the slot-6 load boundary (1990) and level-1 play after the loop
  (4000-4590) differ by 1.8-46 %: object and starfield positions one to two logic steps apart, for the same
  reason.

The only state words that differ are the ones that drift in every replay: the scroll by two rows, the ship's
x by six pixels, and the shop's random cash top-up at `$7BB0` (`6500` here against `6400` on the reference,
the `+0..3 x 50`). `$E04`, `$E02`, `$A07`, `$A6C`, `$8FEC`, `$CF0`, `$CCA` and `$D7A` agree at every
checkpoint.

Audio over the ending (game frames 2108-3652): envelope correlation 0.87, output level 0.81x, the usual
`reference-compare.md` section 6 figures.

**Conclusion: restarting from level 1 after the ending is what the original does. There is nothing to fix.**

The CDTV release was not captured. Its music map (`cdtv-music-map.md`, row "Ending (slot 6 module)") treats
the ending the same as a shop: no CD track is played while the module runs, and `music_init` from `$7C50`
restarts the level track afterwards. That says nothing about the ending or the loop differing; the game logic
is the same code. Checking the CDTV loop on real CDTV data is still open.

## 6. How to see it yourself

In this runtime:

```
./build/x2recomp --assets generated/assets --input-script tests/replays/ending_natural.txt
```

or, headless, with the state words:

```
./build/x2recomp --headless --assets generated/assets --input-script tests/replays/ending_natural.txt \
  --watch e04,e02,a06,a6c,8fec,cf0,cca,c92,c94,cd0,d7a,ca4 --watch-log /tmp/end.txt
```

In FS-UAE or any other emulator, with the same disks, the pokes become:

| When | Poke |
|---|---|
| in level 5 | `$CD0 = $CEA = $CCE = 0`, `$CD2 = $CD4 = $40` — jump to the end of the level and let only its end event fire |
| after the guardians appear | `$D7A = 1`, and the guardians' and the structure's HP (`obj+$32`) to 1 |

Without any poke at all, play level 5 to the end (about 16,000 frames of scrolling at the native rate),
destroy the 18 guardians and then the core.
