# Xenon 2 Megablast (Amiga) - memory map and boot chain

Source: `Xenon2.zip` (two ADFs, tagged `[t+3]` = cracked release with a 3-option trainer).
Tools: `tools/unpack_xenon2.py`, `tools/unpack_bs1.py`, `tools/analyze68k.py`, `tools/build_entrypoints.py`
(all native Python; Ghidra/REA could not be used - no Java runtime, REA has no Amiga/68k loader. Disassembly via capstone M68K).

## Boot chain (disc 1, OFS AmigaDOS)

| Step | What | Notes |
|---|---|---|
| Boot block | standard `DOS\0` boot block (opens dos.library, starts the DOS startup-sequence); not analysed further | confidence high |
| `s/startup-sequence` | `add21k` / `BS1` / `XENON-II` | |
| `c/add21k` | 280-byte helper (not analysed) | |
| `BS1` | 30516-byte hunk, ByteKiller-family backwards cruncher with XOR checksum; unpacks to $30000 (56843 bytes), then JMP $30000 -> `jmp $32756`. It is the **crack-group intro** ("...th in 1989", uses graphics.library). | `tools/unpack_bs1.py`, checksum residue verified 0 |
| `XENON-II` | single chip CODE hunk (0x323 9C bytes = 205 KB). Code offsets $000-$135 = ByteKiller-style decruncher (flashes COLOR00), packed stream from $136..$32396 read backwards. Unpacks to $30000 (0x464CA bytes) then `JMP $70000`. | `tools/unpack_xenon2.py` |

Unpacked `XENON-II` image (base $30000, file `generated/lane-d/xenon2_30000.bin`):

| Image range | Runtime | Content |
|---|---|---|
| $30000-$6FBFF (0x3FC00 B) | **copied to $400** by the stub (`move.b (a0)+,(a1)+` x 0x3FC00, then `JMP $400`) | the real game, `generated/lane-d/game_0400.bin` |
| $70000-$764C9 | stays at $70000 (stub) | trainer / crack menu: copper bars, keyboard/joy trainer options, OpenLibrary(graphics) via exec ($4), `trap #0` to enter supervisor, then jumps to the game |

**Recompilation target = resident game image at $400 (+ level modules), NOT the stub/BS1.** The stub applies three conditional runtime patches to the game (trainer), verified against the image bytes:
- `move.b #$60,$35984` (= game $5D84): turns `bhi.b $5d8a` (`62 04`) into `bra.b` in the damage routine (`sub.w d0,$cc6.w`), skipping the death/low-energy `bsr $5d00` - infinite energy
- three `$4E71` NOPs at `$3771C` (= game $7B1C): removes `subq.w #1,$CF0.w` (lives) - infinite lives
- flag3 word patches at `$37446`/`$3744C` (= game $7846/$784C): hold `0001`/`0031`; a no-op at the default flag value 1 (start-level selector)
The patches are applied per stub flags `$70232` / `$70236` / `$7023A` (default 1). The game image itself is unpatched. Confidence high.

## Runtime memory map (game, chip RAM, bare metal, 512 KB, supervisor mode, no OS after boot)

