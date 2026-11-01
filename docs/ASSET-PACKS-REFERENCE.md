# Asset pack reference

Every field of `manifest.json`, with its type, default and limits, and what the loader and the checker say when a value is wrong. The
guide (`docs/ASSET-PACKS.md`) explains what the sections are for; this page is for looking things up. The machine-readable form is
`docs/schema/x2-asset-pack-v2.schema.json`, and a test (`tools/assetpack/test_reference.py`) fails when a table below and the schema
disagree on a field name, a required field, an enum value or a numeric limit.

## How to read the tables

- **Default** is what the loader uses when the field is absent. **required** means the entry is skipped (or the whole pack refused,
  for `format` and `version`) without it.
- **Entry** means one item of a section (one graphic, one screen, one clip). **A bad entry is skipped** and reported as an error; the
  rest of the pack is used. A bad value in the pack's own descriptive fields (`name`, `id`, `pack_version`, ...) is reported and that
  field is ignored.
- `games` and `builds` can appear on the pack and on every entry (see "Scoping"). An entry applies when the game that runs and the build
  are in both lists.
- Unknown fields are ignored by the game and reported as warnings by `--check-asset-pack` (a dump adds many descriptive fields; the
  checker says each once, with a count).

## Syntax shared by all sections

| Kind | Form | Notes |
|---|---|---|
| Path | a relative path inside the pack, at most 240 bytes | no absolute path, drive letter, backslash, `.` or `..` component, doubled slash, control character or Windows device name (`CON`, `NUL`, ...); a directory pack may not reach outside itself through a link; in a ZIP two names that differ only by upper and lower case refuse the pack |
| Graphic key | `<16 hex digits>-<w>x<h>`, for example `0123456789abcdef-16x12` | the hash of the picture as the game draws it and its size in Amiga pixels; written by `--dump-assets` |
| Flash key | `f:` and a graphic key | the hit-flash silhouette of a mask shape (a dump writes these as `kind: flash`) |
| Panel key | `<16 hex digits>-<n>` with `n` from 1 to 8 | the status panel's `n` cells in a row; written by `--dump-assets` |
| Sample key | `<16 hex digits>-<bytes>` | the hash and byte length of a sound sample; written by `--dump-audio` |
| Line key (`far`) | 16 hex digits, or `-` or an empty string for an unused row | the hash of one 320-pixel line of the far layer |
| Colour, Amiga | `#rgb` | the 12-bit colour the game uses; written in a dump's `seen_palettes` |
| Colour, picture | `#rrggbb` (palettes `colors`); `#rgb` or `#rrggbb` (screen and panel `ref_palette`) | |
| Scale | an integer from 1 to 8 | picture pixels per Amiga pixel; the pack's `scale` is the default for entries |
| Anchor | `[x, y]`, each from -4096 to 4096 | the picture pixel that is placed on the original's top-left corner |
| Rectangle | `[x, y, w, h]` | in pixels of the picture it is cut from |
| Slot name | `title`, `level`, `level1` to `level5`, `cd2` to `cd13` | music slots; see the guide for which game uses which |
| Screen name | lowercase letters, digits, `_` and `-`, at most 48, starting with a letter or digit | the key in `screens` |
| Build tag | lowercase letters, digits and `-`, at most 48 | `canonical`, `power-pack-2234`, `compilation-1762`, `pdos-v5`, `cdtv`, `atari-st-kixx` |
| Pack id | `^[a-z0-9][a-z0-9._-]{1,63}$` | |
| Version | `1.2.3`, or `1.2.3-beta` / `1.2.3+build` | `pack_version` |
| Version constraint | `>=`, `>` or `=` and then `x.y` or `x.y.z` (no operator means `>=`) | `requires` |

## The manifest

