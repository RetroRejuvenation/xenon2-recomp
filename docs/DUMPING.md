# Dumping your own Amiga disks

x2recomp imports disk images that you made from your own game disks. This page says how to make a good dump with a
KryoFlux or an Amiga DrawBridge, how to import it, and how to tell that it is good. The tool options below were
checked against the tools' own documentation (listed at the end); anything that could not be checked is marked.
Nothing here is needed if you already have an IPF or ADF of a supported release (`README.md`, "Supported
releases").

## Which format to dump to

| You have | Dump as | Why |
|---|---|---|
| a KryoFlux | **stream files** (`NN.S.raw`), several revolutions | the safest: it keeps everything on the disk, including copy-protection tracks, and can be re-decoded later |
| an Amiga DrawBridge | **SCP** (flux) if you can, otherwise ADF | a plain ADF holds only the AmigaDOS sectors; the copy-protected retail first disk cannot be fully captured that way |
| a Greaseweazle or SuperCard Pro | SCP, or HFE | both are read |

An ADF is enough for a disk without copy protection (the second disk of the two-disk set has none). The original
first disk is copy-protected (a Rob Northen Copylock key track): it needs a flux dump, from which the importer
verifies the key track and imports the disk (`docs/re-notes/retail-protection.md`). Without the key track (a plain
ADF) the retail first disk is refused, unless you choose the explicit opt-in protection patch (README,
"Supported releases").

## KryoFlux (DTC)

1. Use a 3.5 inch drive on the KryoFlux, write-protect the disk, and let the drive spin up.
2. Capture **both sides, 82 cylinders, as stream files, with several revolutions**. With the DTC command-line tool:

   ```
   dtc -p -fDisk1/track -i0 -s0 -e81 -g2 -r5 -l8
   ```

   | Option | Meaning (KryoFlux manual) |
   |---|---|
   | `-p` | create the output folder (`Disk1/`) if it does not exist |
   | `-fDisk1/track` | the file name prefix, here `track`, which gives `Disk1/track00.0.raw`, `track00.1.raw`, ... |
   | `-i0` | image type 0: **stream files, preservation**. Do not choose a decoded type for the dump you import |
   | `-s0 -e81` | first and last track (cylinders 0 to 81, so a disk written past cylinder 79 is kept) |
   | `-g2` | both sides (`-g0` or `-g1` for one side only) |
   | `-r5` | revolutions to sample; the manual says the default depends on the image type, so set it. A dump of one revolution cannot read a sector that straddles the index pulse; 3 is the least worth having, 5 is better |
   | `-l8` | output level 8 (format messages), as in the manual's examples |

   Use `-d` to pick the drive if you have more than one, and `-dd` only if your drive needs the density line set.
   The file name prefix does not matter to the importer: it takes the cylinder and side from the end of the name
   (`track07.1.raw`, or `dumpfile07.1.raw` after `-fDisk1/dumpfile`).
3. Do the second disk the same way into its own folder (`Disk2/`). **Do not rename or move individual files** and
   do not mix two disks in one folder: one folder is one disk.
4. If you also want a plain ADF for quick use, the manual's example makes both in one pass:
   `dtc -p -fDisk1/track -i0 -fDisk1.adf -i5 -l8`. An ADF made this way is fine for the second disk, but it cannot
   hold the protection of the first.

## Amiga DrawBridge

1. Connect it, write-protect the disk, and use the DrawBridge software to read the disk. Its software reads **ADF**,
   and can create **SCP** flux files (SCP creation arrived in application v2.5 and was improved in v2.7).
2. For the copy-protected first disk, read an **SCP** with the highest revolution count the software offers.
   The v2.8.6 release notes call its accurate flux extraction "experimental" and say it "can't handle some
   long-track disks"; the Copylock track is not an ordinary track, so check the result as described below, and if
   the key track does not verify, re-read it, update the firmware, or use a KryoFlux.
3. For the second disk an ADF is enough. If a read reports unreadable tracks or sectors, retry, clean the drive
   head, or read an SCP instead: the importer then votes over the revolutions and names the sectors it could not
   read.
4. Not verified here: the exact names of the DrawBridge program's menu entries and how many revolutions its SCP
   output stores. Follow the program's own help.

## Importing the dump

```
x2recomp --import Disk1 Disk2          # KryoFlux folders
x2recomp --import disk1.scp disk2.adf  # SCP and ADF, in any mix
```

Give disk 1 and disk 2 in any order; the importer tells them apart by content. You can also drop them on the import
window. Add `--diagnose-image FILE...` first if you want to look before importing.

### What a good retail dump looks like

`x2recomp --diagnose-image` prints a per-track report and what the importer makes of it, without printing any game
data. The first line counts the 160 standard tracks (cylinders 0 to 79, both sides); that is the one to read. For a
good dump of the retail first disk it looks like this (a KryoFlux set; paths shortened, values that depend on your
disk replaced by placeholders):

```
Disk1 (KryoFlux streams): KryoFlux stream set: 159 standard tracks, 1 raw (non-Amiga or protection) tracks, 0 empty or damaged of 160
  key track (cylinder 0, head 1): Rob Northen Copylock verified, seed 0x........
  game image at $70C00: decoded, retail entry and Copylock stub present, sha1 <sha1>; variant retail-<pressing> (retail original, ...)
what the importer makes of it:
  Disk1 (KryoFlux streams): KryoFlux stream set flux image: 164 tracks, 5 revolutions each, 5 not standard Amiga tracks (kept raw)
  Disk1 (...): retail original, ... read from KryoFlux stream files: key track verified (Rob Northen Copylock, seed 0x........); ...
```

The second line of the importer's part counts every track that is not standard Amiga data, including cylinders 80
and 81, which `-e81` also reads and which are blank on these disks (four tracks). That is why a flux dump says
`164 tracks` and `5 not standard Amiga tracks`: the key track plus the four blank ones. Disk 2 looks like this:

```
Disk2 (KryoFlux streams): KryoFlux stream set: 160 standard tracks, 0 raw (non-Amiga or protection) tracks, 0 empty or damaged of 160
  key track (cylinder 0, head 1): ordinary Amiga sectors (the image cannot carry a Copylock key track)
what the importer makes of it:
  Disk2 (KryoFlux streams): KryoFlux stream set flux image: 164 tracks, 5 revolutions each, 4 not standard Amiga tracks (kept raw)
  Disk2 (...): BS1 [t+3] trained release, disk 2 (sha1 a748afc7a142335a40cc46fb7785dea820770241)
```

An SCP or IPF of the same disks reports the same first line. Then `x2recomp --import Disk1 Disk2` ends with
`imported (...)` and exit code 0, and the files it writes include `copylock.txt` for a retail disk.

### Checking that a dump is good

- **All tracks decode.** In the first line of `--diagnose-image`, disk 2 shows `160 standard tracks, 0 raw` and disk 1
  shows `159 standard tracks, 1 raw`: **exactly one** non-standard track, the key track (cylinder 0, head 1), and
  `0 empty or damaged`. More raw tracks than that, or `empty or damaged` tracks, mean a bad read.
- **The key track verifies.** The line `key track ... Rob Northen Copylock verified` appears for disk 1. If it says the
  key track could not be verified, re-dump with more revolutions, clean the head, or try another drive.
- **Unreadable sectors are listed** by track and sector, for example `track 3 sector 9: data checksum error in all 3
  revolutions`. `100 sectors rescued by a later revolution` is normal for a worn disk. The importer refuses an
  incomplete disk rather than guess.
- **`copy-protected tracks are only imported for the retail Xenon 2 disc 1, which this image is not`** means the disk
  has tracks that are not standard Amiga sectors and is not recognised as the retail first disk (another protected
  disk, or too many unreadable tracks to recognise it).
- **The result is a known disk.** The importer names the variant and prints the SHA-1 of what it decoded;
  `docs/re-notes/sources.md` lists the known images. A pressing that is not in the list is imported with a warning if
  it decodes, and is worth reporting.
- Keep the original flux files. They are the best record of your disk, can be re-decoded by newer versions, and are
  the only way to preserve the protection. Do not share them: they are copies of the game.

### What to send in a bug report

Send the output of `x2recomp --diagnose-image` for each disk (it holds no game data), the exact `dtc` or DrawBridge
settings and software version, the drive type, and the first lines of `x2recomp --import` including the exit code.
Do not attach disk images or flux files.

## Sources

- KryoFlux manual, command-line options and the Amiga examples (`-f`, `-i0`, `-i5`, `-s`, `-e`, `-g`, `-r`, `-p`, `-l`):
  <https://www.kryoflux.com/download/kryoflux_manual.pdf> (2009-2024 edition)
- DrawBridge project README and release notes (ADF and SCP reading, v2.5, v2.7, v2.8.6):
  <https://github.com/RobSmithDev/ArduinoFloppyDiskReader> and <https://amiga.robsmithdev.co.uk/whatsnew>
- SCP and KryoFlux stream formats, as read by the importer: `docs/re-notes/flux.md`
