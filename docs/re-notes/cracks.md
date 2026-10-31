# Cracked and re-packed variants: what they hold and how to recover the canonical inputs

Part of the disk-variants work (phase P7, analysis half). Findings only: no game bytes appear here or anywhere in the
repository. Images are named by their TOSEC name and SHA-1 (`docs/re-notes/sources.md` has the catalogue). The recipes
are in `tools/variants/`; the tests are `tools/variants/test_cracks.py` (synthetic fixtures only).

"Canonical" means the pair the importer accepts today: `game_0400.bin` (sha1 `1d37b014...`) from the BS1 `[t+3]` disk 1
and `disc2.bin` (sha1 `a748afc7...`) from disk 2.

## 1. Results at a glance

| Variant (sha1 prefix) | What it is | Game image | Disc 2 | Bytes differing from canonical | Behaviour (replays, credit text restored) | Verdict |
|---|---|---|---|---|---|---|
| BS1 `[t+3]` (`45e66f9c`, `268e8b52` `[a]`) | the supported trainer set | existing importer | standard disc 2 | 0 (this is canonical) | - | supported today |
| BS1 `[h Deejay99]` (`d6e1a7ba`) | UAE-fix trainer | existing importer | its own disc 2 (26 bytes differ) | 0 | - | supported today |
| BS1 `[h Timecircle]` (`cb366028`) | BS1 trainer with another menu | **identical**: same `XENON-II` (sha1 `778cf75e...`), the C++ importer already produces the canonical image (it only warns "unrecognised") | standard disc 2 | 0 | identical | **add its sha1 to the known table** (no code) |
| PNA (`2028f31a`) | **cracked retail disk 1** | plain at `$70C00` | standard disc 2 (not on this disk) | 181 (credit text only) | identical | easy |
| PNA `[a]` (`12b7bd53`) | PNA + a trainer patch | as PNA | standard | 185 (text + 4 bytes at `$7B00`: infinite ships) | diverges at the first death (frame 3500 of `level1_play`) | supportable, flagged non-canonical |
| Quartex QTX (`2b98cbba`) | **cracked retail disk 1** | plain at `$70C00` | standard | 226 (text 220 + 6 at the protection sites) | identical | easy |
| Flashtro +16 (`a3574a21`) | cracked retail disk 1 (the **first** pressing) | plain at `$70C00` | standard | 936 | identical | easy, flagged non-canonical |
| Black Monks (`0df98225`) | cracked retail disk 1, tail left chain-encoded | plain body + a tail repair (see 3.4) | standard | 195 (incl. a trainer hook at `$7913`) | `level1_play`, `ending_natural` identical; boss 1 and level 2 diverge (the hook) | supportable, flagged non-canonical |
| Predators +9 (`2405da96`) | cracked retail disk 1 with a trainer entry | plain at `$70C00` | standard | 568 (224 bytes of entry code replaced) | the runtime refuses it (`unexpected entry code`) | **unsupported** (needs the entry restored) |
| Flashtro +16 `[a]` (`1b4855f9`) | cracked retail disk 1, transformed image | not the plain image nor the retail chain | standard | - | - | **unsupported** (not analysed further) |
| Vision Factory / ONYX (`8f50b895`; the TOSEC `[cr PNA][h VF][t +1 VF]` is the same disk) | **real one-disk crunch** | crunched snapshot (ONYX cruncher) | six crunched files = the standard disc 2 minus sector 0 | 1101 (text, loader routine `$921A`, 10 bytes) | identical | supportable (needs a native ONYX decruncher) |
| VF `[a]` (`83e19d34`) | same six data files, exe crunched with an Ice-style cruncher | snapshot via Ice depack | same | 1109 | identical except one frame near frame 500 | supportable (needs a native Ice depacker) |
| Black Monks `[h][t +36]` (`1181e872`) | same six data files, ByteKiller exe, trainer menu build | snapshot via the existing BS1 ByteKiller depacker | same | 2009 (trainer hooks) | diverges at frame 1000 | supportable, flagged non-canonical |
| BS1 single-disk `[+3]`, Defjam, Flashtro +17, PNA `[a2]`/`[h Divinity]`/`[t +6 DOM]`, QTX `[h QS][t UA]`, Subway +34 | listed by TOSEC | **not obtained** | - | - | - | cannot be analysed; the DAT lists them (`sources.md` V10b) |

