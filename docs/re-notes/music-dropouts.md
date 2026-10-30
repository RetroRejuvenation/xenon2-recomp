# Music dropouts: when the music stops, and whether it is the game or us

Question (from playing the Windows build), in the reporter's two messages: *"I think sometimes the music stops
on occasion, can't remember if that's authentic or not"*, then *"it seemed to be on heavy firing when several
sound effects chained together"* and *"the music is silenced for as long as I keep firing heavily, and resumes
as soon as I stop"*.

**Short answer: authentic.** The disk game has one tune, and that tune has long rests in which only AUD1 and
AUD2 play. Those are exactly the two channels its sound effects take, so while the player holds fire the music
becomes inaudible for as long as the rest lasts — up to **14.8 s** in level 1. Every rest, and every handover
of a channel between the music and an effect, appears at the same frame in the Amiberry reference capture of
the same replay (section 4), and so does the suppression under sustained fire (section 7). No emulation bug was
found, and nothing was changed in the audio path.

**Section 7 answers the firing report directly**, and is the part to read first: a sound effect reloads its own
duration counter every time it is re-issued, so **chained effects on the same channel hold that voice with no
hand-back at all** for as long as the chain lasts. It is one voice (AUD2), never all four — but AUD2 is on the
loud side, and during the tune's rests it is one of only two voices playing.

**Section 8 is the optional fix.** `--protect-music` (off by default) plays the suppressed voice from the
driver's own shadow registers on a mirror voice that is not on the bus, so the tune keeps playing through the
shooting while the game's state, timing and picture stay bit-identical.

Two things were added anyway, both for the live output and neither affecting the emulation:

- `--audio-latency MS` (settings key `audio_latency`, default 40) so a machine that cannot hold 50 fps can buy
  more headroom, and
- a visible message when the live output's buffer runs dry, so a host-side dropout can no longer be mistaken
  for the game's own silence (section 5).

Sources: the disassembly of the resident image (`generated/lane-b/x2-rt.dis`) and a chip-RAM dump for the
tables it builds at run time; music-only renders of the whole replay corpus (`--sfx-volume 0 --audio-out`, the
Phase 8 per-source gains of [cdtv-music-map.md](cdtv-music-map.md) §5); `--audio-cues`, `--watch-log` and
`--pc-log` over those runs; the step-aligned Amiberry captures
`generated/ref/runs/{s_go,s_tp,s_b1}` ([reference-compare.md](reference-compare.md) §10); and two Amiberry
captures made for section 7, `generated/ref/runs/{f_hold,f_tap}`, of a 20 s-firing / 10 s-quiet cycle.

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

`--own` additionally reports which voices the sound effects held and for how long without a break, from the
driver's own flags:

```
./build/x2recomp --headless --report-every 0 --assets generated/assets --input-script $R \
    --watch-log own.txt --watch 1B76C,1B778,1B784,1B790
python3 -I tools/audio/music_dropouts.py music.wav --own own.txt
```

CTest `music_gaps` (`tests/audio/music_gaps.cmake` + `tests/audio/check_music_gaps.py`, stdlib only) pins both
shapes confirmed below: the left pair plays over `level1_play` frames 20-440, 1000-2800 and 3600-4400 with no
gap over 1.5 s and is silent with the right pair loud over 2850-3570; and in a run with fire held from frame
1000, AUD0 and AUD3 are never taken by an effect while AUD2 is held for most of the time (measured 0.0 %,
0.0 % and 85.4 %).

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
4. Nobody has listened to any of this. All of sections 2, 4 and 7 are waveform measurements.
5. The window drag hole is documented, not fixed.
6. Section 7 measures one voice at a time only through the L/R pairs, because AUD1 and AUD2 share the right
   side of the output. Our side also has the driver's own `$B(a4)` flags per voice; the reference capture has
   only the pairs, so the per-voice claim rests on the pairs plus the shared code path.

## 7. Heavy firing: the reported case

> *"the music is silenced for as long as I keep firing heavily, and resumes as soon as I stop"*

### 7.1 The rule, from the code

There is **no priority and no mixing policy**. A command's channel is fixed by the queue it arrived on and
nothing else, and a command landing on a channel an effect already holds simply extends the hold.

