# CDTV release analysis (Lane C)

Source: `Xenon-2-Megablast_CDTV_EN-FR-DE-IT_Disc-Image.zip` (Redump-style, one BIN per track + CUE).
Working dir (gitignored): `generated/lane-c/` (`disc/` = extracted BINs, `data-files/` = ISO files,
`adf/` = disk-version extraction, `wav/` = 5 s verification samples, `data-files-tail-raw.bin` = see Open Q1).

Tools written: `tools/iso9660.py` (raw MODE1 2352 reader/extractor), `tools/cdtv_tracks.py` (track survey + WAV export),
`tools/cdtv_analyze.py` (AmigaDOS hunk summary), `tools/unpack_xenon2_disk.py` (decruncher for the disk-version exe).
All verified runnable with `python3 -I`.

## 1. Disc layout

CUE is present: 13 files / 13 tracks. Track 01 = `MODE1/2352` data, tracks 02-13 = `AUDIO`.
Each audio BIN carries its own 150-sector (2 s) zero pregap (CUE `INDEX 00 00:00:00`, `INDEX 01 00:01:62/63`).
The CUE INDEX 01 values (137/138 frames) are not what the data shows: measured programme audio starts at sector 150 in every
audio file (tracks 02-11 also have one stray non-zero sample in sector 0; 12-13 are fully zero for 150 sectors).

Track 01 (6005 raw sectors, sync+header 16 bytes, 2048 user bytes at +16, sector 0 MSF 00:02:00):
- sectors 0-15: zero; sector 16: PVD (`CD001`, system id `CDTV`, volume `XENONII`, 2449 blocks)
- sectors 0..2448: ISO9660 volume (Level 1 names, `;1` versions, no Rock Ridge/Joliet)
- sectors 2449..5855 (3407 sectors, 6,977,536 user bytes): NOT in the ISO volume, non-zero, low-entropy (6.9 bit), smooth,
  looks like 8-bit signed PCM (mean |delta| 7). Referenced by nothing found so far (Open Q1).
- sectors 5856..6004: zero (149 sectors).

## 2. Track table (audio)

| Trk | sectors | programme len | peak | note |
|----|--------|---------------|------|------|
| 01 | 6005 | - | - | DATA MODE1 |
| 02 | 20800 | 4:35.33 | 26760 | title string "MEGABLAST XENON II MIX" |
| 03 | 31975 | 7:04.33 | 30203 | "MEGABLAST INSTRUMENTAL" |
| 04 | 28012 | 6:11.49 | 25004 | "MEGABLAST" (2nd line blank) |
| 05 | 23950 | 5:17.33 | 28898 | "MEGABLAST HIP HOP ON PRECINCT 13" |
| 06 | 23325 | 5:09.00 | 26029 | "MEGABLAST ORIGINAL RAP FEAT. MERLIN" |
| 07 | 30825 | 6:49.00 | 20063 | "MEGABLAST RAP FEAT. MERLIN" |
| 08 | 20325 | 4:29.00 | 28018 | "DESIRE THROUGHOUT THE ENTIRE WORLD" |
| 09 | 25350 | 5:36.00 | 22491 | "DOIN THE DO" |
| 10 | 26775 | 5:55.00 | 24981 | "THEME FROM S EXPRESS" |
| 11 | 30338 | 6:42.51 | 27665 | "NOTHING TO LOSE" |
| 12 | 15175 | 3:20.33 | 30584 | "BOOS BOOMING" |
| 13 | 25925 | 5:43.67 | 24376 | "OOCHY KOOCHY" |

Programme length = (sectors-150)/75. None are silent. The title strings are the 12 consecutive 40-byte records at X2 hunk
offset `$3EC32` (record index = disc track - 2, see code at `$3E03E-$3E052`). The track-to-title pairing for 04 and 07/08
is slightly uncertain because the strings wrap across records; the code indexes strictly by (track-2)*40.
Samples: `generated/lane-c/wav/track{02,07,12}_sample.wav` (5 s each, 20 s into programme).
Music is Bomb the Bass / Tim Simenon remixes (credits strings in X2 and PICCODE), i.e. NOT the tracker music of the disk version.

