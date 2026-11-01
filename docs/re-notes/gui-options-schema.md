# Launcher UI: the options, the settings schema and the first pages (workstream B)

The launcher and the in-game menu have to say the same things the command line and `settings.cfg` say, and the "equivalent command line"
under each page has to be a command that really does what the page shows. This note describes the three pieces that make that true: the
options parser that left `main.cpp`, the settings file's writer, and the schema table the pages are built from. Nothing here opens a
window by itself; the launcher (a later workstream) hosts the pages.

## The parser is one library now (`runtime/cli_options.*`, library `x2cli`)

`Options`, `parse_args`, `apply_settings` and `usage` lived in an anonymous namespace in `main.cpp`. They are in `x2::cli` now and
`main.cpp` uses them through `using` declarations. The move changed nothing:

- `x2recomp --help` and the error text of a bad option are byte-for-byte what they were (compared with the binary built from the commit
  before the move).
- `tests/cli/parse_cases.txt` holds 275 cases: every flag spelling, good and bad values, the `=` forms, repeated flags, settings under
  arguments. `tests/cli/parse_baseline.txt` is what the parser **as it stood in `main.cpp`** produced for them (every `Options` field
  that differs from a default-constructed `Options`, and whether the parse succeeded); the test `cli_options` (`parse_equivalence`)
  replays the cases through the library and compares at zero tolerance. A change of behaviour has to change the baseline in the same
  commit, where a reviewer sees it (`x2_cli_options_tests SOURCE_DIR --update` rewrites it).
- Without SDL3 the library still builds: the two things it takes from the SDL-only input code (the cursor mode names and the default
  autofire rate) have a fallback with the same values.

New in the library:

- `settings_from_options(Options, Settings&)` is `apply_settings` the other way round. It writes into the `Settings` fields the options
  correspond to and leaves the rest alone, so the caller starts from what was loaded. Values the command line takes and the file would
  refuse are brought into the file's range: `--scale 12` is a window twelve times the picture while `settings.cfg` allows 1 to 8,
  `--stereo-sep 3` is clamped to 1, `--cursor on` is `cursor = show`. A test round-trips a `Settings` with every field changed.
- `--print-settings` prints the settings a **window** run with these arguments would use: `settings.cfg` from the data directory (the
  one `--data-dir` names), then the arguments on top, in `settings.cfg` format. It starts no window, needs no game data and never
  writes the file (a headless run never reads `settings.cfg`; this flag answers for the window run whatever else is on the command
  line). `format_settings` writes the output, so it is a file the parser reads back.
- A test (`help_flags_are_parsed`) takes every `--flag` the help text names and checks that the parser knows it, so a help line cannot
  outlive a flag. It found one old fault: the `--game` entry of the help text ended in a stray line ("`--game st) or auto ...`"); it
  now ends with "or auto (default: the disk release if imported, else the CDTV release, else the Atari ST one)".

## `settings.cfg` is written back whole (`runtime/persist.*`)

- `merge_settings_text(old, settings)` returns the old text with the values of `settings` written in. Comments, the order and spacing
  of the lines, lines whose value already means what `settings` says (`fullscreen = yes` stays `yes`; `wide = 0` stays `0`) and lines
  of keys this version does not know are kept. A line whose value changed is rewritten and keeps its comment. When a key appears twice
  the last line is the one that counts, so it is the one that changes. A value the parser refuses is replaced by the value in use. A
  known key the file lacks is appended under `# Settings this version added:`. An empty or missing file gives `format_settings`
  as before. CRLF files are read and come back with LF.
- `save_settings_file(path, settings, why)` merges onto what `path` holds now, writes `path.tmp` beside it and renames it over the old
  file, so an interrupted write leaves the old file whole. `main.cpp` uses it for the first-run file and for the write when the window
  closes (before, the file was rewritten from `Settings` alone, which dropped comments and unknown keys).
- Five keys for the launcher and the menu: `launcher` (`auto`, `always`, `never`), `ui_text` (`normal`, `large`), `ui_contrast`
  (`normal`, `high`), `ui_motion` (`auto`, `full`, `reduced`) and `last_game` (`disk`, `cdtv`, `st`, or empty before the first run).
  Nothing reads `launcher` yet; the launcher workstream adds the `--gui` and `--no-gui` flags and the behaviour. The command line has no
  flag for any of the five, and the schema says so (below).

## The schema (`runtime/ui/settings_schema.*`)

One row for every key of `settings.cfg`: its label (at most 18 characters, the width of the label column), a one-line help text, its
type (bool, int with a range and unit, float, choice, text), which releases honour it, whether it takes effect at once or at the next
start, the pages that show it and a group heading, whether it is an expert option, a cheat or a picture option (the Faithful switch
overrides picture options), the `input.cfg` action that does the same on a hotkey, and how the command line says it.

