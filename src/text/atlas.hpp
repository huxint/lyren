#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gfx/renderer.hpp"
#include "text/fonts.hpp"

namespace lyren::text {

// Where a rasterized glyph or icon lives in the atlas, and its offset from the pen position (buffer px).
struct AtlasEntry {
    uint16_t x = 0, y = 0, w = 0, h = 0;
    int16_t left = 0, top = 0;
};

// R8 coverage atlas for glyphs and icons, shelf-packed. When full it grows once (1024² → 2048²), after that it is
// cleared; reset() tells the frame builder to start the frame again. Sampled linearly: entries drawn at their own size
// land on texel centres and stay exact, scaled ones (Painter::set_zoom) are filtered.
class Atlas {
public:
    Atlas(gfx::Renderer& renderer, Fonts& fonts);
    ~Atlas();

    // size in buffer pixels; subpixel is the quarter-pixel horizontal offset (0..3).
    const AtlasEntry& glyph(uint16_t face, uint32_t gid, float size, int subpixel);
    const AtlasEntry& icon(std::string_view name, int size);

    gfx::TextureId texture() const { return texture_; }
    uint32_t size() const { return size_; }
    void begin_frame() { reset_ = false; }
    bool reset() const { return reset_; }
    size_t entries() const { return glyphs_.size() + icons_.size(); }
    float fill() const { return float(next_y_) / float(size_); }

private:
    AtlasEntry add(int w, int h, int left, int top, const uint8_t* pixels, int pitch);
    bool place(int w, int h, uint16_t& x, uint16_t& y);
    void clear();

    struct Shelf {
        uint16_t y, h, x;
    };
    gfx::Renderer& renderer_;
    Fonts& fonts_;
    uint32_t size_ = 1024;
    gfx::TextureId texture_;
    std::vector<Shelf> shelves_;
    uint16_t next_y_ = 0;
    bool reset_ = false;
    std::unordered_map<uint64_t, AtlasEntry> glyphs_;
    std::unordered_map<std::string, AtlasEntry> icons_;
    std::vector<uint8_t> scratch_;
};

}  // namespace lyren::text
