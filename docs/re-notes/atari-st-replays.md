# Atari ST replay corpus and the comparison with Hatari (phase S6)

Phase S6 gives the Atari ST (Kixx) release what the disk build and the CDTV release already had: a corpus of scripted replays with recorded frame and audio hashes, a measure of
what each one reaches, and an independent reference to compare against (Hatari 2.6.1 with EmuTOS 1.4). Nothing here is game data: the scripts are input lines and pokes, the
manifests are hashes, the comparison output is numbers. **Status: the release plays in the interpreter on every scripted path below; a hand-played session has not been done.**

| | |
|---|---|
| Replays | 22 generated scripts in `tests/replays/st/` (`tools/trace/make_st_replays.py`) plus the three hand-written scripts of the S5 hook tests (`hold_fire`, `level2`, `ending_l5`) |
| Manifests | a `.hashes` file each: frame hash and cumulative audio hash every 500 frames and at the last frame; recorded twice, identical |
| Run time | 0.9-2.2 s per replay (interpreter, macOS arm64), 0 unmapped accesses in all |
| Tests | `replay_st_<name>` x 22 (Skipped without the Kixx dump), `st_replay_scripts` (no game data; the generator against the committed scripts, manifest shape, the Hatari conversion), `recomp_st_<name>` on a translated build |
| Tools | `make_st_replays.py` (write, `--record`, `--hatari`), `st_probe.py` (states at chosen frames, object list, level loads, liveness), `st_coverage.py` (the table below), `tools/compare/st_replay_compare.py` (the Hatari comparison), harness patch `0002-x2ref-poke.patch` |

## 1. Front end and building blocks

Frames of the interpreter run with `--st-fdc-speed 8`: caption and title from ~1000; the first fire (at 1400) leaves the title; the SELECT menu (1 PLAYER GAME, 2 PLAYER GAME, MUSIC)
is up from ~1550; a fire at 1650 starts the game; level 1 is loaded at ~1760 and waits at GET READY until a fire; the ship is controllable from ~2200. A direction is taken
only when held about 12 frames (the game polls the joystick once per logic step, every third VBL), so menu and shop moves hold 12. The game reads **joystick port 1** only.

