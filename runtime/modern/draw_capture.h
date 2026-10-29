// Draw-command capture (modernisation layer, docs/re-notes/modernisation-design.md).
//
// Xenon 2 draws every picture with the blitter (plus a few CPU writes) into the back buffer `$406`, and
// `wait_frame_swap_copper` ($202E) swaps `$406`/`$40A` once per logic step. DrawCapture observes the machine
// through Machine::on_blit / on_cpu_write and groups everything drawn into one logic step: each blit with its
// full register set (decoded into buffer-relative position) and, optionally, the A/B/C words it consumed; each
// CPU write into a screen buffer; and, when the step's picture is swapped in, the object lists, the scroll and
// starfield state, the palette and the finished picture. It never changes machine state, so a run with the
// capture attached produces the same frames as one without.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

namespace x2 {
class Machine;
}

namespace x2::modern {

// Screen geometry: 4 interleaved lores planes, 40 bytes per plane row, 160 bytes per line, 200 visible lines,
// of which 0-191 are the playfield (redrawn every step) and 192-199 the status panel (updated in place).
constexpr uint32_t kRowBytes = 40;
constexpr uint32_t kLineBytes = 160;
constexpr int kPlanes = 4;
constexpr int kWidth = 320;
constexpr int kLines = 200;
constexpr int kPlayfieldLines = 192;
// The game draws partly visible tile rows into the 16 lines above each buffer; Step::image keeps them.
constexpr int kGutterLines = 16;

// One blit, with the registers as the CPU set them before BLTSIZE.
struct BlitCmd {
    uint32_t seq = 0;    // order among the step's blits and CPU writes
    uint32_t pc = 0;     // the instruction that wrote BLTSIZE (REG_PPC)
    uint32_t caller = 0; // long on top of the stack at that point: the drawing routine's return address
    uint32_t obj = 0;    // object record being drawn (run_object_list_draw, $366A), 0 = none
    uint16_t con0 = 0, con1 = 0, fwm = 0, lwm = 0;
    uint16_t w = 0, h = 0; // words, rows
    uint32_t apt = 0, bpt = 0, cpt = 0, dpt = 0;
    int16_t amod = 0, bmod = 0, cmod = 0, dmod = 0;
    uint16_t adat = 0, bdat = 0, cdat = 0;
    uint32_t regs[16] = {}; // D0-D7, A0-A7
    uint32_t frame = 0;
    uint16_t line = 0;
    // Decoded destination: which screen buffer (0 = the step's draw buffer, 1 = the other one, -1 = elsewhere),
    // plane, line and pixel x (word x * 16 + A shift).
    int buffer = -1;
    int plane = 0, y = 0, x = 0;
    // With DrawCapture::capture_data: index into Step::words of w*h triples {A, B, C}: A after the first/last
    // word masks and the shifter, B after the shifter, C as read. ~0u = not captured (line or fill mode).
    uint32_t data = ~0u;
    bool line_mode() const { return con1 & 1; }
    bool fill_mode() const { return con1 & 0x18; }
    int ash() const { return con0 >> 12; }
};

struct CpuWrite {
    uint32_t seq = 0;
    uint32_t pc = 0, addr = 0, value = 0, old = 0; // old = chip contents before the write
    uint8_t size = 0;
    int buffer = -1; // as BlitCmd::buffer
};

// One live object record at the end of a step (list head pointers at $A88/$AEA/$B4C/$BAE/$C10, next link at
// +$12, a record with a zero +0 word ends a list).
struct ObjectSnap {
    uint32_t addr = 0;
    uint8_t list = 0;
    uint32_t update = 0, draw = 0, hit = 0, gfx = 0; // +2, +6, +$A, +$16
    int16_t x = 0, y = 0;                           // +$20, +$24
};

// Game variables sampled at the swap (memory-map.md, timing.md).
enum Var { kRing = 0, kScrollCD0, kMapRow, kFineScroll, kSpeed, kFrameCount, kVarCEA, kVarCE8, kVarCount };
constexpr uint32_t kVarAddr[kVarCount] = {0x436, 0xCD0, 0xCCE, 0xCD6, 0xCD8, 0xCDC, 0xCEA, 0xCE8};
constexpr uint32_t kStarTable = 0x42FDC; // 48 x {line offset.w, pixel mask.w, sub-line accumulator.w}
constexpr int kStars = 48;

struct Step {
    uint32_t index = 0;
    uint32_t first_frame = 0, swap_frame = 0;
    uint16_t swap_line = 0;
    uint32_t draw_buf = 0;  // the buffer this step drew into (shown after the swap)
    uint32_t other_buf = 0; // the buffer on display while it drew
    std::vector<BlitCmd> blits;
    std::vector<CpuWrite> writes;
    std::vector<ObjectSnap> objects;
    std::vector<uint16_t> words; // blit A/B/C data (BlitCmd::data)
    uint16_t vars[kVarCount] = {};
    std::array<uint16_t, kStars * 3> stars{};
    uint16_t palette[32] = {};  // COLOR00-31 as last written at the swap
    // The finished picture: lines -kGutterLines..kLines-1 of draw_buf (capture_data only).
    std::vector<uint8_t> image;
    bool partial = false; // the capture started (or was reset) while this step was being drawn
};

class DrawCapture {
public:
    static constexpr uint32_t kDrawPtr = 0x406, kShowPtr = 0x40A;
    static constexpr uint32_t kBufferBytes = kLines * kLineBytes;
    static constexpr uint32_t kListHeads[5] = {0xA88, 0xAEA, 0xB4C, 0xBAE, 0xC10};
    static constexpr uint32_t kObjectDrawReturn = 0x3678; // return address of run_object_list_draw's jsr (a1)

