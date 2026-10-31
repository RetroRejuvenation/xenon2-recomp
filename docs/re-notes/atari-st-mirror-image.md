# Atari ST: Mirror Image, the Kixx data blocks, and the preview build's source residue

Analysis only. No game bytes are committed: this note gives sizes, hashes and measurements, and every extracted file stays in
the git-ignored `generated/`. It follows `docs/re-notes/atari-st-analysis.md` (Image Works and Kixx, the YM music, the
engine comparison) and uses the same public Atarimania dumps (`docs/re-notes/acquisitions-flux-acorn-st.md` section 3).

Dumps used (sha1 of the file as downloaded): Mirror Image `.stx` `123d53d3…` (1,772,126 B), Kixx `.stx` `a7f128f8…`, Image
Works disk 1 `dd64375a…` and disk 2 `c827ba60…`, ST Format issue 3 `X2.PRG` `c1f4005e0e2b`.

New tools (`tools/acq/`, synthetic tests in `test_stmi.py`): `stx_fz.py` (sector access that honours Pasti fuzzy masks, per-side
images, boot checksums, protected-track and entropy summary), `stmap.py` (word-block value-mapping test, ascending
pointer-table finder, printable-text finder).

## 1. A bug in `stx.py` that hid the Mirror Image track 0

`stx.py` reads a sector at `base + data_offset`, with `base` the end of the sector descriptors. A track record that carries a
**fuzzy mask** (the weak-bit sector, here the Copylock key sector) stores the mask first, so the offsets are relative to the end
of the mask: `base + fuzzy_count + data_offset`. Read the old way, Mirror Image's track 0 returns the mask and garbage; read
correctly it returns the key sector `Rob Northen Comp…` and a BPB. `stx_fz.sector_bytes` does it right (a synthetic test pins
the difference); `stx.py` itself is unchanged here because another change owns it. Only that one track has a fuzzy mask on any of
the four `.stx` dumps (the ST Format disk is an `.msa`), so the earlier analyses of Image Works and Kixx are unaffected.

## 2. Mirror Image

It is one double-sided `.stx` (80 cylinders, ten 512-byte sectors per track, plus empty tracks 80-83), 1,600 sectors.

| Side in the `.stx` | What it is | Evidence |
|---|---|---|
| side 1 | **Image Works disk 2, byte for byte** | boot sector with OEM `X2B` and the text "Please boot from other disk"; all 800 sectors equal the Image Works disk 2's at the same track and sector id; the side images have the same sha1 (`810dc02c9714…`); mean entropy 7.54 bits/byte, 798 sectors above 7.0, no packer signature |
| side 0 | a **second protected disk (disk 1 of the pair)** | track 0 sector 6 is written with a CRC error (FDC status `0x88`), is about 4.9% longer than its neighbours (17,306 against about 16,500 time units) and holds `Rob Northen Comp…`; the sector has the fuzzy mask; the rest of track 0 is empty apart from one BPB-only sector and a 216-byte table (sectors 2 and 7) |

So Mirror Image uses **the same Copylock ST** as Image Works. Its key sector differs from Image Works disk 1's in exactly nine
bytes (offsets 16-24, the key seed), so the protected loader is re-keyed and every encrypted sector differs: of 782 non-empty
sectors, 31 are the same as Image Works disk 1's (the `RNC.` filler of the last tracks); the rest differ. The layout is
**similar, not identical**: the number of non-empty sectors per track matches on 77 of 80 tracks, but the number of
high-entropy (encrypted-looking) sectors per track matches on only 46 of 80 (Mirror Image's encrypted data starts at track 2
and ends at 32; Image Works' starts at track 10, with other high-entropy sectors on tracks 1 and 3). Both have a low-entropy
region on tracks 33-73 and the filler and key area on 74-76. So it is a re-keyed and re-laid-out relative of Image Works
disk 1, not a bit copy.

What is **not** known:

- **How the disk boots.** Side 0's sector 1 is a BPB with no code (checksum `0x1235`, not `0x1234`), and the executable
  boot sector on the dump is the one on side 1, which says to boot from another disk. The dump therefore holds the two
  disks of the release as two sides of one image; which physical medium Mirror Image booted from cannot be told from the
  dump. Image Works disk 1 itself has an executable boot sector.
- **Its programs.** The data is encrypted (Copylock ST: a trace-exception decryptor keyed from the sector), and the Image Works
  layers were not finished either (`atari-st-analysis.md` section 1). No RNC stream decodes: the two `RNC\x01` byte patterns on side
  0 are chance matches (their headers are nonsense) and the `RNC.` runs are the Copylock filler. Hence **no instruction
  comparison with Kixx or the Amiga/CDTV builds is possible for Mirror Image**, and none is claimed; the data comparison above is
  all there is.
- **Whether the Image Works disk 2 payload is the same game data as Kixx's** (likely, since the disk 2 sizes and layout fit,
  but unproven while it is encrypted).

