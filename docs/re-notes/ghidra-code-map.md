# Xenon 2 Megablast - code map (Ghidra pass)

Supersedes the heuristic capstone map in `code-map.md` for function boundaries, indirect sites and hardware access. `code-map.md` is still useful for prose descriptions. Where the two disagree, this file is right unless it says otherwise; see section 8.

Confidence key: **H** = I read it in the Ghidra listing. **M** = inferred from register usage, callers or structure. **L** = name guessed from context.

## 1. Method

- **Tool:** Ghidra 12.1.4 headless (`/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless`), with JDK `/opt/homebrew/opt/openjdk@21`. Language `68000:BE:32:default`. Raw images are loaded with `BinaryLoader`.
- **REA could not be used.** It has no 68k support. These attempts failed (exact errors):
  - `open_binary game_0400.bin` returned `unsupported binary format`.
  - `open_binary disc1/XENON-II` (the AmigaDOS hunk file) returned `unsupported binary format`.
  - `open_binary xenon2.gpr` (the Ghidra project) returned `truncated or unsupported binary header`.
  - An ELF32/EM_68K wrapper around the image returned `unsupported ELF architecture`.

  So all analysis is Ghidra-headless, driven by scripts. No Ghidra Amiga hunk loader is installed. That is not needed because the images are already unpacked flat memory.
- **Memory setup** (`tools/ghidra/X2Prep.java`, a pre-script):
  - The image sits at its runtime base.
  - Holes in `$0-$7FFFF` are uninitialised chip RAM.
  - `CUSTOM` is at `$DFF000` (`$200`), `CIAA` at `$BFE000` and `CIAB` at `$BFD000`. All three are volatile.
  - Every custom and CIA register is labelled.
  - For level programs, `game_0400.bin` is added as a second block at `$400`, so calls into the resident code resolve.
  - Ghidra's "Create Address Tables" analyser is turned off. It turned graphics and music data into pointer tables and put data over real code (for example at `$9818`). Dispatch tables are supplied explicitly instead.
- **Seeds are kept minimal on purpose.** That way the export can show what Ghidra reaches by itself.
  - Game seeds:
    - the reset entry `$400`/`$7032`;
    - IRQ1 `$95E4`, IRQ2 `$0FAC`, IRQ3 `$20C2` and `$2098`;
    - the 56 resident API jump-vector slots at `$E0C-$F5B` (found in this pass, see section 2).
  - Level seeds: the six header `bra.w` slots and their targets. Slot 6 is seeded at `$54E00`.
- **Post-script** (`tools/ghidra/X2Export.java`):
  1. Records the functions found by plain auto-analysis (the `pure` flag).
  2. Adds known dispatch tables as computed refs. A target is rejected if it is mid-instruction or fails Ghidra's `PseudoDisassembler`.
  3. Classifies every heuristic entry point from `generated/lane-d/entrypoints.json`. It checks whether Ghidra already reached it, whether it overlaps an instruction, `PseudoDisassembler.checkValidSubroutine`, and pointer evidence (an exact BE32 copy of the address anywhere in the images). Plausible ones are added, strongest evidence first.
  4. Makes functions for code that was reached but sits outside any function (jump-table targets).
  5. Exports functions, computed-flow sites, hardware access, code writes, control flow out of the window, and xrefs into watched ranges. It also writes a listing.
- **Dispatch tables in the level modules** are found by an opcode scan in `tools/ghidra_x2.py` (`find_pc_tables`). It looks for two patterns:
  - `movea.l d8(pc,Xn),An ; jmp/jsr (An)`
  - `lea abs.l,An ; movea.l d8(An,Xn),An ; jsr (An)`
- **Merge:** `tools/merge_entrypoints.py` combines the heuristic output and the Ghidra exports.

### Reproduce

```
python3 tools/ghidra_x2.py all        # 6 levels first, then game (~45 s); project: generated/lane-d2/ghidra-proj
python3 tools/merge_entrypoints.py    # -> generated/lane-d2/{functions,indirect_sites,hw_access,entrypoints_v2}.json
```

Per-program raw output is in `generated/lane-d2/`:
- `ghidra_<prog>.json`: the export;
- `ghidra_<prog>.lst`: the listing;
- `cfg_<prog>.txt`: seeds, tables and candidates;
- `headless_<prog>.log`.

To open the project interactively, run `ghidraRun` with `JAVA_HOME` set, then open `generated/lane-d2/ghidra-proj/xenon2.gpr`.

