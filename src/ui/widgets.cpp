#include <array>
#include <cmath>
#include <numbers>

#include "ui/ui.hpp"

namespace lyren::ui {

    constexpr Color white{1, 1, 1, 1};

    bool Ui::button(Id id, Rect r, std::string_view label, ButtonKind kind, std::string_view icon, std::string_view tip) {
        const Theme &t = *theme_;
        auto it = interact(id, r);
        if (it.hovered) {
            cursor = wl::Cursor::pointer;
        }
        const auto [hot, down] = feel(id, it);
        r = r.inset(down * 1.2f);
        Color bg, fg = t.text;
        switch (kind) {
            case ButtonKind::primary:
                bg = t.accent.mix(white, 0.08f * hot).mix({0, 0, 0, 1}, 0.1f * down);
                fg = t.on_accent();
                break;
            case ButtonKind::danger:
                bg = t.danger.mix(white, 0.08f * hot).mix({0, 0, 0, 1}, 0.1f * down);
                fg = white;
                break;
            case ButtonKind::secondary:
                bg = t.fill(1.25f + 0.75f * hot + 0.5f * down);
                break;
            case ButtonKind::ghost:
                bg = t.fill(1.15f * hot + 0.6f * down);
                fg = t.text_dim.mix(t.text, hot);
                break;
            case ButtonKind::tonal:
                bg = t.accent.with_alpha(0.13f + 0.07f * hot + 0.05f * down);
                fg = t.accent;
                break;
        }
        p_->rect(r, bg, t.round(r.h));
        const TextStyle st = t.style(type::label, fg, text::medium);
        const float tw = p_->measure(label, st), iw = icon.empty() ? 0 : label.empty() ? 16 : 22;
        const float x = r.x + (r.w - tw - iw) / 2;
        if (!icon.empty()) {
            p_->icon(icon, {x, r.y + (r.h - 16) / 2, 16, 16}, fg);
        }
        p_->text_in({x + iw, r.y, tw + 1, r.h}, label, st);
        if (it.hovered && !it.held) {
            tooltip(id, r, tip);
        }
        return it.clicked;
    }

    float Ui::button_width(std::string_view label, bool icon) {
        const float w = p_->measure(label, theme_->style(type::label, {}, text::medium));
        return std::round(w + (icon ? 22 : 0) + (label.empty() ? 18 : 36));
    }

    bool Ui::icon_button(Id id, Rect r, std::string_view icon, float icon_size, bool active, std::string_view tip) {
        const Theme &t = *theme_;
        auto it = interact(id, r);
        if (it.hovered) {
            cursor = wl::Cursor::pointer;
        }
        const auto [hot, down] = feel(id, it);
        Rect bg = r.inset(down * 1.5f);
        if (hot > 0) {
            p_->rect(bg, t.fill((1.25f + 0.65f * down) * hot), t.round(std::min(bg.w, bg.h)));
        }
        const float zoom = (icon_size - down * 1.2f) / icon_size;
        p_->set_zoom(zoom, r.x + r.w / 2, r.y + r.h / 2);
        p_->icon(icon, center(r, icon_size, icon_size), active ? t.accent : t.text_dim.mix(t.text, hot));
        p_->set_zoom(1);
        if (it.hovered && !it.held) {
            tooltip(id, r, tip);
        }
        return it.clicked;
    }

    float Ui::chip_width(std::string_view label, bool icon) {
        return std::ceil(p_->measure(label, theme_->style(type::label, {}))) + 28 + (icon ? 18 : 0);
    }

    bool Ui::chip(Id id, Rect r, std::string_view label, bool selected, std::string_view icon) {
        const Theme &t = *theme_;
        auto it = interact(id, r);
        if (it.hovered) {
            cursor = wl::Cursor::pointer;
        }
        const auto [hot, down] = feel(id, it);
        const float on = anim(sub_id(id, 0x5e), selected ? 1 : 0, pace::steady);
        r = r.inset(down * 1.2f);
        p_->rect(r, t.fill(1 + 0.75f * hot).mix(t.accent.with_alpha(0.16f + 0.05f * hot), on), t.round(r.h));
        const Color fg = t.text_dim.mix(t.text, hot).mix(t.accent, on);
        const TextStyle st = t.style(type::label, fg);
        const float tw = p_->measure(label, st);
        p_->text_in({r.x + 14, r.y, tw + 1, r.h}, label, st);
        if (!icon.empty()) {
            p_->icon(icon, {r.x + 18 + tw, r.y + (r.h - 14) / 2, 14, 14}, fg);
        }
        return it.clicked;
    }

