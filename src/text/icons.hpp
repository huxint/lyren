#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace lyren::text {

// Lucide SVGs embedded from assets/icons (see CMakeLists.txt).
struct IconSource {
    const char* name;
    const char* svg;
};
extern const IconSource icon_sources[];
extern const std::size_t icon_source_count;

// Rasterizes an icon to a size×size coverage bitmap; false if no icon has that name.
bool rasterize_icon(std::string_view name, int size, std::vector<uint8_t>& coverage);

}  // namespace lyren::text
