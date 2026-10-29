# CDTV music map: which CD track the CDTV game plays where the disk game plays its tune

Phase 8 (optional, off by default). Runtime: `runtime/cdtv/` (`cdtv_music.h` cue layer, `cd_player.h` playback,
`cdtv_disc.h` import). Sources:

- the disk version's driver calls (`--audio-cues` over every replay in `tests/replays/`, with `--watch-log` of
  `$E04`, `$9FC`, `$A0A`, `$CDC`; `docs/re-notes/audio-notes.md`);
- a linear disassembly of the CDTV `X2` code hunk (hunk offsets below; the file offset is +36), made with
  `tools/dis68k.py`, focusing on every caller of `play_track` `$3A524` and stop `$3A5B4`.

Confidence: H = read in both programs' code and seen at run time in the disk game; M = read in code, the
meaning is inferred; L = guess.

## 1. The two programs side by side

| Moment | Disk game (call, caller return) | CDTV game | CD track (default) | Conf. |
|---|---|---|---|---|
| Title sequence | `music_init` from `$8230` (ret `$8236`), frame 2 | `$3C418`/`$3B552`: `play_track(table[0])` | **2** "Megablast (Xenon II Mix)" | H |
| Leaving the title (fire) | `music_stop` from `$8672`/`$8680` | stop `$3A89E`/`$3A926` | stop | H |
| SELECT menu, disk load | silent apart from effects | its own menus (see 3) | none | H |
| Game start after GET READY | `music_init` from `$7896` (ret `$789C`), gated by `$9FC` | `$3A966`/`$3A9B2`: `play_track(table[$A0A])`, gated by `$602` | level n track | H |
| Level 1 | `$E04` = 1 | `$A0A` = 1 | **3** "Megablast (Instrumental)" | H |
| Level 2 | `$E04` = 2 (`$7D8C`, ret `$7D92`, after the level change) | | **5** "Megablast (Hip Hop on Precinct 13)" | H |
| Level 3 | `$E04` = 3 | | **2** "Megablast (Xenon II Mix)" | H |
| Level 4 | `$E04` = 4 | | **3** "Megablast (Instrumental)" | H |
| Level 5 | `$E04` = 5 | | **5** "Megablast (Hip Hop on Precinct 13)" | H |
| End of level / before the shop | `music_stop` from `$7B8C` (ret `$7B92`), then `$9000` (ret `$9006`) at the level load | `$3ADD2` stop, `$3350C`/`$3CE80` stop at the level load | stop | H (disk), M (CDTV meaning of `$672`) |
| Shop (mid-level and end-of-level) | the slot-6 module's own driver (`$2090`=`$6477C`, `$2094`=`$647A6`) plays its own music and effects | no `play_track` anywhere in the shop path | none: the module's sound stays | M |
| Back in the level after the shop | `music_init` from `$7C50` (ret `$7C56`) | `$3AED2`/`$3AF82`/`$3B056`: `play_track(table[$A0A])`, gated by `$602` | same level track, from the start | H |
| Boss | no music change (only `$19FE4` from the level script `$6D22`, which ends running effects) | no `play_track` | level track continues | H |
| Game over, high-score entry, continue | no music change until the title restarts | no `play_track` | level track continues, then title | H (disk), M (CDTV) |
| Ending (slot 6 module) | module driver; afterwards `music_init` from `$7C50` | as the shop | none during the module | M |
| Game pause (P) | music keeps playing (the VBL tick runs in the pause loop `$79BA-$79C8`) | not changed by the pause | keeps playing by default; `--cd-pause duck` or `stop` | H (disk) |
| Music OFF in the menu | `$9FC` = 0: no in-game `music_init` | `$602` = 0: no in-game `play_track` (`$3A9F4`: stop) | none | H |

`table` is the six bytes at CDTV `$3EE26`: `02 03 05 02 03 05` = TITLES, LEVEL 1..5. The CDTV "SELECT TITLES"
screen (`$3DFE0`) lets the player change each slot to any of tracks 2-13; `--cdtv-tracks T,L1,L2,L3,L4,L5`
(settings key `cdtv_tracks`) does the same here.

The level number: the disk game's `$E04` holds 1-5 at every in-game `music_init` (seen in all replays: boss1,
level2..5, shop, shop5, ending). The cue layer reads it at the call; any other value falls back to level 1.

Tracks 4, 6 and 7 are only reachable through the CDTV music-select screen; tracks 8-13 only through the CDTV
info screens (section 3). By default the disk game therefore uses tracks 2, 3 and 5.

## 2. Measured cue sequence (`--music cdtv --cdtv-log`, `tests/cdtv/golden/`)

`boss1` (title, level 1, mid-level shop, level 1 again, boss, end-of-level shop, level 2):

```
frame line event track caller
2     159  title 2     008236
461   304  stop  0     008678
974   273  level 3     00789C
1044  259  stop  0     007B92
1060  8    stop  0     009006
1982  11   level 3     007C56
3351  259  stop  0     007B92
3367  8    stop  0     009006
4884  8    level 5     007D92
```

`level3`, `level4` and `level5` reach their level through the level-change path at frame 1281 (`$7D92`) and pick
tracks 2, 3 and 5. The pause loop gives `pause` and `resume` cues.

## 3. CDTV-only screens (no counterpart in the disk game)

