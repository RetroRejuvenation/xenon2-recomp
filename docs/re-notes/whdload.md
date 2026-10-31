# WHDLoad installs and LHA archives (importer, phase P5)

How the importer reads a WHDLoad install of Xenon 2, what the installs hold, how the retail `Disk.1` relates to
the preservation IPFs, and how the LHA decoder was validated. Images are named by public identity (WHDLoad
archive name and version, SPS number, sha1), never by where they are kept.

## What an install holds

A WHDLoad game directory (a folder, a zip, or an `.lha` archive) for Xenon 2 holds the slave
(`<name>.Slave`, an AmigaOS executable), icons, `ReadMe`, `Manual`, `Hints`, and for the floppy releases the disk
images `Disk.1` (and `Disk.2`). The slave is the game-specific patch code; it holds no game data. The disk images
are made by the install's imager (RawDIC, source `xenon2.islave.s` by Wepl, public domain), which writes each
disk's tracks as they are: the files are 901,120-byte (880 KB) images, except the Power Pack's `Disk.1`.

| Install (WHDLoad archive) | Slave | `Disk.1` sha1 | Floppy version (boot-block CRC-16/ARC) | `Disk.2` sha1 |
|---|---|---|---|---|
| `Xenon2_v1.9_2Disk_0297` | 1.9 Floppy (11.05.2005) | `22a41983…` | v3 (`$7ea7`), retail first pressing (SPS 0297) | `a748afc7…` (canonical) |
| Wepl 1.7 floppy (2003) | 1.7 Floppy (11.05.2003) | `d7ca6fa6…` | v3 | `a748afc7…` |
| Wepl 1.8 floppy (2004), and `[IPF1762]` 1.9 | 1.8 / 1.9 Floppy | `808f7ffb…` | v2 (`$c0fb`), as on the Bombuzal + Xenon 2 disk 1 (SPS 1762) | `c294d664…` (not the canonical disc 2: 203 sectors differ) |
| `Xenon2_v1.9_1Disk_2234` | 1.9 Floppy | `6461b317…`, 1,085,184 bytes | v6 (`$6827`), The Power Pack (SPS 2234, Speedlock) | none |
| `Xenon2CDTV` v1.6 | 1.6 CDTV (11.05.2005) | none | the CDTV release | none (game files and speech samples instead) |
| `Xenon2Demo` v1.0 | Xenon II Demo (JOTD) | none | the playable demo | none |

The imager selects the floppy version from the CRC-16/ARC of the 1 KB boot block: `$7f8f` v1 (SPS 2874, second
pressing; no install in the set holds it), `$c0fb` v2, `$7ea7` v3, `$7e53` v4 (one-disk budget, standard tracks),
`$bdce` v5 (one-disk budget, Rob Northen PDos), `$6827` v6. The importer uses the same table
(`runtime/import/whdload.cpp`) to name a retail disk 1 whatever container it comes in.

## What the importer does

- **LHA** (`.lha`, `.lzh`, found by content) is unpacked like a zip, one level of nesting each (3 in all).
- A **slave** is recognised by structure (hunk header, `moveq #-1,d0 ; rts`, `WHDLOADS`) and reported by name and
  version. A CDTV or demo slave is reported as a different program and not imported.
- A **`Disk.2`** of the retail installs is the canonical disc 2 (sha1 `a748afc7…`) and imports as such, next to
  any supported disc 1.
- A **retail disk 1** (any 880 KB image whose boot block matches v1-v3 and whose decoded game image is the two-disc retail
  build) is a retail disc 1 **without a key track** (the key track of a WHDLoad `Disk.1` is blank): the importer identifies
  the pressing from the decoded game image, refuses the import with the options (an IPF or flux dump that keeps the key
  track, or the explicit opt-in `--apply-protection-patch --i-confirm-i-own-the-original`), and with the opt-in produces
  exactly the image the IPF route produces with the patch (sha1 `1ca85d73...` for SPS 0297, checked by the
  `variants_import` test). v2 (the Bombuzal + Xenon 2 compilation) and the Power Pack's `Disk.1` (v6) are different builds
  and are imported as such since P3d (`single-disk-variants.md`); v4 and v5 are named as one-disk releases and refused. If a supported disc 1 is given too, the retail one is noted as not used.

