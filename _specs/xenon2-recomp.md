# Spec for Xenon 2 Megablast Multiplatform Recompilation

branch: claude/feature/xenon2-recomp

## Summary

Produce a native, multiplatform (macOS, Windows, Linux at minimum) recompilation of the 1989 Amiga vertical shoot-'em-up Xenon 2: Megablast. The shipped Amiga game code is statically translated from 68000 machine code into portable native code, running on top of a thin hardware-abstraction runtime that stands in for the Amiga's custom chips (Agnus/Denise/Paula), CPU services and OS calls. The result should play identically to the original at its native 50Hz, with no emulator involved at runtime.

The project folder contains the source material: the original two-disk Amiga ADF images, a collection of cracked/alternate Amiga releases, and the CDTV (CD-ROM) release disc image, which carries higher-quality CD audio. The Amiga disk version is the primary code target; the CDTV audio is an optional enhancement layer.

The approach and project structure should follow the precedent set by the existing James Pond 2: Codename Robocod recompilation (https://github.com/griches/JamesPond2RobocodRecomp/), adapting its lessons to Xenon 2's own engine, loader and copper/blitter usage. The reverse-engineering toolchain available is the REA skill/MCP tooling and Ghidra.

Beyond faithful reproduction, the following modernisation goals are accepted as additional, clearly separable goals: 60/120fps interpolated rendering, widescreen support, and upgraded sprite colours and resolution. These must never compromise the faithful mode, which remains the default reference behaviour.

## Functional requirements

### Source analysis and preparation
- Identify and verify which of the supplied images is the canonical, uncracked or best-quality Amiga disk set, and document the choice and checksums.
- Extract the contents of the disk images (boot block, loader, game executable(s), data files, packed assets) and document the disk layout, any custom track formats or copy protection, and how the game loads from disk.
- Identify and analyse the CDTV disc contents: data track, the audio tracks, and how the CDTV release differs from the disk release (loader, music playback, language selection for English/French/German/Italian).
- Produce a documented map of the game's code and data: entry points, main loop, interrupt handlers, level/data loaders, decompression routines, sprite/blitter routines, music/sound engine, input handling and high-score handling.
- Source assets and original game files are not redistributed by the project; the user must supply their own disk images, and the build/run flow must make that clear.

### Recompilation pipeline
- A repeatable, scripted pipeline that takes the user's supplied disk image(s) and produces the translated native code and extracted asset data, with no manual steps required once set up.
- Handle self-modifying code, jump tables, computed jumps, overlays and any decompression-then-execute stages, either by translating the post-decompression code or by an explicit documented strategy.
- Provide a way to verify translated functions against a reference (for example, comparing behaviour against the original running under an emulator) so regressions are detectable.
- Document how the pipeline works so another Amiga title could reuse the approach, referencing the James Pond 2 project's conventions where applicable.

### Runtime and hardware abstraction
- Emulate only what the game relies upon: CPU register/memory model, Chip/Fast RAM layout, custom chip registers, interrupts (vertical blank, copper, audio, disk), CIA timers and the keyboard/joystick/mouse interfaces.
- Reproduce the Amiga display faithfully: bitplane graphics, palette changes and copper-driven effects (split screens, per-scanline palette changes, scrolling), hardware sprites, sprite/playfield priority and collision registers, and blitter operations including line and fill modes if used.
- Reproduce Paula audio: four DMA audio channels, sample playback, period/volume changes, and the game's own music/sound-effect replay routine, with correct stereo channel mapping.
- Replace Amiga OS/library calls and trackloading with native equivalents, loading data from the user's extracted files.
- Run at the original 50Hz (PAL) timing by default, with correct game speed independent of host refresh rate.

### Game completeness
- Title screen, options, one- and two-player modes, all levels and bosses, shop/weapons purchase system, power-ups, scoring, lives/continues, game over and end sequences, and high-score table.
- Original behaviour preserved: enemy patterns, scrolling, collision, weapon behaviour, difficulty and random-number behaviour.
- Pause, quit and reset controls appropriate for a modern desktop application.

### Platform and input
- Native builds for macOS (Apple Silicon and Intel), Windows and Linux from one codebase, with a single documented build process.
- Window and fullscreen modes, integer and aspect-correct scaling, optional CRT-style filtering, vsync handling.
- Keyboard and gamepad support with remappable controls, covering both players; the original joystick (single fire button) plus modern pad conventions.
- Persistent settings and high scores stored in the standard per-user location on each platform.

### CDTV audio enhancement
- Optionally play the CDTV version's CD audio tracks as the music source in place of the tracker-style in-game music, including selecting the correct track for each game state (title, levels, bosses, shop, game over).
- Language selection for the supported CDTV languages (English, French, German, Italian) where the CDTV release provides it.
- Fall back cleanly to original Amiga audio when no CDTV image is supplied.
- Allow the user to choose between original Amiga audio, CDTV audio, or a mix per category (music vs. sound effects).

### Modernisation (additional goals, optional and off by default)
- **High frame rate**: render at 60Hz and 120Hz (and arbitrary host refresh rates) by interpolating object positions and scroll between 50Hz logic steps, keeping game logic at its original rate so gameplay is unchanged.
- **Widescreen**: extend the visible playfield beyond the original 4:3 view, handling the vertical-scrolling playfield, the side panels/status display and any edge-of-screen spawning so that widescreen never reveals unfinished edges or alters gameplay (enemies must not spawn or despawn visibly in the extended area, or the extended area must be handled in a documented, consistent way).
- **Upgraded sprite colours**: allow sprites and backgrounds to display with expanded colour depth beyond the 16/32-colour Amiga limits, including removing sprite colour-sharing limitations, where doing so does not change the game's look unexpectedly.
- **Upgraded resolution**: render at higher internal resolution with clean scaling, and support optional replacement/upscaled asset packs in a defined, documented format.
- Modernisation settings are exposed through a settings menu and persisted, and a single switch restores the fully faithful presentation.

### Documentation and project hygiene
- A README covering what the project is, legal position (no game data included), how to supply the original files, how to build and run on each platform, and current status.
- Documentation of the reverse-engineering findings (memory map, data formats, function notes) kept alongside the project.
- Version control history that separates tooling, runtime, translated code and modernisation work.

## Possible edge cases

- The disk version uses a custom loader or trackdisk format, or copy protection (including protection in the cracked releases supplied), which must be neutralised or bypassed without redistributing cracks.
- Multiple cracked/trained versions in the supplied archives contain trainers or intro screens added by crackers; the pipeline must target an unmodified or clearly identified clean version.
- Code that is decompressed, relocated or generated at runtime, self-modifying code, and indirect jumps through computed tables may defeat static translation.
- Timing-dependent behaviour: the game may depend on exact raster position, copper timing, CIA timer values or blitter completion timing for effects, and these need cycle-approximate handling.
- Mid-frame palette and register changes that rely on beam position and must be reproduced at the correct scanline.
- Hardware sprite reuse (multiplexing) and sprite-to-playfield collision flags the game reads back from custom chips.
- Memory-layout assumptions: the game may read or write past buffers, rely on uninitialised memory, or assume specific Chip RAM addresses.
- 68000-specific behaviours: address errors, odd-address access, flags edge cases, and instructions with undocumented side effects.
- Two-player mode ports and controller conflicts on a single keyboard.
- The 50Hz game versus 60Hz or higher host displays producing judder if interpolation is disabled; variable-refresh and 120Hz+ displays.
- Interpolation artifacts: objects that teleport, spawn, wrap or change frames between logic steps must not be smeared across the screen.
- Widescreen revealing parts of the playfield where the original relied on hidden tile edges, off-screen spawning or scrolling buffer limits; the HUD/status panel must adapt sensibly.
- Upgraded sprites interacting with original collision, priority and copper effects, and sprites that rely on palette tricks.
- The CDTV audio tracks may not align with the disk version's music cues; CD tracks are Red Book audio inside a multi-track bin image and the cue sheet structure needs handling; loop points and track transitions may be abrupt.
- Hosts with no audio device, unusual sample rates or high audio latency.
- Save/high-score file corruption or the per-user folder not being writable.
- Different Amiga ROM versions or PAL/NTSC differences affecting speed.

## Acceptance criteria

- The user can supply their own Amiga disk image(s) and run a single documented pipeline to produce a working native build on macOS, Windows and Linux.
- With modernisation features off, the game boots to the title screen and is playable from the first level through to the end of the game on all supported platforms, with no emulator at runtime.
- Visuals, scrolling, enemy behaviour, weapons, scoring and the shop match the original game when compared side by side with a known-good emulator capture.
- Music and sound effects play with correct timing and stereo placement, matching the original Amiga audio.
- Game speed is correct at 50Hz regardless of host display refresh rate, and does not drift over a long session.
- Both one- and two-player modes work with keyboard and gamepad, with remappable controls.
- High scores and settings persist between sessions in a per-user location.
- When a CDTV image is supplied and enabled, the CDTV music plays in the correct places, with a clean fallback when absent.
- 60Hz and 120Hz modes render smoothly without altering gameplay and without visible interpolation artifacts on spawns, transitions or screen wraps.
- Widescreen mode displays an extended playfield with no visible edge defects and no change to gameplay outcomes.
- Upgraded colour/resolution options work and can be toggled off to return to the exact original look.
- No original game assets, ROMs or cracked code are included in the repository; the repository builds and passes its checks without them present.
- README and reverse-engineering documentation exist and are accurate to the delivered state.

## Open questions

- Which release is the canonical code target: the original ImageWorks disk set, or one of the cracked sets, and do any releases differ materially in code or levels?
- Is the CDTV version a pure re-release of the disk code with CD audio added, or does it have materially different code, loader or levels, and should the project translate both?
- What implementation language and graphics/audio/input library should the runtime use, and should the James Pond 2 project's choices be mirrored exactly or only loosely?
- How closely must raster-timing effects be reproduced: scanline-accurate, or frame-accurate with per-line palette tables?
- What is the target minimum host spec, and are mobile or web targets in scope later?
- How will the pipeline be validated against the original in practice: emulator trace comparison, frame-hash comparison, or manual play-through?
- What format should replacement/upscaled asset packs take, and are community-supplied packs in scope?
- Should the PAL timing be the only supported mode, or should NTSC (60Hz) timing also be available?
- Licensing: which licence applies to the project's own code, and are there concerns about distributing tooling that derives from the original binary?
  **Answered 2026-10-30: GPL-3.0-or-later**, because the accurate-timing code is derived from GPL
  WinUAE/Amiberry sources. See `LICENSE` and `docs/PROVENANCE.md`.
- Can the widescreen area be populated sensibly without changing gameplay (for example extended backgrounds), and what is acceptable if the original data does not provide it?

## Testing guidelines

- Create automated tests for the CPU/instruction translation layer, covering each 68000 instruction class, addressing modes and flag behaviour, validated against a reference implementation.
- Add custom-chip tests for blitter operations (including minterms, shifts, masks, line and fill modes), copper list execution, sprite rendering and priority, and Paula audio mixing.
- Use frame-by-frame comparison against reference emulator captures at fixed points (title screen, each level's start, each boss, shop, game over) using image hashing or tolerance-based diffing.
- Use deterministic input replay recordings to run through levels headlessly and compare game state and frames to references, catching translation regressions.
- Test audio output against reference captures for music and sound effects, including channel levels and stereo mapping.
- Test the pipeline end to end from a clean checkout with user-supplied images, on each platform in continuous integration where possible, and verify it fails with a clear message when images are missing or wrong.
- Verify timing: confirm logic runs at 50Hz under 50, 60, 75, 120 and 144Hz host refresh rates, and that interpolated modes keep logic state identical to non-interpolated runs.
- Test widescreen at several aspect ratios (16:9, 16:10, 21:9, 32:9) and at window resize and fullscreen switching.
- Test input with keyboard, multiple gamepad types, hot-plugging, and two simultaneous players.
- Test the CDTV audio path with and without the image present, for each language, and with malformed or partial images.
- Manual full play-through of the game on each platform before each milestone release, including both player modes and a high-score entry.
- Verify that no copyrighted game data is tracked in the repository.
