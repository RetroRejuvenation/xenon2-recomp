# Dumping your own Amiga disks (draft)

x2recomp imports disk images that you made from your own game disks. This page says how to make a good dump with
a KryoFlux or an Amiga DrawBridge, and how to check it. **It is a draft written from the readers' requirements and
the public descriptions of the tools; follow the tool's own current documentation for the exact buttons and
switches, and tell us what differs.** Nothing here is needed if you already have ADF images of the supported
release (`README.md`, "Supported release").

## Which format to dump to

| You have | Dump as | Why |
|---|---|---|
| a KryoFlux | **stream files** (`trackNN.S.raw`), several revolutions | the safest: it keeps everything on the disk, including copy-protection tracks, and can be re-decoded later |
| an Amiga DrawBridge | **SCP** if your DrawBridge software offers it, otherwise ADF | a plain ADF holds only the AmigaDOS sectors; a copy-protected original cannot be fully captured that way |
| a Greaseweazle or SuperCard Pro | SCP, or HFE | both are read |

An ADF is enough for a disk without copy protection (the second disk of a two-disk set has none). A disk that
loads through a custom loader or protection scheme (the original first disk of many games, Xenon 2 included) needs
the flux dump: the importer verifies the Copylock key track in it and imports the retail disk 1 from it
(`docs/re-notes/retail-protection.md`). Without the key track (a plain ADF) the retail first disk is refused, unless you
choose the explicit opt-in protection patch (README, "Supported releases").

## KryoFlux (DTC)

1. Use a 3.5 inch drive on the KryoFlux, write-protect the disk, and let the drive spin up.
2. Capture **both sides, tracks 0 to 81** (82 cylinders, so a disk that was written past cylinder 79 is kept),
   double density, **at least 3 revolutions** (5 is better; a dump of one revolution cannot read a sector that
   straddles the index pulse). With the command-line tool the relevant options are the output file name pattern
   `track` (giving `track00.0.raw`, `track00.1.raw`, ...), the first and last track (`-s0 -e81`), the side
   selection (`-g0`/`-g1`, or both) and the number of revolutions; check the current `dtc` help for the
   spelling. Keep the default image type **0 (stream)**, not a pre-decoded type.
3. Put each disk's files in **its own folder** (`Disk1/`, `Disk2/`). Do not rename the files: the track number
   comes from the name. Zip the folders if you like.
4. Import: `x2recomp --import Disk1 Disk2` (folders), or drop them on the import window. One folder is one disk.
   A folder that holds several disks in subfolders is read disk by disk, too.

## Amiga DrawBridge

1. Connect it, write-protect the disk, and use the DrawBridge's disk reader to read the disk. If it offers an
   **SCP** (flux) output, choose it and the highest revolution count it allows; otherwise read an **ADF**.
2. If the reader reports unreadable tracks or sectors for an ADF, retry, clean the drive head, and prefer the
   flux output: the importer then votes over the revolutions and names the sectors it could not read.
3. Import the file: `x2recomp --import disk1.scp disk2.scp` (or `.adf`).

## Checking a dump

- `x2recomp --import` prints what it found, for example `disk1.scp: SCP flux image: 160 tracks, 3 revolutions
  each`. A line such as `100 sectors rescued by a later revolution` is normal for a worn disk.
- Unreadable sectors are listed by track and sector (`track 3 sector 9: data checksum error in all 3
  revolutions`). Re-dump, clean the head, or try another drive. A disk with a few failing sectors may still be
  playable, but the importer refuses an incomplete disk rather than guess.
- `copy-protected tracks are only imported for the retail Xenon 2 disc 1, which this image is not` means the disk has
  tracks that are not standard Amiga sectors and is not the retail Xenon 2 first disk (another protected disk, or
  too many unreadable tracks to recognise it). For the retail first disk the importer reports `key track verified
  (Rob Northen Copylock, seed ...)`, or says why the key track could not be verified (re-dump with several
  revolutions). `x2recomp --diagnose-image FILE` shows the per-track picture.
- Compare with a known-good ADF if you have one: the importer reports the SHA-1 of the images it uses, and
  `docs/re-notes/sources.md` lists the known ones.

## What to keep

Keep the original flux files. They are the best record of your disk, can be re-decoded by newer versions, and are
the only way to preserve protection. Do not share them: they are copies of the game.
