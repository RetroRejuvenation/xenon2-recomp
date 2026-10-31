# CDTV release: reference emulation (Amiberry) — what is available and what happened

Purpose: evidence for the plan to add the CDTV release (`X2`, `docs/re-notes/cdtv.md`) as a second recompilation
target. For the disk build the patched Amiberry harness (`docs/re-notes/reference-compare.md`,
`tools/compare/amiberry-harness/`) supplied ground truth: screenshots, audio and RAM dumps at chosen frames. This note
records whether the same can be done for the CDTV disc, and what a CDTV run needs. No ROM or game bytes appear here;
ROMs are identified by name and CRC32 only, and every ROM is **user-supplied**.

## 1. What the Amiberry we built supports

The patched Amiberry (same tree as the disk-build harness) has the CDTV model compiled in: `cdtv.cpp`, the block-device
layer (`blkdev.cpp`, `blkdev_cdimage.cpp`) and the configuration keys `chipset_compatible=CDTV`, `cdtvcd=true`,
`cdtvram=true`, `cdimage0=<.cue/.iso>`, `kickstart_ext_rom_file=`. Its ROM table (`src/rommgr.cpp`) lists the CDTV
extended ROMs it recognises:

| ROM | CRC32 | Size |
|---|---|---|
| CDTV extended ROM v1.0 | `42baa124` | 256 KB (also as two 128 KB halves, parts 252606-01 / 252607-01) |
| CDTV extended ROM v2.7 | `ceae68d2` | 256 KB |
| CDTV/A570 extended ROM v2.30 | `30b54232` | 256 KB (391298-01) |
| CDTV extended ROM v47.1 (Kickstart 3.2 family) | `ab6274e7` | 256 KB |
| CDTV-CR extended ROM v3.32 / v3.44 | `581a85cf` / `0b7bd64f` | 256 KB |

The base ROM for a first-generation CDTV is Kickstart 1.3 (34.5, A500/A1000/A2000, CRC32 `c4f0f55f`, 256 KB).

## 2. ROM availability

Which of these the project's maintainer holds, as user-supplied material (nothing is in the repository):

| ROM | Available | Notes |
|---|---|---|
| Kickstart 1.3 (34.5), CRC32 `c4f0f55f`, 256 KB | yes | the disk-build harness already uses it (stored as a 512 KB image that is the 256 KB ROM twice; the first half has the CRC above) |
| CDTV extended ROM v1.0, CRC32 `42baa124` | **yes** | held in the Cloanto "Amiga Forever" encrypted form (`AMIROMTYPE1` header, 262,155 bytes); decrypts to the 262,144-byte ROM with `rom.key` |
| CDTV extended ROM v2.7 (`ceae68d2`), A570 v2.30 (`30b54232`), CDTV 3.2-family, CDTV-CR | **no** | not in the ROM set held |

**Cloanto decryption is verified**: the encrypted ROM is `AMIROMTYPE1` followed by the ROM XOR-ed with `rom.key`
repeated cyclically (key 1,544 bytes). With the user's `rom.key` every ROM in the set decrypts to a plausible ROM whose
CRC32 matches Amiberry's table (the 1.0 extended ROM gives `42baa124`; the Kickstart 3.1 A500 ROM gives `fc24ae0d`).
The two key files in the set differ (a 1997 key of 2,069 bytes and the `rom.key` of 1,544 bytes); the 1,544-byte one is
the one that works for the set. A reference run therefore needs, from the user: `Kickstart 1.3`, the Amiga Forever
`rom.key` and the encrypted CDTV extended ROM — or the decrypted ROMs. Decrypted copies must stay in git-ignored
`generated/` and are never committed.

## 3. The harness for a CDTV run

