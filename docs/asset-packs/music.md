# Tutorial: replace the music

You will make a pack whose own tracks play at the game's music cues: a title tune, and a level tune with an intro and a loop. The tracks here
are generated tones so you can run every step without any music; replace them with your own recordings (WAV, FLAC or Ogg Vorbis).

Run commands from a source checkout; `x2recomp` is the built program (add `--assets DIR` if your game is not imported).

## 1. Learn where the cues are

A **cue** is the moment the game starts, changes or stops its music. A pack replaces the music of a cue by naming its **slot**:

| Slot | Disk builds | CDTV release |
|---|---|---|
| `title` | title tune | title, attract and SELECT screens |
| `level1` to `level5` | start of that level | start of that level |
| `level` | any level without its own slot | the same |
| `cd2` to `cd13` | not used | one CD track exactly |

To see the cues of your build, log them while a replay runs. On the disk builds:

```
x2recomp --headless --input-script tests/replays/level1_play.txt --frames 1500 --audio-cues cues.txt
```

```
# frame line event d0 caller(return address)
2 159 music_init 0000 008236
461 304 music_stop 0009 008678
974 273 music_init 0000 00789C
```

The call from `008236` is the title tune; from `00789C`, a level start. On the CDTV release the same job is `--game cdtv --cdtv-log cues.txt`, which
logs the `play` calls with the CD track and the caller. You do not need the addresses for a pack: you only need to know that a cue exists
for the title and for each level. Boss fights, the shop and the ending have no cue, so a pack track simply continues through them.

## 2. Make some tracks

If you only want to see the steps work, this makes two short test tracks (a 2 second melody for the title and the same shape lower for the
level):

```python
import math, struct, wave

def melody(path, freqs, seconds_each, rate=44100, amp=9000):
    w = wave.open(path, "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
    out = bytearray()
    for f in freqs:
        n = int(rate * seconds_each)
        for i in range(n):
            env = min(1.0, i / 400, (n - i) / 400)            # short fades so the notes do not click
            v = int(amp * env * math.sin(2 * math.pi * f * i / rate))
            out += struct.pack("<hh", v, v)
    w.writeframes(bytes(out)); w.close()

melody("my-pack/music/title.wav",  [330, 392, 440, 392], 0.5)
melody("my-pack/music/level1.wav", [220, 262, 330, 262], 0.5)
```

(Run `mkdir -p my-pack/music` first.) Real tracks: any rate from 8000 to 192000 Hz, mono or stereo, up to two hours; they are decoded
as they play, and resampled with a high-quality filter.

## 3. Write the manifest

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.music",
  "name": "My music",
  "pack_version": "0.1.0",
  "license": "CC0-1.0",
  "provides": ["audio"],
  "audio": {
    "music": {
      "defaults": {"crossfade_ms": 250},
      "slots": {
        "title": {"file": "music/title.wav", "loop": true},
        "level1": {"file": "music/level1.wav", "loop": {"start_s": 0.5, "end_s": 2.0}, "gain_db": -3}
      }
    }
  }
}
```

- `loop: true` repeats the whole file. `{"start_s": 0.5, "end_s": 2.0}` plays from the start, then repeats the part between those
  seconds forever, sample-exact. For a tune with an intro, give the intro as a separate file: `"intro": "music/l1_intro.flac", "file":
  "music/l1_loop.flac", "loop": true` plays the intro once, then the loop.
- `crossfade_ms` overlaps one track into the next (and at the loop point); `fade_in_ms` and `fade_out_ms` shape starts and stops.
- `gain_db` (-60 to 24) sets the level; `--music-volume` still scales it.
- With no `loop`, the file's own loop markers are used (a WAV `smpl` chunk, or `LOOPSTART` and `LOOPLENGTH` tags in FLAC and Ogg) and with none,
  the game's rule: a finished track is started again by the game's own poll, up to a second late.
- `{"file": null, "mute": true}` gives a level silence.

## 4. Check, listen, compare

```
x2recomp --check-asset-pack my-pack
```

```
info: asset pack: My music: loaded: 0 graphics, 0 far lines, 0 palettes, 2 music slots (...)
check: My music: 0 errors, 0 warnings, 2 entries used, 0 skipped: valid
```

The checker tells you about loop points past the end of a file, clipping and deflated audio in a ZIP. Then render the sound of a run to a file:

```
x2recomp --headless --input-script tests/replays/level1_play.txt --frames 1500 --asset-pack my-pack --audio-out with-pack.wav
x2recomp --headless --input-script tests/replays/level1_play.txt --frames 1500 --asset-pack my-pack --faithful --audio-out faithful.wav
```

`with-pack.wav` has your tracks at the cues; `faithful.wav` is the original, bit for bit. (`--audio-hash --hash-at 1499` prints a hash of the sound up
to that frame: the pack changes it, `--faithful` gives the original's.) In the window: `x2recomp --asset-pack my-pack`, press F1 to hear the original
tune at once, and F1 again to return.

## 5. Detail worth knowing

- The original music is muted in the mix **from the instruction that starts it**, so the tracker never leaks. Sound effects keep playing.
- A slot that cannot be opened (missing file, damaged file) is skipped with one warning; the next candidate is tried, and when none is left the
  original music plays.
- `retrigger: "continue"` leaves a slot running when its cue comes again; the default `restart` starts it again, as the game does after a death.
- Put music in a ZIP stored, not compressed: `zip -r -n .wav:.flac:.ogg ../my-pack.zip .` from inside the folder.
- `--music cdtv` (a disk build with your own CDTV disc's tracks) comes after a pack slot in the order of precedence: the pack wins.
- The Atari ST release has no music slots yet.
