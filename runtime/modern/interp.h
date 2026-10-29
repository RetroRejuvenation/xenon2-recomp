// Interpolated re-rendering between logic steps (docs/re-notes/modernisation-design.md).
//
// The game rebuilds its 192-line playfield from scratch every logic step (far-layer ring copy, foreground tile
// map, starfield, object bobs) and keeps the status panel (lines 192-199) in place. The Interpolator replays
// one captured step's draw list (DrawCapture with capture_data) into a canvas `scale` times the Amiga
// resolution, moving each command by a fraction of its layer's or object's motion between two consecutive
// steps a and b:
//   far layer   the ring row $436 (optionally smoothed to constant speed, see InterpOptions)
//   tiles       the foreground scroll, measured by matching tile blits between a and b
//   stars       each star's continuous line position (line + sub-line accumulator / $180)
//   objects     the object record's +$20/+$24 position (records matched by address and update handler)
//   the rest    (status panel, unidentified blits) not moved.
// For t < 0.5 a's list is replayed moved forward by t, otherwise b's list moved back by 1 - t, so t = 0 and
// t = 1 give a's and b's pictures exactly, objects present in both move linearly, and objects that appear or
// vanish do so at the midpoint. The palette is blended per channel from a's to b's (palette fades).
//
// The canvas is split into horizontal bands that replay the whole list clipped to their rows, so `threads`
// workers give the same pixels as one.
#pragma once
#include "modern/draw_capture.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace x2::modern {

struct InterpOptions {
    int scale = 1;                // canvas pixels per Amiga pixel on each axis
    bool smooth_far_layer = true; // move the far layer (half a line per step) at constant speed; then t = 0 / 1
                                  // can differ from the real pictures by half a line in that layer
    int max_object_jump = 48;     // a larger per-step move of an object is a cut for that object
    int threads = 1;              // render bands in parallel (0 = one per core, at most 8)
};

// Which part of the picture a draw command belongs to (how it is moved).
enum class Layer : uint8_t { Other, Far, Tiles, Stars, Object };
Layer layer_of(const BlitCmd& b);

// Motion between two consecutive steps, a -> b, in Amiga pixels.
struct PairMotion {
    bool ok = false;           // both are gameplay steps with a plausible scroll: interpolation is possible
    const char* why = "";      // reason when !ok
    float far_from = 0, far_to = 0; // far layer position at t = 0 / 1, relative to a's ring row
    int far_step = 0;          // ring rows moved a -> b (screen down)
    int fg_dy = 0;             // foreground tile layer, screen y
    std::unordered_map<uint32_t, std::array<float, 2>> objects; // record -> (dx, dy)
    std::array<float, kStars> star_dy{};
    std::array<bool, kStars> star_ok{};
};

class Interpolator {
public:
    Interpolator();
    ~Interpolator();
    Interpolator(const Interpolator&) = delete;
    Interpolator& operator=(const Interpolator&) = delete;

    InterpOptions opt;

    // b must be the step after a; z, the step before a, is used for far-layer smoothing (may be null).
    // Not interpolable (a cut): a non-gameplay step, steps not consecutive or too far apart, a scroll jump, an
    // empty (cleared) playfield, or most objects jumping further than max_object_jump.
    PairMotion analyse(const Step* z, const Step& a, const Step& b) const;
    // Picture at t in [0, 1] as RGBA8, (kWidth * scale) x (kLines * scale).
    void render(const Step& a, const Step& b, const PairMotion& mo, float t, std::vector<uint8_t>& rgba);
    // The same into rows of 32-bit RGBA pixels (R in the low byte) `stride` pixels apart.
    void render(const Step& a, const Step& b, const PairMotion& mo, float t, uint32_t* out, size_t stride);
    // A step's own picture (its captured image) as RGBA8 at `scale`, for comparisons.
    static void picture(const Step& s, int scale, std::vector<uint8_t>& rgba);
    // Gameplay step: drew the far layer copy and the foreground tiles, and has its blit data.
    static bool gameplay(const Step& s);

private:
    // One draw command of the step being replayed, with its displacement in canvas pixels.
    struct Cmd {
        bool blit;
        bool star;
        int ox, oy;
        uint32_t index; // into Step::blits or Step::writes
    };
    void build(const Step& s, const PairMotion& mo, float f, bool from_a);
    void base(const Step& s);
    void band(const Step& s, int y0, int y1, const uint32_t* pal, uint32_t* out, size_t stride);
    void replay_blit(const Step& s, const BlitCmd& b, int ox, int oy, bool clip, int y0, int y1);
    void replay_write(const Step& s, const CpuWrite& w, int ox, int oy, bool clip, bool star, int y0, int y1);

    int w_ = 0, h_ = 0;              // canvas size (gutter included)
    std::vector<uint8_t> canvas_;    // 4-bit colour index per canvas pixel
    std::vector<uint8_t> base_;      // the replayed step's own picture at the canvas scale
    uint64_t base_key_ = ~0ull;      // step and scale base_ holds
    std::vector<Cmd> cmds_;
    struct Pool;
    std::unique_ptr<Pool> pool_;
};

// Host-rate presentation: owns the capture and maps emulated time to a step pair. A step's picture is swapped
// in at T = swap frame + swap line / lines per frame; at emulated time tau the newest step k with T_k <= tau
// is known, and the picture shown is the pair (k-1, k) at t = (tau - T_k) / (T_k - T_{k-1}), clamped to 1:
// one logic step (normally 3 frames, 60 ms) of added latency, the step being computed meanwhile.
//   - A pair that is not interpolable but whose newest step is a gameplay step (a cut inside gameplay, or the
//     first step of a level) shows that step's own picture, at the same latency, so the picture never steps
//     back in time.
//   - Anything else (menus, shop, title, no steps), and a step held for more than hold_frames (game pause,
//     disk loads), presents the faithful frame instead.
class InterpPresenter {
public:
    Interpolator interp;
    DrawCapture capture;
    double hold_frames = 8;

    void attach(Machine& m, int lines_per_frame);
    void detach(Machine& m) { capture.detach(m); }
    // The window picture (kWidth x kLines, times the scale) for emulated time tau, in frames since power-on,
    // into rows of `out` `stride` pixels apart. False when there is no step picture: show the faithful frame.
    bool frame(double tau, uint32_t* out, size_t stride);
    bool frame(double tau, std::vector<uint8_t>& rgba); // kWidth * scale x kLines * scale RGBA8

    uint64_t presented = 0, interpolated = 0, render_ns = 0;

private:
    double step_time(const Step& s) const { return double(s.swap_frame) + double(s.swap_line) / double(lines_); }
    int lines_ = 313;
    uint32_t motion_for_ = ~0u; // index of the step b the cached motion belongs to
    PairMotion motion_;
};

} // namespace x2::modern
