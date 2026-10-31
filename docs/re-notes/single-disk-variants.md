# Other retail variants: Power Pack, the 1762 compilation, "4 Others", LGCSS, and the builds behind them

Part of the disk-variants work (analysis half of phase P6). Findings only: no game bytes appear here or anywhere in
the repository. Every number below was measured on the images named by their public identity (SPS number, TOSEC
name, WHDLoad install, sha1); nothing says where an image is kept. The recipes that reproduce each finding are in
`tools/variants/` (run any of them with `-h`); the synthetic-data tests are `tools/variants/test_variants.py`.

Companion notes: `retail-disk1.md` (the two-disk retail release and its Copylock), `sources.md` (the catalogue), the
WHDLoad imager source for Xenon 2 (public domain, by Wepl), which documents the six floppy versions and which this
work follows where it says so.

## Summary

| Variant | Disk layout | Game image | Disc 2 (game data) | Protection | Runs in today's runtime? | Support effort / risk |
|---|---|---|---|---|---|---|
| **"Xenon 2 & 4 Others"** (TOSEC `(19xx)(-)`, 2 disks) | ordinary OFS disk, menu; game file named `1` | **byte-identical to canonical** (the file has the BS1 `XENON-II` sha1 `778cf75e…`) | disk 2 differs from canonical in 2 unused sectors | none (cracked) | **yes, after finding the exe by structure** | **low** |
| "4 Others" `[a]` | same | same | **damaged copy**: tracks 61 and 79 are a copier's bad-track filler | none | **no**: levels 3 and 4 break | reject with a clear message |
| **Power Pack** (SPS 2234, 1 disk; imager v6; WHDLoad `1Disk 2234`) | standard track 0, Speedlock key track 1, custom data tracks 2-159 (all decode, checksums verified) | the *same build as the first retail pressing* (SPS 0297) but with its own startup code, loader and **reduced-precision samples**; needs two crunch stages | the six level modules are stored as game files and **reproduce the canonical modules exactly**; the other 143 sectors of disc 2 are never read | Speedlock (intro only) | **yes, since P6b** (`build-variants.md`): runtime entry/start-up variant | **done** |
| **Bombuzal + Xenon 2** (SPS 1762, 2 disks; imager v2) | both disks are ordinary 11-sector tracks; disk 1 is a custom loader disk with a blank track 1 (no Copylock) | game image stored **plain** at disk offset `$70C00`, but a **different build** (entry `bra.w $6C3A`, code after `$D000` shifted by -1502 bytes) | modules 1-4 identical to canonical; module 5's last sector and all of module 6 differ | none found | **yes, since P6b**: its own loader address (`$9012`) and probe addresses; replays are its own goldens | **done** |
| **LGCSS** compilation (TOSEC, 1 disk) | ordinary OFS disk | contains `X2.BIN`, 379,232 bytes: the **playable demo**, not the game | none | none | n/a | recognise and report only (plan D3) |
| Budget one-disk standard (imager v4) and PDos one-disk (imager v5) | v4 all-standard tracks; v5 `$1800`-byte PDos tracks with sync `$1448` | game image crunched on the disk and decrunched to `$400` by the WHDLoad slave (load offsets are in its source); documented by the imager/slave source only | n/a | none (v4); PDos (v5) | unknown | **not analysable: no image of either is available** |

## Reading the images

All three IPFs below decode with `tools/ipf.py` + `tools/mfm.py`; the Power Pack's custom tracks need the custom
decoder in `tools/variants/speedlock_v6.py` (layout from the WHDLoad imager source).

### "Xenon 2 & 4 Others" (two ADFs, plus `[a]`)

- Disk 1 is an AmigaDOS OFS disk (boot-block CRC16 `6bdc`, shared with the LGCSS disk) with a menu script
  (`s/menu`: F1 = Xenon 2 "Datadisk required", F2-F5 = other games). The Xenon 2 executable is the 205,784-byte file
  named `1`: **sha1 `778cf75e75d7e1b2036f4d68f8a557b69e99a817`, the same file as `XENON-II` on the BS1 `[t+3]` disk**.
  So the game image it yields is the canonical one; the importer only has to find the file by structure instead of by
  the name `XENON-II`.
- Disk 2 (sha1 `5e5d786550…`) differs from the canonical disc 2 in two sectors: sector 8 (track 0: canonical is format fill, here
  other junk) and sector 1752 (track 159: zero in canonical, junk here),
  both unused. **Measured equivalence:** with the canonical image and this disk 2, the replays `level1_play`, `boss1`,
  `level3`, `level4_boss`, `ending_natural` and `shop5` match every recorded frame and audio hash.
