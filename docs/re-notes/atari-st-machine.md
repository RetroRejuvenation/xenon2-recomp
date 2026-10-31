# The Atari ST machine (`runtime/st`, phase S2)

The Atari ST target is in development (see `atari-st-requirements.md` for what the game needs and `atari-st-analysis.md`
for how it was analysed). This note describes the machine that runs it: what is modelled, what is simplified, and how it
is tested. Everything here is measured on the Kixx re-release (public identity: `.st`, 819,200 bytes, sha1
`323d6859635b6b82c86363db048b26e0f3df994c`); no game data is in the repository.

## What runs

`x2recomp --game st [--headless --frames N]` boots the **imported disk** (`<data dir>/assets-st`, or `--assets DIR`: `disk.bin` and
`manifest.txt` from the S1 importer; stale imports are redone from their recorded sources) and `--st-disk FILE.st` boots a plain `.st`
for development. Either way it runs the **real boot sector on an emulated WD1772**: there is no TOS and no pre-link. The boot sector loads at `$70000` (TOS's disk buffer) in supervisor
mode with the state TOS hands over (SR `$2300`, `_dskbufp`, `_sysbase`, `phystop`, `_memtop`, the memory-valid magics, the
video base at `phystop - $8000`, MFP `VR = $48`, keyboard ACIA control `$96`, 50 Hz sync, low resolution). From there the
game's own code does everything, through the controller. At real drive speed:

| Frames | What happens |
|---|---|
| 0-250 | The boot sector reads tracks 1-25 of side 0 (250 sectors, one single-sector command each, 20 ms apiece) to `$10000`, then jumps there: a two-instruction stub (`lea $10000,sp` / `jmp $2EE0C`) into the RNC ProPack method 1 depacker that came in with the block. |
| 250-355 | The depacker unpacks the front-end to `$40000` (a black screen). |
| 355-1250 | The front-end runs: palette, video base, Timer A (`TACR 1`, `TADR $4D`, 7,979 Hz, about 160 interrupts a frame), the Bitmap Brothers caption typed in, and, at the same time, the load of the resident engine from tracks 34-79 of side 0 (460 sectors, one command each) straight to `$400`. |
| 1250+ | The engine: the credits screen (frame 1300), then the Xenon 2 Megablast title over the star field (by frame 3000), with the music player writing the YM2149 about 120 times a frame. |

Over 3,000 frames: **710 sector reads in order, a VBL every frame, 173,467 MFP interrupts, 0 unmapped accesses, 0 CPU
exceptions.** (An early run died at the first instruction of the loaded block, executing packed data: the Kixx boot sector's
BPB says **one side** but the image is a double-sided 80 x 10 disk and side 1 holds the other half of the track data. The
geometry therefore comes from the file size when it is a standard size, and from the BPB only otherwise. The next
failure was a front-end that cleared VR bit 3 with `bclr` and found VR at 0 instead of TOS's `$48`: the hand-over state
matters.)

## Structure

- `runtime/bus_hooks.h` is the seam to the Musashi core. Musashi's callbacks (`m68k_read_memory_*`, `m68k_write_memory_*`,
  the disassembler reads, `x2_instruction_hook`, and a new `x2_int_ack`) are globals, one machine kind per process. A
  non-null `g_bus_hooks` hands them to the ST machine; the Amiga/CDTV path pays one predictable null-pointer test per
  access. `M68K_EMULATE_INT_ACK` is now a configured handler: with no hooks it returns Musashi's autovector as before, so
  every Amiga interrupt is acknowledged exactly as it was (`tools/recomp/opdump.c` and `tools/variants/stubrun.c`, which link
  the same core, define the same stub). The trace exception stays off (only the Image Works Copylock needs it, phase S7).
- `st/st_machine.*`: the bus (RAM, a 192 KB stub ROM holding an `RTE` that every interrupt vector starts out at and a
  `STOP` loop that CPU exceptions start out at, so a faulting program is a visible stub hit; the I/O page; an
  unmapped-access counter), the scheduler and the instrumentation. Time is the CPU cycle count (8,021,247 Hz: 313 lines of
  512 cycles). The CPU runs in slices that end at the next device event (an MFP timer underflow, an FDC or ACIA completion,
  a line boundary), so the MFP and FDC see the CPU at cycle accuracy to within one instruction (`now()` includes the cycles
  of the instruction in flight). Interrupts: VBL on level 4 (autovector) raised at line 0, the MFP on level 6 with the
  vector from the interrupt acknowledge cycle.
- `st/mfp.*`: the MC68901: four timers (delay mode, all prescalers; pulse-width and event-count modes are not modelled),
  the 16-channel interrupt controller (IER, IPR, ISR, IMR, VR base, software or automatic end of interrupt, priority
  masking), GPIP edge detection (AER). CPU and MFP clocks convert exactly (2,457,600 / 8,021,247, a remainder is carried),
  so 10,000 Timer A underflows take exactly 10,000 periods.
- `st/fdc.*`: the WD1772 and the ST's DMA chip at command level: restore, seek, step, step in/out, read sector (single and
  multiple), force interrupt; write commands fail with write protect (the game never writes). Real timing: 6/12/2/3 ms
  steps, a 15 ms verify, 20 ms a sector, about a second to "record not found", a motor that stays up for nine revolutions.
  The visible DMA address advances in 16-byte steps while a sector transfers (the Copylock timing check of the Image Works
  releases reads it, S7). INTRQ drives MFP GPIP bit 5 (active low); a status read clears it. `--st-fdc-speed N` makes the
  drive N times faster.
