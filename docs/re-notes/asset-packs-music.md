# Asset packs: music replacement (workstream WS4)

What a pack's `audio.music` section does in the runtime, how it is built, and what was measured to build it. The format itself (slot
names, `loop`, `intro`, `crossfade_ms`, ...) is specified in `asset-packs-v2-design.md` section 5.3; this note records the cues, the
engine, the decisions where the specification left a choice, and the evidence. Code: `runtime/audio/stream_source.*`,
`runtime/audio/music_slots.*`, `runtime/cdtv/cd_player.*`, `runtime/cdtv/cdtv_music.*`. Tests: `tests/audio/music_slots_tests.cpp`
(`music_slots`), the data tests `music_pack_disk` and `music_pack_cdtv` (`tests/audio/music_pack_check.cmake`), and the unchanged CD tests.

## 1. Where the music cues are

A cue is a moment where the game starts, changes or stops its music. The runtime already had them for the CDTV layer (PC traps on
the disk build's tracker driver, the CD-play trap of the CDTV release); WS4 reads the same events, with the level word at the cue.

### Disk build (and the cracks, Power Pack, 1762 builds: the same driver)

| Event | Trap | Caller (return address) | Slot names tried |
|---|---|---|---|
| title tune | `music_init` `$19F9E` | `$8236` (the call at `$8230`) | `title` |
| level start, restart after a death, back from the shop, level change | `music_init` | `$789C`, `$7C56`, `$7D92`, ... (anything but `$8236`) | `levelN`, `level`, with N = the level word `$E04` (1-5; anything else counts as 1) |
| stop | `music_stop` `$19FBA` | `$8678`, `$7B92`, `$9006`, ... (not the call from inside `music_init`, return `$1A00C`) | none: the pack track stops |

Measured on `level1_play` (title at frame 2, leaving the title at 461, GET READY, level 1 at 974): `2 title caller 008236`, `461 stop 008678`,
`974 level 00789C`. The shop and the ending run their own driver (`$2090` is not the resident tick), boss fights keep the level tune:
neither is a cue, so there is no slot for them and a pack track simply continues through them (as the tracker does).

### CDTV release

The game plays CD tracks itself. Its CD-play routine (trap `$3A924`, hunk offset `$3A524`) is called from a handful of places; the cue
layer sees the track and the caller. The track depends on the player's SELECT TITLES choice, so the exact key is the CD track (`cdN`);
the aliases say *where* the game asked:

| Role | Callers (return address) | What the game is doing | Level word `$E0A` |
|---|---|---|---|
| `title` | `$3C822`, `$3B95C`, `$3AD6A` | the title and attract sequence (frames 20 and 430 in every replay) and the SELECT screen (`$3AD6A`, frame 1842) | not used |
| `levelN` / `level` | `$3ADB6` (game start after GET READY), `$3B45A` (level change), `$3B2D6` (back in the level after the shop) | a level starts | N = `$E0A`, 1-5 |
| none (`cdN` only) | everything else, e.g. `$5047E`'s info and picture pages (tracks 8-13) | CDTV-only screens | |

`$3AD6A` is a judgement call. The game plays the *level 1* track there (it indexes its table with level 1 while the SELECT screen is
built), but the level has not started, and the screen belongs with the title music for a listener. A pack that wants otherwise names
`cd3` (the exact key beats any alias). Measured on `tests/replays/cdtv/level1_play`: `20 play 2 03C822`, `430 play 2 03B95C`,
`1842 play 3 03AD6A`, `2159 play 3 03ADB6`; `cd_play` always stops the running track first, so every play cue is preceded by a stop cue at
the same output frame.

Resolution order for a cue (first slot the library has *and that opens*): exact (`cdN` on CDTV), role alias (`title`, or `levelN`), then
`level` (levels only). A slot that cannot be used is skipped with one warning and the next candidate is tried; if none is left the
original music plays. On the disk build the CD track of `--cdtv` (a user's own disc audio) comes after the pack, then the tracker.

### Atari ST

**Not mapped, and not part of this release** (the plan, decision U5: the ST gets screens and the panel only). What is known
(`atari-st-s0.md`, `atari-st-sound.md`): the engine's music and effects driver runs once per VBL and writes YM registers 0-10 (about 11
writes per frame, no envelope); the front end plays its title music and a digitised sample through a Timer A handler at `$40C94`; the tune
number is a variable of the engine and the tune count was never measured. Music and effects share the three YM voices, so muting the chip
for a pack track would also silence effects until the driver's start and stop entries and the voice allocation are found. That is the
work for the release that adds ST audio; the slot names `title`, `level1-5`, `level` are reserved for it and the loader already accepts
`games: ["st"]`.

## 2. The engine as built

**`StreamSource` / `StreamSpec`** (`audio/stream_source.h`). A source is a random-access stereo stream of floats at the int16 scale,
silent outside its length. A spec is how to play one: source rate and length, a factory that opens a fresh reader per voice, the loop rule,
gain, crossfade and fades. A CD track (`cd_stream_spec`), a decoded pack file (`DecoderStream`), an in-memory buffer and an intro followed
by a body (`ConcatStream`) all play through the same voices. `CdPlayer::play(TrackSource*, n)` is unchanged for the CD and builds its
spec; `play(spec, n)` is the general entry.

**`CdPlayer`.** A voice carries its own source rate, a resampling kernel (one per source rate, built when the first voice of that rate
starts: same 32-tap Kaiser-sinc, 256 phases, cut-off 0.46 x the lower rate; a source at the output rate is copied), gain and its own loop
state. Everything else is as before: equal-power fades, volume ramp, pause modes, exact rational source positions, `FP_CONTRACT` off (now
also a per-file compiler flag for `cd_player.cpp`, which relied on the pragma alone). The loop rules:

| Rule | Behaviour | Used by |
|---|---|---|
| `Game` | at the end the voice finishes; the game's own poll (every 50 ticks on the disk build, `cue_poll` on CDTV) asks `finished()` and restarts the whole stream from its start, intro included, up to a second late. With the player's own crossfade (`--cd-crossfade`) the CD restarts early, as before | CD tracks; pack slots with `loop_mode: "game"` or no declared loop |
| `Seamless`, no crossfade | the read position wraps inside the voice: positions past `loop_end` map back into `[loop_start, loop_end)`, so the filter taps run across the wrap and the loop is sample-exact at any source rate | pack loops |
| `Seamless`, crossfade | the next pass starts `crossfade` output frames before `loop_end` at `loop_start`, equal-power overlap, as the CD's crossfaded loop (the loop's period is shorter by the overlap; the overlap is limited to half the loop) | pack loops with `crossfade_ms > 0` |
| `Off` | plays once, then silence. `finished()` stays false so the CDTV game's poll never restarts it | `loop: false` |

