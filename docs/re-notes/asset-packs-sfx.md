# Asset packs: sound effects, speech and `--dump-audio` (WS3b)

This note records what was measured and what was built for sound-effect and speech replacement and the audio dump
(`docs/re-notes/asset-packs-v2-design.md` sections 5.1, 5.2, 5.5 and 5.6). The player's guide is `docs/ASSET-PACKS.md`; this is
the engineering side. Music slots are WS4.

## 1. What exists

| Piece | File | Notes |
|---|---|---|
| Paula event sink | `runtime/audio/paula.{h,cpp}` | `SoundEvent` (Start, Reload, Stop, Period, Volume), `set_event_sink`, per-channel mix gate `set_channel_mix`. With no sink and the gates at 256 the output is bit-identical to before. |
| Replacement stage | `runtime/audio/pack_audio.{h,cpp}` (library `x2packaudio`) | `PackAudio`: classifies starts, looks the sample up in the pack, mixes pack voices into Paula's output frames, `after_frame()` once per emulated frame after `CdtvMusic::after_frame`. |
| Dump | `runtime/audio/audio_dump.{h,cpp}` | `AudioDumper`: `--dump-audio DIR`. |
| Disk effect tags | `runtime/cdtv/cdtv_music.*` (`EffectTags`), `runtime/modern/layout.*` (`AudioLayout`) | The writer-of-AUDxLC tags that tell effects from tracker notes, now per build. |
| Speech file names | `runtime/cdtv/cdtv_game.*` (`set_file_loaded_hook`) | The CDTV file service tells the stage which disc file landed where. |
| Tests | `tests/audio/pack_audio_tests.cpp` (`pack_audio`), `tests/audio/dump_audio.cmake` + `check_dump.py` + `keys/*.keys` (`dump_audio_disk`, `dump_audio_cdtv`, `dump_audio_compilation-1762`) | The data tests are Skipped without the assets. |

The stage touches the output mix only. The channel keeps playing the original sample (DMA, interrupts, statistics, CPU timing), so a
run with a pack has the machine state, frame hashes and CPU timing of a run without one; the replay corpus, run with `--dump-audio`
(which installs the sink and the tags), reproduces every recorded frame and audio hash.

## 2. How the games start sounds (measured)

All figures are from the level-1 replays (`tests/replays/level1_play.txt`, `tests/replays/cdtv/level1_play.txt`) with the event
trace of the sink.

**Disk builds.**

- Effects and tracker notes both start by a DMA enable on a channel whose AUDxLC/LEN/PER/VOL were written just before. The two are told
  apart only by *who wrote AUDxLC*: the effect routine of the resident driver (`$1B26A-$1B6CF`) against the tracker (`$19F9E-$1B269`,
  and `$1B5D2`, which hands a channel back to the music). That is the tag `CdtvMusic` already installs for `--sfx-volume`; the
  pack stage uses it, so it is installed whenever packs or a dump are configured (it reads state only).
- **One shot, then a park loop.** Right after a DMA start the effect code writes the loop pointers: a 32-word buffer at the bank's start
  (`$2ABA0` canonical) and period 124 when the sound is over. The first buffer plays once (`Reload` events show the channel moving to
  the park buffer), so the key of an effect is the bytes of the *first* buffer: the latch at the start. Period 124 is written
  immediately before the DMA off at the end of a sound: that parking write is why a write of period 124 reaches a voice 3 ms late (below) and
  why the dump ignores a period change in the last 3 ms.
