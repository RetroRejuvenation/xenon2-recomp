# Music dropouts: when the music stops, and whether it is the game or us

Question (from playing the Windows build): *"I think sometimes the music stops on occasion, can't remember if
that's authentic or not."*

**Short answer: authentic.** The disk game has one tune, and that tune has long rests in which only AUD1 and
AUD2 play. Those are exactly the two channels its sound effects take, so while the player holds fire the music
becomes inaudible for as long as the rest lasts — up to **14.8 s** in level 1. Every rest, and every handover
of a channel between the music and an effect, appears at the same frame in the Amiberry reference capture of
the same replay (section 4). No emulation bug was found, and nothing was changed in the audio path.

Two things were added anyway, both for the live output and neither affecting the emulation:

- `--audio-latency MS` (settings key `audio_latency`, default 40) so a machine that cannot hold 50 fps can buy
  more headroom, and
- a visible message when the live output's buffer runs dry, so a host-side dropout can no longer be mistaken
  for the game's own silence (section 5).

Sources: the disassembly of the resident image (`generated/lane-b/x2-rt.dis`); music-only renders of the whole
replay corpus (`--sfx-volume 0 --audio-out`, the Phase 8 per-source gains of
[cdtv-music-map.md](cdtv-music-map.md) §5); `--audio-cues`, `--watch-log` and `--pc-log` over those runs; and
the step-aligned Amiberry captures `generated/ref/runs/{s_go,s_tp,s_b1}` ([reference-compare.md](reference-compare.md) §10).

Confidence: H = read in code and seen at run time; M = seen at run time, meaning inferred; L = guess.

## 1. Every way the game can silence or stop the music

Base register of the driver is `a3 = $19F9E`, so an offset `$x(a3)` is at `$19F9E + x`.

### 1.1 The five call sites of `music_stop` (`$19FBA`, core `$1A09A`)

`music_stop` writes DMACON `$000F` (clears all four AUDxEN), ADKCON `$00FF`, **and AUD0-3VOL = 0**, and clears
the playing flag `$5F0` (`$1A58E`) and all four effect records' active flags.

| Caller | Context | Conf. |
|---|---|---|
| `$1A00C` | inside `music_init`, before it builds the tables: every `music_init` logs a stop first | H |
| `$8672`, `$8680` | the two exits from the title loop. `$8680` comes from `tst.b $A01 / bne` at `$82A2` (fire). **The SELECT menu and the disk load are silent apart from effects** | H |
| `$7B8C` | end of level, before the shop (`$7B92` on return) | H |
| `$9000` | the level load (`$8FF2`), right after `$19FE4` | H |

That is all. Over 12,000 frames of level 1 and over the whole corpus there is **no** other stop: see 1.3.

### 1.2 The DMACON audio writes

| Address | Write | When | Effect on the music |
|---|---|---|---|
| `$1A0C2` | `$000F` | `music_stop` | all four voices off |
| `$1A500` | `$000F` | **song command 4** (`$1A4D8`), and the end of the volume countdown of 1.4 | all four voices off, playing flag cleared; the music cannot restart without `music_init` |
| `$1A240` | clear one bit | one tick before the next note on that channel, **unless** an effect owns it (`$B(a4)`) or the next command byte is `$83` | the note is cut one tick early: the normal note-off |
| `$1A2E0` | `$8200 \| bit` | at every note start, **unconditionally** (not guarded by `$B(a4)`) | the voice starts |
| `$1A130` | `$8200 \| $F` | `$19FC8`: re-enable all four. **Never called** (1.3) | — |
| `$1B30E`, `$1B494`, `$1B5B2`, `$1B5C8` | clear one bit | the effect code stopping its channel | that voice is the effect's |
| `$1B4FE`, `$1B5EE`, `$1B6C8` | `$8200 \| bit` | the effect code starting its channel | |