### Results

| Program | Functions | Plain-analysis functions | Insn bytes | Tables added | Table targets rejected |
|---|---|---|---|---|---|
| game ($400) | 305 | 252 (62 seeds) | 31,174 | 45 | 0 |
| level 1 | 43 | 14 | 4,394 | 16 | 0 |
| level 2 | 51 | 17 | 6,346 | 13 | 0 |
| level 3 | 44 | 15 | 4,784 | 13 | 0 |
| level 4 | 49 | 12 | 4,390 | 15 | 1 (`$55A68` as a table entry; it is still a function because of its `move.l #imm,(6,a0)` reference) |
| level 5 | 40 | 12 | 3,588 | 19 | 3 (`$72A86/$72BC6/$72D88`: the table scan overran into data) |
| slot 6 (shop + ending) | 69 | 64 | 7,200 | 25 | 0 |

In `functions.json` there are 601 functions in total:
- by confidence: 416 high, 183 medium, 2 low;
- by source: 386 `ghidra-auto`, 162 `ghidra-orphan-entry` (jump-table and computed-jump targets), 27 from tables or flow, 3 seeds, and 23 heuristic candidates validated by Ghidra.

No instruction bytes are left outside a function in any program.

## 2. New structural facts

1. **Resident API vector table at `$E0C-$F5B`** (H). It holds 56 slots of 6 bytes, each a `jmp abs.l`. The level modules call the resident engine almost only through these slots, for example `jsr $0EC0.w`, `jmp $0E60.w`, and `$0E5A`/`$0EA2` as entry 0 of every level dispatch table.
   - The old gap sweep decoded the slot interiors misaligned. That accounts for 31 of its "functions" (`$E10`, `$E22`, `$E34`, `$E46`, `$E52` ... `$F48`). All of them are data.
   - The old name `blit_wrapper_a` at `$E96` is really slot `$E96 -> jmp $1BD0`.
2. **Level header slots, by game caller** (H). This partly contradicts `code-map.md`:
   - `$54E00`: `jsr` from `$905A`. In levels 1-5 this runs three `ori.b` header longs, then `bra` to the `$0C` hook, which returns `d4=0`. So it is effectively a no-op. In slot 6 it is the real entry point.
   - `$54E0C` hook: called from `$3066`/`$30D0`/`$312A` (object code). In L1 and L5 it is just `moveq #0,d4; rts`. **It is not `level_init`.**
   - `$54E10` `level_event_a`: called from `$3574`. It is `level_event_scan` over the list at `$54E38`, dispatching through a table with `jmp (a2)`.
   - `$54E18` `level_event_b`: called from `$35B2`. List at `$54E14`, also dispatches with `jmp (a2)`.
   - `$54E20` `level_start`: called from `$6F6A`. It copies data to the top of RAM (`$80000` downwards, stored in `$54E04`) and initialises variables. **This is the real level initialisation.**
   - `$54E24` `level_frame`: called from `$7912` in the main loop.
   - `$54E2C` hook: called from `$6EBE` after the buffer toggle at `$428`.
   - Header data the game reads: `$54E04` (top-of-RAM pointer), `$54E08` (tile graphics, read by `draw_background_*`), `$54E14`/`$54E38` (event lists), `$54E1C` (palette), `$54E28`, `$54E30`/`$54E34`, `$54E3C`/`$54E3E`.
3. **Disc-2 slot 6 is not a level.** It is the shop ("GIVE ME THE CASH !", the weapon list) plus the ending text ("WELL, THAT'S IT VIEWERS."). There is no `bra` header, and execution starts at `$54E00`. The game has 5 levels.
   - It installs its own copy of the music driver by rewriting the IRQ3 callback vectors: `$2090 = $6477C` and `$2094 = $647A6`. It restores `$19FAC`/`$19FD6` at `$55036`.
   - Its menu-action table (`$5573A`, 25 entries, called via `jsr (a0)` at `$556EA`) calls the same resident handlers as the `$47D6` table: `$4906 $4894 $48F6 $48B4 $5206 ...`.
