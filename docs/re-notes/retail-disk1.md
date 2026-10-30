# Retail disk 1: layout, protection, and how it relates to the canonical (BS1) image

Part of the disk-variants work (phases P2 and the analysis half of P3). Findings only: no game bytes appear here
or anywhere in the repository. Tools: `tools/ipf.py` (IPF reader, MAME-derived), `tools/mfm.py` (Amiga MFM decoder),
`tools/copylock.py` (Copylock key-track detector), `tools/retail_game_image.py` (extract and compare the game image),
`tools/test_disk_formats.py` (synthetic-fixture tests). Attribution for the MAME-derived part is in
`THIRD-PARTY-NOTICES.md`.

## Images examined

| Image | Format, size | sha1 of the file | What it is |
|---|---|---|---|
| SPS 0297 disk 1 | IPF, 1,049,304 B | `72cf591760e014b53e946bf9603ae71b3d21d0d8` | retail original, first pressing |
| SPS 0297 disk 2 | IPF, 1,049,612 B | `571b9c323dc251d9c00ee5dbf5b6a2603e4de683` | retail original disk 2 |
| SPS 2874 `[a]` disk 1 | IPF, 1,049,304 B | `11e95a503a80f32a7b3f0fda9ae532ae61552d86` | retail original, second pressing |
| SPS 2874 `[a]` disk 2 | IPF, 1,049,612 B | `5032c1bc0680000c67393bbca6ee01adeeaf34c3` | retail original disk 2, second pressing |
| SPS 2234 (The Power Pack) | IPF, 1,129,113 B | `c06f8a4206b839663457192c7564f9306f8980e1` | single-disk retail compilation |
| WHDLoad `Xenon2_v1.9_2Disk_0297` (Wepl, installer 1.9) | zip | - | its `Slave` is read for the loader flow |

All IPF records and data payloads carry CRC-32s; `tools/ipf.py` verifies every one and every image parsed cleanly.

## Gate G2: disk 2 decodes to our disk 2

IPF to Amiga MFM to ADF, 160 tracks, every track 11 good sectors (header and data checksums valid):

- SPS 0297 disk 2 decodes to an ADF with **sha1 `a748afc7a142335a40cc46fb7785dea820770241`**, which is the
  canonical disc 2 (BS1 `[t+3]` disk 2) exactly.
- SPS 2874 `[a]` disk 2 decodes to the **same** sha1: the two pressings share disk 2.

So disk 2 is clean retail in every source examined, and the IPF reader plus MFM decoder are correct for standard
tracks. Retail disk 2 is a plain 880 KB sector image with no protection.

## Retail disk 1 layout

Decoded with the same tools. SPS 0297 disk 1 gives an ADF with sha1
`22a419836de21a06ab947102111e11a11020281f`, **the same sha1 as the `Disk.1` in the WHDLoad 2-disk v1.9
install**, which is therefore just this decode (the protection track as zeros). SPS 2874 `[a]` disk 1 gives
`894a73e212a845cd3918c9b8b379fd8de8c7421d`. The two pressings differ in 74 sectors (see below).

| Tracks | Content |
|---|---|
| 0 (cyl 0 head 0) | boot block: `DOS\0` header, checksum valid, **custom track loader** (not an OFS boot). Its root block at sector 880 is a decoy: a plausible root with junk names and failing checksums. There is no usable file system |
| **1 (cyl 0 head 1)** | **Rob Northen Copylock key track** (IPF track type 3, 100,048 cells with 1890/1990/2090 ns cells; the second pressing has 100,351). 11 sectors, none with the Amiga `0x4489` sync, so a standard MFM decode finds nothing: the ADF has zeros here |
| 2 to 79 | the **loader image**, $6B6C0 bytes loaded raw to $400 by the boot block (a flat copy: 11 sectors x 512 per track, 79 tracks) |
| 80 to 126 | the **game image**, $3FC00 bytes: ADF offset $70C00. Exactly the size of the canonical image |
| 160 to 167 | unformatted/noise (cyl 80-83), not part of an 880 KB disk |