- **Queue depth is one word per queue.** The VBL (`$2160-$219C`) reads `$414`, passes it as `d0 | $200`, calls
  `($2094)` and writes `$FFFF` back; then the same for `$418` as `d0 | $100`. So at most **two effects can
  start per frame, one per queue**. A writer that stores into a slot still holding an undispatched command
  overwrites it and the earlier command is lost; the fire writer at `$615A` avoids that by testing
  `tst.w $414.w / bpl` and skipping if the slot is busy. H
- **The queue decides the voice.** `$1B282` stores `d0 & $3FF` into `$17C2(a3)` = `$1B760`, and
  `$1B2C0`/`$1B2E2`/`$1B478` read that byte as the channel index. `$200` is AUD2, `$100` is AUD1. The only
  comparisons anywhere in `$1B26A-$1B50C` are the four `cmpi.b #0..3,$17C2(a3)` of the channel dispatch — no
  priority, no "is this channel busy", no voice stealing decision. **Effects can therefore never take AUD0 or
  AUD3.** H (measured: 0 of 12,000 frames for both, section 7.2)
- **A new command reloads the duration counter.** `$1B4C6: move.b $c(a0),$12(a1)` writes the effect table's
  duration into the record's remaining-ticks counter unconditionally, and `$1B4CC: tst.b $d(a0) / bne` sets it
  to 0 ("until something else stops it") for the one table entry that asks for it. `$1B2C8:
  st.b $b(a4,d7.w)` re-marks ownership. Neither path hands the channel back first. **So overlapping commands
  on one channel hold that voice continuously, with no gap.** H
- **The hand-back is immediate and mid-note.** `$1B5A2` (called per channel from the effect tick `$1B540`,
  which the music tick always falls into at `$1A14E`) decrements `$12(a1)`; when it reaches 0, `$1B5AE` clears
  the active flag, clears `$B(a4)`, waits ~12 scanlines and then `$1B5D2-$1B5E4` writes the music's shadow
  LC/LEN/PER/VOL straight back and re-enables DMA. The music voice therefore resumes **at its current note,
  part-way through**, not at the next one — the four shadow fields (`$0`, `$4`, `$6`, `$8` of
  `$1B762 + ch*$C`) are updated by the music tick on every tick whether or not an effect owns the channel
  (`$1A1EE`, `$1A1F2`, `$1A278`, `$1A27E`, `$1A28C`, `$1A3A2`, `$1A3D4` are all unguarded; only the Paula
  writes after them are behind `tst.b $b(a4)`). H
- **No path stops the music tick.** It returns early only on the playing flag, the tempo-accumulator carry, or
  the 1-in-6 skip that never runs (§1.3), and the effect tick runs after it either way. H

The effect table (`$1B352`, 18 entries of `$10` bytes, built at run time by `$1B216` from the sample bank, so
it is zeros in a static dump) read out of a chip-RAM dump at frame 1300:

| Entry | Sample | Period | Duration |
|---|---|---|---|
| 9 — **the fire sound** (`$0289`, `$89 & $7F`) | `$032F38` | 357 | **21 ticks = 0.42 s** |
| others | `$02BAA2`-`$03A09A` | 179-873 | 9-53 ticks (0.18-1.06 s) |
| 13 | `$035E64` | 311 | `$D` = 255: **no duration, plays until stopped** |

That 0.42 s against the rate at which the game re-issues the sound is the whole story:

| Input | Fire sounds in 7,000 firing frames | Re-issue interval | Effect length | Result |
|---|---|---|---|---|
| fire **held** | 294 | ~24 frames | 21 frames | AUD2 free 3 frames in 24 |
| fire **tapped** every 4 frames (an autofire or a rapid weapon) | 1,754 | ~4 frames | 21 frames | the counter is reloaded five times before it can expire: **AUD2 never handed back** |

### 7.2 Measured, ours

Scenario: level 1, energy and ships held by the documented coverage pokes, **fire held for 1,000 frames (20 s)
then released for 500 (10 s), seven times** over 12,000 frames. `--watch 1B76C,1B778,1B784,1B790` reads the
four `$B(a4)` flags, so voice ownership is measured directly rather than inferred.

| | AUD0 | AUD1 | AUD2 | AUD3 |
|---|---|---|---|---|
| fire held: frames owned by an effect, of 12,000 | **0** | 1,063 | 5,997 | **0** |
| fire held: share of the firing blocks | 0 % | 14.0 % | **84.0 %** | 0 % |
| fire held: separate holds / longest | — | 28 / 1.54 s | 288 / **1.36 s** | — |
| fire tapped: share of the firing blocks | 0 % | 13.6 % | **99.8 %** | 0 % |
| fire tapped: separate holds / longest | — | — | 11 / **20.47 s** | — |

