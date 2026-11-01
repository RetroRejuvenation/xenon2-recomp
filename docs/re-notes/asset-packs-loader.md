# Asset packs v2: the loader, the library and the validator (workstream WS1b)

What `runtime/pack` does between a pack on disk and the `PackLibrary` that the presenter and the audio stage read. The format is
`asset-packs-v2-design.md` section 3 and `docs/schema/x2-asset-pack-v2.schema.json`; this note records how it is implemented, which
decisions the implementation had to make where the design left room, and what the later workstreams (WS2 graphics, WS3b sound effects,
WS4 music) get from it.

## Files

| File | Content |
|---|---|
| `pack_api.h/.cpp` | The contract (WS1a), plus the additions listed at the end of this note. |
| `pack_limits.h` | `PackLimits`: every limit of design section 3.5 in one struct (the v1 limits unchanged). |
| `pack_json.*` | Strict JSON reader (RFC 8259: no comments, trailing commas, `NaN`, hex or leading-zero numbers), depth 32, 1,000,000 values. |
| `pack_paths.*` | Path rules (`safe_pack_path`, `path_problem`), ASCII case folding, backslash normalising for ZIP names. |
| `pack_png.*` | The PNG decoder (moved from `modern/assets.cpp`, unchanged behaviour) with a header probe (`png_size`). |
| `pack_source.*` | `PackSource`: a directory or a ZIP, read lazily; `ByteSource` streams. |
| `pack_manifest.*` | `parse_manifest`: manifest.json into typed declarations (`Manifest`), every syntax and range check, scoping fields, unknown fields. Also semver and constraint helpers. |
| `pack_stack.*` | `PackStack`: loads packs one after another and **is** the `PackLibrary` (layering, scoping, atlas expansion, pictures, audio files). |
| `pack_discovery.*`, `pack_load.cpp` | `<data dir>/packs/` scanning, the load order of a run (`load_packs`, `load_pack_stack`), `--list-asset-packs`. |
| `pack_check.*` | The validator behind `--check-asset-pack`. |
| `modern/assets.*` | `AssetLibrary`: the version 1 interface over a `PackStack` (the interpolator still reads `Replacement`, `FarRow`, `PaletteOverride`), `decode_png`, `resample` and `AssetDumper` as before. |
| `tools/assetpack/validate.py` | The same validator in pure Python (no runtime, no game data). |

## The order of things

`load_pack_stack(settings, context, report)` (called once by `main.cpp`, after the game and build are known):

1. the ids in `packs_enabled` are looked up in `<data dir>/packs/` (`discover_packs`), in the listed order; an unknown id is a warning;
2. then `asset_packs` from `settings.cfg` and each `--asset-pack`, in order;
3. a pack named twice (same canonical path) is loaded once;
4. each pack goes through `PackStack::add`, which returns false for a refused pack (it stays in `packs()` with `usable = false` and the reason in
   `problem`, so `FileRef::pack` indices stay stable) and carries on with the next.

`PackStack::add` per pack:

1. open the directory or ZIP (`pack_source`); read and parse `manifest.json` (a failure here refuses the pack);
2. `parse_manifest`: top-level fields, then each section. A bad entry is an Error message and is skipped (`PackInfo::skipped_entries`); the rest of the pack is used;
3. `requires`: `x2recomp` against the runtime's version (`X2_VERSION`; no check when it is unknown) and `packs[]` against the packs loaded before it (id and
   `pack_version`); a failed requirement refuses the pack with one message;
4. scoping: the pack's `games`/`builds` intersected with each entry's; a pack none of whose entries can apply is loaded with an Info line, not an error;
5. the entries are loaded: pictures decoded once per file and accounted against `max_total`; atlas frames cropped (copies, accounted); flash pictures; far strips checked
   (width, rows); backgrounds checked; audio files probed (and decoded for clips); music files checked for existence and probed;
6. a later pack overrides an earlier one **per key**; the first 20 overrides of a pack are reported (Info), the rest counted.

The library is read-only after loading; `PackStack` has no locks. `ByteSource` streams returned by `open_file` are independent per call; ZIP members are read
through a mutex-guarded handle, stored members stream straight from the archive file through their own handle.

## Decisions where the design left room

- **`f:` keys.** The design (4.6) falls back to explicit flash pictures keyed by mask shape. They are written as `graphics` keys with a prefix,
  `"f:<hash>-<w>x<h>": {"png": ...}`, loaded into `flash()`, never into `graphic()`. The schema's `graphics` key pattern and `picture.keys` accept the prefix.
