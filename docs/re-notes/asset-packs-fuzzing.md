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
| The real game with both dumps loaded (graphics, screens, panel, backgrounds; effects, speech): whole run at canvas scale 3, best of 3 against the run without packs | data tests `pack_full_disk`, `pack_full_cdtv`, `pack_full_st` | see the test output | 25 % |

`pack_full_*` also check that the two dumps loaded together leave the pictures of the run unchanged and repeatable (Atari ST: the screens check reports no differing frame). They run serially and are
Skipped without the user's assets. `X2_PACK_FULL_MAX_OVERHEAD` is not an environment variable: pass `-DMAX_OVERHEAD=0.05` to `tests/pack_full.cmake` to hold a run to the design's number.

## What the fuzzers found

| Input | Where | Finding | Fix |
|---|---|---|---|
| `manifest.json` with `"version": 4444...` (more than 19 digits) | `runtime/pack/pack_manifest.cpp`, the "needs a newer x2recomp" message | the version number was cast to `long` for printing: a float-to-integer overflow (undefined behaviour; a crash under UBSan) | the number is printed with `%.0f`; regression: `fuzz_seeds` writes `regress_version_overflow.bin`, and `pack_loader` checks `"version": 1e300` |

Nothing else was found in 15 minutes per target on each of six targets (about 8 million executions of the manifest target, 6 million of the ZIP reader, 40 million of the PNG decoder, and 0.7 to 1 million of
each audio decoder; the Vorbis setup path is the slowest at about 230 executions a second). One run reported an abort in the Vorbis target: it was the target's own check, which wrongly limited samples to +-1e6
(`decode.cpp` keeps a loud stream finite and clamps it later in `float_to_s16`, as documented); the check is now "finite and at most 1e30".
