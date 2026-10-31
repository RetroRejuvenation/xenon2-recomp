# Reverse-engineering notes — index

Working notes from the analysis of Xenon 2 Megablast and from the design of the runtime. Each file states
the question it answers and the evidence behind the answer. Most carry a confidence key: **H** = read in the
code, **M** = inferred, **L** = guessed.

These are notes, not documentation of the current code. Where a note disagrees with the source, the source
wins. Addresses are the game's own (the resident image lives at `$400`, level modules at `$54E00`).

## Where the game's data lives

| File | What it answers |
|---|---|
| [`sources.md`](sources.md) | Catalogue of the known Xenon 2 disk images: which releases exist, their SHA-1s, and which ones the importer accepts or rejects. |
| [`disk-layout.md`](disk-layout.md) | How the two `[t+3]` disks are laid out: boot block, AmigaDOS files, the hunk structure of `XENON-II` and `BS1`, the decrunchers, and where each level module sits on disc 2. |
| [`memory-map.md`](memory-map.md) | The boot chain from the boot block to the running game, and what occupies chip RAM once it is up. |
| [`cdtv.md`](cdtv.md) | Analysis of the CDTV release's disc: track layout, the ISO 9660 file system, and how its build differs from the floppy one. |

## What the game's code does

| File | What it answers |
|---|---|
| [`ghidra-code-map.md`](ghidra-code-map.md) | **The current code map.** Function boundaries, indirect call sites, hardware access and the API jump table, from a Ghidra 12.1.4 headless pass. Authoritative where it disagrees with `code-map.md`. |
| [`code-map.md`](code-map.md) | **Superseded** by the above for anything structural, but still the better prose description of the boot path, the main loop and the interrupt handlers. Its known errors are listed in §8 of `ghidra-code-map.md`. |
| [`audio-notes.md`](audio-notes.md) | The game's sound driver: its entry points, how it drives the Paula registers, and what the runtime's Paula model has to provide for it. |
| [`trace-coverage.md`](trace-coverage.md) | How much of the game's code the replay corpus actually executes, and how the traced code compares with the Ghidra map. |

## How the runtime reproduces it

| File | What it answers |
|---|---|
| [`timing.md`](timing.md) | Frame timing and the accurate bus model: why the game runs its logic every 3 VBLs, the OCS DMA slot model, blitter cycle timing, CPU wait states, and what the model leaves out. The companion to `runtime/machine_timing.cpp`. |
| [`reference-compare.md`](reference-compare.md) | Does the runtime match a reference emulator running the original disks? Pictures, palette, logic rate, stereo balance and game state, compared against Amiberry from the title to 1,400 frames into level 1. |
| [`recompiler-design.md`](recompiler-design.md) | How the static recompiler translates the game's 68000 code to C++ that is exactly equivalent to the Musashi interpreter, and what "exactly" is defined to mean. |
| [`recompiler-report.md`](recompiler-report.md) | What the recompiler actually achieved: how much code it translates, what it leaves to the interpreter, and the measured result. |
| [`modernisation-design.md`](modernisation-design.md) | How the optional modernisation features (interpolated frames, widescreen surround, replacement asset packs, HD panel) are built without touching game logic, audio or the faithful picture. |
| [`cdtv-music-map.md`](cdtv-music-map.md) | Which CD track the CDTV release plays where the floppy game plays each tune, and how the runtime's optional CD-audio layer reproduces that. |
| [`music-dropouts.md`](music-dropouts.md) | Why the music seems to stop sometimes: every way the game silences it, the tune's own 14.8 s rest, the reference comparison that shows it is authentic, and what can still make a dropout in the live output. |

## Disk images, formats and release variants

