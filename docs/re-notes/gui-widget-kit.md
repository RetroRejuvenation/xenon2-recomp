# Launcher UI: the widget kit (workstream A)

The launcher and the in-game menu share one small user-interface kit, written for this project: `runtime/ui/`. This note says how it works,
what it was tested with, what it costs, and where it differs from the plan. Nothing in the game binary uses it yet; the settings pages,
the launcher, the packs page and the in-game menu are later workstreams that build on it. A local plan (not in the repository) lists them.

## What there is

| File | Role |
|---|---|
| `ui_types.h`, `ui_canvas.*` | `Color`, `Rect`, rect cutting, and `Canvas`: a CPU pixel buffer with clipping, fills, blends, chamfered panels, dithering, discs and rings |
| `ui_font.*`, `font_data.inc` | Unscii 8x16 and 8x8 bitmap fonts, UTF-8 decoding, text width, wrap and elide |
| `ui_theme.*` | the two palettes (standard, High contrast), metrics, the contrast table, easing |
| `ui_input.*` | the kit's own events (`Nav`, pointer, scroll, raw key) and the d-pad/stick repeat timer |
| `ui_core.*` | `Context`: passes, widget identity, focus graph and spatial navigation, scroll areas, modals, page transition, key capture |
| `ui_art.*` | procedural icons, library-card emblems, the starfield, a generated test card |
| `ui_widgets.*` | button, toggle, stepper, slider, choice, list, message list, progress, card, badge, page selector, settings row, hint bar, command footer, key capture |
| `ui_png.*` | a minimal PNG writer for tests and screenshots |
| `ui_sdl.*` | SDL3: events to kit events, and showing the canvas in a window (`SdlPresenter`) |
| `demo/` | demonstration pages on mock data (a Display page and six more), and fixed scenes for the golden tests |
| `tests/ui/` | unit tests, golden tests, SDL tests and `x2ui_gallery`, a window to try the kit by hand |

CMake targets: `x2ui` (everything above except SDL: no dependency on the emulator or on SDL, so its tests run on every build), `x2ui_sdl`
(only with SDL3), `x2ui_demo`, and the test programs. `x2ui_gallery` is a developer tool and is not installed.

## How a frame works

- **The canvas.** The UI draws into a canvas of `window / n` pixels, where `n` is the largest whole number that keeps the canvas at least
  480x270 (320x180 with Large text). `SdlPresenter` uploads it to a streaming texture and scales it by `n` with nearest-neighbour filtering,
  centred in the window. All UI art is therefore crisp pixel art at any window size or display density.
- **Integer only.** Layout, drawing, animation and easing use integers (milliseconds, permille). No floating point reaches a pixel, so the
  same calls give the same pixels on every compiler and platform. Floating point appears only in the contrast calculation (a test) and in
  the SDL glue that maps pointer coordinates.
- **Immediate mode.** The page is a function. `Context::run(canvas, events, dt, builder)` runs it once for each input event (so one Down arrow
  or one Accept is seen by exactly one pass), then runs it again, without events, until nothing changes (a moved focus, a scrolled list).
  Every pass redraws the whole frame. `run()` returns whether anything on screen depends on time (`animating()`); a host that has no event
  and no animation does not call it, so an idle UI costs nothing. A widget returns what happened in this pass (`if (button(...)) start();`).
- **Identity.** A widget's id is a hash of its label under the current id stack (`push_id`/`pop_id`). Retained state (focus, scroll offsets,
  slider drag) lives in a map keyed by id.
- **Rect cutting layout.** No layout engine: pages cut rectangles off an area (`cut_top`, `cut_left`...) or stack rows with `Stack`.
  Responsive behaviour (a rail or a tab strip, one column or three cards) is an `if` on the canvas size, which is exact because fonts are
  fixed-width cells.

## Focus and navigation

- Every focusable widget registers a rectangle. Up, Down, Left and Right move to the best neighbour in that direction (distance along the
  axis plus three times the offset across it; ties go to the earlier widget). `Context::neighbor` is a pure function, and the tests use it to
  prove that every control of every demo page can be reached with only Up, Down, Left, Right, Accept and Back.
- **There is always a focused control.** If the focused widget disappears (a page change, a modal) the first widget with `default_focus`, or
  else the first on the page, takes it. Home and End go to the first and last control inside the current scroll area.
