// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Hybrid executor: m68k_execute() with translated code. See docs/re-notes/recompiler-design.md section 3.
#include "recomp/recomp.h"

#include "machine.h"
#include "recomp/x2r_api.h"

#include <algorithm>
#include <cstdio>

extern "C" {
#include "m68kcpu.h"
#include "m68kops.h"
extern int m68ki_initial_cycles; // m68kcpu.c (not declared in its headers)
}

uint8_t* x2r_chip = nullptr;
int x2r_hooked = 0;

namespace x2::recomp {

namespace {

constexpr uint32_t kWords = kChipRamSize / 2;

Stats g_stats;
// Per 16-bit word of chip RAM: 0 = no translation, else index into g_pool of a null-terminated candidate list
// (one entry per module that has a translation at this address).
std::vector<uint32_t> g_slot;
std::vector<const x2r_entry*> g_pool;
size_t g_count = 0;

void build_tables() {
    if (!g_slot.empty()) return;
    std::vector<std::vector<const x2r_entry*>> lists(kWords);
    for (const x2r_module* m = x2r_modules; m->name; ++m)
        for (int i = 0; i < m->count; ++i) {
            const x2r_entry& e = m->entries[i];
            if (e.pc < kChipRamSize && !(e.pc & 1)) lists[e.pc >> 1].push_back(&e);
        }
    g_slot.assign(kWords, 0);
    g_pool.assign(1, nullptr);
    for (uint32_t w = 0; w < kWords; ++w) {
        if (lists[w].empty()) continue;
        g_slot[w] = uint32_t(g_pool.size());
        for (auto* e : lists[w]) g_pool.push_back(e);
        g_pool.push_back(nullptr);
        g_count += lists[w].size();
    }
}

// Runs translated code at REG_PC if any candidate's first instruction matches memory. Returns instructions
// executed (0 = nothing ran; x2r_hooked may then be set but is meaningless).
inline int run_translated(uint32_t pc) {
    if (pc >= kChipRamSize || (pc & 1)) return 0;
    uint32_t s = g_slot[pc >> 1];
    if (!s) return 0;
    for (uint32_t i = s; g_pool[i]; ++i) {
        const x2r_entry* e = g_pool[i];
        x2r_hooked = 0;
        int k = e->fn(pc);
        if (k) {
            if (i != s) std::swap(g_pool[i], g_pool[s]); // the module now loaded is tried first next time
            ++g_stats.calls;
            g_stats.translated += uint64_t(k);
            return k;
        }
    }
    return 0;
}

} // namespace

int execute(int num_cycles) {
    // Musashi's m68k_execute(), with translated code tried at every instruction boundary.
    if (RESET_CYCLES) {
        int rc = RESET_CYCLES;
        RESET_CYCLES = 0;
        num_cycles -= rc;
        if (num_cycles <= 0) return rc;
    }
    SET_CYCLES(num_cycles);
    m68ki_initial_cycles = num_cycles;
    m68ki_check_interrupts();
    if (!CPU_STOPPED) {
        m68ki_set_address_error_trap();
        // As m68k_execute(): Musashi's macro is _setjmp on macOS/BSD (no signal-mask system calls per slice) and
        // plain setjmp elsewhere (glibc's does not save the mask either; MSVC and MinGW have no one-argument
        // _setjmp). Bus errors are never raised here (no m68k_pulse_bus_error, no PMMU).
        m68ki_check_bus_error_trap();
        // `hooked`: the hook for the current boundary has already run (inside translated code). Musashi then
        // executes the instruction at REG_PC without re-checking the cycle count, so this loop does too.
        bool hooked = false;
        for (;;) {
            m68ki_trace_t1();
            m68ki_use_data_space();
            if (!hooked) m68ki_instr_hook(REG_PC);
            int k = run_translated(REG_PC & 0xFFFFFF);
            if (k) {
                hooked = x2r_hooked != 0;
                if (!hooked && GET_CYCLES() <= 0) break;
                continue;
            }
            hooked = false;
            uint32_t pc = REG_PC & 0xFFFFFF;
            REG_PPC = REG_PC;
            for (int i = 15; i >= 0; i--) REG_DA_SAVE[i] = REG_DA[i];
            REG_IR = m68ki_read_imm_16();
            m68ki_instruction_jump_table[REG_IR]();
            USE_CYCLES(CYC_INSTRUCTION[REG_IR]);
            m68ki_exception_if_trace();
            ++g_stats.interpreted;
            if (pc < kChipRamSize) ++g_stats.interp_pc[pc >> 1];
            else ++g_stats.interp_high;
            if (GET_CYCLES() <= 0) break;
        }
        REG_PPC = REG_PC;
    } else {
        SET_CYCLES(0);
    }
    return m68ki_initial_cycles - GET_CYCLES();
}

size_t translated_instructions() {
    build_tables();
    return g_count;
}

Stats& stats() { return g_stats; }

bool enable(Machine& m, std::string& err) {
    build_tables();
    if (!g_count) {
        err = "no translated code linked in";
        return false;
    }
    for (const x2r_module* mod = x2r_modules; mod->name; ++mod)
        for (int i = 0; i < mod->count; ++i) {
            const x2r_entry& e = mod->entries[i];
            if (CYC_INSTRUCTION[e.op] != e.cycles) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "cycle table mismatch at %s $%06X op %04X: generated %u, Musashi %u",
                              mod->name, e.pc, e.op, e.cycles, unsigned(CYC_INSTRUCTION[e.op]));
                err = buf;
                return false;
            }
        }
    x2r_chip = m.chip.data();
    if (g_stats.interp_pc.empty()) g_stats.interp_pc.assign(kWords, 0);
    m.cpu_execute = &execute;
    return true;
}

