# vendor/audio: the FLAC and Ogg Vorbis decoders

Two single-file public-domain decoders for the asset-pack audio (`docs/re-notes/asset-packs-v2-design.md` section 5.4,
`docs/re-notes/audio-decoders.md`). WAV is parsed by our own code (`runtime/audio/decode.cpp`). `dr_flac.h` is an **unmodified
copy** of the upstream file; `stb_vorbis.c` carries ten small local patches (see "Local patches"). The download URLs and hashes of
the files as fetched are below.

| File | Library | Version | Upstream commit pinned | Licence |
|---|---|---|---|---|
| `dr_flac.h` | dr_flac, David Reid (mackron/dr_libs) | v0.13.4 (header: `dr_flac - v0.13.4 - 2026-10-31`) | `35b5c392cc15456736c440a68dbe073178bbffe0` (master head when fetched; the file last changed in `806ee9ada8ca9827b22f76827f739c290f37ba48`, "dr_flac: Version 0.13.4") | choice of **public domain (Unlicense)** or **MIT No Attribution** (the header says: "Choice of public domain or MIT-0") |
| `stb_vorbis.c` | stb_vorbis, Sean Barrett (nothings/stb) | v1.22 (header: `Ogg Vorbis audio decoder - v1.22 - public domain`) | `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5` (master head when fetched; the file last changed in `1ee679ca2ef753a528db5ba6801e1067b40481b8`, 2021-07-12, "update version numbers") | choice of **MIT** or **public domain (Unlicense)** ("available under 2 licenses -- choose whichever you prefer") |

Downloaded 2026-11-01 from:

- https://raw.githubusercontent.com/mackron/dr_libs/35b5c392cc15456736c440a68dbe073178bbffe0/dr_flac.h
  (549,189 bytes, sha256 `b8078eeb5d523502eb7964508cd7ec8450bc79d383bec90aa17fd9e2fefd9c24`)
- https://raw.githubusercontent.com/nothings/stb/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/stb_vorbis.c
  (192,790 bytes, sha256 `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`)

The licence texts are in `LICENSE-dr_flac.txt` and `LICENSE-stb_vorbis.txt` (copied verbatim from the end of each file) and
in `packaging/licenses/` for the packages. This project uses each under the terms it prefers; both are GPL-compatible.

## How they are built

- `dr_flac_impl.c` defines `DR_FLAC_IMPLEMENTATION` with `DR_FLAC_NO_STDIO`, `DR_FLAC_NO_OGG` and `DR_FLAC_NO_SIMD` (the decoder
  is given a memory block or our callbacks, never a file; no Ogg-FLAC; the scalar path only, so the result does not depend on
  the CPU) and includes `dr_flac.h`.
- `stb_vorbis_impl.c` includes `stb_vorbis.c` with `STB_VORBIS_NO_STDIO`, `STB_VORBIS_NO_PUSHDATA_API` and
  `STB_VORBIS_NO_INTEGER_CONVERSION` (memory pull API, float output only).
- Both are compiled as C, with relaxed warnings (like `vendor/puff` and Musashi). `decode.cpp` includes their headers only.
- stb_vorbis is always opened with an `stb_vorbis_alloc` buffer, so all its setup allocations come from a caller-sized block and
  none from the file's declared sizes (and none from `alloca`).

## Local patches

`dr_flac.h`: none.

`stb_vorbis.c` v1.22 has ten changes, every one marked `x2 patch` in the file. They came out of the mutation fuzz
(`tests/audio/decode_tests.cpp`, `docs/re-notes/audio-decoders.md`): a damaged comment header made `start_decoder` write outside the
`stb_vorbis_alloc` buffer (a heap overflow into whatever followed) and `vorbis_deinit` read through a null list. The sha256 above is
of the unpatched upstream file; `git diff` against the first commit of the file shows exactly these lines.

1. In `start_decoder`, after reading the vendor-string length (`len`, an `int`): `if (len < 0 || len >= 0x40000000) return error(f, VORBIS_invalid_setup);`
   (upstream goes straight to `setup_malloc(f, len+1)`, which wraps for `len = 0x7FFFFFFF` or `-1` and then writes `len` bytes).
