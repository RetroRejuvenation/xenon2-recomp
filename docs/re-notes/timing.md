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
under load).

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

The CPU has no DMA contention in this model, so steps never overrun their 3-VBL budget. On real hardware,
heavy blitting could occasionally stretch a step to 4 VBLs.

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
