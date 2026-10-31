# Retail images: the in-game Copylock check and how the runtime answers it

Part of the disk-variants work (phase P3b). Findings and design only: no game bytes appear here or anywhere in the
repository. Context: `docs/re-notes/retail-disk1.md` (how the retail disc is laid out and how the game image is
recovered), `docs/re-notes/sources.md` (the images). Images are cited by their public identity: SPS 2874 `[a]` (second
pressing) and SPS 0297 (first pressing).

## 1. What the check is

The game image decoded from an original retail disk 1 differs from the canonical (cracked) image in 18 bytes of
protection code, 181 bytes of credit text and, in the first pressing, a code revision and a disk-loader routine
(`retail-disk1.md`). Only one place needs the runtime's attention:

| Address | Retail image | Canonical image |
|---|---|---|
| `$70F6` (14 bytes) | the entry of a Rob Northen Copylock routine: `move.l a6,-(a7)`, `lea x(pc),a6`, `movem.l d0-d7/a0-a7,(a6)`, `lea $40(a6),a6`, then self-decrypting code | a replacement stub: load a constant into `d0`, store it at vector `$24`, `bra.w $763A` |
| `$763A` | the routine's exit, shared by both images (`add.w d0,d1`, set up the stack, the vector table, ...) | same |
| `$76AE` | `eori.w #$5A88,d1` (4 bytes) | `move.w #$95E4,d1` |

The game reaches `$70F6` from two places: the boot entry (`$400: bra.w $7032`, `$7032: move #$2700,sr; bra.w $70F6`)
and every CPU exception, because the game points the whole vector table at `$7032` once it has initialised. In other
words any crash re-enters Copylock, which is the anti-tamper design: a restart from the protection routine.

### What the routine does when run for real

Observed by running the retail image in a scratch build of the runtime with Musashi's trace emulation switched on
(`-DM68K_EMULATE_TRACE=1`; the committed configuration has it off) and reading the instruction trace:

1. The prologue saves every register to a table at `$7080`, replaces the illegal-instruction vector with a handler
   and executes `illegal`, then probes for a 68010+ (`movec`, another illegal instruction).
2. The handler sets the **trace vector** (`$24`) to a second handler and the **privilege-violation vector** (`$20`)
   to `$763A`, sets the T bit in the stacked SR, and returns.
3. From then on every executed instruction raises a trace exception whose handler **decrypts the next instruction in
   place**, using the long before it as the key (`move.l -4(a0),d0; not.l d0; swap d0; eor.l d0,(a0)`). The encrypted
   routine therefore only exists in memory one instruction ahead of the program counter; a debugger, or a CPU
   without trace exceptions, sees garbage. The first decrypted instructions are a chain of some thirty `bra`s that scatter
   the control flow, then the real body.
