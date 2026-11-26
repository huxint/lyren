#pragma once
#include <cstdint>
#include <string_view>

#include "core/i18n.hpp"

namespace lyren {

    // Ordered from the lowest to highest requested quality; adapters map these to service-specific formats.
    enum class Quality : uint8_t {
        standard,
        higher,
        exhigh,
        lossless,
        hires
    };

    // Human-readable name for a service-reported stream quality tier.
    inline std::string_view quality_name(std::string_view level) {
        return level == "standard" || level == "medium"  ? tr("Standard")
               : level == "higher"                       ? tr("High")
               : level == "exhigh" || level == "highest" ? tr("Very high")
               : level == "lossless"                     ? tr("Lossless")
               : level == "hires" || level == "hi_res"   ? "Hi-Res"
               : level == "aac96"                        ? "AAC 96 kbps"
                                                         : level;
    }

} // namespace lyren