The command line forms (`CliKind`): `--flag VALUE`, a switch that turns it on, a switch that turns it off (`--no-audio`), an on/off
pair, an item of `--modern` (`interp`, `nosmooth`, `hdpanel`, `noaudio`, `upgrade`) or an item with a value (`scale=4`, `wide=16:9`), a
repeated flag (`--asset-pack`), a choice whose values are flags of their own (`--grab-keyboard`, `--no-grab-keyboard`), and autofire,
whose rate rides on the flag (`--autofire=9`).

`command_line(settings, game, with_game)` composes the command from the **difference to the defaults**, for one release. It leaves out
what the release does not honour (the Atari ST runner receives a subset of the options: `main.cpp` hands it the scale, fullscreen, the
filter, aspect, whole-pixel and curvature options, the volume, the output buffer, sound on or off, the packs and the canvas scale, and the
assists and cheats), and, when the Faithful switch is on, the picture options it overrides. What the command line cannot say (the pack
ids in `packs_enabled`, the five launcher keys, an autofire rate with autofire off, `--asset-pack` without the packs being on) is
returned as notes. `--asset-pack` already turns the packs on, so the `upgrade` item is not written next to it.

`disabled_reason(row, settings, game)` gives the sentence the page shows when a row is greyed: the release does not use it, the Faithful
switch is on, or another setting has to be on first (curvature needs the crt filter, the smoothing options need smooth frames, the volumes
need sound).

### What the tests prove

- `settings_schema` (`tests/settings_schema_tests.cpp`, about 18,000 checks):
  - every key of `format_settings` has a row and every row is a key (a setting added to `Settings` without a row fails the suite);
  - every row's range, choices and spellings are the ones `parse_settings` accepts, one step outside is refused;
  - which releases a row applies to matches what `main.cpp` passes the ST runner;
  - the command-line text for fixed examples;
  - **the round trip**: for every row with a command-line form, every release and a set of non-default values, `command_line` is parsed by
    `parse_args`, turned back into `Settings` by `settings_from_options`, and every setting that release honours must come back equal.
    The same is run through the real binary (`x2recomp --print-settings --data-dir <empty> <the command>`) for 90 commands;
  - every flag and `--modern` item the schema writes is in the help text.
- `settings_pages` (`tests/ui/settings_pages_tests.cpp`): key presses change the settings and the footer's command; rows that do not apply
  or need another setting are inert; Down walks every control of every page and Copy sets the copied text; 44 golden renders at
  zero tolerance (`tests/ui/golden/settings_hashes.txt`, `--update` and `--dump DIR` as for the kit's goldens).
- `persist` covers the merge and the atomic save (comments, unknown keys, duplicate keys, bad values, CRLF, a missing directory).

## The pages (`runtime/ui/pages/`)

`SettingsModel` holds the `Settings` being edited, the release the pages describe, an optional hotkey lookup (the in-game menu gives it
the live `input.cfg` bindings; without one no key is shown) and the cached command line. `settings_rows` builds the rows of one page from
the schema, in groups; `page_play`, `page_display` and `page_audio` add the page heading, the scrolling area and the help line, and
`settings_frame` is a whole frame (page selector, page, help line, command footer, hint bar) for the launcher to host. The Display page
keeps the generated test card; the Audio page offers "Play test tone" when the host gives it one and an "Expert options" disclosure for
the CD track map and folder. The Controls, Packs and Advanced rows are in the schema for the workstreams that build those pages.

A text row (a path, the track map) is shown but not edited: there is no text entry in the kit, so paths will come from the file dialogs.

## Where it differs from the plan

- The three pages live in one file, `pages/settings_page.cpp`, not three: they share every helper and are short.
- The `Options` extraction and `--print-settings` are as planned. `--print-settings` reads the settings file only for a window run's
  semantics, as the plan says; it also never creates the file (the window run does).
- The schema is a table of rows with typed accessors; parsing and printing of `settings.cfg` stays in `persist.cpp`, which the tests
  tie to the table. Generating `parse_settings` from the table would have changed the file parser in the same commit as the table.
- The plan lists `--gui`/`--no-gui` under workstream C, so the `launcher` row has no command-line form yet.
- The labels are limited to 18 characters (a 148-pixel label column), so a few are shorter than the plan's mockups ("Protect the tune",
  "Smooth size").

## Inconsistencies found in the options

- `--scale N` has no upper bound while `scale` in `settings.cfg` is 1 to 8; `--stereo-sep` has none while the file takes 0 to 1.
  `settings_from_options` clamps, and the window still honours what the command line said.
- `--cursor` accepts `on`, `off`, `yes` and `no` (as `CursorPolicy::parse_mode` does) but `cursor =` in the file accepts only `auto`,
  `show` and `hide`. `--print-settings` writes `show` and `hide`.
- `--music-volume` and `--sfx-volume` are clamped to 0 to 100, `--stereo-sep` and `--scale` are not: the command line is not uniformly
  validated. Left as it is, since scripts may depend on it.
- `--modern scale=N` is clamped to 1 to 8 silently while `--scale` is not.
