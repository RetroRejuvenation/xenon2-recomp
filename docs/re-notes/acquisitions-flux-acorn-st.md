# Acquisitions: a real retail flux dump, the Acorn User '97 update, and the Atari ST dumps

The three top acquisitions of `other-versions-and-sources.md` were fetched from public archives and analysed.
Everything below is hashes, sizes, offsets and prose; nothing derived from the images is in the repository (extracted
data stays in the git-ignored `generated/`). Tools written for this are in `tools/acq/` with synthetic tests
(`python3 -m unittest discover -s tools/acq -p 'test_*.py'`, 18 tests).

## 0. Summary

| Acquisition | What it turned out to be | Value |
|---|---|---|
| Internet Archive `amiga-original-xenon-2-powerplay` (SCP + FluxEngine `.flux`) | Not the Power Pack's custom layout: a **third, previously unseen floppy version, WHDLoad imager "v5", Rob Northen PDos** (one disk, 12 sectors per track). First real flux of this game. | New variant, **now implemented** (`pdos-v5.md`): importer, FluxEngine reader, retry pass, runtime build. Imports only from a complete dump; this dump has 7 unreadable sectors, so it is refused. |
| Acorn User CD 3 (`cdrom-riscos-au-cd3`) | The **complete January 1997 Acorn User update** of the game (described as protection-free by the Arc Wiki), as one ArcFS archive (1.26 MB unpacked, 42 files). | Complete Archimedes data set; a full ProTracker module of the in-game tune. No use for the recompilation. |
| Atari ST (Atarimania dumps) | Image Works (2 disks), Mirror Image, Kixx (1 double-sided disk), and the ST Format issue 3 cover disk. The disks share **no bytes** with the Amiga or CDTV builds. | A separate port (by Martin Day's code base); not directly useful until its loader/packer is reversed. |

## 1. The Amiga flux: it is the PDos release

### Provenance
| File | Size | sha1 |
|---|---|---|
| `Amiga - Original - Xenon 2 [Powerplay].scp` | 36,514,264 | `457da8ebea6590a8b152a90f73cbcc8b9753b57e` |
| `Amiga - Original - Xenon 2 [Powerplay].flux` | 28,810,078 | `ffe3dc498b6d95aee872ece6547a3bfc918128f3` |

Source: https://archive.org/details/amiga-original-xenon-2-powerplay (item description: "Xenon 2 for Amiga computers,
original retail disk"; collection `open_source_software`, added 2023-08-31). Retrieved 2026-10-31. "Powerplay" is the
name on the item; which compilation it is has not been established.

### Formats
* `.scp`: SuperCard Pro, version 0, disk type `0x80`, 3 revolutions, tracks 0-163, flags `0x03` (index, 96 tpi),
  25 ns resolution, big-endian 16-bit deltas. Our C++ reader parses it correctly.
* `.flux`: **FluxEngine's native format**: a protobuf stream (field 1 = magic `0x466C7578` "Flux", field 2 = version 2,
  then 160 track messages {cylinder, head, flux bytes}); flux bytes use bit 7 = pulse, bit 6 = index, low 6 bits = ticks
  of 83.333 ns, `0x3F` = +63 and continue. It is an **independent capture** of the same disk (about 3.2 revolutions per
  track, a different start phase from the SCP). Format reader: `tools/acq/flux.py`. The C++ importer reads it
  (`runtime/import/fluxengine.cpp`) and merges it with a second capture of the same disk (the extra revolutions recover sectors, below).

