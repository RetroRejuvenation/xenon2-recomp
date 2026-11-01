// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The Atari ST release's side of the differential test (tests/recomp/difftest.cpp, `--game st`; docs/re-notes/atari-st-recomp.md):
// every translated function and random mid-function entries, from random states, on Musashi and on the hybrid executor, on a bare
// 512 KB bus. Compared: the CPU context (raw lazy flags included), all of RAM, the cycles consumed, the 4-cycle bus rounding of the ST
// (done in the instruction hook, as StMachine does) and every data bus access with its cycle stamp.
#include "bus_hooks.h"
#include "import/rnc.h"
#include "recomp/recomp.h"
#include "recomp/x2r_api.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "m68k.h"
#include "m68kcpu.h"
}

namespace {

constexpr uint32_t kRam = 512 * 1024;

struct Access {
    uint32_t addr, value, pc;
    int cycles;
    uint8_t size;
    char kind; // R, W, or I (an instruction boundary)
};

std::vector<uint8_t> g_ram;
std::vector<Access>* g_log = nullptr;
int g_last_run = 0;
uint64_t g_rounded = 0;

uint32_t open_bus(uint32_t a) { return (a * 0x9E3779B1u) >> 24; } // I/O and ROM space: a fixed pattern, writes ignored

void log_access(char kind, uint32_t a, uint32_t v, int size) {
    if (g_log) g_log->push_back(Access{a, v, uint32_t(REG_PC), m68k_cycles_run(), uint8_t(size), kind});
}

uint32_t rd8(uint32_t a) {
    a &= 0xFFFFFF;
    const uint32_t v = a < kRam ? g_ram[a] : open_bus(a);
    log_access('R', a, v, 1);
    return v;
}
uint32_t rd16(uint32_t a) {
    a &= 0xFFFFFF;
    const uint32_t v = a + 1 < kRam ? (uint32_t(g_ram[a]) << 8 | g_ram[a + 1]) : (open_bus(a) << 8 | open_bus(a + 1));
    log_access('R', a, v, 2);
    return v;
}
uint32_t rd32(uint32_t a) {
    a &= 0xFFFFFF;
    uint32_t v;
    if (a + 3 < kRam) v = uint32_t(g_ram[a]) << 24 | uint32_t(g_ram[a + 1]) << 16 | uint32_t(g_ram[a + 2]) << 8 | g_ram[a + 3];
    else v = open_bus(a) << 24 | open_bus(a + 1) << 16 | open_bus(a + 2) << 8 | open_bus(a + 3);
    log_access('R', a, v, 4);
    return v;
}
void wr8(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    log_access('W', a, v & 0xFF, 1);
    if (a < kRam) g_ram[a] = uint8_t(v);
}
void wr16(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    log_access('W', a, v & 0xFFFF, 2);
    if (a + 1 < kRam) { g_ram[a] = uint8_t(v >> 8); g_ram[a + 1] = uint8_t(v); }
}
void wr32(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    log_access('W', a, v, 4);
    if (a + 3 < kRam) for (int i = 0; i < 4; ++i) g_ram[a + uint32_t(i)] = uint8_t(v >> (24 - 8 * i));
}
uint32_t dasm8(uint32_t a) { a &= 0xFFFFFF; return a < kRam ? g_ram[a] : 0; }
uint32_t dasm16(uint32_t a) { return (dasm8(a) << 8) | dasm8(a + 1); }

// StMachine's rounding: the time between two instruction hooks is made a multiple of 4 cycles.
void insn(uint32_t pc) {
    if (g_log) g_log->push_back(Access{pc, 0, pc, m68k_cycles_run(), 0, 'I'});
    const int d = m68k_cycles_run() - g_last_run;
    if (d > 0) {
        const int pad = (-d) & 3;
        if (pad) {
            m68k_modify_timeslice(-pad);
            ++g_rounded;
        }
    }
    g_last_run = m68k_cycles_run();
}
uint32_t int_ack(int) { return 0xFFFFFFFFu; }

x2::BusHooks g_hooks;

std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Images {
    std::vector<uint8_t> engine;                       // at $400
    std::map<std::string, std::vector<uint8_t>> mods;  // frontend ($40000), level1..5 ($4F000)
};

constexpr uint32_t kEngineBase = 0x400, kFrontBase = 0x40000, kLevelBase = 0x4F000, kEngineFirstCylinder = 34;

bool load_images(const std::string& dir, Images& out, std::string& err) {
    std::map<std::string, std::string> kv;
    {
        std::ifstream mf(dir + "/manifest.txt");
        std::string line;
        while (std::getline(mf, line)) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    if (kv["build"] != "atari-st-kixx") { err = dir + " is not the Kixx import"; return false; }
    const std::vector<uint8_t> disk = read_file(dir + "/disk.bin");
    const size_t cyls = size_t(std::atoi(kv["cylinders"].c_str())), sides = size_t(std::atoi(kv["sides"].c_str()));
    const size_t track = size_t(std::atoi(kv["sectors_per_track"].c_str())) * size_t(std::atoi(kv["sector_bytes"].c_str()));
    if (!track || disk.size() != cyls * sides * track) { err = dir + "/disk.bin does not match its manifest"; return false; }
    std::vector<uint8_t> stream[2];
    for (size_t s = 0; s < 2; ++s)
        for (size_t c = 0; c < cyls; ++c)
            stream[s].insert(stream[s].end(), disk.begin() + long((c * sides + s) * track), disk.begin() + long((c * sides + s + 1) * track));
    out.engine.assign(stream[0].begin() + long(kEngineFirstCylinder * track), stream[0].end());
    for (int n = 0; n <= 5; ++n) {
        std::map<std::string, std::string> f;
        std::stringstream ss(kv["rnc." + std::to_string(n)]);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            const size_t eq = tok.find('=');
            if (eq != std::string::npos) f[tok.substr(0, eq)] = tok.substr(eq + 1);
        }
        const auto& s = stream[std::atoi(f["side"].c_str()) & 1];
        std::vector<uint8_t> data;
        x2::import::RncHeader h;
        std::string why;
        if (!x2::import::rnc_unpack(s.data(), s.size(), size_t(std::atoll(f["side_offset"].c_str())), data, h, why)) {
            err = "rnc." + std::to_string(n) + ": " + why;
            return false;
        }
        out.mods[n == 0 ? "frontend" : "level" + std::to_string(n)] = std::move(data);
    }
    return true;
}

struct Snapshot {
    uint32_t dar[16], ppc, pc, sp[7], ir, t1, t0, s, m, x, n, nz, v, c, int_mask, int_level, stopped, sr;
    int cycles;
    uint64_t rounded;
    std::vector<uint8_t> ram;
    std::vector<Access> bus;
};

// Instruction fetches are reads of the word just advanced over (translated code has them as constants): dropped on both sides.
std::vector<Access> strip_fetches(const std::vector<Access>& in) {
    std::vector<Access> out;
    for (const auto& a : in)
        if (!(a.kind == 'R' && (a.size == 2 || a.size == 4) && (a.addr & 0xFFFFFF) == ((a.pc - a.size) & 0xFFFFFF))) out.push_back(a);
    return out;
}

struct Case {
    const x2r_module* mod;
    uint32_t pc;
    uint64_t seed;
};

Snapshot run_case(const Case& c, const Images& img, const x2r_module* mods, bool hybrid, uint64_t* translated) {
    std::mt19937_64 rng(c.seed);
    auto r32 = [&]() { return uint32_t(rng()); };
    auto pick = [&](int n) { return int(rng() % uint64_t(n)); };
    for (auto& b : g_ram) b = uint8_t(rng());
    std::memcpy(g_ram.data() + kEngineBase, img.engine.data(), std::min<size_t>(img.engine.size(), kRam - kEngineBase));
    auto it = img.mods.find(c.mod->name);
    if (it != img.mods.end()) std::memcpy(g_ram.data() + c.mod->base, it->second.data(), std::min<size_t>(it->second.size(), kRam - c.mod->base));
    const int data_mode = pick(3); // 0 = real image data, 1 = randomised non-code bytes, 2 = sparse random pokes
    if (data_mode) {
        std::vector<uint8_t> code(kRam, 0);
        for (const x2r_module* mm = mods; mm->name; ++mm) {
            if (mm != c.mod && std::strcmp(mm->name, "game") != 0) continue;
            for (int i = 0; i < mm->count; ++i)
                for (uint32_t b = 0; b < mm->entries[i].len; ++b) code[(mm->entries[i].pc + b) % kRam] = 1;
        }
        if (data_mode == 1) {
            for (uint32_t a = 0; a < kRam; ++a)
                if (!code[a] && (rng() & 1)) g_ram[a] = uint8_t(rng());
        } else {
            for (int i = 0; i < 2000; ++i) {
                const uint32_t a = r32() % kRam;
                if (!code[a]) g_ram[a] = uint8_t(rng());
            }
        }
    }
    static const uint32_t kEdge[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
                                     0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFF80, 0xFFFF8000, 0x12345678};
    auto dval = [&]() -> uint32_t {
        switch (pick(4)) {
        case 0: return kEdge[pick(int(sizeof kEdge / sizeof kEdge[0]))];
        case 1: return r32() & 0xFF;
        case 2: return r32() & 0xFFFF;
        default: return r32();
        }
    };
    auto aval = [&]() -> uint32_t {
        const int k = pick(20);
        if (k < 13) return (r32() % kRam) & ~uint32_t(k == 0 ? 0 : 1);
        if (k < 14) return 0xFF8800 | (r32() & 0x3);          // the YM2149 registers
        if (k < 15) return 0xFFFA00 | (r32() & 0x3F);         // the MFP
        if (k < 16) return 0xFFFC00 | (r32() & 0x7);          // the ACIAs
        return dval();
    };
    const uint32_t ssp = (0x1000 + (r32() % (kRam - 0x2000))) & ~1u;
    const uint16_t sr = uint16_t((pick(10) ? 0x2000 : 0) | (uint32_t(pick(8)) << 8) | (r32() & 0x1F));
    const uint32_t pc = c.pc | (pick(10) == 0 ? 0x80000000u : 0);
    // Musashi fetches SSP/PC from $0/$4 on reset: provide them, then restore the RAM contents.
    uint8_t saved[8];
    std::memcpy(saved, g_ram.data(), 8);
    const uint8_t vec[8] = {uint8_t(ssp >> 24), uint8_t(ssp >> 16), uint8_t(ssp >> 8), uint8_t(ssp),
                            uint8_t(pc >> 24),  uint8_t(pc >> 16),  uint8_t(pc >> 8),  uint8_t(pc)};
    std::memcpy(g_ram.data(), vec, 8);
    g_log = nullptr;
    m68k_pulse_reset();
    std::memcpy(g_ram.data(), saved, 8);
    m68k_set_reg(M68K_REG_SR, 0x2700);
    m68k_set_reg(M68K_REG_ISP, ssp);
    m68k_set_reg(M68K_REG_A7, ssp);
    m68k_set_reg(M68K_REG_PC, pc);
    for (int i = 0; i < 7; ++i) REG_SP_BASE[i] = r32() & ~1u;
    for (int i = 0; i < 8; ++i) m68k_set_reg(m68k_register_t(M68K_REG_D0 + i), dval());
    for (int i = 0; i < 7; ++i) m68k_set_reg(m68k_register_t(M68K_REG_A0 + i), aval());
    m68k_set_reg(M68K_REG_USP, (r32() % kRam) & ~1u);
    m68k_set_reg(M68K_REG_SR, sr);
    if ((sr & 0x2000) == 0) m68k_set_reg(M68K_REG_ISP, ssp);
    m68k_set_irq(pick(3) == 0 ? 1 + pick(6) : 0); // a pending interrupt in some runs (taken wherever the mask allows)
    const int budget = 1 + pick(4) * 50 + pick(2500);
    std::vector<Access> bus;
    g_log = &bus;
    g_last_run = 0;
    g_rounded = 0;
    Snapshot s;
    const uint64_t before = x2::recomp::stats().translated;
    s.cycles = hybrid ? x2::recomp::execute(budget) : m68k_execute(budget);
    if (translated) *translated = x2::recomp::stats().translated - before;
    g_log = nullptr;
    m68k_set_irq(0);
    for (int i = 0; i < 16; ++i) s.dar[i] = REG_DA[i];
    s.ppc = REG_PPC;
    s.pc = REG_PC;
    for (int i = 0; i < 7; ++i) s.sp[i] = REG_SP_BASE[i];
    s.ir = REG_IR;
    s.t1 = FLAG_T1; s.t0 = FLAG_T0; s.s = FLAG_S; s.m = FLAG_M; s.x = FLAG_X; s.n = FLAG_N; s.nz = FLAG_Z;
    s.v = FLAG_V; s.c = FLAG_C; s.int_mask = FLAG_INT_MASK; s.int_level = CPU_INT_LEVEL; s.stopped = CPU_STOPPED;
    s.sr = m68k_get_reg(nullptr, M68K_REG_SR);
    s.rounded = g_rounded;
    s.ram = g_ram;
    s.bus = strip_fetches(bus);
    return s;
}

const char* kRegNames[16] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};