"Behaviour" columns come from the replay suite run against a recovered image whose credit-text bytes (`$874A..$876D`,
`$894A..$89DC`) were put back to the canonical values: without that, **every** variant fails the first frame hash at
frame 500, because the credit text is drawn on an early screen. That is the only difference for PNA, so the text is a
presentation difference, not a behavioural one.

## 2. Two corrections to the earlier notes

1. **PNA, PNA `[a]`, QTX and Black Monks are not single-disk games.** They are the retail disk 1 with the Copylock
   removed: the boot block still loads a loader image from tracks 2-79 (`$6B6C0` bytes to `$400`), the game image still
   sits at disk offset `$70C00`, and the hidden "Copylock Amiga Key Disk" is the retail disk's decoy root block
   (`retail-disk1.md`). The "(Disk 1 of 2)" in their TOSEC names is right: the game still reads the standard disc 2.
   Compared sector by sector with the decoded retail disk 1 (second pressing), PNA differs only in the boot block
   (sector 0), the loader image (tracks 1-75: the retail Copylock key track holds ordinary data there, and the loader is
   decrypted) and the game image (decoded to plain). Everything else is identical, including the filler tracks at the end
   of the disk.
2. **The canonical (BS1) image is PNA's crack with BS1's credit text.** PNA's game image differs from the retail second
   pressing by 19 bytes (the 14-byte protection site at `$70F6`, 4 bytes at `$76AE`, 1 byte at `$93F0`); the canonical
   image differs from the retail second pressing by 200 bytes: the same 19 plus 181 bytes of credit text. So BS1 did not
   remove the protection independently: its patch equals PNA's. Only the text differs between PNA and canonical.

The genuinely one-disk disks are Vision Factory (VF), VF `[a]` and the Black Monks `[h][t +36]` build: they carry
OFS files, a crunched executable and six crunched data files.

## 3. Families

### 3.1 BS1 family (baseline)
`BS1 [t+3]` (+`[a]` boot block, +Deejay99, +Timecircle): a standard OFS disk with the packed `XENON-II` executable (sha1
`778cf75e...`) that the importer depacks. The C++ importer already turns the Timecircle disk 1 into the canonical image
(`game_0400.bin` sha1 `1d37b014...`, checked); it only prints "not a recognised image" because the disk's sha1 is not in
`kDisc1`. Adding `cb366028a1d0904601b16da745f2557ff08debfb` as quality 2 is the whole job.

### 3.2 Cracked retail disk 1 (PNA, PNA `[a]`, QTX, Black Monks, Flashtro, Predators)
Layout as the retail disk 1; the game image is stored plain at `$70C00` ($3FC00 bytes, the same size as the canonical
image) instead of the retail XOR chain. Recipe: `tools/variants/crack_retaildisk.py DISK1.adf --out DIR [--disc2 DISC2.adf]
[--canonical game_0400.bin]`. It recognises disks by sha1 only. Disc 2 is not on these disks: the importer needs the
user's standard disc 2, exactly as for the BS1 set.

How each differs from the retail second pressing (closest) and from canonical:

| Variant | vs retail 2874 | vs retail 0297 | vs canonical |
|---|---|---|---|
| PNA | 19 | 513 | 181 |
| PNA `[a]` | 23 | 517 | 185 |
| QTX | 48 | 543 | 226 |
| Black Monks (after the tail repair) | 57 | 552 | 195 |
| Flashtro +16 | 751 | **256** | 936 |
| Predators +9 | 544 | 1038 | 568 |