Track changes crossfade over the new slot's `crossfade_ms` (equal-power, the old voice fades as long); with 0 the old voice fades over its own
`fade_out_ms`, at least 5 ms (click guard). `fade_in_ms` applies when the track starts from silence. `stop` fades over `fade_out_ms`, at least
5 ms. `gain_db` is a linear gain on top of the player's volume. The player's output level for a full-scale file is the CD's (0.75 of int16 full scale before the volume), so pack music and CD
audio sit at the same level.

**`MusicSlots`** (`audio/music_slots.h`). Resolves a list of slot names against the loaded library; builds each slot once, at its first resolve (or
`prepare()`), and caches it. Building opens the file through `PackLibrary::open_file` and the decoder, checks the header against `DecodeLimits`
(mono or stereo only, rates 8-192 kHz), learns the length (counting frames once if the format does not say), decodes and keeps the first 16384
frames (so a cue starts without decoding), and fixes the loop. Voices open their own decoder lazily after the first buffer and keep an
8192-frame cache; a loop wrap costs one seek per pass. A decoder that fails mid-stream (damaged tail, failed seek) makes the rest of that
voice silent with one message; the game goes on.

Loop decisions (`loop` is the manifest's, `loop_mode` defaults to `pack`):

| Manifest | Result |
|---|---|
| `loop: false` | `Off` |
| `loop: true` | whole body, `Seamless` (`Game` with `loop_mode: "game"`) |
| `loop: {start_s, end_s?}` | `Seamless` on those points in the *body file* (the intro, if any, is played once before it); `end_s` past the end of the file is clamped with a warning, `start_s` not before `end_s` loops the whole body with a warning; `Game` with `loop_mode: "game"` (the points are ignored) |
| nothing | the file's own loop (WAV `smpl`, or tags `LOOPSTART`/`LOOPLENGTH`/`LOOPEND`) as `Seamless`; with none, the original's rule: `Game`, the whole stream again |
| `mute: true` | silence, whatever else the slot names (a `file` left in place is not read) |

`intro` must decode like the body; at a different sample rate it is resampled once to the body's (with the decoder's StreamResampler) and kept in
memory. A broken intro drops the slot. A slot with no `file` and no `mute` is ignored with a warning.

**`CdtvMusic`.** The cue handlers resolve the slot where the cue happens, inside the emulation (the trap, or the CDTV CD-play call), because
the disk build must stop the tracker at the instruction that starts it: if a slot matches, Paula's tracker gain is set to 0 there and then
(`set_source_gains`; the mix only, no machine state, and `--protect-music`'s mirror voices are switched off while a slot owns the music) and the
pack track starts at the cue's exact output frame in the same stage that renders the CD. Effects keep their gain. If the next cue finds no
slot, the tracker is unmuted at that cue.

- `retrigger: "continue"`: a cue for the slot that is already playing does nothing. The CDTV game always stops before it plays, so a stop
  cue followed by a play cue for the same continuing slot at the same output frame is dropped together.
- A finished pack track on the CDTV release with `Game` rule is restarted by the game's own poll (`cue_poll` sees the player's `finished()`), as a
  CD track is.
