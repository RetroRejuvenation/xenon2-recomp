// SDL3 window presenting pictures with vsync through the output filters (runtime/gfx/sdl_filters.h): by
// default the Video framebuffer with nearest-neighbour integer scaling of square pixels (the faithful view),
// letterboxed. Pacing is done by the caller (FramePacer / the audio clock, modern::VsyncTimeline), not here.
#pragma once
#include "gfx/sdl_filters.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace x2 {

class SdlDisplay {
public:
    ~SdlDisplay();
    bool open(const std::string& title, int width, int height, int scale, bool fullscreen, bool aspect_4_3,
              std::string& error);
    // A picture of w x h canvas pixels, `scale` per Amiga pixel, through the view's filter.
    void present(const uint8_t* rgba, int w, int h, int scale, const View& v);
    // The last picture again, with the view (pause, option changes).
    void redraw(const View& v);
    void toggle_fullscreen();
    // Resizes the window to `scale` times the picture (ignored while fullscreen).
    void set_scale(int scale);
    // The window shape set_scale() uses: 4:3 or the picture's.
    void set_aspect(bool aspect_4_3) { aspect_ = aspect_4_3; }
    void set_title(const std::string& title);
    bool fullscreen() const { return fullscreen_; }
    bool vsync() const { return vsync_; }
    // Refresh rate of the window's display, 0 if unknown.
    float refresh_hz() const;
    // Window pixels per Amiga pixel for the view (at least 1).
    int output_scale(const View& v) const;
    // Text screen (first-run import) in SDL's built-in 8x8 debug font: lines of 0xRRGGBB text, word-wrapped to
    // the window width at about 80 columns.
    struct TextLine {
        std::string text;
        uint32_t rgb;
    };
    void present_text(const std::vector<TextLine>& lines);

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    FilterChain chain_;
    int w_ = 0, h_ = 0;
    bool aspect_ = false, fullscreen_ = false, vsync_ = false;
};

} // namespace x2