Flashtro +16 is a crack of the **first** pressing (it carries the first-pressing code revision at `$1F8A` and the
different loader routine at `$93F0`); all the others are cracks of the second pressing. Its replays are nonetheless
identical to canonical with the credit text restored, because the first-pressing differences do not change what the
replays exercise. Predators replaces 224 bytes of entry code at `$400` with its trainer start-up: the runtime checks the
entry code and refuses the image, so it cannot be supported without restoring bytes that are only known from the retail
disk.

### 3.3 One-disk crunches (VF, VF `[a]`, Black Monks `[h][t +36]`)
Ordinary OFS disks (`startup-sequence`, a text, devices) holding:

* **six crunched data files** with identical contents in all three disks (sha1 `b7e877cc...`, `69cd6092...`, `963925a0...`,
  `e1302718...`, `c8f61609...`, `65942047...`). Their names encode disc 2 sector ranges: `000b00e5` = 229 sectors from
  sector `$0B`, `00fa00fe` = 254 from `$FA`, `02020113` = 275 from `$202`, `05c400f8` = 248 from `$5C4`, `031f0159` = 345
  from `$31F`, `04820138` = 312 from `$482`. Each decrunches (`tools/variants/onyx_crunch.py`) to **exactly** those
  sectors of the standard disc 2 (byte for byte);
* **a crunched executable**, which is the whole game as a memory snapshot that includes the game image at `$400`. The
  three disks use three crunchers: ONYX (VF, `87fa47ae...`), an Ice-style one (VF `[a]`, `7b27789c...`,
  `tools/variants/ice_crunch.py`) and the BS1 ByteKiller (Black Monks `[h]`, `48a31ecb...`, the existing
  `tools/bkunpack.py`). Block and snapshot layouts are in the docstrings of `crack_onyx.py`.

Rebuilding disc 2: place the six decrunched chunks at their sectors; the unused 10-sector gaps between chunks and the
first ten sectors carry the formatter's `$E5` fill; the tail after the last chunk is zero; sector 0 holds the disc id
(`X2B ` at offset 2) followed by about 500 bytes of other data. The result is identical to the standard disc 2 **except sector 0**: these
disks do not hold its payload (it is not in the snapshot either). Evidence that the payload is not needed: the replays
produce the same frame hashes with a sector 0 reduced to the disc id (`level1_play`, `ending_natural`, `boss1`,
`level2`). `crack_onyx.py --disc2 STANDARD.adf` uses the user's own disc 2 unchanged, which is the exact route.

The crunches replace the game's disk loader (`$921A..$95A8`, 910 bytes) by their own; the runtime's disk emulation answers
at the loader's entry, so this is harmless, and the VF replays are identical to canonical with the credit text restored.

### 3.4 Black Monks (cracked retail disk 1): the tail
The image body is plain, but the last 19455 bytes (`$3B400..$3FFFF`) are left in the retail XOR-chain form and the
crack overwrote the first two stored words of that part, so a plain chain decode is wrong for the whole tail (the error
is a constant 16-bit XOR on every later word). The image itself contains an earlier block (`$3AB4F`, 1088 bytes)
identical to the head of the tail, so the head is read from there and the remaining tail decodes from its last word. This
depends on that duplicate in the data: it is a per-disk recipe, not a general rule. With it, the recovered image differs
from canonical by 195 bytes and passes `level1_play` and `ending_natural`; boss 1 and level 2 diverge because of the
trainer hook at `$7913`.

### 3.5 Not analysed further
Flashtro +16 `[a]`: the first three stored words are patched and from `$E0C` on tens of KB differ from any decoding
tried (plain, retail chain with the start value fitted, constant-error chains). Needs reading the disk's loader; low
value (a trainer build of the first-pressing crack, which Flashtro +16 already covers).

## 4. Verification

