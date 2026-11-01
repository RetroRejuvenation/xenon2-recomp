# The in-game menu and the Controls page (GUI workstream E)

F1 (or Start on a gamepad, or Cmd+1 on a Mac) pauses the game and opens a panel with the Faithful switch and the same pages the launcher will
have. This note says how it fits into the two window loops, which settings a running game can take and which it cannot, what changed in
`input.cfg`, how it is tested, and where it differs from the plan. The kit it is built from is `gui-widget-kit.md`; the settings it edits are
`gui-options-schema.md`.

## What the player sees

```
+-- the game, dimmed ---------+---------------------------------+
|                             | PAUSED                Amiga disk |
|   the held picture          | [ Resume ]                      |
|   (nothing runs)            |  Faithful            [ OFF ]    |
|                             |  Display                        |
|                             |  Audio                          |
|                             |  Controls                       |
|                             |  Asset packs                    |
|                             |  Assists and cheats             |
|                             |  Reset the game                 |
|                             |  Quit                           |
|                             | help line                       |
|                             | Equivalent command line  [Copy] |
+-----------------------------+---------------------------------+
```

- The panel is 400 canvas pixels wide when the canvas is at least 600 wide; on the smallest canvas (480x270) it takes the whole width. The
  rest of the canvas is black at about half strength, so a change of filter or window size is judged on the running picture.
- Each page is a scrolling list of the schema's rows, with the hotkey beside the row, a badge ("cheat", "next start", "CDTV") and the one-line
  help of the focused row. The command line under it says what a start from a clean file would need to give the same settings.
- Esc (or B) goes back one level; at the top it resumes. Combinations with Ctrl, Alt or Cmd are not the menu's (Alt+Return does not press Enter), except Ctrl/Cmd+Q, which quits. F1 or Start closes from anywhere, whichever key `menu` is bound to. Page Up and
  Page Down move between the pages. Mouse and touch work, with the kit's 20-pixel targets.
- Text size, contrast and reduced motion (Display, "This menu") apply at once; the canvas is re-laid out when the text size changes.

## How it sits in the window loops

`Overlay` (`runtime/ui/overlay.*`) knows the kit, the schema and `input::Bindings`; it knows nothing of either machine. A host gives it
`OverlayHooks`:

| Hook | What the host does |
|---|---|
| `current()` | the settings the run is using now: `settings.cfg`, the command line over it, and whatever the hotkeys have changed since |
| `apply(before, after)` | applies the change to the running game and keeps it in `settings.cfg`; returns what took effect and what waits |
| `save_bindings(b, why)` | puts the bindings in force (`InputManager::set_bindings`) and writes `input.cfg` |
| `reset_game()`, `quit()` | the F12 reset (the ST runner: boot the disk again) and the quit flag |
| `set_open(bool)` | freeze or thaw the game |

Freezing is the P pause: nothing runs, the audio device stops and drops what it had queued (`SdlAudioOut::set_paused`), the keyboard grab is
released and the pointer shown (`GrabPolicy`/`CursorPolicy` `set_paused`), the pacer and the present clock are reset every iteration so
nothing runs ahead on resume. The Amiga loop keeps `paused` (the P/F9 pause) and `menu_open` apart (`freeze()` stops the device when
either is set), so closing the menu in a user-paused game leaves it paused. On opening and closing, `InputManager::release_all` forgets
every held key and tap: the key-ups went to the menu, not to the game.

While the menu is open the loops hand it every event except the few the input layer needs for itself (quit, focus, gamepads coming
and going). `SdlDisplay::set_overlay` takes a callback that `redraw()` calls after the filters and before the frame is shown;
`Overlay::draw` uploads its canvas as a blended texture there (`SdlPresenter::draw_over`). Nothing else in `SdlDisplay` changed, and the loops
call `display->redraw` every 10 ms while frozen, as they did for the pause.

Edits to the loops, all small and in one place each:

- `runtime/main.cpp`: the includes; one block after `apply_picture` that builds the overlay and its hooks (`freeze`, `do_reset`, `hk.*`; the
  reset code that was inline in the `Reset` case is the `do_reset` lambda now); the event loop routes events to the overlay while it is
  active, handles `HostCommand::Menu`, ticks it and holds the picture while `menu_open`; the Atari ST branch loads `settings.cfg` for window
  runs and passes `Settings`, the file and its path in `RunOptions`.
- `runtime/st/st_run.cpp`: the same, with the ST's own hooks.
- `runtime/gfx/sdl_display.*`: `set_overlay`, `renderer()`.
- `runtime/input/input.*`: the `menu` action, pad bindings, version 6 (below).

## Live apply (`runtime/ui/live_apply.*`)

`apply_live(before, after, game, hooks)` compares the two `Settings` row by row in schema order, ignores rows the release does not honour,
calls the hook for each change (the picture rows share one call) and returns the keys that took effect and the keys that wait. An empty hook
means "this release cannot take it now". The hooks are the code paths the hotkeys already use, so there is one behaviour:

| Setting | Amiga disk and CDTV | Atari ST |
|---|---|---|
| `scale`, `fullscreen`, `volume` | at once | at once |
| `modern` (Faithful), `interp`, `interp_scale`, `smooth_far`, `wide`, `hd_panel`, `upgrade` | at once (`apply_picture`) | next start |
| `filter`, `aspect`, `integer`, `curvature` | at once | at once |
| `autofire`, `autofire_rate`, `god_mode` | at once | at once |
| `credits`, `cheat_scores` | at once (for the next game) | next start |
| `music`, `music_volume`, `sfx_volume`, `speech_volume`, `audio_upgrade` | at once | not for the ST |
| `protect_music` | at once (the disk release only) | not for the ST |
| `grab_keyboard`, `cursor` | at once | not for the ST (no grab or cursor policy there) |
| `input_debug` | at once | next start |
| `ui_text`, `ui_contrast`, `ui_motion` | at once (the menu's own look) | at once |
| `game`, `launcher`, `last_game`, `high_scores`, `audio`, `audio_filter`, `stereo_separation`, `audio_latency`, `cd_*`, `cdtv*`, `asset_packs`, `packs_enabled` | next start | next start |

The `overlay` test fixes this table (every schema row must be in exactly one list, so a new row has to be decided) and checks it for
both the Amiga and the ST hook sets. `merge_changed(before, after, file)` copies only the changed rows into the in-memory copy of
`settings.cfg`, which the host then saves (atomically, keeping comments and unknown keys, `save_settings_file`): a command-line override
that was never saved (`--scale 6`) does not leak into the file, as with the hotkeys. On the CDTV release the music-source row is not saved,
as with the hotkey: its CD on or off is not the disk release's setting.

Rows that wait show "Applies at the next start: ..." in the help line (and on the Controls page) until the next key press.

## `input.cfg` version 6

- **`menu`** is a new action, F1 by default. `faithful` has no default key now. `HostCommand::Menu` replaces `Faithful` on F1 and on Cmd+1.
- **Pad bindings**: `pad_fire` (south, east, west, north), `pad_button2` (left and right shoulder) and `pad_menu` (start). The names do not depend
  on the pad's labels (`south` is A on an Xbox pad and Cross on a PlayStation one); the Controls page shows the labels of the connected pad. The
  d-pad and the left stick always steer.
- **Upgrade rules** (the existing classification, extended): a file that holds nothing but the version 5 defaults is upgraded (F1 becomes the
  menu; the old file is kept as `input.cfg.bak`). A file you edited is never rewritten by the game. For one that predates version 6 and does
  not mention `menu`, the menu takes F1 unless the file itself binds F1; then the file's choice wins, the menu has no key, and the note
  printed at start-up (and by `--list-keys`) says why and how to bind one. The gamepad's Start and Cmd+1 still open it.
- The Controls page writes the file: it is the user's change, so it is the user's file from then on (version 6, never upgraded over). The write
  goes to a temporary file and is renamed over `input.cfg`. A write that fails is said in the page and the bindings stay in force.

## The Controls page

Gamepad rows first (fire, second button, menu), then the schema's rows for the page (autofire and its rate, keyboard grab, mouse cursor), then
every keyboard action in groups. A row is the keys (or buttons) and a `+`: Enter on the keys, then the key, **replaces** the binding; the `+` adds
one (up to six); Backspace while waiting clears it; Esc cancels; Del restores the row's default (a default key that another action holds
now stays with its owner and the message says so). "Restore all defaults" resets everything.