So with fire tapped the hold is a single unbroken 20.47 s run covering the whole firing block: the voice is
never handed back until the player stops. With fire merely held the game's own 24-frame shot rate leaves
3-frame gaps, so it is 288 brief holds instead — audible as stuttering rather than as a sustained stop.

The music tick keeps running throughout. Watching AUD2's note counter (`$1A614` = `$1E` of its channel record)
through a firing block: it counts 5,4,3,2,1 and reloads continuously while `$1B784` reads `FFFF` (owned), and
the hand-back at frame 5007 of that run lands with the counter at 4, i.e. **part-way through a note**. The
playing flag `$1A58E` stays `$FF00` the whole time.

What the player hears, music-only renders over level-1 frames 1050-11900 (218 s), same frames, three input
patterns:

| Input | both sides playing | left only | right only | **nothing** | longest silence |
|---|---|---|---|---|---|
| no fire | 60.0 % | 7.0 % | 26.2 % | 6.8 % (14.7 s) | 0.86 s |
| fire held | 46.9 % | 20.0 % | 15.0 % | 18.1 % (39.3 s) | 1.50 s |
| fire tapped | 46.4 % | 20.6 % | 13.8 % | **19.2 % (41.8 s)** | **13.88 s** |

The left side (pure music) is unchanged by firing: mean 1232 while a block fires and 1660 between blocks, the
same to three figures in all three runs, because the tune is frame-locked and effects cannot reach AUD0/AUD3.
38 % of frames are bit-identical between the no-fire and fire-tapped runs; the residue is a sub-frame phase
shift of 2 % of the signal (mean |difference| 27 against a level of ~1400), because the effect routine's
busy-waits (`$1B536`, ~12 scanlines, up to four per dispatch) push the music tick later inside the VBL on the
frames where an effect starts. Note content is unaffected.
The right side loses **53 %** of its level with fire held and **58 %** with it tapped. Inside the tune's 14.8 s
rest, where the left pair has nothing, the right side goes from 2610 (no fire) to 406 (held) to 174 (tapped):
that overlap is the 13.88 s stretch above, and it is the reported symptom.

So: **one voice, not all four.** Away from the tune's rests the loss is an instrument line disappearing from
the right channel; during a rest it is the whole tune. `--timing fast` and `--timing accurate` give the same
result (84.2 % against 84.0 % ownership, 293 against 294 fire sounds), so this is not an artefact of the bus
model.

### 7.3 Reference

Two new captures with the identical input, converted by `tools/compare/to_harness.py --step-sync`:
`generated/ref/runs/f_hold` and `generated/ref/runs/f_tap` (12,000 game frames each).

The reference's absolute levels are 1.334x ours throughout, which is exactly the `output_gain = 0.75` of
`runtime/audio/paula.h`, so the comparison is of the **change** between firing and not firing. Both sides are
the per-game-frame standard deviation, as in section 4.

| Scenario | Side | Between blocks → while firing: reference | ours | Envelope correlation |
|---|---|---|---|---|
| fire **held** | left (music only) | **−26 %** (2216 → 1647) | **−26 %** (1660 → 1232) | **0.9979** |
| | right (music + effects) | **−17 %** (5344 → 4421) | **−17 %** (4007 → 3313) | **0.9938** |
| fire **tapped** | left (music only) | **−25 %** (2216 → 1652) | **−26 %** (1659 → 1235) | **0.9952** |
| | right (music + effects) | **+16 %** (5351 → 6182) | **+15 %** (4018 → 4634) | **0.9837** |

The right side getting *louder* while the music on it is being suppressed is the shooting itself filling the
channel, and the reference does it by the same amount.

Every long left-pair rest matches, in both scenarios, with the usual one-frame label offset:

| | reference | ours |
|---|---|---|
| left-pair rests ≥ 1 s, fire held | 14 | 14 |
| left-pair rests ≥ 1 s, fire tapped | 14 | 14 |
| the long one | 2828-3579 (15.06 s) | 2827-3578 (15.06 s) |
| the others | 463-975 (10.28), 5384-5557 (3.49), 5592-5766 (3.51), 5801-5857 (1.14), 6009-6065 (1.14), 6072-6182 (2.22), 6217-6390 (3.49), 6425-6485 (1.22), 7589-7640 (1.04), 7883-8057 (3.51), 8092-8265 (3.49), 8508-8681 (3.49), 8716-8890 (3.51) | all the same, one frame earlier |