std::string compare(const Snapshot& a, const Snapshot& b) {
    char buf[256];
    for (int i = 0; i < 16; ++i)
        if (a.dar[i] != b.dar[i]) { std::snprintf(buf, sizeof buf, "%s %08X vs %08X", kRegNames[i], a.dar[i], b.dar[i]); return buf; }
    auto field = [&](const char* name, uint32_t x, uint32_t y) -> bool {
        if (x == y) return false;
        std::snprintf(buf, sizeof buf, "%s %08X vs %08X", name, x, y);
        return true;
    };
    if (field("PC", a.pc, b.pc) || field("PPC", a.ppc, b.ppc) || field("SR", a.sr, b.sr) || field("IR", a.ir, b.ir) ||
        field("FLAG_X", a.x, b.x) || field("FLAG_N", a.n, b.n) || field("FLAG_Z", a.nz, b.nz) || field("FLAG_V", a.v, b.v) ||
        field("FLAG_C", a.c, b.c) || field("FLAG_S", a.s, b.s) || field("FLAG_M", a.m, b.m) || field("FLAG_T1", a.t1, b.t1) ||
        field("FLAG_T0", a.t0, b.t0) || field("INT_MASK", a.int_mask, b.int_mask) || field("INT_LEVEL", a.int_level, b.int_level) ||
        field("STOPPED", a.stopped, b.stopped) || field("cycles", uint32_t(a.cycles), uint32_t(b.cycles)) ||
        field("rounded", uint32_t(a.rounded), uint32_t(b.rounded)))
        return buf;
    for (int i = 0; i < 7; ++i)
        if (a.sp[i] != b.sp[i]) { std::snprintf(buf, sizeof buf, "SP[%d] %08X vs %08X", i, a.sp[i], b.sp[i]); return buf; }
    for (size_t i = 0; i < a.ram.size(); ++i)
        if (a.ram[i] != b.ram[i]) { std::snprintf(buf, sizeof buf, "ram[$%05zX] %02X vs %02X", i, a.ram[i], b.ram[i]); return buf; }
    const size_t n = std::min(a.bus.size(), b.bus.size());
    for (size_t i = 0; i < n; ++i) {
        const auto &x = a.bus[i], &y = b.bus[i];
        if (x.addr != y.addr || x.value != y.value || x.size != y.size || x.kind != y.kind || x.cycles != y.cycles || x.pc != y.pc) {
            std::snprintf(buf, sizeof buf, "bus access %zu: %c%u $%06X=%X @%d vs %c%u $%06X=%X @%d", i, x.kind, x.size, x.addr, x.value,
                          x.cycles, y.kind, y.size, y.addr, y.value, y.cycles);
            return buf;
        }
    }
    if (a.bus.size() != b.bus.size()) { std::snprintf(buf, sizeof buf, "bus accesses %zu vs %zu", a.bus.size(), b.bus.size()); return buf; }
    return "";
}

} // namespace