4. The body relocates a vector table, selects the drive and spins the motor through the CIAs (`$BFD100`), waits for
   the **disk index pulse** (it polls CIA-B's interrupt register for the FLAG pin) and then reads the key track
   with raw disk DMA, measuring the time between sector syncs.
5. The routine ends by executing a privileged instruction in user mode; the privilege-violation vector it installed
   sends the CPU to `$763A` with the registers restored from the table at `$7080`.

So running the check genuinely needs three things the runtime does not have: the 68000 trace exception in the CPU
core (`M68K_EMULATE_TRACE`, off, and absent from the translated code), a floppy drive model (motor, drive select,
step and side lines, index pulses, `DSKSYNC`/`DSKPT`/`DSKLEN`/`DSKBYTR`), and the key track's cell timing from a
flux-capable image. With the trace emulation on and no drive, the routine runs for hundreds of frames (the motor
delay alone is some 600,000 loop iterations, each instruction trapped) and then spins forever on the missing index
pulse. This was confirmed, not assumed. It is a possible route to a fully faithful answer, but it is large, slow in
the interpreter, and unnecessary for what the game does with the result; it is not planned.

### What the game does with the result

Nothing the runtime can observe. After the routine leaves, `$763A` runs `add.w d0,d1`, and `$76AE` then stores
`d1 ^ $5A88` into the low word of **vector `$64`**, the level-1 interrupt autovector (software, serial and
disk-block interrupts). In the canonical image that value is the constant `$95E4`, the address of the game's own
disk-block handler; in the retail image it is derived from the Copylock key and the loader's register state, so a
copy that skips the check without the key gets a wrong handler address. The game enables the disk-block interrupt, but
the runtime replaces every disk read with a function call (`runtime/disk_shim.cpp`, `$921A`) and never raises it, so
the vector is never used. Verified by running with the vector left at the value the zeroed boot registers give
(`$5A88`): every replay is frame-for-frame identical to the canonical image once the credit text is accounted for
(section 3).

## 2. The design

**The importer verifies; the runtime answers.**

- When a retail disk 1 is imported from an image that still carries the key track (IPF today; a flux image later),
  the importer reads cylinder 0 head 1, checks it is a genuine Copylock track and writes the result next to the game
  image as `copylock.txt`, the **receipt**:

  ```
  x2recomp-copylock 2
  signature rob-northen-comp
  syncs ok
  seed 0x36FA43
  image-sha1 <SHA-1 of the game image, 40 lower-case hex digits>
  ```

  The `image-sha1` line binds the receipt to the image it was written for: the runtime hashes the `game_0400.bin` it
  loaded and refuses a receipt that names another image, or an old version 1 receipt that names none (import the disc
  again). `verify_assets` also refuses a `copylock.txt` that the manifest does not record. The receipt is a statement
  by the importer, not a secret: anyone can write a file of this shape, so it protects against stale or mismatched
  files, not against someone who chooses to run a retail image.

  The checks are the ones in `tools/copylock.py`: the eleven sync words in the documented order with index bytes 0 to
  10, the `Rob Northen Comp` signature at the start of sector 6, and the sector 0 data equal to the 23-bit LFSR
  sequence from the recovered seed. A plain ADF, which cannot contain the track, is refused for a retail image.
  `tools/retail_assets.py` is the Python prototype of this import; the native importer (`runtime/import/retail.cpp`) is
  its port and produces byte-identical output (checked on both pressings).
- At start-up the runtime (`runtime/retail_protection.cpp`) recognises a retail image by the Copylock prologue at
  `$70F6` and requires the receipt. With it, a **PC trap at `$70F6`** sets the program counter to `$763A`, exactly
  where the real routine ends up, leaving the registers alone. Without it, the image is **refused** (exit code 3,
  with an explanation), because starting it would crash in the routine. An image without the prologue (the canonical
  one) is never touched, receipt or not.
- The image bytes are not changed. A trap rather than a byte patch also keeps the translated executor happy (section 4).
- **Nothing in the program is the key.** The receipt carries the seed only as an identifier of what was verified
  (reported at start-up); the runtime does not compute with it. The check itself is skipped, not computed: this is
  an honest HLE of a routine that cannot be run, justified by the importer having seen a genuine key track. The same
  idea is used by the public WHDLoad slave for this game, which writes `jmp $763A` over `$70F6` ("third copylock"),
  the difference being that here the image is untouched and the decision depends on the user's own disc.
- What is generic and what is per pressing: the prologue, the exit address and the trap are common to both retail
  pressings (and to every retail image in this family: they share the `$70F6`/`$763A` layout). The receipt's seed is
  the same for both pressings (`0x36FA43`), which the importer checks per disc.

**Opt-in alternative (implemented in P3c):** `--apply-protection-patch --i-confirm-i-own-the-original` applies the
18-byte protection removal (a 14-byte replacement of the stub at `$70F6` and a 4-byte replacement of the constant at
`$76AE`, kept as a data table in `runtime/import/variants.cpp` and checked byte for byte against the retail original
before anything is written) to a retail image whose SHA-1 is a catalogued pressing. The result equals the canonical
(BS1) image except for the credit text (35 + 146 bytes, one loader byte in pressing 2874) and, in pressing 0297, the
code revision and loader routine. The patched image has no stub, so the runtime treats it as plain: no receipt, no trap.
It is never the default and never silent: the disclaimer is printed whenever the option is used, the CLI refuses with
exit code 4 unless `--i-confirm-i-own-the-original` is also given (the import window asks for a second keypress after
showing the disclaimer), a source whose decoded image is not a catalogued pressing is refused, and the manifest records
`patched=true` and the ranges. A patched import is not repeated automatically when the importer version changes: it needs
the confirmation again. Patched imports are their own variant with their own recorded replay hashes
(`tests/replays/retail-patched/<pressing>/`); the canonical replay goldens do not apply to them, nor do the retail ones.

## 3. Evidence

All runs headless, accurate timing, against the interpreter build; hashes are the replay corpus's `framehash` at
500-frame steps (`tests/replays/*.hashes`).

| Experiment | Result |
|---|---|
| Retail image, no trap | the CPU enters the routine and is at PC 0 in the first frames, executing garbage (the decrypt chain does not work without trace exceptions) |
| Same, with trace emulation on | the routine runs, then waits forever for the drive's index pulse |
| Retail 2874, trap at `$70F6`, the **credit text** (`$874A`, 35 bytes) and the one differing loader byte (`$93F0`) restored to the canonical values | **frame hashes identical to canonical in all 19 replays** (every level, the bosses the corpus reaches, shop, ending, game over, two-player, attract) |
| Retail 0297, the credit text restored (its `$1F8A` and `$93F0` differences left as on the disc) | identical in 18 of 19; `level4_boss` differs from frame 3000 on |
| Retail 0297, same, plus the `$1F8A` range restored | `level4_boss` identical too: the pressing-1 code revision at `$1F8A` is a **real gameplay difference** in the level 4 boss, not cosmetic |
| Retail 0297, `$93F0` (396 bytes, a different disk-read routine) restored or not | no effect: the runtime replaces the disk code, so the pressing-1 loader variant needs no handling |
| Retail 2874, the 146-byte block at `$894A` restored alone | no effect on frames |
| `d0`/`d1` at boot varied (0/0, `$7670CF6B`/1 and `$7670CB6B`/`$401`, the last two making the level-1 vector `$95E4`) | the retail 2874 `level1_play` frame **and audio** hashes are identical for all three |

The audio hashes of the retail images never equal the canonical ones, even where every frame does. That is a
boot-time cycle offset, not a logic difference: the canonical stub spends 38 cycles before `$763A` and the retail trap
spends none. Demonstrated by replacing the canonical stub with a different number of cycles (`nop; jmp $763A.w`): the
frame hashes stay identical and all audio hashes move. Retail images therefore have their own recorded manifests
(`tests/replays/retail/<pressing>/`).

### Why the credit text moves every frame hash

Unmodified retail frames differ from canonical from frame 175 on (first the intro's text, then everything), although
the game state is identical until then (a chip RAM diff at frames 120 and 174 shows only the known retail/canonical
byte ranges, plus one pixel byte at frame 174). The credit strings at `$874A` are different text of the same length
("DESIGNED BY..." against the cracker's "TRAINED BY..."), they are drawn by the intro, and the drawing cost depends on the
glyphs, so the **frame at which the intro ends shifts** and the frame-indexed input script then meets the game at a
different moment. Restoring only those 35 bytes removes every difference. This is a property of retail content, not
of the protection.

### `level4_flood`

The `level4_flood` replay restarts through `$7032` once in both retail images (around frame 3400) and not in the
canonical image. That replay is a coverage tool that pokes game state to fire many enemy types at once; with the
retail text the poking meets the game at another moment and provokes a CPU exception, whose restart goes through
`$70F6` as designed (the trap answers it, and the replay then carries on). With the text restored it does not
happen. It is recorded as is in the retail manifests and is not a protection fault.

## 4. The translated executor

`-DX2_RECOMP=ON` builds run the retail images with the existing translation, **unchanged**: the hybrid executor
verifies each instruction's bytes before running translated code. On `level1_play` (5,980 frames) both retail images
execute 55.7 million instructions of which all but **one** run translated; the one is `$76AE`, whose bytes differ from
every translation and which falls back to the interpreter by itself. The interpreter-recorded retail manifests match
the hybrid executor's hashes exactly (checked for `level1_play`, `boss1`, `level4_boss`, `ending_natural` and `shop`,
both pressings). No re-translation from a retail trace is needed. Pressing 0297's `$1F8A` revision is code the
canonical translation does not contain; it executes interpreted. Coverage of the retail-only code (one routine and one
instruction) can be added by tracing the retail images if wanted.

## 5. What the importer does (P3c, done)

- Decode an IPF (later SCP/KryoFlux/HFE) disk 1 into tracks; check cylinder 0 head 1 with the Copylock checks above and
  write `copylock.txt` only on success. For flux input the decoder must keep the 11-sector long track (no `0x4489`
  syncs) and reconstruct the sector data by the sync words; real flux has not been seen yet.
- Recover the game image at disk offset `$70C00` with the word-XOR chain (`tools/retail_game_image.py`), record the
  pressing (by the decoded image's sha1: `fc84cc6f...` for 2874, `8d02d25b...` for 0297) and produce `disc2.bin`
  from disk 2 (SPS 0297 and 2874 disk 2 are the canonical disc 2, `a748afc7...`).
- Manifest: `variant`, `canonical=false`, the pressing, and that the receipt was written.
- Refuse a retail image from a source that cannot carry the key track, with a message naming the formats that can.
- `ensure_imported` must treat `copylock.txt` as part of the import (it belongs to the manifest's file set).

## 6. Limits, stated plainly

- The check is **skipped, not executed**. If a future change makes the runtime raise disk interrupts or read a real
  disk, the level-1 vector (`$64`) must be set to the game's handler (`$95E4`); the registers at boot would then have
  to satisfy `d0.w + d1.w = $CF6C`. Nothing in the runtime depends on it today.
- The importer cannot compute the key the real routine returns (the relation to the track's LFSR was searched for and
  not found by simple means: windows of the LFSR stream, byte/word/bit alignments, rotations and xors of the value the
  public slave uses). It does not need to.
- Only IPF decodes of the two SPS pressings were available; flux decoding of a physical disc is untested.
- The first-pressing `$1F8A` revision and the cosmetic credit text are retail content: retail imports are
  `canonical=false` and have their own goldens.
- Not examined: the other WHDLoad versions (v4 plain one-disk, v5 PDos, v6 Speedlock) use different protections and
  loaders.

**The native import (P3c).** `x2recomp --import DISK1 DISK2` with an IPF, SCP, KryoFlux stream set, HFE or extended ADF of
disc 1: `verify_copylock_track` (`runtime/import/retail.cpp`) checks cylinder 0 head 1 (the order check is circular, so
an index pulse anywhere in the track is fine, and flux streams that run past one revolution are handled; the signature
and LFSR checks look at every copy of a sync word), `decode_retail_image` reads the 510 sectors at `$70C00` and undoes the
word XOR chain, the pressing is identified by the SHA-1 of the image (`variants.cpp`), and `write_assets` writes the
image, disc 2, `copylock.txt` and the manifest (importer version 3: the receipt names the image). Retail disc 1 from a plain ADF or DMS is recognised by
its game image (the decoy volume at block 880 is checked after that, never before) and refused. Unknown retail images
that pass the key-track check are imported with a warning; the patch option refuses them. `--diagnose-image` reports all of
this per image. Limits that remain: the key-track check cannot see the cell timing (1890/1990/2090 ns) that the IPF holds
and a flux decode only approximates; flux dumps of a physical disc have not been seen yet.

## 7. Tools and tests

```
python3 tools/retail_assets.py DISK1.ipf DISK2.(ipf|adf) --out DIR     # game_0400.bin, disc2.bin, copylock.txt
x2recomp --assets DIR ...                                              # the receipt makes it run
python3 -I tools/trace/run_corpus.py --exe build/x2recomp --assets DIR --record-hashes --no-trace \
        --hashes-out tests/replays/retail/<pressing>                  # record a manifest set
ctest -R retail                                                        # retail_protection (unit), retail2874_*, retail0297_*, retail_without_receipt
```

`retail_protection` needs no game data (a synthetic image with only the prologue opcodes). The retail replay and
receipt tests exit 77 (Skipped) without `X2_RETAIL_2874_ASSETS` / `X2_RETAIL_0297_ASSETS`. `retail_import` (synthetic
key tracks, discs, extended ADF, SCP and HFE sources, the refusal paths, the patch option, the manifest) needs no data;
`retail_import_2874` / `retail_import_0297` import a real disc pair (`X2_RETAIL_<pressing>_DISK1/DISK2`) and check the
image sha1, disc 2, the receipt, the retail replay hashes, `--diagnose-image`, the patch refusal (exit code 4) and the
patched image's sha1 and replay hashes.
