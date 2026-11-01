// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Execution tracer for coverage runs (Phase 5b): records every executed instruction, taken/not-taken branch
// counts, indirect jump/call targets (including rts used as a jump), interrupt entries, HLE trap redirects and
// writes into executed code. Code in the overlay window is attributed to the module loaded there, identified by a
// content hash, so overlays that share the window are kept apart. The disk build's window is $54E00-$7FFFF (six
// disc-2 modules loaded by the disk shim); the CDTV release's is the 180,000-byte area at $4E26 that X2 loads LEVELn,
// INTRO and PICCODE into (docs/re-notes/cdtv-c0.md), named after the disc file.
//
// Enabled only by the driver (--trace-out); the Machine calls in through a null-checked pointer, so a run
// without tracing does no extra work beyond that check. Output: one deterministic JSON file per run, consumed
// by tools/trace/merge_traces.py and tools/trace/report.py.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x2 {

class Machine;

class Tracer {
public:
    static constexpr uint32_t kOverlayBase = 0x54E00;
    static constexpr uint32_t kOverlayEnd = 0x80000;
    static constexpr uint32_t kLevelTable = 0x91EA; // {start sector.w, count.w} x 6 in the resident image

    // Where code is attributed: the overlay window, and the extent of the resident ("game") context. Addresses
    // outside the window belong to the game context below `game_end`; above it, to nothing (executed PCs there are
    // reported as "outside"). The disk build's game context is everything below its window; the CDTV release's is
    // the whole of chip RAM except the window (its main code lies above the overlay area).
    //
    // The Atari ST release (docs/re-notes/atari-st-recomp.md) has no module loader to hook: the real loader unpacks a front end
    // to $40000 and, from the engine's first instruction on, one level module at a time to $4F000, all inside the window
    // [$40000, $80000). Its modules are told apart by phase and by content: before the engine starts ($7CF8) the window holds the
    // front end ("frontend") and low RAM holds the stage-1 loader ("stage1"); after, low RAM is the engine ("game") and the
    // window holds a level module named "mod_<key>", the key being a hash of 2 KB from byte 16 of the module, re-taken at the first
    // entry after the program wrote the module's first bytes.
    struct Layout {
        uint32_t overlay_base, overlay_end, game_end;
        bool st = false;
        uint32_t st_engine_entry = 0, st_level_base = 0, st_boot_base = 0;
        static Layout disk() { return {kOverlayBase, kOverlayEnd, kOverlayBase}; }
        static Layout cdtv(uint32_t chip_size) { return {0x4E26, 0x4E26 + 180000, chip_size}; }
        static Layout atari_st() {
            Layout l{0x40000, 0x80000, 0x40000};
            l.st = true;
            l.st_engine_entry = 0x7CF8;
            l.st_level_base = 0x4F000;
            l.st_boot_base = 0x70000;
            return l;
        }
    };

    // The machine the tracer watches: RAM (a power of two in size), the frame counter, and the CPU registers the tracer needs.
    // `irq_vectors` are (level, address of a vector table entry): a PC equal to the long there, with the CPU's mask at that
    // level and a lower mask stacked, is an interrupt entry.
    struct Env {
        const uint8_t* mem = nullptr;
        uint32_t size = 0;
        const uint32_t* frame = nullptr;
        std::function<uint32_t()> sp, sr;
        std::vector<std::pair<int, uint32_t>> irq_vectors;
    };

    // Per-instruction flags in the output.
    enum : uint8_t { kCallTarget = 1, kBranchTarget = 2, kIndirectTarget = 4, kInterruptEntry = 8, kHleTrap = 16 };

    explicit Tracer(Machine& m, Layout layout = Layout::disk());
    Tracer(Env env, Layout layout);
    ~Tracer();

    // Machine hooks.
    void on_instruction(uint32_t pc);
    void on_redirect(uint32_t from, uint32_t to); // a PC trap moved the CPU (the new PC's instruction runs unhooked)
    void on_cpu_write(uint32_t addr, int size);
    // A blitter area blit wrote D to chip RAM bytes lo..hi (inclusive; line mode is not reported).
    void on_dma_write(uint32_t lo, uint32_t hi);
    // Disk shim hook: count sectors from `sector` were copied to `dest`.
    void on_disk_load(uint32_t dest, uint16_t sector, uint16_t count, const uint8_t* data);
    // CDTV file service hook: `len` bytes of the disc file `name` were copied to `dest`. A load at the overlay window's
    // base starts a new module named after the file (lower case); other loads into the window are recorded only.
    void on_file_load(uint32_t dest, const std::string& name, const uint8_t* data, size_t len);

    bool write_json(const std::string& path, const std::string& label, uint32_t frames) const;

private:
    struct Ctx;
    struct Load { uint32_t frame; uint16_t sector, count; uint32_t dest; uint64_t hash; };
    struct Indirect {
        std::string text;
        uint16_t op = 0;
        bool rts = false;
        std::map<std::pair<int, uint32_t>, uint64_t> targets; // (ctx, pc) -> count
    };
    struct CodeWrite { uint64_t count = 0; uint32_t first_frame = 0; uint32_t value_size = 0; };

    void overlay_loaded(const std::string& name, uint64_t key, const Load& ld);
    void st_phase(uint32_t pc);
    Ctx* low_ctx();
    Ctx* ctx_for(uint32_t addr, uint32_t& off);
    Ctx* overlay_ctx();
    int ctx_index(const Ctx* c) const;
    void process_prev(uint32_t next, uint32_t sp_after);
    void record_exec(uint32_t pc);
    uint32_t rd32(uint32_t a) const;
    uint16_t rd16(uint32_t a) const;

    Env env_;
    uint32_t mask_ = 0;
    Layout layout_;
    bool st_engine_ = false, st_dirty_ = false;   // ST: the engine has started; the level module's first bytes were written
    int st_frontend_ = -1, st_stage1_ = -1;       // ST: context indices
    std::vector<std::unique_ptr<Ctx>> ctxs_; // [0] = game; overlay modules follow in load order
    int overlay_ = -1;                      // index of the module currently in the overlay window

    std::vector<uint8_t> op_class_;         // per 16-bit opcode
    uint32_t prev_pc_ = 0;
    int prev_ctx_ = -1;
    uint8_t prev_class_ = 0;
    bool have_prev_ = false;
    std::vector<uint32_t> shadow_;          // return address pushed by a call, per stack word

    std::map<std::pair<int, uint32_t>, Indirect> indirect_;                 // (ctx, site)
    std::map<std::tuple<int, int, uint32_t>, uint64_t> interrupts_;         // (level, ctx, pc)
    std::map<std::pair<int, uint32_t>, uint64_t> hle_;                      // (ctx, pc)
    std::map<std::tuple<int, uint32_t, int, uint32_t>, CodeWrite> code_writes_; // (ctx, addr, writer ctx, writer pc)
    struct Change { uint32_t frame; uint16_t before, after; };
    std::map<std::pair<int, uint32_t>, std::vector<Change>> code_changes_;
    std::map<uint32_t, uint64_t> outside_;  // executed PCs outside any window
};

} // namespace x2
