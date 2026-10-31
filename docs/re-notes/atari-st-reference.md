# Atari ST reference: Hatari and EmuTOS, built locally

An independent reference for the planned Atari ST target (`.omc` plan, decision D2): Hatari 2.6.1 running EmuTOS 1.4,
both built or fetched into the git-ignored `generated/ref/hatari/`. Nothing is installed system-wide, nothing is
committed except the scripts and one small patch, and nothing from either project is shipped with x2recomp. It plays the
same role for the ST that the patched Amiberry (`docs/re-notes/reference-compare.md`) plays for the Amiga.

## 1. What was used

| Item | Version | Source | sha256 of the download | Licence |
|---|---|---|---|---|
| Hatari | 2.6.1 (source) | `https://github.com/hatari/hatari/archive/refs/tags/v2.6.1.tar.gz` | `75f09c8deddb511b8c1574b84c507743097db9abc6edbb078f942efa8677547e` | GPL-2.0-or-later |
| EmuTOS, 256 KB images | 1.4 | `https://sourceforge.net/projects/emutos/files/emutos/1.4/emutos-256k-1.4.zip` | `aadd90cf0c99925d3f2943149dd51ee4deb6015aefe22ade4a5e6c04fb6f2e9d` | GPL-2.0-or-later (with the exceptions in its own licence notes) |
| EmuTOS, 512 KB images | 1.4 | `https://sourceforge.net/projects/emutos/files/emutos/1.4/emutos-512k-1.4.zip` | `1ef7bd25f61bcfc66d19debc2f0ebb0d5e5ba811ccd7f0fddfcb105bb72d1663` | as above |

- **EmuTOS replaces the Atari ROM**, so no Atari TOS is needed or used. The default image is `etos256uk.img`
  (sha256 prefix `3bfdc561eb19`).
- **Use the UK image, not the US one.** `etos256us.img` starts the ST in 60 Hz NTSC video; the Kixx release is a PAL
  game and its timing is built around 50 Hz. With the US image a 1500-VBL run recorded 25.0 s of audio; with the UK image
  it records 29.94 s, which matches 1500 VBLs at 50.05 Hz.
- Hatari was built on macOS arm64 against the SDL2, libpng and capstone libraries already present on the machine. Hatari
  defaults to a universal binary, which fails when the libraries are single-architecture: `build.sh` passes the host
  architecture.
- **Nothing here is shipped.** Hatari and EmuTOS are local tools; this repository only holds the harness scripts and a
  patch to Hatari (GPL-2.0-or-later, as Hatari itself, noted in the patch's `x2ref.c`).

## 2. Build

```
tools/compare/hatari-harness/build.sh        # env: X2_GENERATED, HATARI_VERSION, EMUTOS_VERSION, SDL2_PREFIX, JOBS
```

It downloads the three archives (printing their sha256), unpacks them, applies `patches/0001-x2ref-harness.patch`
(skipped with `X2REF_NO_PATCH=1`), configures and builds Hatari, and prints `hatari --version`. Re-running it is safe: a
patch that is already applied is detected. The binary is `generated/ref/hatari/build/src/Hatari.app/Contents/MacOS/Hatari`
on macOS.

## 3. The harness

`tools/compare/hatari-harness/run.sh OUTDIR QUIT_VBL [SHOTS] [INPUT_SCRIPT] [RAM_VBLS]` runs Hatari once, headless
(`SDL_VIDEODRIVER=dummy`), from a private `HOME`, with a generated empty configuration file, and quits at `QUIT_VBL`.
Environment: `DISK` (required), `TOS`, `MEMSIZE` (KiB, default 512), `MACHINE` (`st`/`ste`), `JOY0`/`JOY1`
(`keys`/`none`; the scripted joystick), `WAV=1`, `FF=on|off`, `WATCH`, `AUTO`, `HATARI_BIN`, `X2_GENERATED`.

Stock Hatari has `--parse` debugger scripts, `--control-socket`/`--cmd-fifo` and trace options, but no way to take a
frame-numbered screenshot or to set the joystick at a given VBL deterministically. The patch (about 300 lines, one new
file `src/x2ref.c` plus a call from `Main_WaitOnVbl` and a setter in `joy.c`) adds an environment-driven hook, the same
idea as the Amiberry harness:

| Variable | Effect |
|---|---|
| `X2REF_DIR` | output folder; the hook is inert if unset |
| `X2REF_SHOTS` | VBLs at which a PNG of the finished frame is saved, `"500-12000/500"` syntax (`/n` = every n-th) |
| `X2REF_RAM` | VBLs at which the ST RAM (512 KB here) is dumped to `ram_<vbl>.bin` |
| `X2REF_INPUT` | script of `"<vbl> <event> <0\|1>"`: `JOY0_`/`JOY1_` + `UP DOWN LEFT RIGHT FIRE`, or `KEY<hex ST scancode>` |
| `X2REF_WATCH` | ST RAM word addresses (hex) logged every VBL |
| `X2REF_QUIT` | VBL at which to finish the WAV, close the logs and exit |
| `X2REF_WAV` | `1` = record the YM output to `ym.wav` from the first VBL |

`frames.log` gets one line per VBL: `<vbl> pc=<hex> sr=<hex> vbase=<hex> [<addr>=<word> ...]`. VBL numbers count from
emulator start (the first VBL is 1), so they include the EmuTOS boot before the disk's own code runs.

| Capability | Status |
|---|---|
| Frame-numbered screenshots | yes, patched; PNG of the screen as drawn at the end of the VBL |
| Scripted joystick and keyboard | yes, patched; per-VBL state, same line style as the Amiberry scripts |
| Audio capture | yes, stock `Sound_BeginRecording`; complete in fast-forward at 50 Hz (29.94 s for 1500 VBLs) |
| RAM dump at a VBL | yes, patched |
| Per-VBL PC/SR | yes, in `frames.log` |
| Instruction-level PC trace | **not patched in.** Stock Hatari has `--trace cpu_disasm --trace-file` (whole-run, very large) and the debugger's breakpoint/`trace` commands via `--parse`; a range-limited trace like `X2REF_PCTRACE` would be a further small patch |
| Speed | fast-forward runs at about 35 times real time (22,000 VBLs in 12 s), so 12,000 VBLs take under 10 s |
| Determinism | a fixed script gave the same VBL numbers for the same pictures; the hashes of two runs were not compared bit for bit here |

## 4. What it shows for the Kixx release

The Kixx budget release (`.st`, 819,200 bytes, sha1 `323d6859635b6b82c86363db048b26e0f3df994c`) is an unprotected, plain
disk that boots through its own loader. Under EmuTOS 1.4 (UK image) on an emulated ST with 512 KB:

- It **boots**; no TOS patching or compatibility option was needed. EmuTOS prints bus-error warnings for its RAM-size
  probe at `$400000` and above, which is normal.
- With fire pulsed on **joystick port 1** every 150 VBLs from VBL 2000
  (`inputs/kixx-title-to-level1.txt`, `JOY1=keys`), the sequence is: the front end loading (a blank screen for at least 600
  VBLs in a test run), the Xenon 2 Megablast title and the credits pages (Designed by, Music programming David Whittaker, and so on),
  then **level 1 by about VBL 6000**, then, with the script's fire pulses and no steering, the ship dies and the high-score table follows (seen at VBL 16,000
  in a run with the US image). A level-1 picture shows the playfield with the status bar (P1 score, energy bar, ships) as on the Amiga.
