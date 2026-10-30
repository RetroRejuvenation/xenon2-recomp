// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Hybrid executor: runs translated code (generated/recomp, built with -DX2_RECOMP=ON) and falls back to the
// Musashi interpreter at any instruction boundary. See docs/re-notes/recompiler-design.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x2 {
class Machine;
}

namespace x2::recomp {

struct Stats {
    uint64_t translated = 0;   // instructions executed by translated code
    uint64_t interpreted = 0;  // instructions executed by the interpreter
    uint64_t calls = 0;        // translated function entries
    std::vector<uint32_t> interp_pc; // per 16-bit word of chip RAM: interpreted executions at that PC
    uint64_t interp_high = 0;  // interpreted at a 24-bit PC >= $80000 (outside chip RAM)
};

// Installs the hybrid executor as `m.cpu_execute`. Fails (and leaves the interpreter in place) if a translated
// cycle constant disagrees with Musashi's table or no translation is linked in.
bool enable(Machine& m, std::string& err);
// Back to plain m68k_execute.
void disable(Machine& m);
// Number of translated instructions linked in (0 = interpreter-only build).
size_t translated_instructions();
Stats& stats();
// "PC COUNT HEXBYTES" lines for the most-interpreted PCs (seed file for tools/recomp/recomp.py --profile), and a
// readable coverage summary.
bool write_profile(const Machine& m, const std::string& path);
void print_summary(const Machine& m, int top);

// Drop-in replacement for m68k_execute() (exposed for tests).
int execute(int cycles);

} // namespace x2::recomp
