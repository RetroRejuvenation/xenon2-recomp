# CDTV disc: the data after the ISO volume (decision item D6)

Question: the CDTV disc's data track (Track 01, `MODE1/2352`, 6005 sectors) holds an ISO9660 volume of 2449 sectors
(`docs/re-notes/cdtv.md` §1). What is the 3407-sector, 6,977,536-byte region after it (sectors 2449..5855), which
looks like smooth 8-bit PCM, and does anything use it? Short answer: **it is unreferenced mastering leftover, about
four fifths real sampled audio of unidentified origin (2671 sectors) and one fifth a labelled test-pattern fill (736 sectors)**. Nothing in the
program, the overlays or the device driver reads it, and the game plays without it. The analysis is reproducible
with `tools/cdtv_data_region.py` (it prints structure and statistics only; nothing derived from the disc is committed).

Confidence key: **certain** = checked byte by byte or by construction, **high** = several independent observations,
**medium** = consistent with the evidence but not proven, **open** = not determined.

## 1. Raw structure (certain)

- All 6005 sectors of Track 01 are `MODE1` with a valid 12-byte sync, a header with continuous MSF addresses
  (`00:02:00` .. `01:22:04`, absolute = LBA + 150), 2048 user bytes, a **correct EDC** (CD-ROM CRC-32, 0 mismatches
  in 6005) and the 8 reserved bytes zero. There is no Mode 2 / XA structure anywhere, so the 2048-byte-per-sector
  view is the right one and the region is ordinary data written by the mastering process, not audio stored in
  an unusual sector form.
- The ISO9660 volume ends exactly where the region starts: the PVD's volume space size is 2449 blocks (sectors
  0..2448); the volume descriptor set is the PVD (16), a second identical PVD (17) and the terminator (18).
  The root directory is at extent 19, the path tables at 2445-2447, so file data and metadata all lie inside the
  volume. The PVD names the publisher `MIRRORSOFT`, system id `CDTV`, volume id `XENONII`, creation date
  1991-10-03; the preparer and application fields are empty and the application-use area holds 13 non-zero bytes.
  There is no second session and nothing in the volume describes the later sectors.
- Layout of Track 01:

| Sectors | Count | What |
|---|---|---|
| 0..2448 | 2449 | the ISO9660 volume (101 files) |
| 2449..5119 | 2671 | sampled-audio-like content (5,470,208 bytes), section 3 |
| 5120..5855 | 736 | **labelled filler**: each sector starts with its own sector number as ASCII digits and a zero byte (`5120\0`), then fills with `0xAA` (odd sectors) or `0x55` (even) to the end; the label always equals the sector number |
| 5856..6004 | 149 | all zero (the data track's trailing pad before the first audio track's pregap) |

The content part ends abruptly at the `0x1400` (5120) boundary mid-signal (the last bytes of sector 5119 are not
silent) and the filler starts there. The signature of the filler (numbered sectors, alternating `55`/`AA`) is the kind
of pattern a mastering or disc-image tool writes to pad a track to a required length, so the region looks like a buffer
that was only partly overwritten: first the tail of whatever was in the buffer, then the pad.

## 2. Is anything referencing it? No (high)

- **Device commands.** `X2` issues exactly two command types to `cdtv.device`: command 43 (play track, `io_Offset` =
  track, `io_Length` = 2) at `$3A566`/`$3A5BA` and command 56 (volume, `io_Offset` = `$7FFF`/0) at `$3A5EA`/`$3A616`
  (code-hunk offsets; file offsets +36). A scan of every `move.w #imm,28(An)` and every write to `io_Offset` (44),
  `io_Length` (36) and `io_Data` (40) shows no other command: **no `CD_READ`, `CD_READXL`, `CD_SEEK` or
  offset-based access**, and no register-form command write. The other two `io_Length`-like writes at `$3CBCA`/`$3CBDC` belong to the
  object-movement code (offsets `$24` of an object), not to an I/O request. `cdtv.device` is opened in `X2` and in
  `CDTV.TM`; `CDTVPREFS` talks to `bookmark.device` (the NVRAM card) only.
- **Exec I/O calls elsewhere.** `DoIO`/`SendIO`/`CheckIO`/`WaitIO`/`AbortIO` call patterns occur only in `X2`
  (the four known CD-audio sites), `CDTV.TM` (the driver itself) and `CDTVPREFS`. None of the `0x601A` overlays
  (`LEVEL1-5`, `INTRO`, `PICCODE`, `PICDATA*`) or the sample banks contains one or a device name.
- **File access.** All other data comes through `dos` `Open`/`Read`/`Close` on `CD0:` files, i.e. through the ISO volume,
  which cannot reach sectors outside it.
- **Constants.** Searching for the sector numbers 2449..5855 (and +150 for absolute addresses) as longwords, and for
  `N*2048` / `N*2352` byte offsets, gives hundreds of hits per file, all coincidences: round pixel and bitplane offsets
  such as 3200, 4096 and 4800 dominate and every sampled hit sits in graphics or table code. I did not find
  a constant that is used as a CD address. This search proves little by itself; the call-site analysis above is what
  carries the conclusion.
- **WHDLoad.** The CDTV WHDLoad install (`docs/re-notes/cdtv-slave.md`) copies only the ISO files and the game plays,
  and its slave never reads the CD itself, so no version of the program known to us depends on the region.

## 3. What the content is

Evidence that it is real mono 8-bit linear PCM (high):

