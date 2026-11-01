// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "trace/tracer.h"

#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <tuple>

extern "C" {
#include "m68k.h"
}

namespace x2 {

namespace {

enum OpClass : uint8_t { kNormal, kBcc, kBra, kBsr, kDbcc, kJmpInd, kJmpDir, kJsrInd, kJsrDir, kRts, kRtr, kRte };

uint8_t classify(uint16_t op) {
    if ((op & 0xF000) == 0x6000) {
        int c = (op >> 8) & 0xF;
        return c == 0 ? kBra : c == 1 ? kBsr : kBcc;
    }
    if ((op & 0xF0F8) == 0x50C8) return kDbcc;
    if (op == 0x4E75) return kRts;
    if (op == 0x4E77) return kRtr;
    if (op == 0x4E73) return kRte;
    bool jmp = (op & 0xFFC0) == 0x4EC0, jsr = (op & 0xFFC0) == 0x4E80;
    if (jmp || jsr) {
        int mode = (op >> 3) & 7, reg = op & 7;
        // (An), d16(An), d8(An,Xn), d8(PC,Xn): the target depends on register contents.
        bool ind = mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg == 3);
        // abs.w, abs.l, d16(PC): the target is fixed by the instruction.
        bool dir = mode == 7 && reg <= 2;
        if (ind) return jmp ? kJmpInd : kJsrInd;
        if (dir) return jmp ? kJmpDir : kJsrDir;
    }
    return kNormal;
}

uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

} // namespace

struct Tracer::Ctx {
    std::string name;
    uint64_t key = 0;
    uint32_t base = 0, size = 0;
    std::vector<Load> loads;
    std::vector<uint32_t> count, taken, not_taken, btarget; // per 16-bit word
    std::vector<uint16_t> op;
    std::vector<uint8_t> len, flags;
    std::vector<uint8_t> code_byte; // per byte: 1 = part of an executed instruction
    Ctx(std::string n, uint64_t k, uint32_t b, uint32_t s)
        : name(std::move(n)), key(k), base(b), size(s), count(s / 2), taken(s / 2), not_taken(s / 2), btarget(s / 2),
          op(s / 2), len(s / 2), flags(s / 2), code_byte(s) {}
};

namespace {
Tracer::Env env_of(Machine& m) {
    Tracer::Env e;
    e.mem = m.chip.data();
    e.size = m.chip_size;
    e.frame = &m.frame;
    e.sp = [&m] { return m.areg(7); };
    e.sr = [&m] { return m.sr(); };
    for (int lvl = 1; lvl <= 6; ++lvl) e.irq_vectors.push_back({lvl, 0x60 + 4 * uint32_t(lvl)}); // the autovectors
    return e;
}
} // namespace

Tracer::Tracer(Machine& m, Layout layout) : Tracer(env_of(m), layout) {}

Tracer::Tracer(Env env, Layout layout)
    : env_(std::move(env)), mask_(env_.size - 1), layout_(layout), op_class_(0x10000), shadow_(env_.size / 2) {
    for (uint32_t op = 0; op < 0x10000; ++op) op_class_[op] = classify(uint16_t(op));
    ctxs_.push_back(std::make_unique<Ctx>("game", 0, 0, layout_.game_end));
}

Tracer::~Tracer() = default;

uint32_t Tracer::rd32(uint32_t a) const { return (uint32_t(rd16(a)) << 16) | rd16(a + 2); }
uint16_t Tracer::rd16(uint32_t a) const {
    a &= mask_;
    return uint16_t((env_.mem[a] << 8) | env_.mem[(a + 1) & mask_]);
}

// Low RAM: the engine ("game"). On the ST, before the engine has started it is the stage-1 loader instead, a context of its own
// so that the loader's code is not taken for engine code at the same addresses.
Tracer::Ctx* Tracer::low_ctx() {
    if (layout_.st && !st_engine_) {
        if (st_stage1_ < 0) {
            ctxs_.push_back(std::make_unique<Ctx>("stage1", 0, 0, layout_.game_end));
            st_stage1_ = int(ctxs_.size()) - 1;
        }
        return ctxs_[size_t(st_stage1_)].get();
    }
    return ctxs_[0].get();
}

Tracer::Ctx* Tracer::overlay_ctx() {
    if (overlay_ < 0) {
        ctxs_.push_back(std::make_unique<Ctx>("overlay-unloaded", 0, layout_.overlay_base,
                                              layout_.overlay_end - layout_.overlay_base));
        overlay_ = int(ctxs_.size()) - 1;
    }
    return ctxs_[size_t(overlay_)].get();
}