4. **The `$47D6` table holds pickup/weapon effect handlers, not enemy AI** (M). The shop calls them directly. `$4914` is one of them; the heuristic only had it as a low-confidence gap-sweep entry. `$4634` is renamed `powerup_dispatch`.
5. **Object record handler slots** (H):
   - `+2` = update handler, called by `run_object_list` `$3656` at `$3662`.
   - `+6` = draw handler, called by `run_object_list_draw` `$366A` at `$3676`. Its values are `blit_bob_*` (`$128E`, `$1368`, `$1438`), `null_handler` `$3130`, or API slots.
   - `+$A` = kill/hit handler, jumped to by `obj_call_handler_a` `$5924` with `move.l $A(a1),-(sp); rts`.
   - `+$1C` = animation script stream, used by `anim_script_step` `$2C5A`.
   - `code-map.md` says `$3676` uses obj+2. **That is wrong: it uses obj+6.**
6. **The keycode table at `$7954`** is `{$19 P -> $79BA, $45 ESC -> $7800, $40 SPACE -> $797A, $00 default -> $79D6}`.
   - `$797A` is real code, but Ghidra's data-reference analyser had placed a pointer over it (`$7978`). The export clears that data when it adds the table target.
   - The variables at `$7960-$7979` sit between the table and the code.
7. **Music driver API** (H): seven 14-byte wrapper stubs at `$19F9E` (init -> `$19FFC`), `$19FAC` (tick -> `$1A154`), `$19FBA` (stop -> `$1A09A`), `$19FC8` (-> `$1A112`, DMACON/ADKCON), `$19FD6` (sfx), `$19FE4` (-> `$1A0F4`) and `$19FF2` (-> `$1A13C`).
   - `$19F9E` is code (the init wrapper), not just the a3 base. `a3` is set by `lea -x(pc),a3` inside the core.
8. **RNG** `$1F50` (H): a 64-bit `addx` generator. Its seeds `$1FEE`/`$1FF2` start as `$63141592`/`$35358979`, the digits of pi. `$9692` writes the seed (`move.l $9660,$1FEE`), and the old lane-d "code_writes" candidates for `$1FEE`/`$1FF2` were these seed writes.

## 3. Interrupts and main loop (H unless noted)

| Address | Name | Notes |
|---|---|---|
| `$0FAC` | `irq2_cia_keyboard` | CIAA ICR/SDR read, handshake through CIAA CRA, INTREQ ack |
| `$20C2` | `irq3_vbl` | Reads **JOY1DAT** (`$DFF00C`, the port-2 joystick; the old notes call it JOY0DAT) and CIAA PRA (fire). Reads INTREQR low byte `$DFF01F`. Calls `($2094)`, then `($2090)`. |
| `$2098` | `irq3_vbl_diskload` | Installed at `$6C` around disk loads (`$8FBE`, `$902C`, `$90B8`, `$91AE`), restored to `$20C2` |
| `$95E4` | `irq1_dskblk` | INTREQ ack, sets `$9206` |
| `$7032` -> `$70F6` | `reset_trace_vector_then_init` | `$70F6` writes `$7670CB6B` to vector `$24`, then `bra.w $763A`. `$763A` is an `add.w d0,d1` landing pad that falls into `$763C`. Ghidra's function body `$70F6..$7954` covers init and the main loop. All display, interrupt and DMA setup is here (see hw table). |
| `$78A4` | `main_loop` | Calls in order: `$17D6`, `$233A`, `$202E`, `$88B6`, `$1F22`, `$17D6`, `$233A`, `$6766`, `$16BE`, `$683C`, `jsr $54E24`, `$35D4`, `$6372`, `$61E6`, `$3554`. Then `key_dispatch` `$7940` (`jmp (a0)` at `$7952`). This matches `code-map.md`. |

## 4. Routines by subsystem