Boot block behaviour (disassembled): relocates itself, then reads tracks 2 upward with raw disk DMA (no word sync),
finds the MFM alignment itself, decodes the odd/even longs by hand, and ends with `jmp $400`. It never uses
trackdisk or the AmigaDOS file system.

### The Copylock key track (verified)

`tools/copylock.py` on cylinder 0 head 1 of both pressings: the eleven sync words `8A91 8A44 8A45 8A51 8912 8911
8914 8915 8944 8945 8951` in the documented order, index bytes 0-10, the `Rob Northen Comp` signature at the start of
sector 6's data, and the 512 data bytes of sector 0 match a 23-bit LFSR whose seed is **0x36FA43** (first pressing).
Cell times 1890/1990/2090 ns are present, i.e. the speed-change checks of the new-style Copylock. This matches the
public description of the format (disk-utilities `copylock.c`), so the protection is **confirmed as Copylock**.

## Where the protection lives

1. **In the loader image (tracks 2-79), not in a standalone executable.** The image begins with a Copylock stage:
   anti-trace code (vector fiddling, `ILLEGAL` used as a branch, a `MOVEC` probe for 68010+), then a chain-decrypted
   body. After the first decryption pass the real loader is plain: hardware set-up, copper, interrupts, a disk
   routine, and the Copylock check at about $63E.
2. **The key.** The WHDLoad 2-disk v1.9 slave bypasses the hardware check by supplying a constant key
   (`$7670CF6B`, rotated and `eori #$400` before the first pass) for both pressings. The first pass is a byte chain
   (each byte XORed with the previous plain byte) run forwards up to the entry point; reproducing it under Musashi with the key
   from the slave gives plain loader code. This was reproduced here (stage 1 only); the rest of the loader was not
   needed.
3. **Inside the game image.** The game image has its own check call at **$070F6** (a 14-byte Copylock-style
   register-saving stub) and a check constant at **$076AE**. These are among the bytes the BS1 crack replaced
   (below). So the game code itself calls the key routine; it is not only the loader.

The importer does **not** need to run any of this: the game image is stored on the disk and is recoverable with
arithmetic alone (next section). The question for the runtime is only what to do at $070F6 and $076AE.

## The game image: recovering it, and what differs from the canonical image

The game image on disk is **encrypted by a 16-bit-word XOR chain**: `stored[k] = plain[k] ^ plain[k-1]`, with
`plain[-1] = $002A`. It needs no key and no emulation. `tools/retail_game_image.py` implements it
(`chain_decode`/`chain_encode`; inverse of each other, tested).

Compared with the canonical image (`game_0400.bin`, BS1 `[t+3]`, sha1 `1d37b0142da49bd8a1c9bb03fe6a438ecad9fab4`):

| Retail pressing | sha1 of the decoded image | Differs from canonical in |
|---|---|---|
| SPS 2874 `[a]` | `fc84cc6f1ed47e3da0d5dc913632c705140f1e0a` | **200 bytes, 5 ranges** |
| SPS 0297 | `8d02d25be543d21d3545b321ec774b41afc9d7e3` | 694 bytes, 6 ranges |

The canonical image is therefore **the second pressing's game image with a small crack applied**, and is otherwise
bit-identical to retail. This answers `sources.md` open question 1. The ranges (game addresses, image base $400):

