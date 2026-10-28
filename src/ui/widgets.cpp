#include <cmath>
#include <numbers>

#include "ui/ui.hpp"

namespace lyren::ui {

    constexpr Color white{1, 1, 1, 1};

    // Readable text over the accent, which follows the cover and can be light.
    static Color on_accent(Color accent) {
        return accent.luminance() > 0.6f ? Color{0.07f, 0.07f, 0.08f, 1} : white;
    }

    bool Ui::button(Id id, Rect r, std::string_view label, ButtonKind kind, std::string_view icon) {
        const Theme &t = *theme_;
        auto it = interact(id, r);
        float hot = anim(id, it.hovered ? 1 : 0, 150);
        float down = anim(sub_id(id, 0xd0), it.held && it.hovered ? 1 : 0, 90);
        r = r.inset(down * 1.5f);
        Color bg, fg = t.text, border{};
        switch (kind) {
            case ButtonKind::primary:
                bg = t.accent.mix(white, 0.12f * hot).mix({0, 0, 0, 1}, 0.12f * down);
                fg = on_accent(t.accent);
                break;
            case ButtonKind::secondary:
                bg = t.elevated.mix(t.text, 0.06f * hot + 0.05f * down);
                border = t.border;
                break;
            case ButtonKind::ghost:
                bg = t.text.with_alpha(0.07f * hot + 0.05f * down);
                break;
        }
        p_->rect(r, bg, t.radius_s(), border, border.a > 0 ? 1 : 0);
        TextStyle st{.size = t.fs(13), .color = fg};
        float tw = p_->measure(label, st), iw = icon.empty() ? 0 : label.empty() ? 16 : 22;
        float x = r.x + (r.w - tw - iw) / 2;
        if (!icon.empty()) {
            p_->icon(icon, {x, r.y + (r.h - 16) / 2, 16, 16}, fg);
        }
        p_->text_in({x + iw, r.y, tw + 1, r.h}, label, st);
        return it.clicked;
    }

    bool Ui::icon_button(Id id, Rect r, std::string_view icon, float icon_size, bool active) {
        const Theme &t = *theme_;
        auto it = interact(id, r);
        float hot = anim(id, it.hovered ? 1 : 0, 150);
        float down = anim(sub_id(id, 0xd0), it.held && it.hovered ? 1 : 0, 90);
        Rect bg = r.inset(down * 2);
        if (hot > 0) {
            p_->rect(bg, t.text.with_alpha((0.08f + 0.05f * down) * hot), std::min(bg.w, bg.h) / 2);
        }
        float s = icon_size - down * 1.5f;
        p_->icon(icon, center(r, s, s), active ? t.accent : t.text_dim.mix(t.text, hot));
        return it.clicked;
    }

    bool Ui::slider(Id id, Rect r, float &value, SliderStyle style) {
        constexpr float tau = 2 * std::numbers::pi_v<float>;
        const Theme &t = *theme_;
        auto it = interact(id, r);
        bool changed = false;
        if (it.pressed || it.held) {
            float v = std::clamp((in_->x - r.x) / r.w, 0.f, 1.f);
            changed = v != value;
            value = v;
        }
        float hot = anim(id, it.hovered || it.held ? 1 : 0, 150);
        float th = 4 + hot;
        float cy = r.y + r.h / 2, filled = r.w * value;
        Color fill = style.accent ? t.accent : t.text_dim.mix(t.text, hot);
        bool flowing = style.wave > 0 && filled > 0 && t.motion > 0;
        float phase = flowing ? -static_cast<float>(std::fmod(now * 1.4, 1.0)) * tau : 0;
        p_->wave({r.x, cy - th / 2, r.w, th}, filled, style.wave, 26, phase, fill, t.text.with_alpha(0.14f));
        if (flowing) {
            animating_ = true;
        }
        for (auto [a, b] : style.marked) {
            float x0 = std::max(r.x + r.w * a, r.x + filled), x1 = r.x + r.w * b;
            if (x1 - x0 > 0.5f) {
                p_->rect({x0, cy - th / 2, x1 - x0, th}, t.text.with_alpha(0.12f), th / 2);
            }
        }
        float knob = 13 * hot;
        if (knob > 0.5f) {
            Rect k{r.x + filled - knob / 2, cy - knob / 2, knob, knob};
            p_->shadow(k, knob / 2, 8, t.shadow);
            p_->rect(k, white, knob / 2);
        }
        return changed;
    }

