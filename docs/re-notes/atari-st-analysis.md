# Atari ST Xenon 2 Megablast: loader, packer and engine comparison

Analysis only. Nothing here is built into the runtime, and no game bytes are committed: the notes below give sizes,
hashes and measurements; every extracted file lives in the git-ignored `generated/`. The disk images are the public
Atarimania dumps listed in `docs/re-notes/acquisitions-flux-acorn-st.md` section 3 (Image Works 1989, two Pasti `.stx`;
Mirror Image 1991, one `.stx`; Kixx 1993, one `.st`/`.stx`; the ST Format issue 3 cover disk `.msa`).

Tools (all in `tools/acq/`, synthetic tests in `test_st.py`):

| Tool | Purpose |
|---|---|
| `stemu.c` | minimal Atari ST around the project's Musashi core (trace exception on): WD1772 + DMA, MFP, video counter, ACIA, VBL, YM2149 write log. Runs a boot sector and dumps memory. |
| `sttrace.py` | static follower for Copylock-ST's trace-vector self-decryption (decodes the encrypted code without running it) |
| `stshape.py` | instruction-shape window matching and 48-byte byte-window overlap between two 68000 images |
| `ymrender.py` | YM2149 register-write log to WAV (listening aid) |
| `rnc.py`, `stx.py` | existing RNC ProPack and Pasti readers, reused |

## 1. Protection: Rob Northen Copylock ST (Image Works)

The Image Works disks carry Copylock ST, not a custom packer. Findings:

- Track 0 sector 6 is deliberately written with a **bad CRC** and holds the text "Rob Northen Comp" (512 bytes,
  sha1 prefix `6f4285e90259`). The loader reads it and expects the controller to report a CRC error.
- A **timing check** polls the DMA address while that sector transfers and requires it to run at least ~2% slower than
  a normal sector (the sector is written long on the original disk). Copies made without the long sector fail here.
- The **key** is derived from the first 24 bytes of the sector (six rounds of `add.l; rol.l #3`).
- The payload is decrypted with the **trace exception**: the loader installs a trace vector, enables the T bit, and the
  handler decrypts the *next* instruction in place (plain = raw XOR `swap(~previous raw longword)`; only the first 4
  bytes of an instruction are encrypted), then re-encrypts it after it ran. The first instruction after T is enabled
  runs raw. An illegal-instruction (`$4AFC`) handler toggles T, and a privilege-violation handler re-targets the trace
  vector, giving several layers. `sttrace.py` reverses a layer statically (bra/bcc/dbra/bsr following).
- Stage 1 is a 163,840-byte block loaded at `$40000` (sha1 prefix `9f521c79d3e6`); layer 1 was decoded
  (`$401AA` onward: vector table setup, checks, then the sector read).

`stemu` satisfies the check rather than bypassing it: `--bad-sector 0:6 --slow-sector 0:6:10` makes the harness report
the CRC error and a 10% slower transfer, and the loader's own code passes unmodified. (Earlier attempts to hook PCs
inside the decrypted region derailed the run because skipping an instruction leaves the next one raw.)

**Not finished:** layers 1 and 2 are now decoded (`atari-st-image-works.md`), but layer 3 was not followed to the
payload, so the Image Works engine itself was **not** decrypted. Mirror Image side 0 (GEMDOS/RNC programs) was
identified as a different packaging but not decoded.

## 2. Packer and layout: Kixx (unprotected)

The Kixx re-release has no Copylock. It boots a plain loader that reads an **RNC ProPack method 1** block (the packer
already supported by `tools/acq/rnc.py`) and starts the front-end program:

| Item | Where | Size | Notes |
|---|---|---|---|
| Front-end program | RNC block, run at `$40000` | 159,108 B unpacked (sha1 prefix `582db50d97b3`) | menus/loader, 36,678 instructions in a linear sweep |
| Resident engine | tracks 34-79, loaded at `$400` | 235,520 B (`056109480f83`) | the game proper, 65,501 instructions |
| Five level modules | side 1, five RNC blocks | 111,900 / 125,712 / 133,284 / 171,638 / 153,738 B unpacked | one per level group |
| Five 12,000-byte blocks | side 1 | 12,000 B each | not RNC; not decoded |

The `.st` is track-interleaved double-sided, so a contiguous logical stream crosses sides; per-side logical images
have to be built first (`--sides 2` in `stemu`). The Amiga's six level-module sectors from `tools/recomp/recomp.py`
(`[[11,229],[250,254],[514,275],[799,345],[1154,312],[1476,248]]`) have no ST equivalent: the ST loads by track.

## 3. What the ST engine is, against the Amiga and CDTV builds

Method: every instruction is reduced to mnemonic and operand shape (numbers, absolute-address sizes and branch
targets dropped); a window of 6 consecutive shapes (windows with fewer than 3 distinct shapes excluded) counts as
found if it occurs in the other image (`stshape.py`, the same method as `docs/re-notes/cdtv-code.md` section 5). A
linear sweep decodes data as instructions, which adds noise to both sides, so a control (a CDTV sound-sample file) is
measured the same way.

