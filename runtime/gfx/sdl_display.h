// SDL3 window presenting the Video framebuffer: nearest-neighbour integer scaling of square pixels (default) or
// a 4:3 fit (--aspect), letterboxed, with vsync. Pacing is done by the caller (FramePacer), not by vsync.
#pragma once
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
    void present(const std::vector<uint8_t>& rgba);
    void toggle_fullscreen();
    // Resizes the window to `scale` times the picture (ignored while fullscreen).
    void set_scale(int scale);
    bool fullscreen() const { return fullscreen_; }
    bool vsync() const { return vsync_; }
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
    SDL_Texture* texture_ = nullptr;
    int w_ = 0, h_ = 0;
    bool aspect_ = false, fullscreen_ = false, vsync_ = false;
};

} // namespace x2
