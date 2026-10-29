#include "gfx/sdl_display.h"

#include <algorithm>

namespace x2 {

SdlDisplay::~SdlDisplay() {
    if (texture_) SDL_DestroyTexture(texture_);
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
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture_) { error = SDL_GetError(); return false; }
    SDL_SetTextureScaleMode(texture_, aspect_4_3 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
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

void SdlDisplay::present(const std::vector<uint8_t>& rgba) {
    SDL_UpdateTexture(texture_, nullptr, rgba.data(), w_ * 4);
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(renderer_, &ow, &oh);
    SDL_FRect dst;
    if (aspect_) {
        // Fit a 4:3 picture (PAL lores pixels are slightly wider than tall).
        float dw = std::min(float(ow), oh * 4.0f / 3.0f), dh = dw * 3.0f / 4.0f;
        dst = {(ow - dw) / 2, (oh - dh) / 2, dw, dh};
    } else {
        int s = std::max(1, std::min(ow / w_, oh / h_));
        dst = {float((ow - w_ * s) / 2), float((oh - h_ * s) / 2), float(w_ * s), float(h_ * s)};
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
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