Tracer::Ctx* Tracer::ctx_for(uint32_t addr, uint32_t& off) {
    addr &= 0xFFFFFF;
    if (addr >= layout_.overlay_base && addr < layout_.overlay_end) { off = addr - layout_.overlay_base; return overlay_ctx(); }
    if (addr < layout_.game_end) { off = addr; return low_ctx(); }
    return nullptr;
}

int Tracer::ctx_index(const Ctx* c) const {
    for (size_t i = 0; i < ctxs_.size(); ++i)
        if (ctxs_[i].get() == c) return int(i);
    return -1;
}

void Tracer::overlay_loaded(const std::string& name, uint64_t key, const Load& ld) {
    overlay_ = -1;
    for (size_t i = 1; i < ctxs_.size(); ++i)
        if (ctxs_[i]->key == key && ctxs_[i]->name == name) overlay_ = int(i);
    if (overlay_ < 0) {
        ctxs_.push_back(std::make_unique<Ctx>(name, key, layout_.overlay_base, layout_.overlay_end - layout_.overlay_base));
        overlay_ = int(ctxs_.size()) - 1;
    }
    ctxs_[size_t(overlay_)]->loads.push_back(ld);
}

void Tracer::on_disk_load(uint32_t dest, uint16_t sector, uint16_t count, const uint8_t* data) {
    dest &= 0xFFFFFF;
    Load ld{*env_.frame, sector, count, dest, fnv1a64(data, size_t(count) * 512)};
    if (dest == layout_.overlay_base) {
        // A new module: identify it by the content of its first 8 KB (a module reloaded with fewer sectors after
        // the shop keeps its identity), and name it from the game's level table when the start sector matches.
        uint64_t key = fnv1a64(data, std::min<size_t>(size_t(count) * 512, 8192));
        std::string name;
        for (int i = 0; i < 6; ++i)
            if (rd16(kLevelTable + 4 * uint32_t(i)) == sector) name = "level" + std::to_string(i + 1);
        if (name.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "mod_%016llx", (unsigned long long)key);
            name = buf;
        }
        overlay_loaded(name, key, ld);
    } else if (dest > layout_.overlay_base && dest < layout_.overlay_end) {
        overlay_ctx()->loads.push_back(ld);
    }
}