- **Pairs.** Some effects (for example the 1,002-byte `028540cb744d5850-1002`) are started on two channels a few frames apart with the same
  buffer (AUD1 and AUD2, both on the right side); the weapon uses AUD2 alone. Each start is its own Paula channel, so a mono replacement
  gets a voice per start (reproducing the original's level), and a stereo replacement gets one voice (below).
- **Stale starts.** Occasionally the game enables a channel pair before it has written one channel's registers; the channel then plays
  the park buffer's neighbourhood (the start of the sample bank, with the previous length) for a while. Paula models that, so the
  dump lists it as a sample (`uses` 1, `ref_period` 124, the bank's first bytes): ignore such entries. They never match a replacement an
  author writes.
- Weapon fire (the effect `2b086881fb6a3b63-4160`, 944 starts in the level-1 replay) is cut short by the next shot after about 77 ms, so
  `cut: channel` (the default) is what the original does.
- The shop and the ending run their own driver (`($2090)` points elsewhere); the writer tags count its notes as effects, so a dump of
  those screens lists the music's instrument samples and a pack can replace them one by one (and `--sfx-volume` governs them).

Per build (level-1 replay):

| Build | Driver | Effects in the replay | Same sample keys as canonical |
|---|---|---|---|
| canonical, retail 2874 and 0297, cracks, Power Pack | canonical addresses | 8-9 | retail 7 of 9, Power Pack 4 of 9 (lower-precision samples; list the other key in `keys`) |
| 1762 compilation | moved down by `$5DE` (`$19C3A` for canonical `$1A218`), tick pointer cell `$209E` (canonical `$2090`) | 9 | 8 of 9 (the ninth is a stale start) |
| PDos v5 | music code `$B4` higher, pointer cell elsewhere; effect code not measured (the dump cannot start a level) | not identified | the stage does nothing on this build and says so |

The table of addresses is `AudioLayout` (`kDiskAudio`, `kDisk1762Audio`, `kDiskPdosAudio` in `runtime/modern/layout.cpp`); the shadow
channel records of the 1762 build (`$1B184`) were checked by shape (the same four records, park pointer, periods 439/186/179/179).
The cracks were not measured individually (no local images); they carry the canonical game body.

**CDTV release.**

- There is no tracker: every Paula start is an effect or speech, so every start is a candidate.
- Speech is the buffer `$B645C-$D0973` (`$1A518` bytes, the size every `SPEECHnn` is loaded with, `cdtv-c0.md` section 5). A start whose
  pointer lies in it is speech (`audio.speech`), any other start is an effect (`audio.sfx`). The file service trap remembers the name
  of each `SPEECHnn` loaded, so `cd0_file` identifies a clip by disc file without hashing it. In the level-1 replay the narrator
  plays `SPEECH63`, `SPEECH50` and `SPEECH37` (53,870, 18,944 and 40,472 bytes), all at period 298, each started on a channel pair.
- Twelve effects are heard in the same replay; one four-byte buffer (the channel parking loop) is ignored by the dump.

**ST.** No Paula, nothing to replace (`--dump-audio` with `--game st` says so and exits 3).

## 3. The voice, as built

The design's rules hold; where the build differs or decides something the design left open:

- **Level.** A mono file at full scale plays at the level of one Paula channel at full volume (half of a side's full scale at the
  output gain), not at the CD player's full scale. That is what makes a dump play back at the original's level with `gain_db` 0:
  the dump writes the 8-bit sample times 256. (The design sentence "a full-scale file maps to Paula full scale" is therefore
  one channel's full scale; raise a file with `gain_db`, range -60 to +24.)
- **Alignment.** Paula's output stage delays what the machine produces by 15.625 output frames (the decimating FIR's group delay of 63
  samples at four times the output rate, minus the half sample of the area average that precedes it); every voice event is shifted by
  `kLatencyFrames` = 16 and a new voice starts 3/8 of a frame into its clip, so a voice starts, changes and ends in step with the original it
  replaces (and with `original: mix`). The fraction matters: measured on a replay with the filter off, the correlation of a dump played
  back with the original rises from 0.983 (16 frames flat) to 0.993 (15.625).
- **Pitch** is the integer ratio `ref_period / AUDxPER` in Q32.32 positions over the clip prepared at the output rate (a file at another
  rate is resampled once, at its first use, with the decoders' Kaiser sinc; interpolation between frames is linear, exact when the
  ratio is 1). Periods below 124 are clamped like Paula's. `ref_period` 0 takes the period of the start.
- **A write of the minimum period (124) reaches a voice 3 ms late** (`kPeriodDeferMs`): see the park write above; the voice is detached by then
  and the write is dropped, so a parking write just before a cut (or before the same sound starts again) never makes a voice chirp. Every
  other period, a pitch bend, applies at once, and always to the voice that was on the channel when the game wrote it.
- **Volume** follows AUDxVOL through a 2 ms ramp; `--sfx-volume` and `--speech-volume` scale pack voices as they do the
  originals (the speech volume is read from the settings every frame).
- **Stereo files and pairs.** A mono file gets a voice per start; a stereo file started on a pair of channels within 3 ms plays once
  (its siblings only gate their own original), because the file already has both sides.
- **Cut.** `cut: channel` ends (with `fade_out_ms`) at the channel's next start or when it goes idle (the *idle* edge, which follows
  the DMA off by at most one sample period); `cut: never` detaches the voice from the channel and lets it ring. A `loop` voice also
  fades when a reload moves the channel to a different buffer than it started on.
- **Polyphony** 16 live voices; the oldest is faded in 5 ms when a 17th starts; at most 32 including fading ones.
- **The original** is muted from the start to the next start or idle by Paula's per-channel gate (`mute`: 0, `mix:-12`: 256 x 10^(-12/20)),
  so `--protect-music`'s mirror voices, which are separate, are unaffected.
- **The Faithful switch** (F1, `--faithful`) and `audio_upgrade = false` end all voices (5 ms fade) and open every gate at once; the
  main loop copies the F1 state into the pack settings before the stage runs.
- **Hash cost.** A start is hashed only when it is an effect or speech start and the library has a clip of that byte length
  (`has_clip_length`), or a dump is running.
- **Determinism.** Gains come from integer arithmetic and an exp/sin built from `+ - * /` only (no libm), `pack_audio.cpp` is compiled
  without FMA contraction, positions and pitch are integers; a synthetic scenario with resampled stereo and mono clips, loop, original mix,
  pitch bends and pairs has one recorded hash that Debug and RelWithDebInfo builds (and the CI platforms) agree on (`pack_audio` test `determinism`).

## 4. `--dump-audio DIR`

A run (window or headless, with or without a script) records the first bytes of every distinct effect and speech start and writes, at
the end, `sfx/<key>.wav` and `speech/<key>.wav` (mono 16-bit, the 8-bit sample times 256, every sample repeated a whole number of times so that the rate, `repeats x 3,546,895 / ref_period`
rounded, is 44.1-192 kHz and within 1e-5 of exact: the file holds each sample for its true duration, like Paula's own zero-order hold, so it
plays back like the original and a long loop does not drift), `music_cues.txt` when the music layer reports cues
(`AudioDumper::note_cue`; WS4 feeds it), and `manifest.json`, a valid version 2 pack with `audio.sfx` / `audio.speech` entries
(`file`, `ref_period`) plus the dump-only fields `bytes`, `uses`, `first_frame` (the machine's frame), `channels` (bit mask), `periods`
`[min, max]`, `peak`, `source` and, for speech, `cd0_file`. `ref_period` is the period most starts used. Not entered: all-zero buffers and
buffers shorter than 16 bytes (the channel parking loops).

The dump merges into what is there: an existing manifest keeps every other member (they are not re-serialised); entries already listed
and WAV files already on disk are never touched, so an author can edit them, play another route for more sounds and dump again;
`--dump-assets` into the same directory carries an existing `audio` section over. A version 1 manifest becomes version 2. A directory
whose manifest is not a JSON object is refused. Run `--dump-audio` after `--dump-assets` in the same command or separately.

`tests/audio/check_dump.py` verifies a dump without the runtime: file format, rate, the samples being the 8-bit data times 256, and the
key being the FNV-1a hash of the bytes. The data tests run it on the level-1 replays and compare the key list with
`tests/audio/keys/` (hashes of samples, no audio).

## 5. The dump as a pack (round trip)

`--dump-audio` output loads as a pack unchanged (`x2audio` registers its decoders as the loader's `AudioCodec` at start-up: `pack_codec()`).
Tests `dump_roundtrip_disk` and `dump_roundtrip_cdtv` (Skipped without the assets) run the level-1 replay plain, with a dump, and with the dump as
`--asset-pack`, once as a directory and once as an uncompressed ZIP: the two pack runs render identical audio, each correlates with the plain run at
**0.994 (disk) and 0.993 (CDTV)** (threshold 0.99; the output filter is off for the comparison because pack voices bypass it), and
`--check-asset-pack --against` reports every dumped sound as covered. Getting there found three things worth knowing:

- A sound the game **loops** (the effect code leaves the registers on the same buffer, so it reloads it: the disk build's 1,002-byte sweep, three
  sounds in the level-1 replay) must be a `"loop": true` entry, or the file ends and the sound stops early. The dump sets it when it sees a
  reload of the buffer a sample started with; the voice wraps at the file's exact length in output frames (Q32.32), not its rounded frame count.
- A deferred parking write must not reach the next voice: the period event carries the serial of the voice that was on the channel, and only a
  write of the minimum period (124) is deferred; every other period applies at once.
- Time alignment to a fraction of a frame matters (15.625 frames, section 3) and a file must hold each 8-bit sample for its true duration
  (section 4): without those the correlation was 0.57, then 0.96 and 0.98.

## 6. Open items

- A replay is not a hand-played session; the shop and ending drivers (which count as effects) are not in the replays.
- Not mapped: the PDos effect code; the individual cracks (they should share the canonical driver).
- The CDTV hand-play has not happened, so speech and effects in the CDTV release are checked by replay only.
- `AudioDumper::note_cue` has no caller until WS4's music cues.
