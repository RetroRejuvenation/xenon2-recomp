// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Differential test: every translated function (and random mid-function entry points) is run from many random
// initial states on the Musashi interpreter and on the hybrid executor, and everything observable is compared:
// the CPU context (Musashi's own variables, raw lazy flags included), all of chip RAM, the custom chip and CIA
// state, the custom/CIA register-write log (frame/line/hpos stamped), the cycles consumed, the instruction-hook
// sequence and the ordered list of data bus accesses (address, size, value, R/W, cycle stamp).
//
//   x2_recomp_difftest [--game disk|cdtv|st] [--assets DIR] [--trials N] [--mid N] [--seed S] [--module NAME] [--only PC] [--verbose]
//
// --game cdtv tests the CDTV release's translation (docs/re-notes/cdtv-recomp.md): a 1 MB machine, X2 at $400 and each overlay
// (cd0/LEVEL1-5, INTRO, PICCODE) relocated into the overlay area at $4E26 the way X2's own relocator does.
//
// --game st tests the Atari ST release's translation (difftest_st.cpp, docs/re-notes/atari-st-recomp.md): a bare 512 KB bus with the
// ST's 4-cycle instruction rounding, the engine at $400, the front end at $40000 and each level module at $4F000 (RNC-unpacked from
// the import, --assets generated/assets-st).
//
// Exit codes: 0 all equal, 1 a difference (details printed), 77 game data missing (reported as Skipped).
#include "boot.h"
#include "disk_shim.h"
#include "machine.h"
#include "recomp/recomp.h"
#include "recomp/x2r_api.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "m68kcpu.h"
}

int run_st_difftest(const std::string& assets, int trials, int mid, uint64_t seed, const std::string& module, long only, bool verbose);

namespace {

struct Options {
    bool cdtv = false;
    bool st = false;
    bool assets_given = false;
    std::string assets = "generated/assets";
    int trials = 12;   // per function entry
    int mid = 3;       // random mid-function entries per function, one trial each
    uint64_t seed = 0x5EED;
    std::string module;
    long only = -1;
    bool verbose = false;
};

struct Snapshot {
    uint32_t dar[16], ppc, pc, sp[7], ir, t1, t0, s, m, x, n, nz, v, c, int_mask, int_level, stopped, sr;
    int cycles;
    std::vector<uint8_t> chip;
    std::array<uint16_t, 0x100> regs;
    uint16_t dmacon, intena, intreq, adkcon;
    uint8_t cia[2][9];
    std::vector<x2::RegWrite> writes;
    std::vector<x2::BusAccess> bus; // data accesses only, with 'I' markers for instruction boundaries
    uint32_t restarts, blits, unmapped;
};

std::vector<uint8_t> read_file(const std::string& p) {
    std::vector<uint8_t> d;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
        std::fclose(f);
    }
    return d;
}

// The CDTV release's images (tools/recomp/recomp.py --build cdtv): the pre-linked X2 and the overlays as X2 leaves them in the
// overlay area ($4E26, 180,000 bytes): the file as read, with $406 added to every longword its GEMDOS-style table lists.
constexpr uint32_t kCdtvOverlayBase = 0x4E26, kCdtvOverlaySize = 180000, kCdtvRelocAdd = 0x406;

uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

bool relocate_overlay(std::vector<uint8_t>& d) {
    if (d.size() < 0x1C + 4 || d[0] != 0x60 || d[1] != 0x1A) return false;
    const size_t tlen = be32(d, 2), text0 = 0x1C;
    if (tlen > d.size() - text0 || text0 + tlen + 4 > d.size()) return false;
    size_t pos = be32(d, text0 + tlen), p = text0 + tlen + 4;
    for (;;) {
        if (pos + 4 > tlen) return false;
        const uint32_t v = be32(d, text0 + pos) + kCdtvRelocAdd;
        for (int i = 0; i < 4; ++i) d[text0 + pos + size_t(i)] = uint8_t(v >> (24 - 8 * i));
        uint8_t b;
        do {
            if (p >= d.size()) return false;
            b = d[p++];
            if (b == 1) pos += 254;
        } while (b == 1);
        if (b == 0) return true;
        pos += b;
    }
}

struct CdtvImages {
    std::vector<uint8_t> game;
    std::map<std::string, std::vector<uint8_t>> overlays; // module name (lower case) -> relocated bytes
};

