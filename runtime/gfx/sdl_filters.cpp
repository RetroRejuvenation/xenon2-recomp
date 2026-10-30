// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "gfx/sdl_filters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace x2 {

namespace {
constexpr float kPixelAspect = 16.0f / 15.0f; // 320x256 on a 4:3 display
constexpr int kMaxTexture = 8192;

void destroy(SDL_Texture*& t) {
    if (t) SDL_DestroyTexture(t);
    t = nullptr;
}

void blend(SDL_Texture* t, SDL_BlendMode mode, Uint8 mod = 255) {
    SDL_SetTextureBlendMode(t, mode);
    SDL_SetTextureColorMod(t, mod, mod, mod);
}
} // namespace

SDL_FRect view_rect(int aw, int ah, const View& v, int ow, int oh) {
    if (!v.aspect) {
        if (v.integer) {
            int s = std::max(1, std::min(ow / aw, oh / ah));
            return {float((ow - aw * s) / 2), float((oh - ah * s) / 2), float(aw * s), float(ah * s)};
        }
        float s = std::min(float(ow) / float(aw), float(oh) / float(ah));
        return {(float(ow) - float(aw) * s) / 2, (float(oh) - float(ah) * s) / 2, float(aw) * s, float(ah) * s};
    }
    const float wide = float(aw) * kPixelAspect;
    float dh = std::min(float(oh), float(ow) * float(ah) / wide);
    if (v.integer) dh = float(std::max(1, int(dh) / ah) * ah);
    const float dw = std::round(dh * wide / float(ah));
    return {std::floor((float(ow) - dw) / 2), std::floor((float(oh) - dh) / 2), dw, dh};
}

void FilterChain::release() {
    for (SDL_Texture** t : {&src_, &pre_, &lines_, &screen_, &mask_, &glow_}) destroy(*t);
    sw_ = sh_ = 0;
    lines_rows_ = lines_h_ = 0;
}

bool FilterChain::upload(SDL_Renderer* r, const uint8_t* rgba, int w, int h, int scale) {
    if (!src_ || w != sw_ || h != sh_) {
        destroy(src_);
        src_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!src_) return false;
        sw_ = w;
        sh_ = h;
    }
    scale_ = std::max(1, scale);
    return SDL_UpdateTexture(src_, nullptr, rgba, w * 4);
}

SDL_Texture* FilterChain::target(SDL_Renderer* r, SDL_Texture*& t, int w, int h) {
    float tw = 0, th = 0;
    if (t && SDL_GetTextureSize(t, &tw, &th) && int(tw) == w && int(th) == h) return t;
    destroy(t);
    t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    return t;
}

void FilterChain::draw(SDL_Renderer* r, const View& v, int ow, int oh) {
    SDL_Texture* out = SDL_GetRenderTarget(r);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    if (!src_) return;
    const SDL_FRect d = view_rect(sw_ / scale_, sh_ / scale_, v, ow, oh);
    switch (v.filter) {
    case VideoFilter::Sharp:
    case VideoFilter::Edge: sharp(r, out, d); break;
    case VideoFilter::Scanlines:
    case VideoFilter::Crt: scanlines(r, out, d, v); break;
    default:
        SDL_SetTextureScaleMode(src_, SDL_SCALEMODE_NEAREST);
        blend(src_, SDL_BLENDMODE_NONE);
        SDL_RenderTexture(r, src_, nullptr, &d);
        break;
    }
}