| Address | Use | Conf. |
|---|---|---|
| $000-$3FF | vectors. All vectors $08-$3FC set to $7032 (restart). $24 (trace) = bogus $7670CB6B (anti-trace). $64 = $95E4 (L1: DSKBLK done), $68 = $0FAC (L2: CIA-A keyboard), $6C = $20C2 (L3: VBL; temporarily $2098 while loading from disc). $80 trap#0 used only by stub. | high |
| $400 | `bra.w $7032` (entry), then zeros to $9FF | high |
| $404 | last-frame counter snapshot (for frame pacing); $41C = VBL counter (IRQ3 `addq`); $410 = pacing threshold (4 if CIA-A CRA bit 7 reads 1, else 5; the 8520 always reads 0, so 5 = one logic step per 3 VBLs, see timing.md) | high |
| $406 / $40A | draw / display screen buffer pointers (swapped by `wait_frame_swap_copper` $2026/$202E). Initially $4C700 / $44000. | high |
| $414 / $418 | queued sound effect / music command words ($FFFF = none), dispatched from the VBL via $2094 | high |
| $430-$458, $43C-$458 | misc game flags / 8 x (word,long) tracked-object id table | medium |
| $9FC-$A07 | options & input: $9FC music on, $9FF last raw keycode, $A00 joystick/fire bits assembled by IRQ3 from JOY0DAT/CIA-A PRA (bits 0-3 directions, bit7 fire), $A01 fire-edge latch, $A03/$A05 player-present flags, $A07 abort flag | medium |
| $A88,$AEA,$B4C,$BAE,$C10 | heads of 5 object lists (record 0x62 B: +0 active word, +2 update handler ptr, +$E/+$12 prev/next, +$44 id) | medium-high |
| $CDC-$CF0 | frame counter, scroll/anim counters, $CF0 = lives, $E04 = current level (1-6), $E06 = energy | medium |
| $1014 / $1038 | two copper lists (4 x BPLxPT pairs + end); `$700E` holds the one currently loaded in COP1LC, toggled each frame | high |
| $19F9E-$1BB7F | **music/sound engine** (Paula direct) - a3 = $19F9E is the data/base register, driver state in $19F9E+$5E0.. and 4 x 0x24-byte channel records | high |
| $1BB80-... | 20 sample records `{len.l, rate.w, data}` (period computed from 3579545 Hz / rate) followed by sample data | high |
| $1C400-$3FBFF | graphics (tile/sprite/bob bitmaps, ~2-6% zero bytes) - not analysed | medium |
| $4321C | 200-word y-offset table (n*$A0) used for line addressing | high |
| $435FC | initial supervisor stack | high |
| $44000-$455FF | disc track buffer (11 x 512 sector) and (when no load) screen buffer A; $45600-$47600 raw MFM buffer | high |
| $49600 | disc-ID read buffer (disc 2 sector 0, checks `"X2B "` at +2) | high |
| $4C700 | screen buffer B / copy of $44000 (saved around disc loads) | high |
| $54E00-$7FFFF | **level module** (disc 2), loaded on demand; level N = sectors table at $91EA | high |

Display: BPLCON0 = $4000 (4 planes), DIWSTRT/STOP $3A81/$02C1 (lines 58-257, 200 lines; verified from the runtime register log), DDFSTRT/STOP $38/$D0, BPL1MOD/BPL2MOD = $78 (planes interleaved, 40 B/row x 3 gaps), no hardware sprites (DMACON $87D0: DSKEN,BLTEN,COPEN,BPLEN,DMAEN,BLTPRI; SPREN off; SPRxDATA cleared). Interrupts enabled: INTENA $C02A = INTEN+VERTB+PORTS(L2)+DSKBLK. Audio DMA driven by the music engine ($DFF096 bits 0-3), audio interrupts not used.

## Disc 2 (raw, non-DOS, but standard Amiga MFM 11 sectors/track)
- Sector 0: disc ID (`00 00 "X2B " 60 00 ...`).
- Linear sector index n -> track = n/11, sector = n%11, side/cylinder via step logic; **equals ADF byte offset n*512**.
- Level table at game $91EA, entry = {start sector.w, count.w}, indexed by (level-1)*4:
  L1 11/229, L2 250/254, L3 514/275, L4 799/345, L5 1154/312, L6 1476/248 (L4 ends exactly at $80000). Sectors 1-10 are $E5 filler. Entries 7+ unused (65535) except `{24576,190}` junk.
- Level modules are **uncompressed**, loaded to $54E00. Header (longs/words): `+0,+4` data pointers (executed as harmless `ori.b`), `+8` ptr, `+$C bra.w level_init`, `+$10 bra`, `+$14 ptr`, `+$18 bra`, `+$1C ptr = palette (8 longs, movem d0-d7)`, `+$20 bra`, `+$24 bra.w level_frame (called once per frame from main loop)`, `+$28 ptr`, `+$2C bra`, then tables. $54E04 is used as a variable slot (game saves/restores it around load).
- Files: `generated/lane-d/level{1..6}_54e00.bin`.

## Copy protection / anti-tamper
- Disc-ID check ("X2B " at disc 2 sector 0) - trivially satisfied by the ADF.
- Anti-trace trick in game $7104-$763A: trace vector $24 = odd address (address error => reset $7032) and encrypted-on-the-fly code (trace handler at $71CC/$7206 XOR-decrypts the long at PC with `swap(not(prev long))`). Treated as a decoy: not referenced from the live boot path ($7032 -> $70F6 -> bra $763A). Confidence medium that it is dead; **verify with a trace when an emulator is available**.
- 68000 24-bit address assumption: jump table at $47D6 stores flag bytes in the top byte of code pointers (`movea.l $47D6(pc,d3.w),a1` then `jsr (a1)`). Recompiler must mask code pointers to 24 bits.
