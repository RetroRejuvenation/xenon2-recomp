# Static recompiler (Phase 5a): design

Goal: run the game's 68000 code as native C++ that is **exactly equivalent** to the Musashi interpreter. "Exactly"
means the same register file, SR, memory, bus accesses (address, size, value, order and beam time), cycle counts
and interrupt boundaries. Speed comes second. Anything the recompiler cannot prove equivalent stays on the
interpreter.

## 1. Lifting strategy: partial evaluation of the reference interpreter

The semantics of each instruction are **not re-derived by hand**. The generator `tools/recomp/recomp.py` uses
Musashi's own opcode handlers as the specification and specialises them for the one instruction at one address:

- `m68kmake` (vendored) produces `m68kops.c`. A small dumper (`tools/recomp/opdump.c`, built by the generator
  with the same Musashi sources and `runtime/x2_m68kconf.h`) runs Musashi's `m68ki_build_opcode_table()` and
  writes, for each of the 65,536 opcode words, the handler it dispatches to and its 68000 base cycle count
  (`CYC_INSTRUCTION`). The opcode-to-handler mapping is therefore Musashi's own, not a reimplementation.
- Every handler body that the game uses is copied into `generated/recomp/x2r_handlers.h` as an
  `always_inline` function `h_<name>(ir, pc, ext[])`. The generator changes only how the instruction stream is
  read:
  - `REG_IR` becomes the constant opcode word;
  - `OPER_I_*`, `EA_*_DI/IX/AW/AL/PCDI/PCIX` and `OPER_*` for those modes read their extension words from a
    `static const` array baked in at generation time;
  - `REG_PC` is still updated exactly as `m68ki_read_imm_16/32` would update it.

  The flag formulas (lazy `FLAG_X/N/Z/V/C` variables), the undefined-flag behaviour, the X flag, DIVU/DIVS/MULU/MULS
  timing, shift cycles, MOVEM, exceptions (divide by zero, CHK, TRAP, privilege, illegal), SR writes with their
  interrupt check, STOP and RTE are all Musashi's code, run on Musashi's state.
- The C++ compiler then folds the constant opcode and extension words: `DX` becomes `REG_D[3]` and `(d16,An)`
  becomes `REG_A[n] + 0x1C`. Decode, the jump-table dispatch and operand extraction are compiled away.

This is the first Futamura projection, applied per instruction. Control flow is lifted on top of it.

- **Granularity:** one C++ function per code-map function (Ghidra `functions.json`, the handler sets,
  indirect-site targets and API vectors), plus extra seeds from an optional execution profile. It holds every
  instruction reachable from the entry through branches (not calls).
- **Labels:** every instruction has two labels:
  - `L_<pc>` is a full boundary: cycle check, then instruction hook;
  - `E_<pc>` is the entry used by the dispatcher after it has already done those.
- **Branches:** after the handler runs, the successor is chosen by comparing `REG_PC` with the statically known
  targets: fall-through, Bcc/DBcc/BRA/BSR targets, and absolute `JMP`/`JSR`. Inside one function this becomes a
  direct `goto`. The comparison is cheap and stays in even when it looks redundant. Any other value
  (exception, RTS, RTE, `JMP (An)`, a trap that moved the PC) goes through the function's own `switch`, and
  from there back to the dispatcher.
- **Calls:** `JSR`/`BSR` to another function returns to the dispatcher. That keeps the native stack flat, so the
  executor can hand control to the interpreter at any instruction boundary.

## 2. CPU state model

There is no separate register file. Translated code reads and writes Musashi's own `m68ki_cpu`:

- `REG_D`, `REG_A`, USP/ISP in `REG_SP_BASE`;
- `FLAG_*`, `FLAG_S`, `FLAG_INT_MASK`;
- `REG_PC`, `REG_PPC`, `REG_IR`;
- the cycle counter `m68ki_remaining_cycles` and `m68ki_initial_cycles`.

Translated code and the interpreter can therefore swap at any instruction boundary with nothing to copy.

- The PC high byte is kept. Some code pointers carry flag bits in bits 24-31, as with `jsr (a1)` at `$47D0`. The
  interpreter then runs with, for example, `REG_PC=$80xxxxxx`, and those bits end up in pushed return
  addresses. Each translated function captures `pchi = REG_PC & $FF000000` on entry and builds every PC value it
  stores as `pchi | addr`. Successor tests compare the full 32-bit value.
- `REG_DA_SAVE` (the bus-error rollback copy) is not refreshed by translated instructions. Nothing reads it,
  because no bus error is ever raised (`M68K_EMULATE_ADDRESS_ERROR` and PMMU are off for this 68000).

## 3. Executor and instruction boundaries

`x2r_execute(int cycles)` (`runtime/recomp/executor.cpp`) is a drop-in replacement for `m68k_execute`. It is
installed through `Machine::cpu_execute`, and its loop is the same as Musashi's:

```
eat RESET_CYCLES; SET_CYCLES(n); initial = n; m68ki_check_interrupts();
if !STOPPED: do {
    hook(REG_PC) unless the previous translated block already called it for this boundary;
    fn = lookup(REG_PC & $FFFFFF)                            // per-word table over chip RAM
    k = fn ? fn(...) : 0                                      // k = instructions executed natively
    if k == 0: REG_PPC=REG_PC; save DA; REG_IR=fetch; handler(); USE_CYCLES(CYC[IR])   // Musashi, 1 insn
} while (GET_CYCLES() > 0)
REG_PPC = REG_PC; return initial - GET_CYCLES()
```

