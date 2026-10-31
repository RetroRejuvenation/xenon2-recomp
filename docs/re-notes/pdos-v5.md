# The Rob Northen PDos release (WHDLoad imager version v5)

A one-disk release of Xenon 2 Megablast, known until now only from the WHDLoad imager's source (`xenon2.islave.s`, version v5)
and from one public flux dump (Internet Archive item `amiga-original-xenon-2-powerplay`, an SCP and a FluxEngine capture of the
same disk; `acquisitions-flux-acorn-st.md`). This note is the format, the importer's rules, the runtime build and what has and has
not been verified. Code: `runtime/import/{pdos,rnc,fluxengine}.*`, `runtime/boot.*` (`kBuildPdos`), `runtime/disk_shim.*`.
Tests: `tests/import/pdos_tests.cpp`, `tests/build_tests.cpp`, `tests/replays/pdos-v5/`.

**Status: the importer and the runtime build are done and tested on synthetic disks; the one real dump has seven unreadable
sectors, so no complete import of real data has been demonstrated, and a damaged dump is refused.**

## Identification

The disk's 1 KB boot block has CRC-16/ARC `0xBDCE` and starts with `DOS` (the imager's own identifier for v5; the other versions are
v1 `7f8f`, v2 `c0fb`, v3 `7ea7`, v4 `7e53`, v6 `6827`). Track 0 (cylinder 0, head 0) is a standard Amiga track of eleven sectors; tracks
1 to 159 hold twelve 512-byte sectors each in the PDos format. The assembled disk is `11 + 159 x 12` sectors = **982,528 bytes**, which
is what the WHDLoad install keeps as `Disk.1` (so that file is read directly).

## The PDos track

A raw MFM track of about 100,100 cells (one revolution at a 2 us cell), twelve sectors **8,288 cells** apart, each:

```
0x4489 (16 cells)  info (2 x 32 cells)  even half (128 x 32 cells)  odd half (128 x 32 cells)  gap
```

- **Info:** two raw longs; the data bits (the 0x55555555 positions) of the first, shifted left one and ORed with those of the second, give
  `[sector:8][track:8][checksum:16]` (the top bit is not used). A sector is accepted only when the sector and track numbers are the ones
  being looked for.
- **Data:** the even half holds the odd-numbered data bits and the odd half the even-numbered ones of each long, the halves 4,096 cells
  apart (`128 x 32`). Decoding a long: `dec = (even & 0x55555555) << 1 | (odd & 0x55555555)`, and the sector's longs are a running XOR:
  `out[k] = dec[k] ^ dec[k-1]` (with `dec[-1] = 0`).
- **Checksum:** the XOR of every raw even and odd long, masked with `0x55555555`, then folded: `(x & 0xFFFF) | ((x >> 16) << 1) & 0xFFFF`.
  A sector that fails it, or whose header names another sector or track, is not read; **nothing is ever repaired from a checksum** (a
  16-bit checksum makes a guessed repair look like a success: in the analysis of the real dump 10 of 12 such "repairs" disagreed with
  a direct read).

The decoder searches every `0x4489` marker with a small slack (-6..+6 cells) and treats the bit stream as a circle (a marker that
straddles the end of the stream is found by reading past it). It reads the track's own stream first, then the alternate streams the flux
layer kept (other revolutions and PLL settings, `flux.md`), and never replaces a sector it already holds.

## The files

- The **file table** is eleven 8-byte entries at disk offset `0x87C`: first sector, sector count, byte offset into the first sector, and a
  padding word, all big-endian. The game's sector numbers are **one above the image's** (the loader subtracts one, `_v5_load1/_v5_load2`
  in the imager source), so an entry's stream starts at `(first - 1) x 512 + offset`. The game reads the table at `$380`; the runtime copies it
  there from the disk before it starts.
- Each file is an **RNC method 1** (Rob Northen ProPack) stream: an 18-byte header (`RNC\1`, unpacked size, packed size, CRC-16 of the
  unpacked data, CRC-16 of the packed data, leeway, chunks) and a bit stream of 16-bit little-endian words. `runtime/import/rnc.cpp`
  checks the packed CRC first, decodes with every table, count, distance and length bounded, and checks the unpacked CRC; a
  stream "verifies" only when both match. The decoder was validated against the independent Python decoder (`tools/acq/rnc.py`), on the
  real streams (identical output) and against a test encoder in C++ that round-trips overlapping matches, several chunks and odd literal
  runs.
- The **game** is the stream at image offset `0x58806` (243,372 bytes unpacked), decrunched to `$400`; its first word is `bra.w $7382`.
  The stream at `0x1606` (409,146 bytes) is a bigger resident program with the loader and the Bitmap Brothers intro. The runtime does
  not run it: it starts the game directly, as it does for the other builds. (An earlier note in `acquisitions-flux-acorn-st.md` described
  the two the other way round.)
- The **eleven packed files**, by file-table entry (unpacked size, sectors the stream spans): entries 1 to 6 are the six level modules (104,860
  / 117,674 / 128,228 / 164,278 / 147,074 / 102,824 bytes), entries 7 to 11 five files of 12,000 bytes each. 94 to 100 % of their 48-byte
  windows are found in the canonical disc 2, so the release stores the canonical disc 2's data as RNC files. In the real dump entries 1, 2
  and 3 contain the unreadable sectors; entries 4 to 11 verify.

