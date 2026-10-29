// Replacement asset packs (Phase 9d, docs/ASSET-PACKS.md).
//
// The interpolator replays every captured draw command (modern/interp.h). An asset pack replaces single
// original graphics in that replay with RGBA pictures, keyed by the content of the original graphic as the game
// drew it, so a key does not depend on addresses, disk version or level module:
//   sprite / tile  the 4 plane blits of a cookie-cut bob or masked tile (A = mask, B = plane data) merged into one
//                  picture, or the single 4-plane copy of an opaque tile: FNV-1a 64 of (width, height, each pixel
//                  of the drawn mask's bounding box as "drawn" + colour index), plus the size. The bounding box
//                  makes the key independent of the blitter shift (x position).
//   far line       one 320-pixel line of the far layer (planes 0-1, as copied from the ring buffer).
// Palette overrides are keyed by the level's 16 original colours.
//
// AssetLibrary holds the loaded packs (directories or ZIP files with manifest.json and PNGs), with validation,
// bounded memory and safe path handling. AssetDumper writes every distinct original graphic a run draws as PNGs
// plus a manifest in the same format, for pack authors (user data: never distribute it).
#pragma once
#include "modern/draw_capture.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x2::modern {

// An RGBA picture with premultiplied alpha: R in the low byte, A in the high byte.
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};

enum class GfxKind : uint8_t { Sprite, Tile, Far };

// One original graphic drawn by a step.
struct Graphic {
    GfxKind kind = GfxKind::Sprite;
    uint32_t first = 0, last = 0;   // Step::blits indices of its blits (far: the blit of the line)
    uint32_t row = 0;               // far: the line's row within that blit
    bool cookie = false;            // drawn through a mask (else an opaque copy)
    int x = 0, y = 0, w = 0, h = 0; // Amiga pixels; y is the buffer line (negative: the gutter above it)
    uint64_t hash = 0;
    std::vector<uint8_t> pix;       // with keep_pixels: w*h, 0x10 | colour index where drawn, else 0
    // Where the blits start: the source data (mask and plane pointers), the pixel x of the first word plus the
    // shift, the line of the first row, and the number of lines drawn (sprites and tiles).
    uint64_t src = 0;
    int ox = 0, oline = 0, rows = 0;
};

// Bottom-clipped graphics. The game draws tiles and objects that cross the bottom of the playfield with fewer
// rows from the same source data, so each clipped height would be a key of its own. SourceTracker remembers,
// per source and level palette, the fullest graphic drawn from it; a later draw from the same source with fewer
// rows is the top part of that graphic, and gets its replacement clipped to the rows drawn.
class SourceTracker {
public:
    struct Full {
        uint64_t hash;
        int w, h, dx, dy, rows; // dx, dy: the graphic's top-left relative to (ox, oline)
    };
    // The fuller graphic drawn earlier from g's source, if g has fewer rows; then records g.
    const Full* fuller(const Graphic& g, const uint16_t* palette);
    void clear() { m_.clear(); }

private:
    std::unordered_map<uint64_t, Full> m_;
    Full found_{};
};

// The graphics among a step's blits: merged cookie-cut groups (4 consecutive plane blits sharing the mask),
// opaque 4-plane copies and far-layer lines. Partial groups (fewer than 4 planes, other minterms such as the
// hit flash) are not graphics; they always replay as the original.
void find_graphics(const Step& s, std::vector<Graphic>& out, bool keep_pixels);
std::string hash_hex(uint64_t h);                     // 16 lowercase hex digits
std::string graphic_key(uint64_t hash, int w, int h); // "0123456789abcdef-16x12"

// Limits applied while loading packs (all images together, per file, per picture).
struct PackLimits {
    size_t max_file = 32u << 20;       // bytes of one PNG or manifest.json
    size_t max_zip = 256u << 20;       // bytes of a ZIP pack, and of all its members unpacked
    int max_side = 4096;               // width or height of one picture
    size_t max_pixels = 8u << 20;      // pixels of one picture
    size_t max_total = 512u << 20;     // bytes of all decoded pictures of all packs
    size_t max_entries = 65536;        // graphics + far lines + palettes per pack
};

