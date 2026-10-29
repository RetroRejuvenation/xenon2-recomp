# Frame timing: VBL, logic rate and the CIA-A CRA bit 7 test

Question: the game reads CIA-A CRA bit 7 at `$768A` to choose its frame pacing. Which value does a real Amiga
return, and how fast does the game logic actually run?

**Answer: on real hardware bit 7 always reads 0. So the game sets `$410 = 5`, and one logic step (with one new
picture) happens every 3 VBLs: 16.67 per second on PAL.** Phase 3a defaulted to bit 7 = 1, which gave 25 Hz
logic and music slowed to 5/6 speed. That default has been fixed (`runtime/cia.cpp`, `runtime/machine.cpp`).

## 1. What `$768A` tests (init, `$763C`)

```
0000767e  31fc00040410   move.w #$4, $410.w        ; assume "bit 7 set" case
00007684  50f90001a591   st.b   $1a591.l           ; music flag = $FF
0000768a  0839000700bfee01 btst.b #$7, $bfee01.l   ; CIA-A CRA ($BFEE01) bit 7
00007692  660c           bne.b  $76a0
00007694  31fc00050410   move.w #$5, $410.w        ; bit 7 clear
0000769a  51f90001a591   sf.b   $1a591.l           ; music flag = 0
```

These are the only references to `$1A591` and the only read of `$BFEE01`. The other two `$BFEE01`
accesses, at `$FCC`/`$FDA`, are the keyboard SPMODE handshake (`bset`/`bclr #6`).

## 2. How `$410` paces the game

The VBL handler `$20C2` (L3, VERTB) adds **2** to `$41C` on every VBL, with two `addq.w #1,$41C.w`, at
`$20CE` and `$20D6`. The handler also does these things:

- it samples JOY1DAT and the CIA-A PRA fire bit into `$A00`/`$A01`;
- it dispatches queued sound commands `$414`/`$418`;
- it calls the music tick through `$2090` (= `$19FAC`).

It does not touch the display.

The main loop `$78A4` calls `wait_frame_swap_copper_alt` `$202E` once per iteration:

```
00002034  3038041c       move.w $41c.w, d0
00002038  90780404       sub.w  $404.w, d0
0000203c  b0780410       cmp.w  $410.w, d0
00002040  65f2           bcs.b  $2034              ; wait while ($41C - $404) < $410
00002042  31f8041c0404   move.w $41c.w, $404.w
0000204e  0839000000dff005 btst.b #$0, $dff005.l   ; wait for V8 ...
00002058  0c100002       cmpi.b #$2, (a0)          ; ... and VHPOSR line >= $102 (258)
0000205e  ...                                     ; swap $406/$40A, toggle $700E between $1014/$1038
0000207e  23c000dff080   move.l d0, $dff080.l      ; COP1LC
00002084  33c000dff088   move.w d0, $dff088.l      ; COPJMP1
```

`$41C` moves in steps of 2, so the difference since the last swap goes 2, 4, 6, ...

| `$410` | swap when the difference reaches | VBLs per logic step | PAL (50 Hz) | NTSC (60 Hz) |
|---|---|---|---|---|
| 4 (bit 7 = 1) | 4 | 2 | 25 Hz | 30 Hz |
| 5 (bit 7 = 0) | 6 | 3 | **16.67 Hz** | 20 Hz |

These are maximum rates. If a step takes longer than its VBL budget, the swap slips by whole VBLs (slowdown
under load). Which steps slip depends on the CPU's and the blitter's exact speed, and every logic step draws
random numbers, so the step lengths decide the game's random sequence (§8).

## 3. Which branch was meant for which video standard

The music tick `$1A154` tests the flag that the init code set next to `$410`. The flag is `$5F3(a3)`, with
a3 = `$19F9E`, so it is `$1A591`:

```
0001a15e  4a2b05f3       tst.b  $5f3(a3)        ; flag ($1A591)
0001a162  670e           beq.b  $1a172          ; 0: play the tick
0001a164  532b05f2       subq.b #$1, $5f2(a3)
0001a168  6608           bne.b  $1a172
0001a16a  177c000605f2   move.b #$6, $5f2(a3)
0001a170  60e0           bra.b  $1a152          ; -> rts: every 6th VBL tick is skipped
```

