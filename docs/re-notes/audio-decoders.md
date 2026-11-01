# Audio decoders for asset packs (WS3a)

Status: implemented (`runtime/audio/decode.{h,cpp}`, CMake target `x2audiodec`, test `audio_decode`). Nothing in the runtime links
it yet; WS3b (sound-effect and music replacement) is its first user. Design context: `asset-packs-v2-design.md` section 5.4.

Asset packs are untrusted input, so the job is not only "decode WAV, FLAC and Ogg Vorbis" but "never crash, never allocate what a
header merely claims, never give two machines different samples".

## Formats

| Format | Decoder | Notes |
|---|---|---|
| WAV | ours (`WavDecoder`) | PCM 8, 16, 24 and 32 bit, IEEE float 32 and 64, WAVE_FORMAT_EXTENSIBLE (PCM and float sub-formats), mono and stereo. Chunks are scanned (odd sizes padded, unknown chunks skipped, up to 1,024); the data size may be 0 or `0xFFFFFFFF` (streamed file: runs to the end); a partial last frame is dropped; a data chunk that overruns the file is an error, damage after the audio is ignored. `smpl` loop. |
| FLAC | `vendor/audio/dr_flac.h` 0.13.4 | Through read/seek/tell callbacks onto our `ByteSource`, with dr_flac's allocator replaced by a counting one that enforces `DecodeLimits::budget`. No Ogg-FLAC, no SIMD (the scalar path, so the result does not depend on the CPU), no stdio. 4 to 32 bit. |
| Ogg Vorbis | `vendor/audio/stb_vorbis.c` 1.22 | The memory API only (the file is read into memory first, bounded by `max_file_bytes` and the budget). Opened with a caller-sized setup buffer: 2 MB, then 8 MB, then 32 MB if the first ones report out of memory, never beyond the budget. No push-data API, float output only. |
| MP3, Opus, AAC | not supported | Refused with "unknown format". Deliberately: MP3 and AAC have patent history, and Opus needs a second library; WAV, FLAC and Vorbis cover lossless and lossy. |

Versions, upstream commits, URLs and hashes: `vendor/audio/README.md`. Licences: public domain or MIT-0 (dr_flac), MIT or public
domain (stb_vorbis); `THIRD-PARTY-NOTICES.md` and `docs/PROVENANCE.md` list them.

## API (`x2::audio`, `runtime/audio/decode.h`)

- `ByteSource`: `read`, `seek`, `tell`, `size`, optional `data()`. `memory_source(ptr, n)` views memory the caller keeps alive;
  `memory_source(std::vector<uint8_t>&&)` owns it. (A ZIP member or a file can implement the interface; WS3b will.)
- `AudioDecoder::open(source, DecodeLimits, &err)` identifies the format by its first bytes and returns a streaming decoder or null
  with a message. `info()` gives format, channels, rate, bits, length in frames (-1 if unknown) and the loop. `read(float*, frames)`
  returns interleaved float frames, 0 at the end or after a failure (`failed()`, `error()`); `seek(frame)`.
- `decode_all` / `decode_all_f32(source, limits, OutputFormat{channels, rate}, clip, &err)` decode a whole clip, convert channels
  (mono is duplicated to stereo; stereo is averaged to mono) and resample to the host rate. The result has 16-bit PCM (or float) and
  `peak`, the largest absolute sample before clamping, so a pack checker can warn about clipping.
- `StreamResampler(channels, in_rate, out_rate)`: `push`, `finish`, `pull`. A 64-tap Kaiser-windowed sinc (beta 8, cut-off 0.46 times
  the lower rate, 256 phases, linear interpolation between phases). Output frame k sits at input position k times in_rate over
  out_rate computed in integers, so the result does not depend on how the input was chunked; equal rates copy exactly.

### Loop metadata

