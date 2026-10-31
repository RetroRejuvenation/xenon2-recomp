# CDTV replay corpus (phase C6a)

The CDTV release (`x2recomp --game cdtv`, `docs/re-notes/cdtv-boot.md`) has 23 recorded replays in `tests/replays/cdtv/`: scripts from
`tools/trace/make_cdtv_replays.py` and frame and audio hash manifests (every 500 frames and at the last frame, like the disk build's).
They are regression coverage for the later phases (the translated executor, modernisation); they are **not** a claim that the game
was played through: most of them use coverage pokes, which are marked in the script and printed when a run uses them.
No game data is in the repository: scripts and hashes only. `tools/trace/cdtv_probe.py` measures what a replay reaches (it
dumps chip RAM at chosen frames and prints the engine variables, the disc files the game opened and the hittable-object list).

All 23 manifests match a fresh run, twice (the runtime is deterministic), and every replay runs 6000 frames or fewer in 7-8 s
(10 s when six run at once) with 0 unmapped accesses, 0 restarts and no frozen frame counter. The four earliest scripts and manifests
(C3) are unchanged.

## 1. The variable map, measured

The C0 note derived the CDTV addresses by code equivalence. The replays needed them as absolute chip addresses, so they were checked
against RAM dumps of both builds playing level 1 (the same replay shape, frames 2500 and 3500; values compared word by word over
`$C80-$E20`): the engine's variable block is the disk build's **plus 6**, absolute. Evidence is that the values follow the same
pattern in both builds (energy full, then hit; ships decrementing; the scroll position falling from `$1200`; the event cursors a
step behind it; `$E0A` = 1 and `$E08` = 1) and that each poke below had the expected effect in the running game.

| Role | Disk | CDTV | Confirmed by |
|---|---|---|---|
| level 1-5 | `$E04` | `$E0A` | the dump; poking level-1 and the complete flag loads `LEVEL<n>` and `SOUNDS0<n>` |
| loop counter | `$E02` | `$E08` | 1 at game start; 2 after the natural ending |
| level-complete flag | `$A07` | `$A0D` | `$FF` makes the main loop load the next level; set by the boss's death |
| shop request | `$A6C` | `$A72` | `$FF` opens the shop (`SOUNDS00`, a `SPEECHnn`) |
| scroll position / second copy / 2.5x | `$CD0` / `$CEA` / `$CCE` | `$CD6` / `$CF0` / `$CD4` | values fall together from `$1200`; the same ratio |
| event cursors | `$CD2`, `$CD4` | `$CD8`, `$CDA` | one step behind the scroll position |
| lower scroll clamp | `$CE8` | `$CEE` | stays `0` until the poke; the fast-forward moves it |
| energy, ships, credits | `$CC6`, `$CCA`, `$CF0` | `$CCC`, `$CD0`, `$CF6` | the boot table; the assists work |
| score, cash | `$C92`, `$C9A` | `$C98`, `$CA0` | score rises as enemies die; cash poked to 200000 shows `0200000` in the shop |
| shop cursor column, row | `$4F2`, `$4F4` | `$4F8`, `$4FA` | the cursor moves with the joystick; EXIT is column 0, row 4 |
| special weapons carried | not mapped | `$CA6` | poked in `level1_special` (C3): the second button then fires them; no disk counterpart was identified |
| head of the hittable-object list | `$B4C` | `$B52` | the list walk reproduces the objects; objects +`$A` = hit handler, +`$32` = HP, +`$12` = next |
| ending flag | `$8FEC` | `$3D24E` | set by the game itself when the level-5 shop is requested late |
| level-5 guardians left | `$D7A` | `$D80` | (poke used by `ending_natural`) |

Everything outside the variable block (objects, the ship, the bosses' state) is different: objects live low in memory (about `$1000-$4000`)
instead of at `$3F000` and above.

### Bosses and their hit handlers

The overlay code was re-assembled (long absolute addressing), so the handler addresses were found by the objects themselves: type, HP and
lifetime match the disk build's one for one. Overlay buffer: `$4E26` onwards (the disk's modules load at `$54E00`).

| Level | Object | Disk handler | CDTV handler | Evidence |
|---|---|---|---|---|
| 1 | nautilus boss, HP `$1E`, spawned with the level | `$561DA` | `$62D4` | same type `$50`, HP `$1E`; the replay kills it |
| 2 | spider, HP `$4B`; its 3 parts HP `$C` | `$5685A` / `$56944` | `$69AE` / `$6AAA` | same types and HP |
| 3 | boss, type `$50` | `$55642` | `$5716` | the only type-`$50` object near the level's end; the disk's helper `$55BC0` is `$5CDC` |
| 4 | head HP `$64`, eyes HP `$21`/`$32` | `$559D6` / `$55A1C` | `$5A82` / `$5AD4` | same HP set; the other level-4 objects (HP `$AF`, `$32`, `$14`) also match one to one |
| 5 | the 22 objects the end event spawns | `$561D2` | `$6318` | 22 objects of one handler, as the disk build's note says |
| 5 | the structure the core damages | `$E48` | `$E4E` | inside the engine's handler table (variable block + 6) |
| 5 | pods in the arena, HP `$28`; the large structure, HP `$C8` | `$56678` / `$56526` | `$6806` / `$669C` | same types and HP |

## 2. What each replay reaches

"Opened" is the disc file list the loader log shows after the front end (`tools/trace/cdtv_probe.py`).

| Replay | Frames | Reaches |
|---|---|---|
| `attract` | 1500 | the joypad-mode page and the title pages |
| `menus` | 2500 | the guide pages and SELECT menu, then `LEVEL1` loaded (frame 2105) |
| `menu_2p` | 6000 | the SELECT menu with the cursor moved to 2 PLAYER GAME and MUSIC toggled; a 2-player game; P, SPACE and ESC (the original has no keyboard: the keys reach the same key table; after ESC the game is at the start of level 1 again, but the replay does not separate that from the 2-player game ending, so no claim is made about what ESC does) |
| `level1_play` | 6000 | level 1 played with no pokes (deaths happen) |
| `level1_special` | 3600 | three special weapons poked in and fired with the second button |
| `level1_ff` | 6000 | level 1 from `$1180` to `$1F8`: the mid-level shop (opened, `SOUNDS00`; left at once; `SOUNDS01` reloaded) and the second half |
| `level1_flood`, `level2_flood`, `level4_flood` | 6000 | many enemy types alive at once after one-step scroll jumps (level 3 omitted: see 3) |
| `shop` | 6000 | the mid-level shop; cash 200000 poked; purchases happen (cash falls in `$C8`-`$258` steps) at the first cells; the other cells show the game's own static (no stock) |
| `shop5` | 6000 | `LEVEL5`, the shop (`SOUNDS09`), purchases |
| `level2` ... `level5` | 6000 | `LEVEL<n>` + `SOUNDS0<n>` opened through the game's own end-of-level path; the scroll fast-forwarded to the end (level 2: `$1`; levels 3 and 4: below `$60`; level 5 holds at `$5C0`, as the disk build's level 5 holds at `$560`) |
| `boss1` | 6000 | the level-1 boss **defeated**: HP held at 1, the game's death code runs, bonus counted into cash, level-complete flag, the end-of-level shop, then `LEVEL2` loaded through the main loop (frame 5694) |
| `boss2`, `boss3` | 6000 | the level-2 spider and the level-3 boss alive in their arenas with HP 1 — **not defeated** |
| `level4_boss` | 6000 | level 4's arena (the boss structure at the right edge); eyes and head with HP 1; **not destroyed**: the ship's sweep does not line up with them |
| `two_player` | 6000 | 2 PLAYER GAME; GET READY PLAYER 1 and PLAYER 2 alternate; ships, score and continues per player |
| `game_over` | 5345 | a death with a high score (2,000,000), the name entry (the table shows the entry), the continue prompt (credits 3 → 2), the last credit lost, GAME OVER, the second entry and the SELECT menu |
| `ending` | 6000 | a late level-5 shop request: the game sets the ending flag (`$3D24E` = `$FF`) and runs the shop code (`SOUNDS09`) |
| `ending_natural` | 5680 | level 5 **finished** with no shop or ending poke: the end event, the 22 guardians, the level-complete flag, the shop capsules, the game's own shop request, the ending flag, the ending, then level 1 again with the loop counter at 2 (frame 4451) |

