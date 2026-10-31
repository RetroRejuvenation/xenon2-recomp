# Flux-level disk images: SCP, KryoFlux streams and HFE

Code: `runtime/import/flux.{h,cpp}` (PLL, per-track decoding, reports), `scp.cpp`, `kryoflux.cpp`, `hfe.cpp`.
Tests: `tests/import/flux_tests.cpp`, `flux_fixtures.h` (synthetic flux), the flux cases in `import_tests.cpp`.
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

## Decoding a track (`decode_flux_track`)

Each revolution is run through the PLL, and the start of the next revolution (up to 24 ms, which holds any one
sector) is appended, so a sector across the pulse reads whole. The MFM sector decoder then runs on the bit stream
as a *linear* stream (no wrap-around): the end of the last revolution is not continuous with its start, and a
degenerate sector (constant data) can pass the XOR checksums with the wrong bits across such a seam. A sector
across the pulse of a one-revolution dump is therefore reported unreadable rather than risked; dump at least
three revolutions (`docs/DUMPING.md`).

The first intact copy of each sector wins; two intact copies with different data (possible with weak or
duplicated data) set the track's `disagree` flag. A track with no Amiga sector in any revolution becomes a `Raw`
track holding the first revolution's bit cells. Failures are reported per sector ("track 3 sector 9: data
checksum error in all 3 revolutions").

## Evidence

### Synthetic flux (every `ctest` run)

Random sectors are MFM-encoded, converted to transition times with a spindle error, slow wow (0.5 %), gaussian
timing noise on every transition and a randomly placed index pulse, then wrapped as SCP, KryoFlux stream and HFE.
Sectors recovered byte-exact, 8 tracks (88 sectors) per cell, by gaussian jitter (sigma as a fraction of a 2 us
cell):

| jitter | 1 revolution | 3 revolutions |
|---|---|---|
| 0 - 10 % | 88/88 | 88/88 |
| 12 % | 74/88 | 86/88 |
| 14 % | 16/88 | 30/88 |
| 17 % | 0/88 | 0/88 |

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

The Frontier Elite II result also exposed the SCP index-pulse convention above: reading it with a separate tail
left 28 sectors unreadable in every revolution; reading it as documented here gives all 1760.

## Limits

- Amiga double-density MFM only (2 us cell, 80-84 cylinders, 11 sectors). HD and other densities are refused or
  stay raw.
- No weak-bit recovery, no per-track variable-density PLL, no soft decisions: a track noisier than about 12 %
  jitter is reported unreadable.
- A one-revolution dump cannot read a sector that straddles the index pulse.
- HFE v3 and SCP files made with unusual cell widths are best effort or refused.
- Raw (protection) tracks are kept as bit cells for the retail-disk import (a separate piece of work); nothing
  reads them yet, and the importer says so when it meets one.