- `$5047E` (16-byte rows `08 08 09 09 0A 0A 0B 0B 0C 0D 0D`, `0C 0C`, `0D 0D`, `0B 0B`, `0A 0A`, `09 09`,
  `08 08`) is indexed by `$3DD76`, which the CDTV main menu (`$3DB10-$3DBB4`) sets to `$00-$60` from the chosen
  entry and which then steps through the picture sequence table `$5040E` (`$505BC-$5065E`). The menu strings
  ("LOOK AT SOME USEFUL INFO ON THE MUSICIANS AND ARTISTS", "SEE WHAT MUSIC'S ON THIS CD") make these the
  info/picture slideshows (PICCODE/PICDATA files). So tracks 8-13 (the other Bomb the Bass songs) play only on
  those screens. Conf. M. The disk game has no such screens, so they are not used.
- The language menu (`$412EA`), the joypad-mode screen and the music-select screen are CDTV-only as well.

## 4. Speech and sound banks (task 4: investigated, not implemented)

- `SOUNDSnn` (10 files) are not extra voices: `$4F8AC-$4F956` loads bank `$A0A` (or `$A0A`+4) into the sample area
  `$1D9AC` at each level start. They are the per-level effect sample banks, which the disk game has in its own
  image and level data. Nothing to add.
- `SPEECHnn` (67 files): the only loader is `$40D56` (name `CD0:SPEECHnn` at `$40C10`/`$40C1E`), called by the
  narrator routine `$40DEC`, which is given a text pointer in `a0` and a clip number in `$40DE8`. Its 20 callers
  (`$3D1A0`-`$3FBB2`) are the CDTV front end: the menu captions (e.g. `$4EEF4` "SELECT SOMETHING, IT DOESN'T
  COST A THING... YET" with clip `$3A`), help and info pages, the language and music-select screens; the title
  loads clip 63 (`$3C3AC`). None of the callers is in the game loop, and the disk game has none of these screens,
  so no disk-game event maps to a clip. The clips are not language-specific (one set of 67 for four languages).
  Conclusion: optional CDTV speech is not implemented; `--lang` is not implemented either, because the disk game
  has no text variants and the only language-dependent CDTV content is the CDTV-only front end (out of scope).

## 5. Music versus effects in the disk game's driver

The resident driver at `$19F9E-$1B6CF` plays both, on shared channels (an effect takes a channel and gives it
back):

- effect code `$1B26A-$1B6CF`: the command routine (called by the VBL through `($2094)` at `$217E` and `$2194`)
  writes one-shot effects straight to Paula (`$1B4DC`, `$1B510`, `$1B530`); command values below `$80` instead
  fill an effect record at `$1B6D0+` that the music tick then plays through `$1B540` -> `$1B5A2`/`$1B5F6`
  (synthesised effects, LC written at `$1B6C0`; the menu "select" sound `$020D` is one);
- music code `$19F9E-$1B269`, and `$1B5D2`, which restores the music's shadow pointer when an effect ends.

So the writer of each `AUDxLC` is decided by the address of the writing instruction (Musashi's `REG_PPC`, which
the translated code maintains exactly as the interpreter). Writes from elsewhere count as music while `($2090)`
is `$19FAC` and as effects while the shop/ending module's own driver is installed. Paula takes the writer over
when the channel latches the pointer (DMA start or buffer reload) and applies per-writer gains to its output mix
only; channel state, DMA, interrupts and timing are untouched.

Evidence that the game is unaffected (`tests/cdtv/cdtv_game.cmake`, boss1, 6000 frames): frame hashes equal the
committed replay hashes; `$1F50`, the RNG words `$1FEE-$1FF4`, `$404`, `$41C`, `$E04` and `$CDC` identical every frame
with and without CD music; the full chip RAM at the end of the replay identical; the cue logs and audio hashes identical under the translated
executor, `--interp-only` and the interpreter-only build.

Waveform checks (numpy on `--audio-out` renders of boss1; nobody has listened):

| Section (frames) | Amiga | effects only (`--music-volume 0`) | `--music cdtv` | `--music mix` |
|---|---|---|---|---|
| title 10-450 | 4019 rms | 0 | 418 (track 2's quiet intro) | 4042 |
| SELECT menu 470-940 | 347 (select sound) | 347 | 347 | 347 |
| shop 1100-1950 | 1922 | 1922 | 1922 (CD stopped) | 1922 |
| level 1 2000-3300 | 3917 | 2904 | 3689 | 4528 |

- `cdtv - effects-only` correlates 0.99996 with the resampled track 2 from the title cue (lag 0), and 0.99997 /
  0.99979 with tracks 3 and 5 after the level cues;
- `mix - cdtv - (amiga - effects-only)` is 0 rms (the mix is exactly additive);
- with `--music cdtv` the title and the first level frames (before any effect) are digital silence apart from
  the CD, i.e. no tracker music leaks through.

## 6. Open questions

1. CDTV `$672` (the flag before the stop at `$3ADD2`) is assumed to be the shop/end-of-level request; not traced.
2. Whether the CDTV shop module plays its own Paula music as the disk one does is not checked (the CDTV
   `LEVELn` files share 84 % of their bytes with disc 2, so probably yes).
3. Real CDTV seek latency before a track starts (up to about a second) is not modelled; the CD starts at the
   Paula output frame of the cue.
