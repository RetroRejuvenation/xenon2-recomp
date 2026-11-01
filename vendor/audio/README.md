# vendor/audio: the FLAC and Ogg Vorbis decoders

Two single-file public-domain decoders for the asset-pack audio (`docs/re-notes/asset-packs-v2-design.md` section 5.4,
`docs/re-notes/audio-decoders.md`). WAV is parsed by our own code (`runtime/audio/decode.cpp`). `dr_flac.h` is an **unmodified
copy** of the upstream file; `stb_vorbis.c` carries four small local patches (see "Local patches"). The download URLs and hashes of
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

`stb_vorbis.c` v1.22 has four changes, every one marked `x2 patch` in the file. They came out of the mutation fuzz
(`tests/audio/decode_tests.cpp`, `docs/re-notes/audio-decoders.md`): a damaged comment header made `start_decoder` write outside the
`stb_vorbis_alloc` buffer (a heap overflow into whatever followed) and `vorbis_deinit` read through a null list. The sha256 above is
of the unpatched upstream file; `git diff` against the first commit of the file shows exactly these lines.

1. In `start_decoder`, after reading the vendor-string length (`len`, an `int`): `if (len < 0 || len >= 0x40000000) return error(f, VORBIS_invalid_setup);`
   (upstream goes straight to `setup_malloc(f, len+1)`, which wraps for `len = 0x7FFFFFFF` or `-1` and then writes `len` bytes).
2. The same check after reading each user-comment length.
3. After reading `comment_list_length`: refuse a value below 0 or above 0x10000 (upstream passes `sizeof(char*) * length` to
   `setup_malloc` as an `int`, which truncates, then fills `length` entries).
4. In `vorbis_deinit`: only walk `comment_list` when it is not null (upstream reads `comment_list[i]` after a failed allocation).

Each is a pure refusal: files that were valid before decode identically. If upstream fixes any of these, drop the matching patch
when the file is next re-vendored.

## Sanitizer note

The vendored C files are built with `-fno-sanitize=shift,float-cast-overflow`. dr_flac and stb_vorbis shift negative or sign-bit values
left and dr_flac's seek search casts a NaN estimate to an integer that is then clamped; every supported compiler defines both as
intended. Without the opt-out they would be reported on every sanitizer run and hide real findings in the rest of the code.