## What the importer does (`x2recomp --import`, `--diagnose-image`)

Inputs: flux (SCP, FluxEngine `.flux`, KryoFlux stream sets; **two captures of the same disk given together are merged**,
`flux.md`), an extended ADF (standard track 0, raw tracks 1-159) or the 982,528-byte `Disk.1`.

1. Assemble the disk: track 0's eleven sectors, then every PDos track's sectors. A sector that does not read stays zero and is listed.
2. Verify the game stream and all eleven packed files (both CRC-16s). The unpacked game is the game image (`game_0400.bin`, padded with
   zeros to `$3FC00`); `disc2.bin` is the disk itself, which the game's own loader reads.
3. **The import goes ahead only when the game and all eleven files verify.** Otherwise it is refused with a message that names the damaged
   files (in table order; "the first six are the levels") and the unreadable `track:sector` pairs, and suggests a second capture of the
   same disk. Sectors that are unreadable but lie outside the game and its eleven files are reported as a warning and stored as zeros
   (the runtime loads nothing else).

The manifest carries `build=pdos-v5`; the replay goldens in `tests/replays/` do not apply to it.

## The runtime build (`kBuildPdos` in `runtime/boot.cpp`)

The game is a separate assembly of the engine with long absolute addressing, so code addresses are different (the variables, `$CC6`
energy, `$CCA` lives, `$E04` level, `$A07` end-of-level and so on, are the same). Found by shape-normalised matching against the canonical
image and checked by what the code does: restart `$7382` (the vector-fill start-up, reached from `bra.w` at `$400`), disk loader `$94E2`,
vertical blank `$1EB4` (the address `move.l #$1eb4,$6c` installs), main loop `$7624` (the `addq.w #1,$cdc.l` frame step), death routine `$5DB4`,
high-score table `$87E6`, table saved at `$89F4`. Chip RAM 512 KB, no disc-id check.

The disk shim (`runtime/disk_shim.*`) replaces the loader like the others, with two build parameters: **the sector bias** (-1: game sector
`n` is image sector `n - 1`; a request below sector 1 is a bad request, answered as the original loader answers an error) and **twelve sectors
per track** for the staging buffer the original leaves at `$44000`.

## Evidence

- **Synthetic (every `ctest`):** the RNC decoder against a test encoder and under mutation with the packed CRC repaired (no wrong success,
  no crash; ASan/UBSan clean); the PDos track encoder and decoder (any rotation, wrong track, damaged sectors, alternate streams that fill
  gaps without overwriting); a synthetic 982,528-byte disk with 11 packed files and a game stream, imported as `Disk.1`, as an extended
  ADF, and as an SCP plus a FluxEngine capture each lacking a different sector (neither imports alone, together they do and the result is
  the original disk); a damaged file, a damaged game stream and an unused damaged sector (refused with the sector named / refused /
  imported with a warning); the build table, the boot copy of the file table to `$380`, and the loader with its bias.
- **Real dump, local only (`pdos_files`, `x2recomp --diagnose-image`):** both captures merged read 1,901 of 1,908 PDos sectors; the game
  verifies; eight of the eleven files verify; the seven unreadable sectors (`95:9 97:11 99:2 100:1 101:4 110:2 114:6`) lie inside the first
  three files, so **the importer refuses this dump**, naming those three files and sectors. SCP alone: 1,896 sectors and the game does not
  verify. The disk assembled with zeros in the seven sectors has SHA-1 `32cdbe74f8fd9e9b488e58087b8a40e6f0eb383a` and is not a valid release.
- **Boot, on development assets** (`tools/variants/write_assets.py pdos-v5 DISK.bin DIR --allow-damaged`, a flag only that development script has):
  the game starts, the title, credits and attract cycle run, the SELECT menu works, the game loads (`LOADING LEVEL 1`) and the shop screens
  draw, with no unmapped memory accesses in 19 replays. The game's own credit text is the original's ("Designed by The Bitmap Brothers"); the
  canonical image is the trainer crack ("Trained by Bamiga Sector One"), so the frame hashes of the credit pages differ from the canonical
  ones; the first 150 frames (the intro screens) are pixel-identical to the canonical image's.
- **High-score table:** `$87E6` holds the default empty table text (`" 1. 0000000 ::: "` repeated); the code at `$89F4` has the same instruction
  shape as the canonical `$8B1A`. Neither has been exercised by a name entry in the runtime.

## Limits and what is not verified

- **No complete dump exists** (the public item has seven damaged sectors, with two independent captures). Level data for levels 1-3 are
  therefore unavailable, and **no replay that starts a level can be a golden**: the game restarts a few frames into level 1 on zeros. Only
  `tests/replays/pdos-v5/attract.hashes` is recorded (the front end loads none of the damaged files); the other replays would encode
  the game crashing on zeros. A complete dump is needed for the rest (and would make the importer's `pdos_files` test
  report an import rather than a refusal).
- `main_loop_pc` `$7624` is reached when a game starts (3 logic frames before the restart on the damaged level 1); it has not been checked
  over a full level. `hiscore_saved_pc` `$89F4` and the table address are checked by code shape and by the default table, not by playing.
- A WHDLoad `Disk.1` of this release has not been seen; that route is covered only by the synthetic disk.
- To get a complete dump: dump your own copy of the disk (`docs/DUMPING.md`) with several revolutions per track, and give it to `--import`;
  a second capture of the same disk can be given together with the first.