## 3. ISO9660 file list (101 files)

```
CDTV.TM            22152  lba 414   cdtv.device 35.2 (6.2.91) driver module, raw (starts 4E75), not a hunk file
CDTVPREFS           5876  lba 28    AmigaDOS hunk exe (CDTV prefs; bookmark.device)
RMTM                2820  lba 26    AmigaDOS hunk exe (remote-control/player prefs helper; opens playerprefs.library)
DEVS/SYSTEM-CONFIGURATION 232 lba 23  identical size to the disk version's devs/System-Configuration
S/STARTUP-SEQUENCE    20  lba 25    "rmtm\nCDTVPrefs\n\nx2\n"
X2               455288  lba 2222  AmigaDOS hunk exe: CODE 437,404 B (4452 relocs) + BSS 372,160 B. Main game.
INTRO             95816  lba 1116  Atari-ST-style 0x601A PRG image: manual/help text in EN,FR,DE,IT + attract pages
LEVEL1..LEVEL5   118347 131487 142292 178589 161445  (lba 1163 1221 1354 2134 1649)  0x601A images, level code+data
PICCODE           30203  lba 588   0x601A image: credits/info screens (Bomb The Bass text, EN/FR)
PICDATA1-9,B,C    15959..89289 bytes (11 files)  0x601A images: picture/shop/info graphics
SOUNDS00..SOUNDS09 57468..172390 (10 files)  sample banks (header = (offset,len..) table, e.g. 000002A0 00020040 01BF ...)
SPEECH00..SPEECH66 9208..53870 (67 files)    voice samples; begins with zero table
```
Full listing with LBAs: run `python3 -I tools/iso9660.py <Track01.bin>`; extraction target `generated/lane-c/data-files/`.
`0x601A` files: 28-byte Atari ST GEMDOS PRG header (tlen at +2, dlen/bss/sym/res/flags all 0), `0x1C+tlen` then a
relocation byte table (trailing 5..1571 bytes). These look like the Bitmap Brothers' cross-platform overlay format,
not AmigaDOS hunks; loaded by X2 from `CD0:LEVELn` / `CD0:INTRO` (strings at X2 `$3CE54`, `$3D3CC`), `CD0:SPEECHnn`
(`$40C34`) and `CD0:SOUNDSnn` (`$4F9CC`).

## 4. Code differences vs disk version

Disk 1 (`Xenon II - Megablast (1of2)[t+3].adf`, OFS, bootable): `XENON-II` (205,784 B, hunk file with ONE crunched blob, no relocs),
`BS1` (30,516 B, entropy 7.5, packed), `c/add21k`, `s/startup-sequence`, `devs/System-Configuration`. Disk 2 is a non-DOS data disk.

`XENON-II` decrunches (stub at code offset 0, backward bit-stream cruncher in the Bytekiller family; transliterated in
`tools/unpack_xenon2_disk.py`, verified plausible: yields strings "(c) Copyright 1988,1989 The Bitmap Brothers", `progstart`,
`shop_right`, "INSERT GAME DISK", "NuXENON2") to a 287,946-byte image at `$30000..$764CA`; the stub then `jmp $70000`
(image offset `0x40000`); image offset 0 is `bra $6C32` style entry.

Comparison (12/24/48-byte window search, 12-byte windows):
- Not identical, not a trivially patched copy. CDTV `X2` is a re-link/rebuild of the same engine (hunk exe, OS-friendly, absolute
  addresses differ, uses dos.library + cdtv.device instead of trackdisk).
- About 27-31 % of the disk image's bytes appear verbatim in CDTV X2. By disk-image region: `0x00000-0x1C000` 36-100 % match (engine code +
  tables/strings, e.g. 2327-byte and 1899-byte exact runs at disk `0xC5BD`/`0xE2BC`); `0x1C000-0x40000` (~144 KB) 0 % (graphics/sample
  data that on CDTV lives in PICDATA*/SOUNDS*/LEVEL* and CD audio); `0x40000+` partial (loader/trackloader).
