#pragma once
#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "core/i18n.hpp"
#include "ui/ui.hpp"

// Drawing helpers shared by the parts of the shell.
namespace lyren {

    // Multiplies the painter's alpha for one scope.
    struct Fade {
        ui::Painter &p;
        float saved;
        Fade(ui::Painter &painter, float a)
        : p(painter),
          saved(painter.alpha()) {
            p.set_alpha(saved * a);
        }
        ~Fade() {
            p.set_alpha(saved);
        }
    };

    // Widgets in the scope draw with another theme, such as the stage's.
    struct UseTheme {
        ui::Ui &ui;
        const ui::Theme &saved;
        UseTheme(ui::Ui &u, const ui::Theme &t)
        : ui(u),
          saved(u.use_theme(t)) {}
        ~UseTheme() {
            ui.use_theme(saved);
        }
    };

    // Quintic smoothstep of t, clamped to 0..1.
    inline float smooth01(float t) {
        t = std::clamp(t, 0.f, 1.f);
        return t * t * t * (t * (6 * t - 15) + 10);
    }

    inline ui::Rect mix(ui::Rect a, ui::Rect b, float k) {
        return {std::lerp(a.x, b.x, k), std::lerp(a.y, b.y, k), std::lerp(a.w, b.w, k), std::lerp(a.h, b.h, k)};
    }

    // Counts as "1.2k", or "1.2万" in Chinese.
    inline std::string short_count(int n) {
        if (language() == Language::zh) {
            return n >= 10000 ? trf("{:.1f}w", n / 1e4) : std::to_string(n);
        }
        return n >= 1000 ? std::format("{:.1f}k", n / 1e3) : std::to_string(n);
    }

    // With thousands separators: 12,345.
    inline std::string grouped(int n) {
        std::string s = std::to_string(n);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
            s.insert(static_cast<size_t>(i), ",");
        }
        return s;
    }

    // Width of a button that fits its label and icon.
    inline float button_width(ui::Ui &ui, std::string_view label, bool icon) {
        float w = ui.painter().measure(label, {.size = ui.theme().fs(13)});
        return std::round(w + (icon ? 22 : 0) + (label.empty() ? 24 : 36));
    }

} // namespace lyren