| Range | Bytes | In 2874 | In 0297 | What it is |
|---|---|---|---|---|
| $070F6 | 14 | differs | differs | retail Copylock call stub; BS1 loads the key into d0 and stores it at a vector instead. **Protection removal** |
| $076AE | 4 | differs | differs | a check constant (retail `eori.w`-style, BS1 `move.w` immediate). **Protection removal** |
| $0874A | 35 | differs | differs | on-screen text: BS1 changed the credit string ("TRAINED BY"). **Cracker text** |
| $0894A | 146 | differs | differs | on-screen text block: BS1 credit ("BS1") in place of retail digits/placeholders. **Cracker text** |
| $093F0 | 1 / 396 | 1 byte | 396 bytes | the disk-loader routine area. In 2874 one byte; in 0297 a different routine (delay loops and raw reads) from the one 2874 and BS1 share |
| $01F8A | 99 | same as canonical | differs | in 0297 this is code; in 2874 and BS1 it is ASCII text. A **retail revision difference**, not a crack |

So between the two retail pressings the game differs in two places ($01F8A and $093F0, 456 bytes in all); BS1's
crack touches only 200 bytes, 18 of them logic.

Consequences for the importer (to be implemented, not done here):

- A retail import can produce the game image **with arithmetic only**: read the 3FC00 bytes at disk offset $70C00,
  chain-decode, done. No Copylock emulation, no key.
- Byte-identity with `game_0400.bin` is not achievable for a retail import without applying BS1's 200 bytes, which
  would mean redistributing the crack. Instead the runtime must satisfy $070F6 and $076AE itself (hooks that
  return the key and skip the check), and the retail image gets its own `variant`, `canonical=false`, with its own
  golden hashes. The text ranges are cosmetic.
- The disk-loader area ($093F0) is where the BS1 disk shim interacts: the runtime's existing HLE at $0921A is the
  call into the same routine in retail (the WHDLoad slave patches that same address), so the HLE interface is
  shared; what remains to be checked is whether the 0297 loader variant needs its own entry handling.
- Either pressing is a valid source; the 2874 `[a]` decode is closest to what the runtime already runs.

## Power Pack (SPS 2234, single disk)

Decoded with the same tools: **only track 0 (the boot track, an ordinary 11-sector Amiga track) decodes as
standard MFM; the other 159 do not.**

- Tracks 2 to 159 (158 tracks) are IPF "type 2" tracks with **one raw block each** of about 111,580 cells (a long
  track, 2 us cells), no `0x4489` syncs at all: a **custom track format**, not the Amiga sector format.
- Cyl 0 head 1 is IPF type 6 with **four blocks and 1800/2000/2200 ns cells**: a key track with speed changes (a
  different protection from the two-disk release's Copylock; the WHDLoad notes call this the Speedlock Associates
  one-disk version).
- Disc 2's data is therefore not stored as 1760 x 512 sectors in an Amiga layout that the existing importer could
  read; recovering `disc2.bin` from it needs a decoder for this custom format (a P6 task) and a check of the result
  against sha1 `a748afc7...`.

## Not determined

- The exact semantics of the check at $070F6 and $076AE (how much of the Copylock key the game uses afterwards).
- Whether the 0297 loader variant at $093F0 needs separate handling in the runtime.
- The second loader stages (after the first decrypt pass) beyond what the slave reveals; not needed for import.
- Which game build the other six floppy versions in the WHDLoad slave ship; only these two pressings were read.
- The Power Pack custom track format and its key track.
- Flux behaviour: the Copylock timing (1890/1990/2090 ns) is modelled from the IPF; real flux from a physical
  disk has not been seen.

## Tools

```
python3 tools/ipf.py IMAGE.ipf                         # header and one line per track (type, cells, timings)
python3 tools/mfm.py IMAGE.ipf [--adf OUT.adf] [--tracks]   # per-track decode report; optional ADF
python3 tools/copylock.py IMAGE.ipf [CYL HEAD]         # Copylock key-track detection, signature, LFSR seed
python3 tools/retail_game_image.py DISK1.adf [CANONICAL.bin]   # decode the game image, list differing ranges
python3 -I -m unittest discover -s tools -p 'test_disk_formats.py'   # 15 tests, synthetic fixtures only
```

Tests that need the images themselves are not part of this change; the synthetic tests cover the codecs, the
checksum and CRC rejection paths, the Copylock detector, and the chain cipher.