- Match is high inside CDTV X2 at hunk offsets `0x30000-0x40000` and `0x50000-0x5C000` (game logic + big data tables), low at
  `0x00000-0x30000` (graphics/sample payload and BSS-like zeros) and `0x40000-0x50000` (CDTV-specific menus).
- LEVEL1..5 on CDTV share ~84 % of their bytes (32-byte windows) with disk 2, i.e. level data is the same data, repackaged
  as 0x601A images. INTRO only ~8 %, X2 ~10 %.
- Exclusive to CDTV: CD0: file loading, CD audio, language menu, music-select screen ("SELECT TITLES"), joypad ("SELECT JOYPAD MODE AND PRESS BUTTON A"),
  CDTV remote high-score entry text, the bundled `CDTV.TM`/`CDTVPREFS`/`RMTM`. Exclusive to disk: trackloader, "INSERT GAME DISK", tracker music.
- Recompilation implication: the CDTV `X2` and the disk `XENON-II` need separate lifted code; shared logic can be located by the
  exact runs above (a tool for relocation-masked matching is a next step, not done).

## 5. CD audio control (cdtv.device)

X2 opens `dos.library` (`$3A442`) and `cdtv.device` (`$3A4E4`). Two ioreqs: `$3A45E` (play), `$3A48E` (volume/attenuate).
All offsets are hunk-relative (add 36 for the file offset). Playback routines (hunk offsets):

| addr | role |
|------|------|
| `$3A4F6` | stop: AbortIO (-480) + WaitIO (-474) of ioreq `$3A45E`, `$3A4F2 = -1` |
| `$3A524` | `play_track(d0.w)`: stop; `$3A4F2 = d0`; ioreq cmd **43 (0x2B)**, `io_Offset = d0`, `io_Length = d0+1`, `io_Data = 0`; SendIO (-462) |
| `$3A55C` | VBlank/tick service (called from `$34CB8`): every 50 ticks CheckIO (-468); if finished, re-issue same track (looping) |
| `$3A5B4` | alias to stop |
| `$3A5BA` | volume full: ioreq `$3A48E`, cmd **56 (0x38)**, `io_Offset = $7FFF`, `io_Length = 2`; DoIO (-456) |
| `$3A5E6` | volume 0 (same with offset 0) (no callers found; may be unused or called via a pointer) |

Hypothesis: cmd 43 is `CD_PLAYTRACK` and cmd 56 is `CD_ATTENUATE` in this cdtv.device's numbering (offset=track, length=end-exclusive;
attenuate offset=volume 0..$7FFF, length=fade ticks). The cdtv.device command numbers should be checked against `CDTV.TM`.
`play_track` is always followed by `$3A5BA`.

## 6. Music cue -> track number hypotheses

Disc track numbers (2..13 = audio), passed straight to `play_track`. Tables in X2:
- `$3EE26` (6 bytes) = `02 03 05 02 03 05`: indexed by "music slot" 0..5 = TITLES, LEVEL 1..5 (EXIT = 6). The "SELECT TITLES" screen
  (`$3EB0C` strings, handler `$3DFE0`) lets the player choose which disc track each slot plays; defaults: **Titles=2, L1=3, L2=5, L3=2, L4=3, L5=5**
  (each change shows the 40-byte title record for `(track-2)`). The handler caches the playing track in `$3394C` to avoid restarts.
- In-game: `$A0A` (word) = level number 1..5; code at `$3A99C`, `$3A9A8`, `$3AEBC`, `$3AF74`, `$3B048` does
  `d0 = table[$3EE26 + level]; play_track(d0)`. Title screen (`$3C418`) plays `table[0]`.