bool load_cdtv_images(const std::string& dir, CdtvImages& out, std::string& err) {
    out.game = read_file(dir + "/game_0400.bin");
    if (out.game.empty()) { err = dir + "/game_0400.bin is missing"; return false; }
    for (const char* n : {"LEVEL1", "LEVEL2", "LEVEL3", "LEVEL4", "LEVEL5", "INTRO", "PICCODE"}) {
        std::vector<uint8_t> d = read_file(dir + "/cd0/" + n);
        if (d.empty()) continue;
        if (!relocate_overlay(d)) { err = std::string("cd0/") + n + " is not a relocatable overlay"; return false; }
        if (d.size() > kCdtvOverlaySize) d.resize(kCdtvOverlaySize);
        std::string lower;
        for (const char* c = n; *c; ++c) lower += char(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c);
        out.overlays[lower] = std::move(d);
    }
    return true;
}

// Instruction-stream reads appear on the interpreter's bus (Musashi fetches opcode and extension words through
// m68k_read_memory_16/32) but not in translated code, where they are constants. Musashi advances REG_PC before
// each such read, so a fetch is a 16/32-bit read at REG_PC - size; those are dropped from both logs by the same
// rule. (A data read of exactly the word just fetched would be dropped too, on both sides alike; its value still
// reaches the compared registers or memory.)
std::vector<x2::BusAccess> strip_fetches(const std::vector<x2::BusAccess>& in) {
    std::vector<x2::BusAccess> out;
    for (const auto& a : in)
        if (!(a.kind == 'R' && (a.size == 2 || a.size == 4) && (a.addr & 0xFFFFFF) == ((a.pc - a.size) & 0xFFFFFF)))
            out.push_back(a);
    return out;
}

const char* kRegNames[16] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
                             "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};

struct Case {
    const x2r_module* mod;
    uint32_t pc;
    uint64_t seed;
};

class Runner {
public:
    Runner(const Options& o, const x2::GameAssets& a, const CdtvImages& c, const x2r_module* mods)
        : opt_(o), assets_(a), cdtv_(c), mods_(mods) {}

