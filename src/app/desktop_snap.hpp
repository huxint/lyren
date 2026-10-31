#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace lyren {

    enum class DesktopAnchor : uint8_t {
        none,
        start,
        center,
        end
    };

    struct DesktopSnap {
        DesktopAnchor x = DesktopAnchor::none, y = DesktopAnchor::none;
    };

    inline constexpr float desktop_edge = 8;

    // Each axis docks independently. A wider release distance keeps small pointer movements from flickering.
    inline float desktop_axis(float position, float size, float extent, DesktopAnchor &anchor, bool dragging) {
        const float last = std::max(desktop_edge, extent - desktop_edge - size);
        const std::array targets{desktop_edge, std::clamp((extent - size) / 2, desktop_edge, last), last};
        position = std::clamp(position, desktop_edge, last);
        if (dragging) {
            if (anchor != DesktopAnchor::none && std::abs(position - targets[std::to_underlying(anchor) - 1]) <= 28) {
                return targets[std::to_underlying(anchor) - 1];
            }
            anchor = DesktopAnchor::none;
            float nearest = 16;
            for (size_t i = 0; i < targets.size(); ++i) {
                const float distance = std::abs(position - targets[i]);
                if (distance <= nearest) {
                    nearest = distance;
                    anchor = static_cast<DesktopAnchor>(i + 1);
                }
            }
        }
        return anchor == DesktopAnchor::none ? position : targets[std::to_underlying(anchor) - 1];
    }

} // namespace lyren
