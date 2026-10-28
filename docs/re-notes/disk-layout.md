# Xenon 2 Megablast (Xenon2.zip, "[t+3]" trained pair) - disk layout and loader notes

Lane B. All findings produced by `tools/adf.py`, `tools/hunk.py`, `tools/bkunpack.py`, `tools/dis68k.py`
(libcapstone via ctypes). Intermediate artefacts are in `generated/lane-b/` (gitignored):

| file | what |
|---|---|
| `disc1.adf`, `disc2.adf` | copies of the two ADFs from `Xenon2.zip` (901120 bytes each) |
| `disc1-files/` | files extracted from disc 1 (OFS) |
| `disc1-boot.bin` | 1024-byte boot block of disc 1 |
| `hunks/<name>/hunk0.bin` | raw hunk payloads |
| `bs1-unpacked.bin` | BS1 hunk payload after depacking (loads at $30000) |
| `x2-unpacked.bin` | XENON-II after depacking, memory image based at $30000 (287946 bytes) |
| `x2-image-0400.bin` | first $3FC00 bytes of the above = the game image as it sits at $400 at run time |
| `x2-rt.dis` | linear disassembly of the game image with base $400 (run-time addresses) |

IMPORTANT context: this is a cracked + trained release. Disc 1 was made by "BAMIGA SECTOR ONE" (BS1), the
game image carries a "DONE BY SWITCH IN 1989" stub and a "TRAINED BY BAMIGA SECTOR ONE" credit. Behaviour of
the original (uncracked) game's protection is NOT visible here, see "Protection".

## 1. Disc 1 - AmigaDOS OFS, volume "BAMIGA SECTOR ONE"

* Boot block: `DOS\0` (OFS, no INTL), checksum valid, rootblock ptr 880, stock bootcode
  (`FindResident("dos.library")`, return `resident->rt_Init` in a0; no custom code, no virus).
  sha1 of disc image 45e66f9c...
* Root block 880 valid, bitmap block 907. 505 blocks owned by files, 1255 free, **0 used-but-unowned** blocks
  (nothing hidden outside the filesystem). All header/ext/bitmap checksums verify.

```
BAMIGA SECTOR ONE:
  c/add21k                 280  (hunk exe: CODE 176 + BSS 4, 9 relocs)
  devs/System-Configuration 232  (Workbench Preferences struct, irrelevant)
  s/startup-sequence        20  "add21k\nBS1\nXENON-II\n"
  BS1                    30516  (hunk exe: 1 CODE hunk 30456 bytes, ByteKiller-style packed)
  XENON-II              205784  (hunk exe: CODE hunk 205724 bytes, CHIP memory, + 4 byte BSS; no relocs)
```
`python3 -I tools/adf.py generated/lane-b/disc1.adf ls` reproduces this listing.

### Startup flow

1. Boot block -> AmigaDOS -> `s/startup-sequence` runs three commands in order.
2. `add21k`: opens graphics + intuition, takes `IntuitionBase->ActiveScreen`, sets its BitMap depth to 1,
   `FreeRaster()` (gfx -498) on the second plane, `RemakeDisplay()` (intui -384). Purpose: free ~20 KB of chip RAM
   by shrinking the Workbench screen to 1 bitplane (name = "add 21K").
3. `BS1` (cracker intro "synuswavicalistic ... switch are proud to present their latest invention # synthetic"):
   * tiny decruncher at hunk+$000, packed data at hunk+$E8: header = packed_len $7604, unpacked_len $DE0B
     (56843), crc $77D9200B; backward bit-stream, per-long XOR checksum; colour flicker on $DFF180; on CRC
     mismatch it flashes and returns -1. Decrunches to **$30000** and `jmp $30000`.
   * payload at $30000: `jmp $32756`, then colour tables, font, scroller; routine at $32756 saves regs, takes over
     hardware, runs until the left mouse button (`btst #6,$BFE001`) and **returns via rts** to DOS.
   * Only CIA/PRB access: `$BFD100 |=$F8 ; &=$87` (all drives deselected, motor off). No disk reads.
   Decruncher is reproduced by `tools/bkunpack.py bs1` (output CRC residue 0, stream consumed exactly).
