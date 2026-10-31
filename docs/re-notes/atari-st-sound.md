# Atari ST sound and input (phase S4)

How the ST target makes sound and takes input, how that was checked against a reference emulator, and what is still open.
No game data is described beyond what the program itself does; the Kixx release is cited by its SHA-1
(`323d6859635b6b82c86363db048b26e0f3df994c`).

## The YM2149 generator (`runtime/st/ym2149.*`)

The ST's sound chip is a three-channel square-wave and noise generator with an envelope. The front-end plays its music through the
chip's registers and plays digitised speech and effects by writing the volume register about 7,979 times a second from the MFP's
Timer A. The generator therefore has to be both a sound chip and a sample-accurate volume DAC. It models the chip at 250 kHz
(2 MHz master / 8, the rate of the chip's internal counters) and decimates to the host rate, as Hatari does at its highest accuracy.

- **Tone.** Each channel counts up to its 12-bit period (0 behaves as 1) and toggles.
- **Noise.** A 17-bit LFSR (taps 17 and 14) shifts every `2 x period` ticks (the noise counter runs at half rate; period 0 is 1).
- **Mixer** (register 7): a channel's output is high when (tone high or tone disabled) and (noise high or noise disabled), so a channel
  with both disabled outputs its amplitude steadily. That is the digi-sample technique.
- **Envelope** (registers 11-13): 96 positions (3 blocks of 32 levels). Block 0 plays once, then blocks 1 and 2 repeat. Each shape
  (0-15) is a table of "down / up / held low / held high" per block. A write to register 13 restarts it, even with the same value.
- **Levels.** Amplitude register bits 0-3 map to the chip's 5-bit steps (0, 1, 5, 7, ..., 31) and bit 4 selects the envelope. A step is
  converted to a level with the 32-entry single-channel table measured on a real ST (the table used by the sc68 player and by Hatari;
  it is a set of physical measurements, listed in `ym2149.cpp` with that attribution).
- **Mix and output.** The three channel levels are summed (a third each), averaged over the 5 or 6 ticks of a host sample (Bresenham
  step, so 48,000 Hz output is exact over a second), run through a mild one-pole low-pass, AC-coupled (a DC blocker with 8 fractional
  bits so a steady level decays to silence) and scaled by 5/8 to put the level where Hatari's output is. `StConfig::audio_filters`
  or `ym.set_sound(rate, false)` turns the filters off (the tests use that to get exact per-tick levels).
- **Sample-accurate writes.** Every register write is timestamped with the CPU cycle and converted to a tick
  (`tick_of(cycle) = cycle * 250000 / 8021247`), queued, and applied at that tick when the frame is rendered. A write is therefore
  heard within one tick (4 us) of when the CPU made it, which is what the 7,979 Hz sample player needs.
- **Determinism.** All of it is integer arithmetic; a frame's samples depend only on the machine state, so audio hashes are golden-testable.
  Rendering happens once per frame at the end of `StMachine::run_frame` (`audio()` returns that frame's mono int16 samples, about 959
  at 48 kHz); with the sound off (`audio_rate = 0`, the default) the chip only keeps its registers, so machine-only tests and the
  headless picture runs pay nothing.

## Checking it against Hatari

`tools/compare/st_audio_compare.py` compares our audio with the `ym.wav` Hatari writes (the harness, `tools/compare/hatari-harness`,
sets `WAV=1`). The two runs do not start together (Hatari boots EmuTOS first, and its floppy loads run faster than the real drive we
model), so the tool aligns them by envelope cross-correlation, optionally refined per window (`--local-lag`). Per window it reports

- `env`: Pearson correlation of the 20 ms RMS envelopes,
- `band`: correlation of the window-averaged 16-band log spectra (timbre),
- `fine`: the mean of the per-frame 16-band spectrum correlations.

Result for 12,000 frames of the Kixx disk (230 s), with the title music, the front-end's digi-sample player, and the first level:

| set | windows | env | band | fine |
|---|---|---|---|---|
| all active windows, local alignment | 102 | 0.965 (median) | 0.9968 | 0.874 |
| front-end digi-sample windows (6-26 s) | | 1.00 | 0.99 | 0.99 |

The waveform correlation of the digi-sample windows, after a fixed offset, is 0.955-0.96 (the residual is the output filter and the
resampling). Level ratio (our RMS over Hatari's) is 1.03 after the output scale was set. In game the percussion is noise based, and a
noise channel gives different waveforms from different LFSR phases (the two emulators' noise phases differ), so the waveform
correlation there is only 0.44-0.5, while the envelope and spectrum agree. The windows that fall in the floppy load phase differ because the two
runs' loads take different times; they are the low tail (10th percentile 0.64 for `env`).

## The keyboard path and the IKBD (`runtime/st/ikbd.*`)

The game reads one joystick, IKBD port 1 (the mouse port is port 0 and the game never reads it). The IKBD model follows Hatari's:

- reset (`$80 $01`) answers `$F1`, the HD6301 ROM version;
- "pause output" (`$13`) holds everything the IKBD would send (key codes and joystick packets) in its buffer, nothing is lost; the next
  valid command, or `$11`, releases it in order. Bytes that are not commands are ignored and do not release it;
- joystick packets are `$FE` (port 0) or `$FF` (port 1) followed by the state byte: bit 0 up, 1 down, 2 left, 3 right, 7 fire;
  interrogation (`$16`) answers `$FD` and both states even when event reporting is off;
- keys send the scancode, or code | `$80` on release.

### Input scripts (`runtime/st/st_input.*`)

`--input-script FILE` takes the project's frame-indexed format reduced to the ST (`FRAME fire [N]`, `FRAME joy DIRS [N]`,
`FRAME key HEX [N]` with an ST scancode, `every P until G`, `frames N`) and also the Hatari harness's lines (`VBL JOY1_FIRE 1`,
`VBL KEY39 0`) so the same script drives the reference emulator and this runtime. Amiga-only commands (`poke`) are refused.

### Host keys

The primary joystick bindings (the Amiga port 2 set) drive IKBD port 1; the secondary set drives port 0. Keys that are bound to
nothing go to the ST as scancodes through `st_scancode_for_amiga_raw` (P pauses, Esc aborts). Host commands (quit, fullscreen,
pause, volume) are shared with the Amiga build.

### Evidence that the chain works

Driving the runtime with the Hatari reference's script (a six-frame fire pulse every 150 frames from frame 2000), the game reaches
level 1 with `$CC6 = $27` (energy) and `$CCA = 3` (ships) at frame 2723 (`$E04 = 1` at 2202). Hatari reaches the same state at its VBL
3356: its EmuTOS boot costs about 739 VBLs and it accelerates floppy loads, which explains the offset. After the start, the internal timings
(energy steps +12, +15, +15, +12 frames; respawn +49) are identical in the two runs. The test `st_kixx` checks the frames 2202 and
2723, the ships, and golden audio hashes (title music at frames 800 and 1500, the idle title at 2999, and the played run).

### The Space key

In level 1 with the starting equipment, pressing Space (scancode `$39`) for 4, 40 or 30 frames at several moments leaves every picture
hash (every fifth frame to frame 3600) identical to the run without it, while P (`$19`) pauses (the pictures differ from that frame
on). So the key does not fire or do anything visible by itself; the special action noted in `atari-st-s0.md` probably needs an item
or stock the player has not got at the start. Not confirmed (it needs a shop purchase or a pick-up, which needs the later phases'
hooks to set up quickly).

## Tests

- `st_sound` (synthetic, 290 checks): the DAC table and the cycle-to-tick conversion; tone periods (1, 2, 7, 100, 4095, and 0 = 1);
  all sixteen volume steps; channel addition; the noise LFSR against an independent model; envelope shapes 0, 10, 13 and period
  stretching; a volume ramp written 31 ticks apart appears within a tick; late stamps; decimation counts at 48 kHz; the filters; sound off;
  determinism; IKBD pause/release, non-command bytes, `$F1` reset, interrogation; the script parser (both formats, relative frames,
  `every`, 15 kinds of bad input rejected with the line number), the player driving a machine; the scancode table.
- `st_kixx` (the real disk, exit 77 without it): audio sample count, the idle and played audio hashes, the level start frame and state.
- Sanitizers: all three ST tests pass under AddressSanitizer + UBSan; the only reports are the known shift warnings in Musashi's generated
  opcode tables.