<!-- fields: root -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `format` | string | **required** | `x2-asset-pack` | Any other value: the pack is not used. |
| `version` | integer | **required** | 1 or 2 | 1 allows only `graphics`, `far`, `palettes`, `scale` and the descriptive fields; the v2 sections in a version 1 manifest are reported as errors and ignored. A version above 2 refuses the pack with "needs a newer x2recomp". |
| `name` | string | the folder or file name | 1 to 80 bytes | Shown in lists and in messages. |
| `id` | string | made from `name` | `^[a-z0-9][a-z0-9._-]{1,63}$` | The name used by `packs_enabled` and by other packs' `requires`. Two loaded packs with the same id give a warning, and the later one wins. |
| `pack_version` | string | none | `1.2.3` or `1.2.3-beta` | Compared by `requires` of other packs. |
| `description` | string | none | at most 2000 bytes | |
| `author` | string | none | at most 200 bytes | |
| `url` | string | none | at most 400 bytes | |
| `license` | string | none | 1 to 200 bytes | Say under which terms the pack may be used and shared. A missing licence is a warning, and an error with `--strict`. |
| `credits` | array | none | at most 200 entries | People and sources. |
| `preview` | path | none | a picture inside the pack, path at most 240 bytes | Shown by tools; the checker warns above 1 MB. |
| `provides` | array of strings | none | `graphics`, `screens`, `audio` | What the pack contains, for lists. Not checked against the sections. |
| `games` | array of strings | all three | `disk`, `cdtv`, `st` | The pack is used only for these games. `disk` covers the canonical image, the retail pressings, the Power Pack, the 1762 compilation, the PDos build and the cracks. |
| `builds` | array of strings | all | build tags | Narrows `games` to some builds of the disk game by the manifest's `build=` tag. |
| `requires` | object | none | | Version 2 only. |
| `scale` | integer | 1 | 1 to 8 | The default picture scale of the entries. |
| `graphics` | object | none | at most 65536 entries in the whole pack | Sprites and tiles by key (and `f:` flash keys). |
| `far` | array | none | | Far-layer strips by line hash (version 1 form). |
| `palettes` | array | none | | Palette overrides. |
| `backgrounds` | array | none | | Version 2: a level's far layer as one picture. |
| `atlases` | array | none | | Version 2: sprite sheets that expand into `graphics`. |
| `groups` | object | none | each value a list of keys | Version 2, for tools only: named lists of graphic keys. A key that nothing defines is a warning. |
| `stars` | object | none | | Version 2: how the starfield is drawn. |
| `screens` | object | none | at most 256 screens | Version 2: whole-screen pictures. |
| `panel` | object | none | | Version 2: the status panel, cell by cell. |
| `audio` | object | none | | Version 2: sound effects, speech and music. |

