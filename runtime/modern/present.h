// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Presentation stage of the modernisation layer (docs/re-notes/modernisation-design.md, Phases 9b/9c).
// Platform independent: it decides which picture the window shows and when; the SDL window applies the output
// filter (runtime/gfx/sdl_filters.h).
//
//   PresentOptions  the user's choices (settings.cfg, command line, hotkeys) and the single Faithful switch.
//   Presenter       the picture to show: the faithful frame itself (no copy, so it hashes like Video::front()),
//                   or the interpolated playfield composed over the faithful frame at the canvas scale, and
//                   the CPU edge-preserving upscale (Scale2x/Scale3x) for the edge filter.
//   PresentClock    maps host display times to emulated time: smooth, monotonic, a fixed lag behind the
//                   newest emulated frame (frames are run when the audio device asks for them, so they arrive
//                   in bursts; the presentation must not).
//   VsyncTimeline   the display time of each present on a fixed refresh grid, so successive presents are
//                   exactly one refresh period apart in emulated time too.
#pragma once
#include "modern/interp.h"
#include "persist.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x2 {
class Machine;
}

namespace x2::modern {

// The faithful frame (Video::front()): kFrameWidth x kFrameHeight, the display window (kLines lines) starting
// at line kFrameTop (DIWSTRT $3A - Video::kFbTop $2C).
constexpr int kFrameWidth = 320, kFrameHeight = 256, kFrameTop = 0x3A - 0x2C;

// Widescreen surround width (Phase 9e): an aspect ratio of the whole picture (the 256-line frame as the window
// shows it, with square or 4:3 pixels), or a number of Amiga pixels per side. At most kMaxWide per side.
constexpr int kMaxWide = 320;
struct WideSpec {
    int num = 0, den = 0; // aspect ratio, e.g. 16:9
    int px = 0;           // else pixels per side (0 = off)
    bool on() const { return num > 0 || px > 0; }
    bool operator==(const WideSpec& o) const { return num == o.num && den == o.den && px == o.px; }
};
// "off", "0".."320", or "W:H" (e.g. 16:10, 16:9, 21:9, 32:9). False for anything else.
bool parse_wide(const std::string& text, WideSpec& out);
std::string wide_name(const WideSpec& w);
// Amiga pixels per side: (frame height x ratio, in Amiga pixels - 320) / 2, clamped to 0..kMaxWide.
int wide_pixels(const WideSpec& w, bool aspect);
// The hotkey's cycle: off, 16:10, 16:9, 21:9, 32:9.
WideSpec next_wide(const WideSpec& w);

struct PresentOptions {
    bool faithful = false; // the Faithful switch: overrides everything below
    bool interp = false;   // interpolated frames between logic steps at the display rate
    int scale = 3;         // interpolation canvas, Amiga pixels x scale
    bool smooth = true;    // far layer at constant speed
    VideoFilter filter = VideoFilter::Nearest;
    bool aspect = false;   // 4:3 display (PAL lores pixels 16:15 wide) instead of square pixels
    bool integer = true;   // integer scale factors (the vertical one with aspect)
    bool curvature = false;
    bool upgrade = false;  // replacement asset packs and palette overrides (modern/assets.h)
    WideSpec wide;         // widescreen surround
    bool hd_panel = false; // status panel upscaled with the edge-preserving scaler at the canvas scale

