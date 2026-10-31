# The CDTV game importer (phase C2)

What `x2recomp --import <CDTV disc image>` makes of the CDTV release, and how that was checked. Evidence for the plan in
`.omc/plans/xenon2-cdtv-target-plan.md` (architecture 3: pre-link at import, bare-metal start, traps); code in
`runtime/import/{hunk,iso9660,cdtv_game}.*`. No game data is quoted here: hashes, sizes and offsets only.

## 1. Pipeline

1. **Data track.** From a `.cue` (also inside a ZIP or a folder) the importer reads track 1 (MODE1/2352) whole, through the same
   container reader as the CD-audio import (`cdtv::read_data_track`; the audio tracks are not needed). A bare `.bin` (raw MODE1/2352) or
   `.iso` (cooked 2048) is read directly. At most 64 MB; the real data track is about 14 MB.
2. **ISO 9660.** `IsoVolume` opens the primary volume descriptor in either layout, checks the volume against the image, lists the
   directory tree (depth at most 8, 4,096 entries, directory loops and overlaps with file extents refused) and reads file extents only
   inside the volume. The volume id must be `XENONII`.
3. **`X2`.** Parsed as an AmigaDOS load file (`parse_hunk_file`: HUNK_HEADER, CODE, DATA, BSS, RELOC32, END; NAME/SYMBOL/DEBUG skipped;
   overlays, resident libraries, extended memory flags refused; every length checked against the file and hard limits) and linked
   (`link_hunk_file`) at `$400` with each next hunk directly behind: hunk 0 CODE 437,404 bytes at `$400`, hunk 1 BSS 372,160 bytes at
   `$6B09C`, end `$C5E5C`, the layout the WHDLoad CDTV slave uses (`cdtv-slave.md` section 2). Both hunks are chip.
4. **Game files.** The files the game opens by name (`INTRO`, `LEVEL1-5`, `PICCODE`, `PICDATA<digit/letter>`, `SOUNDS<nn>`, `SPEECH<nn>`; 95 on
   the disc; `cdtv-c0.md` lists the loader sites) are stored unmodified under `cd0/`. The CDTV's own software on the disc (`CDTV.TM`, `CDTVPREFS`,
   `RMTM`, the startup sequence and system configuration) is not part of the game and is not stored.
5. **Output** (`write_cdtv_assets`): `game_0400.bin`, `cd0/`, `manifest.txt` (last); stale `disc2.bin`, `copylock.txt` and `cd0` files of an
   earlier import are removed first.

## 2. Evidence on the real disc

| Check | Result |
|---|---|
| ISO volume | `XENONII`, 2,449 blocks, 101 files (99 in the root, two in subdirectories) |
| `X2` | 455,288 bytes, SHA-1 `f2d180ef…dffd`; hunk table `0x4001AB27`, `0x40016B70` (both chip) |
| Fix-ups | 4,452 in all: 4,367 pointing into hunk 0, 85 into hunk 1; all inside hunk 0, all at even offsets |
| Relocated values | every low 24 bits lie within `[$400, $C5E5C]` after relocation and within the target hunk before it |
| Upper bytes | 10 values carry a non-zero upper byte (a 68000 ignores it: 24-bit address bus); the importer keeps them as the OS loader would (the sum wraps at 32 bits); the WHDLoad slave's one `jsr (a1)` fix is the visible symptom |
| Image | 809,564 bytes, SHA-1 `fa8419ed…5b2d`; **byte-identical to an independent relocation** (a separate script and the `cdtv_import_disc` test both redo the arithmetic: unrelocated hunk bytes, each fix-up's long plus its target's base, BSS zero) |
| Differences from the unrelocated hunk | only at fix-up sites (4,687 bytes differ in all; no site is listed twice) |
| Languages | `ENGLISH`, `FRANCAIS`, `DEUTSCH`, `ITALIANO` found in the program: EN, FR, DE, IT |
| Game files | 95, their SHA-1s in the manifest; `cd0` files equal the ISO files byte for byte |

## 3. Manifest keys (`build=cdtv`)

`importer_version`, `source` (the disc image path, for the automatic re-import), `build=cdtv`, `chip_kb=1024`, `variant`, `variant_name`,
`recognised`, `container`, `canonical=false`, `patched=false`, `copylock=absent`, `languages`, `iso_volume`, `iso_files`, `x2_sha1`,
`game_0400.bin_sha1`, `image_base`, `image_bytes`, `entry` (the first instruction of hunk 0), `hunk_count`, `hunk.N`
(`kind,bytes,memory,base,fixups_in,fixups_to`), `reloc_total`, `file_count`, `file.N` (`name,size,sha1`). An uncatalogued `X2` still imports
(variant `cdtv-unrecognised`, a warning) so another CDTV pressing can be examined.

## 4. What the importer does not do

- It does not start the game: the runtime refuses `build=cdtv` ("CDTV build: runtime support not available yet") until the boot table entry, the
  file and CD-audio traps and the stand-in level-3 handler exist (phase C3).
- It does not touch the unreferenced 6.9 MB after the ISO volume (`cdtv-data-region.md`).
- The `CD0:` overlays are stored raw; their relocation is the game's own routine at hunk offset `$3CEEE` (`cdtv-c0.md`).
