#pragma once
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

    enum Feature : uint8_t {
        tabular_nums = 1
    };

    // Shapes in font units so one result serves every size. Results are cached by text.
    class Shaper {
    public:
        explicit Shaper(Fonts &fonts);
        ~Shaper();

        const Shaped &shape(std::string_view text, uint8_t features = 0);
        // Drops cached results not used for a while once the cache is large.
        void end_frame();

    private:
        struct Entry {
            Shaped shaped;
            uint64_t used;
        };
        Fonts &fonts_;
        hb_buffer_t *buf_;
        CacheMap<Entry> cache_;
        size_t glyphs_ = 0;
        uint64_t frame_ = 0;
    };

} // namespace lyren::text