A loop is half-open `[start, end)` in frames of the file. Sources, in order of trust: a WAV `smpl` chunk (first forward loop; the
chunk's end is the last frame played, so we add one); for FLAC and Vorbis, comment tags `LOOPSTART`, and `LOOPLENGTH` or `LOOPEND`
(`LOOPLENGTH` wins when both are present). A loop that is empty, out of order or past the end of the file is ignored and
`info().loop_note` says why. Tags are the convention music tools such as Audacity and game engines use.

## Limits (`DecodeLimits`)

Every limit is checked before the memory it guards is allocated.

| Limit | Default | Guards |
|---|---|---|
| `budget` | 64 MB | all decoder allocations together (dr_flac through its allocator, stb_vorbis through the setup buffer), and for `decode_all` the decoded and converted output as well |
| `max_file_bytes` | 512 MB | the size of the file |
| `max_frames` | two hours at 192 kHz | the length a header declares |
| `min/max_sample_rate` | 8,000 to 192,000 Hz | |
| `max_channels` | 2 | more is refused, not mixed down |
| `max_calls` | 2^24 | read calls `decode_all` makes before giving up (a decoder that yields almost nothing per call) |

The caller (WS3b's loader) tightens these per kind: a sound effect or speech clip of up to 120 s and 32 MB, sound effects and speech
256 MB in total, music streamed, as in the design.

## Determinism

Same bytes, same limits, same host rate give the same samples on every platform:

- the resampler works in integer positions, builds its kernel with `double` maths from `std::sin` and a series Bessel function and
  rounds to float once, and `decode.cpp` is compiled with FMA contraction off (`-ffp-contract=off`, MSVC `/fp:strict`);
- dr_flac runs its scalar path; stb_vorbis's float output is deterministic for one build of the library;
- the test pins two FNV-1a golden hashes: the mono FLAC fixture decoded to 16-bit, and a synthetic WAV sine resampled from 22,050 Hz
  to 48,000 Hz stereo (the kernel is built with `double` maths, so only a platform's libm could move it). They are compared exactly in
  CI on macOS arm64, Linux GCC and Clang and Windows MSVC. If a platform differs, the right fix is in the decoder, not the golden.
  stb_vorbis uses float transforms, so Vorbis is only checked to repeat exactly within one build (the tolerance checks cover its values).

## Tests (`tests/audio/decode_tests.cpp`, ctest `audio_decode`)

Fixtures are short synthesised tones in `tests/audio/fixtures/` (recipe and sha256 in its README; `make_fixtures.sh` regenerates them
bit-exactly with ffmpeg 4.4). They are not recordings, are each under 64 KB, and the hygiene scan allows exactly this directory and
these extensions (`tools/check_no_game_data.py`).

Covered: the memory source; every WAV format with exact values; extensible, chunk scanning, padding, stray bytes, size fields of 0 and
`0xFFFFFFFF`; WAV errors (truncation, bad `fmt`, too many channels, limits); `smpl` loops and seeking; streaming in chunks of 1, 7,
1,000 and 4,096 frames matches `decode_all` bit for bit; FLAC and Vorbis fixtures (format, rate, length, frequency, peak, loop tags,
limits, truncated files); the resampler (frame counts, identity, frequency and level, DC, removal of content above the new Nyquist frequency, chunk independence, stereo independence, empty input);
channel and 16-bit conversion; the two goldens; and a mutation fuzz.

### Fuzz

Ten seeds (WAV 16-bit, stereo extensible, 24-bit and float; FLAC 16-bit, 24-bit and with a loop tag; Ogg mono, stereo and with a
loop tag) are mutated by random bytes, truncation, 0xFF or 0x00 over a header-sized region, insertion, deletion of a run, splicing a
copy of an earlier run, and a 32-bit field set to an extreme value. Each result is decoded whole (a third of the time with a channel
and rate conversion) under tightened limits, then streamed with random chunk sizes, seeks and reads past the end. The checks: no
crash, no sanitizer report, a non-empty message on every refusal, finite samples and a consistent size on every success. The default is
3,000 mutations per kind (twice that for the WAV kinds); `X2_AUDIO_FUZZ_ITERATIONS=N` changes it.

Run under AddressSanitizer and UndefinedBehaviorSanitizer at 20,000 iterations per kind. What it found, all fixed:

| Finding | Where | Fix |
|---|---|---|
| Heap overflow past the setup buffer: a comment or vendor-string length of 0x7FFFFFFF made `len + 1` wrap, `setup_malloc` returned a tiny block and `len` bytes were written | stb_vorbis `start_decoder` | local patch: refuse a length below 0 or at or above 2^30 |
| The comment count times 8 passed to `setup_malloc` as an `int` truncated; the loop then filled `count` entries | stb_vorbis `start_decoder` | local patch: refuse a count above 65,536 |
| Null read when the comment list allocation failed after the count was read | stb_vorbis `vorbis_deinit` | local patch: skip the walk when the list is null |
| Overflow to infinity: a WAV float sample of 3e38 survived the finite check and the resampler's sums overflowed | `WavDecoder::convert` | float samples are capped at plus or minus 1e30 (NaN and infinity were already silence) |
| Left shifts of negative or sign-bit values; a NaN cast to an integer in dr_flac's seek search | dr_flac, stb_vorbis | not patched (defined behaviour on every supported compiler, and the seek result is clamped); the vendored C files are built with `-fno-sanitize=shift,float-cast-overflow` so they do not bury real reports |

The WAV parser itself produced no sanitizer findings. Two test-harness bugs also surfaced (a source viewing a temporary buffer, and a
wrong WAVE_FORMAT_EXTENSIBLE sub-format GUID in both the test builder and the parser, which would have refused real extensible
files); both are fixed and the second has a regression test.

## What this is not

It does not decide which clip plays when, mix, or touch Paula; that is WS3b. It does not open ZIPs or discover packs (WS1a, WS1b).
It adds no runtime dependency until WS3b links it, so a normal build, its size and its replay hashes are unchanged.
