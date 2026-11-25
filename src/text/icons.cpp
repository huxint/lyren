#include "text/icons.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

namespace lyren::text {

    namespace {
        const IconSource *find_icon(std::string_view name) {
            for (std::size_t i = 0; i < icon_source_count; ++i) {
                if (name == icon_sources[i].name) {
                    return &icon_sources[i];
                }
            }
            return nullptr;
        }
    } // namespace

    bool rasterize_icon(std::string_view name, int size, std::vector<uint8_t> &coverage) {
        if (size <= 0) {
            return false;
        }
        bool fill_fallback = false;
        const IconSource *src = find_icon(name);
        if (!src && name.ends_with("-fill")) {
            src = find_icon(name.substr(0, name.size() - 5));
            fill_fallback = src != nullptr;
        }
        if (!src) {
            return false;
        }

        // nanosvg does not know currentColor; any opaque colour works since only coverage is kept.
        std::string svg = src->svg;
        for (size_t p; (p = svg.find("currentColor")) != std::string::npos;) {
            svg.replace(p, 12, "#000000");
        }
        if (fill_fallback) {
            if (size_t p = svg.find("fill=\"none\""); p != std::string::npos) {
                svg.replace(p, 11, "fill=\"#000000\"");
            }
            for (size_t p = 0; (p = svg.find("fill-opacity=\"", p)) != std::string::npos;) {
                const size_t q = svg.find('"', p + 14);
                if (q == std::string::npos) {
                    break;
                }
                svg.replace(p + 14, q - (p + 14), "1");
                p += 16;
            }
        }
        NSVGimage *image = nsvgParse(svg.data(), "px", 96);
        if (!image || image->width <= 0.f || image->height <= 0.f) {
            if (image) {
                nsvgDelete(image);
            }
            return false;
        }
        // 2x supersampling gives clean anti-aliased curves and soft duo-tone glazes at small UI sizes.
        const int ss = size <= 64 ? 2 : 1;
        const int hi = size * ss;
        NSVGrasterizer *rast = nsvgCreateRasterizer();
        std::vector<uint8_t> rgba(static_cast<size_t>(hi) * hi * 4);
        nsvgRasterize(rast, image, 0, 0, static_cast<float>(hi) / std::max(image->width, image->height), rgba.data(), hi, hi, hi * 4);
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);

        coverage.resize(static_cast<size_t>(size) * size);
        if (ss == 1) {
            for (size_t i = 0; i < coverage.size(); ++i) {
                coverage[i] = rgba[i * 4 + 3];
            }
        } else {
            for (int y = 0; y < size; ++y) {
                const size_t r0 = static_cast<size_t>(y * 2) * hi * 4;
                const size_t r1 = r0 + static_cast<size_t>(hi) * 4;
                for (int x = 0; x < size; ++x) {
                    const size_t c = static_cast<size_t>(x * 2) * 4 + 3;
                    const unsigned sum = unsigned{rgba[r0 + c]} + rgba[r0 + c + 4] + rgba[r1 + c] + rgba[r1 + c + 4];
                    coverage[static_cast<size_t>(y) * size + x] = static_cast<uint8_t>((sum + 2u) >> 2);
                }
            }
        }
        return true;
    }

} // namespace lyren::text
