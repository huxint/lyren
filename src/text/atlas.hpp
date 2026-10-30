#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/string_map.hpp"
#include "gfx/renderer.hpp"
#include "text/fonts.hpp"

namespace lyren::text {

    // Where a rasterized glyph or icon lives in the atlas, and its offset from the pen position (buffer px).
    struct AtlasEntry {
        uint16_t x = 0, y = 0, w = 0, h = 0;
        int16_t left = 0, top = 0;
        bool color = false; // in the colour sheet
    };

    // Glyphs and icons, shelf-packed: coverage in an R8 sheet, colour emoji in a premultiplied BGRA sheet made when the
    // first one is drawn. When a sheet is full it grows once (R8 1024² → 2048², BGRA 512² → 1024²), after that both are
    // cleared; reset() tells the frame builder to start the frame again. Sampled linearly: entries drawn at their own
    // size land on texel centres and stay exact, scaled ones (Painter::set_zoom) are filtered.
    class Atlas {
    public:
        Atlas(gfx::Renderer &renderer, Fonts &fonts);
        ~Atlas();

        // size in buffer pixels; subpixel is the quarter-pixel horizontal offset (0..3).
        const AtlasEntry &glyph(uint16_t face, uint32_t gid, float size, int subpixel);
        const AtlasEntry &icon(std::string_view name, int size);

        gfx::TextureId texture() const {
            return mono_.texture;
        }
        uint32_t size() const {
            return mono_.size;
        }
        gfx::TextureId color_texture() const {
            return color_.texture;
        }
        uint32_t color_size() const {
            return color_.size;
        }
        void begin_frame() {
            reset_ = false;
        }
        bool reset() const {
            return reset_;
        }

    private:
        struct Shelf {
            uint16_t y, h, x;
        };
        struct Sheet {
            VkFormat format;
            uint32_t size, max; // size is 0 until the sheet is made
            gfx::TextureId texture = 0;
            std::vector<Shelf> shelves;
            uint16_t next_y = 0;
            bool place(int w, int h, uint16_t &x, uint16_t &y);
        };
        AtlasEntry add(Sheet &sheet, int w, int h, int left, int top, const uint8_t *pixels, int pitch);
        AtlasEntry color_glyph(Face &f, uint32_t gid, float size);
        void clear(Sheet &full);

        gfx::Renderer &renderer_;
        Fonts &fonts_;
        Sheet mono_{VK_FORMAT_R8_UNORM, 1024, 2048}, color_{VK_FORMAT_B8G8R8A8_UNORM, 0, 1024};
        bool reset_ = false;
        std::unordered_map<uint64_t, AtlasEntry> glyphs_;
        CacheMap<AtlasEntry> icons_;
        std::vector<uint8_t> scratch_;
    };

} // namespace lyren::text
