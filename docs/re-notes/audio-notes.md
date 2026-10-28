# Audio: the game's sound driver and how the runtime's Paula model serves it

Sources:

- the disassembly of the resident image (`generated/lane-b/x2-rt.dis`);
- the register-write logs (`--dump-dir .../regs`);
- the driver-call log (`--audio-cues`) and the Paula statistics printed with `--audio-out`.

These come from the scripted run in `docs/BUILDING.md` (`--fire 460:5 --fire 950:5 --fire 1200:5`), plus
fire held at frames 1400-1499 and 1700-1849 in level 1.

Confidence: H = read in code and seen at run time; M = seen at run time, meaning inferred; L = guess.

## 1. Driver entry points (resident, `$19F9E-$1B600`)

| Entry | Core | What it does | Conf. |
|---|---|---|---|
| `$19F9E` | `$19FFC` | **music_init**, `d0` = tune index (10-byte records at `$1A828`). Calls `music_stop`, builds the sample tables (`$1B1B6`), resets 4 channel records at `$1A596` (`$30` bytes each), and sets the "playing" flag `$5F0(a3)`. It also does **`bset #1,$BFE001`**, which turns the power LED **and the audio low-pass filter off**. | H |
| `$19FAC` | `$1A154` | **music_tick**, called from the VBL via `($2090)` every frame. The tempo byte `$1A584` is added to an accumulator, and a carry skips the tick. | H |
| `$19FBA` | `$1A09A` | **music_stop**: DMACON `$000F`, ADKCON `$00FF`, AUD0-3VOL = 0. | H |
| `$19FC8` | `$1A112` | Re-enables audio DMA (`$8200\|$F`) and clears ADKCON `$00FF`. No caller seen in the scripted run. | M |
| `$19FD6` | `$1B26A` | **sound_command**, called from the VBL via `($2094)`: `$414` is passed as `d0\|$200` and `$418` as `d0\|$100`. | H |
| `$19FE4` | `$1A0F4` | Sets a byte in each of the four channel records (`$1744/$1768/$178C/$17B0(a3)` = 1). Called from `$3C6E`, `$6D22` (a level event, seen at frame 1652) and `$8FFA` (level load). Probably a music fade or release. | L |
| `$19FF2` | `$1A13C` | Sets `$5EB/$5EC(a3)` = 3. No caller seen. | L |

The shop/ending module (disc-2 slot 6) carries its own copy of the driver. It installs `$2090=$6477C` and
`$2094=$647A6` and restores them at `$55036`. The cue log traps `$647A6` only while `$2094` points to it.

## 2. Music start/stop call sites (for the CDTV cue mapping)

Every `music_init` call passes **`d0 = 0`**. The disk version has **one tune**. Title and in-game music use
the same ten instruments (table in section 4) and differ only in the tempo byte, which is written right after
the call:

| Call site | Context | Tempo `$1A584` | Frame in scripted run | Conf. |
|---|---|---|---|---|
| `$8230` | title sequence start (`$8220`) | `$3F` | 2, line 143. It is audible from frame 3. | H |
| `$7896` | after "GET READY PLAYER n" (`$842A` prints `$8836`), just before `main_loop` `$78A4`; skipped if `$9FC` (music on/off) = 0 | `$30` | 1271, line 273. The level-1 music is audible from frame 1272. | H |
| `$7C50` | in-game restart path, `$9FC`-gated (not reached in this run) | `$30` | — | M |
| `$7CDA` | after `$6862`/`$6ECA`, `$9FC`-gated (not reached) | `$30` | — | M |
| `$7D8C` | after `$88B6` (palette), `$9FC`-gated (not reached) | `$30` | — | M |

Stops:

- `$8672` and `$8680`: `music_stop` on the two exits from the title loop. `$8680` is reached from `tst.b $A01 / bne` at `$82A2` (fire). The call seen at frame 460, the frame of the first fire press, came from `$8672`. **The SELECT menu and the disk load are silent apart from sound effects.** The code does this, so it is not an emulation gap.
- `$9000`: the level load (`$8FF2`) calls `$19FE4` and then `music_stop`.
- `$8DAE` (`not.b $9FC`): probably the music on/off option (M). `$9FC` gates every in-game `music_init`.

The CDTV version replaces this with CD tracks (`docs/re-notes/cdtv.md` §6): it uses table `$3EE26`
(TITLES, LEVEL 1-5) and plays them in the same places. In the disk version, the cue points to map are the
title start (`$8230`) and each in-game start (`$7896`, `$7C50`, `$7CDA`, `$7D8C`). The level number at those
points is in the game state (`$E04` is compared against `$8F98` by `$917E`). It is not in `d0`.

## 3. Sound effects

Sound effects are queued by writing a command word to `$414`, which is the main queue (`d0|$200`), or
`$418`, the second queue (`d0|$100`). The VBL dispatches them before the music tick. The writers are the
immediate `move.w #n,$414/$418` instructions listed below. Commands seen at run time:

| Command (as logged) | Source | When | Channel | Conf. |
|---|---|---|---|---|
| `$020D` | `$8DA2` | menu selection (frame 951) | AUD2 | H (source), M (meaning) |
| `$0289` | `$6160` (only when `$414` is free) | player firing: every 24 frames while fire is held | AUD2 | M |
| `$0185` | `$3202` (`move.w d0,$418`) | level event or enemy (frames 1549-1591, 2078-2120) | AUD1 | M |
| `$020A` + `$010A` | `$5D12`/`$5D18` | both queues at once. Probably the player's death or explosion (frames 1603 and 2132, with no steering) | AUD2 + AUD1 | L |

