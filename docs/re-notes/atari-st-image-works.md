# Atari ST Image Works release: Copylock ST layers

Analysis only. Nothing here is built into the runtime and no game bytes are committed: this note gives sizes, hashes and
measurements; every extracted file lives in the git-ignored `generated/sources-extra/atari-st/out/`. The images are the
public Atarimania dumps listed in `docs/re-notes/sources.md` (Image Works 1989: two single-sided Pasti `.stx`).
It continues `atari-st-analysis.md` section 1, which stopped at the second Copylock layer.

**Result in one paragraph.** Layer 1 and most of layer 2 of Rob Northen's Copylock ST are now understood and reproduced
statically in `tools/acq/stcopylock.py` (layer 1 matches the emulated memory in 161,294 of 161,294 bytes; layer 2 in
159,438 of 161,156 bytes, 98.93%). **Layer 3 (the real loader) and the game image were not reached**, so there are no
decoded loader or game images, no hash table of them, and no Image Works against Kixx instruction-shape comparison. The
only Image Works against Kixx figures are raw byte-window counts on the still-encrypted disk (section 5). Section 6 lists
the protection-handling options an importer would have.

## 1. Tools

| Tool | What changed |
|---|---|
| `tools/acq/stcopylock.py` (new) | static model of layers 1 and 2: key derivation, handler addresses, the two XOR layers, the trace formula; `python3 tools/acq/stcopylock.py DISK1.stx` prints the verifiable facts without writing any bytes |
| `tools/acq/stemu.c` | added a TOS-style `rte` stub at vector `$70`, `--itrace FILE:LO:HI:N[:FROM]` (distinct instructions with registers), `--reg NAME=VALUE`, `--set32 PC:ADDR:VALUE[:AFTER]`, `--watch LO:HI` (write log), an exact `--max`, `STEMU_REGS=1` |
| `tools/acq/test_st.py` | 11 new synthetic tests (7 for `stcopylock`, 4 for the new harness options); 25 tests in the file; the 6 harness tests skip themselves without a Musashi build tree |

Typical run on a raw `.st` image built from the `.stx` with `stx.raw_image`: `stemu DISK1.st --boot-at 0x10000 --bad-sector 0:6 --slow-sector 0:6:10 --vbl 90000 --max N`.
The harness satisfies the protection check by modelling the disk (bad CRC, slow sector), never by patching the loader.

## 2. Layer structure

| Layer | Where | What it does | Status |
|---|---|---|---|
| 0 | boot sector | reads stage 1 (tracks 1-32, 163,840 B, sha1 prefix `9f521c79d3e6`) to `$40000` and jumps to it | done |
| key | track 0 sector 6 | 512 B, bad CRC, "Rob Northen Comp" (sha1 prefix `6f4285e90259`); the sector must also read about 2% slow | done |
| 1 | stage 1 at `$40000` | reads the key sector, derives the 32-bit key (six rounds of `add.l; rol.l #3` over its first 24 bytes), installs a trace-vector handler at an address that is the key XOR a constant (`$67CFE`), and re-enters `$40000` with the trace bit set | done |
| 1 body | `[$406F0, $67CFE)` | the handler decrypts it with a cumulative XOR seeded `(key & $FF) ^ $B8` and stores the result at `$40000`: 161,294 B, sha1 prefix `5001f2ef00b4` | done, 100% vs emulation |
| 2 | decrypted body | patches its own instructions with `eor.l d0,<addr>` (key-dependent), places a second handler at key XOR a constant (`$407A4`), runs a second cumulative XOR in place (byte i+1 ^= new byte i) over `[$40A7C, $FFFFC)`, then re-enters `$40000` | done to 98.93% |
| 3 | restart at `$40000` with the trace bit set | the real loader, expected to decrypt and unpack the engine | **not reached**; blocked on the trace vector (section 3) |

Reference hash of the emulated memory `[$40A7C, $68000)` at the start of pass 3: sha1 prefix `b69bfbba275e`. The static
layer 2 result over the same range has prefix `19a49b6e0290`; the two differ in 1,718 bytes, the first at `$6760E`.
That span is unexplained (most likely code the loader rewrites after the chain runs, but this was not confirmed).

