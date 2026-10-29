// Execution tracer for coverage runs (Phase 5b): records every executed instruction, taken/not-taken branch
// counts, indirect jump/call targets (including rts used as a jump), interrupt entries, HLE trap redirects and
// writes into executed code. Code in the level-module window $54E00-$7FFFF is attributed to the module that
// the disk shim loaded there, identified by a content hash, so the six disc-2 overlays are kept apart.
//
// Enabled only by the driver (--trace-out); the Machine calls in through a null-checked pointer, so a run
// without tracing does no extra work beyond that check. Output: one deterministic JSON file per run, consumed
// by tools/trace/merge_traces.py and tools/trace/report.py.
#pragma once
#include <cstdint>
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

    // Per-instruction flags in the output.
    enum : uint8_t { kCallTarget = 1, kBranchTarget = 2, kIndirectTarget = 4, kInterruptEntry = 8, kHleTrap = 16 };

    explicit Tracer(Machine& m);
    ~Tracer();

    // Machine hooks.
    void on_instruction(uint32_t pc);
    void on_redirect(uint32_t from, uint32_t to); // a PC trap moved the CPU (the new PC's instruction runs unhooked)
    void on_cpu_write(uint32_t addr, int size);
    // A blitter area blit wrote D to chip RAM bytes lo..hi (inclusive; line mode is not reported).
    void on_dma_write(uint32_t lo, uint32_t hi);
    // Disk shim hook: count sectors from `sector` were copied to `dest`.
    void on_disk_load(uint32_t dest, uint16_t sector, uint16_t count, const uint8_t* data);

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

    Ctx* ctx_for(uint32_t addr, uint32_t& off);
    Ctx* overlay_ctx();
    int ctx_index(const Ctx* c) const;
    void process_prev(uint32_t next, uint32_t sp_after);
    void record_exec(uint32_t pc);
    uint32_t rd32(uint32_t a) const;
    uint16_t rd16(uint32_t a) const;

    Machine& m_;
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