The scripts are built from a handful of blocks: `front_end` (fires at 1400, 1650, then one every 50 frames until 2150), `play` (autofire taps while sweeping), `invincible`
(`$CC6 = $27`, `$CCA = 3` held), `goto_level` (`$E04 = N-1`, `$A07 = $FF`: the game's own end-of-level path loads level N), `fast_forward` (below), `scroll_to`/`flood_to`
(one-step scroll jumps; `flood_to` leaves the event cursors so every skipped event fires at once), `shop`/`shop_leave`, `aim`.

Pokes used (the variables are the disk build's, at the same addresses, `atari-st-hooks.md`): `$E04` level, `$A07` level-complete, `$A6C` shop request, `$CD0`/`$CEA`/`$CE8` scroll
position and lower clamp, `$CD2`/`$CD4` event cursors, `$CC6` energy (full `$27`), `$CCA` ships, `$CF0` credits, `$C92.l` score, `$C9A.l` cash, `$4F2`/`$4F4` shop cursor,
`$B4C` head of the hittable-object list (`pokeobj`: next at +`$12`, hit handler at +`$A`, HP at +`$32`), `$D7A` guardians left, `$28A0`/`$28A4` the random generator.
Every poked script says so in its header; the three that are not poked (`attract`, `level1_play`, `menu_2p`) are plain input.

### Fast-forward, and what made it fault

`fast_forward` moves the scroll position down in small steps while the ship is held at the top of the screen (**8 units every 6 frames, about 4.5x the game's own speed**; level 3
needs 9 frames, see below). Larger jumps, and any poke of `$CCE` (the engine derives it itself on the ST; it is not a copy of the position), made the scroll update compute a negative
speed (`$CDA`), the sub-tile row `$CD6` left 0-15, and the tile routine at `$1E90` looped over unmapped memory: 18 million unmapped accesses on levels 1, 4 and 5. The ship routine
at `$6AFC` derives the speed from the ship's history (`$47C`-`$4E2`) and its y, so a ship low on the screen turns every jump into a backwards scroll; a ship at the top and small steps
keep it positive. The fault is a poke artefact (the game itself never scrolls backwards), found empirically, and the generator now never produces it: all 22 replays have 0 unmapped accesses.
The lower clamp `$CE8` is lowered with the position and set to 0 when the fast-forward ends. Level 3 froze with the object list a cycle (139 objects, the pool exhausted by a flood
of events) when advanced every 6 frames: the same pool-exhaustion hang the disk and CDTV builds' notes describe; every 9 frames is healthy. Natural play of level 2 stalls at
`$F7A` (the scroll stops there for good; our ship sits at the bottom): **Hatari's scroll stalls at the same position with the same inputs**, so it is the game, not the runtime.

## 2. Boss and shop maps

Handlers are the ST's (level code loaded at `$4F000`; found by object type and HP on the list at `$B4C`, then checked by behaviour):

| Level | Objects | Hit handler | HP | Replay result |
|---|---|---|---|---|
| 1 | nautilus (type `$50`) | `$50396` | `$1E` | **defeated** (`boss1`): the weak point takes shots at x `$98`-`$A7` against the ship's hit box (`$460`/`$464`); HP held at 1; the death code at `$503DA`-`$50432` sets `$A07` |
| 2 | spider; 3 parts | `$50A16`; `$50B00` | `$4B`; `$C` | arena reached, alive with HP 1, **not defeated** (`boss2`); death code: the spider's routine at `$50A26`-`$50A80` spawns the explosion (`jsr $E72`) and runs **`st.b $A07` at `$50A7C`** (no other flag) |
| 3 | boss (type `$50`); 2 helpers | `$4F800`; `$4FD76` | 2; `$A` | arena reached, HP 1, shots never connect (`boss3`, `level3_ff`); **level complete reached** through its two death flags (`boss3_done`) |
| 4 | head; eyes; structure and parts | `$4FB76`; `$4FBBC`; `$504EE`, `$50464`, `$50498`, `$504EA` | `$64`; `$32`; `$AF`, `$14`, `$14`, `$32` | arena reached (`level4_ff`, `boss4`), parts held at HP 1, **not defeated**; death code: the head's routine ends in `jsr $E72` and **`st.b $A07` at `$4FBB2`** (eyes `$4FBBC` only change their move routine) |
| 5 | 22 guardians; the core's target | `$50360`; `$E48` (resident) | held at 1 | `boss5`: arena, guardians shot down to 14 left; the **ending** through `ending_natural` (guardians, core, level-complete, shop capsules, ending, +1 credit, back to level 1) |

**Death flags of the bosses (found by disassembly of the level code at `$4F000`, checked in the replays where a script can reach them)**:

| Boss | What sets the level-complete flag `$A07` | Checked |
|---|---|---|
| 1 | the nautilus' death code `$503DA`-`$50432`, `st.b $A07` at `$5042E` | `boss1`: HP held at 1, one shot, `$A07` set, score 12150 in the emulator and in Hatari |
| 2 | the spider's hit handler `$50A16` (HP `$4B`) on HP 0 runs `$50A26`..., `st.b $A07` at `$50A7C`; its three parts (`$50B00`, HP `$C`) only explode | static only: no shot reaches the spider (below) |
| 3 | the parts (`$4F800`, HP counters at `$D66 + 2*index`) set the bytes **`$D92`** and **`$D93`** when they die; the level routine `$4F3E0`-`$4F41E` (every frame while `$CD0` is below its limit) clears `$D94`/`$D95`, and when both flags are non-zero and `$A07` is still 0 calls `jsr $E6C` (d7 = 9, the boss explosion) and does `st.b $A07` at `$4F41E` | **`boss3_done`**: the two bytes poked to `$FF` in the arena, `$A07` is set 3 frames later, the end-of-level shop request (`$A6C`) follows 540 frames after that (as after boss 1), leaving the shop loads level 4 (`$E04` = 4); the run is deterministic |
| 4 | the head's hit handler `$4FB76` (HP `$64`) on HP 0 runs `jsr $E72` and `st.b $A07` at `$4FBB2`; the eyes (`$4FBBC`, HP `$32`) and the structure (`$504EE`, `$50464`, `$50498`, `$504EA`) only change their own routines | static only |

So bosses 2 and 4 are the same shape as boss 1 (one object whose death handler sets `$A07`), boss 3 is the only one with a separate flag pair. A script can only set the flags (boss 3);
for bosses 2 and 4 the HP poke is not enough because no shot ever reaches them in the scripted fights (see section 6), so their death path is mapped but not executed.
Level 1's mid-level shop request comes at `$CD0` = `$9BF` with the ship at y `$B0` (the disk builds' rule `$CD0 + y < $A80`); level 5's at about `$638`
(where `level5_ff` holds the scroll); no request was seen on levels 2-4 in any run.

Shop sequence (verified in `shop`, `shop5`, `boss1`, `ending_natural`, `level5_ff`): the request opens the shop and input is read ~500-600 frames later. Cursor `$4F2` column
0-4, `$4F4` row 0-4; the SELL phase has nothing to sell; fire on EXIT (column 0, row 4) moves to the BUY phase (cursor back to (0,0)); in the BUY phase a fire on a cell buys
it (cash `$C9A` drops by the price, one purchase per ~100 frames; the cursor needs 12 frames per move); fire on EXIT leaves. The end-of-level shop after boss 1 takes five exit
presses 150 frames apart before level 2 loads.

## 3. What each replay reaches, measured

`python3 tools/trace/st_coverage.py --exe build/x2recomp --disk kixx.st --twice` dumps RAM every 100 frames and reads the engine variables and the object list with `st_probe.py`'s
helpers. It says what was **seen** at those frames (a state shorter than 100 frames can be missed); `deterministic` = the same hashes in two runs, equal to the committed manifest.
"Level loads" = runs of side-1 reads of tracks 0-13 (a level 1 load at ~1760 counts); "bosses" are labelled only inside their own level (the code at `$4F000` differs per level) and
include the level 1 nautilus, which spawns with level 1.

| replay | frames | s | level loads | levels seen | bosses seen (lowest HP) | level done | shop | min energy | min ships | credits | max score | deterministic |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| attract | 5000 | 1.0 | 0 | - | - | - | - | 0 | 0 | 0-0 | 0 | yes |
| boss1 | 10187 | 2.0 | 2 | 1,2 | L1 nautilus (1), L2 spider (4B), L2 parts (C) | yes | yes | 13 | 3 | 0-3 | 16850 | yes |
| boss2 | 9152 | 1.9 | 2 | 2 | L1 nautilus (1E), L2 spider (1), L2 parts (1) | - | - | 27 | 3 | 0-3 | 34000 | yes |
| boss3 | 10643 | 2.2 | 1 | 3 | L1 nautilus (1E), L3 boss (1), L3 helpers (1) | - | - | 1F | 3 | 0-3 | 19000 | yes |
| boss3_done | 13143 | 2.8 | 1 | 3,4 | L1 nautilus (1E), L3 boss (1), L3 helpers (1) | yes | yes | 1F | 3 | 0-3 | 19900 | yes |
| boss4 | 9152 | 1.9 | 1 | 4 | L1 nautilus (1E), L4 structure (1) | - | - | 1F | 3 | 0-3 | 46750 | yes |
| boss5 | 4570 | 0.9 | 1 | 5 | L1 nautilus (1E), L5 core target (1), L5 guardians (1) | - | - | 27 | 3 | 0-3 | 1250 | yes |
| ending_natural | 7720 | 1.6 | 2 | 1,5 | L1 nautilus (1E), L5 core target (1), L5 guardians (1) | yes | yes | 27 | 3 | 0-4 | 2300 | yes |
| game_over | 5765 | 1.1 | 1 | 1 | L1 nautilus (1E) | - | - | 1 | 0 | 0-3 | 2000000 | yes |
| level1_ff | 7116 | 1.5 | 1 | 1 | L1 nautilus (1E) | - | yes | 1F | 3 | 0-3 | 15720 | yes |
| level1_flood | 6400 | 1.3 | 1 | 1 | L1 nautilus (1E) | - | yes | 27 | 3 | 0-3 | 28770 | yes |
| level1_play | 6000 | 1.2 | 1 | 1 | L1 nautilus (1E) | - | - | 7 | 0 | 0-3 | 5670 | yes |
| level2_ff | 7440 | 1.5 | 2 | 2 | L1 nautilus (1E), L2 spider (4B), L2 parts (C) | - | - | 27 | 3 | 0-3 | 33250 | yes |
| level2_flood | 6400 | 1.4 | 2 | 2 | L1 nautilus (1E), L2 spider (4B), L2 parts (C) | - | - | 17 | 3 | 0-3 | 46050 | yes |
| level3_ff | 9075 | 1.9 | 1 | 3 | L1 nautilus (1E), L3 boss (2), L3 helpers (A) | - | - | 1F | 3 | 0-3 | 18100 | yes |
| level4_ff | 7440 | 1.6 | 1 | 4 | L1 nautilus (1E), L4 head (64), L4 eyes (32), L4 structure (AF) | - | - | 1F | 3 | 0-3 | 38400 | yes |
| level4_flood | 6400 | 1.4 | 1 | 4 | L1 nautilus (1E), L4 structure (AF) | - | - | 1F | 3 | 0-3 | 34200 | yes |
| level5_ff | 9416 | 2.0 | 1 | 5 | L1 nautilus (1E), L5 core target (0) | - | yes | 1F | 3 | 0-3 | 15900 | yes |
| menu_2p | 5950 | 1.2 | 1 | 1 | L1 nautilus (1E) | - | - | 0 | 2 | 0-3 | 2100 | yes |
| shop | 6400 | 1.3 | 1 | 1 | L1 nautilus (1E) | - | yes | 27 | 3 | 0-3 | 900 | yes |
| shop5 | 6400 | 1.3 | 1 | 5 | L1 nautilus (1E), L5 core target (A) | - | yes | 27 | 3 | 0-3 | 0 | yes |
| two_player | 6400 | 1.3 | 1 | 1 | L1 nautilus (1E) | - | - | 0 | 0 | 0-3 | 1650 | yes |

Scripts: `attract` (title, credits, attract loop, no input); `level1_play` (level 1 played, three ships and a credit lost, no pokes); `level1_flood`/`level2_flood`/`level4_flood`
(one-step jumps `$C00,$900,$600,$300`: many enemy types at once; level 3's flood stalls the logic (the step counter stops) and level 5's faults, so they are not in the corpus and not investigated); `level1_ff` (first half, the mid-level shop left
at once, second half); `shop` (level 1 mid-level shop with cash poked: SELL, BUY, eight cells, exit); `shop5` (the level 5 shop); `level2_ff`-`level5_ff` (levels 2-5 loaded through the
game's own path and fast-forwarded to the end; `level5_ff` includes its mid-level shop); `boss1`-`boss5`; `ending_natural` (level 5's end event, guardians and core, the game's
own ending, level 1 again with the extra credit); `two_player` (energy 1 so deaths hand over between players, continues); `game_over` (2,000,000 score poked: death, name entry,
continue prompt, last credit lost, GAME OVER, second entry); `menu_2p` (menu navigation, MUSIC toggle, a 2 player game, pause `P`, SPACE, abort `Esc`, back to the menu, 1 player game).

## 4. Tests

* `replay_st_<name>` (22): `tests/st_replay_check.cmake` runs the script headless on the dump (found by sha1 `323d6859...f3df994c` in `generated/st-dev`), requires 0 unmapped accesses, pins the
  end state (`EXPECT`: level, credits) and compares every frame and audio hash with the manifest. Exit 77 (Skipped) without the dump.
* `st_replay_scripts`: the committed scripts equal the generator's output, every manifest has the right shape and every manifest a script, the script syntax, and the Hatari conversion (9 checks).
* On a translated build (`-DX2_RECOMP=ON`, `recomp_st_<name>`; S7's scenarios are `recomp_st_scn_<name>` because three share names with these replays): interpreter and translated executor agree on the
  framebuffer hash, audio hash, a state digest of all RAM and registers at **every frame**, the frame log and the final RAM, on the 21 scripts of the first set (92.2-96.8% of the executed instructions translated); `boss3_done` was added afterwards and has not been run on a translated build.
  `tools/trace/run_corpus.py --game st` now runs both same-named scripts.
* ASan + UBSan (`-fsanitize=address,undefined`, RelWithDebInfo): `attract`, `level1_play`, `boss1`, `boss3`, `ending_natural`, `game_over`, `two_player`, `menu_2p`, `level5_ff`, `shop5` run
  clean, hashes equal to the manifests.

## 5. Comparison with Hatari

Method (`tools/compare/st_replay_compare.py NAME --disk kixx.st --exe build/x2recomp --out DIR --scene LABEL:FRAME ... --series FROM:TO:STEP [--audio]`): one script, both emulators.
The script is converted for Hatari (`make_st_replays.py --hatari`); the harness (`tools/compare/hatari-harness`, patch `0002`) gained `POKE`/`POKEOBJ` lines so the pokes run there too, and
**`ANCHOR <level>` sections: the inputs after an anchor run relative to the first scroll movement after that level starts**. That is the alignment by game event: Hatari's floppy runs at
real speed (the front end and each level load take 700-1500 frames longer than with `--st-fdc-speed 8`) and its load times vary from run to run (even EmuTOS's boot PCs differ), so
VBL numbers cannot be shared; with anchors the gameplay is repeatable (the pixel series of two Hatari runs are identical). Before the first anchor, events land at VBL t + 436 + a constant and the
GET READY fire chain is lengthened through the load. Pictures: Hatari's 832x552 shot cropped to 320x200 against ours as 3-bit gun indices (`st_compare.py`), the best of +-8 frames
around the aligned frame for scenes and +-3 for the series. Variables: the engine words logged every VBL (Hatari) or on change (ours) at matched events. Audio: `st_audio_compare.py`
(YM output, 100-frame windows, 1.5 s local lag; Hatari recorded in real time).

| Replay | Anchors (ours / Hatari VBL) | Pictures | Variables at matched events | YM audio (active windows: env / spectrum / fine) |
|---|---|---|---|---|
| title and attract loop (`attract`, no input) | none: matched by search | 185 shots, best frame within 14 of a constant offset of 1067: **median 99.88%**, 138 at least 99%, 164 at least 95%; the offset stays within 1055-1080 over 4,000 frames | - | not run (S3 checked the YM against Hatari) |
| level 1 played (`level1_play`) | 2299 / 3504 | scenes 99.30%, 97.64%, 90.06% (frames 2300, 2574, 3000); 73 points: at least 88% to frame 3250, then the runs part (+9 scroll units at 3350); median 53.3%; they match again after each restart | first hit: energy `$1F`, score 50 in both at an offset of 1202; then ships 2, 1, 0 and the credit lost at offsets 1178-1214 (scores 4770 against 4470 by then) | 0.52 / 0.87 / 0.60 (47) |
| mid-level shop with purchases (`shop`) | 2299 / 3201 | scenes 85.45%, 99.09%, 98.36%, 98.19%, 99.97%; 41 points **median 97.6%** | shop request at offset 905; cash 200000, 199800, 198800, 198200 **identical** in both | not run |
| level 1 boss and end-of-level shop (`boss1`) | 2299 / 3239; level 2 not reached in time | scenes 99.97% (mid-level shop), 95.68% (boss arrives), 94.90% (boss death), 99.97% (end shop); 67 points median 90.5% | boss killed in both: `done` at offsets 939, **score 12150 in both** at the death | not run |
| a later level (`level2_ff`) | 2925 / 4457 | first scenes 99.57%, 80.96%; scroll position within +-1 of Hatari's for 3,000 frames (4,300 units of fast-forward), score 5500 in both at frame 4000; the fights part later (median 51.8%) | scores 300 and 300 at the first kill | not run |
| level 2 boss arena (`boss2`) | 2925 / 4623 (shift 2200:800, `--chain-extend 800`) | entry scene 47.11% (the runs are a few scroll units apart while the picture scrolls); arena **99.63%**, 8700 **99.66%**; 31 points 5800-8800 median 96.1%, **100% at every point from 7600 to 8800** (1,200 frames; Hatari's scroll is +8..+10 units ahead throughout) | score 300 and 300 at the first kill; at the arena 34000 / 33600, cash 700 / 900 | not run |
| game over (`game_over`) | 2299 / 3227 | scenes at death, name entry, continue, new game, last life, second entry and GAME OVER **99.85-100%**; 35 points **median 99.9%**, 30 at least 99% | ships, credits and scores identical at every event (death, credit lost, continue, last credit, game over), a constant offset of 928-934 over 1,500 frames | **0.91 / 0.99 / 0.78** (38) |
| level 5 end, the ending and the wrap (`ending_natural`) | level 5: 2931 / 4648 | scenes 98.92%, 98.16%, 97.83%, 96.41%, 95.51%, the ending screens **99.86%, 99.87%**; 27 points median 96.4% | same events at a constant offset of 1730: shop request, **credits 3 to 4** (the ending's extra credit), level 1 again; scores 200 against 450 (a different kill early on) | 0.34 / 0.30 / 0.16 (40) |
| menu, 2 players, pause, abort (`menu_2p`) | 2574 / 3563 | 34 points median 98.2%; the menu, abort and restart screens 99.85-99.88% | - | not run |

How to read it:

* **Where the two runs are in the same state the pictures are the same**: the static screens (menu, shop, name entry, continue, GAME OVER, the ending) match at 99.8-100%, the first
  ~1,000 frames of a played level at 88-99%, and the scroll position agrees to within a few units over thousands of frames of fast-forward. Game events (hits, deaths, credits, the
  ending, a boss death) happen at constant offsets with the same variable values, which is the strongest check that the engine, the video timing and the input path agree.
* **Play parts the runs once something random or timing-dependent differs**: `level1_play` separates after ~1,000 frames of play (a ship hit lands differently; at 3350 the scroll
  positions differ by 9 units). The random generator is **not** the cause: it is also called in the loader's busy loops, so its state differs at matched frames, but seeding both runs to the
  same value at the first scroll movement (`--seed-rng 2310`) leaves the whole series unchanged. A diff of the two RAMs at aligned frames is dominated by the logic-step phase (ship
  coordinates at one instant) and did not isolate a first cause; whether the parting is in our timing model or in Hatari's is **not settled**. The 4-VBL step share (2.77% against Hatari's
  2.87%, `atari-st-video.md`) is the known small difference.
* **The 1.3% front-end drift**: `atari-st-video.md` measured the front end's animation running 1.3% fast. In the attract loop the offset to Hatari moves by at most 25 frames in 4,000 (and
  constant to 5 frames over the stretch 1,700-5,800), and the in-game events of `game_over` hold a constant offset to within 6 frames over 1,500 frames: the drift is not visible at this resolution after the loads.
* **Audio**: where the events line up (`game_over`) the YM output correlates at 0.91 (20 ms envelope) and 0.99 (spectrum); in `level1_play` the music matches (0.87 spectrum) and the effects
  follow the diverged fight (0.52 envelope); in the level 5 fights and the ending 0.34/0.30, where the two runs shoot different things. Mono-music-only stretches were not separated.

## 6. Bugs and findings

* **No runtime fault was found**: 0 unmapped accesses, deterministic hashes, a running logic-step counter (`st_probe.py` prints steps per 100 frames; about 33 when healthy) in all 22 replays,
  clean under ASan and UBSan, identical on the translated executor. The faults met while building the corpus were poke artefacts (section 1).
* Tooling: `recomp_st_*` test names and the corpus's trace names collided between `tests/replays/st` and S7's `tests/recomp/st_scenarios` (`attract`, `level1_play`, `game_over`); the scenarios now
  carry a `scn_` prefix in both. Hatari hangs at exit after a real-time recording, so the comparison driver ends it once the last VBL is logged.
* **The level 2 arena and the stalls are the game, not the runtime** (checked against Hatari, 2026-11): in `boss2` the ship stops answering the stick at scroll position `$1A2` (ours; Hatari `$1AB`-`$1B3`) and the
  spider never comes alive; Hatari's picture is **identical** for 1,200 frames of the same inputs (100% from frame 7600 to 8800, the scroll offset a constant +8..+10 units). At the stall the engine is in the
  push-back state: `$42E` (the push-back counter) and `$CDA` (the scroll step) both flip between 1 and `$FFFF` every ~9 frames, so the scroll jitters by about 3 units around one position, `$CEA` (the scroll clamp)
  follows it at +`$10`, the ship's coordinates (`$460`/`$462` = `$8C`/`$8`, the top edge) do not change although `$A00` shows the stick, and nothing else moves. The spider's logic (move routine `$51086`) only runs
  while `$CD0` is below `$160`, so with the scroll held at `$1A2` it never starts; its body sits at x `$E5E5` and its three parts at the bottom left, off screen, so no shot can reach them. That also explains the levels
  3 and 4 bosses "not dying under swept fire": the shots have nothing to hit. A natural play of level 2 (`nat2`: no fast-forward, autofire, the stick swept; energy and ships still poked) stalls the same way
  further on (ours at `$AE8` after 9,180 frames with 24 chained objects (types `$114`-`$11C`, hit handlers `$4FFB4`/`$4FFD6`/`$4FFF0`, HP 2, x/y trailing each other) above the screen at y `$FF5C`-`$FFA3`;
  Hatari at `$F78`-`$F86`, the position found earlier at `$F7A`): both emulators stall, at different places because the runs part (a different shot count: score 27550 against 9850 at the same VBL offset).
  **Not established**: why the push-back never ends. The energy and ships pokes keep the ship alive, so the push-back sequence that would end in the death routine (`$6AF0` -> `$6796`) may be what is being held
  open; whether a hand-played level 2 reaches the arena without any poke is listed in `docs/re-notes/st-hand-play-needed.md`.
* A shot-path read-through (collision loop `$3868`/`$3888` over the `$B4C` list with the box words at +`$36`..+`$3C`, the shot hit routine `$38B0` called from `$4D36`, hit dispatch `$6386`) found nothing that depends on
  the emulator; experiments that gave the spider and its parts a full-screen hit box (`pokeobj ... $36..$3C`) and `$C90` set (the ram path) made the ship take damage but never ran a boss handler, as expected with the spider's
  logic not started. The experiment scripts are not committed.

## 7. Not covered

Hand play of any kind; the bosses of levels 2 and 4 defeated by shots (their death code is mapped but not executed; boss 3's level-complete path is run through its poked flags in `boss3_done`); level 5's hand-fought end (the replays poke the guardians' HP and, in S5's `ending_l5`, `$D7A`);
the mid-level shop triggers of levels 2-4 (none seen) and level 5's exact position; a second player's progress through the levels; the Hatari comparison of level 3 and 4, floods and `shop5`;
real-time audio of the title and the front end (S3's YM check stands).

## 8. Reproduce

```
python3 tools/trace/make_st_replays.py                                   # the 22 scripts
python3 tools/trace/make_st_replays.py --record --exe build/x2recomp --disk kixx.st   # the manifests
python3 tools/trace/st_coverage.py --exe build/x2recomp --disk kixx.st --twice         # the table above
tools/compare/hatari-harness/build.sh                                    # Hatari 2.6.1 + EmuTOS, patches 0001 and 0002 (local, not committed)
python3 tools/compare/st_replay_compare.py game_over --disk kixx.st --exe build/x2recomp --generated generated --out /tmp/cmp \
        --shift 2200:900 --scene dying:2700 --scene gameover:5500 --series 2300:5700:100 --audio
python3 tools/compare/st_replay_compare.py boss2 --disk kixx.st --exe build/x2recomp --generated generated --out /tmp/cmp \
        --shift 2200:800 --chain-extend 800 --scene arena:7900 --series 5800:8800:100   # Hatari needs ~5 minutes (the driver's 300 s cap)
```

For a replay that fast-forwards through level 1 the first `--shift` must put the first poke just after Hatari's level start (its load time varies run to run, 3,000-3,400 VBLs here); with 2200:600 the energy poke
lands before the level starts and the level-2 jump is lost, with 2200:1000 the ship is shot down first. `--chain-extend 800` keeps the GET READY fire chain going through the slower level 2 load.