    bool Ui::link(Id id, Rect r, std::string_view s, const TextStyle &style) {
        const Theme &t = *theme_;
        r.w = std::min(std::ceil(p_->measure(s, style)) + 1, r.w);
        auto it = interact(id, r);
        const float hot = anim(id, it.hovered ? 1 : 0, pace::quick);
        TextStyle st = style;
        st.color = style.color.mix(t.text, 0.6f * hot);
        const float w = p_->text_in(r, s, st);
        if (hot > 0) {
            const float a = p_->ascent(st.size), d = p_->descent(st.size);
            p_->rect({r.x, std::round(r.y + (r.h - a - d) / 2 + a + 2), w, 1}, st.color.with_alpha(0.55f * hot));
        }
        if (it.hovered) {
            cursor = wl::Cursor::pointer;
        }
        return it.clicked;
    }

    bool Ui::slider(Id id, Rect r, float &value, bool accent) {
        const Theme &t = *theme_;
        auto it = interact(id, r);
        if (it.hovered || it.held) {
            cursor = wl::Cursor::pointer;
        }
        bool changed = false;
        if (it.pressed || it.held) {
            const float v = std::clamp((in_->x - r.x) / r.w, 0.f, 1.f);
            changed = v != value;
            value = v;
        }
        const float hot = spring(sub_id(id, 0x51), it.hovered || it.held ? 1.f : 0.f, 180, 0.25f);
        const float press = spring(sub_id(id, 0x52), it.held ? 1.f : 0.f, 160, 0.34f);
        const float hot_clamped = std::clamp(hot, 0.f, 1.f);
        const float th = 2.6f + 1.4f * hot_clamped;
        const float cy = r.y + r.h / 2;
        const float filled = std::clamp(r.w * value, 0.f, r.w);
        const Color fill = accent ? t.accent : t.text_dim.mix(t.text, hot_clamped);
        p_->wave({r.x, cy - th / 2, r.w, th}, filled, 0, 26, 0, fill, t.text.with_alpha(0.14f));
        const float knob = 10.5f * hot + 1.8f * press;
        if (knob > 0.5f) {
            const Rect k{r.x + filled - knob / 2, cy - knob / 2, knob, knob};
            p_->shadow(k, knob / 2, 8, t.shadow.with_alpha(hot_clamped));
            p_->rect(k, white, knob / 2);
        }
        return changed;
    }

    bool Ui::tabs(Id id, Rect r, std::span<const std::string_view> labels, int &selected) {
        if (labels.empty()) {
            return false;
        }
        const Theme &t = *theme_;
        const TextStyle st = t.style(type::lead, t.text, text::medium);
        constexpr float gap = 28;

        std::array<float, 16> widths{};
        auto width_of = [&](size_t i) {
            return i < widths.size() ? widths[i] : p_->measure(labels[i], st);
        };
        float sel_x = 0, sel_w = 0, cur_x = 0;
        for (size_t i = 0; i < labels.size(); ++i) {
            const float w = p_->measure(labels[i], st);
            if (i < widths.size()) {
                widths[i] = w;
            }
            if (static_cast<int>(i) == selected) {
                sel_x = cur_x;
                sel_w = w;
            }
            cur_x += w + gap;
        }
        // The indicator springs within the bar, so it stays under its tab while the bar itself moves. Its leading edge
        // sets off first and the trailing one follows, so it stretches towards the new tab and contracts as it lands.
        const float to0 = sel_x, to1 = sel_x + sel_w;
        const bool rightwards = to0 > spring_value(sub_id(id, 1), to0);
        const float x0 = spring(sub_id(id, 1), to0, rightwards ? 380 : 220), x1 = spring(sub_id(id, 2), to1, rightwards ? 220 : 380);
        const float ix = r.x + x0, iw = x1 - x0;
        bool changed = false;
        float x = r.x;
        for (size_t i = 0; i < labels.size(); ++i) {
            const float w = width_of(i);
            const Rect c{x, r.y, w, r.h};
            auto it = interact(sub_id(id, 0x100 + i), c.inset(-gap / 2, 0));
            if (it.hovered) {
                cursor = wl::Cursor::pointer;
            }
            if (it.clicked && selected != static_cast<int>(i)) {
                selected = static_cast<int>(i), changed = true;
            }
            const float hot = anim(sub_id(id, 0x100 + i), it.hovered ? 1 : 0, 120);
            const Color fg = static_cast<int>(i) == selected ? t.text : t.text_dim.mix(t.text, hot * 0.6f);
            p_->text_in(c, labels[i], {.size = st.size, .color = fg, .features = st.features});
            x += w + gap;
        }
        p_->rect({ix, r.bottom() - 2, iw, 2}, t.accent, 1);
        return changed;
    }

