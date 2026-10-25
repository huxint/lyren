#include "ui/motion.hpp"

#include <cmath>

namespace lyren::ui {

float Bezier::operator()(float x) const {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    // Polynomial form of each coordinate: ((a t + b) t + c) t.
    float cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
    float cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
    auto sample_x = [&](float t) { return ((ax * t + bx) * t + cx) * t; };
    // Newton first; bisection when the slope is too flat to trust.
    float t = x;
    for (int i = 0; i < 8; ++i) {
        float err = sample_x(t) - x;
        if (std::abs(err) < 1e-5f) return ((ay * t + by) * t + cy) * t;
        float slope = (3 * ax * t + 2 * bx) * t + cx;
        if (std::abs(slope) < 1e-6f) break;
        t -= err / slope;
    }
    float lo = 0, hi = 1;
    t = x;
    for (int i = 0; i < 32; ++i) {
        float v = sample_x(t);
        if (std::abs(v - x) < 1e-5f) break;
        (v < x ? lo : hi) = t;
        t = (lo + hi) / 2;
    }
    return ((ay * t + by) * t + cy) * t;
}

}  // namespace lyren::ui