Only the level-1 boss and level 5 (through its guardians and core) are defeated, as in the disk build's corpus (its notes say the same:
"only the level-1 boss is beaten" among the first four levels).

## 3. Findings and fixes

- **`pokeobj` followed the disk build's object list** (`$B4C`) and found nothing in the CDTV release. The list head is now part of
  the build table (`BuildInfo::var_objlist`; `$B52` for the CDTV release) and `apply_poke` takes it; a smoke test covers both heads.
- **The mid-level shop opens late if the scroll jump comes too early.** With the jump at frame 1780 (during GET READY) the shop opened about
  1000 frames later; with the jump at 2100 it opened about 80 frames later. The disk build's trigger is `scroll + ship y < $A80`, so the
  likely cause is the ship's height before it has flown in (inferred from that rule; the CDTV ship object was not located). The CDTV
  replays jump at 2100.
- **The CDTV shop is slower** than the disk build's: every shopkeeper phrase is a speech sample loaded from the disc
  (`SPEECHnn`), and input is taken between phrases, so one purchase takes about 200 frames (the disk build's 90). The shop replays use a
  fire every 40 frames per cell; only the first cells (the top row) had stock at level 1.
- **Static in the item cells is the game's own effect** (an empty cell), not a rendering fault: the disk build's SELL phase shows the same
  static, and the language-select-looking guide pages reuse the shop screen. (The C3 note left this open.)
- **Level 3 can hang after a fast-forward.** A first `boss3` (lead-in of 200 frames instead of 300, a 40-frame step) froze the frame counter near
  frame 2600 with the CPU looping in `$362FC-$3631C`; the same fast-forward with the `level3` lead-in ran healthily. It is probably the pool-exhaustion hang
  the disk build's README describes (`obj_alloc` looping on a self-linked list node after an event flood), but that was **not traced** here. It is
  a poke artefact (the scroll is moved far faster than the game scrolls), not a runtime fault the replays can show: the committed scripts avoid
  it (`boss3` uses the `level3` lead-in), and the CDTV `level3_flood` is left out for the same reason as the disk build's.
- **No runtime fault** was found: 0 unmapped accesses, 0 restarts and a running frame counter in all 23 replays.

## 4. Not covered

- Hand play: nobody has played the CDTV build through; the replays are scripted.
- The bosses of levels 2, 3 and 4 are reached but not defeated; the level-3 end and level-4 head need steering to line up with their
  vulnerable points.
- Speech content and the CD audio levels (C4); the translated executor (C7, done: `cdtv-recomp.md`, these replays are its test and its seeds); a reference for the pictures (C5). The Amiberry reference
  of level 1 showed vertical streaks where this runtime draws scenery, so frame hashes here are our own recordings and say nothing yet
  about agreement with a real CDTV.
- The Amiga-side language selection of the CDTV disc (EN/FR/DE/IT text) is in the front end's guide pages, not a separate menu; no
  replay chooses a language.
