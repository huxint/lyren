#include "text/icons.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

namespace lyren::text {

bool rasterize_icon(std::string_view name, int size, std::vector<uint8_t>& coverage) {
    bool filled = name.ends_with("-fill");
    if (filled) name.remove_suffix(5);
    const IconSource* src = nullptr;
    for (std::size_t i = 0; i < icon_source_count && !src; ++i)
        if (name == icon_sources[i].name) src = &icon_sources[i];
    if (!src) return false;

    // nanosvg does not know currentColor; any opaque colour works since only coverage is kept.
    std::string svg = src->svg;
    for (size_t p; (p = svg.find("currentColor")) != std::string::npos;) svg.replace(p, 12, "#000000");
    if (filled)
        if (size_t p = svg.find("fill=\"none\""); p != std::string::npos) svg.replace(p, 11, "fill=\"#000000\"");
    NSVGimage* image = nsvgParse(svg.data(), "px", 96);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    std::vector<uint8_t> rgba(size_t(size) * size * 4);
    nsvgRasterize(rast, image, 0, 0, float(size) / image->width, rgba.data(), size, size, size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    coverage.resize(size_t(size) * size);
    for (size_t i = 0; i < coverage.size(); ++i) coverage[i] = rgba[i * 4 + 3];
    return true;
}

}  // namespace lyren::text
