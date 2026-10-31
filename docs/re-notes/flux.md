# Flux-level disk images: SCP, FluxEngine, KryoFlux streams and HFE

Code: `runtime/import/flux.{h,cpp}` (PLL, retry pass, per-track decoding, merging, reports), `scp.cpp`, `fluxengine.cpp`,
`kryoflux.cpp`, `hfe.cpp`.
Tests: `tests/import/flux_tests.cpp`, `flux_fixtures.h` (synthetic flux, including a FluxEngine writer), `pdos_tests.cpp`
(FluxEngine reader, merged captures), the flux cases in `import_tests.cpp`.
The importer accepts these in any combination with the other formats; the user-facing description is in
`docs/BUILDING.md` and the dumping guide is `docs/DUMPING.md`.

## What a flux image is

A floppy drive's head reports a pulse each time the magnetisation reverses. A flux dump records the time between
pulses, for a whole revolution of every track, usually several revolutions. Nothing in it says where the bits or
sectors are: a decoder has to recover the cell clock (a PLL), turn the pulses into MFM bit cells, find the sectors
and verify their checksums. That is the whole point of keeping the flux: protection tracks, marginal sectors and
the exact timing survive, and a disk can be re-decoded later with a better decoder.

For an Amiga double-density disk a cell is 2 us, so a transition follows 2, 3 or 4 cells (4, 6 or 8 us) after the
previous one in the data and in the gaps. A track is about 100,000 cells (200 ms at 300 rpm), 11 sectors of 1088
raw bytes plus a gap.

## The readers

| Format | Read by | Notes |
|---|---|---|
| SCP (SuperCard Pro) | `read_scp` | Header, 168 track offsets, per revolution an index time, a flux count and the 16-bit big-endian tick values (25 ns by default; 0 means "add 65536"). Two-sided images number tracks `cylinder * 2 + side`. Only 16-bit cells. A bad checksum is a warning. |
| FluxEngine `.flux` | `read_fluxengine` | FluxEngine's native protobuf file (also written by tools that follow it). Field 1 is the magic 0x466C7578, field 2 the version (2), repeated field 3 a track `{1: cylinder, 2: head, 3: flux bytes}`. A flux byte has bit 7 = a transition, bit 6 = the index pulse, bits 0-5 = ticks of 1/12 us (83.33 ns); the byte 0x3F adds 63 ticks and continues the interval. Index pulses cut the track into revolutions (what lies outside the first and last pulse is dropped). Layout checked on one real capture (`acquisitions-flux-acorn-st.md`); only these fields are read, unknown fields are skipped, other versions give a warning. Bounded: 4 MB of flux per track, 64 million transitions in all, 32 revolutions per track. |
| KryoFlux stream | `read_kryoflux_stream` + `kryoflux_track_from_name` | One file per track (`<prefix>NN.S.raw`: DTC uses whatever prefix `-f` gave, NN is the cylinder, S the side; the track number is taken from the end of the name). Flux1/Flux2/Flux3 cells, Ovl16 carry, Nop1-3, and out-of-band blocks: Index (stream position, sample counter, index counter), StreamInfo, StreamEnd (a non-zero result is reported), KFInfo (the text gives `sck=`, the sample clock), EOF. The index blocks cut the stream into revolutions; what lies before the first index pulse and after the last is dropped. |
| HFE (HxC) | `read_hfe` | v1 and, best effort, v3. Holds bit cells, not flux timing, so there is no PLL: the track bytes (least significant bit first, sides interleaved in 256-byte halves of 512-byte blocks) go straight to the sector decoder. HD images and non-MFM encodings are refused. v3 has not been tested against a real file. |

Not read: extended-resolution SCP cell widths, HFE FM/GCR, IPF (see the disk-variants plan), raw `.kf` Greaseweazle
formats (a Greaseweazle can write SCP or HFE, which are read).

## The two ways a capture records the interval across the index pulse

The index pulse falls in the middle of an interval between two transitions. Captures store it differently, and the
decoder must join the end of one revolution to the start of the next without losing or inventing time, or a
sector that straddles the pulse reads wrongly.

- **SCP:** the first value of every revolution after the first is the *whole* interval (the time since the last
  transition before the pulse). Evidence from a real three-revolution dump: the first values are 5.8, 8.1 and
  8.1 us (plausible MFM intervals); adding "index time minus the sum of the values" as a separate tail would give
  intervals of 10 to 13.6 us, which MFM cannot produce. So `tail_ns` stays 0 for SCP and the revolutions are
  simply concatenated.