    // The options in force: Faithful means no interpolation, nearest integer scaling of square pixels, no
    // upgraded graphics, no surround.
    PresentOptions effective() const;
    std::string describe() const;
};

// A picture to present: RGBA8, w x h, `scale` canvas pixels per Amiga pixel on each axis.
struct Canvas {
    const uint8_t* rgba = nullptr;
    int w = 0, h = 0, scale = 1;
};

// Edge-preserving integer upscalers (AdvMAME2x / AdvMAME3x, a.k.a. Scale2x / Scale3x) on RGBA8 pixels.
void scale2x(const uint32_t* src, int w, int h, uint32_t* dst);
void scale3x(const uint32_t* src, int w, int h, uint32_t* dst);

class Presenter {
public:
    // The asset packs for `upgrade` (shared with the caller; may be empty). Takes effect at the next configure().
    void set_assets(std::shared_ptr<const AssetLibrary> lib) { assets_ = std::move(lib); }
    // Amiga pixels added on each side of the picture by the surround (0 = off).
    int wide() const { return wide_px_; }
    // Applies the options (their effective form) and attaches or detaches the draw capture. keep_capture
    // captures even without interpolation (--modern-check, --modern-shots).
    void configure(Machine& m, const PresentOptions& o, int lines_per_frame, bool keep_capture = false);
    const PresentOptions& options() const { return eff_; }
    bool interpolating() const { return eff_.interp; }
    // The faithful picture: `front` itself, or its edge upscale with the edge filter.
    Canvas faithful(const std::vector<uint8_t>& front);
    // The picture for emulated time tau: the interpolated playfield over the faithful frame when there is a
    // step picture for tau, else faithful(front). With the surround it is (kFrameWidth + 2 wide()) * scale
    // wide; without interpolation and replacement graphics its centre is the faithful frame exactly.
    Canvas at(double tau, const std::vector<uint8_t>& front);
    // Machine reset: forget the captured steps.
    void reset();
    InterpPresenter* interp() { return ip_.get(); }
    // Upscale factor of the edge filter: 2, 3 or 4 (Scale2x twice); the window sets it from its size.
    int edge_factor = 3;
    uint64_t edge_ns = 0, edged = 0;

private:
    Canvas edge(const uint32_t* src, int w, int h, int scale);
    void hd_panel(const uint32_t* front, uint32_t* centre, size_t stride, int scale);
    PresentOptions eff_;
    std::shared_ptr<const AssetLibrary> assets_;
    int wide_px_ = 0, scale_ = 1;
    bool packs_ = false;
    std::vector<uint32_t> panel_a_, panel_b_;
    bool keep_capture_ = false;
    std::unique_ptr<InterpPresenter> ip_;
    std::vector<uint8_t> full_;
    std::vector<uint32_t> edge_a_, edge_b_;
};

class PresentClock {
public:
    explicit PresentClock(double frame_ns) : frame_ns_(frame_ns) {}
    // Frames the presentation stays behind the newest emulated frame. tau is asked for the display time of the
    // next refresh, up to one refresh period ahead, and frames arrive up to an audio chunk late, so the window
    // sets lag = kMargin + refresh period / frame period. When tau still catches up with the newest frame the
    // clock adds up to kMaxExtra frames to the lag (and gives it back slowly): it adapts to the audio device's
    // chunk size.
    static constexpr double kMargin = 0.75, kMaxExtra = 1.0;
    double lag = kMargin;

    // Start again from the next frame_done (pause, reset, stalls).
    void reset() { valid_ = false; }
    // `frames` emulated frames are complete at host time now_ns (Machine::frame after run_frame).
    void frame_done(uint64_t now_ns, double frames);
    // Emulated time to show at host time display_ns: increasing between resets, never past the newest frame.
    double tau(uint64_t display_ns);
    uint32_t snaps() const { return snaps_; }
    double rate() const { return rate_; }
    double extra_lag() const { return extra_; }

private:
    double frame_ns_;
    bool valid_ = false;
    uint64_t t0_ = 0;
    double tau0_ = 0, rate_ = 1, integ_ = 0, newest_ = 0, last_ = 0, extra_ = 0;
    uint32_t snaps_ = 0;
};

class VsyncTimeline {
public:
    explicit VsyncTimeline(double period_ns = 1e9 / 60) : period_(period_ns), mode_(period_ns) {}
    // The refresh period the display mode reports; used until the vsync waits have been measured.
    void set_mode_period(double period_ns);
    double period() const { return period_; }
    // Display time of the next present: the first refresh after now that no earlier present was given.
    uint64_t next(uint64_t now_ns);
    // The present for the last next() returned at now_ns after waiting for vsync: keeps the grid locked to it
    // and measures the real refresh period (the median interval between such presents).
    void presented(uint64_t now_ns);
    uint32_t resyncs() const { return resyncs_; }

private:
    double period_, mode_;
    bool init_ = false;
    double origin_ = 0;
    int64_t slot_ = 0;
    uint32_t resyncs_ = 0;
    uint64_t last_ = 0;
    std::vector<double> intervals_;
    bool measured_ = false;
};

} // namespace x2::modern