### What the disk is
`--diagnose-image` on the SCP: track 0 standard Amiga MFM, **159 raw tracks**, no Copylock key track, game image at
`$70C00` not readable: so not retail disk 1. It is also **not the SPS 2234 Power Pack layout** (which has one monolithic
block per track and a Speedlock key track; the importer refuses the SCP for that reason). The raw tracks hold 100,100
cells per revolution at a uniform 2 us cell (the Power Pack's tracks are longer than a revolution at that rate), with
**12 sectors of 8,288 cells** each.

That is exactly the WHDLoad imager's **version v5**, "Rob Northen PDos" (`xenon2.islave.s`: `TLENTRY 1,159,$1800,$1448,
_decodev5`): a standard track 0, then tracks 1-159 of 12 sectors x 512 bytes. Until now v5 was known only from the
imager source; no image had been found. **The 1 KB boot block's CRC-16/ARC is `$bdce`, which is the imager's own v5
identifier** (`docs/re-notes/sources.md`: v1 `7f8f`, v2 `c0fb`, v3 `7ea7`, v4 `7e53`, v5 `bdce`, v6 `6827`). The sector layout
(checksummed, with a running XOR key over the data) is documented in `tools/acq/pdos.py` and verified here on every track.

### Result of decoding the real dump
* Track 0 decodes as ordinary Amiga MFM (boot block `DOS`, bootable, with a custom loader that prints a "LOADING GAME"
  line).
* Of 1,908 PDos sectors, **1,901 read directly with a valid track number, sector number and checksum**; **7 (0.4%)
  remain unreadable** (tracks 95, 97, 99, 100, 101, 110, 114; weak spots in the media: the flux there has half-cell
  intervals in every revolution of both dumps).
* The assembled disk image (982,528 bytes, the 7 unreadable sectors zero-filled) has sha1
  `32cdbe74f8fd9e9b488e58087b8a40e6f0eb383a`; `python3 tools/acq/pdos.py DUMP.scp --flux DUMP.flux --out DIR` reproduces it.
* The disk holds **RNC (Rob Northen ProPack method 1) packed files**: 14 streams from `0x1606` on. `tools/acq/rnc.py`
  decodes them; a stream counts only when both CRC16s in its header match. **10 verify bit-exactly**; the 4 that do not
  contain the unreadable sectors (or, for the last, run past track 159).

| Stream (image offset) | Unpacked | sha1 of unpacked data | Content |
|---|---|---|---|
| `0x1606` | 409,146 | `fed11e9c5331e1d9cdbd250088d7bd50d8a4132c` | stage 1: the resident loader and intro (entry is bare-metal, `move #$2700,sr`); the runtime does not run it |
| `0x58806` | 243,372 | `5116260e663d43a387912a0d16487b8bb441d0d0` | **the game**: decrunched to `$400`, entry `bra.w $7382` (`pdos-v5.md`) |
| `0xb5c47` | 164,278 | `5b1ab0df08d9b7d221078dfc3d03db3f0e278ca0` | data |
| `0xce59e` | 147,074 | `99a41846bdce84721220988b038db037e5f83aeb` | data: 94% of its 48-byte windows are found in the canonical disc 2 |
| `0xdf358` | 102,824 | `d8c6c44e0edac39f032ba3358b65b639b27fe1b3` | data: 91% found in the canonical disc 2 |
| `0xed18b`, `0xed94a`, `0xee222`, `0xeec1e`, `0xef565` | 12,000 each | `d72e634b…`, `eb953931…`, `ea0316c5…`, `f542e647…`, `8c7ecfd1…` | 100% found in the canonical disc 2 |

So this release stores the canonical **disc 2 data** as RNC files (the windows that differ are the level modules'
relocations/cosmetic changes, to be characterised), and its resident program is a **different build**: its code is the
retail game's code in large relocated blocks (the same offset deltas against both retail pressings, e.g. 59,872 and
193,586; the 1762 build's deltas are 1,502 larger), together with extra code (the Bitmap Brothers intro mentioned in the
slave). It is not byte-comparable to the canonical image.

### Lessons for the decoder (both are now in the C++ importer: `flux.md`, `pdos-v5.md`)
1. **Phase tracking matters on weak media.** An interval-based PLL (round each interval separately) left 36 sectors
   unreadable; a phase-tracking PLL (with the valid-MFM clamp, runs of 2-4 cells) read 24 of them directly. The C++ importer's PLL already
   tracks phase (gains 0.1 / 0.005, no clamp). The clamp was what helped most on the weak tracks, so a retry pass with
   `n` limited to 2-4 cells for sectors that stay unreadable is the retry pass now in `decode_flux_track` (it applies only to tracks that hold standard sectors and to the alternate streams of raw tracks, and
   never to the first pass, because Copylock/Speedlock key tracks legitimately break the 2-4 rule). Measured in `flux.md`.
2. **Checksum-guided repair is not trustworthy with a 16-bit checksum.** Flipping the most ambiguous intervals until a
   sector's checksum validates "repaired" 22 sectors; 12 could also be read directly, and **10 of those 12 disagreed**:
   the flips were false positives. Repairs were discarded; only direct reads are used. Stream CRC16s (an independent,
   whole-stream check) are what validate the result.