void disable(Machine& m) { m.cpu_execute = &m68k_execute; }

bool write_profile(const Machine& m, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# interpreted PCs: pc count code-bytes (seed file for tools/recomp/recomp.py --profile)\n");
    for (uint32_t w = 0; w < g_stats.interp_pc.size(); ++w) {
        if (!g_stats.interp_pc[w]) continue;
        uint32_t pc = w * 2;
        std::fprintf(f, "%06X %u ", pc, g_stats.interp_pc[w]);
        for (uint32_t i = 0; i < 6 && pc + i < kChipRamSize; ++i) std::fprintf(f, "%02x", m.chip[pc + i]);
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

void print_summary(const Machine& m, int top) {
    const Stats& s = g_stats;
    uint64_t total = s.translated + s.interpreted;
    std::printf("recomp: %llu instructions, %llu translated (%.2f%%), %llu interpreted (%llu at PC >= $80000), "
                "%llu translated entries\n",
                (unsigned long long)total, (unsigned long long)s.translated,
                total ? 100.0 * double(s.translated) / double(total) : 0.0, (unsigned long long)s.interpreted,
                (unsigned long long)s.interp_high, (unsigned long long)s.calls);
    std::vector<std::pair<uint32_t, uint32_t>> hot;
    for (uint32_t w = 0; w < s.interp_pc.size(); ++w)
        if (s.interp_pc[w]) hot.push_back({s.interp_pc[w], w * 2});
    std::sort(hot.begin(), hot.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (int i = 0; i < top && size_t(i) < hot.size(); ++i) {
        uint32_t pc = hot[size_t(i)].second;
        uint32_t s16 = g_slot.empty() ? 0 : g_slot[pc >> 1];
        std::printf("  interpreted %06X x%u (%.2f%%) op %02X%02X %s\n", pc, hot[size_t(i)].first,
                    total ? 100.0 * hot[size_t(i)].first / double(total) : 0.0, m.chip[pc], m.chip[pc + 1],
                    s16 ? "translated (code bytes differ from every translation)" : "no translation");
    }
}

} // namespace x2::recomp
