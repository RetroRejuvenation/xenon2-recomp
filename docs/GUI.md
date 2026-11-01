# The launcher and the in-game menu

x2recomp has a small window you can use instead of remembering options: the **launcher**, which opens before a game starts, and the
**in-game menu**, which opens over a running game. Both are made of the same pages. Everything they do can also be done on the command
line, and every page shows the command line that does the same thing, so the window teaches the flags instead of hiding them.

Nothing here contains game data. The launcher's pictures are drawn by the program itself.

## When the launcher opens

| You start | What happens |
|---|---|
| `x2recomp` with no arguments (or double-click the app) | the launcher opens |
| `x2recomp --gui` (with anything else) | the launcher opens, with those arguments shown as an override note |
| `x2recomp --no-gui` | no launcher; nothing imported prints how to import and exits with code 3 |
| `x2recomp` with any other argument, or headless | no launcher: the game starts as it always did, so scripts, tests and replays are unaffected |
| nothing imported yet | the launcher opens on the **import page** |

`launcher = auto | always | never` in `settings.cfg` changes the default. The home page has a switch, "Start the last game directly next
time", which writes `launcher = never` and starts the release you played last; run `x2recomp --gui` to see the launcher again.

A development checkout that has `./generated/assets` and nothing imported skips the launcher unless you pass `--gui`.

## Moving around

You never need a mouse.

| | Keyboard | Gamepad |
|---|---|---|
| Move | arrow keys | d-pad or left stick |
| Select, press, change a value | Enter or Space; Left and Right change a slider or a stepper | A (the south button) |
| Back | Esc | B (the east button) |
| Next and previous page | PageDown and PageUp (or Tab and Shift+Tab) | right and left shoulder |
| Show the full help or command | I | Y (north) |
| Put the row back to its default | Delete or Backspace | X (west) |
| Open and close the in-game menu | F1 (Cmd+1 on a Mac) | Start |
| Quit | F10, Ctrl+Q | |

A mouse or a touch screen works too (click to select, wheel to scroll). The button hints at the bottom of every page follow the pad you
are using (A, B, X, Y on an Xbox-style pad, Cross and Circle on a PlayStation-style one).

## The pages

**Play.** One card for each release you have imported (Amiga disk, Amiga CDTV, Atari ST) with its variant and a status: `canonical`,
`patched`, `imported` or `preview`. Select a card to play it. The card for something you have not imported says "Import...". Under the cards:
**Import another game...**, a one-line summary of the display and audio settings (each one jumps to its page), the scores, assists and cheats
rows, and the command line Play will run.

**Import.** Drop disk images, flux dumps, a disc image, a zip or a folder on the window, or use "Choose files..." or "Choose a folder...".
The window says what the files look like ("Atari ST disk images", "a CDTV disc image"), then shows a progress bar and **Cancel**. A
cancelled import changes nothing, and an earlier import stays exactly as it was. Errors are plain sentences with the importer's own message
below them. A release on two disks takes the second disk as another drop. The same importer as `x2recomp --import`.