- The `[a]` disk 2 (sha1 `31997fddbf…`) is a **bad copy**: sectors 671-681 (track 61) and 869-879 (track 79) hold a
  copier's `(W)XCOPY` filler where level data should be, plus partial damage in sector 737. Measured: `level3`,
  `level4` and `level4_boss` replays no longer match; `level1_play`, `boss1`, `level2`, `level5`, `shop5` and
  `ending_natural` still do. The `[a]` disk 1 carries the same executable.

### LGCSS compilation

Contains the demo: `X2.BIN` (379,232 bytes, sha1 `24e39807…`) is **byte-identical to the WHDLoad demo's `x2.bin`**,
beside a 1,024-byte `X2DEMO.BIN`, and the other files belong to other games. One file (`4`, 140,164 bytes) has the same decrunch stub shape as `XENON-II` but unpacks to
378,722 bytes: a different game. **Importer hardening:** the C++ importer's executable test (`is_xenon_exe` plus the
`$3FC00..$80000` size window) accepts that file; the real `XENON-II` unpacks to `$464CA` bytes. A tighter test (the
exact size, or the decrunched image's entry word `60 00 6C 30` and tail) should replace the window before disks of
other compilations are offered to the importer.

### The Power Pack (SPS 2234)

Layout, as in the imager source (version v6) and confirmed here on the IPF:

| Tracks | Content |
|---|---|
| 0 | ordinary Amiga track (boot) |
| 1 | Speedlock key track (4 blocks, 1800/2000/2200 ns cells) |
| 2 | custom: sync `$4489`, then `$1800` bytes (the start of the loader) |
| 3-159 | custom: one block per track, sync `$4489`, 4 raw bytes, `$1B00` data bytes as an even block + checksum + odd block + checksum |

All 158 custom tracks decode with valid checksums. The WHDLoad install's `Disk.1` (1,085,184 bytes, sha1
`6461b317…`) is byte for byte the decode of tracks 3-159, so either form is a valid input.

On those 157 tracks (`$1B00` bytes each): the **intro** is `$4B170` crunched bytes at track 3 (`$63D9C` bytes unpacked), the
**main** image is `$2A314` crunched bytes at track 48 (`$3F1F4` bytes, loaded at `$E0C`), and the **six game files** sit
at tracks 74, 86, 100, 115, 134 and 149, each crunched and then run-length coded. Both crunchers are ByteKiller-style
(backwards, MSB-first for intro/main, LSB-first for files).

Findings:

1. **The six files are the level modules, exactly.** Decoded, they equal the canonical disc 2 at sectors 11, 250, 514,
   799, 1154 and 1476 (117,248 / 130,048 / 140,800 / 176,640 / 159,744 / 102,656 bytes). Five are byte-identical; module
   2 differs in one byte (offset 3: `$AB` here, `$AA` canonical).
2. **Only the modules matter on disc 2.** A disc 2 holding just sector 0 and those six modules (1617 of 1760
   sectors; the other 143 are sectors 1-10 and the 10-sector blocks before each later module, zero here) passes **all 19
   replays** with identical frame and audio hashes. So `disc2.bin` can be rebuilt from the Power Pack functionally,
   not byte-for-byte (the 143 unused sectors and the disc-id sector would be synthesised).
3. **The game image is the first-pressing build with a different front end.** Composed from the unpacked intro (whose first
   `$A0C` bytes are the intro's memory; the canonical image has only the 4-byte entry stub `60 00 6C 30` there and zeros
   otherwise, so that area can be synthesised) and the main image, it matches the canonical image everywhere except: `$7032` (8 bytes) and `$70E0` (1,370 bytes), the **startup code** (the imager
   source jumps to `$762E` instead of `$7032`); `$8FD2` (16); `$921A` (986 bytes, the **disk loader**: the same
   address the runtime already hooks, but a different routine with its own file table at `$9512`); and **two sample
   blocks** (`$218FE` 37,466 bytes and `$2BAA2` 40,238 bytes). Against the first retail pressing it matches at `$1F8A`,
   `$76AE`, `$874A` and `$894A` too, so it is that pressing's revision of those places.
4. **The samples are lower precision.** In both blocks every byte of the Power Pack has only bits that the canonical
   byte also has, and the canonical byte is 0-15 higher: the low 0-4 bits were dropped (a cheaper sample set for the budget
   release). The frames of a Power Pack game would match; the audio would not.