- Value widgets (toggle, stepper, slider, choice) use Left and Right themselves and never let focus leave sideways; the page selector is
  reached with **Back** from a page, or Left from a widget that does not use it. Lists use Up and Down inside and let focus out at the ends
  (menus can set `wrap`).
- A **modal** (`open_modal`, then `modal_begin`/`modal_end` last in the builder) dims the page, traps focus, closes on Back, and makes everything
  built before it inert, for the pointer too.
- **Key capture** (`key_capture`): Accept starts "Press a key..."; while it runs, navigation events are ignored and the host must deliver key
  presses as `RawKey` events (`SdlInput::set_capture(ctx.capturing())`, refreshed between events). Escape cancels, Backspace clears, a click
  elsewhere cancels.
- **Pointer.** Hover focuses (so the help line follows the mouse), a click needs press and release on the same widget, sliders drag, the wheel
  scrolls the area under it. Touch arrives from SDL as mouse events. Targets are at least 20 canvas pixels high.
- **Gamepad.** D-pad and left stick navigate with the repeat timing 400 ms then every 80 ms (`DirRepeater`); South accepts, East goes back,
  North is Details, West is Default, shoulders change page, Start opens the menu, Back/Select toggles the footer. Button names in the hint bar
  follow the connected pad (A/B/X/Y, Cross/Circle/Square/Triangle, or the Nintendo swap) and the last device used (keyboard names for the
  keyboard and the pointer).
- Keyboard: arrows, Enter/Space, Esc, Tab and Shift+Tab or PageDown and PageUp for pages, Home/End, Delete or Backspace for Default, F1 for the
  menu, I for Details. These are defined in `ui_sdl.cpp`; the in-game menu workstream decides how they meet the game's own keys.

## Look

Dark navy ground with amber (focus, primary) and cyan (command line) accents, chamfered 3-pixel corners, 1-pixel borders, no shadows, no
rounded corners. The focus is a 2-pixel amber ring with a 1-pixel ground-coloured gap, plus a lifted row and a marker, so it never rests on
colour alone. Icons and card emblems are drawn from rectangles and circles; there is no bitmap, no game art, no logo. High contrast is white
on black with a yellow focus, no dither, no starfield.

Contrast (WCAG ratios, computed from the palette; `ui_kit` asserts every pair the widgets draw with, at least 4.5 for text and 3 for the
focus ring and selection marker):

| Colour | on ground | on panel | on raised |
|---|---|---|---|
| text | 16.2 | 14.5 | 12.5 |
| dim | 7.9 | 7.1 | 6.1 |
| amber | 10.5 | 9.4 | 8.1 |
| cyan | 10.6 | 9.5 | 8.2 |
| ok | 10.7 | 9.6 | 8.3 |
| warn | 13.1 | 11.8 | 10.2 |
| err | 6.9 | 6.2 | 5.3 |

Text on an amber button is the ground colour (10.5). High contrast: text 21.0 / 21.0 / 17.0, dim at least 11.0, every accent at least 6.3.
Disabled controls fade towards the panel and are exempt, the way disabled controls are in WCAG; the reason is always in the help line.

Motion (all off with `Context::motion.reduced`): 90 ms page slide and fade, a 1-pixel breathing focus ring on cards, a moving dither stripe in
progress bars, and the launcher starfield (three layers, 12 updates a second). Nothing flashes.

## Font

**Unscii 2.1 by Viznut, the 8x8 and 8x16 variants. Public domain.** The licence was read from the upstream distribution before vendoring:
the project page says "`unscii-16-full` falls under GPL because of how Unifont is licensed; the other variants are in the Public Domain", and
`unscii.txt` in the source release says "Everything made by me (viznut) is in the Public Domain. The first source font, Fixedsys Excelsior,
is also in the Public Domain. Those pieces that come from the Unifont project ... are under GPL. For this reason, unscii-16-full also falls
under GPL." The upstream Makefile builds `unscii-8.hex` and `unscii-16.hex` from Viznut's own sources only; the Unifont glyphs are merged
into `unscii-16-full.hex`, which is **not** used. The two `.hex` files are vendored unmodified in `vendor/unscii/` with their checksums and
the quoted licence text (`LICENSE-unscii.txt`, also shipped as `packaging/licenses/Unscii-LICENSE.txt` and listed in
`THIRD-PARTY-NOTICES.md`). `tools/ui/make_font.py` extracts the subset the UI draws (ASCII, Latin-1, Latin Extended-A, some punctuation,
arrows, box drawing, block elements, geometric shapes: 594 glyphs in each face) into `runtime/ui/font_data.inc`; the `ui_font_data` test fails if
the committed file is not what the script makes from the vendored inputs. A character outside the subset is drawn as a hollow box.
The font is "based on classic system fonts" by upstream's own description; upstream dedicates the result to the public domain, and this
project relies on that statement.

