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
| 3 | restart at `$40000` with the trace bit set | the real loader, expected to decrypt and unpack the engine | **not reached** |

Reference hash of the emulated memory `[$40A7C, $68000)` at the start of pass 3: sha1 prefix `b69bfbba275e`. The static
layer 2 result over the same range has prefix `19a49b6e0290`; the two differ in 1,718 bytes, the first at `$6760E`.
That span is unexplained (most likely code the loader rewrites after the chain runs, but this was not confirmed).

Two 32-bit immediates in the loader (the handler address and the second body's target, each stored XOR-ed with the key)
are needed to verify the layers. With a known address each implies the disk's key, so they are not committed: the tool
reads them from a local JSON file named by `$X2_ST_COPYLOCK_CONSTS` and skips the layer check without it.
Exception vectors `$8-$27` are used as a register save area, which is why a naive trace-vector handler breaks: at pass 3
vector `$24` holds a value that looks like instruction bytes (`$07BE4879` when `a3` is `$28`).

## 3. What blocked layer 3

After layer 2's `rte`, the loader restarts at `$40000` with the trace bit set and needs a valid trace vector at `$24`.
In the harness `$24` is invalid. Tried without progress: `--reg a3=0x28`, and `--set32` writing `$407A4` or `$40000` into
`$24`. The likely cause is register or low-memory state that a real TOS leaves at boot and the harness does not model;
that cannot be settled without a TOS run or a hardware trace, which were not available. A next attempt could run the
disk under a real TOS 1.x image if one is supplied, compare the registers at `$40000` entry, and `--watch 0x8:0x28`.

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
