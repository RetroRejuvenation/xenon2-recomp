# Asset pack authoring tools

Helpers for people who make asset packs (higher-resolution graphics, backgrounds, artwork, sound) for the recompiled games. They need only
Python 3 (standard library); nothing here contains or produces game data other than from dumps you make yourself. The pack format is
described in `docs/ASSET-PACKS.md` and `docs/re-notes/asset-packs-v2-design.md`; the manifest schema is
`docs/schema/x2-asset-pack-v2.schema.json`.

| Tool | What it does |
|---|---|
| `dump_all.sh` | Runs every replay script of a build with `--dump-assets` and merges the results into one dump: `dump_all.sh -g disk -a ASSETS -x build/x2recomp -r tests/replays -o dump` (`-g disk\|cdtv\|st`, `-f FRAMES`, `-k` keeps the per-replay parts). |
| `merge_dumps.py` | Merges several `--dump-assets` folders: `merge_dumps.py OUT DUMP1 DUMP2 ...` (brighter copy of a picture wins, palettes remapped, far-layer strips rebuilt). |
| `contact_sheet.py` | A labelled overview picture of a dump or pack: `contact_sheet.py DUMP --out sheet.png --scale 3`. |
| `slice.py` | Cuts a sheet into per-key pictures: `slice.py sheet.png --grid 64,48 --keys keys.txt --out frames/`, `--frames frames.json`, or `--manifest manifest.json --all`. |
| `pack_atlas.py` | Packs per-key pictures into a sheet and writes its `atlases` entry: `pack_atlas.py frames/ --out-png sheets/ships.png --into manifest.json --scale 4 --padding 2 --bleed 2`. |
| `make_skeleton.py` | Writes a tiny valid pack with every v2 section (placeholder keys that match nothing): `make_skeleton.py my-pack/ --scale 4`; `--check` validates a pack against the schema and the files it names. A copy is committed in `examples/asset-pack-skeleton/`. |

Keys are `<fnv1a64 hex16>-<w>x<h>` (graphics), `<hash16>-<length>` for sound effects and speech and `<hash16>-<span 1-8>` for status panel cells; the tools refuse paths
that are absolute, contain `..` or backslashes, and pictures larger than the runtime's limits (4096 pixels a side and 8 Mpixel; 16384 and
32 Mpixel for backgrounds). PNGs are read and written with the built-in codec (interlaced files are refused: re-save without interlacing).

Run the tests with `python3 -I tools/assetpack/test_assetpack.py` (also the CTest `assetpack_tools`).
