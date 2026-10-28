# Static recompiler (Phase 5a): results

Design: `recompiler-design.md`. All numbers below come from the reference `Xenon2.zip` assets (see
`docs/BUILDING.md`).

## Regenerate, build and test

```
python3 -I tools/prep_assets.py --zip Xenon2.zip                 # once: generated/assets
python3 -I tools/recomp/recomp.py --trace generated/trace       # -> generated/recomp (gitignored), ~2 s
cmake -S . -B build-recomp -G Ninja -DX2_RECOMP=ON
cmake --build build-recomp                                       # generated code: ~1 min CPU
ctest --test-dir build-recomp --output-on-failure                # all tests, incl. recomp_difftest + recomp_equivalence
./build-recomp/x2recomp --headless --frames 2000 --recomp-stats 20 ...   # hybrid (default in this build)
./build-recomp/x2recomp --headless --frames 2000 --interp-only ...       # same binary, interpreter only
python3 -I tools/recomp/isa_report.py --trace RUN.json           # instruction-set usage (RUN.json from --trace-out)
```

- **What the generator needs:**
  - `generated/assets/{game_0400.bin,disc2.bin}`;
  - the code map in `generated/lane-d2/` (`functions.json`, `entrypoints_v2.json`, `indirect_sites.json`);
  - a C compiler (it builds the vendored `m68kmake` and `tools/recomp/opdump.c`);
  - capstone, for the optional length cross-check.
- **Default build:** without `-DX2_RECOMP=ON` it is unchanged and does not need `generated/recomp`.
- **Trace seeds:** `--trace DIR` adds the call, indirect-jump and interrupt targets seen in a coverage run
  (`runtime/trace` / `tools/trace`: `coverage.json` and `indirect_targets.json`). This picks up object handlers
  that the Ghidra code map never decoded. Without these seeds, worker-11's replays would have run about 36% of
  level 4's and 57% of level 5's executed instructions on the interpreter.
- **Profile seeds:** `--recomp-profile FILE` writes the PCs that were still interpreted. Pass the file to
  `recomp.py --profile FILE` to translate them as well. With the code map and trace seeds, no profile is needed.

## Translation

The generator emits these files into `generated/recomp`:

- `x2r_handlers.h`: 559 Musashi handlers;
- 15 `x2r_<module>_<n>.cpp` files;
- `x2r_tables.cpp`: the dispatch entries;
- `manifest.json`.

| Image | Functions | Translated instructions |
|---|---|---|
| game (resident, `$400`) | 326 | 8,287 |
| level 1 (`$54E00`) | 80 | 1,151 |
| level 2 | 85 | 1,716 |
| level 3 | 79 | 1,333 |
| level 4 | 99 | 1,521 |
| level 5 | 101 | 1,525 |
| slot 6, shop and ending | 119 | 2,292 |
| **total** | **889** | **17,825** |

- **Function count.** There are 889 functions, against 601 in the code map. The extra ones are `bsr`/`jsr`
  targets found while building the CFGs, plus trace seeds.
- **Over-translation.** The static total includes some data decoded as code, such as runs of `ori.b #0,d0`
  (`$0000`). This is harmless:
  - each instruction checks its own bytes before it runs;
  - the semantics are Musashi's for whatever bytes are there.
- **Length cross-check.** The generator's decoder and capstone agree on 32,281 of 32,297 instruction lengths,
  counting functions that share code. All 16 disagreements are on data words, where capstone decodes 68020
  full-extension index formats (`([bd,An],Xn)`) or treats `btst Dn,#imm` as long. The 68000, like Musashi,
  uses the brief format. Even a wrong length could not cause a mistranslation (see the design, section 1).

## Instruction set

- **Static.** 78 mnemonics and 559 Musashi handlers (mnemonic x size x addressing mode) appear in the
  translated code.
- **Dynamic (the 2000-frame gate run, 23,200,881 instructions).** 69 mnemonics and 348 handlers:
  - `move`/`movea`/`moveq`/`movem` (including `move #imm,SR`);
  - `add`/`adda`/`addi`/`addq`/`addx`, `sub`/`suba`/`subi`/`subq`;
  - `cmp`/`cmpa`/`cmpi`, `tst`;
  - `and`/`andi` (including `andi #,CCR`), `or`/`ori` (including `ori #,CCR`), `eor`/`eori`, `not`, `neg`;
  - `clr`, `ext`, `swap`;
  - `mulu`/`muls`, `divu`/`divs`;
  - `asl`/`asr`/`lsl`/`lsr`/`rol`/`ror`, with immediate and register counts;
  - `btst`/`bset`/`bclr`/`bchg`, both static and dynamic bit numbers;
  - `Bcc`/`BRA`/`BSR` in both `.b` and `.w` forms;
  - `DBcc` (`dbf`, `dbcs`, `dbge`, `dbne`), `Scc` (`st`, `sf`, `seq`);
  - `jmp` (`(An)`, `abs.w/l`, `(d8,An,Xn)`, `(d8,PC,Xn)`), `jsr`, `rts`, `rte`, `lea`, `pea`.

  Every 68000 addressing mode occurs.
- **Present but not executed in that run:** `movep.l`, `link`, `exg`, `nop`, `move SR,(An)+`, `svs`/`scs`/`scc`,
  `dbeq`, `dblt` and `addx.b`. They are translated, and the differential test exercises them.
- **Not used anywhere:**
  - BCD instructions (`abcd`, `sbcd`, `nbcd`);
  - `roxl`/`roxr`, `negx`, `subx`, `cmpm`, `tas`, `chk`, `trap`, `trapv`, `stop`, `reset`, `move usp`, `rtr`.