struct Replacement {
    const Image* img = nullptr;
    int scale = 1;      // picture pixels per Amiga pixel (1-8)
    int ax = 0, ay = 0; // anchor: the picture pixel placed on the original's top-left corner
};
struct FarRow {
    const Image* img = nullptr; // a strip, 320 * scale wide
    int scale = 1;
    int row = 0;                // the line's rows are row * scale .. (row + 1) * scale - 1
};
// Layer bits of PaletteOverride::layers: 1 << int(Layer) (modern/interp.h: Other 0, Far 1, Tiles 2, Stars 3,
// Object 4).
struct PaletteOverride {
    uint16_t match[16] = {};  // the level's COLOR00-15 (12-bit)
    uint32_t rgba[16] = {};   // replacement colours, 0xAABBGGRR
    uint8_t layers = 0x1F;
};

class AssetLibrary {
public:
    PackLimits limits;

    // Adds the pack at `path` (a directory or a .zip with manifest.json at its root or in one top folder). Its
    // entries override those of packs loaded before. Returns false if the pack cannot be used at all; problems
    // with single entries (missing or invalid PNG, bad key) are appended to `messages` and the entry is skipped.
    bool load(const std::string& path, std::vector<std::string>& messages);

    const Replacement* graphic(uint64_t hash, int w, int h) const;
    const FarRow* far_line(uint64_t hash) const;
    // The override for a step palette (COLOR00-15), the latest loaded pack first; null if none matches.
    const PaletteOverride* palette(const uint16_t* colors) const;

    bool empty() const { return graphics_.empty() && far_.empty() && palettes_.empty(); }
    size_t graphics() const { return graphics_.size(); }
    size_t far_lines() const { return far_.size(); }
    size_t palettes() const { return palettes_.size(); }
    size_t bytes() const { return bytes_; }
    const std::vector<std::string>& packs() const { return packs_; }

private:
    std::vector<std::unique_ptr<Image>> images_;
    std::unordered_map<std::string, Replacement> graphics_;
    std::unordered_map<uint64_t, FarRow> far_;
    std::vector<PaletteOverride> palettes_;
    std::vector<std::string> packs_;
    size_t bytes_ = 0;
};

// PNG decoding (8-bit and 16-bit RGB/RGBA/grey, palette and grey at 1-8 bits, tRNS; not interlaced) into
// premultiplied RGBA. The size is checked against the limits before anything is inflated.
bool decode_png(const std::vector<uint8_t>& file, Image& out, std::string& why, int max_side = 4096,
                size_t max_pixels = 8u << 20);
// Area-weighted resampling (premultiplied, so edges stay clean) by num/den: a pack picture at scale den drawn
// on a canvas at scale num. The size is rounded; integer up-scales are exact pixel replication.
void resample(const Image& src, int num, int den, Image& out);
// Relative path inside a pack: no absolute paths, drive letters, backslashes, empty, "." or ".." components.
bool safe_pack_path(const std::string& p);

// --dump-assets DIR: every distinct graphic of the gameplay steps it is given, as PNGs (the step's palette,
// transparent where the original is not drawn) in DIR/sprites, DIR/tiles and DIR/far (far lines in strips of
// up to 192), and DIR/manifest.json listing key, kind, size, uses, first frame and palette. The manifest is a
// valid asset pack at scale 1, so the dump can be loaded as a pack (it reproduces the original picture).
class AssetDumper {
public:
    bool open(const std::string& dir, std::string& why);
    void step(const Step& s);
    // Writes the PNGs and the manifest. Each graphic is written with the brightest palette it was drawn with
    // (not the black of a fade-in).
    bool finish(std::string& why);
    size_t sprites() const { return n_sprites_; }
    size_t tiles() const { return n_tiles_; }
    size_t far_lines() const { return far_.size(); }
    size_t palettes() const { return palettes_.size(); }
    size_t errors() const { return errors_; }
    static constexpr size_t kMaxGraphics = 50000;

private:
    struct Entry {
        GfxKind kind;
        int w, h;
        uint32_t uses = 0, first_frame = 0, palette = 0;
        std::string png;
        std::vector<uint8_t> pix;
    };
    struct FarLine {
        std::string key;
        uint32_t palette = 0;
        std::vector<uint8_t> pix;
    };
    uint32_t palette_index(const uint16_t* colors);

    std::string dir_;
    std::map<std::string, Entry> entries_;
    SourceTracker sources_;
    std::vector<std::array<uint16_t, 16>> palettes_;
    std::vector<int> brightness_;                    // per palette: the sum of its colour components
    std::unordered_map<uint64_t, uint32_t> far_seen_; // line -> index in far_
    std::vector<FarLine> far_;                       // in order of first appearance
    size_t n_sprites_ = 0, n_tiles_ = 0, errors_ = 0;
    bool open_ = false;
};

} // namespace x2::modern
