# CDTV reference ground truth from the WHDLoad slave route (phase C5)

Phase C5 of `.omc/plans/xenon2-cdtv-target-plan.md`. `cdtv-reference.md` got the CDTV disc to boot in Amiberry but left three
questions open after C3 (`cdtv-boot.md`): (a) the Amiberry CDTV run showed level 1 as dotted vertical streaks on black where our
runtime shows scenery; (b) the "language page" shows noise in most item cells in our run; (c) independent frame and RAM evidence
for the replay goldens was missing. This note answers them with a second, independent reference: the game run under the WHDLoad
CDTV slave (`Xenon2CDTV.Slave` 1.6 by Wepl) on an A500-class machine, which does not use the CDTV firmware or the CD drive at all.
No game bytes, ROM bytes, pictures or audio are in the repository; captures stay in git-ignored `generated/`.

## 1. Short answer

| Question | Answer |
|---|---|
| Does the WHDLoad route work in the harness? | **Yes.** Kickstart 2.04, ECS Agnus, 1 MB chip + 1 MB fast, a host directory as `DH0:`; the game reaches the joypad page at about frame 480, the menus and level 1 with the same input script as the CD route. Deterministic: two runs give the same picture hashes. |
| (a) Amiberry's level-1 streaks? | **An Amiberry configuration error, not the game.** Changing only `chipset=ocs` to `chipset=ecs_agnus` in the CDTV config makes the CD-based run show the real scenery. The most likely mechanism (inferred, section 5) is the OCS Agnus's 19-bit DMA pointers wrapping at 512 KB while the game's display buffers sit in the second hunk above `$80000`. It also supports the 20-bit pointers of our 1 MB model (C1). |
| (b) Noise in the item cells? | **The game's own effect.** The same pages in the WHDLoad run have the same noise cells, 94% of the pixel differences on those pages lie inside the cell area, and the rest of the page matches. |
| (c) Independent evidence for goldens | **Yes**: pictures (section 3), game variables and 1 MB chip RAM dumps (section 4) at matched events agree. Audio is not compared here (section 6). |
| Runtime bugs found | **None.** |

## 2. Method

**Machine and files.** `tools/compare/amiberry-harness/x2ref-cdtv-whd.uae.in` (A500+-class, ECS Agnus, `chipmem_size=2` = 1 MB,
`fastmem_size=1` = 1 MB fast RAM, PAL, cycle-exact CPU, memory and blitter) with `run-cdtv-whd.sh`; `prep-cdtv-whd.sh` lays out the
host directory (WHDLoad + the install + a two-line `S/startup-sequence`). All inputs are user-supplied: a Kickstart 2.04 ROM, the
WHDLoad program (the project's user package), and the WHDLoad install of Xenon 2 CDTV (slave and the disc's files as plain files).