Two 32-bit immediates in the loader (the handler address and the second body's target, each stored XOR-ed with the key)
are needed to verify the layers. With a known address each implies the disk's key, so they are not committed: the tool
reads them from a local JSON file named by `$X2_ST_COPYLOCK_CONSTS` and skips the layer check without it.
Exception vectors `$8-$27` double as a register save area and as the stack: stage 1 runs with `a7` at `$28` while it
installs a handler, so a push lands on a vector. Layer 1 uses exactly that idiom: `movem.l d0-d7,$8` saves the registers
(so `d7`, the key, lands on the trace vector `$24`), and a later `eori.l #const,$24.w` turns it into the handler address.

## 3. What blocks layer 3 (measured, 2026-11)

**Where the layers meet.** The tail of the layer 2 handler (`$40788-$407D6`, entered through the trace exception, so it runs
with `T` off) does this, in order: clears two 32,000-byte screen buffers at `$70000` and `$78000`; patches its own code
with `eor.l d0,<operand>` (the key); derives an address from the key (`$407A4`) and patches the word there; then builds a
return frame with `pea $40000`, duplicates the word on top of the stack and toggles the trace and supervisor bits in the
copy (`eori.w` on `(a7)`), loads `d0-d7` from a table (`$40A5C`, palette-like words) into the Shifter palette and executes
`rte`. That `rte` re-enters `$40000` with `SR` `$A304` (trace on). `$40000` is then plain code (`lea`, `move #$2700,sr`,
timer and vertical-blank setup, a screen base, then a computed return into the next stage), not encrypted code.

**The blocker.** The first instruction after that `rte` is traced, so the 68000 takes a trace exception through vector
`$24`. Nothing in the layer 2 tail writes `$24`; it still holds the key from the register save (so the exception jumps
to the key, an address outside ST RAM). The only instruction in the tail that looks as if it might install a vector is
`move.l ($10,pc,d2.w),-(a3)` at `$407A4`; it is the self-patched instruction (the word patch makes it that), and it needs
`a3=$28` and `d2.w=$FFFE` to store the longword at `$407B4` (the absolute operand of the following `eori.l`, which is
`$000407BE`) into `$24`. The loader sets `a3=0` (its register table) and `d2=0` (a `moveq #0,d2` two instructions
earlier), so as emulated the store goes to `$FFFFFC` and the vector stays the key. Forcing `a3`/`d2` to those values does
install `$24=$407BE`, but the bytes there are then a computed longword (`d0` xor a constant) that decodes to a branch and an
`addq`, and the next trace exception branches to an odd address. So this reading of the instruction is not it either.

**Experiments, all negative** (`tools/acq/stemu` with the new `--setreg` and `--preload`):

- Real machine state. The registers at `$40000` entry under EmuTOS 1.4 (Hatari, `.st` copy of disk 1, breakpoint at
  `pc=$40000`, `savebin` of `$0-$FFFF`) are the same as the harness's (`d0=$21 d1=1 d2=$FFFF a0=$40000 a3=0 a4=$11CA
  a5=$FFFF8604 a6=$FFFF8606`), and preloading the real EmuTOS vectors and system variables (`--preload`) lets layer 2 reach
  the same tail with the same result, so the missing state is not TOS's.
- Candidate trace vectors written at the pass 3 entry (`--set32 0x40000:0x24:VALUE:AFTER`) for `$407A0 $407A4 $407A8 $407AE
  $407B8 $407BE $407C2 $40788`: every run leaves the loaded code within a few thousand instructions and wanders into
  unmapped space; none reads another sector.
- Musashi's address-error emulation is off in the harness. It was not tried: the only odd-address branch seen occurs in the forced-register run, which is already a wrong reading, so it is listed here as an untested variable.

**Conclusion.** With the images available, layer 3 cannot be reached by running the code in `stemu`: either the harness
misses a hardware behaviour the tail depends on (a bus error, the trace-exception frame details, the Copylock sector
timing outside the 2-5% window, or the kind of read the loader issues) or the straight reading of the tail above is
missing a step. Telling these apart needs one ground-truth run of the original `.stx` on a machine model that implements
the Pasti sector (bad CRC, timing, fuzzy mask) and the 68000 faults, for example Hatari built with the CAPS image library
(the build here has no STX support; fetching the library source was not possible in this session) or a hardware trace.
The two numbers to read from it are the value of vector `$24` and the registers `a3`/`d2` at the first trace exception
after the second `rte` to `$40000`; the harness comparison is `stemu ... --watch 0x20:0x28` and
`--itrace FILE:0:0x1000000:N:FROM` around that point.

Next step, in order: (1) build Hatari with CAPS support and boot `Xenon II - Megablast (Disk 1 of 2).stx` under EmuTOS with
a debugger script (`b pc = $40000`, `r`, `m $0-$40`, `c`), which prints the register and vector state at each of the three
entries to `$40000`; (2) feed what differs into `stemu` with `--setreg`/`--preload`; (3) once pass 3 runs, dump the engine
and write the documented algorithm into the importer as option 4 below.

## 4. Hashes (sha1 prefixes, 12 hex digits)

| Item | Size | sha1 prefix |
|---|---|---|
| Key sector (track 0 sector 6) | 512 B | `6f4285e90259` |
| Stage 1 (tracks 1-32) | 163,840 B | `9f521c79d3e6` |
| Layer 1 body | 161,294 B | `5001f2ef00b4` |
| Emulated `[$40A7C, $68000)` at pass 3 | 161,156 B | `b69bfbba275e` |
| Static layer 2 over the same range | 161,156 B | `19a49b6e0290` |
| Decoded loader, unpacked game image | | not available |

## 5. Image Works against Kixx (raw only)

Layer 3 was not decoded, so the instruction-shape windows of `stshape.py` could not be run on the Image Works engine.
What was measured on the raw (still encrypted where encrypted) disk images:

- Disk 1 tracks 0-35 are high entropy (encrypted); tracks 36 onward look like data. No RNC ProPack signature appears in
  either disk's raw bytes, so unlike Kixx the payload is not stored as plain RNC blocks.
- 24-byte windows of disk 1 that also appear in the Kixx resident engine (235,520 B): 1,393 of 99,812. Disk 2: 0 of 102,274.
  The disk 1 hits were not located by track, so this does not show which part of the disk is shared.
- Therefore the earlier claim that the Image Works engine is the Kixx engine remains an inference from the credits and
  the Kixx, CDTV and Amiga comparison in `atari-st-analysis.md`, not a measurement on Image Works code.

## 6. Protection handling options for an importer

An importer for this release has to decrypt the payload, and the decryption is keyed from track 0 sector 6.

1. **Pasti `.stx` is required.** Only STX keeps the bad CRC flag and the long-sector timing. `.st` and `.msa` copies cannot
   carry the CRC state or timing, so the original loader would fail its own check on them; whether the 24 key bytes
   survive depends on the imager and was not checked. Cracked or already unpacked releases are a separate case.
2. **Receipt plus runtime trap (the Amiga retail Copylock model).** The importer reads the key sector from the user's own
   `.stx`, derives the key on their machine and stores a small receipt in their data folder; the runtime answers the
   loader's check from it. For a recompiled ST target the loader is replaced by the import step, so the trap would only
   matter if the original loader code were run. This option is available once layer 3 is understood.
3. **A documented patch of the check**, as with the opt-in 18-byte Amiga patch. Not worked out: the check lives in
   trace-encrypted code (the first 4 bytes of each instruction depend on the previous raw longword), so any patch
   must re-encrypt neighbouring bytes. It would also have to stay opt-in with a user-verification step, like the Amiga one.
4. **Emulated import**: run the user's `.stx` in `stemu` (already in the tree, Musashi based) up to the point where
   the engine is in memory, dump it, and unpack or relocate it. This works from the `.stx` alone and needs no patch, but
   needs layer 3 to run to completion (section 3).
5. **Offline static decryption** with `stcopylock.py` plus the layer 3 decoder: fastest at import time and independent
   of an emulator, but needs layer 3 decoded first.

Options 2, 4 and 5 all depend on the work that remains. The Kixx build (unprotected, RNC packed) remains the cheapest
supported ST import.