- all 256 byte values are used (entropy of the content part 6.2 bits, mean -1.1, rms 29); the stream is smooth as signed 8-bit
  (mean |difference| / standard deviation 0.041), and consecutive bytes correlate at 0.978 in **both** byte phases
  with identical statistics (entropy 6.67 bits at even and odd positions, mean |difference| 3.61 and 3.60). Real 16-bit
  audio would have a noise-like low byte next to a smooth high byte, and stereo would alternate two streams, so it is
  one stream of 8-bit samples: mono 8-bit linear;
- above 0.25 cycles per sample the spectrum of the steady segments is the 8-bit quantisation floor (about 0.3 LSB rms),
  so the signal is band-limited and sampled with 8-bit resolution;
- the DC offset is -1.4 in every segment, so all segments came through one conversion chain; 2036 samples are
  clipped at -128/127 (mostly in segment A), the content is not normalised hot;
- items are separated by **exactly zero** sectors (digital silence), preceded by decaying levels.

Segments, delimited by zero runs (absolute sectors, inclusive; sizes include any trailing silence the decay has not
reached, so they are approximate item sizes only):

| Segment | Sectors | Bytes | Character |
|---|---|---|---|
| A | 2449..2885 | 894,976 | broadband, 5% of the energy above 0.25 cycles/sample, strong level changes (rms 27) |
| (gap) | 2886..2895 | 20,480 | zero |
| B | 2896..4138 | 2,545,664 | one long steady item, rms 33, 99% of the energy below 0.02 cycles/sample, bass-dominated (dominant periods about 462 and 205 samples) |
| C1-C5 | 4161..4200, 4211..4251, 4312..4387, 4426..4527, 4546..4921 | 82 KB, 84 KB, 156 KB, 209 KB, 770 KB | shorter items; zero gaps of 22 sectors before C1, then 10, 60, 38 and 18 between the items, and 14 before D |
| D | 4936..5119 | 376,832 | steady item, cut at the filler |

At 22050 Hz mono the content part is about 248 s; the rate is **open** (see below).

What it is not:

- **Not a copy of the ISO files.** Probe searches of 48-byte windows in both directions find no common bytes with any of the
  101 ISO files (`SOUNDS*`, `SPEECH*` included), with the disk build's game image, `disc2.bin` or the disk ADFs.
- **Not a copy of the CD audio tracks 02-13** at the rates tried: normalised cross-correlation of low-passed chunks of segments A-D
  (12 s each, eight candidate sample rates from 5512.5 to 44100 Hz) against all 12 tracks gave a best score of
  0.16, which is the noise level for these search lengths. This does not exclude a different mix, a different
  speed or a rate not on the list.
- **Not the length of any CD track:** the whole region is 3407 sectors (45.4 s at CD-DA's 75 sectors per second) and the
  content part 2671 sectors; no track length of the release matches either.
- Segment and item sizes do not equal any ISO file size (checked against all `SOUNDS`, `SPEECH`, `PICDATA`, `LEVEL`
  files within +-6 sectors; the four near hits are coincidences of size only).
- Not compressed by Fibonacci-delta or ADPCM schemes: a linear reading gives a plausible smooth spectrum and the
  quantisation floor, which a mis-decoded stream would not.

Candidate renderings for listening (all derived from the user's own disc, git-ignored, never committed): the content part as mono
8-bit WAV at 11025, 22050 and 44100 Hz, one stereo-interleaved reading at 22050 Hz (for comparison), and each
segment at 22050 Hz, written to `generated/lane-c/d6/` (`tools/cdtv_data_region.py --wav-dir` writes the three mono
versions). The 22050 Hz mono file is the most plausible default; if the music sounds pitched too high or too low, the
other two are the likely alternatives.

## 4. Conclusion

- **Purpose: unreferenced mastering leftover (high).** Reasons: it starts exactly after the ISO volume's end; the program's
  only CD device traffic is track play and volume; it has no directory entry, no table and no header of its own; it is
  cut at a round sector boundary where a numbered `55`/`AA` pad takes over; and its content is unrelated at byte level
  to everything else on the disc.
- **Origin of the audio: open.** It is real sampled audio with a consistent signal chain, bass-heavy in the long
  items, with digital silence between items. It could be a draft or alternate-mix sample set, material from another
  project that was in the mastering buffer, or test audio; the data alone does not say. It does not correlate with the
  twelve CD tracks as such, so it is not one of those tracks at a simple rate. Identifying it needs listening
  (and, if useful, a fingerprint against the Bomb the Bass catalogue) and is a human task.
- **Rights.** It is part of a copyrighted release of unknown provenance, so the project must not redistribute or embed any
  of it; the analysis tool prints structure and writes audio only on request, to a location the user chooses.

## 5. Recommendation

1. **Ignore it in the importer and the runtime.** Nothing uses it; the CDTV target (`.omc` plan P10) does not need it;
   leaving it out cannot change the game.
2. **Do not add it to the CD-audio importer by default.** If the user wants it, a later optional `--extract-extras`
   style tool could write the content part as WAV to a user-chosen folder; that is a convenience, not part of the game.
3. **Open items for a human:** listen to the rendered candidates to settle the sample rate and identify the music; if it is
   recognisable (a mix of a track on the disc, or a different tune), record that here. If a second CDTV image with the same
   region becomes available, a byte comparison of the regions would show whether it is a mastering constant or per-disc garbage.
