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

## Related documents outside this directory

- [`../../docs/PROVENANCE.md`](../PROVENANCE.md) — which code here was written from external sources.
- [`../BUILDING.md`](../BUILDING.md) — every build option and runtime flag.
- [`../../tests/replays/README.md`](../../tests/replays/README.md) — the replay format and what the
  coverage-only chip-RAM writes do.
