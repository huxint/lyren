#pragma once

namespace lyren::ui {

    // CSS-style cubic-bezier timing curve through (0,0), (x1,y1), (x2,y2), (1,1).
    struct Bezier {
        float x1, y1, x2, y2;
        // Eased value for time fraction t in 0..1.
        float operator()(float t) const;
    };

    // Spring motion: response is the time to cover 99% of a step, in seconds. With bounce (0..1) it overshoots a
    // little and settles back; 0 is critically damped.
    struct Spring {
        float value = 0, velocity = 0;
        void advance(float target, float dt, float response, float bounce = 0);
    };

    namespace ease {
        inline constexpr Bezier standard{0.2f, 0, 0, 1};          // hover, toggles, sliding indicators
        inline constexpr Bezier emphasized{0.05f, 0.7f, 0.1f, 1}; // things arriving: pages, popups, panels
        inline constexpr Bezier exit{0.3f, 0, 0.8f, 0.15f};       // things leaving
        inline constexpr Bezier linear{0, 0, 1, 1};
    } // namespace ease

} // namespace lyren::ui
