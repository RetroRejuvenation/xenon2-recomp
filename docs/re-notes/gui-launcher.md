# Launcher UI: the launcher window and the import flow (workstream C)

The launcher is the window `x2recomp` opens when it is started without arguments (or with `--gui`): a home page with a card for every
imported release, the Display and Audio pages built from the settings schema (workstream B), and an import page that replaced the old
first-run screen. It is built from the widget kit (`gui-widget-kit.md`) and the schema (`gui-options-schema.md`); nothing in it is
specific to the emulator. This note says how the pieces fit, what the rules are, what is tested, and where it differs from the plan.

## Pieces

| File | Role |
|---|---|
| `runtime/ui/library.*` | `scan_library(data dir)`: which releases are imported (the manifests the importers write), their variant name, a one-word status (`canonical`, `patched`, `imported`, `preview`) and, for CDTV, whether the CD music was imported too |
| `runtime/ui/worker.*` | `Worker`: one background job at a time with progress, messages and cancel (`JobContext`); the UI thread polls it once per frame |
| `runtime/ui/import_job.*` | `run_import`: decides the family from what was given (a CDTV disc image, Atari ST disk images, or Amiga disk images) and runs the right importer with progress and cancel |
| `runtime/ui/launcher_state.*` | `LauncherState`: the library, the `SettingsModel` being edited, the import flow (dropped files, log, headline, patch offer) and what the user decided (play which release, quit) |
| `runtime/ui/pages/launcher_pages.*` | `launcher_frame`: the home page, the import flow, About, and the settings pages around them |
| `runtime/ui/launcher.*` | `run_launcher`: the SDL3 host (window, events, drops, native file dialogs, clipboard); returns which release to start |
| `runtime/import/importer.*` | `ImportOptions::progress`, `ImportOptions::cancel`, `Result::cancelled` |

`x2ui_launcher` (everything except the host) has no SDL, so its tests run on every build. `x2ui_host` (the window) is built with SDL3 and linked
into `x2recomp`.

## When it shows

`launcher_wanted(options, argc)` (`runtime/cli_options.cpp`): never for a headless run, always with `--gui`, never with `--no-gui`, and
otherwise (`launcher = auto`) when the program was started without arguments. `launcher = always` and `never` in `settings.cfg` are the same as the
two flags, and the command line wins. A **first run** (nothing imported, no explicit game location) opens the launcher even with other arguments,
straight on the import page, unless `--no-gui` was given. A development checkout that plays `./generated/assets` and has nothing imported
starts the game without a launcher unless `--gui` is given. `launcher = never` with `game = auto` starts the release played last (`last_game`).

`main.cpp` calls the launcher once, early, before it chooses the release: the launcher saves what the user changed to `settings.cfg` and answers
with the release to play; `main()` then runs as if `--game` had been given (the answer is applied again after the settings are merged, because
the command line is parsed a second time on top of the file).

## The import flow

1. Drop files on the window, or "Choose files..." / "Choose a folder..." (SDL's native dialogs; their results come back on another thread and are
   queued for the UI thread). The paths are added to the ones already given.
2. `run_import` runs on the worker. **Amiga disk images** go through `import_paths` with `ImportOptions::progress` and `::cancel`: progress is
   monotonic and ends at 1000, stages are "Opening the files", "Reading the images", "Decoding the flux capture", "Identifying the release",
   "Writing the game files"; cancel is polled between inputs and between stages (not inside the decoding of one image) and never while
   `write_assets` runs, so a cancelled import leaves the previous one byte for byte as it was (tested). **A CDTV disc image** and **Atari ST
   disk images** have coarser stages; the CD audio (about 680 MB) cannot be cancelled once it has started, and the page says so.
3. Outcomes: *imported* (the card appears, focused, with Play), *failed* (the importer's own first error as one sentence, the full list below
   it; the given files are forgotten), *one disk missing* (the files stay and the second drop carries both), *patch offer* (below), *cancelled*.
4. While a job runs the page is inert except for Cancel (which takes the focus), and a drop is refused with a message rather than queued.

### The protection patch

