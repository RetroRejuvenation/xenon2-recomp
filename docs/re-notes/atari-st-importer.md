# Atari ST importer (phase S1)

**Question:** what does it take to accept an Atari ST release of Xenon 2 Megablast into an assets folder that a later ST machine can boot,
and how are the other ST dumps recognised and refused? Plan: `.omc/plans/xenon2-atari-st-target-plan.md`. Analysis behind it:
`atari-st-analysis.md` (loader, packer, engine), `atari-st-image-works.md`, `atari-st-mirror-image.md`. Confidence key as in the index.

## What is imported

Only the **Kixx budget re-release** (1993): one double-sided disk, 80 cylinders x 2 sides x 10 sectors of 512 bytes (819,200 bytes), no copy
protection, an executable boot sector (word sum $1234, OEM "X2"). Its boot sector carries a stale BPB (10 sectors, 1 side) and there is no
file system: the loader reads raw tracks through the WD1772. So the importer **pre-links nothing**. It validates the image, normalises it to
one logical image and records what it found; the ST machine (a later phase) boots the real loader from `disk.bin` on an emulated controller. **H**

Output, in its own folder (`<data dir>/assets-st`, or `--assets DIR`):

| File | Contents |
|---|---|
| `disk.bin` | the logical image: cylinder-major, sides interleaved, `sectors_per_track` sectors of 512 bytes per track (a plain `.st`; FAT's block order) |
| `manifest.txt` | written last; see below |

No `game_0400.bin`, no `disc2.bin`; a stale one from an Amiga import in the same folder is removed. The Amiga assets stay in `assets`.

Manifest keys (all `key=value`): `importer_version`, `source=` (one per input, for the automatic re-import), `build=atari-st-kixx`, `game=st`,
`variant=st-kixx`, `variant_name`, `container` (ST / MSA / STX (Pasti)), `canonical=false`, `patched=false`, `copylock=absent`, `cylinders`,
`sides`, `sectors_per_track`, `sector_bytes=512`, `layout=cylinder-major`, `boot_checksum=0x1234`, `bootable=true`, `source_sha1`,
`disk.bin_size`, `disk.bin_sha1`, `rnc_streams=11`, and `rnc.N=side=..,cylinder=..,offset=..,side_offset=..,packed=..,unpacked=..,crc_packed=..,crc_unpacked=..`.
`verify_assets` checks `disk.bin` against its recorded size and SHA-1; `ensure_imported` rebuilds a missing or damaged folder from the recorded
`source=` lines.

## Containers (`runtime/import/st_disk.*`)

All three decode to one in-memory `StDisk` (tracks of sectors with the id field, WD1772 status, bit position and read time), so the Amiga
`DiskImage` (a fixed 11 sectors per track) is untouched.

- **`.st`** raw, track-interleaved. Geometry from the BPB when it is consistent (512 bytes, 9-11 sectors, 1-2 sides, exact division, 70-86
  cylinders), else from the size table (368,640 / 409,600 / 450,560 single sided 80 tracks of 9/10/11; 737,280 / 819,200 / 901,120 double
  sided; 829,440 = 81 x 2 x 10). Kixx needs the table: its BPB says one side. Detection is by content: a plain image needs a standard size and
  a 512-byte BPB or a $1234 boot sector.
- **`.msa`** header $0E0F, sectors per track, sides - 1, first and last track; per track a big-endian length, raw when it equals the track size,
  else run-length coded (marker $E5, value, big-endian count). Every length and run is bounds-checked.
- **`.stx`** Pasti ("RSY\0"). Track records of 16 header bytes (size, fuzzy-mask size, sector count, flags, track length, track number: low 7
  bits cylinder, bit 7 side, type) followed by 16-byte sector descriptors (data offset, bit position, read time, id field cylinder/head/sector/size code,
  CRC, FDC status: $08 CRC error, $10 record not found). **A sector's data is at descriptor-end + fuzzy-mask length + data offset**: the offset
  is relative to the end of the mask, not to the record start (found on Mirror Image, whose track 0 has a 512-byte mask). Sectors with a CRC error keep
  their data; "record not found" has none.

Caps: 86 cylinders, 64 sectors per track, 8 MB of sector data per disk (charged for every descriptor, so descriptors aliasing one blob cannot
inflate), 256 MB read in total, ZIP nesting 3, 1024 folder files. gzip (`.st.gz`) is not read on this path.

## Identification

A disk is recognised by the SHA-1 of its **logical image** (missing sectors zero, CRC-error sectors keep their data), so the three containers
of one disk agree. Reference values (`variants.cpp: st_releases()`):

| Release | Logical-image SHA-1 | Status |
|---|---|---|
| Kixx 1993, 2 sides | `323d6859635b6b82c86363db048b26e0f3df994c` | importable (`.st` and `.stx` agree) |
| Image Works 1989, disk 1 (1 side) | `c26449f0718e0ec061106f308a31babccd169f94` | refused: Copylock ST |
| Image Works 1989, disk 2 | `810dc02c9714ee133bfb882efc6b1fad30002671` | refused with its disk 1 |
| Mirror Image 1991 (both disks as two sides) | `a07f2d8f5ddfc724c1f043e4949fac2440b8bfd4` | refused: re-keyed Copylock ST, weak-bit mask |
| ST Format 3 cover disk (81 x 2 x 10) | `bb759b04f5a292fd9f4a51631cf9cbcd72a2bb33` | refused: a TOS preview build, not the game |

An unknown ST disk is reported with its geometry, hashes and findings and refused (exit 3). Several disks given together: the importable
one wins, the rest are listed as "not used"; an identical copy in another container is noted as the same disk.

## Diagnostics

`x2recomp --diagnose-image FILE` (also `--list-variants`, whose ST rows show the expected `disk.bin` SHA-1) prints: container, geometry,
the boot sector checksum and OEM, non-standard tracks, **copy-protection findings** (a "Rob Northen Comp" key sector, an STX weak-bit mask, a
sector that reads more than 3% longer than the median of its track), sectors the logical image cannot hold intact, a FAT12 listing when the
disk has a real FAT (first FAT sector starts with the media descriptor and two $FF; Kixx does not, which a stale-BPB-only check would get
wrong), the RNC ProPack blocks, the logical SHA-1 and the recognised release.

**Kixx, the RNC blocks** (the packed stream is found by header, both CRC-16s must verify; the offset is into the side's cylinder-major stream): 11,
verified. The front end on side 0 (cylinder 1 + 12: 126,048 -> 159,108), five level modules on side 1 (cylinder 0 + 0: 67,162 -> 111,900;
13 + 1024: 75,687 -> 125,712; 28 + 0: 79,891 -> 133,284; 43 + 3584: 103,003 -> 171,638; 63 + 4608: 69,479 -> 153,738) and five 12,000-byte
blocks at the end of side 1 (77 + 2560, 77 + 4608, 78 + 2048, 79 + 0, 79 + 2560). **H** (the unpacked CRC-16 matches). The importer records
them as an index; the machine does not need them, the real loader unpacks them.

## Not done here

- No ST machine: the import tells the user so (CLI message, import window). Phase S2 onward.
- Image Works (Copylock ST layer 3) and Mirror Image import later; a Pasti `.stx` is needed for them, a `.st`/`.msa` cannot hold the key sector.
- `kImporterVersion` is unchanged (3): the Amiga assets are not affected.

## Tests

`x2_st_tests` (`st`: synthetic disks in all three containers, FAT12 volumes, RNC blocks from a test encoder, malformed and mutated inputs, an
aliasing bomb, tamper detection; clean under ASan/UBSan) and `st_files` (`X2_ST_TEST_DIR`: real dumps, exit 77 without; the Kixx `.st` and `.stx`
must give the SHA-1 above and import).