    // Install the observers (replacing any on_blit / on_cpu_write already set). Attached after power-on, the
    // step in progress is marked partial.
    void attach(Machine& m);
    void detach(Machine& m);
    // Forget the kept steps (machine reset); the step in progress is marked partial.
    void reset();

    // Called with each completed step (its picture has just been swapped in).
    std::function<void(const Step&)> on_step;
    // Keep completed steps in steps() (default on; off when on_step consumes them). At most max_kept are kept.
    bool keep = true;
    size_t max_kept = SIZE_MAX;
    // Record blit data and the finished picture (needed to re-render a step; costs ~60 KB per step).
    bool capture_data = false;
    // Also record CPU writes outside both screen buffers (default off: they are mostly game state).
    bool all_writes = false;

    const std::vector<Step>& steps() const { return steps_; }
    const Step& current() const { return cur_; }

    // Decode a chip address against the two buffers: buffer index (0 = `draw`, 1 = `other`, -1 = neither), and
    // plane / line / byte column within it.
    static int locate(uint32_t addr, uint32_t draw, uint32_t other, int& plane, int& y, int& xbyte);

private:
    void blit(Machine& m);
    void cpu_write(Machine& m, uint32_t addr, uint32_t value, int size);
    void finish_step(Machine& m);
    uint32_t find_object(const Machine& m) const;
    void capture_words(const Machine& m, BlitCmd& b);
    static uint32_t long_at(const Machine& m, uint32_t a);

    Step cur_;
    std::vector<Step> steps_;
    uint32_t next_index_ = 0;
    uint32_t seq_ = 0;
    bool started_ = false;
};

// Text dump of one step (the --draw-log format, read by tools/modern/analyze_draw_log.py):
//   S index first_frame swap_frame swap_line draw_buf other_buf vars0..7
//   B pc caller obj frame line con0 con1 fwm lwm w h apt bpt cpt dpt amod bmod cmod dmod adat bdat buffer plane y x
//     d0..d7 a0..a7
//   W pc addr size value old buffer
//   O addr list update draw hit gfx x y
void write_step_log(FILE* f, const Step& s);

} // namespace x2::modern
