# CDTV release: reference emulation (Amiberry) — what is available and what happened

Purpose: evidence for the plan to add the CDTV release (`X2`, `docs/re-notes/cdtv.md`) as a second recompilation
target. **Outcome: the CDTV disc boots to gameplay in the patched Amiberry** (section 4), so it can supply reference
screens, Paula audio, CD audio and 1 MB RAM dumps. For the disk build the patched Amiberry harness (`docs/re-notes/reference-compare.md`,
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

## 2. ROM availability and what each does

All ROMs are **user-supplied**; nothing is in the repository. Identified by CRC32 (they match Amiberry's table).

| ROM | CRC32 | Held | Result with the Xenon 2 disc (Kickstart 1.3 base, 1 MB chip + 512 KB fast RAM) |
|---|---|---|---|
| Kickstart 1.3 (34.5, 256 KB) | `c4f0f55f` | yes | the base ROM used with every extended ROM below |
| CDTV extended ROM v1.0 | `42baa124` | yes (also as the 128 KB halves, `791cb14b` and `accbbc2e`) | **boots to the game** |
| CDTV extended ROM v2.7 | `ceae68d2` | yes | does not boot: the machine falls back to the Kickstart "insert Workbench" screen. Same with the control disc, and with Kickstart 2.04 or 3.1 as the base, so this is not the disc |
| CDTV/A570 extended ROM v2.30 (also as the 128 KB halves `48e4d74f` and `8a54f362`) | `30b54232` | yes | **boots to the game** |
| v2.35 (v2.30 with the community CDTV 2.35 patch applied, CD1000 variant, patch files GPLv3) | `4ff7ab41` | yes (built locally from v2.30) | **boots to the game** |

Other ROMs seen in the set are not extended ROMs for this purpose (a Kickstart 1.3 copy, 4 KB and 8 KB parts that look like
controller/NVRAM ROMs); none was needed.

**Cloanto decryption is verified** for the ROMs that arrive in the Amiga Forever form: `AMIROMTYPE1` followed by the ROM
XOR-ed with `rom.key` repeated cyclically (key 1,544 bytes); the decrypted CRC32s match Amiberry's table. Decrypted copies
stay in git-ignored `generated/`.

Amiberry itself recognises v1.0, v2.7, A570 v2.30, v47.1 and CDTV-CR v3.32/v3.44 (`src/rommgr.cpp`); v2.35 is unknown to its
table but is detected as a generic extended ROM and maps and runs.

## 3. The harness for a CDTV run

`tools/compare/amiberry-harness/x2ref-cdtv.uae.in` (CDTV, OCS, 68000, 1 MB chip, **512 KB fast RAM**, PAL, cycle-exact,
`cdtvcd=true`, `cdtvram=true`) and `run-cdtv.sh OUTDIR QUIT_FRAME [SHOTS] [INPUT] [RAM]`:

```
KICK=<kickstart-1.3.rom> EXT=<cdtv-extended.rom> CD=<disc.cue> [X2REF_LOG=1] \
    tools/compare/amiberry-harness/run-cdtv.sh OUT 4800 "700,3200,4700" \
    tools/compare/amiberry-harness/inputs/cdtv-menus-to-level1.txt
```

`inputs/cdtv-menus-to-level1.txt` presses port-2 fire (`JOY2_FIRE`) for six frames every 150 frames from frame 700. The
original Redump `.cue` works as it is (file names with commas and parentheses are fine); a data-track-only cue and a cooked
2048-byte ISO of track 1 boot identically, so the audio tracks are not needed to boot.

Harness findings:

- **RAM dump is already 1 MB**: `X2REF_RAM` writes `chipmem_bank.allocated_size` bytes (verified: 1,048,576 bytes for the
  CDTV config). The earlier note's claim of a 512 KB limit was wrong.
- **CD audio is in `paula.wav`**: Amiberry mixes CD audio into the same sound buffer that the harness taps. Checked by
  matching band-energy envelopes of the recording against each CD track: a clear peak (0.59 against a median of 0.18) puts
  track 8 starting at about 20.7 s of the recording, when the game starts its music. So the WAV holds Paula **plus** CD audio
  mixed as Amiberry mixes them; there is no separate CD-only tap, and no way to split the two.
- `X2REF_WATCH` still defaults to the disk build's variable addresses; set it for the CDTV program's own variables.
- `X2REF_LOG=1` writes Amiberry's own log through `-o write_logfile=yes -o logfile_path=...` (no change to the user's
  Amiberry configuration). CDTV command tracing is a compile-time option in Amiberry (`CDTV_DEBUG_CMD`), not enabled.

## 4. What happened

| Run | Result |
|---|---|
| Control (another CDTV disc, a Psygnosis title), extended ROM v1.0, v2.30, v2.35 | boots to its intro, CPU in RAM; with v2.7 it falls back to the Kickstart screen |
| Xenon 2, any ROM above, **no fast RAM** (the first attempt) | CDTV logo, then black; the CPU idles in the extended ROM's exec idle loop (`stop #$2000`) |
| Xenon 2, v1.0 / v2.30 / v2.35, **512 KB fast RAM** | boots to the game: "SELECT JOYPAD MODE AND PRESS BUTTON", the option menu, "LOADING LEVEL 1", level 1 with the HUD |
| 2 MB chip RAM (ECS Agnus) without fast RAM | the CPU runs from RAM and the screens have colours; not followed through to gameplay |

**Why it idled without fast RAM.** The two boot discs have identical boot files (`CDTV.TM`, `CDTVPREFS`, `RMTM`
are byte-identical to the control disc's), and the control disc boots, so the disc is fine. The difference is memory: the
CDTV program needs two chip hunks totalling 811,612 bytes (`cdtv-slave.md` §2) in a machine with 1 MB of chip RAM, and the
operating system's own allocations (cdtv.device buffers, the DOS stack, `CDTVPrefs`, `rmtm`) also land in chip RAM when there is
no fast RAM. The most likely failure is that `LoadSeg` cannot find a free 372 KB chip block for the BSS hunk, so the start-up
script ends without a visible message. Adding fast RAM moves the operating system's allocations out of chip RAM. This is the same reason the WHDLoad slave asks for 811,008 bytes of base
memory plus separate expansion. This is a diagnosis by experiment (chip only: idles; fast RAM added: boots; chip enlarged: runs), not
a trace of the failing allocation.

The screenshots (under git-ignored `generated/`) show the joypad-mode menu, the 1/2-player menu with "MUSIC ON", "LOADING LEVEL 1" and level 1.
In level 1 the centre of the playfield shows dotted vertical streaks on black rather than the scenery of the disk version;
the cause (a real feature of the CDTV level, a loading state, or an emulation artefact) has not been checked, so the level
picture is **not yet usable as ground truth**.

## 5. What this gives the CDTV target

| Need | Available | Caveat |
|---|---|---|
| Boot-to-gameplay reference of the real disc | yes, any of v1.0, v2.30, v2.35 (at the same frame numbers the three ROMs show different pictures; the likely reason is the different operating-system boot time, not checked) | use one ROM for all recordings; v2.30 is the base of v2.35 and the A570 image, v1.0 is the original CDTV |
| Screenshots at chosen frames | yes | game time differs from frame number by the boot; align on game events (as the disk harness does with `WAITLOAD`) |
| RAM dumps | yes, 1 MB | CDTV program variables not yet mapped |
| Paula audio | yes, **with CD audio mixed in** | cannot separate; for CD music the event log of our runtime remains the check |
| PC / instruction traces | yes (`X2REF_PCTRACE`, `X2REF_PCLOG`, `X2REF_INSNLOG`) | not yet used on the CDTV program |
| Level picture as ground truth | not yet | see the streak note above |
| WHDLoad-slave route (A500 + slave, no CD music) | still possible as a second reference | needs the user's WHDLoad program |

Recommendation: record the CDTV reference with the Amiberry CDTV model (one ROM, fixed input script), and treat the
WHDLoad-slave route as a cross-check. Before relying on pictures, resolve the level-1 streak question.