- **Conflicts.** A key another action uses, or one the game reads itself (P, Esc and Space reach the game), is asked about before it
  moves: "Key in use ... Move it here / Cancel". Esc cannot be bound (it cancels the capture). The two joysticks count as different actions.
- **The menu stays reachable.** A change that would leave neither a key nor a gamepad button for `menu` is refused with a message.
- **Gamepad capture.** The kit delivers the next gamepad button as a raw key above `kPadKeyBase`; a keyboard key during a pad row, or a
  button during a key row, is explained and ignored.
- **button2.** The row reads "Second button" and says that the CDTV release's special weapon uses it and the disk and ST releases do not; C
  and the shoulder buttons are the defaults.
- The footer says where the bindings are kept and how to print them (`--list-keys`).

## Tests

- `input` (`tests/input_tests.cpp`, 1,338 checks): F1 is the menu, `faithful` unbound by default and still working when bound, `set_bindings` and
  `release_all`, the gamepad's menu button, pad names round trip, bad names reported, conflicts and game keys, `save_bindings_file`, and the
  migration cases above through real files (the version 5 defaults, a customised file that holds F1, one that left F1 free, one that names
  `menu`).
- `overlay` (`tests/ui/overlay_tests.cpp`, about 690 checks and 44 golden renders at zero tolerance; `--dump DIR` writes them over a test card):
  the live-apply table for both releases, open and close by every route, page navigation, the Faithful row, Reset and Quit with their
  confirmations (and Back out of them), F10 and Ctrl/Cmd+Q, settings applied and the "next start" note, the look following the settings and
  the window size, capture, add, clear, restore, conflicts, game keys, the gamepad, the unreachable-menu refusal, every control reachable
  by the arrow keys alone, bindings saved through the hook and a failed save said.
- `menu_pause_disk`, `menu_pause_cdtv`, `menu_pause_st` (the user's own assets; Skipped without them): a window run on SDL's dummy video driver
  with `--menu-script 60:F1,Down,Return,Return,Down,Return,Down,Right,Left,Escape,Escape,F1,F1` (open, Faithful on and off, Display, window
  size up and down, back, close, open, close) has the same frame hashes at frames 100, 200 and 320 as a run that never opened the menu, opens
  and closes twice, and leaves `settings.cfg` at `scale = 3`. This is the pause and resume determinism check, on the real loops.

## Where it differs from the plan

- **F1, not F10** opens the menu (the maintainer's decision after the plan): F10 still quits, so nothing existing changes except what F1 did.
  The faithful action has no default key.
- **No "Back to launcher"**: that waits for the launcher (workstream C). The Packs page is the schema's rows plus a note; the pack list,
  reorder and Check pack are workstream D's page and can replace it.
- **Everything is saved when it changes**, not on Resume: a crash or a kill leaves the file as the menu last saw it. The writes are atomic.
- **No test tone** button on the Audio page: the hook exists (`OverlayHooks::test_tone`) and no host supplies a tone yet.
- **The ST loop redraws while paused** now (the P pause used to leave the window as it was); before the first picture it draws a black frame.
- **ST window runs read `settings.cfg`.** The ST branch of `main` returned before the settings were read, so the window options of the file
  never reached that release (the plan assumed they did). They do now, with the command line on top, as for the Amiga releases.
- `Event::scancode` was added to the kit's raw key event (bindings are by scancode, the physical key), and `key_capture` takes the prompt text.

## Not done here

The launcher and the importer (workstream C), the packs page (D), the Advanced page, the log and diagnostics (F), a gamepad diagram for
the Controls page, and a Raspberry Pi timing of the overlay (the work while open is the kit's: about 0.1 ms per frame on an Apple M-series
core, and nothing while closed).
