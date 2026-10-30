# DMS (Disk Masher System) archives

How `runtime/import/dms.cpp` reads them, and how it was checked. The decoder is our own code, written from the
format as implemented by xDMS (public domain; Andre Rodrigues de la Rocha, maintained by Heikki Orsila). Part of
the disk-variants work (importer: DMS, extended ADF, and later IPF and flux formats).

## Format

All multi-byte fields are big-endian.

- **Archive header, 56 bytes.** `DMS!` at 0; general-info flags at 10 (bit 0 no-zero, 1 encrypted, 2 appended,
  3 banner present, 4 high density, 5 MS-DOS, 8 FILEID.DIZ present); Unix time at 12; first and last track at 16 and
  18; packed and unpacked size (3 bytes) at 21 and 25; creator version at 46 (`major * 100 + minor`); disk type at
  50 (7 = an FMS file, not a disk); default compression mode at 52; CRC-16 of bytes 4..53 at 54.
- **Track header, 20 bytes.** `TR`; track number at 2; packed length at 6; length after the first stage at 8;
  unpacked length at 10; flags at 12; mode at 13; 16-bit byte sum of the unpacked data at 14; CRC-16 of the packed
  data at 16; CRC-16 of header bytes 0..17 at 18. Then the packed data.
- **A DMS "track" is a whole cylinder.** A double-density disk is 80 of them, 11264 bytes each (both sides), numbered
  0..79. Number 80 is the FILEID.DIZ text, `0xFFFF` the banner. A "track 0" of 2048 bytes or less is a fake boot
  block (an advertisement), not disk data.
- **CRC-16** is CRC-16/ARC (reflected polynomial `0xA001`, initial value 0).
- **Modes.** 0 none, 1 simple, 2 quick, 3 medium, 4 deep, 5 heavy1, 6 heavy2. Simple is RLE alone. Quick, medium,
  deep and heavy run a dictionary stage and then an RLE pass (for heavy, only when flag bit 2 is set):
  - RLE: `0x90 0x00` is a literal `0x90`; `0x90 n v` is `n` copies of `v`; `0x90 0xFF v hi lo` takes a 16-bit count.
  - Quick: flag bit 1 = literal byte, 0 = match of 2..5 bytes at a distance of up to 255 in a 256-byte window.
  - Medium: the same shape with a 16 KB window; matches use the static two-level table (`d_code`/`d_len`, the LZHUF
    tables) for the length and the distance.
  - Deep: LZHUF's adaptive Huffman tree for the character/length symbol, updated after every symbol and rebuilt when
    a frequency reaches `0x8000`; distances use the static tables.
  - Heavy: canonical Huffman codes (510 character/length symbols, 14 or 15 position symbols for the 4 or 8 KB
    window), the trees read from the stream when flag bit 1 is set and otherwise kept from the previous track.
- **State between tracks.** The window, positions and (for deep and heavy) trees carry over to the next track only
  when the track's flag bit 0 is set; otherwise the decoder resets after the track. The window positions start a
  match length short of the window end and advance by that length at the end of every track so that a state-keeping
  track stays in step with the encoder.

## What our reader does

- Places each cylinder by its **track number** (tracks `2n` and `2n+1`), so duplicates (some archives repeat track 0
  at the end; the later one wins) and gaps are handled. xDMS writes the tracks one after another instead, which
  gives an over-long image for such an archive.
- A cylinder that is absent is `Empty`. When the archive says **no-zero** (flag bit 0), absent cylinders are zero;
  `to_adf(..., zero_fill_missing)` fills them. Archives that cover only part of a disk without that flag stay
  partial, and the importer says so.
- Every track header CRC, data CRC and unpacked checksum is verified. Any mismatch, a track that does not
  decompress, an oversized length (above 32000 bytes) or truncated data is `Corrupt`/`Truncated`. Decoders read zero
  bits past the end of their input (the format relies on that) but every write is bounds-checked and every loop makes
  progress, so damaged input cannot hang or overrun.
- Refused with a message: encrypted archives, high-density disks, FMS files, tracks that do not unpack to 11264
  bytes, unknown modes.

## How it was checked

- **Synthetic tests** (`disk_image`, in CI): archives built by encoders in `tests/import/container_fixtures.h` for modes
  none, simple, quick and medium (including keep-state across tracks and the full distance table), and heavy with a
  one-symbol tree; fake boot blocks, banner, FILEID.DIZ, duplicate tracks, partial and no-zero archives, every
  header/CRC/checksum failure, truncation, an over-subscribed heavy tree, random data fed to every mode, and 300
  damaged archives. Deep and general heavy streams are not encoded by the tests; they are covered by real files.
- **Real files** (`dms_files`, local only, skipped when no folder is given): run on a set of 500 DMS files from public
  Amiga archives, 497 decoded with every CRC and checksum verified. Archives using each mode: none 30, simple 30,
  quick 1, medium 19, deep 5, heavy1 8, heavy2 456. The three that were rejected (two with a track that does not
  decompress, one with a checksum mismatch after unpacking) are rejected at the same tracks by an independent
  decoder.
- **Cross-check against an independent decoder.** On 237 files, the cylinders decoded by our reader were identical,
  byte for byte, to those produced by a Rust port of xDMS, for every mode. (Output files differ in length for
  archives with duplicate tracks and for no-zero archives, because that decoder appends tracks in file order;
  the comparison is per cylinder, by track number.)

## Not covered

- Encrypted archives (there is no password prompt), high-density disks (this project's discs are 880 KB), and DMS
  files that are not Amiga disks.
- Only whole 11264-byte cylinders are accepted.
