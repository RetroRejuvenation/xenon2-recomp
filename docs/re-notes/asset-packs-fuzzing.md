# Asset packs: fuzzing, sanitizers, integration tests and cost gates (WS7)

The files the pack code reads (a manifest, a ZIP, a PNG, a WAV, a FLAC or an Ogg Vorbis file) come from the Internet, so they are tested
as hostile input. This note says what runs where, how to run it by hand, what it found, and which gates were set wide on purpose.
Design: `asset-packs-v2-design.md` section 9.

## Targets

`tests/fuzz/` holds six libFuzzer entry points. Each is an ordinary `LLVMFuzzerTestOneInput`, so the same source runs as a coverage-guided fuzzer
(Clang, `-DX2_FUZZ=ON`) and as a plain test (every compiler, every platform).

| Target | Reads | Checks besides "no crash, no UB, no hang" |
|---|---|---|
| `x2_fuzz_manifest` | `manifest.json` through `parse_json` and `parse_manifest` (first byte: validator on or off) | a refusal always says why; accepted graphic and sample keys are valid; the entry cap holds; every message formats |
| `x2_fuzz_zip` | a ZIP pack through `open_pack_source` (names, central directory, limits, inflate, CRC, streams, seeks past the end) | |
| `x2_fuzz_png` | a PNG through `png_size` and `decode_png` with small limits | the picture's pixel count matches its size |
| `x2_fuzz_wav`, `x2_fuzz_flac`, `x2_fuzz_vorbis` | one file kind each through `AudioDecoder` (streaming reads, seeks, whole-file decode with conversion to 22,050 Hz stereo) | the channel count is 1 or 2; every sample is finite; a converted clip has `frames * 2` samples |

The audio targets force the container's magic onto the input, so the mutations land in the decoder instead of the format sniffer.
`tests/fuzz/manifest.dict` gives the manifest target the schema's words.

**The corpus is generated, not checked in.** `x2_fuzz_seeds` (`tests/fuzz/fuzz_seeds.cpp`) writes it from synthetic data (the helpers of
`tests/support/synth.h`: PNG, ZIP and WAV writers) and the tone fixtures of `tests/audio/fixtures`; the hygiene check never has a binary seed
directory to allow. It also holds the inputs of every bug a fuzzer found (`regress_*`), written by the generator.

## Running

- **As tests** (any build): `ctest -R fuzz_` runs `fuzz_seeds` and then each target over its seed corpus, about a second in all. In a
  sanitizer build (below) this is the regression guard for every finding.
- **Sanitizers**: `cmake -S . -B build-san -DX2_SANITIZE=ON` (GCC or Clang) builds every target with AddressSanitizer and
  UndefinedBehaviorSanitizer, UB aborts (`-fno-sanitize-recover=undefined`). The vendored decoders are compiled with
  `-fno-sanitize=shift,float-cast-overflow` (their known, harmless noise: `CMakeLists.txt`). Run the pack and audio tests with
  `ctest -R '^(pack_loader|pack_integration|pack|pack_audio|audio_decode|music_slots|modern|screens|paula|fuzz_.*)$'`; set
  `ASAN_OPTIONS=detect_leaks=0` where LeakSanitizer does not exist (macOS) or reports the vendored code.
- **libFuzzer**: `CC=clang CXX=clang++ cmake -S . -B build-fuzz -DX2_FUZZ=ON` (Apple's Clang has no libFuzzer; use Homebrew LLVM or a Linux Clang with
  `libclang-rt-dev`), build `x2_fuzz_<name>`, then
  `build-fuzz/x2_fuzz_seeds . build-fuzz/fuzz_corpus; mkdir work; build-fuzz/x2_fuzz_manifest -max_total_time=600 -dict=tests/fuzz/manifest.dict work build-fuzz/fuzz_corpus/manifest`.
  A crash is written as `crash-<hash>`; replaying it is `x2_fuzz_manifest crash-<hash>`.
- **CI** (`.github/workflows/fuzz.yml`, not a required check of `main`): on a pull request touching the pack or audio code, the sanitizer job runs the tests above and each
  target fuzzes for 60 seconds; nightly (and by hand, with a `seconds` input) each target fuzzes for 30 minutes, with its working corpus kept in the Actions cache; a crashing input is uploaded
  as an artifact.

## Integration tests (`x2_pack_integration_tests`, ctest `pack_integration`)

Synthetic packs, no game data:

- The version 1 documentation example (`docs/schema/examples/v1-graphics.example.json`) loads as a directory, as a stored ZIP and as a deflated ZIP to an identical library (a digest of
  every picture, anchor, scale and far line), and the version 1 `AssetLibrary` view reads it.
- Allocation bombs: 4,000 atlas frames that all overlap one rectangle stay inside a 1 MB memory cap and a 100-entry cap (and load completely under the defaults); a PNG header that declares 4096 x 4096
  over a small pixel limit is refused before anything is allocated; absurd `scale` and `ring_lines` values allocate nothing.
- UTF-8 names for the pack directory, the files, the ZIP and the ZIP's members, and the same names written as JSON escapes.
- Determinism: a pack of 200 effects driven through a real `Paula` and the real `PackAudio` (200 starts on four channels with period changes) gives the same audio hash on every run, from the
  directory and from the stored and deflated ZIPs; the pack-off run differs; the hash is recorded in the test (`kGoldenAudio`), so a change of the stage's arithmetic must be recorded on purpose.

## Cost gates

The design budgets are 5 % for graphics and 3 % for audio on an idle machine. Both gates are **set wider on purpose** (CI machines are shared and noisy), and the measured figures are printed so that the
real margin stays visible; sanitizer builds print the figures and do not enforce them.