| Subsystem | Address | Name | Evidence | Conf. |
|---|---|---|---|---|
| Blitter | `$10F6` | `blit_setup_masks_mods` | BLTAFWM/ALWM, BLTxMOD | M |
| Blitter | `$129A` | `blit_bob_cookie` | BLTCON0 = shift \| `$FCA` (A,B,C->D cookie-cut), all pointers, BLTSIZE | H |
| Blitter | `$128E` | `obj_draw_bob` | Loads x,y and the gfx pointer from obj+`$20`/`$24`/`$16`, then falls into `$129A` | H |
| Blitter | `$1368` | `blit_bob_acd` | A,C,D only | M |
| Blitter | `$1438` | `blit_bob_full` | A,B,C,D, all modulos | M |
| Blitter | `$15CE` | `print_string_blit` | Font `$B666`, charset `$86FC` | M |
| Blitter | `$17D6` | `blit_objects_main` | BLTADAT fill, A->D | M |
| Blitter / scroll | `$18B6`, `$1BD0` | `draw_background_a/b` | Draw buffer `$406`, tile gfx from `$54E08`. Reached through API slots `$E8A`/`$E96`. | M |
| Blitter | `$635E` | `blit_copy_wrapper` | From lane-d | M |
| Copper / frame | `$2026`/`$202E` | `wait_frame_swap_copper(_alt)` | Waits `$41C`-`$404` >= `$410`, VPOSR, COP1LC + COPJMP1. Copper lists `$1014`/`$1038` hold only BPLxPT. | H |
| Copper | `$70F6` (init) | — | The only other COP1LC/COPJMP1 writer | H |
| Palette | `$6372` | `palette_fade` | COLOR00..14 (even) | M |
| Palette | `$4926` | (pickup effect) | COLOR00..14; entry 19 of `$47D6` (pickup flash) | M |
| Palette | `$6FE8` | `palette_shadow_init` | Writes `$7012` shadow and COLOR (`$7012+$DF816E = $DFF180`) | L |
| Palette | `$79D6`, `$8220`, `$847E`, `$84AE`, `$885E`, `$88B6` | menu/title palette | COLOR regs | M |
| Sprites | — | — | **No hardware sprites.** Init only clears SPR0-7DATA. Nothing writes SPRxPT/POS/CTL. | H |
| Bitplanes | — | — | No CPU writes to BPLxPT. They are set through the copper lists. | H |
| Audio | `$19F9E-$1A13C` | music API and `music_init`/`music_stop` | AUDxVOL, DMACON, ADKCON. CIAA PRA bit 1 = audio filter (LED). | H |
| Audio | `$1A154` | `music_tick_core` | AUD0/1 LC/LEN/PER/VOL, DMACON. AUD2/3 go through an `a6`-relative channel base that static analysis does not resolve. Dispatch `jmp (a3,a2.w)` at `$1A468`. | H |
| Audio | `$1B26A`, `$1B5A2` | `music_channel_update`, `audio_dma_ctrl` | — | M |
| Input | `$0FAC`, `$20C2` | see section 3 | — | H |
| Disk | `$92C8` `$92EE` `$9308` `$932E` `$93C0` | motor on/off, home, seek, step | CIAB PRB, CIAA PRA bit 4 | H |
| Disk | `$93F8` | `disk_read_track` | DSKPT, DSKLEN, ADKCON | H |
| Disk | `$921A` | `disk_read_sectors` | — | H |
| Disk | `$951E`, `$957E` | MFM sync / decode | — | H |
| Disk | `$8F9A` | `disk_check_discid` | — | H |
| Disk | `$8FF2` | `level_load_and_run` | — | H |
| Disk | `$917E` | `level_load_if_changed` | — | H |
| RNG | `$1F50` | `rng_next` | — | H |
| RNG | `$1FF6` | `rng_mod` | (`divu`) | H |
| Objects | `$2418` | `obj_alloc` | Free list `$42A` | H |
| Objects | `$0FEE` | `obj_free` | — | H |
| Objects | `$6862` | `obj_unlink_free` | — | M |
| Objects | `$35D4` | `update_object_lists` | — | H |
| Objects | `$3656`/`$366A` | update/draw passes | — | H |
| Objects | `$5924` | `obj_call_handler_a` | — | H |
| Objects | `$2C5A` | `anim_script_step` | — | H |
| Objects | `$3130` | `null_handler` | `rts` | H |
| Objects | `$105C`/`$107E` | clip routines | x < `$140`, y vs `$420` | M |
| Level | `$3554` | `level_event_scan` | Scroll triggers in `$54E38`/`$54E14` -> `jsr $54E10`/`$54E18` | M |
| Level | `$4634` | `powerup_dispatch` | Contains `jsr (a1)` via `$47D6` | M |
| HUD | `$16BE`, `$176E`, `$6766` | `hud_draw_energy`, `hud_draw_icons`, score | Energy `$CC6`, `$CCA`; `$C92`/`$C96` | L |

Per-routine register sets for all 41 routines that touch hardware are in `hw_access.json`. Values are R (read), W (write), A (address/DATA reference from Ghidra constant propagation), or `W*`/`R*` (`lea $DFFxxx,An`-relative, tracked by the script).

## 5. Indirect control flow (`indirect_sites.json`, 24 sites)