    // Builds the initial machine state for `c` deterministically from its seed and runs it.
    Snapshot run(const Case& c, bool hybrid, uint64_t* translated) {
        std::mt19937_64 rng(c.seed);
        auto r32 = [&]() { return uint32_t(rng()); };
        auto pick = [&](int n) { return int(rng() % uint64_t(n)); };
        auto m = std::make_unique<x2::Machine>(opt_.cdtv ? x2::kChipRamMax : x2::kChipRamSize);
        const uint32_t chip_size = m->chip_size;
        x2::DiskShim disk(assets_.disc2);
        if (!opt_.cdtv) disk.install(*m);
        // Chip RAM: random, then the game image and (for level code) the module, then optionally random bytes
        // over the images' non-instruction bytes (data tables, variables).
        for (auto& b : m->chip) b = uint8_t(rng());
        if (opt_.cdtv) {
            m->load(x2::kGameBase, cdtv_.game.data(), cdtv_.game.size());
            auto ov = cdtv_.overlays.find(c.mod->name);
            if (ov != cdtv_.overlays.end()) m->load(c.mod->base, ov->second.data(), ov->second.size());
        } else {
            m->load(x2::kGameBase, assets_.game_image.data(), assets_.game_image.size());
            if (c.mod->sectors) {
                size_t off = size_t(c.mod->disc_sector) * 512, len = size_t(c.mod->sectors) * 512;
                len = std::min(len, size_t(x2::kChipRamSize - c.mod->base));
                m->load(c.mod->base, assets_.disc2.data() + off, len);
            }
        }
        int data_mode = pick(3); // 0 = real image data, 1 = randomised non-code bytes, 2 = sparse random pokes
        if (data_mode) {
            std::vector<uint8_t> code(chip_size, 0);
            for (const x2r_module* mm = mods_; mm->name; ++mm) {
                if (mm != c.mod && std::strcmp(mm->name, "game") != 0) continue;
                for (int i = 0; i < mm->count; ++i)
                    for (uint32_t b = 0; b < mm->entries[i].len; ++b) code[(mm->entries[i].pc + b) & m->chip_mask] = 1;
            }
            if (data_mode == 1) {
                for (uint32_t a = 0; a < chip_size; ++a)
                    if (!code[a] && (rng() & 1)) m->chip[a] = uint8_t(rng());
            } else {
                for (int i = 0; i < 2000; ++i) {
                    uint32_t a = r32() % chip_size;
                    if (!code[a]) m->chip[a] = uint8_t(rng());
                }
            }
        }
        // Boundary-heavy operand values.
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
            int k = pick(20);
            if (k < 13) return (r32() % chip_size) & ~uint32_t(k == 0 ? 0 : 1);
            if (k < 15) return 0xDFF000 | (r32() & 0x1FE);
            if (k < 16) return 0xBFD000 | (r32() & 0x1F01);
            return dval();
        };
        uint32_t ssp = 0x1000 + (r32() % (chip_size - 0x2000)) & ~1u;
        uint16_t sr = uint16_t((pick(10) ? 0x2000 : 0) | (uint32_t(pick(8)) << 8) | (r32() & 0x1F));
        uint32_t pc = c.pc | (pick(10) == 0 ? 0x80000000u : 0); // exercise flag bits in the PC high byte
        m->start_cpu(pc, ssp, 0x2700);
        // Musashi keeps the inactive stack pointers in sp[]; reset leaves them from the previous run, so give them
        // deterministic values (they are part of the compared state).
        for (int i = 0; i < 7; ++i) REG_SP_BASE[i] = r32() & ~1u;
        for (int i = 0; i < 8; ++i) m->set_dreg(i, dval());
        for (int i = 0; i < 7; ++i) m->set_areg(i, aval());
        m68k_set_reg(M68K_REG_USP, (r32() % chip_size) & ~1u);
        m->set_sr(sr);
        if ((sr & 0x2000) == 0) m68k_set_reg(M68K_REG_ISP, ssp);
        // Interrupts pending in some runs: they are taken at slice entry and wherever the code rewrites SR.
        if (pick(3) == 0) {
            m->custom_write(x2::reg::INTENA, uint16_t(0xC000 | (r32() & 0x3FFF)));
            m->custom_write(x2::reg::INTREQ, uint16_t(0x8000 | (r32() & 0x3FFF)));
        }
        m->log_writes = true;
        int budget = 1 + pick(4) * 50 + pick(2500);
        std::vector<x2::BusAccess> bus;
        m->trace = [&](x2::Machine&, uint32_t ipc) { bus.push_back(x2::BusAccess{ipc, 0, m68k_cycles_run(), ipc, 0, 'I'}); };
        m->bus_log = &bus;
        std::string err;
        uint64_t before = x2::recomp::stats().translated;
        if (hybrid && !x2::recomp::enable(*m, err, opt_.cdtv ? x2::recomp::Target::Cdtv : x2::recomp::Target::Disk)) {
            std::fprintf(stderr, "difftest: cannot enable the hybrid executor: %s\n", err.c_str());
            std::exit(1);
        }
        Snapshot s;
        s.cycles = m->run_cycles(budget);
        if (translated) *translated = x2::recomp::stats().translated - before;
        m->bus_log = nullptr;
        m->trace = nullptr;
        for (int i = 0; i < 16; ++i) s.dar[i] = REG_DA[i];
        s.ppc = REG_PPC;
        s.pc = REG_PC;
        for (int i = 0; i < 7; ++i) s.sp[i] = REG_SP_BASE[i];
        s.ir = REG_IR;
        s.t1 = FLAG_T1; s.t0 = FLAG_T0; s.s = FLAG_S; s.m = FLAG_M; s.x = FLAG_X; s.n = FLAG_N; s.nz = FLAG_Z;
        s.v = FLAG_V; s.c = FLAG_C; s.int_mask = FLAG_INT_MASK; s.int_level = CPU_INT_LEVEL; s.stopped = CPU_STOPPED;
        s.sr = m68k_get_reg(nullptr, M68K_REG_SR);
        s.chip = m->chip;
        s.regs = m->regs;
        s.dmacon = m->dmacon; s.intena = m->intena; s.intreq = m->intreq; s.adkcon = m->adkcon;
        x2::Cia* cias[2] = {&m->cia_a, &m->cia_b};
        for (int i = 0; i < 2; ++i) {
            x2::Cia& ci = *cias[i];
            uint8_t v[9] = {ci.cra(), ci.crb(), uint8_t(ci.timer_a()), uint8_t(ci.timer_a() >> 8), uint8_t(ci.timer_b()),
                            uint8_t(ci.timer_b() >> 8), ci.pra_out(), ci.prb_out(), uint8_t(ci.irq())};
            std::memcpy(s.cia[i], v, 9);
        }
        s.writes = m->frame_writes;
        s.bus = strip_fetches(bus);
        s.restarts = m->restarts;
        s.blits = m->blits;
        s.unmapped = m->unmapped_accesses;
        return s;
    }

private:
    const Options& opt_;
    const x2::GameAssets& assets_;
    const CdtvImages& cdtv_;
    const x2r_module* mods_;
};