3. **A second, independent capture helps** (the `.flux` revolutions recovered 5 of 12 sectors the SCP could not).

### What importing it took (done: `pdos-v5.md`)
A PDos track decoder (small, specified above), an RNC decoder (small), a file table (the slave reads 22 entries at disk
offset `$400+$50a`), the v5 boot/loader traps (the slave patches `$63a3e` and the `$14006` loader) and a build-table entry
(as for the Power Pack and 1762), plus a flux read quality sufficient for every sector (7 are unreadable in this dump:
a better dump of the same disk, or a physical re-read, is needed for a complete import). Done, with the one limit that a complete dump is still needed; the table above is also corrected there: the game is the stream at `0x58806`, and the file table has eleven entries.

## 2. Acorn User CD 3: the complete 1997 update

| File | Size | sha1 |
|---|---|---|
| `AUCD3.iso` (https://archive.org/details/cdrom-riscos-au-cd3, "Acorn User Cover CD 3, December 1997, issue 188") | 488,001,536 | `95e0d003b20afe2a71bb528f24b9e90ed8015cd8` |
| `ACORNUSER/1997/JAN1997/XENON2` (one file in the ISO, an ArcFS archive) | 1,012,899 | `8591a8291f1b9b82fb7c567069f05e24345d8307` |

Retrieved 2026-10-31. The ISO is a plain ISO 9660 image (`bsdtar` lists it). Only the one Xenon 2 file was extracted.

**Format.** `Archive\0` (ArcFS), 42 files; methods `0x82` (stored) and `0x88` ("crunched": Unix-compress LZW, 9 to 16
bits, then Spark RLE with `0x90` escapes). Reader: `tools/acq/arcfs.py`; all 42 files decode to their directory length.
(The first decoder attempt with a 12-bit limit failed on large files; the maximum is 16 bits.)

**What it is.** The archive's own README says "Xenon2 '97 Acorn User update by Stephen Harrison": QTM sound player
(4 or 8 channels), VGA monitor support, runs from one HD floppy, two low-density discs or a hard disc, on all Acorn RISC
OS machines from the A310 to the StrongARM. Its credits screen names (as in the game) Andrew Neville and Stephen Annetts
(Archimedes programming), Stephen Harrison ('97 update), The Assembly Line (Amiga version), Mark Coleman (graphics),
Bomb the Bass (music) and David Whittaker (music and sound effects). The program contains a developer's note stating the
Archimedes conversion was made without source code and that its copy protection is simple. The Arc Wiki describes the '97
update as protection-free; the program code was not analysed here to confirm that.

**Contents** (1,259,169 bytes unpacked): the main ARM program `Xenon2` (354,796 B), the QTM player module (57,288 B),
two sprite files, loaders, and for each of five levels the compressed level data (`CompData0/1`), `SpriteTab` and boss
data (`BigGuy0/1`), plus the shop templates. The earlier **demo** (521,904-byte program with Desktop Tracker music) is a
different program; this one uses ProTracker `M.K.` modules.

**Music.** The program embeds a **ProTracker module titled MEGABLAST** (23 positions, 15 patterns, 29 samples named like
the demo's `MEGBDRUM`, `MEGASDR1`, `MEGAHATC`, `MEGATUTS`, `MEGABASS`, `MEGAGUIT`, `MEGASTAB`...), 135,746 bytes, and a
small 12-sample module (sound effects and speech words) in the shop data. Rendered with `tools/acq/modplay.py` to
`generated/platforms/wav/acorn-update/megablast_full_tune.wav` (172.8 s, one pass) for listening; the renderer is
approximate. Whether this tune is the Amiga's composition (David Whittaker) rearranged, or Bomb the Bass material, is
for the listener; its relation to the Amiga data is not established here.

**Use to us.** The art and level format were previously found identical to the Amiga's in the demo; this is the complete
set for the Archimedes. Nothing in it helps the recompilation (it is an ARM program). It is a complete, protection-free
reference for the Acorn port and a source of a full tracker module. Rights: Acorn User/Eclipse/Bitmap Brothers; nothing
here is committed.

## 3. Atari ST

Public dumps were available from Atarimania (direct links on each version page, no login); the Internet Archive has only
the first game (Xenon, 1988) for the ST. The Atarimania "Xenon II" page lists version pages for Image Works, Mirror
Image, STe DMA sound (a patch, no dump link), Kixx and an MCM entry (no dump).

| File | Size | sha1 | Notes |
|---|---|---|---|
| `xenon_ii_-_megablast_imageworks.zip` (https://www.atarimania.com/st/dumps/xenon_ii_-_megablast_imageworks.zip) | 1,168,918 | `72668fd7f592de3a1560d22e9928399e6bfd9e7e` | two Pasti `.stx`: disk 1 (932,682 B, `dd64375a…`), disk 2 (933,010 B, `c827ba60…`) |
| `xenon_ii_-_megablast_mirror_image.zip` | 1,197,180 | `7cd22057e0e0e8d97288ca79d45eba4329a80bd0` | one `.stx`, 1,772,126 B (`123d53d3…`), double-sided |
| `xenon_ii_-_megablast_kixx.zip` | 1,734,358 | `559f627020b33e5305d02bb21985505fdb238b98` | `.st` 819,200 B (`323d6859…`) and `.stx` 1,706,268 B (`a7f128f8…`): one double-sided disk |
| `stf03.zip` (ST Format issue 3 cover disk) | 169,915 | `a8eb7377da9c4be72381399d0f18da73973d89f1` | `stf.03.msa` 355,500 B (`62189da2…`), 10 sectors x 2 sides x 81 tracks; holds the preview (not examined further) |

Retrieved 2026-10-31. Reader: `tools/acq/stx.py`.

**Format.** Image Works disks: single-sided, 80 tracks of **10 sectors** (interleaved order 9,10,1,2,...), 409,600 B each;
boot sector with a valid BPB (512 bytes/sector, 400 KB) and a checksum of `0x1234` (bootable); OEM id `X2` on disk 1 and
`X2B` on disk 2, the same disc-id idiom as the Amiga's disk 2 (message "Please boot from other disk"). The rest is not a
FAT file system: the game loads by raw sector. Kixx is the same family as one 800 KB double-sided disk with the boot
message "Colour monitor required".

**Content.** Disk 1 holds a plain-text block "Xenon 2 demo program, (c) Copyright 1988,1989 Martin Day", a register-dump
routine and a `PROGSTART` label: debugging leftovers from the same code base whose `progstart` and `shop_right` symbol
strings appear in the Amiga build. The data is packed or encrypted (entropy 7.4-8.0 bits per byte from track 2 on;
disk 2 is entirely high entropy), **with no packer signature found** (no RNC, Ice, ATM5, PP20, JAM signatures).

**Comparison with the Amiga and CDTV builds.** Windowed byte matching (48-byte windows, low-variety windows excluded)
finds **no shared data** between the ST disks and the Amiga disc 2, the Amiga game image, or any CDTV file; Kixx shares
64 of 177,011 windows with Image Works disk 1 (a different packing of the same game). So the ST version is a separate
port in packed form; comparing its engine with the CDTV overlays (which are Atari-format `0x601A` files) would first
need the ST loader/packer reversed (the boot sector and the loader code at track 1 are plain 68000: an estimated 2-4
days). It is therefore not yet useful for the CDTV recompilation.

**Update:** the loader and packer have since been reversed; see `atari-st-analysis.md` (Image Works is Rob Northen
Copylock ST, Kixx is RNC ProPack and unprotected, and the unpacked ST engine is a close relative of the CDTV code).
The byte-level "no shared data" result above stands for the packed disks.

## 4. Recommendations

1. **PDos release (P6c):** implemented (`pdos-v5.md`). A complete dump is still needed to import it (the Internet Archive item has 7
   unreadable sectors out of 1,908 even with two independent captures; ask whether a better dump exists, or re-dump
   locally). It is the only known image of imager version v5.
2. **Flux reader:** done: the retry pass with the 2-4 cell clamp and the FluxEngine `.flux` reader, with merged captures (`flux.md`).
3. **Acorn update:** nothing to build from it; keep as the Archimedes reference (the listen-only WAV is the deliverable).
4. **Atari ST:** done as analysis (`atari-st-analysis.md`); a Kixx import is the cheapest next step if an ST target is wanted.
5. **Legal:** all of this stays in git-ignored staging; the repository holds notes and tools only.