    bool Ui::tabs(Id id, Rect r, std::span<const std::string_view> labels, int &selected, TabStyle style) {
        const Theme &t = *theme_;
        int n = static_cast<int>(labels.size());
        bool pill = style == TabStyle::pill;
        TextStyle st{.size = pill ? t.fs(13) : t.fs(15), .color = t.text};
        constexpr float gap = 28;

        // Tab extents: label widths, plus an equal share of the leftover width for pills.
        float pad = 0;
        if (pill) {
            float total = 0;
            for (auto l : labels) {
                total += p_->measure(l, st);
            }
            pad = (r.w - total) / static_cast<float>(n);
        }
        auto cell = [&](int i) -> Rect {
            float x = r.x;
            for (int k = 0; k < i; ++k) {
                x += p_->measure(labels[static_cast<size_t>(k)], st) + (pill ? pad : gap);
            }
            return {x, r.y, p_->measure(labels[static_cast<size_t>(i)], st) + pad, r.h};
        };
        Rect sel = cell(selected);
        float ix = spring(sub_id(id, 1), sel.x, 260), iw = spring(sub_id(id, 2), sel.w, 260);
        if (pill) {
            p_->rect(r, t.text.with_alpha(0.06f), r.h / 2);
            p_->shadow({ix + 2, r.y + 3, iw - 4, r.h - 4}, (r.h - 4) / 2, 6, t.shadow.with_alpha(0.5f));
            p_->rect({ix + 2, r.y + 2, iw - 4, r.h - 4}, t.elevated.mix(t.text, 0.06f), (r.h - 4) / 2);
        }
        bool changed = false;
        for (int i = 0; i < n; ++i) {
            Rect c = cell(i);
            auto it = interact(sub_id(id, 0x100 + static_cast<uint64_t>(i)), pill ? c : c.inset(-gap / 2, 0));
            if (it.clicked && selected != i) {
                selected = i, changed = true;
            }
            float hot = anim(sub_id(id, 0x100 + static_cast<uint64_t>(i)), it.hovered ? 1 : 0, 120);
            Color fg = i == selected ? t.text : t.text_dim.mix(t.text, hot * 0.6f);
            label(c, labels[static_cast<size_t>(i)], {.size = st.size, .color = fg}, pill ? Align::center : Align::left);
        }
        if (!pill) {
            p_->rect({ix, r.bottom() - 3, iw, 3}, t.accent, 1.5f);
        }
        return changed;
    }

    bool Ui::toggle(Id id, Rect r, bool &on) {
        const Theme &t = *theme_;
        auto it = interact(id, r);
        if (it.clicked) {
            on = !on;
        }
        float k = spring(id, on ? 1 : 0, 220);
        float hot = anim(sub_id(id, 1), it.hovered ? 1 : 0, 150);
        float down = anim(sub_id(id, 0xd0), it.held ? 1 : 0, 90);
        p_->rect(r, t.text.with_alpha(0.16f + 0.06f * hot).mix(t.accent, k), r.h / 2);
        // The knob stretches while pressed.
        float d = r.h - 6, stretch = 5 * down;
        p_->rect({r.x + 3 + (r.w - 6 - d - stretch) * k, r.y + 3, d + stretch, d}, white, d / 2);
        return it.clicked;
    }

    void Ui::spinner(Rect r, Color color, float thickness) {
        constexpr float tau = 2 * std::numbers::pi_v<float>;
        float start = static_cast<float>(std::fmod(now * 1.3, 1.0)) * tau;
        float sweep = tau * (0.18f + 0.16f * static_cast<float>(std::sin(now * 2.4)));
        p_->arc(r, thickness, start, sweep + 0.6f, color);
        animating_ = true;
    }

    void Ui::label(Rect r, std::string_view s, const TextStyle &style, Align align) {
        if (align != Align::left) {
            float w = std::min(p_->measure(s, style), r.w);
            r.x += align == Align::center ? (r.w - w) / 2 : r.w - w;
            r.w = w + 0.5f;
        }
        p_->text_in(r, s, style);
    }

} // namespace lyren::ui