// Returns an empty string when equal, else a description of the first difference.
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
        field("FLAG_X", a.x, b.x) || field("FLAG_N", a.n, b.n) || field("FLAG_Z", a.nz, b.nz) ||
        field("FLAG_V", a.v, b.v) || field("FLAG_C", a.c, b.c) || field("FLAG_S", a.s, b.s) || field("FLAG_M", a.m, b.m) ||
        field("FLAG_T1", a.t1, b.t1) || field("FLAG_T0", a.t0, b.t0) || field("INT_MASK", a.int_mask, b.int_mask) ||
        field("INT_LEVEL", a.int_level, b.int_level) || field("STOPPED", a.stopped, b.stopped) ||
        field("cycles", uint32_t(a.cycles), uint32_t(b.cycles)) || field("DMACON", a.dmacon, b.dmacon) ||
        field("INTENA", a.intena, b.intena) || field("INTREQ", a.intreq, b.intreq) || field("ADKCON", a.adkcon, b.adkcon) ||
        field("restarts", a.restarts, b.restarts) || field("blits", a.blits, b.blits) || field("unmapped", a.unmapped, b.unmapped))
        return buf;
    for (int i = 0; i < 7; ++i)
        if (a.sp[i] != b.sp[i]) { std::snprintf(buf, sizeof buf, "SP[%d] %08X vs %08X", i, a.sp[i], b.sp[i]); return buf; }
    for (size_t i = 0; i < a.chip.size(); ++i)
        if (a.chip[i] != b.chip[i]) { std::snprintf(buf, sizeof buf, "chip[$%05zX] %02X vs %02X", i, a.chip[i], b.chip[i]); return buf; }
    for (size_t i = 0; i < a.regs.size(); ++i)
        if (a.regs[i] != b.regs[i]) { std::snprintf(buf, sizeof buf, "custom reg $%03zX %04X vs %04X", i * 2, a.regs[i], b.regs[i]); return buf; }
    if (std::memcmp(a.cia, b.cia, sizeof a.cia)) return "CIA state differs";
    if (a.writes.size() != b.writes.size()) { std::snprintf(buf, sizeof buf, "register writes %zu vs %zu", a.writes.size(), b.writes.size()); return buf; }
    for (size_t i = 0; i < a.writes.size(); ++i) {
        const auto &x = a.writes[i], &y = b.writes[i];
        if (x.line != y.line || x.hpos != y.hpos || x.pc != y.pc || x.reg != y.reg || x.value != y.value || x.source != y.source) {
            std::snprintf(buf, sizeof buf, "register write %zu: reg %04X=%04X pc %06X h %u vs reg %04X=%04X pc %06X h %u", i,
                          x.reg, x.value, x.pc, x.hpos, y.reg, y.value, y.pc, y.hpos);
            return buf;
        }
    }
    size_t n = std::min(a.bus.size(), b.bus.size());
    for (size_t i = 0; i < n; ++i) {
        const auto &x = a.bus[i], &y = b.bus[i];
        if (x.addr != y.addr || x.value != y.value || x.size != y.size || x.kind != y.kind || x.cycles != y.cycles || x.pc != y.pc) {
            std::snprintf(buf, sizeof buf, "bus access %zu: %c%u $%06X=%X @%d vs %c%u $%06X=%X @%d", i, x.kind, x.size, x.addr,
                          x.value, x.cycles, y.kind, y.size, y.addr, y.value, y.cycles);
            return buf;
        }
    }
    if (a.bus.size() != b.bus.size()) { std::snprintf(buf, sizeof buf, "bus accesses %zu vs %zu", a.bus.size(), b.bus.size()); return buf; }
    return "";
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--game" && (v = next())) {
            if (std::string(v) != "disk" && std::string(v) != "cdtv" && std::string(v) != "st") { std::fprintf(stderr, "difftest: --game is disk, cdtv or st\n"); return 2; }
            o.cdtv = std::string(v) == "cdtv";
            o.st = std::string(v) == "st";
        }
        else if (a == "--assets" && (v = next())) { o.assets = v; o.assets_given = true; }
        else if (a == "--trials" && (v = next())) o.trials = std::atoi(v);
        else if (a == "--mid" && (v = next())) o.mid = std::atoi(v);
        else if (a == "--seed" && (v = next())) o.seed = std::strtoull(v, nullptr, 0);
        else if (a == "--module" && (v = next())) o.module = v;
        else if (a == "--only" && (v = next())) o.only = std::strtol(v, nullptr, 16);
        else if (a == "--verbose") o.verbose = true;
        else { std::fprintf(stderr, "usage: x2_recomp_difftest [--game disk|cdtv|st] [--assets DIR] [--trials N] [--mid N] [--seed S] [--module NAME] [--only HEXPC] [--verbose]\n"); return 2; }
    }
    if (o.st) return run_st_difftest(o.assets_given ? o.assets : "generated/assets-st", o.trials, o.mid, o.seed, o.module, o.only, o.verbose);
    x2::GameAssets assets;
    CdtvImages cdtv;
    std::string err;
    const x2r_module* mods = o.cdtv ? x2r_modules_cdtv : x2r_modules;
    const auto target = o.cdtv ? x2::recomp::Target::Cdtv : x2::recomp::Target::Disk;
    if (o.cdtv ? !load_cdtv_images(o.assets, cdtv, err) : !x2::load_game_assets(o.assets, assets, err)) {
        std::printf("difftest: SKIPPED, no game data (%s)\n", err.c_str());
        return 77;
    }
    if (!x2::recomp::translated_instructions(target)) {
        // A build that carries only the other game's translation has nothing to test here.
        std::printf("difftest: SKIPPED, no translated code for the %s release linked in\n", o.cdtv ? "CDTV" : "disk");
        return 77;
    }
    // Check that the linked translation was generated from these images (it verifies its bytes at run time
    // anyway, but a mismatch here would make the whole test meaningless).
    {
        const uint32_t size = o.cdtv ? x2::kChipRamMax : x2::kChipRamSize;
        std::vector<uint8_t> img(size, 0);
        const std::vector<uint8_t>& game = o.cdtv ? cdtv.game : assets.game_image;
        std::copy(game.begin(), game.end(), img.begin() + x2::kGameBase);
        for (const x2r_module* m = mods; m->name; ++m) {
            std::vector<uint8_t> mem = img;
            if (o.cdtv) {
                auto ov = cdtv.overlays.find(m->name);
                if (ov != cdtv.overlays.end()) std::copy(ov->second.begin(), ov->second.end(), mem.begin() + m->base);
            } else if (m->sectors) {
                size_t len = std::min(size_t(m->sectors) * 512, size_t(x2::kChipRamSize - m->base));
                std::copy_n(assets.disc2.begin() + long(m->disc_sector) * 512, len, mem.begin() + m->base);
            }
            for (int i = 0; i < m->count; ++i) {
                const x2r_entry& e = m->entries[i];
                if (((mem[e.pc] << 8) | mem[e.pc + 1]) != e.op) {
                    std::printf("difftest: %s $%06X: translated opcode %04X does not match the game data\n", m->name, e.pc, e.op);
                    return 1;
                }
            }
        }
    }

    Runner runner(o, assets, cdtv, mods);
    std::mt19937_64 rng(o.seed);
    long cases = 0, failures = 0, functions = 0, exercised = 0;
    uint64_t translated_total = 0;
    for (const x2r_module* m = mods; m->name; ++m) {
        if (!o.module.empty() && o.module != m->name) continue;
        for (int fi = 0; fi < m->func_count; ++fi) {
            const x2r_func& f = m->funcs[fi];
            if (o.only >= 0 && long(f.entry) != o.only) continue;
            ++functions;
            std::vector<uint32_t> starts(size_t(o.trials), f.entry);
            std::vector<uint32_t> body;
            for (int i = 0; i < m->count; ++i)
                if (m->entries[i].fn == f.fn) body.push_back(m->entries[i].pc);
            for (int i = 0; i < o.mid && !body.empty(); ++i) starts.push_back(body[rng() % body.size()]);
            uint64_t fn_translated = 0;
            bool failed = false;
            for (uint32_t pc : starts) {
                Case c{m, pc, rng()};
                ++cases;
                Snapshot a = runner.run(c, false, nullptr);
                uint64_t t = 0;
                Snapshot b = runner.run(c, true, &t);
                fn_translated += t;
                std::string d = compare(a, b);
                if (!d.empty()) {
                    ++failures;
                    failed = true;
                    std::printf("DIFF %s $%06X (function $%06X) seed %llu: %s\n", m->name, pc, f.entry,
                                (unsigned long long)c.seed, d.c_str());
                } else if (o.verbose) {
                    std::printf("ok   %s $%06X seed %llu: %d cycles, %zu bus accesses, %llu translated insns\n", m->name, pc,
                                (unsigned long long)c.seed, a.cycles, a.bus.size(), (unsigned long long)t);
                }
            }
            translated_total += fn_translated;
            if (fn_translated) ++exercised;
            else std::printf("note %s $%06X: translated code never ran in %zu trials\n", m->name, f.entry, starts.size());
            (void)failed;
        }
    }
    std::printf("difftest: %ld functions, %ld cases, %ld differences; %ld functions ran translated code, %llu translated "
                "instructions executed\n",
                functions, cases, failures, exercised, (unsigned long long)translated_total);
    return failures ? 1 : 0;
}