int run_st_difftest(const std::string& assets, int trials, int mid, uint64_t seed, const std::string& module, long only, bool verbose) {
    Images img;
    std::string err;
    if (!load_images(assets, img, err)) {
        std::printf("difftest: SKIPPED, no Atari ST data (%s)\n", err.c_str());
        return 77;
    }
    if (!x2::recomp::translated_instructions(x2::recomp::Target::St)) {
        std::printf("difftest: SKIPPED, no translated code for the Atari ST release linked in\n");
        return 77;
    }
    const x2r_module* mods = x2r_modules_st;
    // The linked translation must have been generated from these images (it verifies its bytes at run time anyway, but a mismatch
    // would make the whole test meaningless).
    for (const x2r_module* m = mods; m->name; ++m) {
        std::vector<uint8_t> mem(kRam, 0);
        std::memcpy(mem.data() + kEngineBase, img.engine.data(), std::min<size_t>(img.engine.size(), kRam - kEngineBase));
        auto it = img.mods.find(m->name);
        if (it != img.mods.end()) std::memcpy(mem.data() + m->base, it->second.data(), std::min<size_t>(it->second.size(), kRam - m->base));
        for (int i = 0; i < m->count; ++i) {
            const x2r_entry& e = m->entries[i];
            if (((mem[e.pc] << 8) | mem[e.pc + 1]) != e.op) {
                std::printf("difftest: %s $%06X: translated opcode %04X does not match the game data\n", m->name, e.pc, e.op);
                return 1;
            }
        }
    }
    g_ram.assign(kRam, 0);
    g_hooks.read8 = rd8; g_hooks.read16 = rd16; g_hooks.read32 = rd32;
    g_hooks.write8 = wr8; g_hooks.write16 = wr16; g_hooks.write32 = wr32;
    g_hooks.dasm8 = dasm8; g_hooks.dasm16 = dasm16; g_hooks.insn = insn; g_hooks.int_ack = int_ack;
    x2::g_bus_hooks = &g_hooks;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    if (!x2::recomp::enable_raw(g_ram.data(), kRam, err, x2::recomp::Target::St)) {
        std::fprintf(stderr, "difftest: cannot enable the hybrid executor: %s\n", err.c_str());
        return 1;
    }
    std::mt19937_64 rng(seed);
    long cases = 0, failures = 0, functions = 0, exercised = 0;
    uint64_t translated_total = 0;
    for (const x2r_module* m = mods; m->name; ++m) {
        if (!module.empty() && module != m->name) continue;
        for (int fi = 0; fi < m->func_count; ++fi) {
            const x2r_func& f = m->funcs[fi];
            if (only >= 0 && long(f.entry) != only) continue;
            ++functions;
            std::vector<uint32_t> starts(size_t(trials), f.entry);
            std::vector<uint32_t> body;
            for (int i = 0; i < m->count; ++i)
                if (m->entries[i].fn == f.fn) body.push_back(m->entries[i].pc);
            for (int i = 0; i < mid && !body.empty(); ++i) starts.push_back(body[rng() % body.size()]);
            uint64_t fn_translated = 0;
            for (uint32_t pc : starts) {
                Case c{m, pc, rng()};
                ++cases;
                Snapshot a = run_case(c, img, mods, false, nullptr);
                uint64_t t = 0;
                Snapshot b = run_case(c, img, mods, true, &t);
                fn_translated += t;
                const std::string d = compare(a, b);
                if (!d.empty()) {
                    ++failures;
                    std::printf("DIFF %s $%06X (function $%06X) seed %llu: %s\n", m->name, pc, f.entry, (unsigned long long)c.seed, d.c_str());
                } else if (verbose) {
                    std::printf("ok   %s $%06X seed %llu: %d cycles, %zu bus accesses, %llu translated insns\n", m->name, pc,
                                (unsigned long long)c.seed, a.cycles, a.bus.size(), (unsigned long long)t);
                }
            }
            translated_total += fn_translated;
            if (fn_translated) ++exercised;
            else std::printf("note %s $%06X: translated code never ran in %zu trials\n", m->name, f.entry, starts.size());
        }
    }
    x2::g_bus_hooks = nullptr;
    std::printf("difftest: %ld functions, %ld cases, %ld differences; %ld functions ran translated code, %llu translated instructions "
                "executed\n",
                functions, cases, failures, exercised, (unsigned long long)translated_total);
    return failures ? 1 : 0;
}