    bool Ui::toggle(Id id, Rect r, bool &on) {
        const Theme &t = *theme_;
        auto it = interact(id, r);
        if (it.hovered) {
            cursor = wl::Cursor::pointer;
        }
        if (it.clicked) {
            on = !on;
        }
        float k = spring(id, on ? 1 : 0, 220);
        float hot = anim(sub_id(id, 1), it.hovered ? 1 : 0, 150);
        float down = anim(sub_id(id, 0xd0), it.held ? 1 : 0, 90);
        p_->rect(r, t.text.with_alpha(0.16f + 0.06f * hot).mix(t.accent, k), t.round(r.h));
        // The knob stretches while pressed.
        float d = r.h - 6, stretch = 4 * down;
        p_->rect({r.x + 3 + (r.w - 6 - d - stretch) * k, r.y + 3, d + stretch, d}, white, t.round(d));
        return it.clicked;
    }

    bool Ui::segmented(Id id, Rect r, std::span<const std::string_view> labels, int &selected) {
        const Theme &t = *theme_;
        const float n = static_cast<float>(labels.size()), w = r.w / n;
        p_->rect(r, t.text.with_alpha(t.dark ? 0.07f : 0.055f), t.round(r.h));
        float shown = anim(sub_id(id, 2), selected >= 0 ? 1 : 0, 150);
        float x = r.x + w * spring(sub_id(id, 1), static_cast<float>(std::max(selected, 0)), 260);
        if (shown > 0) {
            Rect thumb = Rect{x, r.y, w, r.h}.inset(3);
            p_->rect(thumb, (t.dark ? t.elevated.mix(t.text, 0.1f) : t.surface).with_alpha(shown), t.round(thumb.h));
        }
        bool changed = false;
        for (size_t i = 0; i < labels.size(); ++i) {
            Rect cell{r.x + w * static_cast<float>(i), r.y, w, r.h};
            Id cid = sub_id(id, 0x100 + i);
            auto it = interact(cid, cell);
            if (it.hovered) {
                cursor = wl::Cursor::pointer;
            }
            if (it.clicked && selected != static_cast<int>(i)) {
                selected = static_cast<int>(i), changed = true;
            }
            float hot = anim(cid, it.hovered ? 1 : 0, 120);
            bool on = selected == static_cast<int>(i);
            label(cell, labels[i], t.style(type::label, on ? t.text : t.text_dim.mix(t.text, 0.5f * hot), on ? text::medium : 0), Align::center);
        }
        return changed;
    }

    void Ui::spinner(Rect r, Color color, float thickness) {
        constexpr float tau = 2 * std::numbers::pi_v<float>;
        float start = static_cast<float>(std::fmod(now * 1.3, 1.0)) * tau;
        float sweep = tau * (0.18f + 0.16f * static_cast<float>(std::sin(now * 2.4)));
        p_->arc(r, thickness, start, sweep + 0.6f, color);
        animating_ = true;
    }

    void Ui::skeleton(Rect r, float radius) {
        const Theme &t = *theme_;
        constexpr float tau = 2 * std::numbers::pi_v<float>;
        const float wave = t.motion > 0 ? 0.5f + 0.5f * std::sin(tau * (static_cast<float>(std::fmod(now * 0.7, 1.0)) - (r.x + r.y) / 1200)) : 0.5f;
        p_->rect(r, t.text.with_alpha(0.05f + 0.045f * wave), radius);
        if (t.motion > 0) {
            animating_ = true;
        }
    }

    void Ui::equalizer(Id id, Rect r, Color color, bool playing) {
        constexpr float tau = 2 * std::numbers::pi_v<float>;
        constexpr std::array<float, 3> speed{1.9f, 2.7f, 2.3f}, offset{0.f, 0.37f, 0.71f};
        const float w = r.w / 5, live = anim(id, playing && theme_->motion > 0 ? 1 : 0, 300);
        for (size_t i = 0; i < 3; ++i) {
            const auto t = static_cast<float>(std::fmod(now * speed[i] + offset[i], 1.0));
            // Two sines at different rates, so the bars never fall into step.
            const float dance = 0.5f + 0.3f * std::sin(tau * t) + 0.2f * std::sin(tau * (2 * t + offset[i]));
            const float h = std::max(w, r.h * (0.25f + 0.75f * dance * live));
            p_->rect({r.x + w * static_cast<float>(2 * i), r.bottom() - h, w, h}, color, w / 2);
        }
        if (live >= 1) {
            wake(now + 0.05);
        }
    }

    void Ui::label(Rect r, std::string_view s, const TextStyle &style, Align align) {
        p_->text_in(r, s, style, align);
    }

} // namespace lyren::ui