| Gate | Where | Measured on the development machine (arm64, RelWithDebInfo) | Limit |
|---|---|---|---|
| Pack audio stage with 200 effects: extra time per emulated frame | `pack_integration` | 0.13 ms | 3 % of the 20 ms frame, times 4 = 2.4 ms |
| Two streaming 44.1 kHz stereo tracks (decode and resample to 48 kHz), per frame | `pack_integration` | 0.14 ms | 2.4 ms |
| A library of 10,000 graphics and 100 screens: load; 3,000 sprite lookups and 40 block lookups per frame | `pack_integration` | 11 ms; 0.012 ms | 20 s; 4 ms |
| The real game with a pack carrying every kind of content, headless at canvas scale 3 on the flood replays: extra microseconds per presented picture in the presenter (`Presenter::at`, the "present time" line of the run), best of 2 against the run without packs. Two packs: the replay's dumps loaded as packs (`--dump-assets`, `--dump-audio`), and the demo pack `make_demo_pack.py` builds from them (every picture, screen, panel cell, effect and music slot replaced, at 2x) | data tests `pack_full_disk`, `pack_full_cdtv` | dumps: 216 us (disk), 209 us (CDTV); demo pack: 263 us (disk), 238 us (CDTV); audio adds nothing measurable | 833 us (5 % of a 60 Hz frame, the design's budget) |

**The graphics cost is inside the design's budget** (1.3 to 1.6 % of a 60 Hz frame on the flood replays, the richest scenes of the corpus; the limit is the 5 % the design asked for, with about three
times the measured figure as margin). Audio meets its 3 % with a wide margin (the real game with only the audio dump runs as fast as without; the synthetic stage costs 0.13 ms per emulated frame).

How it got there (a performance pass after WS7, which had measured 9 % / 14 % with the dumps and 25 % / 22 % with the demo pack and had not optimised anything):

- **The first figure was partly the test's own hashing.** WS7 timed whole runs, which include hashing every presented picture (`--modern-out -`). With a pack the screens and the panel are presented at
  the canvas scale instead of as the 1x faithful frame, so the run hashed 21.2 GB instead of 14.3 GB (disk) and 21.2 GB instead of 8.3 GB (CDTV) at about 1.5 GB/s: several seconds that no player pays. The run now
  reports `present time: N pictures, X us per picture`, the presenter alone, and the gate compares that.
  On that measure the starting point was +590 us (disk) and +680 us (CDTV) with the dumps, and +3,250 us and +2,000 us with the demo pack.
- **Round stars forced a single render band** (a disc spans band edges), so the demo pack lost the 8-thread rendering of the replay: 5.5 ms per interpolated frame against 0.65 ms. Each band now records the star
  blocks that touch its rows; they are merged in the order one band would have met them, the stars still visible are chosen once the whole canvas is replayed, and each band then draws the discs on its own rows.
  The pictures are bit-identical (13 configurations of 7,201 pictures each were hashed before and after, at scales 2, 3 and 4, with and without the surround), and `modern_backgrounds` now also checks that `--modern threads=1`,
  `threads=3` and the default give the same round-star pictures.
- **The replay cleared the replacement overlay under every pixel it drew** (one `memset` per pixel row of every blit; about 40 % of the replay time with a pack). A per-row map of the 16-pixel blocks a picture or a disc
  was written to lets the replay skip the clearing where nothing can be.
- **Frames outside the game** (title, menus, shop, loading: a third of the disk flood replay, two thirds of the CDTV one) were built at the canvas scale pixel by pixel with a division per pixel, and a pack's screen
  was composited pixel by pixel. Each frame line is now expanded once and copied for the other canvas rows, and a screen row without transparency is copied.

Other canvas scales, same replay, demo pack, extra us per picture: disk 160 (x2), 320 (x4), 790 (x6, 4.7 % of a frame, where the run without packs already takes 7 %), 290 (x3 with a 64-pixel surround on each side);
CDTV 200 (x2), 380 (x4). Ordinary levels draw far fewer objects than the flood replays. The synthetic gates above are unchanged.

`pack_full_*` also check that the two dumps of a stretch of the level-1 playthrough loaded together leave the presented pictures of that stretch unchanged and repeatable (frames 1150-1450 on the disk
build and 2700-3000 on the CDTV release, where nothing fades: a screen fades to within one colour level, which `screens_*` checks); on the Atari ST the screens check reports no differing frame. The ST has no headless presenter loop to time. They run serially and are Skipped
without the user's assets. Pass `-DMAX_US_PER_PICTURE=N` to `tests/pack_full.cmake` to hold a run to another limit.

## What the fuzzers found

| Input | Where | Finding | Fix |
|---|---|---|---|
| `manifest.json` with `"version": 4444...` (more than 19 digits) | `runtime/pack/pack_manifest.cpp`, the "needs a newer x2recomp" message | the version number was cast to `long` for printing: a float-to-integer overflow (undefined behaviour; a crash under UBSan) | the number is printed with `%.0f`; regression: `fuzz_seeds` writes `regress_version_overflow.bin`, and `pack_loader` checks `"version": 1e300` |

Fifteen minutes on each target, six running at once on a ten-core machine: the manifest target ran 1.1 million executions before it found the version overflow and 38 million after the fix; the ZIP reader 10.5
million, the PNG decoder 70 million, WAV 1.7 million, FLAC 1.1 million, Vorbis 0.3 million (its setup path is the slowest, about 260 executions a second). Nothing else was found. One run aborted in
the Vorbis target: it was the target's own check, which wrongly limited samples to +-1e6 (`decode.cpp` keeps a loud stream finite and clamps it later in `float_to_s16`, as documented); the check is now
"finite and at most 1e30". The pack and audio tests, the fuzz replays and the integration tests also pass under ASan and UBSan (macOS arm64, Apple Clang 21 and Homebrew LLVM).
