# Tutorial: replace a sound effect

You will dump the sounds of your own copy, pick one, make a replacement of the same duration and pitch, and have the game play yours in its
place. The same steps work for the CDTV release's effects and (under `audio.speech`) its speech.

Run commands from a source checkout; `x2recomp` is the built program (add `--assets DIR` if your game is not imported).

## 1. Dump the sounds

```
x2recomp --headless --input-script tests/replays/level1_play.txt --dump-audio my-dump
```

```
dump-audio: 8 sound effects, 0 speech clips -> my-dump/manifest.json
```

`my-dump/sfx/` has one WAV per distinct effect, named by its **key**: the hash and byte length of the original sample (`2b086881fb6a3b63-4160`). The
manifest lists each with what the dump learned:

```json
"2b086881fb6a3b63-4160": {"file": "sfx/2b086881fb6a3b63-4160.wav", "ref_period": 357, "bytes": 4160, "uses": 944,
                          "first_frame": 985, "channels": 4, "periods": [357, 357], "peak": 0.992, "source": "effect"}
```

`uses` says how often it played in the replay (the weapon fire here: 944), `channels` which Paula channels played it (a bit mask), `periods` the lowest and
highest pitch settings the game used. A dump is itself a valid pack that plays the originals, so you can load it as a pack to see that the
path works: `x2recomp ... --asset-pack my-dump`. Never distribute a dump.

Which sound is which? Play `my-dump/sfx/*.wav` and look at `uses`: the sound with hundreds of uses in a shooting replay is the weapon. Use
`first_frame` to match a sound to what the replay does at that moment. To hear your pick in context use `--audio-out` (below).

## 2. Make a replacement

A replacement is any WAV, FLAC or Ogg Vorbis file, mono or stereo, at most 120 seconds. For it to sound right in the game:

- **Duration and character.** Make it about as long as the original (the dump's WAV is the reference; this one is 419 ms), so it does not
  cut into the next sound or leave a gap. The game cuts a sound when the same channel starts another, so a long tail is lost.
- **Pitch.** `ref_period` is the Paula period at which your file sounds like the original sounded when played normally; the dump
  wrote 357 for this one. The game changes the period for pitch bends and sweeps, and the voice follows the change relative to `ref_period`.
  If you keep the pitch of the original, copy `ref_period` from the dump. If you want the sound at its own rate, set `"pitch": "fixed"`.
- **Level.** The dump plays back at the original's level with `gain_db` 0. Normalise your file to about the same loudness.

As a stand-in, a falling chirp of the same duration:

```python
import json, math, struct, wave

key = "2b086881fb6a3b63-4160"                       # the key of the sound you chose
dump = json.load(open("my-dump/manifest.json"))
entry = dump["audio"]["sfx"][key]
src = wave.open("my-dump/" + entry["file"])
seconds = src.getnframes() / src.getframerate()

rate = 44100
n = int(rate * seconds)
out = wave.open("my-pack/sfx/fire.wav", "wb"); out.setnchannels(1); out.setsampwidth(2); out.setframerate(rate)
out.writeframes(b"".join(
    struct.pack("<h", int(14000 * (1 - i / n) * math.sin(2 * math.pi * (1800 - 1400 * i / n) * i / rate)))
    for i in range(n)))
out.close()
print(entry["ref_period"])
```

(Run `mkdir -p my-pack/sfx` first.)

## 3. Write the manifest

```json
{
  "format": "x2-asset-pack",
  "version": 2,
  "id": "my.fire-sound",
  "name": "My weapon sound",
  "pack_version": "0.1.0",
  "license": "CC0-1.0",
  "provides": ["audio"],
  "audio": {
    "sfx": {
      "2b086881fb6a3b63-4160": {
        "file": "sfx/fire.wav",
        "ref_period": 357,
        "gain_db": -3
      }
    }
  }
}
```

The other fields have good defaults: `pan: "channel"` puts a mono file on the side of the channel that played the original, `pitch: "follow"`
tracks the game's period changes, `cut: "channel"` ends it when the channel gets a new sound, `original: "mute"` mutes the original in the
mix, `loop: false`. If the game loops a sound (a sweep that repeats), set `"loop": true`; the dump sets that for you. To hear your sound
over a quiet original instead of replacing it, `"original": "mix:-12"`.

Another build may use a different sample for the same sound (the Power Pack's samples are lower precision). Add the other build's key to
`keys`: `"keys": ["abcdef0123456789-3984"]`, and the same file plays for both.

## 4. Check and listen

```
x2recomp --check-asset-pack my-pack --against my-dump --full
```

```
info: asset pack: My weapon sound: against: coverage of the dump: ..., sound effects 1/8, speech 0/0
check: My weapon sound: 0 errors, 0 warnings, 1 entries used, 0 skipped: valid
```

`--full` decodes the file and reports clipping. Then render a run with and without the pack and listen to both:

```
x2recomp --headless --input-script tests/replays/level1_play.txt --frames 1500 --asset-pack my-pack --audio-out with-pack.wav
x2recomp --headless --input-script tests/replays/level1_play.txt --frames 1500 --audio-out original.wav
```

With the pack, `--audio-hash --hash-at 1499` gives a different hash from the plain run, and `--modern noaudio` or `--faithful` gives the plain run's
hash back: the pack changes only the mix, never the machine. `--sfx-volume 0` silences the originals and the pack's effects alike.

In the window, `x2recomp --asset-pack my-pack`, play, and use the F1 menu's Faithful row to compare against the original.

## 5. Troubleshooting

- **Nothing changes.** The key is not the one the game plays in your build: `--check-asset-pack --against my-dump` reports keys the dump never saw. Check that the
  dump came from the same build. A pair of keys can differ between builds.
- **The sound is cut short.** Your file may be shorter than the original's playing time, or the game started another sound on the channel (`cut: "channel"`).
  Use `"cut": "never"` to let a long tail ring out.
- **The pitch is wrong.** `ref_period` is for the pitch where your file sounds natural; a replacement recorded an octave lower needs a `ref_period` twice as
  large. Or use `pitch: "fixed"`.
- **Too quiet or loud.** Adjust `gain_db`; the voice also follows the game's own volume for the channel.
- **It plays twice, or on both sides.** The game starts some sounds on two channels at once; a mono replacement plays once per start, as
  the original does. Use a stereo file to play both sides from one voice.
- **The disk build lists "instrument" samples.** In the shop and ending the game plays its tune through a different player, and a dump
  of those screens lists the tune's samples as effects. Replace them one by one if you want, or leave them out.