- `$5047E` (24 bytes): `08 08 09 09 0A 0A 0B 0B 0C 0D 0D FF FF FF FF FF 0C 0C FF...`, indexed by word `$3DD76`, used at `$505BC-$5061A`
  (two sites, one per branch of an end-of-level / shop-like screen): tracks 8..13 (DESIRE, DOIN THE DO, S EXPRESS, NOTHING TO LOSE, BOOS BOOMING,
  OOCHY KOOCHY). `FF` = no music change. Probably per-level shop/sequence music; semantics of `$3DD76` unconfirmed.
- Tracks 4, 6, 7 are not referenced by any default cue (only reachable via the music-select screen).
- `INTRO`, `LEVEL1-5`, `PICCODE` contain no direct calls to `$3A524`/`$3A5BA`; all music control is in X2.

## 7. Language selection (EN/FR/DE/IT)

No system-locale/prefs use. A boot-time in-game menu in X2 (hunk `$3D62A-$3D748`): if word `$412EA` is negative (initialised to `$FFFF`) it shows
a choose-language screen (pointer table `$412EE` -> "ENGLISH", "FRANCAIS", "DEUTSCH", "ITALIANO", then a 5th entry "XENITEESE"; caption
"CHOOSE/CHOISISSEZ/WAHLE/SCEGLI"), the joypad moves `$F8`, button A confirms and stores the index in `$412EA` (0..3 = EN,FR,DE,IT).
Afterwards text is picked via per-language pointer tables indexed `lang*4`: `$4A5E` (menu string block, 0x6CA bytes copied to `$4EEF4`:
EN "SELECT SOMETHING, IT DOESN'T COST A THING... YET" at `$4EA1A`, FR `$4EB55`, DE `$4ECF3`, IT after), `$4A46`/`$4A4A`/`$4A42`/`$4A72` (intro/help pages loaded from `INTRO`,
whose text is laid out EN `0x973`, FR `0x25DB`, DE `0x477A`, IT `0x68A5`). A fifth value 4 triggers a character-translation table (`$41152`, `$3EB0B`) -
a hidden scrambled alphabet mode. SPEECH/SOUNDS files are not language-specific as far as seen (67 SPEECH files, no per-language duplication seen).
The disk version contains no language strings (`ENGLISH`/`FRANCAIS` absent from the unpacked image).

## 8. Boot chain

CDTV boot ROM -> `CDTV.TM`/`DEVS/SYSTEM-CONFIGURATION` -> `S/STARTUP-SEQUENCE`: `rmtm`, `CDTVPrefs`, `x2`. Names resolve to `RMTM;1`, `CDTVPREFS;1`, `X2;1`.
X2 then uses `CD0:` paths for all other files.

## 9. Open questions

1. Track 01 tail (LBA 2449..5855, ~6.9 MB) is outside the ISO volume and unreferenced by anything found. Candidates: 8-bit raw PCM streamed by raw
   `CD_READ`, leftover premastering data, or copy-protection filler. Search X2 for raw CD_READ (cmd 2) with LBA/byte offsets near 2449 found nothing but one coincidence at `$6AD9C`.
   Exported as `generated/lane-c/data-files-tail-raw.bin`.
2. Confirm cmd 43 / 56 against real cdtv.device numbering (disassemble `CDTV.TM` command dispatcher).
3. What exactly indexes `$3DD76` and when the `$5047E` track table is used (shop vs end sequence vs credits).
4. SPEECH00-66 and SOUNDS00-09 format (zero-led SPEECH tables; SOUNDS header entries) and which cue triggers which clip. SPEECH language dependence unverified.
5. 0x601A overlay loader: load addresses, relocation byte-table format and the entry convention (is the first word `bra.s` at +0x1C always?).
6. Relocation-masked diff of disk `XENON-II` image vs CDTV `X2` to separate "same source" routines from rewritten ones.
7. Disk 2 (non-DOS, dostype `\0\0X2`) layout, needed to compare LEVELn precisely.
8. Why CUE INDEX 01 values (137/138 frames) disagree with the 150-sector pregap actually present.
