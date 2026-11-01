# Launcher UI: the Asset packs page (workstream D)

The Asset packs page lists the packs the program can load, turns them on and off, sets the order they load in, shows what each pack says about
itself, runs the pack checker on one, and makes a dump of the game for pack authors. It is built on the widget kit (`gui-widget-kit.md`) and the
settings schema (`gui-options-schema.md`); what a pack is, and how packs are made, is in `docs/ASSET-PACKS.md`. Nothing here opens a window by
itself: the launcher and the in-game menu host the page, as they host the Play, Display and Audio pages.

## What there is

| File | Role |
|---|---|
| `runtime/ui/pages/packs.{h,cpp}` | `PacksModel` (the list, the edits, the jobs' results) and `page_packs` |
| `runtime/ui/pack_jobs.{h,cpp}` | `PackJobs` (one worker thread, a check or a dump), `run_dump_process`, the dump folder names |
| `runtime/pack/pack_discovery.*` | `pack::inspect_pack(path, info)`: what one pack's manifest says, without loading its pictures or sounds; `PackInfo::credits` |
| `tests/ui/packs_page_tests.cpp` | behaviour, the worker jobs, the dump process, reachability, 40 golden renders |

`SettingsModel` has one new field, `PacksModel* packs`. With it set, `settings_frame` shows the page; without it the Packs tab is the placeholder it
was, so a host that has not wired the page in is unchanged.

## The page

- The two switches at the top are rows of the schema: **Asset packs** (`upgrade`) and **Asset-pack audio** (`audio_upgrade`, greyed for the Atari ST
  release, which has no pack audio). They carry the same help, "next start" badge and Faithful-switch reasons as on the Display and Audio pages.
- **The list** has one line per pack, in load order: `[x]` on, `[ ]` off, `[!]` not usable, `[?]` an id in `packs_enabled` that the packs folder no
  longer has; the position in the load order; the name; the version, games and what it provides (`gfx`, `scr`, `snd`) when there is room; and a
  status: `unchecked`, `ok`, `N warnings`, `errors`, `unusable`, `not found`, or `not for disk` (the pack's `games` and `builds` do not include the
  release being described). Packs from the packs folder come first, in the order of `packs_enabled`; then the packs added by path, in the order of
  `asset_packs`; then the packs that are off. That is the order the loader uses (`load_packs`), so the number on a line is the layer it has.
- **Details** under the list: name, version, author, licence, the games it is for, the credits (or the description), the entry count (or the coverage
  line after a check against a dump), and, in the warning colour, why a pack is not used.
- **Buttons:** Turn on / Turn off / Forget (the pack's id in `packs_enabled`), Move up and Move down (inside the packs from the folder, or inside the
  packs added by path: the two groups keep their order, as the loader keeps it), Check pack, Add a pack..., Remove, Rescan, Open folder. A pack added
  by path stays on while it is listed; Remove takes it off the list and does not touch the files. A pack that cannot be used (`PackInfo::usable`
  false) cannot be turned on, with the reason in the line under the buttons. Enter on the list does the same as Turn on / Turn off.
- **Add a pack...** calls the host's file dialog (`PacksModel::choose_pack`); without one the button is greyed, and the host can still call
  `PacksModel::add_path` for a pack folder or `.zip` dropped on the window. A path inside the packs folder is turned on by id, any other path goes
  into `asset_packs`. A path that is no pack, has an unusable manifest, contains `;` (the separator of the list in `settings.cfg`) or is already there
  is refused with a sentence that says why.
- **Check** runs `check_asset_pack` (the code behind `--check-asset-pack`) on a worker thread and shows its report: a first line that says valid, N
  warnings or N errors, the entry counts, then every finding in the order the checker gave them, with the same severities. **Strict check** counts
  warnings as failures, as `--strict` does. **Compare to dump** also passes the newest dump as `--against` (it is greyed until there is one), which
  adds the keys that match nothing and the coverage line.
- **Dump my assets** starts a headless run of this program (`--headless --game G --frames N --dump-assets DIR --dump-audio DIR`) and shows its
  progress; Cancel kills the run and removes what it wrote. The dump goes to `<data dir>/dumps/<game>`, then `<game>-2`, `<game>-3`: a dump never
  overwrites another. It is written beside the final folder as `.partial` and renamed when the run succeeded, so a failed or cancelled run leaves
  nothing. The page says what the dump is: the user's own copy's pictures and sounds, for making a pack, never to be shared.

## The worker (`PackJobs`)

One job at a time, on a `std::thread`. The page polls (`PacksModel::poll`, called every pass) and never waits; a finished job is collected on the
next pass. `PackJobs::busy()` stays true until the result has been taken, so a host that redraws about ten times a second while
`PacksModel::busy()` is true always sees the result. (`Context::animate()` is also called while a job runs, which is enough when motion is on; the
host's timer is what makes it work with reduced motion.) The check cannot be interrupted; the dump can.

`run_dump_process` is the default dump runner (tests replace it with fakes through `PacksModel::dump_runner`):

- It starts `exe` with the arguments of `dump_command_args` (`posix_spawn` and a pipe on macOS and Linux; `CreateProcessW` without a console window
  and an anonymous pipe on Windows), reads the standard output and error together line by line, and turns the status reports (`[frames A-B] ...`,
  printed every 100 frames with `--report-every 100`) into progress. A pipe is block buffered, so progress moves in steps of a few hundred frames.
- A non-zero exit gives the run's own last `x2recomp:` line as the error ("--game cdtv: no imported game was found..."); an exit of 0 without a
  `manifest.json` is "the run finished but wrote no dump".
- The Atari ST release is never asked for `--dump-audio` (it has no sampled sound).
- `frames` is 6000 (two minutes of game time at 50 frames a second): the title screen and the attract demo, headless and without input, which is
  what a release package can do without the test replays. A dump that covers every level needs the replays (`tools/assetpack/dump_all.sh`) or a
  game played by hand with `--dump-assets DIR`; the page shows neither as a button.

## Tests

`tests/ui/packs_page_tests.cpp` (ctest `packs_page`; synthetic packs written by `tests/support/synth.h`, no window, no SDL, no game data):

- rows and status for a packs folder with a good folder pack, a ZIP pack, a pack for another release, a broken manifest and a folder that is no pack;
  enable, disable, forget, order, add by path, add by drop (quoted paths), remove; the selection following a pack; the schema's two switches; the
  page driven by key presses (the list, the buttons, Add a pack, Rescan, Open folder);
- the check on the worker: valid and warned packs, a path that is not there (exit code 3), one job at a time;
- the dump: folder numbering, the command line, and `run_dump_process` against **this test program run as a fake game** (`--headless` as the first
  argument): success with monotonic progress and a renamed folder, an existing folder refused, the run's own error text, an exit without a dump, a
  program that is not there, cancel killing a run that never ends; the page's button with fake runners (success, cancel, failure, no program);
- every control reachable with the arrow keys (the kit's rule), for the disk and ST releases, at both canvas sizes, with and without packs turned on;
- 40 golden renders (10 scenes, 640x360 and 480x270, normal and high contrast; `tests/ui/golden/packs_hashes.txt`, zero tolerance): the empty
  state, packs on, packs added by path with one that is gone, a missing id, CDTV, ST, a check report, a warned pack, a dump running (a runner held at
  45 %) and a dump finished. The fixtures live in a fixed relative folder so the footer's `--asset-pack` paths are the same on every machine.

`packs_dump_game` (the same program with `--real-dump EXE ASSETS DIR`) is the data test: it dumps 400 frames of the user's imported game through
`run_dump_process` and the real `x2recomp`, checks the progress and the folder, and runs the checker on the dump as a pack (a dump is a valid pack
at scale 1). It reports Skipped without the imported game.

## Where it differs from the plan

- **Coverage against a dump is a switch, not a separate button.** The plan showed it as an equivalent command in Advanced; with "Dump my assets" on
  this page, comparing with the newest dump costs one toggle, and the checker's own coverage line appears in the report and the details.
- **Preview is metadata, counts and the report, as planned for v1.** `--modern-shots` needs the game running, which the launcher does not have;
  the in-game menu is where a pack is seen working. Thumbnails stay a later addition.
- **No `runtime/pack/pack_check.*` change.** `check_asset_pack` already returns the report as data (`CheckResult`); the API addition is
  `inspect_pack` in `pack_discovery.*` and `PackInfo::credits`.
- **`pack_jobs.*` rather than the launcher's `worker.*`.** The launcher workstream owns a general job runner for imports; this page needs two jobs
  and no dependency on it. They can share one runner later.
- **No `launcher`/menu wiring.** `SettingsModel::packs` is the single hook. A host sets `packs_dir`, `dumps_dir`, `exe_path` (and, for a game that is
  not in the default place, `assets_dir` and `data_dir`), the file dialog and the folder opener, and keeps redrawing while `busy()`.