Each translated instruction starts with:

```
L_pc: if (GET_CYCLES() <= 0) return k;                  // Musashi's loop condition
      x2_instruction_hook(REG_PC);                       // traces, $7032 count, HLE traps
      if (REG_PC != pchi|pc) { hooked = 1; return k; }   // a trap (e.g. the disk shim $921A) moved the PC
E_pc: if (code bytes at pc != translated bytes) { hooked = 1; return k; }
      REG_PPC = pchi|pc; REG_IR = op; REG_PC = pchi|pc+2;
      h_<handler>(op, pchi|pc, ext);  USE_CYCLES(CYC_INSTRUCTION[op]);  ++k;
```

So translated code checks the cycle slice before **every** instruction and calls the hook at every boundary,
in the same order as the interpreter. It can stop at any boundary:

- slice used up;
- `m68k_end_timeslice()` from a bus write that raised an interrupt;
- a trap or hook PC;
- changed code bytes;
- an unresolved indirect target;
- untranslated code.

The resume state is the exact Musashi state.

- **Interrupts.** Musashi samples the IPL lines only in `m68k_execute` (at slice entry) and in `m68ki_set_sr`
  (MOVE/ANDI/EORI/ORI to SR, RTE, STOP). `m68k_set_irq` from a bus write never takes an exception
  immediately. `Machine::update_irq` ends the slice instead. The executor does the entry check and translated
  code runs the same `m68ki_set_sr`, so interrupts are taken at the same boundaries. No per-instruction
  interrupt poll is needed beyond the cycle check, which sees the slice ended by `m68k_end_timeslice()`.
- **Cycles.**
  - Within one instruction the order matches the interpreter: handler-internal `USE_CYCLES` (shift counts,
    MOVEM, DIV/MUL, branch not taken) happen inside the handler, and the base `CYC_INSTRUCTION[op]` (a
    generated constant) after it.
  - `Machine::hpos()` and `cck_now()` therefore see the same beam position at every bus access, so frame timing
    is bit-identical.
  - At start-up the executor checks every generated cycle constant against Musashi's live table and refuses to
    enable translation if any differs.
- **Bus-error trap.** Musashi arms its never-taken bus-error `setjmp` on every slice, which costs a
  `sigprocmask` system call on macOS. The executor arms the same `jmp_buf` with `_setjmp`.
- **Bus.** Every data access goes through Musashi's `m68ki_read/write_*_fc`, which calls
  `m68k_read/write_memory_*` and then `Machine::read*/write*`. Custom registers, CIAs, blitter, copper strobes,
  Paula catch-up, register-write logging, the tracer and `bus_log` all see the same calls. There is no chip-RAM
  fast path. Instruction-stream reads are the only accesses that do not reach the bus, because they are
  constants. They have no side effects in chip RAM.
- **Code identity and overlays.** Before each instruction runs, translated code compares its opcode and
  extension words with chip RAM; for the instructions the game executes this is one to three 16-bit compares.
  Level modules 1-5 and the shop/ending module are all loaded at `$54E00`, so one address can have up to six
  translations. The dispatch entry for an address lists every candidate. The executor tries the last module that
  matched first, and a candidate whose first instruction does not match executes nothing. Because each
  instruction's semantics depend only on its own bytes and its PC, this per-instruction check makes overlays,
  partially loaded modules and any self-modifying code safe. No module id or hash is needed.

## 4. Coverage and what stays interpreted

- **Coverage.** All 68000 opcodes that Musashi implements can be translated, privileged ones included. Leaving
  something untranslated never risks a mistranslation: those instructions simply run on the interpreter.
- **Untranslated by design:**
  - opcode words that Musashi maps to `illegal`, `1010` or `1111`, or that have no 68000 cycle entry (these are
    data, not code);
  - code whose decode leaves the module image or overlaps another instruction;
  - addresses at or above `$80000`.
- **Not found statically.** Code that static discovery does not reach is listed in the coverage report. The
  generator can take an execution profile (`--recomp-profile`) as extra seeds.

## 5. Verification

- **`x2_recomp_difftest`.** For each translated function, and also at random mid-function entries, it builds two
  machines from the same seed, each with:
  - random D/A registers (boundary values mixed in);
  - random CCR/X and an interrupt mask;
  - random data bytes everywhere except the instruction bytes;
  - a random cycle budget.

  It then runs the interpreter on one machine and the hybrid executor on the other. It compares the full CPU
  context (all registers, USP/ISP, SR, PC, PPC), all of chip RAM, the custom registers and CIA state, the cycles
  consumed, and the ordered list of data bus accesses (address, size, value, R/W, cycle stamp, REG_PC).
  - Instruction-stream reads are on the interpreter's bus but not in translated code. They are recognised as
    16/32-bit reads at `REG_PC - size`, because Musashi advances the PC first, and are dropped from both logs.
- **Full-game gate (`recomp_equivalence`).** It runs the scripted title, menu and level 1 for 2000 frames in both
  modes and requires:
  - identical per-frame framebuffer hashes;
  - identical frame/line-stamped register-write logs;
  - a bit-identical WAV.

`docs/re-notes/recompiler-report.md` records the generated instruction-set usage, the coverage numbers and the
known gaps.