The full ≥ 0.3 s lists (over 200 intervals each, mostly the tune's own 0.3 s gaps) are identical item for item
apart from that offset and a handful of sub-0.5 s entries inside firing blocks where the two runs' random
generators have diverged after the disc load ([reference-compare.md](reference-compare.md) §10.3).

**Verdict: authentic.** The reference loses the same music under the same sustained fire, by the same
percentage, at the same frames. Nothing in the channel hand-back, the DMA restart and latch timing, the
AUDxLEN/LC reload at the end of the effect sample, the DMACON writes or the source tagging differs, and the
result is identical under `--timing fast` and `--timing accurate`. No fix was needed and no audio hash was
re-recorded.

### 7.4 Could it be mitigated for players?

Faithful behaviour stays the default whatever is done. Three options were considered:

1. **Already possible, no code:** `--sfx-volume` below 100 ducks the effects rather than the music. It does not
   bring the lost voice back, but it stops the shooting masking the rest of the tune. Costs nothing and is
   purely an output gain.
2. **Cheap and wrong:** sending effects to AUD0/AUD3 instead. That means changing `$17C2(a3)`, i.e. the game's
   own state, so it would move the music's channel usage too and break every audio hash. Rejected.
3. **Mirror voices** driven from the driver's shadow registers. This is what `--protect-music` does; see
   section 8.

## 8. `--protect-music`: giving the tune its voices back

**Off by default, and a deliberate deviation from the original.** Section 7 establishes that heavy firing
silences a voice of the tune on real hardware too, so this is an enhancement, not a fix.

### 8.1 Why the obvious design is not available

The natural idea is to let the music keep writing Paula and send the *effects* to extra voices: neutralise the
ownership test at `$B(a4)` and route effect-tagged writes elsewhere. The driver's own state machine would
survive that — `$B(a4)` is read **only** to decide whether to write Paula (`$1A1F6`, `$1A212`, `$1A234`,
`$1A290`, `$1A2C2`, `$1A3A6`, `$1A3D8`, `$1A4BE`), and all of the music's bookkeeping is unconditional, which
section 7.2 confirms at run time: AUD2's note counter keeps running through a hold.

What rules it out is **bus timing**. `machine.cpp` builds `audio_dma_` each line from `paula.dma_fetches(c)`,
and `Machine::fixed_dma()` marks those audio slots as taken, so an audio channel's DMA costs the CPU cycles
([timing.md](timing.md)). Change which channels fetch, or at what period, and the CPU's cycle count per line
changes with it — which moves the game's logic-step boundaries and therefore its random generator and its
whole state. The music's periods (124-1270) are not the effects' (179-873), so any design that changes what
the real channels do changes the game. That fails the requirement that the option be audio-only.

### 8.2 What it does instead

Paula runs four **mirror voices** that are not on the bus at all. Their registers are the driver's own shadow
records at `$1B762 + ch*$C` (+0 LC, +4 LEN, +6 PER, +8 VOL), which the music tick keeps up to date on every
tick whether or not an effect owns the channel — `$1A1EE`, `$1A1F2`, `$1A278`, `$1A27E`, `$1A28C`, `$1A2BE`,
`$1A3A2` and `$1A3D4` are all unguarded, and each writes the same value the guarded Paula write after it would
have written. So the shadow is an exact copy of the music's intended Paula state, available even while it is
being suppressed.

| Question | Answer |
|---|---|
| When does a mirror start a note? | On a **music-tagged DMACON write that names its channel**. `$1A2DA-$1A2E0` issues `$8200 \| (1 << ch)` at every note start *unconditionally*, held channel or not, so the event is always observable — see 8.2.1 for why "names" and not "rising edge". LC/LEN latch from the shadow then and at each end of buffer, exactly as the hardware's do. |
| When does it stop? | On a music-tagged DMACON write that clears its bit: `$1A240` (note off) and `$1A0C2`/`$1A500` (music stop). Effect-tagged DMACON writes are ignored by the mirrors, which is the whole point. |
| When is it audible? | Only while the **real** channel is playing an effect (`play_src == Effect`). With nothing firing every mirror is inaudible, so the mix is bit-identical to the faithful one. |
| Who is "music"? | The Phase 8 writer query, unchanged: `$19F9E-$1B269` plus `$1B5D2` is music, `$1B26A-$1B6CF` is effects ([cdtv-music-map.md](cdtv-music-map.md) §5). It reads `REG_PPC`, which the translated executor maintains exactly as the interpreter does. |
| Clipping? | One side can now carry three voices where the game drove two, so protect mode applies a **0.64 output headroom** (the faithful path keeps `output_gain` 0.75 untouched). Worst peak over all 17 replays: **0.852** on `level5`, **0 clipped samples** anywhere, against 0.78 for the faithful mix — the same margin. Protect mode is therefore about 3.9 dB quieter; F8 puts it back. |
| `--music cdtv`? | A complete no-op, headroom included: there is no tracker voice to protect. Verified bit-identical. With `--music mix` the tracker voices *are* protected. |

#### 8.2.1 Note starts: why a rising edge is not enough

This was wrong in the first implementation and is the one subtlety worth recording.

On the hardware, `$1A2E0`'s DMACON set retriggers a channel only because the note-off at `$1A240` cleared the
bit one tick earlier. And `$1A240` is precisely the write the driver skips while an effect owns the channel
(`$1A234` tests `$B(a4)` first). So while a channel is held, the bit is already on at every note start and a
mirror watching for a rising edge sees **nothing after the first note**: it holds one note and only tracks the
melody loosely, through the LC/LEN it re-reads at each buffer reload. Measured, that cost about 10 % of the
level and dropped the per-frame correlation against the unobstructed tune to 0.78.

So Paula is given the **word the CPU wrote** as well as the register's new value (`set_dmacon(dmacon, write)`),
and a write with bit 15 set that names channel `ch` restarts mirror `ch` — but **only while the driver's own
`$B(a4)` flag says an effect owns it** (read from the shadow record at +`$B`). When the channel is free the
rising edge is used instead, which keeps the driver's **tied notes** playing on: the `$83` command at `$1A23A`
is the case where it deliberately skips the cut on a free channel, and there the hardware does not retrigger
either. Both halves are pinned by the unit test.

Known deviation of a mirror from the voice the music would really have had: it still misses the one-tick
(20 ms) gap that `$1A240` would have cut before each note, because no instruction executes to observe. A
mirrored voice is therefore slightly more legato, worth **8-14 % more energy** than the voice would have had —
consistent with one tick restored to a six-tick note. Reproducing it would mean reading the music's note
counter and command stream, i.e. reimplementing the driver rather than mirroring its registers, so it was left.

### 8.3 Evidence

**It works.** Music-only renders (`--sfx-volume 0`, so the mirror is all that is left on a held channel) of the
section 7.2 scenario, protect levels divided by the 0.64 headroom so the numbers compare directly:

| | left (AUD0+AUD3) | right (AUD1+AUD2) | right, inside the 14.8 s rest | no music at all, over 218 s |
|---|---|---|---|---|
| fire tapped, protect off | 1235 | 1492 | 174 | 41.8 s |
| fire tapped, **protect on** | 1235 | **3873** | **2984** | **13.1 s** |
| fire held, protect off | 1232 | 1675 | 406 | 39.3 s |
| fire held, **protect on** | 1232 | **3707** | **2852** | **15.5 s** |
| no fire at all (the ideal) | 1232 | 3585 | 2610 | 14.7 s |

Inside the tune's rest, where the left pair has nothing and the right pair is the whole tune, the music goes
from 174 to 2984 with fire tapped and from 406 to 2852 with it held, against 2610 unobstructed. The time with
no music at all falls to the tune's own floor. Protect runs 8-14 % *above* the unobstructed level, which is the
missing note-off cut of 8.2.1 and nothing else. The left side is unchanged to the last digit, as it must be: a
mirror for AUD0/AUD3 never becomes audible because an effect can never take them.

**Note starts land on the right frames.** Both runs reconstructed with protect on and compared against each
other — the tune is frame-locked, so a heavy-fire run and a no-fire run should rebuild the same music:

| | right pair, per-frame correlation | inside the rest |
|---|---|---|
| rising-edge start (the first, wrong implementation) | 0.889 | 0.776 |
| **named-set start (8.2.1)** | **0.980** | **0.953** |
| left pair, never owned — the ceiling for this comparison | 0.995 | — |

The ceiling is below 1.0 because the two runs are not perfect counterfactuals: the effect routine's busy-waits
shift the music tick inside the VBL, and different enemies die, so other effects land at other times.

**The mirror is inert when nothing takes a channel.** `attract` (title, credits, high scores) with protect on
against protect off scaled by the headroom: max difference **0.84 LSB** on a signal of rms 3870, which is int16
rounding of the scaling and nothing else.

**The effects are untouched.** Effects-only renders (`--music-volume 0`) with protect on and off differ by at
most **0.84 LSB** peak (rms 0.196 LSB on a signal of rms 2190, correlation 0.99999999) — the same waveform at
the same sample positions, scaled by the headroom.

**The game is untouched.** Over `level1_play`, `boss1`, `game_over`, `level5`, `two_player` and
`level1_flood` — 34,525 frames — every frame hash and every watched word is identical with protect on and off:
`$1F50`, the generator state `$1FEE-$1FF4`, `$404`, `$41C`, `$E04`, energy `$CC6`, ships `$CCA`, scroll
`$CD0`/`$CDC`, score `$C92`/`$C94`, `$A06` and the ship's y `$3F040`. The **full 512 KB of chip RAM** at frame
5900 of `boss1` and `level1_play` is byte-identical, so the answer to "which of the driver's variables differ"
is: none. The mirrors only read.

**Same everywhere.** `level1_play` and `boss1` give identical frame *and* audio hashes with protect on under
the default build, the translated executor and `--interp-only`.

**It cannot leak into a test.** The whole `settings.cfg` read is behind `if (!opt.headless)` in `main.cpp`, so
a user's `protect_music = on` can never reach a replay or a golden-frame run. Verified: a headless
`level1_play` with `--data-dir` pointing at a `settings.cfg` that asks for protect produces the committed
faithful hashes and reads the file zero times.

**Pinned.** CTest `protect_music_level1_play` and `protect_music_boss1` check the frame hashes (which are the
faithful ones, committed in `tests/replays/`) and the protect-on audio hashes in
`tests/audio/<name>.protect.hashes`. The Paula unit test `protect_music` checks the mirror on synthetic chip
RAM: the level restored, nothing leaking to the other side, no doubling when the real channel plays the music
itself, bit-identical output with protect off, the retrigger of 8.2.1 both ways (restarts while owned, plays a
tied note on while free), and `dma_fetches`, `dma_starts`, `buffer_reloads`, the interrupt counts and the whole
interrupt list unchanged.

### 8.4 Using it

`--protect-music` / `--no-protect-music`, or `protect_music = on` in `settings.cfg`.

It can also be switched while the game is running. `CdtvMusic::set_protect_music(bool)` /
`protect_music()` is the front end (it owns the shadow address and the CDTV no-op) and calls
`Paula::set_protect_music(bool)`, which:

- fades the mirror voices **and** the output headroom in or out together over `Paula::kProtectRampMs` (20 ms),
  so a toggle is a slide and not a 4 dB step. Measured on a steady tone: the largest sample-to-sample step at
  the switch is 38 of a level of 2560, where an unfaded switch would step by about 8,600;
- keeps the mirrors running until the fade-out finishes, so no mirrored note is left hanging;
- snaps instead of fading when nothing has been output yet, so starting with the option on is exactly as if it
  had always been on — which is why the committed protect-on hashes did not move when the fade was added;
- touches nothing outside the output mix. The unit test `protect_music_toggle` asserts `dma_fetches`,
  `dma_starts`, `buffer_reloads`, the interrupt counts and the whole interrupt list are identical to a run
  that never toggled, and that nothing clipped.

Relation to the other sound options: `--sfx-volume` below 100 turns the *shooting* down, which makes the rest
of the tune audible but does not bring the lost voice back — the two are complementary and can be used
together. **With autofire on, protect-music is what keeps the tune playing**: autofire re-triggers the fire
sound every few frames, which is exactly the case where the channel is never handed back (section 7.1).

**By ear, in three steps.**

1. Play level 1 normally and hold fire continuously from the moment the level music starts. About 37 s in
   (1,866 frames after `music_init`) the tune's left pair rests for 14.8 s and the two voices your shooting
   borrows are the whole tune — so the music disappears until you stop firing. That is the original.
2. Quit and start again with `--protect-music` (or put `protect_music = on` in `settings.cfg`).
3. Play the same stretch the same way: the tune carries on through the shooting. It is ~3.9 dB quieter overall
   — press F8 a couple of times to match — and a protected voice is slightly more legato (8.2.1).
