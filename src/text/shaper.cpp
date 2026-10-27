#include "text/shaper.hpp"

#include <array>

#include "core/utf8.hpp"

namespace lyren::text {

    Shaper::Shaper(Fonts &fonts)
    : fonts_(fonts),
      buf_(hb_buffer_create()) {}
    Shaper::~Shaper() {
        hb_buffer_destroy(buf_);
    }

    // Characters that should stay in the surrounding run whatever font covers them.
    static bool joins_run(char32_t cp) {
        return cp == ' ' || cp == 0x200d || (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0x300 && cp <= 0x36f);
    }

    const Shaped &Shaper::shape(std::string_view text, uint8_t features) {
        key_.assign(1, char(features));
        key_.append(text);
        if (auto it = cache_.find(key_); it != cache_.end()) {
            it->second.used = frame_;
            return it->second.shaped;
        }

        Shaped out;
        std::array<hb_feature_t, 1> feats;
        unsigned nfeats = 0;
        if (features & tabular_nums) {
            hb_feature_from_string("tnum", -1, &feats[nfeats++]);
        }

        // Split into runs by the face that covers each character, then shape each run with its face.
        float pen = 0;
        size_t i = 0;
        while (i < text.size()) {
            size_t start = i, j = i;
            uint16_t face = 0;
            bool chosen = false;
            while (j < text.size()) {
                size_t k = j;
                char32_t cp = utf8::decode(text, k);
                if (!joins_run(cp)) {
                    uint16_t f = fonts_.face_for(cp);
                    if (chosen && f != face) {
                        break;
                    }
                    face = f, chosen = true;
                }
                j = k;
            }
            Face &fc = fonts_.face(face);
            hb_buffer_clear_contents(buf_);
            hb_buffer_add_utf8(buf_, text.data(), int(text.size()), unsigned(start), int(j - start));
            hb_buffer_guess_segment_properties(buf_);
            hb_shape(fc.hb, buf_, feats.data(), nfeats);
            unsigned n;
            hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf_, &n);
            hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf_, &n);
            for (unsigned g = 0; g < n; ++g) {
                out.glyphs.push_back({info[g].codepoint, face, pen + pos[g].x_offset / fc.upem, pos[g].y_offset / fc.upem, info[g].cluster});
                pen += pos[g].x_advance / fc.upem;
            }
            i = j;
        }
        out.width = pen;
        glyphs_ += out.glyphs.size();
        return cache_.emplace(key_, Entry{std::move(out), frame_}).first->second.shaped;
    }

    void Shaper::end_frame() {
        ++frame_;
        constexpr size_t budget = 60'000;
        if (glyphs_ < budget) {
            return;
        }
        // Evict everything not used in the last few frames.
        std::erase_if(cache_, [this](auto &kv) {
            if (kv.second.used + 2 >= frame_) {
                return false;
            }
            glyphs_ -= kv.second.shaped.glyphs.size();
            return true;
        });
    }

} // namespace lyren::text
