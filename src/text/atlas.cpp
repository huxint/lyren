#include "text/atlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "text/icons.hpp"

namespace lyren::text {

constexpr uint32_t max_size = 2048;

Atlas::Atlas(gfx::Renderer& renderer, Fonts& fonts)
    : renderer_(renderer), fonts_(fonts), texture_(renderer.create_texture(size_, size_, VK_FORMAT_R8_UNORM, false)) {}

Atlas::~Atlas() { renderer_.destroy_texture(texture_); }

bool Atlas::place(int w, int h, uint16_t& x, uint16_t& y) {
    Shelf* best = nullptr;
    for (auto& s : shelves_)
        if (h <= s.h && s.h <= h + h / 4 + 2 && s.x + w <= int(size_) && (!best || s.h < best->h)) best = &s;
    if (!best) {
        int sh = (h + 3) & ~3;
        if (next_y_ + sh > int(size_)) return false;
        shelves_.push_back({next_y_, uint16_t(sh), 0});
        next_y_ += sh;
        best = &shelves_.back();
    }
    x = best->x, y = best->y;
    best->x += w;
    return true;
}

void Atlas::clear() {
    if (size_ < max_size) {
        renderer_.destroy_texture(texture_);
        size_ = max_size;
        texture_ = renderer_.create_texture(size_, size_, VK_FORMAT_R8_UNORM, false);
    }
    shelves_.clear();
    next_y_ = 0;
    glyphs_.clear();
    icons_.clear();
    reset_ = true;
}

AtlasEntry Atlas::add(int w, int h, int left, int top, const uint8_t* pixels, int pitch) {
    if (w == 0 || h == 0) return {.left = int16_t(left), .top = int16_t(top)};
    // One pixel of empty border keeps neighbours from bleeding in.
    int pw = w + 2, ph = h + 2;
    uint16_t x, y;
    if (!place(pw, ph, x, y)) {
        clear();
        place(pw, ph, x, y);
    }
    scratch_.assign(size_t(pw) * ph, 0);
    for (int r = 0; r < h; ++r) std::memcpy(&scratch_[size_t(r + 1) * pw + 1], pixels + ptrdiff_t(r) * pitch, size_t(w));
    renderer_.upload(texture_, x, y, uint32_t(pw), uint32_t(ph), scratch_.data(), uint32_t(pw));
    return {uint16_t(x + 1), uint16_t(y + 1), uint16_t(w), uint16_t(h), int16_t(left), int16_t(top)};
}

const AtlasEntry& Atlas::glyph(uint16_t face, uint32_t gid, float size, int subpixel) {
    uint32_t q = uint32_t(std::lround(size * 4));
    uint64_t key = uint64_t(face) << 50 | uint64_t(gid) << 24 | uint64_t(q) << 2 | uint64_t(subpixel);
    if (auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;

    AtlasEntry e;
    Face& f = fonts_.face(face);
    if (f.ft) {
        FT_Set_Char_Size(f.ft, 0, FT_F26Dot6(q * 16), 0, 0);
        if (!FT_Load_Glyph(f.ft, gid, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP)) {
            FT_GlyphSlot slot = f.ft->glyph;
            if (slot->format == FT_GLYPH_FORMAT_OUTLINE) FT_Outline_Translate(&slot->outline, subpixel * 16, 0);
            if (!FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT))
                e = add(int(slot->bitmap.width), int(slot->bitmap.rows), slot->bitmap_left, slot->bitmap_top, slot->bitmap.buffer,
                        slot->bitmap.pitch);
        }
    }
    return glyphs_.emplace(key, e).first->second;
}

const AtlasEntry& Atlas::icon(std::string_view name, int size) {
    std::string key = std::format("{}@{}", name, size);
    if (auto it = icons_.find(key); it != icons_.end()) return it->second;
    std::vector<uint8_t> cov;
    AtlasEntry e;
    if (rasterize_icon(name, size, cov)) e = add(size, size, 0, size, cov.data(), size);
    return icons_.emplace(std::move(key), e).first->second;
}

}  // namespace lyren::text