2. The same check after reading each user-comment length.
3. After reading `comment_list_length`: refuse a value below 0 or above 0x10000 (upstream passes `sizeof(char*) * length` to
   `setup_malloc` as an `int`, which truncates, then fills `length` entries).
4. In `vorbis_deinit`: only walk `comment_list` when it is not null (upstream reads `comment_list[i]` after a failed allocation).

5. `struct Mapping`: `submap_floor` and `submap_residue` are `[16]`, not `[15]`. A mapping may declare 1 to 16 submaps (a 4-bit count plus one), and with
   15 entries the 16th floor index overwrote `submap_residue[0]`, which was then never checked against `residue_count`; `decode_residue` indexed
   `residue_config` with it. Streams with at most 15 submaps are unaffected. (`tests/audio/vorbis_setup_tests.c`.)
6. In `start_decoder`, a sparse codebook with no codeword (`sorted_entries == 0`) still gets the one-element `sorted_values` table with its `-1` sentinel; the
   `DECODE` macro reads `sorted_values[-1]` for the failure result, which dereferenced a null table when such a book was a floor or residue classbook.
   (`tests/audio/vorbis_setup_tests.c`.)
7. In `start_decoder`'s residue setup: a residue classbook or residue book with zero dimensions is refused (`VORBIS_invalid_setup`); `decode_residue` divides by the book's
   dimensions, and no real file uses such a book. (`tests/audio/vorbis_setup_tests.c`.)

8. In `start_decoder`'s codebook lookup setup: the table sizes are checked in 64 bits before anything is allocated. `entries * dimensions` (lookup type 2) is computed in
   `unsigned long long`, and every byte count that is passed to the `int` parameter of `setup_malloc`/`setup_temp_malloc` on the lookup paths is refused with
   `VORBIS_invalid_setup` when it does not fit; upstream multiplied two file-controlled `int`s and let the byte count wrap, after which the fill loop wrote
   through a pointer outside the caller's buffer. (`tests/audio/vorbis_setup_tests.c`.)

9. In `seek_to_sample_coarse`, the pending first-frame `discard_samples_deferred` is cleared before the seek's pump frame, and the two `assert`s in `stb_vorbis_seek` that
   bounded the sample offset inside the decoded frame became a real check that returns `VORBIS_seek_failed`. They vanish under `NDEBUG`; a seek before the first read applied the
   stream-start discard to a frame decoded after the jump, leaving `channel_buffer_start` past `channel_buffer_end` and the next `stb_vorbis_get_samples_*` write past the
   caller's buffer. (`tests/audio/vorbis_setup_tests.c`.)

10. In `start_decoder`'s temp-memory estimate: a type 2 residue is sized over the full long block (`blocksize_1`), as `decode_residue` uses `2*n`. `decode_residue` also
   checks its temp allocation for NULL, and `make_block_array` returns NULL instead of storing the block pointers through NULL, so a caller arena with a free gap below the
   real request fails with `VORBIS_outofmem` instead of writing to address 0. (`tests/audio/vorbis_setup_tests.c`.)

Patches 1 to 4, 7 and 8 are pure refusals: files that were valid before decode identically. Patches 5 and 6 change no value a valid file reads, and patches 9 and 10 correct
state a valid file never relied on: 9 keeps the stream-start discard at sample 0 (it is only cleared for a frame decoded after a mid-stream seek), and 10 reserves the temp
memory `decode_residue` already asked for. A stream that decoded without a fault before decodes the same. If upstream fixes any of these, drop the matching patch when the file
is next re-vendored.

## Sanitizer note

The vendored C files are built with `-fno-sanitize=shift,float-cast-overflow`. dr_flac and stb_vorbis shift negative or sign-bit values
left and dr_flac's seek search casts a NaN estimate to an integer that is then clamped; every supported compiler defines both as
intended. Without the opt-out they would be reported on every sanitizer run and hide real findings in the rest of the code.