When the flag is set, the music plays 5 of every 6 VBL ticks. That is the usual way to make a 60 Hz VBL
deliver a 50 Hz tempo. So the program treats bit 7 = 1 as "60 Hz machine" and pairs it with `$410 = 4`, and
treats bit 7 = 0 as "50 Hz": `$410 = 5`, music at full VBL rate. This matches the `4 NTSC / 5 PAL` note in
`memory-map.md`.

This is the opposite of the 6526's TODIN convention, where 1 means 50 Hz. Either way, the Amiga's 8520 has
no TODIN bit.

## 4. What a real Amiga reads

The Amiga Hardware Reference Manual, Appendix F (8520 Complex Interface Adapters, Control Registers), gives
the CRA bitmap with bit 7 as `UNUSED`, and states:

> All unused register bits are unaffected by a write and forced to 0 on a read.

(Amiga Developer CD 2.1, `Hardware_Manual_guide/node0146`; `node0145` lists bit 7 as `UNUSED`.)

The 8520 replaced the 6526's BCD TOD with a 24-bit binary counter and dropped TODIN. So `btst #7,$BFEE01`
returns 0 on every Amiga, PAL or NTSC, whatever Kickstart, a cracker intro or the trainer wrote to CRA.
Whether Kickstart writes the bit does not matter.

Consequences:

- On real hardware the game always takes the `$410 = 5` / music-flag-0 branch. The `$410 = 4` / tick-skip
  branch is unreachable on an Amiga. It looks like code written for a 6526-style TODIN bit.
- On PAL, the logic and the picture update at 16.67 Hz, and the music plays at the full 50 ticks/s.
- On an NTSC Amiga the same branch runs: logic at 20 Hz and music 20% fast. This is not something the runtime
  needs, because it models PAL.

## 5. Runtime change and measurement

- `Cia::write(CRA)` now stores `v & 0x7F`, so bit 7 reads 0 as on the 8520.
- The Machine constructor no longer writes `CRA = $80`.