| File | What it answers |
|---|---|
| [`ending-flow.md`](ending-flow.md) | How the game ends (level 5, the ending, the loop back to level 1) and why that is the original's behaviour. |
| [`dms.md`](dms.md) | The DMS (Disk Masher) decoder: the format, every mode, and how it was validated on real files. |
| [`flux.md`](flux.md) | The flux readers (SCP, KryoFlux streams, HFE): the format notes, the phase-locked loop and its measured limits. |
| [`whdload.md`](whdload.md) | WHDLoad installs and LHA archives: what an install holds, the floppy versions v1-v6 and their layouts. |
| [`retail-disk1.md`](retail-disk1.md) | The retail first disk: layout, protection, and how it relates to the canonical (BS1) image. |
| [`retail-protection.md`](retail-protection.md) | The in-game Copylock check in retail images and how the runtime answers it from a key-track receipt. |
| [`single-disk-variants.md`](single-disk-variants.md) | Power Pack, the 1762 compilation, "4 Others" and the L.G.C.S.S. disk, and the builds behind them. |
| [`build-variants.md`](build-variants.md) | The game builds the runtime can start (Power Pack, Bombuzal + Xenon 2): the build table and what each needs. |
| [`cracks.md`](cracks.md) | Cracked and re-packed variants: what they hold and how the canonical inputs are recovered. |
| [`atari-st-machine.md`](atari-st-machine.md) | The Atari ST machine the runtime models (`runtime/st`): what boots from the Kixx dump, how the controller, MFP, IKBD and Shifter are modelled, the simplifications, and the tests. |
| [`acquisitions-flux-acorn-st.md`](acquisitions-flux-acorn-st.md) | The first real retail flux dump (the PDos release), the Acorn User '97 update and the Atari ST dumps. |
| [`other-versions-and-sources.md`](other-versions-and-sources.md) | Other versions of the game and where development material might be; rights holders. |

## The CDTV release

| File | What it answers |
|---|---|
| [`cdtv.md`](cdtv.md) | The disc: tracks, ISO 9660 volume, how it differs from the disk release (listed above under the game's data). |
| [`cdtv-code.md`](cdtv-code.md) | The `X2` program and its overlays: OS use, memory, overlay format, shared and new code. |
| [`cdtv-slave.md`](cdtv-slave.md) | What the WHDLoad CDTV slave patches, i.e. what the program needs from the machine. |
| [`cdtv-c0.md`](cdtv-c0.md) | The unknowns settled before implementation: overlays, start-up block, CD commands, variable map, trap list. |
| [`cdtv-runtime-gap.md`](cdtv-runtime-gap.md) | What the runtime lacked and the three architectures costed (the pre-link approach was chosen). |
| [`cdtv-importer.md`](cdtv-importer.md) | The importer: linking `X2` at fixed addresses, the stored files, the manifest. |
| [`cdtv-boot.md`](cdtv-boot.md) | Boot and runtime services: the traps, input, assists, scores. |
| [`cdtv-audio.md`](cdtv-audio.md) | CD audio, speech and effects: the state-to-track table, the poll, loop, volume and mix findings. |
| [`cdtv-replays.md`](cdtv-replays.md) | The 23-replay corpus: hook and variable map, what each reaches. |
| [`cdtv-recomp.md`](cdtv-recomp.md) | The CDTV release on the translated executor. |
| [`cdtv-modern.md`](cdtv-modern.md) | Interpolation, widescreen, asset packs and the interface on the CDTV release. |
| [`cdtv-reference.md`](cdtv-reference.md), [`cdtv-reference-whd.md`](cdtv-reference-whd.md) | The reference runs: Amiberry from the disc, and the WHDLoad slave route; the pixel and RAM comparison. |
| [`cdtv-data-region.md`](cdtv-data-region.md) | The 6.9 MB after the ISO volume: what it is (another title's audio) and why it is ignored. |

## Related documents outside this directory

- [`../../docs/PROVENANCE.md`](../PROVENANCE.md) — which code here was written from external sources.
- [`../BUILDING.md`](../BUILDING.md) — every build option and runtime flag.
- [`../../tests/replays/README.md`](../../tests/replays/README.md) — the replay format and what the
  coverage-only chip-RAM writes do.