| Module | Site | Instruction | Resolution | Targets |
|---|---|---|---|---|
| game | `$217E`/`$2194`/`$21A8` | `jsr (a0)` | vars `$2094`/`$2090` | `$19FD6`/`$19FAC`; `$647A6`/`$6477C` while slot 6 is loaded |
| game | `$47D0` | `jsr (a1)` | table `$47D6` (24-bit masked) | 19 |
| game | `$7952` | `jmp (a0)` | key table `$7954` | 4 |
| game | `$97E6` | `jmp d(pc,d6.w)` | word table `$97EA` | 6 |
| game | `$1A468` | `jmp (a3,a2.w)` | word table `$1A56A` | 12 (Ghidra's extra `$19F9E`/`$19FDE` are filtered as bogus) |
| game | `$2C6C` | `jmp (a2)` | **data-driven**: animation script streams | — |
| game | `$3662` | `jsr (a1)` | **data-driven**: obj+2 | 58 immediates seen (`handler_sets`) |
| game | `$3676` | `jsr (a1)` | **data-driven**: obj+6 | 28 immediates |
| game | `$5924` | `move.l $A(a1),-(sp); rts` | **data-driven**: obj+`$A`. Not flagged by Ghidra; found by pattern search. No other push/rts sites exist in any module. | 22 immediates |
| L1-L5 | 2 per level (`level_event_a`/`_b`) | `movea.l tbl(pc,d0.w*4),a2; jmp (a2)` | table after the `jmp` | L1 5/11, L2 6/7, L3 7/6, L4 8/7, L5 10/9 |
| L1 | `$56116`, `$561D6` | `jmp (a1)` | **data-driven**: tail call to obj+2 | — |
| L5 | `$55E32` | `jsr $46(a2)` | **data-driven** (record field) | Ghidra's `$48E70010` is a constant-propagation artefact |
| slot 6 | `$556EA` | `jsr (a0)` | table `$5573A` (1-based) | 25 (mostly resident pickup handlers) |

`handler_sets` in `indirect_sites.json` lists every `move.l #imm,(2|6|$A|$1C,An)` across all modules.
- obj+`$A` also receives `$C66`/`$C84`/`$C8A` from pickup handlers (`$4B62`...`$528C`). Those addresses are in the zeroed variable area, so for those object types the field is a data pointer, not code (M).
- `$5559A` (L1 obj+`$1C`) is a script stream, which is data. The heuristic called it a function.

A recompiler still needs a runtime trace (or a data-format parser) for the data-driven sites: object handlers that come from level data tables rather than immediates, and animation-script jump targets.

## 6. Self-modifying code and anti-trace

- **Resident game:** no Ghidra WRITE reference lands inside an instruction. All 10 lane-d `code_writes` candidates are data words between functions: the RNG seeds `$1FEE`/`$1FF2`, `$8942`, `$8F98`, `$8F94`, and so on.
  - `$54E04` is a level-header variable, written by `$90EC` and by `level_start`.
  - The only real code patches are the crack stub's trainer patches (`$5D84`, `$7B1C`, `$7846`/`$784C`). Those come from outside the game; see `memory-map.md`.
- **Slot 6:** Ghidra reports 3 code writes (`$64940/48/54` -> `$6492A..$64934`). These are false positives. `a5 = lea -$1A(pc) = $6491E`, a sample header, and the loop runs once (`moveq #0,d1; dbf`). Constant propagation stepped `a5` past it.
- **Anti-trace block `$7104-$763A`: confirmed dead.**
  - After the final run there are zero instructions inside it and zero references into it from anywhere in the game.
  - All nine lane-d entries inside it are unreachable: 8 gap-sweep entries, plus `$7200`, whose only "pointer" is the coincidental `$00007200` at L2 `$6F710`.
  - The only live code next to it is `$70F6-$7103`, which installs the bogus trace vector and branches over the block.
  - Nothing ever sets the T bit: the only SR writes are `#$2700`/`#$2000` and a `move (2,a6),sr` in slot 6.
  - The four "references" seen with address tables switched on came from graphics data at `$10CE4`, `$10D5C`, `$19C7C` and `$19E5C`.

## 7. Reconciliation with the lane-d heuristic (`entrypoints_v2.json -> heuristic_reconciliation`)

Resident entries, 415 in total:

| Status | Count |
|---|---|
| Agree (Ghidra reached it) | 301 |
| Agree (validated by Ghidra and added) | 13 |
| Agree (existing Ghidra function, `$19F9E`) | 1 |
| Seeds | 6 |
| **Disagree: data** | 61 |
| **Disagree: mid-instruction after validation** | 4 (`$3B00 $3FE0 $3FF0 $4020`, table-ptr) |
| **Disagree: dead anti-trace** | 9 |
| **Unknown** (code-shaped, unreferenced) | 20 |

The **94 low-confidence gap-sweep starts** in `entrypoints.json` (the brief says 95, but the file has 94):

| Outcome | Count | Details |
|---|---|---|
| Real code, reached by Ghidra | 20 | — |
| Real code, validated and added | 4 | `$1636` (called from a level module), `$16A0`, `$367E`, `$4914` (shop-table target) |
| Data | 42 | 31 are misaligned decodes inside the `$E0C` API vector table; the rest are variables/tables (`$2092`, `$7014`, `$8F90`, `$22BA`, `$2404`, `$2A14`, `$2A24`, `$326C`, `$5798`, `$796C`, `$8618`, `$8F3E`, `$1FCE`, `$6666`) |
| Dead (anti-trace block) | 8 | — |
| Unknown | 20 | `$1B19E $2C36 $2CF6 $2CF8 $378E $390E $3E1C $4904 $4924 $5DD2 $60D2 $614A $6336 $648A $68A4 $6CC8 $83DA $8858 $8C54 $8E7E` |

The 20 unknowns pass `checkValidSubroutine` but have no call, branch, immediate or exact-long pointer anywhere in the game or the 6 modules. They are either dead code or targets of the data-driven sites. They were deliberately **not** added as functions. A trace should settle them.

The other heuristic kinds:
- **table-ptr:** 37 confirmed as code, 18 data, 4 mid-instruction, 1 dead. Many of the table-ptr "pointers" were coincidental byte patterns such as `$00003200`.
- **immediate-ptr:** 31 code, 1 data (`$891E`, the hi-score table).
- **call-target/branch:** 189 code. 18 of them are table case labels inside functions, which is correct, not separate functions.

Level modules: every heuristic candidate ends up as a function entry except two, which are data:
- L1 `$5559A` (obj+`$1C` script stream);
- slot 6 `$5CABC` (a data pointer stored to `$56488`).

L4 `$55B6A` fails Ghidra's pseudo-check, but it still becomes a function because the dispatch table reaches it. Treat it as needing a trace.

Ghidra also adds about 160 jump-table and computed targets in the levels that the heuristic did not list. The number of instruction bytes reached grows from plain analysis to the final pass: L1 2,958 -> 4,394, L3 1,068 -> 4,784, and L5 192 -> 3,588.

## 8. Disagreements with the old notes (flagged, not silently fixed)

| Old claim (`code-map.md` / `memory-map.md`) | Ghidra finding |
|---|---|
| `$54E0C` = `level_init` | It is a per-object hook (`moveq #0,d4; rts` in L1/L5). The real init is `$54E20` (`level_start`, called from `$6F6A`). |
| `$47D6` = enemy AI jump table | Pickup/weapon effect handlers; the shop module calls the same handlers (M) |
| `$3676` calls obj+2 | It calls obj+6 (the draw handler) |
| `$E96` = `blit_wrapper_a` | API vector slot -> `$1BD0` (`draw_background_b`) |
| IRQ3 reads JOY0DAT (`$DFF00C`) | The address is right, but `$DFF00C` is **JOY1DAT** (game port 2) |
| Disc-2 entry 6 = level 6 | Shop + ending module with its own music player |
| `$19F9E` = data base (`music_base_a3`) | Code: the music-init wrapper (`a3` is set separately) |
| 95 gap-sweep starts | 94 in the file; 24 are code, 42 data, 8 dead, 20 unknown |
| "Remaining code_writes are heuristic false positives" | Confirmed: there is no self-modifying code in the game |

## 9. Remaining unknowns

- The 20 unreferenced code-shaped regions in section 7, the targets of the data-driven sites in section 5, and the L4 obj+6 targets.
- Names marked L or M (HUD, menu palette routines, `$61E6`, `$6766`, the music API `$19FC8`/`$19FE4`/`$19FF2` semantics).
- Audio register usage through the `a6` channel base inside `$1A154`/`$1B26A` (AUD2/3) is not resolved statically.
- Next step: run the game in an emulator with PC tracing (all 5 levels plus the shop), and diff the traced PCs against `entrypoints_v2.json`.
