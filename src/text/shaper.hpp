#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/string_map.hpp"
#include "text/fonts.hpp"

namespace lyren::text {

    // 16 bytes: OpenType glyph ids are 16-bit, so a whole run packs four glyphs to a cache line.
    struct Glyph {
        float x;          // pen position in em from the start of the text
        float y;          // vertical offset in em (up)
        uint32_t cluster; // byte offset of the source text
        uint16_t gid;
        uint16_t face;
    };
    static_assert(sizeof(Glyph) == 16);

    struct Shaped {
        std::vector<Glyph> glyphs;
        float width = 0; // em
    };

    // tabular_nums is an OpenType feature; medium and semibold pick the weight (at most one of the two).
    enum Feature : uint8_t {
        tabular_nums = 1,
        medium = 2,
        semibold = 4
    };

    // Shapes in font units so one result serves every size. Results are cached by text.
    class Shaper {
    public:
        enum class Surface : uint8_t {
            window,
            overlay
        };

        explicit Shaper(Fonts &fonts);
        ~Shaper();

        const Shaped &shape(std::string_view text, uint8_t features = 0);
        void begin_frame(Surface surface);
        void release_surface(Surface surface);

    private:
        struct Entry {
            Shaped shaped;
            std::array<uint32_t, 2> used{};
        };
        Fonts &fonts_;
        hb_buffer_t *buf_;
        CacheMap<Entry> cache_;
        static constexpr size_t glyph_budget = 60'000;
        size_t glyphs_ = 0, next_sweep_ = glyph_budget;
        uint32_t generation_ = 1;
        size_t surface_ = 0;
    };

} // namespace lyren::text