**Display.** Window size, full screen, filter, curvature, aspect, whole-pixel scaling, smooth frames, widescreen, the HD status panel and
whether asset packs are used, with a generated test card showing the filter and the aspect (the game's own picture is never used by the
launcher). The rows for something the selected release does not have are greyed out with the reason ("The Atari ST release has no
interpolated frames"). At the bottom: **Text size** (Normal or Large), **Contrast** (Normal or High) and **Motion** (see "Reading
comfort").

**Audio.** Sound on or off, master, effects, music and speech volume, the output filter, stereo separation, the output buffer, the
asset-pack audio switch and, for the CDTV release, the music source. **Play test tone** plays a short generated sound at the volume you
have set, so you can set it before a game starts. It is a sound made by the program, not the game's.

**Controls.** Every key binding of the game and of the host (`input.cfg`): select a row, press Enter, press the new key (Esc cancels,
Backspace clears). A key that something else already uses is asked about first. The gamepad's fire, second-button and menu buttons are set
here too. Autofire, keyboard grab and the cursor mode are on this page. The CDTV release's second button is `C` by default. "Restore
defaults" puts a row or the page back. Bindings are saved in `input.cfg` as soon as you change one, and `x2recomp --list-keys` prints
them.

**Packs.** The asset packs in your `packs` folder and any you added by path, in load order: switch one on or off, move it up or down
(later packs override earlier ones), see what it says about itself, **Check pack** (the same checker as `--check-asset-pack`, with
**Compare to dump** for coverage), **Add a pack...** with a file dialog or by dropping a folder or zip on the window, **Remove** one you added by
path, **Rescan** the folder, **Open folder** to see it in your file manager, and **Dump my assets** for pack authors. The dump holds pictures and sounds from your own game: do not share it. Changes to which
packs load take effect at the next start. See `ASSET-PACKS.md`.

**Advanced.**
- *Imported games*: what each import recorded (variant, importer version, protection method, the first twelve hex digits of the
  key-track receipt, the name of the source file). The opt-in protection patch is explained here and its full disclaimer can be read, but it
  can only be applied **while importing**, after you confirm that you own the original disc: "Import again..." opens the import flow, which
  asks. It is never the default.
- *Diagnostics*: **Copy diagnostics** puts a bug report on the clipboard (version, platform, SDL and its drivers, the imported releases, the
  enabled packs and every setting that differs from the default). It holds no game data and no folder names beyond the data folder's own.
  **Check an image...** (or drop a file on the page) shows what `--diagnose-image` would say, with no game bytes. **Open the data folder**,
  **Open the log file**, and the input log switch.
- *Log*: the last 500 lines of the program's own log (importer messages, pack reports, settings problems, the notices of the menu).
  `x2recomp.log` in the data folder keeps more, and `x2recomp.log.1` the file before it. Only window runs write it.
- *Settings file*: **Export settings...**, **Import settings...** (the same plain text as `settings.cfg`; which release you played last is
  kept) and **Reset all settings...** (asks first; key bindings and imported games are not touched).
- *Command line only*: the replays, traces, hashes, `--timing`, `--chip-ram` and the developer options have no buttons; the page lists the
  common commands, with a Copy button.

**About.** Version, build, platform, SDL and its drivers; the licence (GPL-3.0-or-later); what the project does and does not contain
(**no game data**: you bring your own copy); the third-party software with its licence; where the documentation is; how to report a problem.

## The in-game menu

F1 (Cmd+1 on a Mac) or the gamepad's Start button pauses the game exactly as the P pause does and opens a panel over the dimmed picture:

- **Resume**
- **Faithful**: the original look and sound only, with every picture option and every asset pack off. The same as `--faithful`.
- **Display**, **Audio**, **Controls**, **Asset packs**, **Assists and cheats**: the pages above. A change shows on the running picture at once
  where the release can take it (window size, filter, aspect, volume, autofire, god mode, and so on); what cannot waits for the next start,
  and the panel says so. Everything is saved to `settings.cfg` and `input.cfg` as you go.
- **Reset the game** (asks first)
- **Back to launcher** (asks first): leaves the game and opens the launcher again.
- **Quit** (asks first)

Esc goes back one level. Outside the menu Esc is the game's own key and is untouched. Recorded replays and the headless runs never open
the menu, and a visit to it does not change the game: the frame hashes after you close it are the same as in a run that never opened it.

## Reading comfort

- **Text size: Large** uses a smaller logical canvas, so everything is drawn bigger and the pages scroll.
- **Contrast: High** is white on black with a yellow focus and no background decoration. The default palette already keeps every text at
  least 4.5:1 against its background.
- **Motion: Reduced** removes the page slides, the starfield, the breathing focus ring and moving progress stripes. `Auto` means full
  motion, because SDL does not report the system's setting; choose Reduced if you want it.
- The focused control is never shown by colour alone: it also has a ring and a marker.
- Nothing in the window times out and nothing flashes.
- Screen readers are not supported, because the pages are drawn by the program itself rather than by the system. The command line
  (`x2recomp --help`) does everything the window does and is the accessible route.

## Files

| File | What |
|---|---|
| `settings.cfg` | every setting; plain `key = value` text; the window saves it as you change things |
| `input.cfg` | key bindings and the gamepad's buttons |
| `x2recomp.log`, `x2recomp.log.1` | the program's log (window runs) |
| `packs/` | your asset packs |
| `dumps/` | the dumps made by "Dump my assets" |

They live in the per-user data folder; **Open the data folder** (Advanced or About) shows it.

## Troubleshooting

- **The launcher does not open.** You passed an argument: use `x2recomp --gui`. Or `launcher = never` is set in `settings.cfg`.
- **"cannot open the launcher window".** The machine has no display (a remote shell); use `--no-gui` and the command line.
- **Test tone is silent.** The sound output could not be opened (the Log on the Advanced page says why), or sound is switched off on the Audio page.
- **A setting is greyed out.** The reason is shown instead of the help text: it is for another release, or the Faithful switch overrides it.
- **A file dialog does not appear.** Drop the file on the window instead; every dialog has that alternative.