5. **Runnable since P6b (`build-variants.md`); the paragraph below is the analysis that led there.** With the low area borrowed from the canonical image and the Power Pack's main, the runtime
   reaches its first VBL handler but never the disc-id check, and none of the recorded frame or audio hashes match,
   because it enters at `$7032` and the Power Pack's startup routine lives elsewhere. Running it needs an entry/startup
   variant in the runtime (and a disk shim keyed on the Power Pack's file ids, which are the module start sectors), and
   golden hashes of its own.

### Bombuzal + Xenon 2 (SPS 1762)

- Both disks decode as ordinary tracks (160 of 160). Disk 1: boot-block CRC16 `c0fb` (imager v2); the boot block is a
  custom loader; **track 1 is a constant fill, so there is no Copylock key track**; the game image is stored **plain**
  (no XOR chain) at disk offset `$70C00`, size `$3FC00` (sha1 `a665f659…`).
- It is a **different build**: entry word `60 00 6C 3A` (canonical `60 00 6C 30`); the first `$D000` bytes shift by +10
  or 0, everything after by -1502 bytes. Only 20,679 of 261,120 bytes agree at the same offset.
- Disk 2 (sha1 `c294d664…`): 203 sectors differ from the canonical disc 2: sector 0 (after the `X2B ` id), the last
  sector of module 5 (171 bytes) and all 201 sectors of module 6 (shop and ending), whose code calls routines at
  addresses of the 1762 build (`$93EC`) where the canonical module calls `$95F4`. Modules 1-4 are identical.
- **Runnable since P6b** (`build-variants.md`). The analysis here said the runtime refused the image (`unexpected entry code`) and
  never reached the first VBL handler; the cause was the disk loader sitting at `$9012` instead of `$921A`. Supporting it means treating it as a second build in the runtime, with its
  own hook addresses and its own hashes.

## What this means for the importer

1. **"4 Others" is a near-free win:** find the executable by structure, accept the non-`[a]` disk 2 (or any disk 2
   whose six modules match), and reject disk 2 images that contain the `(W)XCOPY` filler in module tracks.
2. **A disc 2 only needs the sector-0 disc id and the six modules.** That makes it possible to build a canonical-shaped
   `disc2.bin` from any variant that can supply the modules: the Power Pack today, the one-disk budget releases if
   images turn up. It also means a damaged disk 2 can be detected precisely: compare the six module ranges.
3. **The Power Pack and the 1762 compilation are different builds** and need runtime work, not only importer work:
   the Power Pack is close to the first retail pressing, the 1762 build is further away (shifted code). Neither belongs in
   0.2.0 unless the runtime grows an entry/startup abstraction; the importer can still *recognise* them and say why.
4. **Importer hardening:** the executable test must not accept other crunched programs (see LGCSS above).

## Recommended order of support

1. "4 Others" (non-`[a]`), with the structural exe search and the damaged-disk-2 check.
2. The two-disk retail pressings (already analysed in `retail-disk1.md`), then the first-pressing differences.
3. Power Pack: importer recipe (any of IPF, WHDLoad `Disk.1`) and a runtime entry/startup variant, with its own goldens.
4. 1762: only if the runtime gains a second-build mechanism; recognise-and-report until then.
5. Budget v4/v5: when an image of either can be obtained; the imager source describes where the game image sits.

## Not determined

- v1, v4 and v5 images: none could be examined. v4/v5 are known only from the imager source (game crunched into `$400`,
  patch offsets that differ from this build).
- Whether the 143 unused disc 2 sectors are read by anything outside the 19 recorded replays (none was seen).
- What the Power Pack's intro and its Speedlock check do beyond the loader (the imager skips both).
- Why module 2 differs by one byte in the Power Pack (a version difference or a decrunch edge case).

## Recipes

```
python3 tools/variants/identify.py IMAGE...                       # boot CRC -> imager version, executables, demo, bad tracks
python3 tools/variants/four_others.py D1.adf D2.adf [--canonical-disc2 F]
python3 tools/variants/power_pack_2234.py IMAGE.ipf|Disk.1 [--out DIR] [--canonical-game F] [--canonical-disc2 F]
python3 tools/variants/compilation_1762.py D1 D2 [--canonical-game F] [--canonical-disc2 F]
python3 tools/variants/replay_compare.py --exe BUILD/x2recomp --game F --disc2 F [REPLAY...]
python3 -I -m unittest discover -s tools/variants -p 'test_*.py'  # synthetic fixtures only
```

The recipes read images and print hashes and offsets; `--out` folders are for analysis and must stay git-ignored.