- **KryoFlux:** the cell straddling the pulse is one cell in the stream, and the Index block's sample counter says
  how many ticks of it came before the pulse. The cell becomes the first value of the new revolution minus that
  part, and the part is the previous revolution's `tail_ns`; their sum is the whole cell again.

## The PLL (`pll_decode`)

A first-order digital PLL over the interval list. State: the cell length `T` (starts at 2000 ns, may drift
+-12 %), and the time `bnd` of the last cell boundary it accepted. For each transition at time `t`:

1. `n = round((t - bnd) / T)`, at least 1 (a transition after `n` cells is `n-1` zero bits and a one bit);
2. `err = t - (bnd + n*T)`;
3. `bnd += n*T + 0.1 * err` (phase correction) and `T += 0.005 * err / n` (frequency correction).

Gaps longer than 12 cells (no flux to follow) emit a bounded run of empty cells and restart on that transition.
The gains came from a sweep on synthetic flux with +-2 % spindle error: a phase gain of 0.5 (the first guess)
decodes only 43 of 66 sectors at 12 % jitter, 0.1 decodes 65 of 66; the frequency gain barely matters below 0.01.

**Run clamp.** `PllParams::run_min` / `run_max` limit how many cells a transition may follow. The first pass uses 1
and 12, because copy-protection tracks legitimately break the MFM rule. Valid MFM has runs of 2 to 4 cells, so
the retry pass clamps to 2 and 4: a noisy transition that the plain PLL would round to the wrong number of cells
(and so shift every following bit) is pulled into the legal range, and the phase loop keeps tracking.

## The retry pass

After the plain pass over every revolution, a track that is Amiga formatted (some sector was seen) but still has
sectors without a valid copy is decoded again with the clamped PLL at eleven loop gains, each revolution at each
gain, until every sector is intact. The gain pairs `(phase, frequency)`: (0.1, 0.01), (0.2, 0), (0.3, 0.05),
(0.1, 0), (0.1, 0.02), (0.1, 0.03), (0.2, 0.01), (0.2, 0.05), (0.3, 0), (0.4, 0.02), (0.4, 0.05); they are the
pairs a sweep over a real, marginal dump found useful. Rules that keep it honest:

- a sector that passed its own header and data checksums is never touched, and a sector is only ever accepted on
  those checksums (no repair, no guessing from a neighbour);
- a track with no Amiga sector at all stays raw; for those tracks the same sweep builds *alternate bit streams*
  (`Track::alt`: up to 40 per track, each 12 KB, de-duplicated, from up to 8 revolutions) that a custom-format
  decoder (the PDos release) reads in turn.

## Merging two captures of one disk (`merge_flux_disks`)

Given together, two captures whose track 0 reads the same (at least eight sectors intact in both, all equal) are
treated as one disk: the revolutions of the second are added to every track of the first (up to 32 per track), and
the decoder runs over all of them. Nothing is overridden: a sector valid in any revolution of either capture is
kept, and two valid copies with different data set the `disagree` flag as before. Captures whose track 0 differs
stay separate disks.

## Decoding a track (`decode_flux_track`)

Each revolution is run through the PLL, and the start of the next revolution (up to 24 ms, which holds any one
sector) is appended, so a sector across the pulse reads whole. The MFM sector decoder then runs on the bit stream
as a *linear* stream (no wrap-around): the end of the last revolution is not continuous with its start, and a
degenerate sector (constant data) can pass the XOR checksums with the wrong bits across such a seam. A sector
across the pulse of a one-revolution dump is therefore reported unreadable rather than risked; dump at least
three revolutions (`docs/DUMPING.md`). (The raw PDos tracks are decoded as a circle instead, because their sectors
are checked against 16-bit checksums over a header that names the sector and track; a marker straddling the seam
is found by reading past the end.)

The first intact copy of each sector wins; two intact copies with different data (possible with weak or
duplicated data) set the track's `disagree` flag. A track with no Amiga sector in any revolution becomes a `Raw`
track holding the first revolution's bit cells. Failures are reported per sector ("track 3 sector 9: data
checksum error in all 3 revolutions").

## Evidence

### Synthetic flux (every `ctest` run)

Random sectors are MFM-encoded, converted to transition times with a spindle error, slow wow (0.5 %), gaussian
timing noise on every transition and a randomly placed index pulse, then wrapped as SCP, KryoFlux stream and HFE.
Sectors recovered byte-exact, 24 tracks (264 sectors) per cell, by gaussian jitter (sigma as a fraction of a 2 us
cell). "Plain" is the decoder before the retry pass (the same flux, run through the code of release 0.2.0); "now" has
the retry pass (the sectors only it read are in brackets):

