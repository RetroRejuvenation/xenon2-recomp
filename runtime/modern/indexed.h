// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// The indexed frame: what asset packs match screens and the status panel on (docs/re-notes/asset-packs-screens.md).
//
// Every game draws its screens in palette indices (Amiga: 32 colour registers, ST: 16), and the colours behind the indices
// change with fades and copper lists, so identity is taken from the indices, and the colours are used only to follow a fade.
// The frame is the display window (320 x 200): the Amiga's inside the 320 x 256 faithful frame, the ST's the whole picture.
#pragma once
#include <cstddef>
#include <cstdint>

namespace x2 {
class Video;
}

namespace x2::modern {

struct IndexedFrame {
    int w = 0, h = 0;
    int colours = 0;                    // palette entries per line (32 Amiga, 16 ST)
    const uint8_t* index = nullptr;     // w x h colour indices, `stride` bytes per row
    int stride = 0;
    const uint32_t* pal = nullptr;      // h x colours: the palette each line starts with, 0x00RRGGBB
    const uint32_t* rgba = nullptr;     // the faithful pixels of the same rectangle (R | G << 8 | B << 16 | A << 24)
    int rgba_stride = 0;                // in pixels

    bool valid() const { return index && pal && rgba && w > 0 && h > 0 && colours > 0; }
    uint8_t at(int x, int y) const { return index[size_t(y) * size_t(stride) + size_t(x)]; }
    // The colour the line's palette gives the pixel, as 0x00RRGGBB.
    uint32_t colour(int x, int y) const { return pal[size_t(y) * size_t(colours) + (at(x, y) % colours)]; }
};

// The window of the Amiga video model's front frame (window rows kFrameTop..+199), or an invalid frame when the video model
// has no index capture or does not hold a frame yet.
IndexedFrame amiga_indexed(const Video& v);

} // namespace x2::modern