- **Panel cell schema.** `panel.cells` entries are `$defs/panelCell` (schema): like a picture, but extra `keys` are panel keys (`<hash>-<1..8>`) and there is no flash picture.
- **Stars.** The manifest `size` is a number (0.25-8). `StarsEntry::size` is the rounded integer (at least 1), `size_px` the number as written.
- **A screen whose `signature.min` exceeds its block count** can never match; it is loaded with a warning (the schema example uses a placeholder like that).
- **`wide_scale`** is resolved at load: the screen's own `wide_scale`, else the pack's top-level `scale` (the API says "the pack's scale").
- **Music slot gain.** `defaults.gain_db` is baked into the pack's slots that do not set `gain_db` (`MusicSlot::gain_db`); the other defaults (`crossfade_ms`, `fade_in_ms`,
  `fade_out_ms`, `loop_mode`, `retrigger`) are merged per field across packs into `music_defaults()` and left unset in the slots for the music stage to resolve.
- **`mute` with a file** keeps the file in the slot; the music stage decides (the validator does not object).
- **Layering of `panel`.** The latest pack that declares a panel sets `scale`, `cell_w`, `max_span`; cells merge per key; a later `background` replaces the earlier one.
- **Version 1 manifests with v2 sections** (`screens`, `audio`, ...) or `requires`: each section is an Error and is ignored. The same manifest was silently read by a v1 runtime.
- **Version above 2:** the pack is refused with `"version" N needs a newer x2recomp`.
- **No codec, no clips.** `audio.sfx` and `audio.speech` entries need decoded samples; with no `AudioCodec` registered (`set_audio_codec`, see below) they are not
  registered and one Info line says so. Music slots are still registered (existence checked, headers probed when a codec exists).

## ZIP