void Tracer::on_file_load(uint32_t dest, const std::string& name, const uint8_t* data, size_t len) {
    dest &= 0xFFFFFF;
    // The CDTV program reads each overlay in pieces (dos Read of up to the whole area); the module is identified by the
    // file name and the content of the first 8 KB of what the first read at the window's base delivered.
    Load ld{*env_.frame, 0, uint16_t(std::min<size_t>((len + 511) / 512, 0xFFFF)), dest, fnv1a64(data, len)};
    if (dest == layout_.overlay_base) {
        std::string lower;
        for (char ch : name) lower += char(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        overlay_loaded(lower, fnv1a64(data, std::min<size_t>(len, 8192)), ld);
    } else if (dest > layout_.overlay_base && dest < layout_.overlay_end) {
        overlay_ctx()->loads.push_back(ld);
    }
}

// The ST has no loader to hook (see Layout): the module in the window follows from the execution phase and the content.
void Tracer::st_phase(uint32_t pc) {
    if (!st_engine_ && pc == layout_.st_engine_entry) {
        st_engine_ = true;
        st_dirty_ = true; // the first level module is taken when it is first entered
        overlay_ = -1;
        return;
    }
    if (pc < layout_.overlay_base || pc >= layout_.overlay_end) return;
    constexpr uint32_t kKeyBytes = 2048, kKeyHeader = 16;
    if (!st_engine_) {
        if (pc >= layout_.st_boot_base && pc < layout_.st_boot_base + 512) return; // the boot sector: "overlay-unloaded"
        if (overlay_ < 0 || ctxs_[size_t(overlay_)]->name != "frontend") {
            const uint64_t key = fnv1a64(env_.mem + layout_.overlay_base, kKeyBytes);
            overlay_loaded("frontend", key, Load{*env_.frame, 0, 0, layout_.overlay_base, key});
        }
        return;
    }
    if (pc >= layout_.st_level_base && (st_dirty_ || overlay_ < 0)) {
        // From byte 16 on: the program patches pointers in the module's header after unpacking it (offset 5..7), so the first
        // bytes do not match the unpacked file; the code and tables behind them do (tools/recomp/recomp.py hashes the same range).
        const uint64_t key = fnv1a64(env_.mem + layout_.st_level_base + kKeyHeader, kKeyBytes);
        char name[32];
        std::snprintf(name, sizeof name, "mod_%016llx", (unsigned long long)key);
        overlay_loaded(name, key, Load{*env_.frame, 0, 0, layout_.st_level_base, key});
        st_dirty_ = false;
    }
}

void Tracer::on_instruction(uint32_t pc) {
    if (layout_.st) st_phase(pc & 0xFFFFFF);
    uint32_t sp = env_.sp();
    // Interrupt entry: the PC is the vector target of an interrupt level, the CPU's mask is that level and the stacked SR had
    // a lower mask. The previous instruction's successor is then the stacked PC, not this one.
    int irq = 0;
    for (const auto& [lvl, va] : env_.irq_vectors) {
        if (pc != (rd32(va) & 0xFFFFFF)) continue;
        if (((env_.sr() >> 8) & 7) == uint32_t(lvl) && ((rd16(sp) >> 8) & 7) < uint32_t(lvl)) { irq = lvl; break; }
    }
    if (have_prev_) {
        if (irq) process_prev(rd32(sp + 2) & 0xFFFFFF, sp + 6);
        else process_prev(pc, sp);
    }
    if (irq) {
        uint32_t off;
        Ctx* c = ctx_for(pc, off);
        if (c) {
            c->flags[off >> 1] |= kInterruptEntry;
            ++interrupts_[{irq, ctx_index(c), pc}];
        }
    }
    record_exec(pc);
}

void Tracer::record_exec(uint32_t pc) {
    uint32_t off;
    Ctx* c = ctx_for(pc, off);
    uint16_t op = rd16(pc);
    if (!c || (pc & 1)) {
        ++outside_[pc];
        have_prev_ = false;
        return;
    }
    uint32_t w = off >> 1;
    if (c->count[w]++ == 0) {
        char buf[128];
        unsigned n = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        c->len[w] = uint8_t(n);
        c->op[w] = op;
        for (unsigned i = 0; i < n && off + i < c->size; ++i) c->code_byte[off + i] = 1;
    } else if (c->op[w] != op) {
        code_changes_[{ctx_index(c), pc}].push_back({*env_.frame, c->op[w], op});
        c->op[w] = op;
    }
    prev_pc_ = pc;
    prev_ctx_ = ctx_index(c);
    prev_class_ = op_class_[op];
    have_prev_ = true;
}

void Tracer::process_prev(uint32_t next, uint32_t sp) {
    if (prev_class_ == kNormal || prev_class_ == kRte) return;
    Ctx* c = ctxs_[size_t(prev_ctx_)].get();
    uint32_t w = (prev_pc_ - c->base) >> 1;
    auto flag = [&](uint32_t pc, uint8_t f) {
        uint32_t off;
        if (Ctx* t = ctx_for(pc, off)) t->flags[off >> 1] |= f;
    };
    auto branch = [&](uint32_t target, uint32_t fall) {
        c->btarget[w] = target;
        if (next == target) { ++c->taken[w]; flag(target, kBranchTarget); }
        else if (next == fall) ++c->not_taken[w];
    };
    auto indirect = [&](bool rts) {
        auto [it, fresh] = indirect_.try_emplace({prev_ctx_, prev_pc_});
        if (fresh) {
            char buf[128];
            m68k_disassemble(buf, prev_pc_, M68K_CPU_TYPE_68000);
            it->second.text = buf;
            it->second.op = rd16(prev_pc_);
            it->second.rts = rts;
        }
        uint32_t off;
        Ctx* t = ctx_for(next, off);
        ++it->second.targets[{t ? ctx_index(t) : -1, next}];
        if (t) t->flags[off >> 1] |= kIndirectTarget;
    };
    auto call = [&]() {
        shadow_[(sp & mask_) >> 1] = rd32(sp) & 0xFFFFFF;
        flag(next, kCallTarget);
    };
    uint16_t op = rd16(prev_pc_);
    switch (prev_class_) {
    case kBcc: {
        int8_t d8 = int8_t(op & 0xFF);
        if (d8) branch(prev_pc_ + 2 + uint32_t(int32_t(d8)), prev_pc_ + 2);
        else branch(prev_pc_ + 2 + uint32_t(int32_t(int16_t(rd16(prev_pc_ + 2)))), prev_pc_ + 4);
        break;
    }
    case kDbcc: branch(prev_pc_ + 2 + uint32_t(int32_t(int16_t(rd16(prev_pc_ + 2)))), prev_pc_ + 4); break;
    case kBra:
        c->btarget[w] = next;
        ++c->taken[w];
        flag(next, kBranchTarget);
        break;
    case kJmpDir: flag(next, kBranchTarget); break;
    case kBsr:
    case kJsrDir: call(); break;
    case kJsrInd: call(); indirect(false); break;
    case kJmpInd: indirect(false); break;
    case kRts:
    case kRtr: {
        // A return goes to the address its matching call pushed; anything else is a computed jump through the
        // stack (push target; rts), e.g. obj_call_handler_a $5924.
        uint32_t slot = ((sp - 4) & mask_) >> 1;
        if (prev_class_ == kRtr || shadow_[slot] != next) indirect(true);
        shadow_[slot] = 0;
        break;
    }
    default: break;
    }
}

void Tracer::on_redirect(uint32_t from, uint32_t to) {
    uint32_t off;
    if (Ctx* c = ctx_for(from, off)) {
        c->flags[off >> 1] |= kHleTrap;
        ++hle_[{ctx_index(c), from}];
    }
    // The trap emulated an RTS: retire the matching shadow-stack entry. Musashi then executes the instruction at
    // `to` without calling the hook, so it is recorded here.
    shadow_[((env_.sp() - 4) & mask_) >> 1] = 0;
    have_prev_ = false;
    record_exec(to);
}

void Tracer::on_cpu_write(uint32_t addr, int size) {
    addr &= 0xFFFFFF;
    if (addr >= 0x200000) return;
    addr &= mask_;
    if (layout_.st && addr < layout_.st_level_base + 16 && addr + uint32_t(size) > layout_.st_level_base) st_dirty_ = true;
    if (addr >= layout_.overlay_base && addr < layout_.overlay_end && overlay_ < 0) return; // no module loaded yet
    uint32_t off;
    Ctx* c = ctx_for(addr, off);
    if (!c) return;
    bool hit = false;
    for (int i = 0; i < size && off + uint32_t(i) < c->size; ++i) hit |= c->code_byte[off + uint32_t(i)] != 0;
    if (!hit) return;
    CodeWrite& cw = code_writes_[{ctx_index(c), addr, have_prev_ ? prev_ctx_ : -1, prev_pc_}];
    if (cw.count++ == 0) cw.first_frame = *env_.frame;
    cw.value_size = uint32_t(size);
}

void Tracer::on_dma_write(uint32_t lo, uint32_t hi) {
    for (uint32_t a = lo; a <= hi; ++a) {
        if (a >= layout_.overlay_base && a < layout_.overlay_end && overlay_ < 0) continue;
        uint32_t off;
        Ctx* c = ctx_for(a, off);
        if (!c || !c->code_byte[off]) continue;
        // Writer -2 = blitter, started by the CPU instruction at prev_pc_ (the BLTSIZE write); the address is the
        // first executed-code byte the blit hit.
        CodeWrite& cw = code_writes_[{ctx_index(c), a, -2, prev_pc_}];
        if (cw.count++ == 0) cw.first_frame = *env_.frame;
        cw.value_size = hi - lo + 1;
        break;
    }
}

bool Tracer::write_json(const std::string& path, const std::string& label, uint32_t frames) const {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    auto cname = [&](int i) -> std::string {
        if (i == -2) return "blitter"; // writer = PC of the CPU instruction that started the blit
        return i >= 0 && size_t(i) < ctxs_.size() ? ctxs_[size_t(i)]->name : "outside";
    };
    std::fprintf(f, "{\n  \"format\": \"x2-trace-1\",\n  \"label\": \"%s\",\n  \"frames\": %u,\n", label.c_str(), frames);
    std::fprintf(f, "  \"insn_fields\": [\"pc\", \"count\", \"len\", \"flags\"],\n");
    std::fprintf(f, "  \"flag_bits\": {\"call_target\": 1, \"branch_target\": 2, \"indirect_target\": 4, "
                    "\"interrupt_entry\": 8, \"hle_trap\": 16},\n");
    std::fprintf(f, "  \"branch_fields\": [\"site\", \"target\", \"taken\", \"not_taken\"],\n");
    std::fprintf(f, "  \"modules\": [\n");
    for (size_t ci = 0; ci < ctxs_.size(); ++ci) {
        const Ctx& c = *ctxs_[ci];
        uint64_t insns = 0, bytes = 0;
        for (size_t w = 0; w < c.count.size(); ++w)
            if (c.count[w]) { ++insns; bytes += c.len[w]; }
        std::fprintf(f, "    {\"name\": \"%s\", \"key\": \"%016llx\", \"base\": \"0x%x\", \"end\": \"0x%x\", "
                        "\"executed_insns\": %llu, \"executed_bytes\": %llu,\n     \"loads\": [",
                     c.name.c_str(), (unsigned long long)c.key, c.base, c.base + c.size, (unsigned long long)insns,
                     (unsigned long long)bytes);
        for (size_t i = 0; i < c.loads.size(); ++i) {
            const Load& l = c.loads[i];
            std::fprintf(f, "%s{\"frame\": %u, \"sector\": %u, \"count\": %u, \"dest\": \"0x%x\", \"hash\": \"%016llx\"}",
                         i ? ", " : "", l.frame, l.sector, l.count, l.dest, (unsigned long long)l.hash);
        }
        std::fprintf(f, "],\n     \"insns\": [");
        bool first = true;
        for (size_t w = 0; w < c.count.size(); ++w) {
            if (!c.count[w]) continue;
            std::fprintf(f, "%s[%u,%u,%u,%u]", first ? "" : ",", c.base + uint32_t(w) * 2, c.count[w], c.len[w], c.flags[w]);
            first = false;
        }
        std::fprintf(f, "],\n     \"branches\": [");
        first = true;
        for (size_t w = 0; w < c.count.size(); ++w) {
            if (!c.taken[w] && !c.not_taken[w]) continue;
            std::fprintf(f, "%s[%u,%u,%u,%u]", first ? "" : ",", c.base + uint32_t(w) * 2, c.btarget[w], c.taken[w], c.not_taken[w]);
            first = false;
        }
        std::fprintf(f, "]}%s\n", ci + 1 < ctxs_.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"indirect\": [\n");
    size_t n = 0;
    for (const auto& [k, ind] : indirect_) {
        std::fprintf(f, "    {\"module\": \"%s\", \"site\": \"0x%x\", \"insn\": \"%s\", \"op\": \"0x%04x\", \"rts_jump\": %s, \"targets\": [",
                     cname(k.first).c_str(), k.second, ind.text.c_str(), ind.op, ind.rts ? "true" : "false");
        size_t j = 0;
        for (const auto& [t, cnt] : ind.targets)
            std::fprintf(f, "%s{\"module\": \"%s\", \"pc\": \"0x%x\", \"count\": %llu}", j++ ? ", " : "", cname(t.first).c_str(),
                         t.second, (unsigned long long)cnt);
        std::fprintf(f, "]}%s\n", ++n < indirect_.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"interrupts\": [");
    n = 0;
    for (const auto& [k, cnt] : interrupts_)
        std::fprintf(f, "%s\n    {\"level\": %d, \"module\": \"%s\", \"pc\": \"0x%x\", \"count\": %llu}", n++ ? "," : "",
                     std::get<0>(k), cname(std::get<1>(k)).c_str(), std::get<2>(k), (unsigned long long)cnt);
    std::fprintf(f, "\n  ],\n  \"hle_traps\": [");
    n = 0;
    for (const auto& [k, cnt] : hle_)
        std::fprintf(f, "%s{\"module\": \"%s\", \"pc\": \"0x%x\", \"count\": %llu}", n++ ? ", " : "", cname(k.first).c_str(),
                     k.second, (unsigned long long)cnt);
    std::fprintf(f, "],\n  \"code_writes\": [");
    n = 0;
    for (const auto& [k, cw] : code_writes_)
        std::fprintf(f, "%s\n    {\"module\": \"%s\", \"addr\": \"0x%x\", \"size\": %u, \"writer_module\": \"%s\", \"writer\": \"0x%x\", "
                        "\"count\": %llu, \"first_frame\": %u}",
                     n++ ? "," : "", cname(std::get<0>(k)).c_str(), std::get<1>(k), cw.value_size,
                     cname(std::get<2>(k)).c_str(), std::get<3>(k), (unsigned long long)cw.count, cw.first_frame);
    std::fprintf(f, "\n  ],\n  \"code_changes\": [");
    n = 0;
    for (const auto& [k, v] : code_changes_)
        for (const Change& ch : v)
            std::fprintf(f, "%s\n    {\"module\": \"%s\", \"pc\": \"0x%x\", \"frame\": %u, \"before\": \"0x%04x\", \"after\": \"0x%04x\"}",
                         n++ ? "," : "", cname(k.first).c_str(), k.second, ch.frame, ch.before, ch.after);
    std::fprintf(f, "\n  ],\n  \"outside\": [");
    n = 0;
    for (const auto& [pc, cnt] : outside_)
        std::fprintf(f, "%s{\"pc\": \"0x%x\", \"count\": %llu}", n++ ? ", " : "", pc, (unsigned long long)cnt);
    std::fprintf(f, "]\n}\n");
    return std::fclose(f) == 0;
}

} // namespace x2