4. `XENON-II` (loaded by DOS into CHIP RAM): hunk0 = 310-byte decrunch stub + packed stream.
   * `lea $134(pc),a0 ; adda.l #$32260,a0` -> a0 = end of packed stream (hunk+$32396, i.e. stream = hunk+$136..$32396),
     last long = unpacked size 287946 ($4624A), dest base **$30000**, unpacked backwards in place.
   * Cruncher is a Time-Cruncher/"BK3"-like backward LZ (3-bit literal-run prefix, 2-bit match class,
     offset widths 8/12/14/16, run lengths 2..). `tools/bkunpack.py x2` reproduces it (ends with a2==0 and
     stream pointer exactly at $136). No checksum in this variant.
   * Then `jmp $70000`. Not a RNC/PowerPacker/ICE file.
5. Stub at $70000 (offset $40000 in the depacked image; text "DONE BY SWITCH IN 1989"):
   * `cmpi.l #'SWIT',$7000E` - anti-tamper: if the text is altered it does `jmp $FC0000` (reset). Computed as
     `$7E00*$200`.
   * sets up a small trainer/options screen using graphics.library (`$70540` name string), three longword trainer
     flags at $70232 / $70236 / $7023A (**all initialised to 1 = enabled**, +$7023E/$70242/$70246 data).
   * patches the game image while it still lives at $30000: `move.b #$60,$35984` (if flag1; run-time $5D84:
     bhi->bra), three `$4E71` NOPs at $3771C (flag2; run-time $7B1C), and a word patch at $37446/$3744C built from
     flag3 (run-time $7846/$784C). Also a table patch (`$706DE` -> `$764DA+$122F+n*$28`, 7 longs).
   * `trap #0` -> supervisor, `move #$2700,sr`, **copies $3FC00 bytes from $30000 to $400** (forward byte copy) and
     `jmp $400`.
   Net effect: the game owns the machine from $400 on; AmigaDOS/the OS is never used again (the game restores
   nothing, it writes its own copper list, vectors $8..$3FF all point to $7032 = soft restart).

### The game image at $400 (what a recompiler should treat as "the executable")

* Entry `$400: bra.w $7032` -> `$7032: move #$2700,sr ; bra.w $70F6` -> `$70F6: move.l #$7670CB6B,d0 ;
  move.l d0,$24.w ; bra.w $763A` -> normal init: SP=$435FC, fill vectors $8-$3FF with $7032, program level-1 vector
  $64 = $000095E4 (disk-block-done IRQ), $6C = $20C2 (VBL), $68 = $FAC, CIA init, copper at $1038, `sr=$2000`.
* Memory use: chip RAM from $400 to ~$6FFFF is game code/data; $44000 = 5632-byte (11 sector) track staging buffer;
  $45600 = raw MFM track buffer ($4000 bytes, read buffer is up to $49600); $49600 = scratch for 1-sector id read;
  $4C700 = 32 KB save area; **$54E00 = level overlay load address** (see below).
* Strings in image: "TRAINED BY BAMIGA SECTOR ONE", "(c) Copyright 1988,1989 The Bitmap Brothers",
  "INSERT GAME DISK", "LOADING LEVEL 1", "ENTERING SHOP", default high-score table with "BS1" initials, debug-ish
  symbols ("progstart", "shop_right"...).
* Dead / encrypted block: run-time $7104..~$7630 holds trace-vector based decryption machinery (an `illegal`
  handler at $711A, trace handler at $7206 doing `eor.l d0,(a0)` on the next instruction with
  `d0 = swap(not(prev long))`, installing itself in vectors $10/$20/$24) and a block of encrypted code starting at
  $7232. Nothing in the cracked image references $7104/$711A/$71CC from outside the block; the cracker's
  `$70F6` replacement (`d0=$7670CB6B` into the trace vector $24, then branch to normal init) skips it. Treat as
  **dead data**, but see open questions.

## 2. Disc 2 - the "X2B" game disc: raw sector device, no filesystem

* No DOS boot block, no root/bitmap blocks; byte 0..1 = `0000`, **bytes 2..5 = "X2B "** (`58 32 42 20`) is the
  disc identifier. sha1 a748afc7... It is a plain 1760-sector ADF (880 KB) read via the game's own track loader.
* Sector number = linear ADF block number (track*11 + sector, track = cyl*2 + side; i.e. exactly the file
  offset/512 in the ADF).