- Switching packs off or on mid-tune (the menu's Faithful switch and `--faithful`, `--modern audio|noaudio`): `main.cpp` refreshes the global pack settings when the picture
  options change, the cue layer notices and decides the last cue again: the pack track fades and the original plays, or the other way round. The
  tracker is never stopped, so unmuting is instant and in phase.
- The layer is installed when packs are configured (`--asset-pack`, `packs_enabled`) as well as for the window and CD options; headless
  runs without packs and without music options are untouched.
- Nothing is rendered unless the CD or a pack voice is playing: the audio of every existing run is unchanged.

## 3. Evidence

- **Existing audio goldens unchanged.** `cdtv_audio_*` (CD tracks mixed into the CDTV replays), the 219 checks of `x2_cdtv_tests`
  (player, resampler, crossfade, pause) and every replay's frame and audio hashes pass on the generalised player.
- **Synthetic pack on the real games** (`music_pack_disk`, `music_pack_cdtv`: a pack of integer-waveform WAV files written by the test
  executable, so nothing depends on libm; slots `title`, `level1` with an intro and a 100 ms crossfade, `level` with intro and loop points):

  | Run (`level1_play`, audio hash at frame 5500) | Disk | CDTV |
  |---|---|---|
  | corpus (no pack) | `95519b8af37ff3e1` | `5cc3dadfb8f2e3c7` |
  | with the pack | `a2780a37c109ac42` | `7e1732632d49c96c` |
  | with the pack and `--faithful`, and with `--modern noaudio` | the corpus's | the corpus's |

  The frame hashes with the pack equal the corpus's at every checked frame (the pack changes only the output mix), and the cue log names the
  slots (`title 2 008236 pack:title`, `level 3 00789C pack:level1`; CDTV `play 2 03C822 pack:title`, `play 3 03ADB6 pack:level1`). Two runs give
  the same hashes (`--interp-only` too; not run on a `-DX2_RECOMP=ON` build, but the traps are the CD layer's mechanism, which the recompiled-build tests
  already cover). A spectrum check of an early run on the disk replay with a 330 Hz title, an 880 Hz intro and a 550 Hz level body: the title window peaks at
  330 Hz, the first second of the level at 880 Hz, then 550 Hz, each with rms 4773 (the files' 9000 x 0.75 / sqrt 2, so no tracker underneath), and the
  gap after the stop is exactly silent. (That replay has no audible effects with the music muted, with or without a pack; Paula's per-source gains
  leave effects untouched, which `x2_cdtv_tests` checks on the Paula model.)
- **Real loader, directory and ZIP.** The packs now come from `PackStack` (WS1b), loaded before the cue layer is created; the layer exists whenever a loaded pack has audio (`--asset-pack`, or `packs_enabled` with `packs/` discovery). `music_pack_disk_zip` and `music_pack_cdtv_zip` run the same pack as a ZIP written with stored members (`x2_music_slots_tests - --write-pack OUT.zip GAME zip`): the loader streams the audio from the archive (`ZipSource::open` of a stored member reads at offsets, nothing is inflated), and all hashes equal the directory run's (the same recorded manifests).
- **Unit tests** (`x2_music_slots_tests`, 109 checks, 0.2 s; the same under ASan and UBSan, 13 s): streams and concatenation; a stream at the
  output rate is copied exactly at the CD gain times the slot gain; fades; the game rule restarts at the poll and `Off` stays silent; the wrapped
  loop is sample-exact (periodic mapping of a noise signal) and, resampled 22.05 to 48 kHz, a sine with whole cycles in the loop keeps SNR
  above 55 dB across about 30 wraps; identical samples for any render chunking; the crossfaded loop never drops out; intro then body forever; slot
  names; resolution order; loop decisions for every manifest form; intro resampling; a 90,000-frame file played through 160,000 frames
  of wraps with seeks; damaged files (junk, empty, no file, truncated) and 750 random mutations of WAV, FLAC and Vorbis through the whole path;
  FLAC and Vorbis fixtures at 22.05 and 44.1 kHz through the player; the CDTV cue layer on a synthetic machine (roles, exact beats alias, mute,
  retrigger, the game's loop rule, F1 and the audio toggle, reset); the disk cue layer with a Paula tone standing in for the tracker (gone from
  the first sample with a slot, back without one; `title` and `level` are separate cues).

## 4. Limits and gaps

- ST music: not mapped (section 1).
- The disk game's boss fights, the shop and the ending have no cue: a pack cannot give them their own track.
- A pack track is decoded on the audio path: the first buffer is ready, but a loop seek or a first read past it decodes there. Pack music in
  a ZIP should be stored, not deflated (the library inflates a deflated member into memory once).
- Loop points are in seconds converted to frames of the body file at its own rate; a FLAC or Vorbis whose tags declare a loop uses frames of
  the file as the decoder reports them.
- A Seamless loop with a crossfade starts the next pass at `loop_start`, not before it; authors who want the usual "fade the tail into the
  material before the loop start" bake that into the file (or use no crossfade and exact points).
- `--dump-audio`'s `music_cues.txt` (slot discovery) is WS3b's; the `--cdtv-log` shows the cues and the matched slot meanwhile.