## Tests

- `ui_kit` (`tests/ui/ui_tests.cpp`, about 1,100 checks): canvas operations with exact pixels and a pinned hash; font coverage and UTF-8
  edge cases; contrast of both themes; canvas geometry; the repeat timer; every widget's state machine by synthetic events (including
  clamping, wrap, disabled controls, pointer press-and-release rules, slider dragging past the ends); focus never empty; modal trapping; scroll
  to the focus, wheel and clamping; the message list following the bottom; key capture; the page selector in both layouts; reachability of
  every control of every demo page at both canvas sizes, for the disk and ST models; animation flags; determinism of every scene; frame cost.
- `ui_golden` (`tests/ui/golden/hashes.txt`): 12 scenes (every page, a scrolled page, a disabled-row page, key capture, a modal, two
  motion scenes) at 640x360 and 480x270, in both themes: 48 hashes compared at **zero tolerance**. The hashes are the FNV-1a 64 of the canvas,
  not PNG files, so the hygiene check needs no image allowance. To review a change: `x2_ui_golden_tests --dump DIR` writes every scene as a PNG
  (2x); a failing run writes the actual picture of each mismatch to `build/ui_golden_actual`; `x2_ui_golden_tests --update tests/ui/golden/hashes.txt`
  rewrites the file after review. `x2ui_gallery --shot out.png --scene display_st --size 640x360 --hc` writes one scene.
- `ui_sdl` (dummy video driver): every key, pad button, the d-pad and stick repeat and hysteresis, pointer mapping through the canvas
  geometry and wheel handling, key capture, an event stream driving a page, and the presenter reading back the right colours at the right
  places through the software renderer (which also pins the byte order of the texture).
- `ui_font_data`: the committed font subset matches its inputs.
- The sanitizer job of `.github/workflows/fuzz.yml` also builds and runs `ui_kit` and `ui_golden` under ASan and UBSan (the runner has no SDL3, so
  `ui_sdl` is not part of it; it was run under both sanitizers by hand and passes).
- Cost: a settled Display page frame (one event pass and the settle passes) takes about 0.1 ms on an Apple M-series core at -O2; the gate is
  40 ms (150 ms under the sanitizers). The work is integer fills into a 640x360 buffer, so a Raspberry Pi 3 has the same shape of work; its
  figure is a later measurement.

## Decision gate: stay with the in-house kit

The plan set a gate: if the kit took more than half as long again as estimated, switch to Dear ImGui. It did not. The kit, the font, one
real proving page (Display, with a live generated test card and the disabled-row reasons for the ST release), seven pages in all, 48 golden
scenes and 1,100 checks came to about 4,600 lines including tests. Zero-tolerance goldens, gamepad focus, the retro look and the no-new-dependency rule all hold. Recommendation: continue with the kit.

## Where this differs from the plan

- **Drawing goes to a CPU canvas, not to `SDL_Renderer`.** The plan drew with renderer primitives and a software renderer for the tests. A
  canvas of our own gives bit-identical output on every platform without depending on SDL's blending code, lets all of the kit be tested
  without SDL, and keeps the final step (a texture upload and a nearest-neighbour scale) trivial.
- **Goldens are hashes, not PNG files.** Same zero tolerance, no binary files in the repository, no hygiene allowlist. A PNG of any scene is
  one command away.
- **The help line shows the focused row's help**, not every row's. Rows are label, control, badge and hotkey; the one-line help of the focused
  row (or the reason a row is disabled) is drawn once under the list. It fits 480x270 and 1280x720 alike, and it follows the pointer too.
- **Disabled rows stay focusable** (inert), so a pad user can land on them and read why they are off.
- **Page selector:** keyboard or pad focus on a rail entry selects that page at once (focus follows selection); pointer hover does not.
- **F1 opens the menu** (Decisions in the plan), `Nav::Menu`; F10 is not used by the kit.
- Not done here, by design: the settings schema and the pages that use it (workstream B), the launcher and importer worker (C), the packs
  page (D), the in-game overlay hooks (E) and the Advanced page and logs (F). The demo pages are placeholders for them.