* Map (derived from the game's table at run-time $91EA, confirmed by scanning the disc):

| sector range | use |
|---|---|
| 0 | id sector ("X2B " at +2; rest of sector 'junk', not parsed) |
| 1-10 | $E5 fill (format fill, unused) |
| 11 .. 239 | level 1 blob, load @ $54E00 (229 sectors) |
| 240-249 | $E5 fill |
| 250 .. 503 | level 2 blob (254) |
| 504-513 | $E5 fill |
| 514 .. 788 | level 3 blob (275) |
| 789-798 | $E5 fill |
| 799 .. 1143 | level 4 blob (345) |
| 1144-1153 | $E5 fill |
| 1154 .. 1465 | level 5 blob (312) |
| 1466-1475 | $E5 fill |
| 1476 .. 1723 | level 6 blob (248; last non-zero sector is 1676, rest zero) |
| 1724 .. 1759 | zero |

* Table at run-time **$91EA** (pairs of words, start sector, sector count; indexed by `level-1` from word at
  `$0E04`): `(0x000B,0x00E5) (0x00FA,0x00FE) (0x0202,0x0113) (0x031F,0x0159) (0x0482,0x0138) (0x05C4,0x00F8)`,
  terminator `FFFF FFFF`. The level-6 entry doubles as variables `$91FE`/`$9200`, which routine `$9034` also reads
  (load of "last" blob into $54E00).
* Each blob = loaded at $54E00, then `jsr $54E00`. Blob content: first longs are address constants that happen to
  decode as harmless `ori.b #x,d5/d6` instructions, followed by `bra.w` jump-table slots (code is entered at its
  start), i.e. blob = level-specific tables + code + data (pointers in $5xxxx..$7xxxx range, so it is absolute and
  position-fixed at $54E00). The first long of each blob is saved/restored through `$54E04`/`$8FEE`.
  No packer signatures (PP20/RNC/ICE/ATM5) anywhere on disc 2; entropy is high in places, so the blobs may carry
  an internal/custom compression handled by the level code (NOT yet analysed, see open questions).
* Disc 2 contains no references to disk hardware registers (no DSKLEN/DSKSYNC/CIA PRB access): all disk I/O is
  in the main image.

## 3. Game's trackloader (main image, run-time addresses; add $2FC00 to get the "$3xxxx" depack-image addresses)

* `$921A load_sectors(a0=dest, d0.w=first linear sector, d1.w=count)` -> d0 = 0 ok, $FFFF failure (3 retries,
  retry counter kept in low memory word `$0002`). It copies from the staging buffer `$44000`, one track at a time:
  `d0 / 11` = track, remainder = first sector in track, copies up to the end of the track, then continues with the
  next track (`$9204` = current track).
* Drive control (`$92C8..$9300`): DF0 only. Motor+select via `$BFD100`: `$FF, $7F, $77` (+ ~15 ms delay),
  deselect `$FF,$87,$FF` at end. Side via PRB bit 2 (odd track = upper head, `bclr` /SIDE), direction bit 1,
  step bit 0 (pulse low/high with `$5960` iteration delay). Recalibration at `$9308`: step outward until TRK0
  (`btst #4,$BFE001`) up to 255 times; `$9202` = current cylinder (0 after recal). Seek moves
  `|cyl - $9202|` steps, capped at $52.
* Track read `$93F8` (runtime; `$38FF8` in depack image): `DSKLEN=$4000` (clear), `DSKPTH=$45600`,
  `INTREQ=$1002`, `ADKCON=$6600` (clear precomp/wordsync/msbsync) then `$9100` (set MFMPREC | FAST), then
  `DSKLEN=$A000` written **twice** (DMA read, length $2000 words = 16384 bytes). **WORDSYNC is off and
  DSKSYNC is not programmed**: the full 16 KB (more than one revolution of ~12.6 KB) is captured from an
  arbitrary rotational position. Completion: level-1 interrupt at vector `$64`=`$95E4` clears INTREQ and sets
  byte flag `$9206`; the loader spins on that flag.
* Software sync (`$951E`): scans the raw words for `$5555`/`$AAAA` filler, then compares the following long
  with a 16-row table at `$95A4`/`$95C4` of the `$4489,$4489` sync pattern shifted by 0..15 bits, realigns the
  whole $2000-word buffer by that bit shift into `$45600`.
* Sector decode (`$957E`, odd/even MFM combine with `$55555555` masks): after skipping the `4489` words reads the
  header (4 bytes info: `$9208` format, `$9209` track, `$920A` sector number 0-10, `$920B` sectors-to-gap), skips
  the 16-byte label (+0x20 header checksum..), decodes 512 bytes of data into `$44000 + sector*512`, marks
  `$920E+sector` bit 7 (duplicate-sector abort -> retry), and continues until all 11 sectors seen. **Neither header
  checksum nor data checksum is verified**; format byte, track number and the label are ignored. When
  `$920B == 1` it re-syncs after the gap.
* Disc identity check `$8F9A` (`bsr` from `$9014`,`$909E`,`$9194`): (first saves the $44000 staging buffer to $4C700 if `$406 != $44000`) reads sector 0 to `$49600` and tests
  `cmpi.l #'X2B ',$49602`; on mismatch shows "INSERT GAME DISK" in a loop (`$8C68`/`$8C98`) until it matches. So
  after boot from disc 1 the game asks for disc 2; disc 1 is never read again by the game.
* Callers of `$921A`: `$8FD2` (disc id, 1 sector), `$9046` (last table entry blob), `$90E8` and `$91D4` (blob for
  level `$0E04`), via helper `$917E` (called from `$7852`, `$7D3E`; skips the load when `$8F98 == $0E04`).
  During loads the game temporarily switches the VBL vector `$6C` to `$2098` and back to `$20C2`.
* Loading indicator: writes a digit 0-9 (progress) into the "LOADING LEVEL n" string via `$37B24`.

## 4. Protection notes

* Disc 1 / disc 2: standard 11-sector MFM, no long tracks, no weak bits, no sector labels/checksums checked, no
  timing reads (no DSKBYTR, no CIA timers used by the loader), no custom sync - **the loader is protection-free
  as shipped on this crack.** A reimplementation can serve sectors from a flat 880 KB file.
* Residual / removed protection: the decryptor at run-time $7104-$7630 (see above) is the remnant of the
  original track/key protection; the cracker diverts around it. The trainer menu patches (flag defaults = on) give:
  infinite-lives-like branch patch @ $5D84, NOP block @ $7B1C, and a value patch @ $7846/$784C.
* Anti-tamper: the SWITCH text checksum-ish check ($7000E) -> reset. Irrelevant if the stub is skipped.
* Vectors $8-$3FF all point to $7032 (any unexpected exception restarts the game).

## 5. Facts needed by a runtime disk shim

1. Only hook point needed: `load_sectors` at **$921A** (a0 = dest, d0.w = start sector, d1.w = count;
   return d0.w = 0). Replace the body with `memcpy(dest, disc2 + sector*512, count*512)`; return 0. It is called
   with the VBL vector swapped to `$2098` (harmless) and from the main loop, not from interrupt context.
2. Disc-id check `$8F9A`: must return Z/OK; shim answer = "disc 2 present" (sector 0 bytes 2..5 = "X2B ").
   Shim disk-state fields `$9202` (cyl), `$9204` (track), `$9206/9207` (flags) are loader-private and can be ignored.
3. No other component touches DSKPTH/DSKLEN/DSKSYNC/ADKCON-disk bits or CIA PRB drive lines; ADKCON `$00FF` writes in
   the audio code are audio-modulation clears. The `$BFD100` writes at `$76C6` and `$72C6` (`$FF,$87,$FF`) just
   deselect drives at init.
4. Level load table: see section 2, load address always **$54E00**; sectors 1-10 and the ten-sector gaps are fill.
5. Disc 1 is only needed for bootstrapping: a recompiled build can start from `x2-image-0400.bin` at entry $400
   (state: SR=$2700, SP irrelevant - `$763A` sets SP=$435FC). Initial chip RAM must be all zero outside the
   image; image must be placed at $400 with 0x3FC00 bytes; bytes $6FC00..$7064A in the depacked file (stub and
   tail, 1 K) are not part of the run-time image.
6. If the trainer is wanted, apply on the $400 image: byte $5D84=$60, words $7B1C/$7B1E/$7B20=$4E71, plus the
   flag3 word patches at $7846/$784C (value = `[$7023A]`, +$30 at $784C).

## 6. Open questions

* The six level blobs on disc 2 have not been decoded: is there an internal decompressor (levels use up to 345
  sectors ~ 176 KB, which would exceed the ~$54E00..$6FFFF window if unpacked)? Check what `jsr $54E00` does.
* Real extent of data in the encrypted block ($7232 on) and whether any later code re-enables the trace
  decryptor (e.g. a periodic key check) - needs execution tracing in an emulator (FS-UAE/WinUAE) or Ghidra.
* Whether the (uncracked) original disc used long tracks/key-disk is unknown; this release no longer contains it.
