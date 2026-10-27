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
        auto it = std::ranges::lower_bound(pages, uint32_t(cp >> 8));
        if (it == pages.end() || *it != cp >> 8) {
            return false;
        }
        return bits[size_t(it - pages.begin())][(cp & 0xff) >> 5] & (1u << (cp & 31));
    }

    static Coverage copy_coverage(FcCharSet *cs) {
        Coverage c;
        FcChar32 map[FC_CHARSET_MAP_SIZE], next;
        for (FcChar32 base = FcCharSetFirstPage(cs, map, &next); base != FC_CHARSET_DONE; base = FcCharSetNextPage(cs, map, &next)) {
            c.pages.push_back(base >> 8);
            auto &b = c.bits.emplace_back();
            std::copy_n(map, 8, b.begin());
        }
        return c;
    }

    Fonts::Fonts(const std::string &family) {
        FT_Init_FreeType(&ft_);
        FcConfig *cfg = FcInitLoadConfigAndFonts();
        FcPattern *pat = FcNameParse(reinterpret_cast<const FcChar8 *>((family + ":lang=zh-cn").c_str()));
        FcConfigSubstitute(cfg, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult res;
        FcFontSet *set = FcFontSort(cfg, pat, FcTrue, nullptr, &res);
        for (int i = 0; set && i < set->nfont && faces_.size() < 16; ++i) {
            FcChar8 *file = nullptr;
            int index = 0;
            FcBool color = FcFalse;
            if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch) {
                continue;
            }
            FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &index);
            FcPatternGetBool(set->fonts[i], FC_COLOR, 0, &color);
            // Colour bitmap fonts (emoji) would need an RGBA atlas.
            if (color) {
                continue;
            }
            std::string path = reinterpret_cast<const char *>(file);
            if (std::ranges::any_of(faces_, [&](const Face &f) {
                    return f.path == path && f.index == index;
                })) {
                continue;
            }
            FcCharSet *cs = nullptr;
            FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &cs);
            faces_.push_back({.path = path, .index = index, .coverage = cs ? copy_coverage(cs) : Coverage{}});
        }
        if (set) {
            FcFontSetDestroy(set);
        }
        FcPatternDestroy(pat);
        FcConfigDestroy(cfg);
        FcFini();
        malloc_trim(0);
        if (faces_.empty()) {
            log::error("fontconfig found no font for '{}'", family);
            std::exit(1);
        }
        open(faces_[0]);
        log::info("font: {} (+{} fallbacks)", faces_[0].path, faces_.size() - 1);
    }

    Fonts::~Fonts() {
        for (auto &f : faces_) {
            if (f.size) {
                hb_font_destroy(f.hb);
                FT_Done_Face(f.ft);
                munmap(const_cast<uint8_t *>(f.data), f.size);
            }
        }
        FT_Done_FreeType(ft_);
    }

    void Fonts::open(Face &f) {
        int fd = ::open(f.path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) < 0) {
            log::warn("cannot open font {}", f.path);
            if (fd >= 0) {
                close(fd);
            }
            f.data = reinterpret_cast<const uint8_t *>("");
            return;
        }
        f.size = size_t(st.st_size);
        f.data = static_cast<const uint8_t *>(mmap(nullptr, f.size, PROT_READ, MAP_PRIVATE, fd, 0));
        close(fd);
        FT_New_Memory_Face(ft_, f.data, FT_Long(f.size), f.index, &f.ft);
        hb_blob_t *blob = hb_blob_create(reinterpret_cast<const char *>(f.data), unsigned(f.size), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        hb_face_t *face = hb_face_create(blob, unsigned(f.index));
        f.hb = hb_font_create(face);
        f.upem = float(hb_face_get_upem(face));
        hb_font_set_scale(f.hb, int(f.upem), int(f.upem));
        hb_face_destroy(face);
        hb_blob_destroy(blob);
        if (f.ft) {
            f.ascender = float(f.ft->ascender) / f.upem;
            f.descender = -float(f.ft->descender) / f.upem;
        }
    }

    Face &Fonts::face(uint16_t i) {
        if (!faces_[i].data) {
            open(faces_[i]);
        }
        return faces_[i];
    }

    uint16_t Fonts::face_for(char32_t cp) const {
        for (uint16_t i = 0; i < faces_.size(); ++i) {
            if (faces_[i].coverage.has(cp)) {
                return i;
            }
        }
        return 0;
    }

    void Fonts::trim() {
        for (auto &f : faces_) {
            if (f.size) {
                madvise(const_cast<uint8_t *>(f.data), f.size, MADV_DONTNEED);
            }
        }
    }

} // namespace lyren::text