- `st/ikbd.*`: the keyboard ACIA (MC6850) and the IKBD's protocol: reset `$80 $01` answered with `$F0` after the self
  test, joystick event reporting (`$FE`/`$FF` plus a state byte), key make/break codes, interrogation and disable commands,
  time of day; one byte per 10,267 CPU cycles (7,812.5 baud), receive interrupts on GPIP bit 4.
- `st/shifter.*`: the Shifter's registers (video base, the odd-byte register file, palette masked to three bits per gun,
  sync, resolution) and a **frame-granular** low-resolution renderer (320 x 200, four interleaved planes), with a video
  address counter that advances 160 bytes per display line so the engine's page-flip wait (`$FF8207`) behaves.
- `st/ym2149.*`: the register file with read masks and a cycle-stamped write log (the sound itself is S5).
- `st/st_disk.*`: the logical disk (sides, tracks, sectors) and the boot-sector checksum; `st/st_run.*`: the command-line
  driver (`--game st`, `--assets`, `--st-disk`, `--st-fdc-speed`, `--st-io-summary`, `--st-fdc-log`, `--st-trace-tail N`, `--st-joy-at
  F:HEX`, plus the usual `--headless`, `--frames`, `--hash-at`, `--screenshot-at`, `--report-every`, `--dump-dir`). In a
  window the keys are the arrows and Z/Space/Ctrl for the joystick; there is no sound yet. Exit codes: 3 for a disk that
  cannot be read, 5 for one that does not boot, 6 when the program faults.

## Simplifications (and where they stop being acceptable)

- **No TOS.** After the boot sector the game makes no TRAP calls (`atari-st-requirements.md`); the TOS system variables
  are set at hand-over and not maintained (the engine overwrites vectors and uses `$400-$41F` itself).
- **Instruction timing is Musashi's nominal 68000 table.** The ST's 4-cycle bus rounding and the shifter/MMU wait states are
  not modelled; nothing in the front-end or title depends on it, and gameplay pacing is the engine's own VBL counting.
  Revisit if S6 finds a divergence from a reference.
- **No HBL and no Timer B.** The Kixx build does not use them (no mid-frame palette tricks seen); `hbl_unmasked_lines()`
  counts lines that began with the interrupt mask below 2, the exposure if a build did.
- **Video is frame-granular.** The display latch (base and palette) is taken at the first display line and the whole
  picture is drawn at the end of the frame. S3 makes it line-granular if any build needs mid-frame palette changes.
- **FDC:** command-level, not cycle-accurate MFM. A sector's data is copied to RAM when its transfer completes (a program
  reading RAM during the transfer sees the old bytes), the DMA FIFO is not modelled, and `read address`, `read track` and the
  write commands only return an error. A disk is a logical sector image: weak sectors, bad CRCs and long sectors (Image
  Works' Copylock) need the S1 importer's flux and `.stx` layer plus an S7 timing model.
- **A logical sector image only.** `.stx` and `.msa` are read by the importer (S1) and reach the machine as the same `disk.bin`; flux-level
  detail does not.
- **No sound output** (S5), no mouse, no blitter or STe, no MIDI, no printer, no RS-232.

## Tests

- `tests/st/st_machine_tests.cpp` (CTest `st_machine`, no game data, 112 checks): disk geometry rules including the
  single-sided BPB on a double-sided disk; MFP timer period, vector, priorities, IER-clears-IPR, automatic and software EOI,
  spurious acknowledge; FDC restore, seek timing, single and multi-sector reads against a pattern disk, the DMA address in
  flight, record not found, side select, force interrupt, the speed divisor; IKBD reset reply, byte cadence, joystick and key
  packets, disable; Shifter palette masks, rendering and the video counter; the `StMachine` booting hand-assembled boot
  sectors (a VBL and a Timer A handler counting in memory at the exact rates; an illegal instruction reaching the stub; an
  unmapped access; a bad checksum; a boot sector that reads a sector through the real FDC and MFP registers).
- `tests/st/st_kixx_tests.cpp` (CTest `st_kixx`, skipped without the dump, found by sha1 in `generated/st-dev` or
  `$X2_ST_DEV_DIR`; it also imports the dump with the S1 importer, loads the result the way `--game st` does and checks the same
  golden picture): the 710 sector reads in order with their DMA addresses, the first load ending at about frame 250, one
  VBL a frame, more than 100,000 MFP interrupts, no unmapped access, no exception, the set of I/O registers used, the YM
  being written, three golden pictures (frame 800 the caption being typed, 1300 the credits screen, 2999 the title) and a
  second identical run.
- The machine's I/O register summary (`--st-io-summary`) agrees with the analysis harness's (`tools/acq/stemu.c
  --io-summary`) for the boot sector: the same controller, DMA, MFP, ACIA and YM registers, with the same first reading and
  writing PCs.

## Not here yet (the hand-off to S3-S9)

S3 video (line-granular, then real timing), S4 input (joystick and keys through the IKBD work; the host mapping and fire,
escape and pause in play are not done), S5 sound (the YM register log is there), S6 the levels reached by input, S7 the Image
Works Copylock layers (trace exception, bad-CRC and long sectors), S8/S9 the release packaging and the modernisation options.
