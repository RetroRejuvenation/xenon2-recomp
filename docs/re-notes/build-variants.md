# Game builds the runtime can start (Power Pack, Bombuzal + Xenon 2)

Phase P6b of the disk-variants work. Findings and design only: no game bytes appear here or anywhere in the
repository. Images are named by their public identity (SPS number); nothing says where an image is kept. Companion
notes: `single-disk-variants.md` (how the images are laid out and how to get the game image and disc 2 out of them),
`retail-protection.md` (the two-disk retail release, which is the *same build* as the canonical image).

## Summary

| Build (`build=` in manifest.txt) | Source | Boots | Plays | Replays recorded | Differences from canonical |
|---|---|---|---|---|---|
| `canonical` | BS1 `[t+3]` pair, "4 Others", both retail pressings | yes | yes | `tests/replays/*.hashes`, `tests/replays/retail/<pressing>/` | none (reference) |
| `power-pack-2234` | SPS 2234, one disk | yes | yes | `tests/replays/power-pack-2234/` (19) | frames identical to retail pressing SPS 0297 on all 221 recorded frame hashes; audio differs (lower-precision samples) |
| `compilation-1762` | SPS 1762, two disks | yes | yes (levels 1-5 reached, shop and ending play) | `tests/replays/compilation-1762/` (19) | a different compile of the game: no recorded frame matches canonical or retail |

All three run in the interpreter. The translated executor (`-DX2_RECOMP=ON`) is generated per image from a trace,
so the two new builds would need their own generated code; it verifies each instruction's bytes and falls back to
the interpreter, so it stays correct but is not faster for them. Not run for these builds.

## What the runtime assumed, and what changed

`boot.cpp` used to assume one image: entry word `60 00 6C 30` (`bra.w $7032`), the restart vector target `$7032`,
the disk loader at `$921A`, and a handful of probe addresses in `main.cpp` (VBL handler `$20C2`, main-loop edge
`$78CE`, disc-id compare `$8FDE`, death routine `$5D00`, the high-score table `$891E` and its save point `$8B1A`).

`runtime/boot.h` now has a table, `BuildInfo`, with those addresses per build. `load_game_assets()` recognises the
build from the image itself (`detect_build`: entry word plus the shape of the start-up code, never a hash and never the
file name) and, when `manifest.txt` carries a `build=` line, checks that it names the same build (a different build,
or one this runtime does not know, is refused with a message naming both). A manifest without the line is fine: the
image decides, so canonical imports written by importer version 1 keep working unchanged.

| | canonical | power-pack-2234 | compilation-1762 |
|---|---|---|---|
| entry | `$400` (`bra.w $7032`) | **`$762E`** | `$400` (`bra.w $6C3A`) |
| restart vector target | `$7032` | `$7032` | **`$703C`** |
| disk loader | `$921A` | `$921A` (its own routine) | **`$9012`** |
| VBL handler / main-loop edge | `$20C2` / `$78CE` | same | **`$20D0` / `$72FE`** |
| disc-id compare | `$8FDE` | none (patched out) | **`$8DD6`** |
| death routine | `$5D00` | same | **`$5D0A`** |
| high-score table / save point | `$891E` / `$8B1A` | same | **`$8340` / `$853C`** |

`Machine` gained `restart_pc` (what the restart counter watches), `DiskShim` takes the loader address, and
`main.cpp`/`persist` read the build's addresses; the report's function-name table is printed only for builds whose
code is laid out like the canonical one. The canonical values are exactly the previous constants, so nothing moved for
canonical or retail images: the 19 canonical and 38 retail replay manifests still match.

## The Power Pack

The image is the first retail pressing's code (frames equal pressing SPS 0297 everywhere) with its own front end:

- **Start-up.** The canonical and retail images begin `bra.w $7032`; the Power Pack's loader instead jumps to `$762E`:
  `move.l #key,d0 ; moveq #1,d1 ; move.l d0,$24.w ; add.w d0,d1`, then the shared initialisation at `$763A`/`$763C`.
  That is exactly where the retail Copylock routine ends up (`$763A`), minus the check: the Power Pack's Speedlock lives
  in its intro, which the runtime never runs, so **no key-track receipt is needed** and `copylock.txt` is not used.
  The immediate stored by the first instruction is passed on by the original loader and not used by anything the game
  does later in the replays; it is not compared by `detect_build`.
- **Disk loader.** `$921A` is a different routine (own file table at `$9512`, keyed by file id = module start sector),
  but the call convention the game uses is the same, so the existing shim answers it; replays reach every level module,
  the shop and the ending.
- **No disc-id check.** The loader call at `$8FD2` and the compare at `$8FDE` are patched out in this build, so the
  runtime's disc-id milestone probe is not installed for it.
