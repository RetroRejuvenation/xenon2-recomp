#include "gfx/sdl_display.h"

#include <algorithm>

namespace x2 {

SdlDisplay::~SdlDisplay() {
    chain_.release();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
}

bool SdlDisplay::open(const std::string& title, int width, int height, int scale, bool fullscreen, bool aspect_4_3,
                      std::string& error) {
    w_ = width;
    h_ = height;
    aspect_ = aspect_4_3;
    int ww = width * scale, wh = aspect_4_3 ? width * scale * 3 / 4 : height * scale;
    window_ = SDL_CreateWindow(title.c_str(), ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) { error = SDL_GetError(); return false; }
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) { error = SDL_GetError(); return false; }
    vsync_ = SDL_SetRenderVSync(renderer_, 1);
    if (fullscreen) toggle_fullscreen();
    return true;
}

void SdlDisplay::toggle_fullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_);
}

void SdlDisplay::set_scale(int scale) {
    if (fullscreen_) return;
    SDL_SetWindowSize(window_, w_ * scale, aspect_ ? w_ * scale * 3 / 4 : h_ * scale);
}

void SdlDisplay::set_title(const std::string& title) { SDL_SetWindowTitle(window_, title.c_str()); }

float SdlDisplay::refresh_hz() const {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    return mode ? mode->refresh_rate : 0.0f;
}

int SdlDisplay::output_scale(const View& v) const {
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(renderer_, &ow, &oh);
    return std::max(1, int(view_rect(w_, h_, v, ow, oh).h) / h_);
}

void SdlDisplay::present(const uint8_t* rgba, int w, int h, int scale, const View& v) {
    if (!chain_.upload(renderer_, rgba, w, h, scale)) return;
    redraw(v);
}

void SdlDisplay::redraw(const View& v) {
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(renderer_, &ow, &oh);
    chain_.draw(renderer_, v, ow, oh);
    SDL_RenderPresent(renderer_);
}

void SdlDisplay::present_text(const std::vector<TextLine>& lines) {
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(renderer_, &ow, &oh);
    const int ch = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    int s = std::max(1, ow / (ch * 80));
    size_t cols = size_t(std::max(20, ow / (ch * s) - 2));
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_SetRenderScale(renderer_, float(s), float(s));
    float y = float(ch);
    for (const TextLine& l : lines) {
        SDL_SetRenderDrawColor(renderer_, Uint8(l.rgb >> 16), Uint8(l.rgb >> 8), Uint8(l.rgb), 255);
        std::string rest = l.text;
        do { // wrap at the last space that fits, continuation lines indented
            size_t n = rest.size();
            if (n > cols) {
                size_t sp = rest.rfind(' ', cols);
                n = sp != std::string::npos && sp > cols / 3 ? sp : cols;
            }
            SDL_RenderDebugText(renderer_, float(ch), y, rest.substr(0, n).c_str());
            y += ch * 1.25f;
            rest = rest.size() > n ? "  " + rest.substr(rest[n] == ' ' ? n + 1 : n) : "";
        } while (!rest.empty());
    }
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_RenderPresent(renderer_);
}

} // namespace x2