## A retail `Disk.1` is a plain 880 KB image with a blank key track

The retail disk 1 has custom loader code and a Rob Northen Copylock key track, but its other tracks are standard
Amiga MFM. RawDIC stores those as they are and leaves cylinder 0 head 1 (the key track, sectors 11-21) blank. So a
retail `Disk.1` file is directly an ADF-shaped image, and `tools/retail_game_image.py`'s arithmetic (the game image
is a 16-bit-word XOR chain from disk offset `$70C00`) applies to it unchanged. `tools/variants/whd_disk1.py` does
exactly that and compares with the image decoded from the preservation IPF by `tools/ipf.py` and `tools/mfm.py`:

| WHDLoad `Disk.1` | IPF of the same pressing | Sectors that differ | Decoded game image (sha1) |
|---|---|---|---|
| 1.9 `2Disk_0297` (v3) | SPS 0297 disk 1 | **0** (the whole disk is byte-identical) | `8d02d25b…`, identical |
| 1.7 floppy (v3) | SPS 0297 disk 1 | 11 (cylinder 0 head 1 only) | `8d02d25b…`, identical |
| 1.8 floppy and `[IPF1762]` 1.9 (v2) | SPS 1762 disk 1 | 11 (cylinder 0 head 1 only) | `0419668…`, identical |

The v1 image (SPS 2874, second pressing) decodes to `fc84cc6f…` from the IPF; no WHDLoad install in the set has
that pressing. The three images differ from the canonical BS1 image as described in `docs/re-notes/retail-disk1.md`.

## LHA decoder validation

The decoder (`runtime/import/lha.cpp`) is our own; see `docs/PROVENANCE.md` for what it follows. Every member of an
LHA archive carries a CRC-16 of its data, so a member that decodes with a matching CRC is correct.

- **lhasa's regression archives** (209 files from about twenty archiver releases: LHA for Amiga 1.22 and 2.12,
  LHarc 1.13 and 3.13, LHA for Unix, DOS, OS/2, OS-9, OSK, Atari, X68000, MorphOS, ExpLZH, LHMelt, MacLHA, header
  levels 0, 1, 2 and 3): 257 members, **238 decode with a matching CRC-16**. By method the archives hold -lh0- 102,
  -lh1- 25, -lh4- 2, -lh5- 47, -lh6- 12, -lh7- 8, -lz4- 4 and 40 directory entries; 17 members are of methods we
  refuse (-lh2-, -lh3-, -lhx- 2, -lz5- 8, -lzs- 2, PMArc, and LHARK's own -lh7-, 2, which is recognised as `-lk7-`),
  and 2 -lh5- members are not extracted on purpose (a deliberately truncated archive, and a 400 MB member over the
  size limit). Two format quirks were found this way: the OS-9/68k level-2 header length is two bytes short, and
  LHARK writes an incompatible -lh7- (level 1, OS type `' '`).
- **The WHDLoad install archives** (13 `.lha` files: `whdload.de` installs 2003-2018 with imager and slave
  source, the JST install, the Red Sector music rip, the `wcx` WHDLoad-collection archives): **252 members, all
  -lh0-/-lh5-, all decode with a matching CRC-16.**
- **Synthetic tests** (`x2_lha_tests`, no game data): stored members of every header level with directories
  (extended-header name and directory), hand-built -lh4- to -lh7- streams with a single-symbol offset table,
  an -lh1- round trip through an encoder driven by the decoder's own model (a 42 KB input crosses the 32 KB tree
  rebuild), CRC and header-checksum damage, truncation, oversized members, and a mutation fuzz (20,000 iterations
  run under ASan and UBSan).

Limits: members over 96 MB or archives that unpack to over 512 MB are refused; self-extracting archives and the
rare methods above are not read.