- **Samples.** Two sample blocks (`$218FE`, `$2BAA2`) hold lower-precision data (low 0-4 bits dropped). Frame hashes
  therefore equal the retail ones, while every audio hash differs.
- **The low area.** The packed intro leaves `$400`-`$E0C` to the intro itself; the game never reads it once running, so
  the recipe (`tools/variants/write_assets.py power-pack`) writes the canonical-shaped stub there and zeros. This also
  means a restart through `$7032` (the exception vectors the game installs) lands in zero words (`ori.b #0,d0`), which
  execute as harmless instructions up to the start-up code at `$762E`, where the game re-initialises. `level4_flood`
  restarts once this way (both retail pressings restart there too) and its frame hashes still equal the retail ones; the
  slide itself was not traced. On real hardware the intro's own leftovers would be in that area.
- **Disc 2** is rebuilt functionally (sector 0 disc id, six modules at their start sectors, zeros elsewhere); it is
  not byte-identical to the canonical disc 2 (61 sectors differ, all unused). All 19 replays pass with it. One module
  differs by a single byte (module 2, offset 3).

## Bombuzal + Xenon 2 (SPS 1762)

A different compile of the game: the code after `$D000` is shifted (about -1502 bytes), the boot entry is
`bra.w $6C3A`, and the start-up is `move #$2700,sr ; lea $435FC,a7 ; ...` at `$703C` (no Copylock; track 1 of the
disk is a plain fill). The earlier P6 note said it "never reaches its first VBL handler". Cause: the shim only
intercepted `$921A`, which in this build is ordinary code; its loader is at `$9012` (same routine, different address).
With that address the game starts, reaches the title, plays levels 1-5 and the shop, and runs the ending
(contact sheets were inspected for the title, level 1, level 3, the shop, the shop-5 sequence and the ending).

Differences, measured, not explained in depth:

- **Replays do not match canonical or retail** at any recorded hash except a few identical frames in `ending*` (3 of
  221 against canonical, 13 of 221 against retail 2874). The scripted inputs, written for the canonical timing, land
  differently in this build, so each replay plays its own game; the manifests are goldens of this build only.
- **Variables are at the same RAM addresses** as canonical (the pokes the scripts use for energy, ships, level,
  scroll position work), so the replay corpus reaches the same *kinds* of state: `ending_natural` ends with the game
  looping back to level 1 as in the canonical build (the final score differs: 750 vs 1050).
- **Module 6 (shop and ending)** is the build's own (calls routines at this build's addresses); modules 1-4 are the
  canonical ones. `pokeobj` handler addresses used by the boss replays are module code, so they work for modules 1-4.
- Not investigated: why timing differs (likely the shifted code and a different start-up cost), whether any enemy or
  level data differ beyond module 5's last sector and module 6, and the audio against the canonical tune.

## What the importer (P3d) must provide

- `build=power-pack-2234` / `build=compilation-1762` in `manifest.txt` (any other value is refused unless the runtime
  has the build). Canonical and retail imports need no line.
- Power Pack: `game_0400.bin` = the main image at `$E0C` over the canonical-shaped low area (the stub `60 00 6C 30` and
  zeros) as `tools/variants/write_assets.py power-pack` writes it, and a functional `disc2.bin` (disc id plus the six
  modules at sectors 11, 250, 514, 799, 1154, 1476). No `copylock.txt`.
- 1762: `game_0400.bin` = the plain image at disk-1 offset `$70C00`, `disc2.bin` = disk 2 unchanged.
- The importer's recognition of these disks is by the WHDLoad boot-block CRC16 (v6 Power Pack, v2 compilation) and
  decoded-image hashes, in the variant layer (`single-disk-variants.md`).
- Goldens live in `tests/replays/<build>/`; the CTest names are `build_<build>_<replay>` and `build_table` (synthetic).

## Reproducing

```
python3 tools/variants/write_assets.py power-pack  SPS2234.ipf               generated/assets-power-pack-2234
python3 tools/variants/write_assets.py compilation-1762  DISK1.ipf DISK2.ipf generated/assets-compilation-1762
cmake -DX2_POWER_PACK_ASSETS=... -DX2_COMPILATION_1762_ASSETS=... ; ctest -R build_
python3 tools/trace/run_corpus.py --assets DIR --record-hashes --no-trace --hashes-out tests/replays/<build>
```

## Not determined

- The Power Pack intro's behaviour beyond what the loader needs; whether anything reads the 143 unused disc 2 sectors.
- Whether the 1762 replays' divergence is purely timing; no frame-by-frame comparison against an emulator was made for
  either build.
- The translated executor for these builds.