Added in this change: `tools/compare/amiberry-harness/x2ref-cdtv.uae.in` (CDTV, OCS, 68000, 1 MB chip, PAL,
cycle-exact, `cdtvcd=true`, `cdtvram=true`) and `run-cdtv.sh OUTDIR QUIT_FRAME [SHOTS] [INPUT] [RAM]` (env `KICK`, `EXT`,
`CD` = the `.cue`; optional `X2REF_REF` to point at a different `generated/ref`). Everything else — `X2REF_SHOTS`,
`X2REF_PCTRACE`, `X2REF_WAV`, the input script — is the existing harness and works unchanged.

Harness limits for CDTV, found by reading the hook source:

- `X2REF_RAM` dumps **512 KB** of chip RAM; a CDTV has 1 MB, so the dump function needs the size taken from the machine.
- `X2REF_WATCH` defaults to the disk build's variable addresses (`$404`, `$41C`, ...): meaningless for the CDTV program.
- CD audio is mixed by Amiberry's CD player, not Paula: `X2REF_WAV=1` taps the Paula mix only (`paula.wav`), so CD-audio
  ground truth would need a second tap on the CD mix.
- The CD image must be reachable by a path without special characters in the *file* names of the `.cue`: the Redump
  track files contain commas and parentheses; the run used symlinks with plain names and an edited `.cue` (git-ignored).

## 4. What happened when the disc was booted

Amiberry with Kickstart 1.3 + CDTV extended ROM v1.0, the staged Redump image (and variants), headless:

| Run | Result |
|---|---|
| Control: another CDTV disc (a Psygnosis title) mounted the same way | boots; the publisher's intro screen is on screen by frame ~1000, the CPU runs from RAM. The configuration and the ROMs are good |
| Xenon 2 `.cue` with all 13 tracks | CDTV boot logo at frame 300, then a black screen to frame 3000 |
| `.cue` with the data track only | same |
| cooked ISO of the data track (6,005 sectors) | same |
| with `cdtvram=true`; with joystick fire, port 2 fire and RETURN pressed at several frames | same |
| non-cycle-exact CPU, immediate blits | same |

PC trace around frames 300-900: the CPU is parked in the extended ROM's idle `stop #$2000` (exec's idle loop at
`$F00F94` in this ROM) and wakes only for interrupts; no game code runs and nothing is drawn. So the operating system
is up and idle, i.e. no task is running: the boot sequence did not start `X2`, or it started and blocked.

**This is unresolved.** Hypotheses, none tested:

1. The disc's boot environment (`S/STARTUP-SEQUENCE`: `rmtm`, `CDTVPrefs`, `x2`) targets a later cdtv.device than ROM
   v1.0 (the disc carries its own `CDTV.TM`, "cdtv.device 35.2 (6.2.91)"); a v2.07-era ROM may be required. v2.7 /
   A570 v2.30 are not available here.
2. Amiberry's CDTV emulation lacks something `rmtm` or `CDTVPrefs` needs (bookmark.device, playerprefs.library).
3. A read mode or address outside the ISO volume that the Amiberry CD layer rejects.

To tell these apart: Amiberry's log (it writes only when `write_logfile` is on in the user's Amiberry options, which the
harness does not set), or a debugger breakpoint on the DOS startup. Both are small jobs.

## 5. What a reference for the CDTV build can be, then

| Route | Needs | Gives | Cost |
|---|---|---|---|
| Boot the disc in a CDTV model (this note) | CDTV ROM that works with the disc (v2.07-class, not held) or a fix for the Amiberry/ROM-1.0 hang | everything incl. CD audio and speech | unknown until the hang is explained |
| **Run the WHDLoad CDTV slave on an A500 + Kickstart 1.3 in the existing harness** | the user's WHDLoad program (the slave and the 96 data files are staged), a mounted hard-drive directory | screenshots, Paula audio, RAM dumps, traces; **no CD music** (the slave removes it), speech and effects work | 1-2 days; uses the harness unchanged except mounting a directory |
| Our own runtime as the first CDTV player, checked against the WHDLoad run frame by frame | the above | frame comparison for the picture; CD music checked by event log only | as for the disk build |

Recommendation: use the WHDLoad slave route for picture and Paula reference, and keep the CDTV-model boot as an
investigation for the CD-audio/speech details.