Other immediate writers, not reached in the scripted run: `$414`:

- `#1` at `$3BD0`
- `#$15` at `$3E72`
- `#6` at `$3F82`
- `#$80` at `$40E0`
- `#$83` at `$428C` (also `$418`)
- `#$82` at `$42D2`
- `#5` at `$4730`
- `d0` at `$47CA`
- `#7` at `$489A`
- `#2` at `$4932`
- `#9` at `$5714`
- `#$10` at `$63F6` and `$7996`
- `#$C` at `$8D60` and `$8D80`

Sound effects take a music channel (AUD1 or AUD2, both on the right) for their duration.

How the effect driver writes Paula (`$1B494-$1B516`; relevant to emulation):

1. Clear the channel's DMA bit, then set AUDxLEN=1 and AUDxPER=`$7C`, so the channel stops quickly.
2. Busy-wait about 12 scanlines.
3. Write LEN/LC/PER and enable DMA.
4. About 12 lines later, write LC=`$2ABA0`, LEN=`$20`: a silent loop picked up at the end of the one-shot.

The music tick uses the same one-shot-then-silent-loop pattern. So the driver depends on these behaviours,
which the runtime implements:

- DMA stops at the end of the current sample;
- LC/LEN are latched only at DMA start and at end of buffer;
- the period is re-read at each sample boundary.

## 4. Samples and Paula usage

Distinct sample start addresses (LC latched at a DMA start), from the Paula statistics:

- **Title music (frames 0-459), 10 instruments:** `$1CCA6 $1D966 $1DE88 $1EA56 $20150 $20636 $23E88 $27B04 $2817C $29602`
- **Level 1 music:** the same 10. Level 1 loads its module at `$54E00` and does not replace them.
- **Sound effects seen:**
  - `$2ABE0`: menu select
  - `$32F38`: firing
  - `$30458`, `$2B6B0` and `$22AD0`: other in-level effects
  - `$2ABA0`: the silent loop buffer, also used as a start address
- **Total in the run:** 16 addresses.

Other measured facts:

- **Periods:**
  - AUD0: 179-455
  - AUD1 and AUD2: 124-1270 (sound effects go down to 124)
  - AUD3: 183-221
  - None is below 124, so the minimum-period clamp never triggers.
- **Volumes:** the driver writes `$0340` and `$03A0`. Paula uses bits 0-6, so these mean volume 64 and 32. The left pair (AUD0 and AUD3) is quieter than the right, so the music's RMS is about 2.5x higher on the right. Whether that is the real mix is not confirmed; see Gaps.
- **ADKCON:** only ever written as `$00FF` (clear all modulation bits). **Attach modulation (ATVOL/ATPER) is unused**, so it is not implemented. The runtime counts modulation-set writes; there are 0.
- **Audio interrupts:**
  - INTENA is only ever `$C02A` (INTEN, VERTB, PORTS, DSKBLK), so AUD0-3 are never enabled.
  - Nothing reads INTREQR audio bits. The only INTREQR read is `btst #5,$DFF01F` (VERTB) in the VBL.
  - Paula still sets INTREQ bits 7-10 at each DMA start and buffer reload, as the hardware does. They stay set, because nobody acknowledges them, so `intreq` reads `$07xx` in the reports.
- **Filter:** `music_init` sets CIA-A PRA bit 1, so **the LED filter is off** for the whole game. With the A500 model, only the fixed ~4.9 kHz RC and the 5 Hz DC high-pass apply.
- **Manual (non-DMA) AUDxDAT playback:** unused, because no AUDxDAT writes are logged.

## 5. Measured output (scripted run, A500 model, 48 kHz)

All figures are from `python3 -I` analyses of `--audio-out` WAVs, using numpy in the scratch area. numpy is
not a project dependency.

- **Title, frames 3-455:**
  - non-silent: RMS L 2000-2900, R 5300-7100 (int16)
  - spectrum: 59% of the energy at 100-500 Hz, 21% at 0.5-2 kHz, 4% at 2-5 kHz, under 0.5% above 5 kHz
  - L/R correlation about 0, which is expected with hard-panned, independent channels
- **SELECT menu, frames 470-940:** digital silence (peak 1 LSB), apart from the selection effect at frame 951.
- **Level 1 music:** starts at frame 1272 with a similar spectrum.
- **Firing:** the same run with and without fire held from frame 1400 is bit-identical up to frame 1402.0, which is when the first `$0289` command is serviced. After that the outputs differ by 1300-5000 RMS while fire is held, so the effect is audible.
- **Peak and clipping:** peak 0.78 of int16 full scale, with 0 clipped samples (see the output gain in `runtime/audio/paula.h`).

## 6. Gaps and open questions

- **No reference capture yet.** The levels, the L/R balance and the exact sound have not been compared with a real A500 or with WinUAE (Phase 4). Nobody has listened to the output; everything above is measurement.
- **Phase-6 module driver:** the module's own music driver has not been exercised. The shop/ending has not been reached by a scripted run.
- **Command meanings:** the `$0185` and `$020A/$010A` meanings are guesses.
- **`$19FE4` and `$19FF2`:** their semantics are unconfirmed.
- **Interrupt timing:** audio interrupts and DMA starts are modelled without the few colour clocks of DMA-slot latency. Line-end batching does not change their timing: the runtime advances Paula exactly to each register write.
