#include "text/fonts.hpp"

#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "core/log.hpp"

namespace lyren::text {

    bool Coverage::has(char32_t cp) const {
        auto it = std::ranges::lower_bound(pages, static_cast<uint32_t>(cp >> 8));
        if (it == pages.end() || *it != cp >> 8) {
            return false;
        }
        return bits[static_cast<size_t>(it - pages.begin())][(cp & 0xff) >> 5] & (1u << (cp & 31));
    }

    static Coverage copy_coverage(FcCharSet *cs) {
        Coverage c;
        std::array<FcChar32, FC_CHARSET_MAP_SIZE> map;
        FcChar32 next;
        for (FcChar32 base = FcCharSetFirstPage(cs, map.data(), &next); base != FC_CHARSET_DONE; base = FcCharSetNextPage(cs, map.data(), &next)) {
            c.pages.push_back(base >> 8);
            c.bits.push_back(map);
        }
        return c;
    }

    Fonts::Fonts(const std::string &family) {
        FT_Init_FreeType(&ft_);
        FcConfig *cfg = FcInitLoadConfigAndFonts();
        // The same family at each weight; a variable font's named instances are faces of their own.
        constexpr std::array<int, weights> wanted{FC_WEIGHT_REGULAR, FC_WEIGHT_MEDIUM, FC_WEIGHT_DEMIBOLD};
        for (size_t w = 0; w < chains_.size(); ++w) {
            FcPattern *pat = FcNameParse(reinterpret_cast<const FcChar8 *>((family + ":lang=zh-cn").c_str()));
            if (w > 0) {
                FcPatternDel(pat, FC_WEIGHT);
                FcPatternAddInteger(pat, FC_WEIGHT, wanted[w]);
            }
            FcConfigSubstitute(cfg, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);
            FcResult res;
            FcFontSet *set = FcFontSort(cfg, pat, FcTrue, nullptr, &res);
            auto &chain = chains_[w];
            for (int i = 0; set && i < set->nfont && chain.size() < 256; ++i) {
                FcChar8 *file = nullptr;
                int index = 0;
                FcBool color = FcFalse;
                if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch) {
                    continue;
                }
                FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &index);
                FcPatternGetBool(set->fonts[i], FC_COLOR, 0, &color);
                if (color) {
                    continue;
                }
                std::string path = reinterpret_cast<const char *>(file);
                auto known = std::ranges::find_if(faces_, [&](const Face &f) {
                    return f.path == path && f.index == index;
                });
                const auto at = static_cast<uint16_t>(known - faces_.begin());
                if (known == faces_.end()) {
                    FcCharSet *cs = nullptr;
                    FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &cs);
                    faces_.push_back({.path = path, .index = index, .coverage = cs ? copy_coverage(cs) : Coverage{}});
                }
                if (!std::ranges::contains(chain, at)) {
                    chain.push_back(at);
                }
            }
            if (set) {
                FcFontSetDestroy(set);
            }
            FcPatternDestroy(pat);
        }
        FcResult res;
        // Colour emoji sort far down the chain, so the best one is added at its end.
        FcPattern *emoji = FcNameParse(reinterpret_cast<const FcChar8 *>("emoji:color=true"));
        FcConfigSubstitute(cfg, emoji, FcMatchPattern);
        FcDefaultSubstitute(emoji);
        if (FcPattern *match = FcFontMatch(cfg, emoji, &res)) {
            FcChar8 *file = nullptr;
            FcBool color = FcFalse;
            int index = 0;
            FcCharSet *cs = nullptr;
            FcPatternGetBool(match, FC_COLOR, 0, &color);
            if (color && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) == FcResultMatch) {
                FcPatternGetInteger(match, FC_INDEX, 0, &index);
                color_ = static_cast<int>(faces_.size());
                faces_.push_back({.path = reinterpret_cast<const char *>(file), .index = index, .coverage = copy_coverage(cs), .color = true});
            }
            FcPatternDestroy(match);
        }
        FcPatternDestroy(emoji);
        FcConfigDestroy(cfg);
        FcFini();
        malloc_trim(0);
        if (faces_.empty()) {
            log::error("fontconfig found no font for '{}'", family);
            std::exit(1);
        }
        // A weight the system could not resolve reads as regular.
        for (auto &chain : chains_) {
            if (chain.empty()) {
                chain = chains_[0];
            }
        }
        open(faces_[0]);
        log::info("font: {} (+{} fallbacks)", faces_[0].path, chains_[0].size() - 1);
    }

    Fonts::~Fonts() {
        for (auto &f : faces_) {
            if (f.hb) {
                hb_font_destroy(f.hb);
            }
            if (f.ft) {
                FT_Done_Face(f.ft);
            }
            if (f.size && f.data) {
                munmap(const_cast<uint8_t *>(f.data), f.size);
            }
        }
        FT_Done_FreeType(ft_);
    }

    void Fonts::open(Face &f) {
        static const uint8_t failed_sentinel = 0;
        int fd = ::open(f.path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) < 0 || st.st_size <= 0) {
            log::warn("cannot open font {}", f.path);
            if (fd >= 0) {
                close(fd);
            }
            f.size = 0;
            f.data = &failed_sentinel;
            f.hb = hb_font_get_empty();
            f.upem = 1000;
            return;
        }
        const auto len = static_cast<size_t>(st.st_size);
        void *mapped = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) {
            log::warn("cannot map font {}", f.path);
            f.size = 0;
            f.data = &failed_sentinel;
            f.hb = hb_font_get_empty();
            f.upem = 1000;
            return;
        }
        f.size = len;
        f.data = static_cast<const uint8_t *>(mapped);
        if (FT_New_Memory_Face(ft_, f.data, FT_Long(f.size), f.index, &f.ft) != 0) {
            log::warn("invalid font {}", f.path);
            f.ft = nullptr;
        }
        hb_blob_t *blob = hb_blob_create(reinterpret_cast<const char *>(f.data), static_cast<unsigned>(f.size), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        // Fontconfig packs a variable font's named instance (from 1) above the face index. FreeType takes the pair as
        // it is; HarfBuzz is told the instance separately, so that advances follow the weight.
        const unsigned instance = static_cast<unsigned>(f.index) >> 16;
        hb_face_t *face = hb_face_create(blob, static_cast<unsigned>(f.index) & 0xffff);
        f.hb = hb_font_create(face);
        const unsigned upem = hb_face_get_upem(face);
        f.upem = static_cast<float>(upem > 0 ? upem : 1000u);
        hb_font_set_scale(f.hb, static_cast<int>(f.upem), static_cast<int>(f.upem));
        if (instance > 0 && instance < 0x8000) {
            hb_font_set_var_named_instance(f.hb, instance - 1);
        }
        hb_face_destroy(face);
        hb_blob_destroy(blob);
        if (f.ft && f.upem > 0) {
            f.ascender = static_cast<float>(f.ft->ascender) / f.upem;
            f.descender = -static_cast<float>(f.ft->descender) / f.upem;
        }
    }

    Face &Fonts::face(uint16_t i) {
        if (!faces_[i].data) {
            open(faces_[i]);
        }
        return faces_[i];
    }

    uint16_t Fonts::face_for(char32_t cp, bool emoji, int weight) const {
        const int w = std::clamp(weight, 0, weights - 1);
        const uint32_t key = (static_cast<uint32_t>(cp) << 3) | (static_cast<uint32_t>(w) << 1) | (emoji ? 1u : 0u);
        const size_t slot = (key * 2654435761u) & (lookup_cache_.size() - 1);
        const uint64_t cached = lookup_cache_[slot];
        if ((cached >> 63) && static_cast<uint32_t>((cached >> 16) & 0x0fffffffu) == key) {
            return static_cast<uint16_t>(cached & 0xffffu);
        }
        auto store = [&](uint16_t res) {
            lookup_cache_[slot] = (1ull << 63) | (static_cast<uint64_t>(key) << 16) | res;
            return res;
        };
        if (emoji && color_ >= 0 && faces_[static_cast<size_t>(color_)].coverage.has(cp)) {
            return store(static_cast<uint16_t>(color_));
        }
        const auto &chain = chains_[static_cast<size_t>(w)];
        for (uint16_t i : chain) {
            if (faces_[i].coverage.has(cp)) {
                return store(i);
            }
        }
        // Whatever only the colour face has.
        if (color_ >= 0 && faces_[static_cast<size_t>(color_)].coverage.has(cp)) {
            return store(static_cast<uint16_t>(color_));
        }
        return store(chain.front());
    }

    void Fonts::trim() {
        for (auto &f : faces_) {
            if (f.size && f.data) {
                madvise(const_cast<uint8_t *>(f.data), f.size, MADV_DONTNEED);
            }
        }
    }

} // namespace lyren::text
