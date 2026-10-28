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
    bool vsync() const { return vsync_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int w_ = 0, h_ = 0;
    bool aspect_ = false, fullscreen_ = false, vsync_ = false;
};

} // namespace x2