void FilterChain::sharp(SDL_Renderer* r, SDL_Texture* out, const SDL_FRect& d) {
    // Largest integer pre-scale that does not exceed the window rectangle; bilinear covers the rest.
    int k = std::max(1, std::min(int(d.w) / sw_, int(d.h) / sh_));
    while (k > 1 && std::max(sw_, sh_) * k > kMaxTexture) --k;
    blend(src_, SDL_BLENDMODE_NONE);
    if (float(sw_ * k) == d.w && float(sh_ * k) == d.h) { // already an integer fit
        SDL_SetTextureScaleMode(src_, SDL_SCALEMODE_NEAREST);
        SDL_RenderTexture(r, src_, nullptr, &d);
        return;
    }
    SDL_Texture* pre = target(r, pre_, sw_ * k, sh_ * k);
    if (!pre) return;
    SDL_SetRenderTarget(r, pre);
    SDL_SetTextureScaleMode(src_, SDL_SCALEMODE_NEAREST);
    SDL_RenderTexture(r, src_, nullptr, nullptr);
    SDL_SetRenderTarget(r, out);
    SDL_SetTextureScaleMode(pre, SDL_SCALEMODE_LINEAR);
    blend(pre, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(r, pre, nullptr, &d);
}

void FilterChain::scanlines(SDL_Renderer* r, SDL_Texture* out, const SDL_FRect& d, const View& v) {
    const bool crt = v.filter == VideoFilter::Crt;
    const int aw = sw_ / scale_, ah = sh_ / scale_;
    // Rows per Amiga line: a multiple of the canvas scale near the window's, at least 3.
    int m = scale_ * std::max(1, int(std::lround(d.h / float(ah) / float(scale_))));
    while (m < 3) m += scale_;
    while (m > scale_ && std::max(aw, ah) * m > kMaxTexture) m -= scale_;
    SDL_Texture* pre = target(r, pre_, aw * m, ah * m);
    if (!pre) return;
    if (!lines_ || lines_rows_ != m || lines_h_ != ah || lines_crt_ != crt) {
        // Beam profile across one line: a Gaussian between the floor (gap between lines) and full brightness.
        destroy(lines_);
        lines_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, ah * m);
        if (!lines_) return;
        const float floor = crt ? 0.30f : 0.45f, sigma = crt ? 0.28f : 0.32f;
        std::vector<uint32_t> px(size_t(ah * m));
        for (int y = 0; y < ah * m; ++y) {
            const float p = (float(y % m) + 0.5f) / float(m) - 0.5f;
            const float w = floor + (1.0f - floor) * std::exp(-p * p / (2 * sigma * sigma));
            const uint32_t c = uint32_t(std::lround(255.0f * w));
            px[size_t(y)] = 0xFF000000u | c << 16 | c << 8 | c;
        }
        SDL_UpdateTexture(lines_, nullptr, px.data(), 4);
        SDL_SetTextureScaleMode(lines_, SDL_SCALEMODE_NEAREST);
        lines_rows_ = m;
        lines_h_ = ah;
        lines_crt_ = crt;
    }
    SDL_SetRenderTarget(r, pre);
    SDL_SetTextureScaleMode(src_, SDL_SCALEMODE_NEAREST);
    blend(src_, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(r, src_, nullptr, nullptr);
    blend(lines_, SDL_BLENDMODE_MOD);
    SDL_RenderTexture(r, lines_, nullptr, nullptr);
    SDL_SetTextureScaleMode(pre, SDL_SCALEMODE_LINEAR);

    if (!crt) {
        SDL_SetRenderTarget(r, out);
        blend(pre, SDL_BLENDMODE_NONE);
        SDL_RenderTexture(r, pre, nullptr, &d);
        blend(pre, SDL_BLENDMODE_ADD, 70); // give back some of the brightness the gaps took
        SDL_RenderTexture(r, pre, nullptr, &d);
        blend(pre, SDL_BLENDMODE_NONE);
        return;
    }

    // CRT: compose at window resolution, then place (flat or curved).
    const int dw = std::max(1, int(std::lround(d.w))), dh = std::max(1, int(std::lround(d.h)));
    SDL_Texture* screen = target(r, screen_, dw, dh);
    SDL_Texture* glow = target(r, glow_, std::max(1, aw / 4), std::max(1, ah / 4));
    if (!screen || !glow) return;
    if (!mask_) {
        // Aperture grille: each output column passes mostly one of red, green, blue.
        mask_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 3, 1);
        if (!mask_) return;
        const uint32_t px[3] = {0xFFFFB4B4u, 0xFFB4FFB4u, 0xFFB4B4FFu};
        SDL_UpdateTexture(mask_, nullptr, px, 12);
        SDL_SetTextureScaleMode(mask_, SDL_SCALEMODE_NEAREST);
    }
    SDL_SetRenderTarget(r, glow);
    SDL_SetTextureScaleMode(src_, SDL_SCALEMODE_LINEAR);
    SDL_RenderTexture(r, src_, nullptr, nullptr);
    SDL_SetRenderTarget(r, screen);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    blend(pre, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(r, pre, nullptr, nullptr);
    blend(pre, SDL_BLENDMODE_ADD, 110);
    SDL_RenderTexture(r, pre, nullptr, nullptr);
    blend(mask_, SDL_BLENDMODE_MOD);
    SDL_RenderTextureTiled(r, mask_, nullptr, 1.0f, nullptr);
    SDL_SetTextureScaleMode(glow, SDL_SCALEMODE_LINEAR);
    blend(glow, SDL_BLENDMODE_ADD, 72);
    SDL_RenderTexture(r, glow, nullptr, nullptr);
    blend(pre, SDL_BLENDMODE_NONE);

    SDL_SetRenderTarget(r, out);
    SDL_SetTextureScaleMode(screen, v.curvature ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    blend(screen, SDL_BLENDMODE_NONE);
    const SDL_FRect dst = {d.x, d.y, float(dw), float(dh)};
    if (!v.curvature) {
        SDL_RenderTexture(r, screen, nullptr, &dst);
        return;
    }
    // Barrel: a grid in texture space, each vertex pulled towards the centre by 1 / (1 + k r^2) (scaled so the
    // edge midpoints stay put), darkened towards the corners.
    constexpr int kX = 40, kY = 30;
    constexpr float k = 0.10f;
    std::vector<SDL_Vertex> vx;
    vx.reserve(size_t((kX + 1) * (kY + 1)));
    for (int j = 0; j <= kY; ++j)
        for (int i = 0; i <= kX; ++i) {
            const float u = float(i) / kX, w = float(j) / kY;
            const float px = 2 * u - 1, py = 2 * w - 1, r2 = px * px + py * py;
            const float s = (1 + k) / (1 + k * r2);
            const float shade = 1.0f - 0.18f * r2 / 2;
            SDL_Vertex vert;
            vert.position = {dst.x + (0.5f + 0.5f * px * s) * dst.w, dst.y + (0.5f + 0.5f * py * s) * dst.h};
            vert.color = {shade, shade, shade, 1.0f};
            vert.tex_coord = {u, w};
            vx.push_back(vert);
        }
    std::vector<int> idx;
    idx.reserve(size_t(kX * kY * 6));
    for (int j = 0; j < kY; ++j)
        for (int i = 0; i < kX; ++i) {
            const int a = j * (kX + 1) + i, b = a + 1, c = a + kX + 1, e = c + 1;
            idx.insert(idx.end(), {a, b, c, b, e, c});
        }
    SDL_RenderGeometry(r, screen, vx.data(), int(vx.size()), idx.data(), int(idx.size()));
}

bool render_offscreen(const uint8_t* rgba, int w, int h, int scale, const View& v, int ow, int oh,
                      std::vector<uint8_t>& out) {
    SDL_Surface* surf = SDL_CreateSurface(ow, oh, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return false;
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(surf);
    if (!r) { SDL_DestroySurface(surf); return false; }
    bool ok;
    {
        FilterChain chain;
        ok = chain.upload(r, rgba, w, h, scale);
        if (ok) chain.draw(r, v, ow, oh);
        ok = ok && SDL_FlushRenderer(r);
    }
    if (ok) {
        out.resize(size_t(ow) * size_t(oh) * 4);
        for (int y = 0; y < oh; ++y)
            std::memcpy(&out[size_t(y) * size_t(ow) * 4], static_cast<const uint8_t*>(surf->pixels) + size_t(y) * size_t(surf->pitch),
                        size_t(ow) * 4);
    }
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surf);
    return ok;
}

} // namespace x2
