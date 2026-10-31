# CDTV release: CD audio, speech and effects (phase C4)

Phase C4 of `.omc/plans/xenon2-cdtv-target-plan.md`. It checks what the runtime does with the CDTV release's audio against what the program
asks for, and records the evidence. Builds on `cdtv-c0.md` (sections 2.3-2.4 and 5), `cdtv-boot.md`, `cdtv-music-map.md` and `cdtv-reference.md`.
No game bytes are quoted: addresses, counts and measured figures only. Addresses are **absolute** (hunk offset + `$400`).

## 1. The CD requests, and what changed

The program talks to the CD through five routines: stop (`$3A8F6`), play track `d0.w` (`$3A924`), the once-a-VBL poll (`$3A95C`, called
from the level-3 handler at `$350B8`), volume full (`$3A9BA`) and volume mute (`$3A9E6`). The runtime replaces each with a PC trap
(`runtime/cdtv/cdtv_game.cpp`); nothing in the image is changed.

**The poll is now the game's own.** C3 left the poll routine empty and let the CD player loop a track on its own clock, 50 ticks after the
play. The original's poll is different in two ways, both visible in the routine:

- Its counter (`$3A8F0`, a word the routine keeps in the game's memory) starts at 0 and is reloaded with `$32`; it counts down with
  `subq.w #1 ; bpl`, so it comes round every **51** ticks, not 50, and it **does not stop or restart on a play**: it only runs while a track is
  selected (`$3A8F2 >= 0`). The first poll comes with the first tick after a play (the counter starts at 0), the next 51 ticks later, and so on, in
  a phase that depends on how many ticks the game spent with music selected.
- When a poll comes it asks cdtv.device whether the request has completed (`CheckIO`) and, if so, sends the same request again.

The trap now does exactly that: it keeps the counter in the game's memory (`cd_poll_step`, tested step by step), and at each poll asks the CD
player whether the track has played out (`CdPlayer::finished`); if so the track is started again (`CdtvMusic::cue_poll`), at the first poll after its
end, up to 51 ticks (1.02 s) late, in the original's phase. The player's own loop clock is switched off in this build
(`CdPlayer::set_self_loop(false)`). With `--cd-crossfade` the player still loops itself (a deliberate, opt-in deviation: the restart starts before
the end, so the game's poll never finds the track finished).

Not modelled, and no evidence either way: the **latency of a real drive** (seek, spin-up) between the program's request and the first sound,
and between a track's end and the restarted track. The program has no timeout that would reveal it; Amiberry's CD emulation does not model a
mechanical drive either, so the reference gives no number. The CD therefore starts at the request frame, with no delay, and a loop restart
waits for the next poll only.

## 2. State to track: the table the program uses

The program picks tracks from three byte tables and one fixed value. "Observed" is a play request seen while the game ran (`--cdtv-log`, the caller is
the return address of the call); "static" is read from the code and the table only.

| State | Track | Source | Evidence |
|---|---|---|---|
| Title and attract (boot, and after the title pages) | 2 | byte table `$3F226` (the "titles" slot), calls at `$3C81E` and `$3B958` | observed: frame 20 (caller `$3C822`) and frame 430 (`$3B95C`) |
| SELECT screen and the start of a game | 3 | byte table `$3F227[level-1]` with level 1, call at `$3AD66` (the game-start path runs when the SELECT screen is shown) | observed: frame 1842 (`$3AD6A`) |
| Level 1 | 3 | `$3F227[0]` = 3, calls `$3ADB2`, `$3B2D2`, `$3B382`, `$3B456` | observed: frame 2159 (`$3ADB6`), after the level loads |
| Level 2 | 5 | `$3F227[1]` | static (the same call sites with `$0A0A` = 2) |
| Level 3 | 2 | `$3F227[2]` | static |
| Level 4 | 3 | `$3F227[3]` | static |
| Level 5 | 5 | `$3F227[4]` | static |
| Music select ("SELECT TITLES") | the chosen slot's track | `$3F226[slot]`, call at `$3E41E`; the screen edits the table | static |
| Information pages (the "OUR MUSIC", "THE MUSICIANS AND ARTISTS", "WHAT MUSIC'S ON THIS CD" pages) | 8, 9, 10, 11, 12, 13 by page | table `$5087E[page]`, calls at `$509D2`, `$50A1A`; entries of `255` mean no track | static |
| Shop, bosses, game over, ending | no request: the level's track continues | no `play` call on those paths (`cdtv-music-map.md`) | static |

The tables are `02 03 05 02 03 05` (title, levels 1-5), the same as the disk-build layer's `--cdtv-tracks` default. Levels 2-5 are static because
no replay of this build reaches them yet (phase C6 adds the coverage pokes and replays that will observe them). An information-page entry of 255 asks
the drive for a track that does not exist: cdtv.device returns an error, the request completes at once and the poll sends it again at every poll, so
nothing is heard; the runtime gives the same result (an unknown track stops the player).

The request calls are followed at every call site by volume full (`$3A9BA`); `$3A9E6` (volume mute) has **no caller anywhere in the program or
its overlays** (a search for `jsr`/`bsr` and for its address as data finds none), so the volume is only ever set to full (`$7FFF`), which is the
device's default: there is no volume curve to map. The trap sets the music volume to 100% of the user's `--music-volume`.

## 3. MUSIC ON / MUSIC OFF

The SELECT screen's "MUSIC ON/OFF" toggles one byte, `$A02` (`not.b $a02`, at `$3CBDA`). It is set to `$FF` (on) by `$3AC5A` at start-up
and tested at five places (`$3ADF4`, `$3B2B2`, `$3B362`, `$3B436`, `$3CACC`): the menu text, the replays of the level's track after the shop and other
interruptions (`$3B2B2` and its siblings skip the `play` when off), and a stop after the level (`$3ADF4`). **Toggling does not stop or start anything by itself**:
the cue log shows no CD request at the toggle. It also does **not gate the first play of a game**: with MUSIC OFF selected, starting a game still
requests level 1's track (observed: frame 2258 in a run that selected OFF at frame 1960, `$A02` stays 0). That is what the program's code does as
it reads, and the runtime executes that code unchanged; it is **not checked against the reference emulator**, which would need an input script that
selects OFF (the Amiberry harness takes one).

## 4. Pause, stop, quit, focus

- **Pause.** The program never reads the keyboard (`cdtv-c0.md` §4: the only key code ever stored is `$40`, from the second button), so it
  has no pause of its own, and `--cd-pause` has nothing to hook (the option now says so). Pausing the emulation itself (the host's pause) stops the
  emulated clock, and the CD player runs in emulated time, so the CD is held with everything else.
- **Host reset (F12) and quit.** The machine is rebooted or ends: `CdtvMusic::reset` stops the CD player; the game starts its music again from boot.
- **Window focus.** Unchanged from the disk build: the window layer decides whether to keep running.
- **The music key** (the disk build's source switch) now mutes and unmutes the CD for the session in the CDTV release, since there is no tracker
  tune to switch to; the saved music setting belongs to the disk build and is left alone.
- **No CD tracks imported.** The game runs, the music is silent, and one line says so (`the music will be silent (import the disc with --import
  PATH ...)`); the CDTV start-up line also says `off (no CD tracks imported: the music is silent)`.

## 5. Speech and effects

One player serves both: the effect player (`$330B6-$3321E`, polled from the level-3 handler) writes Paula directly. Evidence from the per-frame
register logs (`--dump-dir`; `tools/cdtv_audio_check.py events`) and the audio render (`--audio-out`; `tools/cdtv_audio_check.py speech`), on the
menus and level-1 replays:

- **Channels and pan.** Effects start in **pairs**: channels 0 and 1 (command slot `$16`) or 2 and 3 (slot `$1C`), with different volumes for the
  pan (for example volumes 4 and 60: right). Paula's hardware mapping puts channels 0 and 3 on the left output and 1 and 2 on the right, so pair 0/1
  and pair 2/3 are mirror images, as the pan values intend. In the render the left/right level ratio of such a pair matches the volume ratio (60/4 = 15
  expected; 15.1 measured; 30/2 = 15 expected, 15.0 measured on an effect).
- **The start sequence.** Each start writes the volumes and a period of 1 to both channels, clears and sets `DMACON`, writes
  pointer, length, volume and period, enables DMA, waits (`dbra` loops of `$C7` and `$63` iterations) and then writes the pointer and length
  again: the second pair is the buffer Paula reloads when the first runs out. For a one-shot effect the second pair points at a short silence buffer
  (`$88CE8`, 2 words), so the sound plays once and then the channel plays silence. This depends on when Paula latches the first pointer; the
  accurate-timing model and `--timing fast` give the same result (below), so the sequence is not sensitive to that.
- **Speech is exact.** `SPEECH63`, `SPEECH50` and `SPEECH37` are loaded to the speech buffer (`$B645C`) and played whole, mono, at period 298
  (11,902 Hz). Correlating the render with each file (8-bit samples, played at that period) gives a normalised correlation of **0.992-0.994** for a
  speech played alone at full volume (frame 431: gain 94.5 per sample unit on the left channel, 94.7 on the right, in both timing modes), **0.993**
  on the louder channel of a panned one (frame 1554; gains 88.5 and 5.9 = 60/64 and 4/64 of 94.5), 0.876 for one that was cut short.
- **Speech can be cut short, as in the program.** At frame 1253 a short silent effect (the menu's "blip") starts on channels 0/1, the pair
  the speech uses, 59 frames into the 4.5 s speech that began at frame 1194: the effect player gives the pair to the newer sound, so the speech ends there. That
  is the effect allocator in the program, not a runtime artefact; it explains the low correlation (0.57) for that speech.
- **Music and effects never compete.** The music is the CD, mixed by the host (`CdPlayer::render` adds into Paula's output frames, saturating);
  Paula's channels carry only effects and speech, so there is no channel stealing and no tune to drop out (the disk build's music dropouts have
  no counterpart here).
- **Reference.** The Amiberry CDTV run (`cdtv-reference.md`) could not be used to compare speech or effects: its picture and event timing are
  not aligned with ours, and an effect-only stretch to correlate against was not found in the 120 s of the run.

## 6. Mix level

Amiberry's CD audio, measured against the disc's tracks in the reference run (windowed cross-correlation of its mixed output against each track;
the level-1 music, track 3, is found at a normalised correlation 0.5-0.63 with effects on top), comes out at about **0.4-0.5 of the file's
amplitude**. That figure belongs to the emulator's own setting (`sound_volume_cd`, 20% attenuation by default) and its mixer scale: it is not a
measurement of the CDTV's analogue mix, which has not been measured. The runtime keeps `CdPlayer::kCdGain = 0.75`, which gives the CD the same output
headroom as Paula (Paula's output gain is 0.75). The relative level of CD and effects can be set to taste with `--music-volume` and
`--sfx-volume`. This is the one item that needs the user's ear.

## 7. Tests and tools

- `cdtv_game_runtime` (synthetic, no game data): the poll counter step by step (first poll with the first tick, then every 51), the poll loop on a
  synthetic 0.68 s track against polls every 51 frames (restarts at frames 52, 103, 154, never while the track plays, not for another track, not when
  stopped, not with the CD off), and the traps.
- `cdtv_audio_menus`, `cdtv_audio_level1_play` (needs the imported release and the disc's audio; Skipped otherwise): the replay with the CD
  mixed in leaves the frame hashes untouched, the audio hashes with the CD are `tests/replays/cdtv/NAME.cd.hashes` and the game's CD requests are
  `NAME.cues`; with no CD tracks the music is silent, said once, and the hashes are the CD-free committed ones. The replays are 6,000 frames, shorter than
  any track, so the loop restart is covered by the synthetic test only.
- **Determinism of the CD hashes.** The CD resampler rounds in float. Before this phase a Debug build with the sanitizers and the Release build gave
  different audio hashes with the CD mixed in (fused multiply-add in one and not the other); `cd_player.cpp` now carries
  `#pragma STDC FP_CONTRACT OFF`, and the two builds give the same hash, the recorded one. The goldens were recorded on arm64 macOS; another
  architecture is expected to agree (no fusing, IEEE single and double) but has not been checked: a mismatch there would show only in the
  `cdtv_audio_*` audio hashes, never in the frame hashes or the cue lists.
- `tools/cdtv_audio_check.py`: `events` lists the Paula starts from a register log; `speech` correlates a render with the speech file a start played.

## 8. What is not known

- The drive's real latency and the CDTV's analogue mix level (sections 1 and 6).
- Levels 2-5, the shop, bosses and ending have not been run in this build; their tracks are read from the table (section 2).
- Whether MUSIC OFF not gating the first play is what the original does on hardware (section 3).