Two findings about the setup: the current WHDLoad program (20.x) does not load under Kickstart 1.3 (AmigaDOS reports "not an
executable"; the program uses extended hunk flags 1.3 does not know), so the reference machine boots 2.04; WHDLoad takes over the
machine anyway, so the Kickstart version does not change what the game sees. And `WHDLoad` finds the game's `data` folder relative
to the current directory, hence the `cd` in the startup-sequence. The slave's tooltypes/arguments used: `PAL NOREQ`.

**Inputs.** The same fire-every-150-frames menu script as the CD route (`inputs/cdtv-menus-to-level1.txt`, port-2 fire). Our
runtime ran the same presses shifted 460 frames earlier (its start-up is shorter by about that much). Frame numbers differ between the two timelines, so frames are paired by picture, not by number.

**Our side.** `x2recomp --headless --game cdtv --assets <scratch dir> --input-script <shifted script> --screenshot-at F:FILE ...`
(a scratch assets folder under `generated/`; the user's own imports are never touched), plus `--dump-ram-at`.

**Comparison.** `tools/compare/cdtv_whd_compare.py` (numpy; a development tool): finds where our 320x256 picture sits in
Amiberry's 377x288 buffer by cross-correlation (x=37, y=18, the same on every page), pairs each reference shot with the runtime shot
that looks most like it (thumbnail search, then exact comparison of the top candidates) and reports exact pixel agreement and the
colour sets. RAM: byte comparison of the chip RAM dumps (1,048,576 bytes each) at matched events.

## 3. Pictures

Reference shots every 10 frames from frame 480 to 5400 (491 shots) against our shots every 10 frames:

| | |
|---|---|
| Exact pixel agreement, median over all shots | **99.89%** |
| Shots at >= 99.9% / >= 99% / >= 95% / >= 90% | 153 (31%) / 333 (68%) / 364 (74%) / 454 (92%) |
| Shots whose colour set equals ours exactly | 444 of 491 (90%); the rest are fades and transitions between pages (up to 12 colours differ for a frame or two) |

By page, the median agreement is 99.9% on the joypad page and the title pages (reference frames up to about 1200; minimum 96.9%),
91-96% on the shop page with the noise cells (94% of the differing pixels lie in the item-cell area; the rest differ by about 420
pixels per shot, which is the starfield and other small changes), and 99.9% in level 1.

**Level 1, every frame.** Per-frame shots of a 131-frame window of level 1 on both sides: **131 of 131** reference frames match a
runtime frame at >= 99.87% (median 99.93%, 98 frames at >= 99.9%). The HUD band (the bottom 30 picture lines) is **100%
identical** in every frame. The frame lag between the timelines is 607-609 frames, the three values appearing because the game
swaps its picture every third vertical blank. The enemies are in the same positions, which also says the random sequence is the same.
The 0.07-0.13% that differs is the twinkling starfield.

**Amiberry CD route with an ECS Agnus.** The CD-based CDTV run (the `cdtv-reference.md` setup, ROM v2.35) with only
`chipset=ecs_agnus` changed shows the same level 1 as the WHDLoad run and ours (four of its five shots match one of ours at
>= 99.88%; the fifth falls between our 10-frame samples), and runs on to the high-score table after the ship's death.

## 4. Game variables and 1 MB of chip RAM

Chip RAM dumps at two matched level-1 moments (reference frame 3610 against our frame 3000, 3700 against 3090; the game's own
frame counter at `$CE2` is equal in both pairs: `$7D` and `$98`):

| Variable | Address | Reference | Ours |
|---|---|---|---|
| level | `$E0A` | 1 | 1 |
| energy | `$CCC` | `$27` | `$27` |
| ships | `$CD0` | 2 | 2 |
| credits | `$CF6` | 3 | 3 |
| PAL word | `$412` | 3 | 3 |
| special weapons | `$CA6` | 0 | 0 |
| frame counter | `$CE2` | `$7D` / `$98` | `$7D` / `$98` |

| Region | Differing bytes (first / second moment) |
|---|---|
| code and data hunk `$400`-`$6B09C` | 504 of 437,404 (0.115%) / 257 (0.059%) |
| rest below 512 KB `$6B09C`-`$80000` | 681 of 85,860 (0.79%) / 295 (0.34%) |
| above 512 KB `$80000`-`$C5E5C` (82.7% non-zero: buffers, sound) | 306 of 286,300 (0.11%) / 242 (0.08%) |

The differences are small clusters (counters and state words, the largest 52 bytes). The upper region matching to 0.1% is direct
evidence that the 1 MB model fetches and writes the same buffers the real machine does. The reference's link base equals ours:
the WHDLoad slave loads the program at `$400` with the second hunk at `$6B09C`, and 99.9% of the bytes of the code and data region are equal.

**A WHDLoad quirk.** In the WHDLoad run the watched variable words at `$E0A`, `$CCC` and `$CD0` show code-looking values for
about seven frames around each file load (`$5FC8`, `$F9F0`, `$33F0`, for example) and then return to their values. This happens
only during loads under the slave and does not occur in ours (our watch log of `$E0A` holds only 0 and 1); nothing in the program's own results depends on it (the pictures
and the dumps at settled moments agree). It is recorded for completeness, not a finding about our runtime.

## 5. Question (a): the streaks, in detail

The streak run used `chipset=ocs` with 1 MB chip RAM. The second hunk (372,160 bytes of BSS) sits at `$6B09C-$C5E5C`, so the
display buffers are above `$80000`. The likely mechanism (inferred from the experiment, not traced in Amiberry's source) is that
its OCS Agnus masks DMA pointers to 19 bits, so bitplane DMA fetches wrap into the first 512 KB and the playfield shows whatever is
there, as vertical streaks. The CDTV's own Agnus is the 1 MB ECS part. Changing the one line fixes it; `x2ref-cdtv.uae.in` now carries `chipset=ecs_agnus` with the reason. The earlier "frames differ between
ROMs at the same frame number" caveat in `cdtv-reference.md` still stands (boot time differs between firmware versions).

## 6. What is and is not covered; open items

- **Covered:** boot flow and page sequence; every front-end screen up to level 1 and level 1 itself, picture-exact apart from the
  starfield and the noise cells; the HUD; colours; game variables; the full chip RAM image at two moments; the random sequence
  (equal enemy positions) up to the shop-to-level-1 point; a CD-based second route that agrees with the slave route.
- **Not covered here:** audio. The WHDLoad slave plays no CD music and the CD route mixes CD audio with Paula in one tap, so a
  Paula-only comparison needs C4's audio work (event-aligned effects against our `--audio-cues` log).
- **Not covered:** levels 2-5, bosses, shop interaction, speech playback, two-player and the ending (C6 builds those replays; this
  reference can be re-used by feeding the same scripts to both sides).
- **Sampling:** the scripted run uses fire presses only, so the ship is idle: moving, shooting and damage are not compared yet.
- **Different timelines:** pictures were paired by looks, not by a shared clock. A shared clock would need the disk harness's
  `WAITLOAD` idea (hold game time through loads) and the CDTV program's own frame counter at `$CE2`; the dumps above show it
  agrees at matched moments.

## 7. Reproducing

```
# 1. lay out the host directory (all inputs user-supplied)
WHDLOAD=<WHDLoad executable> INSTALL=<Xenon2CDTV install folder> tools/compare/amiberry-harness/prep-cdtv-whd.sh <dh0 dir>
# 2. reference run (Kickstart 2.04, headless): shots every 10 frames, RAM dumps at chosen frames
KICK=<ks204.rom> DH0=<dh0 dir> X2REF_WATCH="412,CE2,E0A,CCC,CD0,CF6,CA6,404" \
  tools/compare/amiberry-harness/run-cdtv-whd.sh <out dir> 5400 "480-5400/10" tools/compare/amiberry-harness/inputs/cdtv-menus-to-level1.txt "3000,4000,4800"
# 3. our side: the same presses shifted 460 frames earlier, shots every 10 frames
x2recomp --headless --game cdtv --assets <scratch assets dir> --input-script <shifted script> --screenshot-at F:<dir>/oFFFFF.png ...
# 4. compare
python3 tools/compare/cdtv_whd_compare.py <out dir> <our shots dir> --json <result.json> --first 500
```