AUDxVOL: the music writes it at `$1A2A2` (note start) and `$1A3DE` (per-tick envelope), both skipped while
`$B(a4)` is set; `music_stop` zeroes all four; **`$1A4D8` does not**. AUDxLEN: `$1A200`/`$1A21E` (the
instrument's loop part, one tick after the note), `$1A29C` (note start), `$1A4B8` (`#$20`, the silent loop).

### 1.3 The paths that exist in the code but never run

Checked with `--pc-log 0-11999:1A4D8,1A55C,1A13C,1A112,1A164:...` over a 12,000-frame level-1 run and over
`boss1`, `level5`, `ending` and `attract`: **zero** hits for all of them, against 3,627 hits of `$1A240`.

- **`$1A4D8`, song command 4** — the "stop the song" byte. The tune does not contain it, and the pattern list
  loops instead: command 0 (`$1A46C`) reads the next entry and, on a `0` terminator, restarts from `$8(a0)`.
  **The tune never ends by itself and never has a restart gap.** H
- **The volume countdown** (`$1A17C-$1A198`). When `$5EB` (`$1A589`) is non-zero, every `$5EB` ticks the word
  `$5E6` (`$1A584`) is decremented, and `$1A4D8` stops everything when it reaches 0. `$5EB` is armed only by
  song command `$0B` (`$1A55C`) or by `$19FF2` (`$1A13C`), and neither ever runs. M
- **`$1A164`, the 1-in-6 tick skip.** When `$5F3` (`$1A591`) is set, every sixth VBL the whole tick — music
  *and* effects — is skipped. The flag is set at `$7684` and cleared at `$769A` by a one-time
  `btst #7,$BFEE01` (CIA-A CRA bit 7, the TOD 50/60 Hz source Kickstart sets), together with `$410` = 4 or 5:
  it is the 60 Hz tempo correction. On this machine the bit is clear, so `$1A591` = 0 and the skip never
  happens. M
- **`$19FC8`/`$1A112`**, the DMA re-enable, has no caller at all. H

`$1A584` is the **master volume** word, not a tempo: `$1A284`/`$1A3CC` read it as `d2` and compute
`vol = ((env * d2) & $FFFF) >> 6` (Paula then uses bits 0-6). `$8236` writes `$3F` and `$789C`/`$7C56`/`$7CE0`/
`$7D92` write `$30` into its **high** byte; both give volume 64 for envelope 64 and 32 for envelope 32, which
is why the measured writes are `$0340` and `$03A0` ([audio-notes.md](audio-notes.md) §4). H

### 1.4 `$19FE4` is "end the running effects", not a music fade

`$19FE4` (`$1A0F4`) sets `$1744/$1768/$178C/$17B0(a3)` = 1. Those are `$1B6E2/$1B706/$1B72A/$1B74E`, i.e.
**`+$12` of each effect record** (`$1B6D0 + n*$24`): the effect's remaining-ticks counter. On the next tick
`$1B5A8` decrements it to 0 and falls into `$1B5AE`, which clears the active flag, stops the channel, clears
`$B(a4)` and — via `$1B5D2` — restores the music's shadow LC/LEN/PER/VOL and re-enables DMA. So `$19FE4`
**cuts every running sound effect short and hands the channels back to the music**. H
(This replaces the "probably a music fade (L)" guess in [audio-notes.md](audio-notes.md) §1.)

Its callers are `$3C6E`, `$6D22` (the level script; in `level1_play` it fires about 48 frames after each death
explosion, i.e. at the respawn) and `$8FFA` (the level load).

### 1.5 Event table

Measured over the whole replay corpus (`--audio-cues` plus `--watch-log` of `$E04 $CC6 $CCA $9FC $A0A $CDC
$414 $418 $CD0`).

| Event | Music behaviour | Evidence |
|---|---|---|
| Title starts | `music_init` from `$8230`, master volume `$3F` | cue `f2 music_init caller=008236` in every replay |
| Fire at the title | **stops** (`music_stop` `$8672`/`$8680`) | `f461 music_stop caller=008678` |
| SELECT menu, disk load | **silent**, effects only | `level1_play` frames 464-974, mix rms 69 |
| GET READY / game start | `music_init` from `$7896`, master volume `$30`; gated by `$9FC` | `f974 music_init caller=00789C` |
| MUSIC = OFF in the menu | **no in-game music at all** (`$9FC` = 0 gates every in-game `music_init`); `$8DAE` is `not.b $9FC` | `menu_2p`: `$09FC`=0000, no `music_init` after f4273, 23.8 s with no music |
| Player death | **continues**; `$19FE4` at the respawn only ends the running effects | `level1_play` 5 deaths, no `music_init`/`music_stop`, `music_api_19fe4 caller=006D28` |
| Boss | **continues** | no cue; `level4_boss` has no music gap at all beyond the tune's rests |
| Pause (P) | **continues** (the VBL tick runs in the pause loop `$79BA-$79C8`) | `menu_2p`, `level3..5` |
| End of level | **stops** (`$7B8C`), then again at the level load (`$9000`) | `shop`: `f1965 music_stop caller=007B92`, `f1981 music_stop caller=009006` |
| Shop and ending (slot 6) | the module installs its own driver (`$2090`=`$6477C`, `$2094`=`$647A6`) and plays its own music and effects | `shop`: `slot6_command` from f2105 |
| Back in the level after the shop | `music_init` from `$7C50` | `boss1` `f1982 caller=007C56` |
| GAME OVER, high-score entry, continue | **continues** (the level tune plays on) until the title restarts | `game_over`: `music_init caller=008236` only at f3871 |
| A sound effect takes a voice | **that voice goes silent until the effect ends and its next note starts**; only AUD1 (`$418`) and AUD2 (`$414`) are ever taken | section 2 |

## 2. Why "the music stops": the effects take the only two voices that are playing

The effect command's **high byte is the channel**: `$1B282` stores `d0 & $3FF` in `$17C2(a3)` = `$1B760`, and
`$1B2C0`/`$1B2E2`/`$1B478` read that byte as the channel index. `$414` is passed as `d0 | $200` and `$418` as
`d0 | $100`, so **effects only ever use AUD2 and AUD1** — the right-hand pair. AUD0 and AUD3 (left) are always
music. H

While `$B(a4)` is set for a channel the music tick keeps its own shadow of LC/LEN/PER/VOL up to date but
writes **nothing** to Paula (`$1A290`, `$1A2C2`, `$1A3A6`, `$1A3D8`), and skips the note-off DMACON clear
(`$1A234`). So a stolen voice stays the effect's until the effect's counter expires.

Measured over 11,026 frames (220 s) of level 1 with no firing at all (`--sfx-volume 0`, left side of the plain
render = pure music):

| Rest | Frames | Offset from `music_init` | Length | Right-pair rms during |
|---|---|---|---|---|
| 1 | 2840-3578 | +1866 | **14.80 s** | 3333 |
| 2 | 5383-5556 | +4409 | 3.49 s | 3080 |
| 3 | 5591-5765 | +4617 | 3.51 s | 3080 |
| 4-10 | 5800-6598 | +4826..+5579 | 0.78-3.49 s | 2021-4420 |
| 11-17 | 7588-9305 | +6614..+8283 | 0.84-3.51 s | 516-3670 |

The left pair rests for **2,474 of 11,026 frames (22 %)**, in 17 rests, the longest 14.8 s. In the same run
**both** sides are silent only twice, for 0.72 s and 0.86 s: those are the only real breaks in the tune.

The 14.8 s rest is at the same offset in every replay (`attract` 1867, `level1_play`/`game_over` 2840,
`level2`-`level5` 3147, `two_player` 2845 — always +1866 frames after `music_init`).

So: hold fire through rest 1 and the fire effect camps on AUD2 while AUD0/AUD3 are resting, and for nearly
fifteen seconds all the player hears is shooting. That is the reported symptom, and it is the game's own
arrangement.

The optional **autofire assist** makes it as likely as it can be, because it keeps the fire effect re-issued
for as long as fire is held. Nothing is wrong when it happens, but a player who wants to hear the tune through
rest 1 has to stop shooting.

### What the measurement looks like, and one trap

With `--sfx-volume 0` the Phase 8 gains mute a channel from the moment the **effect code** wrote its pointer
until the music writes it again, so a voice the effects keep re-taking stays muted in a music-only render even
though the music would have played notes on it. That is why `level1_play` (constant autofire) shows a merged
14.8 s "no music at all" interval where the no-fire run shows only three 0.4 s ones. The left side of the
**plain** render is the measurement that cannot be fooled, because effects never touch AUD0/AUD3.

For the same reason the shop and the ending look silent in a music-only render (`shop` frames 1970-5188, mix
rms 1017): the slot-6 module's own driver is deliberately classified as effects ([cdtv-music-map.md](cdtv-music-map.md) §5).

## 3. The tool

`tools/audio/music_dropouts.py` lists the music-silent intervals of any replay, with the game state and the
driver calls around each one:

```
R=tests/replays/level1_play.txt
./build/x2recomp --headless --report-every 0 --assets generated/assets --input-script $R \
    --sfx-volume 0 --audio-out music.wav --audio-cues cues.txt \
    --watch-log watch.txt --watch E04,CC6,CCA,9FC,A0A,CDC,414,418,CD0
./build/x2recomp --headless --report-every 0 --assets generated/assets --input-script $R --audio-out mix.wav
python3 -I tools/audio/music_dropouts.py music.wav --mix mix.wav --cues cues.txt --watch watch.txt
```

It reports two lists: intervals with no music at all, and intervals where the left pair alone is silent (the
tune's own rests). It needs numpy, which is not a project dependency, so run it with `python3 -I`.

CTest `music_gaps` (`tests/audio/music_gaps.cmake` + `tests/audio/check_music_gaps.py`, stdlib only) pins the
shape confirmed below: the left pair plays over `level1_play` frames 20-440, 1000-2800 and 3600-4400 with no
gap over 1.5 s, and is silent with the right pair loud over 2850-3570.

## 4. Reference: the same silences, at the same frames

The step-aligned Amiberry captures of section 10 of [reference-compare.md](reference-compare.md) were re-used.
Amiberry's WAV tap holds the last DAC value when a channel's DMA stops, so it carries a large DC offset (L 192,
R 2559 during the silent SELECT menu); both sides are therefore measured as the per-frame **standard
deviation**, which ignores DC. Game frames come from the last column of each run's `frames.log`; our frames are
labelled one earlier by convention.

| Capture | Side | Reference silent intervals ≥ 0.3 s (game frames) | Ours |
|---|---|---|---|
| `s_go` (`game_over`, 0-4544) | left (music only) | 463-975, **2841-3579** | 462-974, **2840-3578** |
| | right | 463-653, 668-975 | 462-652, 668-974 |
| `s_tp` (`two_player`, 0-5999) | left | 463-980, **2846-3584**, 5389-5562, 5597-5771, 5806-5862, 5940-5979 | 462-979, **2845-3583**, 5388-5561, 5596-5770, 5805-5861, 5940-5978 |
| | right | 463-623, 625-653, 668-980 | 462-622, 625-652, 668-979 |
| `s_b1` (`boss1`, 0-3419) | left | 463-975, 1046-1257, 1373-1422, 1482-1604, 1658-1983 | 462-974, 1045-1256, 1372-1421, 1508-1603, 1667-1982 |
| | right | 463-653, 668-975, 1046-1187, 1200-1214, 1304-1512, 1527-1691, 1706-1748, 1829-1983 | 462-652, 668-974, 1045-1186, 1199-1213, 1303-1511, 1527-1690, 1829-1982 |

Every interval matches to within one frame except `s_b1`'s fourth left-side gap (1482-1604 against 1508-1603,
0.5 s), which is inside the shop, after the disc load where the two runs' random generators have already
diverged ([reference-compare.md](reference-compare.md) §10.3). Our extra interval at `boss1` 3352-3419 is the
end-of-level stop that the reference capture ends before reaching.

Per-game-frame envelope correlation, reference against ours:

| Capture | Left (music only) | Right (music + effects) |
|---|---|---|
| `two_player` | **0.9955** | 0.9420 |
| `game_over` | 0.9168 | 0.8652 |
| `boss1` (0-3419) | 0.8607 | 0.9367 |

**Verdict per interval: authentic, all of them.** Nothing in the Paula model, the DMA restart and latch timing,
the AUDxLEN reload, the period clamp, the mid-sample DMACON handling or the source tagging needed a change, and
no replay or golden hash was touched.

## 5. The live output path: what can make a dropout that no headless run shows

`runtime/audio/sdl_audio.{h,cpp}` and the window loop in `runtime/main.cpp`.

**How it is paced.** The audio device is the master clock: the loop runs one emulated frame whenever
`queued_frames() < target_frames()` and otherwise sleeps 0.5 ms. Each frame pushes about 962 stereo frames
(~20 ms), so the ring buffer sits between the target and the target + 20 ms. The default target is 40 ms, so
the host loop has only about **40-60 ms of headroom** before the device runs dry. The capacity is
`max(target * 4, rate / 4)` = 250 ms, and the stream's frequency ratio is nudged by at most ±0.5 % to absorb
the drift between 49.92 Hz and the host's pacing.

**What an underrun costs.** `pull()` sets `started_ = false`, so the callback keeps outputting silence until
the buffer is primed back to the target. A single stall therefore costs the stall itself **plus** one target
latency of silence — about 80 ms at the default. That is audible, and it sounds exactly like "the music cut
out for a moment".

| Risk | Protected? | Note |
|---|---|---|
| Window **drag / resize** | **no** | On Windows the `WM_ENTERSIZEMOVE` modal loop keeps `SDL_PollEvent` from returning, so no frame runs for the whole drag and the buffer drains in ~60 ms. The fix would be an `SDL_AddEventWatch` callback that runs frames from inside the modal loop; that re-enters the emulation from the OS's loop, so it was not done here. **Do not drag the window while playing.** |
| **Fullscreen** toggle | no | `SDL_SetWindowFullscreen` plus the renderer's reaction can take well over 60 ms: one underrun, then recovery |
| Focus loss / another window on top | yes | the loop keeps running; only the keyboard grab and held keys change (`runtime/input/input.cpp`) |
| **Slow frames** (software renderer, interpolation, CRT filter at 4x on a weak GPU) | partly | with interpolation the present happens inside the same wait loop, before the audio check, so a present over ~60 ms starves the device. The end-of-run report already prints `slow presents`; `--no-interp`, a lower `--interp-scale`, a smaller `--scale` or `--filter nearest` all help |
| Pause (P) | by design | `set_paused` stops the device and flushes, so no stale audio on resume |
| Reset | by design | `flush()`, then ~40 ms of re-prime |
| Volume / scale / music hotkeys | yes | no flush, no device change |
| SDL default-device change (headphones plugged in) | yes | the stream is opened on `SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK`, which SDL3 migrates itself |
| Ring-buffer **overflow** | yes | `push()` drops and counts; it cannot happen while the device clocks the loop |

**What was changed.**

- `--audio-latency MS` (10..500, default 40) and the settings key `audio_latency`, so the headroom can be
  raised on a machine that cannot hold 50 fps. Nothing else in the audio path moved: headless runs,
  `--audio-out`, the replay hashes and the golden frames are untouched.
- The window now prints, at most once a second,
  `live audio: the output buffer ran dry (N so far); the host loop stalled for more than 40 ms.` so a host-side
  dropout is visible while playing instead of only in the `live audio:` summary at quit. That summary now also
  gives the target in ms.

## 6. Open questions

1. The tune's structure beyond the 220 s measured: whether it loops, and where. The 14.8 s rest appears once in
   every run, always +1866 frames after `music_init`; no second occurrence was seen within 220 s.
2. The meaning of the effect commands `$0185` and `$020A`/`$010A` is still inferred
   ([audio-notes.md](audio-notes.md) §3), so the *which* effect, not the *when*, of the 0.3-1 s right-side gaps
   is unconfirmed.
3. The slot-6 module's own music has not been measured separately: the Phase 8 writer query classifies all of
   it as effects by design, so a music-only render cannot see it.
4. Nobody has listened to any of this. All of section 2 and section 4 are waveform measurements.
5. The window drag hole is documented, not fixed.