Effort to finish Mirror Image: it needs the Copylock ST layers followed for both Image Works and Mirror Image (3-5 days for
the first, the second then follows with a new key); there is no separate packer to reverse.

## 3. The five 12,000-byte blocks on Kixx side 1

They are **RNC ProPack method 1** streams (`atari-st-analysis.md` called them "not RNC"; that was wrong: the header scan at
the end of the level modules finds them). Kixx side 1 holds, in order, the five level modules and then these:

| Block | Disk offset (side 1) | Packed | Unpacked | Both CRC16 | sha1 (unpacked) |
|---|---|---|---|---|---|
| 1 | `0x60e00` | 1,978 | 12,000 | ok | `4e7e47202ca1…` |
| 2 | `0x61600` | 2,238 | 12,000 | ok | `3bc627dde11b…` |
| 3 | `0x62000` | 2,548 | 12,000 | ok | `2c5943299d0f…` |
| 4 | `0x62c00` | 2,348 | 12,000 | ok | `03be102cf8f0…` |
| 5 | `0x63600` | 2,122 | 12,000 | ok | `dc84ec34a429…` |

They are the **per-level maps**: the Amiga PDos release stores five 12,000-byte blocks too
(`acquisitions-flux-acorn-st.md`), and each of them is inside the canonical Amiga disc 2, in the level modules, at these module
offsets, and also **byte for byte** in the CDTV overlay of the same level:

| Level | Amiga module (disc 2 sector) | Offset in the module | CDTV file | Offset in the file |
|---|---|---|---|---|
| 1 | 1 (216) | `0x19a0a` | `LEVEL1` | `0x19b94` |
| 2 | 2 (480) | `0x1cc12` | `LEVEL2` | `0x1cdde` |
| 3 | 3 (764) | `0x1f586` | `LEVEL3` | `0x1f7a0` |
| 4 | 4 (1120) | `0x28280` | `LEVEL4` | `0x2849a` |
| 5 | 5 (1441) | `0x23f44` | `LEVEL5` | `0x241b0` |

The Kixx block *i* is the Amiga block *i* **under a one-to-one remapping of the cell values**, not the same bytes:

- each block is 6,000 big-endian words; **the empty cells are the same in all 6,000 positions** of all five blocks;
- the value map from Amiga word to ST word is a **function and a bijection** in all five blocks (no Amiga word maps to two ST
  words and no ST word comes from two Amiga words; 177, 211, 183, 352 and 158 distinct words respectively, equal on both sides);
- the numbers of distinct values and of empty cells agree exactly (4,549, 4,171, 3,705, 4,266 and 3,915 empty cells);
- the cell values look like offsets into the level's tile/block definition table, with three flag bits (Amiga uses the flag
  values 1, 3, 5 and 7, the ST only 1 and 5), and the ST offsets grow faster (its definition records are longer); the byte-exact
  match with the CDTV overlays says the Amiga/CDTV maps are one data set, and the ST maps are the same layout with a
  different tile table.

Verdict: **the level layout data is the same across the Amiga, CDTV and ST builds** (cell for cell), and only the
tile-table offsets are platform-specific. Identity transformation for Amiga to CDTV; remap (bijection, formula not needed) for
Amiga to ST. A sixth Amiga block (PDos stream `0xefdbf`) did not verify (damaged sectors) and has no Kixx counterpart.

### The path tables