<!-- fields: credits[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `name` | string | **required** | at most 200 bytes | |
| `role` | string | none | at most 200 bytes | |
| `url` | string | none | at most 400 bytes | |

<!-- fields: requires -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `x2recomp` | string | none | a version constraint such as `>=0.3` | The pack is refused when the running x2recomp does not satisfy it (no check when the version is unknown). |
| `packs` | array | none | at most 16 entries | Packs that must be loaded **before** this one, in the load order. A missing or too old one refuses this pack with one message. Nothing is fetched or reordered for you. |

<!-- fields: requires.packs[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `id` | string | **required** | a pack id | |
| `version` | string | any | a version constraint | |

## Graphics

<!-- fields: graphics{} -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `png` | path | **required** | path at most 240 bytes | The picture. Normally `w * scale` by `h * scale` pixels for the `w` and `h` of the key; another size is allowed (use `anchor`), and the checker warns. |
| `scale` | integer | the pack's | 1 to 8 | Picture pixels per Amiga pixel for this entry. |
| `anchor` | array of 2 integers | `[0, 0]` | each -4096 to 4096 | Lets the picture overhang the original, for a glow or a shadow. |
| `flash_png` | path | derived | path at most 240 bytes | The picture drawn while the game shows this graphic as a hit flash. Without it the flash is the replacement blended 70 % toward white. |
| `keys` | array of strings | none | at most 64 graphic keys (flash keys for an `f:` entry) | More keys that use the same picture, for a graphic that has another hash in another build. |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

The entry's own key (the key of the object) must be a graphic key or a flash key. The keys of `groups` and `atlases` are checked the same way.

<!-- fields: far[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `png` | path | **required** | path at most 240 bytes | A strip `320 * scale` pixels wide; row block `i` (`scale` rows) replaces the far-layer line whose hash is `lines[i]`. |
| `lines` | array of strings | **required** | at least 1 entry; each a line key, `-` or an empty string | One entry per row block. Identical original lines get the same replacement row. |
| `scale` | integer | the pack's | 1 to 8 | |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

<!-- fields: palettes[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `match` | array of strings | **required** | exactly 16 colours `#rgb` | The level's 16 original colours. The override applies only while the game's palette is exactly this, so during a fade the original colours show. |
| `colors` | array of strings | **required** | exactly 16 colours `#rrggbb` | The replacement colours, index by index. |
| `layers` | array of strings | all layers | `far`, `tiles`, `objects`, `stars`, `other` | Which layers get the colours. A layer list lets the far layer have colours of its own, which the shared 16-colour palette does not allow. |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

The latest loaded pack whose `match` equals the level's palette wins.

<!-- fields: backgrounds[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `level` | integer | **required** | 1 to 5 | The level whose far layer the picture replaces. It wins over `far` lines for that level. |
| `png` | path | **required** | path at most 240 bytes | The far layer as one picture, `320 * scale` pixels wide and `192 * scale` tall, wrapping vertically. A dump writes it as `far_full/levelN.png`. |
| `scale` | integer | the pack's | 1 to 8 | |
| `ring_lines` | integer | 192 | 1 to 4096 (only 192 works) | The height of the repeating layer in lines. The loader accepts up to 4096 and refuses a picture whose height is not `ring_lines * scale`, but the far layer is a 192-line ring on the disk and CDTV builds, so a background that declares another value is ignored with one message. Leave it out or write 192. |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | The Atari ST has no backgrounds (no capture path), so only `disk` and `cdtv` apply. |
| `builds` | array of strings | all | build tags | |

<!-- fields: atlases[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `png` | path | **required** | path at most 240 bytes | The sheet. It counts once toward the memory limit; its frames are cut out at load time. |
| `scale` | integer | the pack's | 1 to 8 | |
| `frames` | array of objects | none | | Frames at stated rectangles. `frames` or `grid` is required. |
| `grid` | object | none | | Equal cells laid out row by row. `frames` or `grid` is required. |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

<!-- fields: atlases[].frames[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `key` | string | **required** | a graphic key | |
| `rect` | array of 4 integers | **required** | `[x, y, w, h]`, each 0 or more, `w` and `h` at least 1 | The frame in the sheet. It must lie inside the picture and, at the entry's scale, be `w * scale` by `h * scale` for the key's `w` and `h`. |
| `anchor` | array of 2 integers | `[0, 0]` | each -4096 to 4096 | As for `graphics`. |
| `flash_rect` | array of 4 integers | derived | `[x, y, w, h]`, each 0 or more | The frame's hit-flash picture in the same sheet. |

<!-- fields: atlases[].grid -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `cell` | array of 2 integers | **required** | `[width, height]`, each at least 1 | The cell size in sheet pixels. Every keyed cell must equal the key's size times the scale. |
| `columns` | integer | as many as fit the sheet | at least 1 | Cells per row. |
| `keys` | array | **required** | graphic keys or `null` | One key per cell, row by row; `null` leaves a cell empty. |

<!-- fields: stars -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `shape` | string | `pixel` | `pixel` or `round` | `pixel` is the original, byte for byte. `round` draws an anti-aliased disc over each star the game plots. |
| `size` | number | 1 | 0.25 to 8 | The disc's diameter in Amiga pixels; fractions are allowed. |
| `softness` | number | 0.5 | 0 to 1 | The width of the soft edge as a share of the radius. |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | Only the disk and CDTV games draw stars through a pack. |
| `builds` | array of strings | all | build tags | |

## Screens and the status panel

<!-- fields: screens{} -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `signature` | object | **required** | | How the screen is recognised. |
| `png` | path | **required** | path at most 240 bytes | `320 * scale` by `200 * scale` pixels (the window area); `anchor` lets it overhang. |
| `scale` | integer | the pack's | 1 to 8 | |
| `anchor` | array of 2 integers | `[0, 0]` | each -4096 to 4096 | |
| `ref_palette` | array of strings | none | 2 to 32 colours, `#rgb` or `#rrggbb` | The full-brightness palette of the screen, indexed by the game's colour numbers. Without it, or with `fade` set to `none`, the picture stays at full brightness while the game fades. |
| `fade` | string | `scale` | `scale` or `none` | `scale` fades the picture with the game's own fade-in and fade-out. |
| `bg_index` | integer | 0 | 0 to 31 | The colour number the game uses for "nothing" inside `dynamic` rectangles. |
| `dynamic` | array | none | at most 32 rectangles | Parts the game keeps drawing itself (scores, names, shop prices): inside them the original pixels whose colour number is not `bg_index` are drawn over the picture. |
| `wide_png` | path | none | path at most 240 bytes | Used in place of black sides when widescreen is on; any width, drawn centred and top-aligned. |
| `wide_scale` | integer | the pack's | 1 to 8 | |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

<!-- fields: screens{}.signature -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `block` | integer | 16 | 8, 16 or 32 | The block size in pixels. |
| `min` | integer | **required** | 1 to 64, not more than the number of blocks | The blocks that must match. A screen with `min` above its block count could never match and is refused. |
| `blocks` | array | **required** | 1 to 64 entries | Blocks of the screen's colour numbers, each a hash. A dump writes 29 to 40 of them; the checker warns below 6. |

<!-- fields: screens{}.signature.blocks[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `x` | integer | **required** | 0 to 4095 | Left edge of the block in window pixels. |
| `y` | integer | **required** | 0 to 4095 | Top edge. |
| `hash` | string | **required** | 16 hex digits | The hash of the block's colour numbers. |

<!-- fields: screens{}.dynamic[] -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `x` | integer | **required** | 0 or more | In Amiga window pixels. |
| `y` | integer | **required** | 0 or more | |
| `w` | integer | **required** | 1 or more | |
| `h` | integer | **required** | 1 or more | |

<!-- fields: panel -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `scale` | integer | the pack's | 1 to 8 | The scale of the cell pictures and of `background`. |
| `cell_w` | integer | 8 | 8 or 16 | The cell width in Amiga pixels. The strip is window lines 192 to 199 on every build. |
| `max_span` | integer | 6 | 1 to 8 | The longest run of cells a key may cover. At each column the longest matching run wins. |
| `ref_palette` | array of strings | none | 2 to 32 colours, `#rgb` or `#rrggbb` | As for screens: lets the panel fade with the game. |
| `fade` | string | `scale` | `scale` or `none` | |
| `background` | object | none | | A picture drawn first over the whole strip, so cell pictures may be transparent. |
| `cells` | object | none | | Replacement pictures by panel key. |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

<!-- fields: panel.background -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `png` | path | **required** | path at most 240 bytes | `320 * scale` by `8 * scale` pixels. |
| `anchor` | array of 2 integers | `[0, 0]` | each -4096 to 4096 | |

<!-- fields: panel.cells{} -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `png` | path | **required** | path at most 240 bytes | `8 * n * scale` by `8 * scale` pixels for a key of `n` cells. A picture with an `anchor` may be another size. |
| `scale` | integer | the panel's | 1 to 8 | |
| `anchor` | array of 2 integers | `[0, 0]` | each -4096 to 4096 | Lets a picture overhang the strip over the playfield. |
| `keys` | array of strings | none | at most 64 panel keys | More keys that use the same picture. |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

## Audio

`audio.sfx` and `audio.speech` entries share one set of fields. On the disk builds only effects are replaced; `speech` exists on the
CDTV release.

<!-- fields: audio -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `sfx` | object | none | sample key to clip | Replacement sound effects. |
| `speech` | object | none | sample key to clip | Replacement speech (CDTV release). |
| `music` | object | none | | Replacement music. |

<!-- fields: audio.sfx{} audio.speech{} -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `file` | path | **required** | path at most 240 bytes | WAV, FLAC or Ogg Vorbis, one or two channels, 8000 to 192000 Hz, at most 120 seconds and 32 MB decoded. |
| `ref_period` | integer | the period of each start | 124 to 65535 | The Paula period at which the file sounds like the original played normally; the voice's speed is `ref_period` divided by the game's current period. A dump writes it. |
| `gain_db` | number | 0 | -60 to 24 | Applied on top of the game's own volume. |
| `pan` | string or number | `channel` | `channel`, or a number from -1 (left) to 1 (right) | `channel` puts a mono file on the side of the Paula channel that started it. Stereo files play as they are. |
| `pitch` | string | `follow` | `follow` or `fixed` | `follow` tracks the game's period changes; `fixed` plays at the file's own rate. |
| `loop` | boolean | false | true or false | Loops the file while the original channel keeps replaying the same sample (a dump sets it for sounds the game loops). |
| `cut` | string | `channel` | `channel` or `never` | `channel` ends the voice (with `fade_out_ms`) at the channel's next start or when it goes idle, as the original does; `never` lets it ring out. |
| `fade_out_ms` | integer | 5 | 0 to 2000 | |
| `original` | string | `mute` | `mute`, `mix`, or `mix:` and a gain such as `mix:-12` or `mix:-6.5` | What stays of the original sound in the mix. A gain is in dB, up to two digits and one decimal. The machine itself always plays the original; only the mix changes. |
| `cd0_file` | string | none | an upper-case disc file name such as `SPEECH63`, at most 32 characters | Speech only (CDTV): identifies the clip by the disc file it was read from, in place of its hash. On `sfx` it is ignored with a warning. |
| `keys` | array of strings | none | at most 64 sample keys | More keys that use the same file, for a sample that has another hash in another build. |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

<!-- fields: audio.music -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `defaults` | object | none | | Values for every slot that does not set its own. Layered packs merge defaults field by field. |
| `slots` | object | none | slot name to slot | The tracks. |

<!-- fields: audio.music.defaults -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `gain_db` | number | 0 | -60 to 24 | Baked into every slot of the same pack that sets no gain of its own. |
| `fade_in_ms` | integer | 0 | 0 to 10000 | Used when a track starts from silence. |
| `fade_out_ms` | integer | 0 | 0 to 10000 | Used when a track stops and when it is replaced with no crossfade (at least 5 ms, to avoid a click). |
| `crossfade_ms` | integer | 0 | 0 to 5000 | Equal-power overlap between tracks and at seamless loops. |
| `retrigger` | string | `restart` | `restart` or `continue` | What a cue for the slot that is already playing does: start it again (as the game does after a death) or leave it running. |
| `loop_mode` | string | `pack` | `pack` or `game` | `pack` loops by the pack's own points. `game` keeps the original's rule: the game's own poll restarts a finished track from its start, up to a second late. |

<!-- fields: audio.music.slots{} -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `file` | path or null | none | path at most 240 bytes; `null` only together with `mute: true` | The track (WAV, FLAC or Ogg Vorbis, at most two hours). A slot needs a `file` or `mute: true`. |
| `intro` | path | none | path at most 240 bytes | Played once before `file`; resampled to the body's rate if it differs. A broken intro drops the slot. |
| `loop` | boolean or object | the file's own loop, else the game's rule | true, false, or `{start_s, end_s}` | true loops the whole body, false plays it once and then silence, an object loops those points of the body file. A loop point past the end of the file is an error. |
| `mute` | boolean | false | true or false | Silence in place of the original music. A `file` left in place is not read. |
| `gain_db` | number | the defaults' | -60 to 24 | |
| `fade_in_ms` | integer | the defaults' | 0 to 10000 | |
| `fade_out_ms` | integer | the defaults' | 0 to 10000 | |
| `crossfade_ms` | integer | the defaults' | 0 to 5000 | |
| `retrigger` | string | the defaults' | `restart` or `continue` | |
| `loop_mode` | string | the defaults' | `pack` or `game` | |
| `games` | array of strings | all | `disk`, `cdtv`, `st` | |
| `builds` | array of strings | all | build tags | |

<!-- fields: audio.music.slots{}.loop -->
| Field | Type | Default | Values and limits | Meaning |
|---|---|---|---|---|
| `start_s` | number | **required** | 0 or more | Seconds into the body file (after the intro). |
| `end_s` | number | the end of the file | more than 0, and after `start_s` | Seconds into the body file. |

## Scoping and layering

- **Game.** `disk` is every Amiga disk build, `cdtv` the CDTV release, `st` the Atari ST. A pack, or an entry of it, with a `games` list is used only for those games.
- **Build.** `builds` narrows the disk game to builds by their tag. The canonical image is `canonical`.
- **Load order.** Packs listed in `packs_enabled` (found in the data folder's `packs/` folder, in the listed order) load first, then the
  paths in `asset_packs`, then each `--asset-pack` in the order given. A later pack replaces an earlier one **per key** (graphics, screens,
  panel cells, clips, music slots; palettes: the latest whose `match` fits). The loader reports the first 20 overrides of each pack.
- **Nothing applies:** a pack none of whose entries applies to the running game is loaded with an information line, not an error.

## Limits

| Limit | Value |
|---|---|
| `manifest.json`, one PNG | 32 MB each |
| a picture | 4096 pixels on a side and 8 million pixels (a background: 16384 and 32 million), checked from its header before it is decoded |
| decoded pictures of all packs | 512 MB |
| entries in one pack (atlas frames included) | 65,536 |
| screens in one pack | 256 |
| signature blocks of one screen | 64 |
| ZIP file | 4 GB (no ZIP64), 65,536 members, a member at most 512 MB unpacked, a deflated member at most 1000 times its size unless it unpacks to 1 MB or less; stored and deflated members only; no encryption |
| JSON | depth 32, 1,000,000 values; strict RFC 8259 (no comments, no trailing commas) |
| one sound file | 512 MB |
| a sound effect or speech clip | 120 seconds, 32 MB decoded, one or two channels, 8000 to 192000 Hz |
| decoded sound effects and speech of all packs | 256 MB |
| a music track | streamed, at most two hours, at most three decoders open at once |
| voices from packs | 16 at a time (the oldest fades out in 5 ms when a 17th starts), at most 32 counting fading ones |

## What the loader and the checker say

A message looks like `error|warning|info: asset pack: <pack>: <section>["<key>"]: <text>`, then one `check:` line. An **error** means an
entry was skipped or the pack was refused; a **warning** means it loaded but is probably not what you meant; **information** is for the
author. Common messages:

| Message (shortened) | Cause |
|---|---|
| `manifest.json: "format" must be "x2-asset-pack"` | the pack is not used |
| `"version" N needs a newer x2recomp` | the manifest is from a later format |
| `graphics["..."]: the key must be "<16 hex digits>-<width>x<height>"` | a mistyped key; the entry is skipped |
| `graphics["..."]: missing file 'sprites/x.png'` | the path is wrong, or the file is missing, or a ZIP name differs in case |
| `graphics["..."]: 'x.png' is AxB, a graphic of this key at this scale is expected to be CxD` | the picture is the wrong size for its key and scale (warning; use `anchor` if it is meant) |
| `graphics["..."]: this key is not in the dump` (with `--against`) | a typo, or a graphic of another build |
| `audio.sfx["..."]: "gain_db" must be -60 to 24` | a value out of range; the entry is skipped |
| `audio.music.slots["..."]: the slot must be title, level, level1-level5 or cd2-cd13` | a mistyped slot name |
| `audio.music.slots["title"]: the loop ends at 3 s, after the end of the file (1 s)` | a loop point past the end of the file |
| `screens["..."]: the signature has fewer than 6 blocks: it may match other screens` | a thin signature (warning) |
| `screens["..."]: has no "ref_palette": the picture stays at full brightness while the game fades` | information |
| `license: missing` | warning; an error with `--strict` |
| `... is byte-identical to the dump's original` (with `--against`) | the picture or clip is a copy of what the dump holds; a dump must not be redistributed |
| `no such asset pack (a directory or a .zip file)` | exit code 3 |

Exit codes of `--check-asset-pack` and `tools/assetpack/validate.py`: **0** valid (warnings allowed), **1** warnings with `--strict`,
**2** the pack is unusable or has invalid entries, **3** the path cannot be read.

## Where the schema and the loader differ

- `backgrounds[].ring_lines` is accepted up to 4096 by the schema and the loader, but only 192 can work.
- A screen's `signature.min` above its number of blocks, an `end_s` that is not after `start_s`, `file: null` without `mute: true`, and a speech-only `cd0_file` on `sfx` are checked by the loader, not by the schema.
- The schema accepts any sample or graphic key of the right shape; whether a key exists in your game is only known to `--check-asset-pack --against DUMP`.
- The validator without the runtime (`tools/assetpack/validate.py`) applies no `games` or `builds` scoping and checks FLAC and Ogg files by signature only.