Measured with the scripted run (`--fire` every 50 frames from 400), counting entries to `$78CE` (the main
loop's back edge):

| Model | Main-loop iterations per 100 VBLs in level 1 |
|---|---|
| CRA bit 7 = 0 (8520, now) | 33-34 (16.67 Hz), measured over frames 900-1600 |
| CRA bit 7 = 1 (old default) | 50 (25 Hz), from the code in section 2; not re-measured |

This was measured with the Phase 3 model, which had no DMA contention and a zero-time blitter, so steps never
overran their 3-VBL budget. On the reference, heavy steps take 4-5 VBLs; the accurate timing model of §8
reproduces that.

## 6. How the picture relates to the VBL

- **Scrolling:** there is no hardware scroll. BPLCON1 is written once (0) at init. The playfield is redrawn
  by the blitter into the back buffer (`$406`). The visible buffer changes only when `$202E` swaps
  `$406`/`$40A` and reloads COP1LC + COPJMP1. The copper lists `$1014`/`$1038` contain only the four BPLxPT
  pairs.
- **Swap position:** the swap waits for line 258 (`$102`) before restarting the copper.
  - DIWSTOP is `$02C1`, so the window is lines `$3A`-`$101` (58-257, 200 lines; the old note `$2CC1` in
    `memory-map.md` was wrong).
  - So the new BPLxPT values are loaded just after the window closes, and are reloaded at line 0. There is
    no tearing.
- **Sprites:** there are no hardware sprites. SPREN stays off, and SPRxDATA are cleared once. All objects
  and the HUD are blitter bobs in the double-buffered screens.
- **Palette:** writes come from the main loop (`$6372` fades, `$4926` pickup flash), so they also land at the
  logic rate. They can land mid-frame, at any beam position; the renderer applies them at the colour clock
  of the write.
- **Input:** the joystick is sampled every VBL, but `$A00`/`$A01` are consumed once per logic step.

Each game image is therefore shown for exactly 3 PAL VBLs (60 ms).

## 7. Implications for 60/120 Hz output (later phase)

- **Source rate:** the source material has 16.67 distinct images per second. Presenting the emulated 50 Hz
  frames on a 60 Hz display (fixed-timestep pacing, as now) shows each image for 3 or 4 host refreshes
  (roughly 50 ms / 67 ms). That is the original's judder plus a 5:6 beat.
- **What real smoothness needs:** interpolating between consecutive *logic* states, 60 ms apart, not between
  VBL frames, which are mostly identical. The candidates are:
  - the vertical scroll position (the scroll/animation counters in `$CDC`-`$CF0`, per `memory-map.md`; not yet
    pinned down);
  - the object records in the five lists at `$A88`/`$AEA`/`$B4C`/`$BAE`/`$C10`.

  Each of these would be rendered at host rate. Showing the presented state up to one logic step late adds
  60 ms of latency.
- **What doubling the logic rate would need:** forcing `$410 = 4` (25 Hz) is not a fix. It makes the game
  run 1.5x faster, because movement is per step rather than time-scaled, and it slows the music to 5/6.
  Doubling the logic rate would need per-step velocities halved throughout the game code.
- **Audio:** the music tick is VBL-driven (50 Hz) and independent of the logic rate. Keep it on the
  emulated 50 Hz clock, not the host refresh.

## 8. Accurate bus timing (Phase 6c)

**Question:** how fast do the CPU and the blitter really run on the reference A500, and how much of that does
the runtime have to model for the game's logic steps (and so its random numbers) to follow the reference?

**Answer:** the runtime now models the OCS chip bus per colour clock (`--timing accurate`, the default;
`runtime/machine_timing.cpp`). Before the first disc load, the game's random generator state is identical to the
reference at 752 of the 753 sampled frames (title, credits, SELECT menu), against 155 with the old model. The
remaining differences come from instant disc loads, not from the bus model (§8.5).

### 8.1 Why it matters

The game's generator `rng_next` (`$1F50`) is pure arithmetic, and every logic step draws numbers (the
starfield, enemy choices, the shop's cash top-up). With a zero-time blitter and no DMA contention, a step never
overran 3 VBLs, while on the reference heavy steps take 4-5. From game frame 115 the reference had made fewer
calls, so every later random outcome differed (`reference-compare.md` section 9).

### 8.2 The reference machine

`tools/compare/amiberry-harness/x2ref.uae.in`: A500, 68000 at 7.09 MHz, OCS Agnus, `chipmem_size=1` (512 KB
chip RAM at `$000000`), `bogomem_size=0` and `fastmem_size=0` (no slow or fast RAM), PAL, cycle-exact CPU,
memory and blitter. The runtime matches it: chip RAM only (the game never addresses `$C00000`).

- **313 lines per frame.** A non-interlaced PAL Agnus keeps the long-field flag and shows 313 lines every frame
  (`vpos` 0-312 in the reference's per-line trace). The runtime had 312. The frame is now 142,102 CPU cycles
  (49.92 Hz), so each logic step has 3 more lines of CPU time, as on the reference. VPOSR reads LOF=1.
- **227 colour clocks per line** (PAL has no long/short line alternation).

### 8.3 The model

Time is counted in absolute CPU cycles; a colour clock (cck, one chip-bus slot) is two. Each CPU bus cycle,
each blitter cycle and each DMA slot is placed on that one clock.

**Fixed DMA slots** (WinUAE `custom.cpp` numbering, the reference's): 4 refresh slots at hpos 3, 5, 7, 9;
audio channel n at hpos 17+2n on a line after one in which the channel fetched a word (Paula fetches are counted
per line; WinUAE decides the audio slots at the start of the line from the same DMA request); bitplane fetches on
the lines of the vertical display window from DDFSTRT to DDFSTOP: lores fetch units of 8 cck with planes 1-6 at
offsets 7, 3, 5, 1, 6, 2 (Hardware Reference Manual figure 6-9), hires units of 4 cck. With the game's 4 lores
planes the fetches take only odd slots, so they slow the blitter (which can use any free slot) but not a CPU in
step with the even slots.

**Blitter.** When BLTSIZE is written the memory operation is done at once, as before; then the blit occupies
the blitter for its cycle sequence, a port of the shifter logic of WinUAE/Amiberry `blitter.cpp`
(`blitter_next_cycle`, `get_current_channel`, and the allocation loop of `decide_blitter_maybe_write2`):

- the BLTSIZE write cycle, two start-up cycles, then per word the channel diagram (A D: 2 cycles, -D: 2,
  BD/CD/ACD/ABD: 3, ABC D/BCD: 4; fill mode with D but not C: one extra idle cycle per word), the pipeline drain,
  and for D blits a final D write two cycles after the end;
- line mode: 4 cycles per pixel (A slot free, C read, free, D write);
- every blitter cycle needs a colour clock that no fixed DMA uses; cycles that do not access memory (start-up,
  idle steps) leave the slot to the CPU;
- BBUSY reads 1 from the BLTSIZE write until the last main cycle, and also in that cycle (the DMACONR latch,
  WinUAE `blit_busy`); INTREQ BLIT is raised in that cycle. BZERO is computed at the start.

The blitter is simulated lazily up to the time of each CPU bus cycle, and up to the CPU's time at the end of
every 16-cck CPU slice and line, so its interrupt is raised within one slice of its cycle.

**CPU bus cycles.** A 68000 bus cycle is one colour clock of address set-up and one transfer slot (WinUAE
`wait_cpu_cycle_read`: `x_do_cycles_pre`, `dma_cycle`, `x_do_cycles_post`). The transfer needs a slot that
neither fixed DMA nor a blitter memory cycle takes; the CPU waits for one. Without BLTPRI, after the CPU has been
denied 3 slots in a row the blitter skips its next cycle and the CPU gets it (HRM: "the blitter gives the 68000
one memory cycle in four"; WinUAE `BLIT_NASTY_CPU_STEAL_CYCLE_COUNT 3`); with BLTPRI the CPU waits until the blitter
leaves a slot free. CIA accesses do not use the chip bus; they synchronise to the E clock (CPU/10): WinUAE
`cia_wait_pre/post` wait for E phase 2, then take 4 + 6 cycles instead of the bus cycle's 4.

Musashi gives each instruction's total time but not where its bus cycles fall. The model places them:

- the instruction's prefetches, one per instruction word, at its start, then its data accesses (from the bus
  callbacks), then the remaining internal cycles; a long access is two bus cycles;
- internal cycles in front of the bus cycles for branches (Bcc/BRA/BSR taken "n np np", not taken "nn np", with
  the condition evaluated from the flags as the instruction will) and DBcc, and in front of a `-(An)` operand
  read; between the prefetches and the data for `d8(An,Xn)`/`d8(PC,Xn)` operands (Yacht, "Yet Another Cycle
  Hunting Table");
- a taken one-word branch or jump refills two prefetch words, so one more read; MOVEM to registers reads one
  word more than it loads.

Waits are charged to Musashi's cycle counter (`m68ki_remaining_cycles`) from the instruction hook and from the
data-access callbacks, which the interpreter and the recompiled executor both call at the same points with the
same counter, so the two stay bit-identical. The interpreter's own instruction-stream reads (the 16/32-bit reads
at `REG_PC - size`) are left out of the callback timing, by the same rule the recompiler difftest uses.

**68000 cycle table.** Measured on the reference, per opcode, on lines without DMA (`X2REF_INSNLOG`, 241,796
instructions in title, menu and level 1, and the same frames in the runtime: 328 opcodes compared): every
opcode's time agrees with Musashi's except data-dependent ones (taken/not-taken branches) and these, which the
model corrects:

| Instruction | Musashi | Reference (and Motorola's table with its footnotes) |
|---|---|---|
| `ADD/SUB/AND/OR.L <ea>,Dn`, register or immediate source | 6 (+EA) | 8 (+EA); `add.l d0,d1` measured 8 |
| `ADDA/SUBA.L <ea>,An`, register or immediate source | 6 (+EA) | 8 (+EA) |
| `ADDQ/SUBQ.W #,An` | 4 | 8 |
| `ANDI.L #,Dn` | 14 | 16 |
| `BCHG/BCLR/BSET` on Dn, bit 0-15 | maximum (8/10/8, 12/14/12) | 2 less; `bclr #n,d1` measured 12 |
| `DIVU`/`DIVS` | 140/158 always | data-dependent (WinUAE `getDivu68kCycles`/`getDivs68kCycles`, Jorge Cwik's analysis), e.g. `divs.w` in the starfield 122-158 |

Musashi already times MULU/MULS by the operand's bits and shifts by their count.

### 8.4 Calibration evidence

All from the reference harness (`reference-compare.md` section 10), with the runtime logging the same things:

- **Blit durations** (BLTSIZE to BBUSY clear, `blits.log`), medians over the title and level 1 of `game_over`:

  | Blit | Reference | Runtime |
  |---|---|---|
  | 40x192 D only (`$01F0`), the playfield clear | 20,724 cck (24,751 on display lines) | 20,838 (24,756) |
  | 1x16 ABCD (`$0FCA`), bob | 117 | 116 |
  | 1x64 A D (`$09F0`) | 206 | 207 |
  | 2x6 ABCD (`$EFCA`), blank / display lines | 50 / 84 | 50 / 84 |
  | 3x27 ABCD (`$1FCA`), display lines | 533 | 530 |
  | 40x139 A D (`$09F0`), during the level load | 11,177 | 11,372 |

  The `timing` CTest pins the blank-line times of `$EFCA` 2x6 (50) and `$7FCA` 1x4 (18) to the reference's.
- **CPU time per instruction**: in the starfield routine `$854C-$8612` over 10 frames the reference takes
  124,416 cycles and the runtime 124,768 (+0.28%); in the title zoom `$80DA-$818E`, 334,068 against 333,840
  (-0.07%). Before the fixes above the runtime was 1.0% (stars) and 2.1% (zoom) off.
- **Logic-step timing**: in the title the main loop reaches the swap wait `$2034` on the same line as the
  reference in nearly every step and never more than one line off, where the old model was 1 line late in
  every step and the first version of this model (312 lines, Musashi's table as is) 4 lines late.

### 8.5 Results

Generator state per game frame, logic steps and lines in the swap wait over whole replays
(`tools/compare/timing_compare.py`; reference runs with the step-aligned load resume of `reference-compare.md`
section 10):

| | Fast model | Accurate model |
|---|---|---|
| title and menu (game frames 1-753): generator state identical | 155/753 frames | 752/753 frames (the other is read mid-update) |
| `game_over`, all 4,519 compared frames: generator state identical | 155 | 755 (all before the load) |
| `game_over`: logic steps over 4,545 frames, reference 1,389 | 1,420 | 1,392 |
| `two_player`: generator identical, game frames 1,500-3,000 (RNG re-seeded once after the load) | (diverged) | 1,479/1,500 |

**Disc loads.** The runtime serves disc sectors instantly (`disk_shim.cpp`), while the reference loads at
floppy speed for hundreds of frames, and the game keeps calling `rng_next` while it loads: about 2,130 calls
during the first level load. So after every load the two runs are thousands of calls apart in the random
sequence whatever the bus model, and exact agreement after a load would need the disk timing to be emulated too.
`tools/compare/resync_rng.py` re-seeds the generator to the reference's state after each load to measure the
bus model alone; with it, `two_player` runs 1,500 frames of level 1 (blitter-heavy) with the generator identical
at every frame until a death lands on a different step at game frame ~3,000.

### 8.6 What is approximated or left out

- **Bus-cycle placement within an instruction** is by the rules of §8.3, not the 68000 microcode. On display
  lines a few instructions wait one colour clock earlier or later than on the reference (the instruction totals
  agree to 0.3%).
- **Copper DMA** is not counted (the game's lists are 8 MOVEs at line 0 and a COPJMP at the swap).
- **Disk DMA** does not exist (instant loads, above). **Sprite DMA** is not modelled (the game never sets
  SPREN).
- **Interrupt timing:** VERTB is raised at the start of line 0 and taken after the current instruction; the
  reference raises it at hpos 3 and has the 68000's IPL sampling and autovector acknowledge, so its VBL handler
  starts about 17 colour clocks later in the line. The handler's own time is the same.
- **Audio DMA slots** follow the previous line's fetches.
- **The blitter's memory operation** is still done when BLTSIZE is written. Writes to blitter registers or to
  the blit's memory while it runs would therefore act differently from the hardware; the runtime counts them
  (`blit_reg_writes_busy`, `blit_restarts`) and the game makes none in any replay.
- **Writes' beam position** for the renderer is the colour clock of the bus cycle, kept within the current
  line.

The model is deterministic, and the recompiled executor stays bit-identical to the interpreter under it (all
per-replay equivalence CTests, `recomp_equivalence` and `recomp_difftest` pass). Headless full speed on this
machine (Apple Silicon), `level1_flood` 6,000 frames (120 s of game time): interpreter 11.2 s (10.7x real time,
fast model 10.7 s), recompiled build 7.1 s (16.8x; fast model 6.0 s).