Each level module holds a table of ascending 32-bit pointers to its enemy path records, directly before the path data. They
are found by `stmap.ascending_runs`; the counts are the same on the Amiga module and the CDTV overlay of each level:

| Level | Entries | Amiga module offset | CDTV file offset |
|---|---|---|---|
| 1 | 87 | `0x14160` | `0x142ea` (file alignment 2) |
| 2 | 66 | `0x17abc` | `0x17c88` |
| 3 | 83 | `0x1a046` (alignment 2) | `0x1a260` |
| 4 | 72 | `0x22d90` | `0x22faa` (alignment 2) |
| 5 | 110 | `0x1e0e0` | `0x1e34c` |

For level 1 the table is immediately followed by the first path record (the 87 entries take 348 bytes, and the first pointer,
with the module loaded at `$54E00`, falls `0x15c` bytes after the table). That is the "87 paths for map 1" that the preview build's source residue names (next
section), which ties the residue to level 1.

## 4. The preview build (ST Format issue 3) carries source residue

The preview `X2.PRG` (240,896 B) has a header with no symbol table (symbol size 0, no debug flag), so there are no function
labels in it. It does contain **seven regions of leftover text, 3,069 bytes in all**, in the file image: a fragment of the
developers' **assembler source** (the largest region, 2,235 bytes at file offset `0x9a`, and a copy at `0x217a`) and four
screens of text.

What the source fragment shows, structurally (no lines are reproduced here):

- it is **68000 assembler for a macro assembler with `incbin`, `equ`, `dc.l`, `rept` and `endr`**; labels and directives are
  tab-separated; there are 28 `incbin` occurrences;
- it assembles the **level module** as `modbase equ *-16` ("first 16 unused"), then a **shot-shape pointer table**
  (`shot_shapes`, with `gsphere_ptr`, `rsphere_ptr`, `sgshot_ptr`, `srshot_ptr`), a **`pathlist`** built by a `rept` loop of
  87 `dc.l` entries, and **`paths`** with one `incbin` per path file of map 1;
- the editor buffer is overwritten with short fragments in places, which is why many lines are cut.

The other regions are the credits and menu strings. They are also found in the Kixx resident engine (the credits page, the "select / player game / music" page and the
high-score page; not necessarily at the same offsets); the credits page appears in the CDTV `X2` as well and is not in the
Amiga game image. The string `NuXENON2` is in the preview and in the Amiga game image.

Counts of what is in each build (occurrences of the given bytes):

| String | preview `X2.PRG` | Kixx resident | Kixx front-end | Amiga game image | CDTV `X2` |
|---|---|---|---|---|---|
| source labels (`modbase`, `shot_shapes`, `map1`, `incbin`) | present | none | none | none | none |
| `NuXENON2` | 1 | 0 | 0 | 1 | 0 |
| credits ("DESIGNED BY", "THE BITMAP BROTHERS") | 1 | 1 | 0 | 0 | 1 |
| credit for the music programmer and effects | 2 | 2 | 0 | 2 | 0 |

The full identifier list (77 identifiers) and the text regions are in the git-ignored
`generated/sources-extra/atari-st/out/` for study; they are copyrighted source and are not reproduced.

What it means: there is **no symbol table to import**, but the residue is real evidence of how the data was built
(path files per map, a module base, a shot-shape table), it ties the preview to the same level-data layout as the Amiga and
CDTV modules (section 3), and its pointer-table structure is why the path tables could be located without a code map.

## 5. Corrections and open items

- `atari-st-analysis.md`: the five 12,000-byte blocks are RNC streams (section 3 above) and the ST Format preview has source
  residue but no label table (section 4); the sentence "the labels are a source of function names" should read "the
  residue names data tables, not functions".
- `stx.py` mishandles fuzzy-mask tracks (section 1); `stx_fz.py` is the corrected reader.
- Open: the Copylock layers (Image Works and Mirror Image engines), Mirror Image's boot path, the Image Works disk 2 payload,
  the formula behind the ST cell values (not needed for any current target), and whether the preview's `map1` path files equal
  the Amiga level 1 path records (the data is not in the residue).
- Value to the project: none for the current targets. The finding that map and path data are the same across all three builds
  supports reusing the Amiga/CDTV level analysis if an ST target is ever built.