The opt-in patch keeps every safeguard of the old screen, and adds one: it is offered only when the importer says a retail disk 1 came without its
key track; it is a button, never the default; it opens a dialog with the importer's disclaimer in full (a scrolling list, reachable with a pad) and
an "I own the original disc" box that starts unticked **every** time the dialog opens and is cleared after each run; "Apply the patch" is inert
until the box is ticked. The request then carries `apply_protection_patch` and `confirmed_ownership` together, which is what
`--apply-protection-patch --i-confirm-i-own-the-original` does on the command line; `import_job` never passes `confirmed_ownership` without
`apply_protection_patch`, and the model never starts a patched run without the box (both tested). The result is recorded as `patched` in the
manifest and the card carries a `patched` badge.

## Settings and the command line

The launcher edits a `Settings` loaded from `settings.cfg`, saves it with `save_settings_file` (merging into what is there: comments and keys
from other versions stay) when something changed (at most every 400 ms, and when the window closes or the user plays), and records
`last_game`. Every page ends with the equivalent command line for the focused release. Three options that were stricter in the file than on
the command line now agree: `--scale` is 1 to 8, `--stereo-sep` is 0 to 1, and `cursor` in the file takes `on`/`yes`/`off`/`no` as well as
`show`/`hide` (`tests/cli/parse_baseline.txt` changed for exactly these: one line of an existing case, plus new cases appended at the end).
`--gui` / `--no-gui` are the command-line form of `launcher = always` / `never` in the schema, so the "equivalent command line" and the
`--print-settings` round trip cover them.

## Tests

- `launcher` (`tests/ui/launcher_tests.cpp`, no SDL, no game data): the library from synthetic manifests; the worker (progress never falls,
  messages stream, cancel, a job that throws, the destructor joins); the import job's failure paths with junk files (nothing is written for a
  failed import, a job cancelled before it starts changes nothing); the model with a **fake importer** (first run, a second family, a failed
  import, one disk then the other, cancel with the previous import untouched, a drop while busy, Back and Quit); the patch safeguards; Play
  writing `last_game` and the settings; the frame driven by key presses (cards and focus following the release, the import view, the patch
  dialog, the buttons that sit side by side); reachability of every control with Up, Down, Left and Right; and 48 golden renders (12 scenes at
  640x360 and 480x270, standard and High contrast) compared by hash at zero tolerance (`--update` rewrites them after review, `--dump DIR`
  writes PNGs).
- `launcher_sdl` (the real window loop under SDL's dummy video driver, through `LaunchHooks`): a quit event; Enter on the focused card plays;
  files dropped on the window reach the importer; settings (and the look: High contrast, Large text, Reduced motion) are applied and saved,
  keeping the comments and unknown keys of the file; Esc quits a first run; and, when the user's own disk images are given
  (`X2_DISK_ZIPS`), a real import through the window's drop path, checked with `verify_assets`.
- `import` gained `progress_and_cancel`: monotonic progress ending at 1000 for a folder, for in-memory inputs and for SCP flux; cancel before
  the start and from inside each of the first progress callbacks; a cancelled import writes nothing and leaves an earlier one verifying.
- `cli_options`: the launcher rule (`launcher_wanted`), `--gui`/`--no-gui` against the settings, the three aligned options.

## Where it differs from the plan

- The launcher opens **its own window** rather than drawing into the game's `SdlDisplay`, and closes it before the game opens its own. That keeps
  the game's display code untouched (workstream E owns it) and means the launcher and the game can each pick their own size.
- Cards are always three side by side at 124 pixels; the plan's one-row-per-release layout for narrow windows was dropped because the kit's
  smallest canvas (480 pixels) always has room for three cards, and the card widget is built for the tall form.
- The Controls, Packs and Advanced pages are the kit's placeholders (workstreams D, E and F fill them; `launcher_frame` has one `switch` for them).
  The old screen's behaviours (drop, two-disc keep, the patch flow) are all there, with Cancel, progress and a visible headline added.
- The shortcut "hold Shift while starting to see the launcher" is not implemented (SDL cannot tell the modifier before a window exists);
  `x2recomp --gui` does it. Esc on the first page of a release list is not a quit key (it goes back); Ctrl/Cmd+Q and the window's close button quit.
- A dev checkout with `./generated/assets` and no import skips the launcher (see above) so the maintainers' workflow does not change.
- Second-level detail the plan listed that is not in this workstream: the Controls page, per-pack views, logs and diagnostics (D, E, F), the
  "Back to launcher" item of the in-game menu (E, stretch), and the Raspberry Pi measurements (F).