- **The game reads joystick port 1.** Fire on port 1 alone starts level 1; fire on port 0 alone leaves the credits
  running.
- Audio: a 240 s capture of the same run (12,000 VBLs) is 82% non-zero samples with a peak of 29,220 (of 32,767): music
  and effects are present.
- A first set of RAM dumps (`X2REF_RAM`) and the per-VBL `frames.log` come from the same run.

The ST Format issue 3 cover disk (`stf03.st`, sha1 `bb759b04f5a292fd9f4a51631cf9cbcd72a2bb33`) is a **GEM launcher**, not
a boot disk: it comes up on the EmuTOS desktop. Its Xenon 2 preview is `A:\XENON_II\X2.PRG` (240,896 bytes); with
`AUTO='A:\XENON_II\X2.PRG'` the preview starts and runs, and with the same fire script a high-score table is on screen at
VBL 4000 (play itself was not inspected frame by frame). A first attempt with `A:\X2.PRG` failed because the file is in the `XENON_II` folder.

Where the artefacts are (all git-ignored, under `generated/ref/hatari-runs/` in a checkout): `kixx-uk/` (shots every 500
VBLs, `ym.wav`, `ram_*.bin`, `frames.log`), `stformat3/` and the small test folders.

## 5. Limits and notes

- **STX (Pasti) dumps need capsimage.** Hatari reads `.st`, `.msa` and `.dim` natively; `.stx` and `.ipf` need the
  capsimage library, whose licence restricts commercial use. It was not built, because the Kixx and ST Format images are
  plain. The Image Works and Mirror Image releases (Copylock ST, `.stx` only) therefore cannot be run here until capsimage
  is added locally for reference use; it must not be redistributed.
- **EmuTOS is not Atari TOS.** The Kixx loader and engine ran without any workaround, and the Atari ST target plan found
  no TOS calls in the engine, so the difference should not matter for this game; it can for other software.
- **The VBL counter includes the OS boot.** When comparing with
  x2recomp (which will boot the real loader on an emulated disk controller), align on game events, not VBL numbers, as
  `docs/re-notes/reference-compare.md` does for the Amiga.
- **Screenshots are 832 x 552**, Hatari's scaled window including borders; the ST picture is 320 x 200 inside. Crop when
  comparing. A small drive-activity LED shows in the top-right corner of every frame.
- **No STe, no blitter** was needed: `MACHINE=st` is the default.
- **Windows and Linux:** not tried. The patch is plain C; `build.sh` has macOS-specific handling (the app bundle location,
  the architecture flag) and would need adjusting.

## 6. Using it for replay comparison

x2recomp replay scripts (`tests/replays/*.txt`) use the Amiga's frame timeline and pokes. For the ST target the plan is to
record ST replay scripts of the same shape and to convert them to harness input lines (`<vbl> JOY1_<dir> <0|1>`) with a
small tool when the `StMachine` boots Kixx; the conversion and the alignment (EmuTOS boot offset, disk-load timing) are
future work. Until then the harness provides: reference pictures and audio for the front end and level 1, RAM dumps of the
engine's variables at known events, and a second opinion on timing (the game's per-step VBL count is recorded in
`frames.log` through `WATCH`).
