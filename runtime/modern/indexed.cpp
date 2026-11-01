// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "modern/indexed.h"

#include "gfx/video.h"
#include "modern/layout.h"

namespace x2::modern {

IndexedFrame amiga_indexed(const Video& v) {
    IndexedFrame f;
    const ScreenGeometry& g = kAmigaScreen;
    if (!v.index_capture() || v.front_index().size() != size_t(Video::kFbWidth) * Video::kFbHeight ||
        v.front_palettes().size() != size_t(Video::kFbHeight) * Video::kIndexColours)
        return f;
    f.w = g.w;
    f.h = g.h;
    f.colours = Video::kIndexColours;
    f.stride = Video::kFbWidth;
    f.index = v.front_index().data() + size_t(g.y) * Video::kFbWidth + size_t(g.x);
    f.pal = v.front_palettes().data() + size_t(g.y) * Video::kIndexColours;
    f.rgba = reinterpret_cast<const uint32_t*>(v.front().data()) + size_t(g.y) * Video::kFbWidth + size_t(g.x);
    f.rgba_stride = Video::kFbWidth;
    return f;
}

} // namespace x2::modern