### Privileged instructions and exceptions

- **Translated like everything else:** `move #imm,SR`, `ori/andi #,CCR` and `rte` in the interrupt handlers.
  They run Musashi's own code, including `m68ki_set_sr()` and its interrupt check.
- **Run as Musashi's code inside the translated instruction:** DIVU/DIVS by zero, CHK and privilege
  violations, raising the exception exactly as the interpreter does. The executor then continues at the vector.
- **Address errors:** none, because the runtime's Musashi configuration does not emulate them.

### Untranslated by design

1. Opcode words that Musashi dispatches to `illegal`, `1010` (line A) or `1111` (line F).
2. Opcode words that have no 68000 cycle entry, that is 68010/020/030/040-only forms such as `moves`,
   `cas`, `pack`, `bftst`, `chk2`, `divl` and `cpBcc`. Musashi would raise an illegal-instruction exception for
   them.

Decoding stopped at 134 such words in all. Every one is in data reached by falling through after a
non-returning call, or past the end of a table. None is executed in the gate run. If one ever is, the
interpreter runs it.

## Verification results

- **`recomp_difftest`** (CTest, about 75 s):
  - Each of the 889 functions gets 12 trials at its entry and 3 random mid-function entries: 13,335 cases.
  - Every case is run on the interpreter and on the hybrid executor, from:
    - random registers, with boundary values mixed in;
    - random CCR/X, S and interrupt mask;
    - a PC high byte in 10% of runs;
    - pending interrupts in a third of runs;
    - random memory around the real images, with data bytes randomised;
    - random cycle budgets of 1-2650.
  - Result: **0 differences** across all compared state:
    - all Musashi CPU variables, raw lazy flags included;
    - chip RAM;
    - custom registers and CIAs;
    - the register-write log;
    - cycles;
    - the hook sequence;
    - the data bus accesses with their cycle and PC stamps.

  A larger run (`--trials 40 --mid 12 --seed 0xC0FFEE`, 45,136 cases) also found 0 differences.
- **Sensitivity check.** Two deliberate mutations both failed the gates:
  - +2 cycles on the VBL handler's first instruction made `recomp_equivalence` report 1,374 of 2,000 register
    logs different, plus different audio and chip RAM. This was measured with an earlier version of the input
    script that also moved the ship.
  - setting V on `moveq #0` made `recomp_difftest` report SR/memory differences.
- **`recomp_equivalence`** (CTest, about 7 s). The scripted run (title, then SELECT menu, GET READY and level
  1, firing, 2,000 frames) compares interpreter and hybrid and gives:
  - all **2,000 framebuffer hashes identical**;
  - all **2,000 register-write logs** (frame/line/hpos stamped) identical;
  - **audio WAV bit-identical**;
  - final chip RAM identical.

## Coverage of the gate run

| | Instructions | Share |
|---|---|---|
| executed | 23,200,881 | 100% |
| translated code | 23,200,881 | **100.00%** |
| interpreter | 0 | 0% |

- **Cross-check.** The interpreter trace of the same run (`--trace-out`, `runtime/trace`) has 23,200,883 hook
  entries. That is two more, which are the two disk-shim traps at `$921A`: the trap runs in place of that
  instruction.
- **No untranslated hot spots** remain. The executor reports the most-interpreted PCs with
  `--recomp-stats N`.
- **Other modules:** the 2,000-frame gate never loads level modules 2-5 or the shop/ending module. The replay
  runs in "Replay equivalence" cover them.

## Replay equivalence

worker-11's replays (`tests/replays/*.txt`, run with `--input-script`) cover the attract mode, the 2-player menu,
levels 1-5, the shop, game over and the ending. Each replay is 5,000-6,000 frames and uses coverage-only pokes.
Each was run on the interpreter and on the hybrid executor. For every replay:

- the per-frame framebuffer hashes and the audio hashes are identical;
- the register-write logs for every frame are identical;
- the final chip RAM is identical;
- 100.00% of executed instructions ran as translated code.

The script that runs these checks is not a CTest yet, because the replays and `--input-script` belong to the
Phase 5b work.

## Performance

The 2,000-frame headless run without logs (Apple silicon, RelWithDebInfo):

- interpreter: 3.1 s;
- hybrid: 1.5 s.

The rest of the time goes mostly to the scanline renderer and Paula. About half of the interpreter's time is
Musashi's per-slice `setjmp` (`sigprocmask` on macOS). The executor uses `_setjmp` for the same, never-taken
bus-error trap.

## Known issues and limits

- **Per-instruction overhead.**
  - Every translated instruction still calls the instruction hook (`x2_instruction_hook`).
  - Every translated instruction compares its code bytes with memory.
  - All memory accesses go through `m68k_read/write_memory_*`.

  This is deliberate: it keeps traces, HLE traps, the tracer's write watch and overlays exact.
- **Many short native calls.** Calls between functions return to the executor, and the copper slots end a CPU
  slice every 32 cycles. So the 23M instructions in the gate run are spread over 8.3M translated entries.
- **Bus-access comparison in the difftest.** It removes instruction-stream reads by the rule "16/32-bit read at
  REG_PC - size", on both sides. A data read of exactly the word just fetched would be removed too. Its value
  still reaches the compared registers and memory.
- **Vendored Musashi.** `tools/recomp/recomp.py` copies handler bodies from the Musashi that is built. After a
  Musashi update:
  1. regenerate with `recomp.py`;
  2. at start-up the executor checks the cycle constants against Musashi's table;
  3. re-run both gates.
