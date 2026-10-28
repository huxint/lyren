#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <harfbuzz/hb.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lyren::text {

    // One font file mapped once; FreeType and HarfBuzz both read the mapping, so font data stays in clean,
    // shared page-cache pages instead of the heap.
    // Which code points a font covers, copied out of fontconfig so it can be shut down.
    struct Coverage {
        std::vector<uint32_t> pages; // cp >> 8, sorted
        std::vector<std::array<uint32_t, 8>> bits;
        bool has(char32_t cp) const;
    };

    struct Face {
        std::string path;
        int index = 0;
        Coverage coverage;
        const uint8_t *data = nullptr;
        size_t size = 0;
        FT_Face ft = nullptr;
        hb_font_t *hb = nullptr;
        float upem = 1000;
        float ascender = 0.8f, descender = 0.2f; // in em
        bool color = false;                      // colour bitmaps (emoji), which have their own atlas
    };

    class Fonts {
    public:
        // Resolves the family's fallback chain with fontconfig, then shuts fontconfig down.
        explicit Fonts(const std::string &family);
        ~Fonts();
        Fonts(const Fonts &) = delete;
        Fonts &operator=(const Fonts &) = delete;

        // First face in the chain that covers cp (0 when none does). emoji: cp is shown as an emoji, which the colour
        // face takes when it covers it.
        uint16_t face_for(char32_t cp, bool emoji = false) const;
        // Opens the face on first use.
        Face &face(uint16_t i);
        // Drops the mapped font pages from this process's RSS; they stay in the page cache.
        void trim();

    private:
        void open(Face &f);
        FT_Library ft_ = nullptr;
        std::vector<Face> faces_;
        int color_ = -1; // the colour emoji face
    };

} // namespace lyren::text