| ST resident engine (65,501 instr) | % of ST windows found there | % of that image's windows found in ST |
|---|---|---|
| CDTV `X2` code (104,364 instr) | 41.9 | **78.3** |
| Amiga game image at `$400` (59,714) | 28.8 | 27.9 |
| Amiga level modules (188,195) | 25.3 | 21.7 |
| CDTV level overlays (160,152) | 11.2 | 14.1 |
| control (12,959 instr of sample data) | 6.0 | 4.0 |

| ST front-end (36,678 instr) | found in | image found in front-end |
|---|---|---|
| CDTV `X2` code | 7.6 | 56.6 |
| Amiga game image | 7.5 | 5.7 |
| control | 4.5 | 2.5 |

Byte-level (48-byte windows): ST resident has 24.1% of its windows in the CDTV `X2` code, 9.4% in the Amiga game
image, 15.7% in the Amiga level modules, 0.0% in the CDTV overlays. The front-end shares essentially nothing
(0.00%).

Reading: the control floor is 4-6%. The ST resident engine is **clearly related** to both Amiga builds, and **much
closer to the CDTV code than to the Amiga disk build**: 78% of the CDTV `X2` code (by shape) appears in the ST
resident engine. That fits the CDTV port having been made from the same later code base as the ST, with the disk Amiga
build diverging more. It is a statistical resemblance, not a function-by-function proof. The front-end is a mixed
case: only 7-8% of its own windows occur in either Amiga-family image (control 4.5%), but 57% of the CDTV `X2` windows
occur in it. The two directions differ because the metric counts windows against a *set*: a small image full of
repeated idioms covers many windows of a large one. Read it as "the front-end contains some of the same routine
shapes (display and data-handling idioms) and a lot of ST-only loader code", not as a second copy of the engine; the
48-byte byte comparison finds no shared bytes.

**ST Format preview (cover disk 3, Oct 1989).** The preview `X2.PRG` (240,896 B, sha1 prefix `c1f4005e0e2b`) is a
**debug build**: it carries developer labels and strings. 75-88% of its instruction windows appear in the Kixx resident
engine, so the retail resident is an evolution of it. The labels are a source of function
names that the retail builds lack.

**Hardware.** The loaders and the engine run on a stock ST: no blitter, no STe DMA-audio, no extra RAM beyond 1 MB.
The STe DMA-audio hack listed in `other-versions-and-sources.md` is not part of any of these releases.

## 4. Music: YM2149 register stream

The engine plays music by writing the YM2149 registers directly (the $FF8800/$FF8802 pair) from a VBL-driven player.
Running the Kixx build in `stemu` with a 50 Hz VBL (90,000 instructions
per frame) captured **52,684 register writes covering 155.5 s**, rendered by `ymrender.py` to
`generated/platforms/wav/atari-st/kixx_ym_full.wav` (git-ignored; reproduce with
`stemu KIXX.st --sides 2 --boot-at 0x70000 --vbl 90000 --ym-log ym.log --max N` then `ymrender.py ym.log out.wav`).

The renderer implements three tone channels, the 17-bit noise generator, the mixer, amplitude and the 16 envelope
shapes at the chip's 2 MHz clock, with a logarithmic volume table. It does **not** reproduce timer-driven "digi-drum"
sample effects (volume-register writes at a high rate from an MFP timer), so drums that rely on them will sound thin.
Treat the WAV as a listening aid, not a reference. The earlier octave error (half period `16 x period` instead of
`8 x period`) is fixed and covered by a 440 Hz test.

## 5. Verdict and effort

- **Value now:** low for the existing targets. The ST disk build adds no data the Amiga and CDTV builds lack that we
  currently need (the music exists as ProTracker modules there). Its value is as a **second reference**: the
  debug-build preview's labels, the evidence of which code base the CDTV `X2` descends from, and a YM music stream.
- **A Kixx ST import** (RNC unpack plus the track layout above): about 1-2 days to reproduce the resident engine and
  level modules from the user's own disk, with tests.
- **A real ST recompilation target** (ST hardware layer: 4-plane interleaved Shifter display, YM2149, FDC loading
  replaced by an import step, no blitter or copper): roughly 3-6 weeks, similar in shape to the CDTV work, with more
  display work because the Amiga's custom chips are not involved. These are estimates, not measurements.
- **Image Works (Copylock ST):** follow the remaining layers (about 3-5 days) or accept the key-derivation approach used
  for the Amiga retail Copylock (a receipt from the user's own dump). Not needed if Kixx is the supported ST release.
- **Mirror Image:** about 1 day to decode its GEMDOS/RNC programs once Kixx is understood.

Open questions: the five 12,000-byte blocks; which 58% of the ST resident engine has no counterpart in the CDTV code
(likely ST-specific display and sound code); the instruction-shape numbers use a linear sweep (data noise), only the control bounds it.