* **Crunchers against the real stubs.** `tools/variants/stubrun.c` runs the 68000 code of a crunched executable on the
  project's Musashi core until its decrunch finishes and dumps memory. The Python ONYX depacker reproduces that output
  for all six data files and the executable (the data files also equal the standard disc 2 sectors byte for byte); the Python
  Ice depacker is byte-identical to the VF `[a]` stub's output (287,834 bytes). The ByteKiller block is verified by its own
  checksum (`unpack_bs1.py`).
* **Replays.** Each recovered image was run through `level1_play`, `ending_natural`, `boss1` and `level2` (frame hashes at
  fixed frames, compared with the canonical hashes) with the credit text restored. Results: PNA, QTX, Flashtro +16
  and the VF disk match everything; Black Monks matches `level1_play` and `ending_natural` only (trainer hook); VF `[a]`
  matches except one frame around 500; PNA `[a]` diverges at the first death (infinite ships) and Black Monks `[h]` at frame
  1000 (trainer hooks); Predators does not start. The unmodified images (with their own credit text) fail the first frame
  hash at frame 500 for every variant, which only shows that the text is on screen then.
* **Importer behaviour (updated in P3c).** The C++ importer now imports the BS1 family including `[h Timecircle]` (the same
  `XENON-II`, the canonical image) and, as their own variants (`canonical=false`, no credit-text restoration, own replay
  manifests in `tests/replays/cracks/<id>/`), **PNA, QTX and Flashtro +16**: the game image is the bytes at disk offset
  `$70C00`, recognised by the sha1 of the whole disk (the same recipe as `tools/variants/crack_retaildisk.py`, which gives
  the same image sha1; checked on all three). Everything else in the table is recognised by sha1 and refused with its
  specific reason: PNA `[a]` (trainer patch), Black Monks (tail repair not done yet), VF, VF `[a]`, Black Monks `[h]` (need
  decrunchers), Predators +9 (entry code), Flashtro +16 `[a]` (transformation not analysed). The wrong "single-disk crack"
  wording is gone.

## 5. Recommended order and effort

| Order | Variant(s) | Work | Estimate |
|---|---|---|---|
| 1 | BS1 `[h Timecircle]` | add the sha1 to the known table | 0 d |
| 2 | PNA, QTX, Flashtro +16 | a "cracked retail disk 1" path in the importer: find the plain image at `$70C00`, pair with the user's disc 2, mark the manifest `canonical=false` with the variant name | 1-2 d (shared with the retail path of P3c) |
| 3 | VF, VF `[a]`, Black Monks `[h]` | native ONYX decruncher (~150 lines), Ice depacker (~100 lines), reuse the BS1 ByteKiller; rebuild disc 2 from the six files; `--disc2`/the user's disc 2 preferred | 3-4 d |
| 4 | Black Monks | the tail repair (per-disk constants) | 0.5 d |
| 5 | PNA `[a]`, Black Monks `[h]` | allowed with a "trainer build: gameplay differs" warning; goldens not applicable | 0.5 d |
| - | Predators, Flashtro `[a]` | refuse with a clear message (entry patched / unknown transformation) | 0 d |
| - | the eight TOSEC-only cracks | cannot be examined until found | - |

All of these are fallbacks for users who have no better source: the retail IPF or flux dump (P3c) gives the unmodified
game and is the preferred route. Variants other than the BS1 family cannot be byte-identical to canonical (credit text
at minimum), so each needs its own recorded output hash (and, for the replay goldens, either its own hashes or the
credit-text bytes masked as in the experiments above).

## 6. Open points

* Whether `$76AE`/`$70FA` (QTX and Black Monks differ from canonical there) matter on paths the replays do not take.
  The runtime now answers the in-game Copylock check from a key receipt (`retail-protection.md`); a cracked disk has none,
  and every recoverable crack passed the replays without it.
* The eight cracks listed by TOSEC and not found; the CDTV "rip" is irrelevant to this phase.
* Sector 0 of disc 2: its payload (a small bitmap-like block after the id) is not needed by the replays but is not
  proven unused by every code path; with the user's own disc 2 the question does not arise.
