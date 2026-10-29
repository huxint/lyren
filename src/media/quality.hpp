#pragma once
#include <cstdint>

namespace lyren {

    // Ordered from the lowest to highest requested quality; adapters map these to service-specific formats.
    enum class Quality : uint8_t {
        standard,
        higher,
        exhigh,
        lossless,
        hires
    };

} // namespace lyren
