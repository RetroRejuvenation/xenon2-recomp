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
| `validate.py` | Validates a pack without the game or the runtime, with the rules and the report format of `x2recomp --check-asset-pack`: `validate.py PACK [--game G] [--build TAG] [--against DUMP] [--full] [--strict] [--json]`; exit code 0 valid, 1 warnings with `--strict`, 2 invalid, 3 unreadable. |
| `make_skeleton.py` | Writes a tiny valid pack with every v2 section (placeholder keys that match nothing): `make_skeleton.py my-pack/ --scale 4`; `--check` validates a pack against the schema and the files it names. A copy is committed in `examples/asset-pack-skeleton/`. |
| `make_demo_pack.py` | Builds a runnable demo pack from your own dump (it needs a dump to know the real keys): `make_demo_pack.py DUMP my-demo-pack/ --scale 2 --game disk` (or `OUT.zip`; `--atlas N`, `--no-audio`, `--no-music`). Recolours and box-scales sprites, tiles and flashes, generates backgrounds, screens (keeping `dynamic` rectangles), a flat-glyph status panel, chirps of the same duration and `ref_period` for every sound effect and speech clip and a loop for each music slot, then validates the result. Never distribute it: it is made from your copy of the game. |

Keys are `<fnv1a64 hex16>-<w>x<h>` (graphics), `<hash16>-<length>` for sound effects and speech and `<hash16>-<span 1-8>` for status panel cells; the tools refuse paths
that are absolute, contain `..` or backslashes, and pictures larger than the runtime's limits (4096 pixels a side and 8 Mpixel; 16384 and
32 Mpixel for backgrounds). PNGs are read and written with the built-in codec (interlaced files are refused: re-save without interlacing).

`python3 -I tools/assetpack/test_reference.py` (the CTest `assetpack_reference`) checks that `docs/ASSET-PACKS-REFERENCE.md` lists exactly the fields, required fields, enum values and limits of the schema, and that the complete example manifests in the guide, the reference and the tutorials validate against it.

Run the tests with `python3 -I tools/assetpack/test_assetpack.py` (also the CTest `assetpack_tools`), `python3 -I tools/assetpack/test_demo_pack.py [--exe build/x2recomp]` (the CTest `assetpack_demo`; the data tests `demo_pack_disk` and `demo_pack_cdtv` run `demo_pack_data_test.py` on your imported game and are Skipped without it) and `python3 -I tools/assetpack/test_validate.py --exe build/x2recomp` (the CTest `assetpack_validate`; without `--exe` the cross-check with the runtime's validator is skipped).