| jitter | 1 revolution, plain | 1 revolution, now | 3 revolutions, plain | 3 revolutions, now |
|---|---|---|---|---|
| 8 % | 264 | 264 | 264 | 264 |
| 10 % | 261 | 264 (3) | 264 | 264 |
| 12 % | 200 | 242 (43) | 261 | 263 (2) |
| 14 % | 39 | 82 (47) | 93 | 161 (72) |
| 16 % | 0 | 1 | 0 | 3 |
| 18 % and up | 0 | 0 | 0 | 0 |

(the table is printed by `x2_flux_tests`, `test_retry_pass`; the earlier 8-track table, 12 %: 74 and 86 of 88, 14 %:
16 and 30, is the same curve measured on a smaller sample.) The retry pass moves the practical limit from about
12 % to about 14 % of the cell, mostly for single-revolution dumps; beyond 16 % nothing helps and nothing is
invented. Compared with the plain decoder, the retry pass can only add sectors, never change one.

Spindle error from -4 % to +4 % decodes everything. Real drives on healthy media show a few percent. Beyond the
limit the decoder fails loudly: noise at 35 % never produces a sector that passes its checksum with wrong data.
The synthetic tests also cover dropouts rescued by a later revolution, a long track, a sector straddling the
pulse, raw (non-Amiga) tracks, truncated and self-referential files, and a mutation fuzz pass (ASan/UBSan clean).

### Real dumps (local only; `flux_files`; public Internet Archive items, retrieved 2026-10-30, none of them Xenon 2)

| Item | File (sha1) | Result |
|---|---|---|
| `simcity512k_kfx` (Amiga, KryoFlux) | `simcity512k_kryoflux.zip` (`1a32086e6add66a4a717bb914bffe6bbb17a0d4e`): 168 `trackNN.S.raw` files, 5 revolutions | decodes to a complete 880 KB image with an AmigaDOS boot block; **byte-identical to the item's reference ADF** (`simcity512k.adf`, `aee246bd5f68d1934dcbaa6bfddd1022154653d1`) |
| `Lowe_FrontierEliteIIAmigaProgramDisk1of2` (Amiga, SCP) | `.scp` (`53372763c8865762c0645a0c01527e7a5fa6980b`), 3 revolutions, 164 tracks | complete 880 KB image, **byte-identical to the item's reference ADF** (`b81c754a9ce390a4fbeb07f8af7219b0ff1ffcbe`); 100 sectors needed a later revolution |
| `Lowe_Pandemonium1of2_1992.08.26` (SCP) | `.scp` (`133165103d8861d1524ac622b500a8ff894e551e`) | not an Amiga-format disk: all tracks stay raw, which is the intended outcome |

The retry pass changed nothing on these two: they read completely in the first pass, and their images are still
byte-identical to the references.

Xenon 2 (the Rob Northen PDos release, Internet Archive item `amiga-original-xenon-2-powerplay`: one SCP and one
FluxEngine capture, three revolutions per track in each) is the first real dump whose data
tracks are a custom format, so it exercises the alternate streams. Sectors read of 1,908 (159 tracks of 12):

| capture | first revolution, plain PLL only | every revolution at every PLL setting |
|---|---|---|
| SCP | 1,752 | 1,896 |
| FluxEngine | 1,859 | 1,885 |
| both, merged | 1,752 (from the first capture) | 1,901 |

One sector (track 98, sector 10) is read at one PLL setting only (phase gain 0.1, frequency gain 0.03, in the SCP capture), by exactly this
sweep. The remaining seven (95:9, 97:11, 99:2, 100:1, 101:4, 110:2, 114:6) are unreadable in every revolution at every
setting; the ambiguous cell intervals are localised and agree between revolutions, so this is the media and not
the decoder (`pdos-v5.md`).

The Frontier Elite II result also exposed the SCP index-pulse convention above: reading it with a separate tail
left 28 sectors unreadable in every revolution; reading it as documented here gives all 1760.

## Limits

- Amiga double-density MFM only (2 us cell, 80-84 cylinders, 11 sectors). HD and other densities are refused or
  stay raw.
- No weak-bit recovery, no per-track variable-density PLL, no soft decisions: a track noisier than about 14 %
  jitter is reported unreadable. Sector checksums are never used to repair data (a 16-bit checksum would make a
  guess look like a success).
- A one-revolution dump cannot read a sector that straddles the index pulse.
- HFE v3 and SCP files made with unusual cell widths are best effort or refused.
- Raw (protection) tracks are kept as bit cells for the retail-disk import (a separate piece of work); nothing
  reads them yet, and the importer says so when it meets one.
