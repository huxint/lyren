#include "text/atlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gfx/image.hpp"
#include "text/icons.hpp"

namespace lyren::text {

    Atlas::Atlas(gfx::Renderer &renderer, Fonts &fonts)
    : renderer_(renderer),
      fonts_(fonts) {
        mono_.texture = renderer.create_texture(mono_.size, mono_.size, mono_.format, true);
    }

    Atlas::~Atlas() {
        for (const Sheet *s : {&mono_, &color_}) {
            if (s->size) {
                renderer_.destroy_texture(s->texture);
            }
        }
    }

    bool Atlas::Sheet::place(int w, int h, uint16_t &x, uint16_t &y) {
        Shelf *best = nullptr;
        for (auto &s : shelves) {
            if (h <= s.h && s.h <= h + h / 4 + 2 && s.x + w <= static_cast<int>(size) && (!best || s.h < best->h)) {
                best = &s;
            }
        }
        if (!best) {
            int sh = (h + 3) & ~3;
            if (next_y + sh > static_cast<int>(size)) {
                return false;
            }
            shelves.push_back({next_y, static_cast<uint16_t>(sh), 0});
            next_y += sh;
            best = &shelves.back();
        }
        x = best->x, y = best->y;
        best->x += w;
        return true;
    }

    void Atlas::clear(Sheet &full) {
        if (full.size < full.max) {
            renderer_.destroy_texture(full.texture);
            full.size = full.max;
            full.texture = renderer_.create_texture(full.size, full.size, full.format, true);
        }
        full.shelves.clear();
        full.next_y = 0;
        const bool is_color = &full == &color_;
        std::erase_if(glyphs_, [is_color](const auto &kv) {
            return kv.second.color == is_color;
        });
        if (!is_color) {
            icons_.clear();
        }
        reset_ = true;
    }

    AtlasEntry Atlas::add(Sheet &sheet, int w, int h, int left, int top, const uint8_t *pixels, int pitch) {
        if (w == 0 || h == 0) {
            return {.left = static_cast<int16_t>(left), .top = static_cast<int16_t>(top)};
        }
        // One pixel of empty border keeps neighbours from bleeding in.
        const size_t texel = sheet.format == VK_FORMAT_R8_UNORM ? 1 : 4;
        int pw = w + 2, ph = h + 2;
        uint16_t x = 0, y = 0;
        if (!sheet.place(pw, ph, x, y)) {
            clear(sheet);
            if (!sheet.place(pw, ph, x, y)) {
                return {};
            }
        }
        scratch_.assign(static_cast<size_t>(pw) * ph * texel, 0);
        for (int r = 0; r < h; ++r) {
            std::memcpy(&scratch_[(static_cast<size_t>(r + 1) * pw + 1) * texel], pixels + static_cast<ptrdiff_t>(r) * pitch, static_cast<size_t>(w) * texel);
        }
        renderer_.upload(sheet.texture, x, y, static_cast<uint32_t>(pw), static_cast<uint32_t>(ph), scratch_.data(), static_cast<uint32_t>(pw * texel));
        return {static_cast<uint16_t>(x + 1), static_cast<uint16_t>(y + 1), static_cast<uint16_t>(w), static_cast<uint16_t>(h),
                static_cast<int16_t>(left),   static_cast<int16_t>(top),    &sheet == &color_};
    }

    // Emoji come as bitmap strikes: the smallest one at least size (or the largest there is), shrunk to size.
    AtlasEntry Atlas::color_glyph(Face &f, uint32_t gid, float size) {
        if (!f.ft || f.ft->num_fixed_sizes == 0) {
            return {};
        }
        auto ppem = [&](int i) {
            return static_cast<float>(f.ft->available_sizes[i].y_ppem) / 64;
        };
        int strike = 0;
        for (int i = 1; i < f.ft->num_fixed_sizes; ++i) {
            const bool fits = ppem(i) >= size, best_fits = ppem(strike) >= size;
            if (fits ? !best_fits || ppem(i) < ppem(strike) : !best_fits && ppem(i) > ppem(strike)) {
                strike = i;
            }
        }
        if (FT_Select_Size(f.ft, strike) || FT_Load_Glyph(f.ft, gid, FT_LOAD_COLOR) || f.ft->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA) {
            return {};
        }
        if (!color_.size) {
            color_.size = 512;
            color_.texture = renderer_.create_texture(color_.size, color_.size, color_.format, true);
        }
        const FT_GlyphSlot slot = f.ft->glyph;
        const FT_Bitmap &b = slot->bitmap;
        const float scale = std::min(1.f, size / ppem(strike));
        const int w = std::max(1, static_cast<int>(std::lround(b.width * scale))), h = std::max(1, static_cast<int>(std::lround(b.rows * scale)));
        const int left = static_cast<int>(std::lround(slot->bitmap_left * scale));
        const int top = static_cast<int>(std::lround(slot->bitmap_top * scale));
        if (w == static_cast<int>(b.width) && h == static_cast<int>(b.rows)) {
            return add(color_, w, h, left, top, b.buffer, b.pitch);
        }
        gfx::Image src{b.width, b.rows, std::vector<uint8_t>(static_cast<size_t>(b.width) * b.rows * 4)};
        for (uint32_t y = 0; y < b.rows; ++y) {
            std::memcpy(src.px.data() + static_cast<size_t>(y) * b.width * 4, b.buffer + static_cast<ptrdiff_t>(y) * b.pitch, static_cast<size_t>(b.width) * 4);
        }
        const gfx::Image dst = gfx::downscale(src, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        return add(color_, w, h, left, top, dst.px.data(), w * 4);
    }

    const AtlasEntry &Atlas::glyph(uint16_t face, uint32_t gid, float size, int subpixel) {
        Face &f = fonts_.face(face);
        if (f.color) {
            subpixel = 0; // bitmaps do not shift by a fraction of a pixel
        }
        uint32_t q = static_cast<uint32_t>(std::lround(size * 4));
        uint64_t key = static_cast<uint64_t>(face) << 50 | static_cast<uint64_t>(gid) << 24 | static_cast<uint64_t>(q) << 2 | static_cast<uint64_t>(subpixel);
        if (auto it = glyphs_.find(key); it != glyphs_.end()) {
            return it->second;
        }

        AtlasEntry e;
        if (f.color) {
            e = color_glyph(f, gid, static_cast<float>(q) / 4);
        } else if (f.ft) {
            FT_Set_Char_Size(f.ft, 0, FT_F26Dot6(q * 16), 0, 0);
            if (!FT_Load_Glyph(f.ft, gid, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP)) {
                FT_GlyphSlot slot = f.ft->glyph;
                if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
                    FT_Outline_Translate(&slot->outline, subpixel * 16, 0);
                }
                if (!FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT)) {
                    e = add(mono_, static_cast<int>(slot->bitmap.width), static_cast<int>(slot->bitmap.rows), slot->bitmap_left, slot->bitmap_top, slot->bitmap.buffer,
                            slot->bitmap.pitch);
                }
            }
        }
        return glyphs_.emplace(key, e).first->second;
    }

    const AtlasEntry &Atlas::icon(std::string_view name, int size) {
        CacheKey::View key{name, static_cast<uint32_t>(size)};
        if (auto it = icons_.find(key); it != icons_.end()) {
            return it->second;
        }
        std::vector<uint8_t> cov;
        AtlasEntry e;
        if (rasterize_icon(name, size, cov)) {
            e = add(mono_, size, size, 0, size, cov.data(), size);
        }
        return icons_.emplace(CacheKey(key), e).first->second;
    }

} // namespace lyren::text