`pack_source.cpp` reads the end-of-central-directory record, then the central directory (at most 64 MB, at most 65,536 members), and keeps only the table. Members are
read on demand: the local header is re-checked, the data read, stored members must have `csize == usize`, deflate members go through `inflate_raw` (the project's puff) with the
declared size, and the CRC-32 must match. Refused: ZIP64, multi-disk archives, encrypted members, methods other than stored and deflate, a member declaring more than
512 MB, a deflated member with a ratio above 1000:1 unless it unpacks to at most 1 MB (checked before anything is inflated). Names: backslashes read as `/` (with a note), folders and
`__MACOSX/` skipped, names failing `path_problem` ignored with a note (the pack still opens, the entries that name them report "missing file"), two members that collide after case
folding (or are the same name twice) refuse the pack. The pack root is the archive root when `manifest.json` is there, else the single top-level folder that holds one.

A corrupt member that nothing reads does not affect the pack (test `zip: lazy members`).

## The validator

`x2recomp --check-asset-pack PATH [--game G] [--build TAG] [--against DUMP] [--full] [--strict] [--json]` and `tools/assetpack/validate.py` run the loader in validate mode
(`StackOptions::validate`: no scoping, unknown and duplicate fields reported, the manifest and decoded pictures kept) and then `Checker` (`pack_check.cpp`):

- picture sizes against the key and scale (graphics, flash pictures, atlas frames, panel cells and background, screens), far strips and backgrounds (the loader already refuses wrong widths);
- keys defined twice (graphics and atlases), `groups` keys that nothing defines, unused files, the preview, `requires` as information, a memory estimate (a warning above 80%);
- audio: peak above full scale (needs a decoded file: `--full`), loop points inside the file (two frames of rounding tolerated), a deflated music member in a ZIP (warning);
- `--against DUMPDIR`: coverage per kind, keys the dump never saw, byte-identical copies of the dump's files (SHA-1);
- with `--game`/`--build`: how many entries apply, and a warning when the pack's scope excludes that game and build.

Severity: **Error** = the loader skips the entry or refuses the pack (exit 2); **Warning** = loaded but probably wrong (exit 1 with `--strict`); **Info** = for the author.
Exit codes: 0 valid, 1 warnings with `--strict`, 2 unusable pack or invalid entries, 3 the path cannot be read (missing, not a directory or a readable ZIP). With `--strict` a
missing license is an Error. The text format is `error|warning|info: asset pack: <pack>: <section>["<key>"]: <text>` and a `check:` summary line; `--json` is an array of
`{severity, pack, section, key, text, message}` with a final `{"severity": "summary", errors, warnings, entries, skipped, exit_code}` object.

Without `--full` audio is probed (headers) but not decoded; pictures are always decoded, because loading them is the check.

`validate.py` implements the same rules from the schema (`minischema.py`) plus the checks above; it reads WAV itself and checks FLAC and Ogg by signature only, and it applies no scoping.
`tools/assetpack/test_validate.py` runs both on 20 packs and requires the same exit code (CTest `assetpack_validate`).

## Additions to `pack_api.h`

All additive; nothing that WS1a defined changed meaning.

- `StarsEntry::size_px`, `PanelSet::background_anchor`, `PackInfo::problem`.
- `AudioInfo`, `AudioLimits`, `AudioCodec` and `set_audio_codec` / `audio_codec`: the loader reads sound files through two function pointers (`probe` for the header,
  `decode` to interleaved int16) so that it does not link the decoders. **WS3b registers the codec** at start-up with an adapter over `runtime/audio/decode.*` (its `ByteSource`
  is stream based, the pack's is offset based; a few lines each way).
- `load_pack_stack` (`pack_stack.h`): `load_packs` with the concrete `PackStack`, for adapters.

## Screens and the panel

Since WS2a the screens and panel passes read their data from the `PackStack` (`pack::packs()`), installed by `main.cpp` for the disk and CDTV runs and by `st_run.cpp` for
the ST run (it uses `pack_settings()`, set before the ST runner starts). WS2a's interim directory-only reader was removed (`ScreenPackLibrary::load`); the class stays as an
in-memory library for tests. Reconciled with WS2a: `ref_palette` takes `#rgb` and `#rrggbb` (screens and panel), `panel` has `ref_palette` and `fade` (later packs override
them per field, as the panel's `scale`, `cell_w` and `max_span`), and a screen whose `signature.min` exceeds its blocks is an Error (the schema example was changed to `min: 1`).
The `screens` tests load the same dump from a directory and from a ZIP and require identical frames (`x2_screens_tests`, and step 2b of `tests/modern_screens.cmake`).

## What the later workstreams get

- **WS2:** `graphic()`, `flash()`, `far_line()`, `background()`, `stars()`, `palette()`, `panel()`, `screens()`/`screens_for_block()` are filled and tested. The interpolator
  still reads graphics, far lines and palettes through `AssetLibrary`, which mirrors the stack after loading (`adopt`); the other lookups are for the new passes. Atlas frames and `keys`
  aliases are already expanded into `graphic()` entries.
- **WS3b:** `sfx()`, `speech()`, `speech_for_file()`, `has_clip_length()` (a set lookup on the sample byte length before any hashing) return decoded clips with every playback option parsed.
- **WS4:** `music_slot()` returns slots with `FileRef`s; `open_file()` gives a seekable `ByteSource` (stored ZIP members and directory files stream, deflated members are inflated once per call);
  `music_defaults()` holds the merged defaults.

## Tests

- `tests/pack_loader_tests.cpp` (CTest `pack_loader`, about 6,700 checks): JSON, paths, versions, PNG, ZIP, manifest parsing and every refusal, the loader for each section,
  scoping, layering, requires, limits, audio through a test WAV codec (clips, speech, music slots, probe-only, no codec), discovery and the load order, the validator (findings, exit codes,
  `--against`, JSON), the version 1 view, and the fuzz: 20,000 mutated manifests (a third by bytes, the rest by editing the parsed tree so that most reach the parser's checks), 5,000 ZIPs and
  5,000 PNGs; `x2_pack_loader_tests SOURCE SCRATCH [iterations]`. Run under AddressSanitizer and UBSan it is clean (CI: the sanitizer job is WS7).
- `tests/pack_cli.cmake` (`pack_cli`): the commands on small packs written by the script.
- `tests/modern_tests.cpp` (`modern`) runs unchanged: every version 1 test, message substring and rendering check passes through the adapter.
- `tools/assetpack/test_validate.py` (`assetpack_validate`).

## Not done here

- Applying anything but graphics, far lines and palettes (WS2, WS3b, WS4).
- Decoders (WS3a) and the registration of the codec (WS3b).
- `tools/assetpack/validate.py` checks FLAC and Ogg by signature only.
- A libFuzzer harness and a nightly fuzz job (WS7); the mutation fuzz runs in the normal test.
