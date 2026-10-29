// Output filters of the window (Phase 9b, docs/re-notes/modernisation-design.md): the picture (the faithful
// frame or the modernisation canvas) is scaled into the window through one of the VideoFilter passes.
//
// Everything is drawn with the SDL_Renderer API only: render-target textures, nearest/linear sampling, MOD and
// ADD blending, tiled textures and SDL_RenderGeometry. That runs on every SDL3 back end (Metal, Direct3D 11/12,
// OpenGL, Vulkan and the software renderer) with no shader sources, no shader cross-compiler and no new
// dependencies. The only CPU pass is the edge upscaler (modern::scale2x/scale3x, before upload).
//
//   nearest    the picture scaled with nearest-neighbour sampling (integer factors: the faithful look)
//   sharp      bilinear-sharp: integer pre-scale with nearest, then bilinear to the window (even pixels at
//              any window size, e.g. with the 4:3 aspect)
//   scanlines  the picture at an integer number of rows per Amiga line, darkened between lines by a beam
//              profile (MOD), brightened back (ADD), then bilinear to the window
//   crt        scanlines plus an aperture-grille shadow mask at window resolution, a bloom (a quarter-size
//              bilinear copy added on top) and, with curvature, a barrel-warped grid with a vignette
//   edge       the CPU edge-preserving upscale (already applied to the canvas), then sharp
#pragma once
#include "persist.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace x2 {

struct View {
    VideoFilter filter = VideoFilter::Nearest;
    bool aspect = false;  // 4:3 display: PAL lores pixels 16:15 wide
    bool integer = true;  // integer scale factor (vertical, with aspect)
    bool curvature = false;
};

// Where a picture of aw x ah Amiga pixels goes in an ow x oh output: centred, square pixels or 4:3, integer
// factors or the largest fit. (Square integer: exactly the faithful window's rectangle.)
SDL_FRect view_rect(int aw, int ah, const View& v, int ow, int oh);

class FilterChain {
public:
    ~FilterChain() { release(); }
    // The picture to show: RGBA8 w x h, `scale` canvas pixels per Amiga pixel.
    bool upload(SDL_Renderer* r, const uint8_t* rgba, int w, int h, int scale);
    // Clears the renderer's current target (ow x oh) and draws the uploaded picture into it.
    void draw(SDL_Renderer* r, const View& v, int ow, int oh);
    bool has_picture() const { return src_ != nullptr; }
    void release();

private:
    SDL_Texture* target(SDL_Renderer* r, SDL_Texture*& t, int w, int h);
    void sharp(SDL_Renderer* r, SDL_Texture* out, const SDL_FRect& d);
    void scanlines(SDL_Renderer* r, SDL_Texture* out, const SDL_FRect& d, const View& v);

    SDL_Texture* src_ = nullptr;    // the picture
    SDL_Texture* pre_ = nullptr;    // integer pre-scale
    SDL_Texture* lines_ = nullptr;  // scanline profile, 1 x (rows per line * lines)
    SDL_Texture* screen_ = nullptr; // crt: the picture at window resolution
    SDL_Texture* mask_ = nullptr;   // crt: aperture grille tile
    SDL_Texture* glow_ = nullptr;   // crt: bloom source
    int sw_ = 0, sh_ = 0, scale_ = 1;
    int lines_rows_ = 0, lines_h_ = 0;
    bool lines_crt_ = false;
};

// The filter output without a window: draws the picture through a FilterChain on SDL's software renderer into
// an ow x oh RGBA8 image (--present-shot, tests). False if SDL cannot create the surface or renderer.
bool render_offscreen(const uint8_t* rgba, int w, int h, int scale, const View& v, int ow, int oh,
                      std::vector<uint8_t>& out);

} // namespace x2
