#include "app/shell.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "core/i18n.hpp"
#include "core/json.hpp"

namespace lyren {

    void Shell::settings_modal(ui::Ui &ui, ui::Rect r) {
        using ui::Align;
        using ui::Rect;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ui.resize_modal(std::min(840.f, ui.width - 40), std::min(656.f, ui.height - 40));
        const Rect frame = r;
        ui.label({r.x, r.y, r.w - 32, 30}, tr("Settings"), {.size = t.fs(22), .color = t.text});
        const std::array tabs{tr("Appearance"), tr("Playback"), tr("Lyrics"), tr("Dynamic Island"), tr("Colors"), tr("System")};
        constexpr std::array<std::string_view, 6> icons{"sparkles", "play", "captions", "panel-top", "disc-3", "settings"};
        const float nav_width = std::min(148.f, r.w * 0.23f);
        const Rect navigation{r.x - 8, r.y + 50, nav_width + 8, r.h - 50};
        p.rect(navigation, t.bg.mix(t.elevated, 0.45f), 12);
        for (size_t i = 0; i < tabs.size(); ++i) {
            const Rect item{navigation.x + 6, navigation.y + 7 + static_cast<float>(i) * 44, navigation.w - 12, 38};
            const auto id = ui.id(0x53455400 + i);
            const auto interaction = ui.interact(id, item);
            if (interaction.clicked && settings_tab_ != i) {
                settings_tab_ = static_cast<uint8_t>(i);
                settings_since_ = ui.now;
            }
            const bool active = settings_tab_ == i;
            const float hover = ui.anim(id, interaction.hovered ? 1 : 0, 140);
            p.rect(item, active ? t.accent.with_alpha(0.1f) : t.text.with_alpha(0.04f * hover), 8);
            p.icon(icons[i], {item.x + 12, item.y + 11, 16, 16}, active ? t.accent : t.text_dim);
            ui.label({item.x + 38, item.y, item.w - 44, item.h}, tabs[i], {.size = t.fs(13), .color = active ? t.text : t.text_dim});
        }
        r = {frame.x + nav_width + 28, frame.y + 50, frame.w - nav_width - 28, frame.h - 50};
        const float row_height = std::max(64.f, t.fs(13) * 1.3f + t.fs(11) * 2.6f + 14);
        constexpr std::array<uint8_t, 6> row_counts{7, 3, 4, 5, 8, 4};
        const Rect viewport{r.x, r.y, r.w, r.h - 30};
        ui.push_id(ui.id("settings-content"));
        ui.push_id(ui.id(static_cast<uint64_t>(settings_tab_)));
        const float enter = ui.progress(settings_since_, 180, ui::ease::standard);
        const float alpha = p.alpha();
        p.set_alpha(alpha * enter);
        const float content_height = row_height * row_counts[settings_tab_] + (settings_tab_ == 0 ? 32 : 0);
        float y = viewport.y + 4 + (1 - enter) * 5 - ui.begin_scroll(ui.id("scroll"), viewport, content_height);
        const float control_width = std::min(244.f, r.w * 0.48f);
        auto row = [&](std::string_view label, std::string_view hint, float extra = 0.f) {
            const float label_width = r.w - control_width - 20;
            const float label_height = t.fs(13) * 1.3f;
            ui.label({r.x, y + 8, label_width, label_height}, label, {.size = t.fs(13), .color = t.text});
            ui::TextStyle hint_style{.size = t.fs(11), .color = t.text_dim};
            auto lines = wrap(p, hint, hint_style.size, label_width);
            for (size_t i = 0; i < std::min(lines.size(), size_t{2}); ++i) {
                const float hint_height = hint_style.size * 1.3f;
                ui.label({r.x, y + 11 + label_height + static_cast<float>(i) * hint_height, label_width, hint_height}, lines[i], hint_style);
            }
            Rect control{r.right() - control_width, y + 8, control_width - 8, std::max(34.f, t.fs(13) + 14)};
            y += row_height + extra;
            return control;
        };
        auto toggle = [&](std::string_view id, Rect control, bool on, std::string_view section, std::string_view key, std::string_view yes, std::string_view no) {
            if (ui.toggle(ui.id(id), {control.right() - 42, control.y + 6, 42, 24}, on)) {
                set_setting_(section, key, on ? yes : no);
            }
        };
        auto choice = [&](std::string_view id, Rect control, int selected, std::span<const std::string_view> labels, std::span<const std::string_view> values,
                          std::string_view section, std::string_view key, bool quoted = true) {
            const auto interaction = ui.interact(ui.id(id), control);
            const float hover = ui.anim(ui.id(id), interaction.hovered ? 1 : 0, 140);
            p.rect(control, t.bg.mix(t.elevated, 0.25f).mix(t.text, hover * 0.035f), 8);
            ui.label({control.x + 12, control.y, control.w - 38, control.h}, labels[static_cast<size_t>(selected)], {.size = t.fs(13), .color = t.text});
            p.icon("chevron-down", {control.right() - 25, control.y + (control.h - 14) / 2, 14, 14}, t.text_dim);
            if (interaction.clicked) {
                std::vector<ui::MenuItem> menu;
                for (size_t i = 0; i < labels.size(); ++i) {
                    menu.push_back({selected == static_cast<int>(i) ? "check" : "", std::string(labels[i]),
                                    [this, section = std::string(section), key = std::string(key), value = std::string(values[i]), quoted] {
                                        set_setting_(section, key, quoted ? json_string(value) : value);
                                    }});
                }
                ui.open_menu(control.x, control.bottom() + 6, std::move(menu));
            }
        };
        auto slider = [&](std::string_view id, Rect control, double current, double low, double high, double step, std::string_view unit, std::string_view section,
                          std::string_view key) {
            const std::string draft_key(id);
            auto draft = settings_sliders_.find(draft_key);
            float value = draft == settings_sliders_.end() ? static_cast<float>((current - low) / (high - low)) : draft->second;
            ui::Id slider_id = ui.id(id);
            if (ui.slider(slider_id, {control.x, control.y, control.w - 62, control.h}, value, {.accent = true})) {
                settings_sliders_[draft_key] = value;
            }
            double result = std::round((low + static_cast<double>(value) * (high - low)) / step) * step;
            std::string display = unit == "%" ? std::format("{:.0f}%", result * 100) : step < 1 ? std::format("{:.2f}{}", result, unit) : std::format("{:.0f}{}", result, unit);
            ui.label({control.right() - 60, control.y, 60, control.h}, display, {.size = t.fs(12), .color = t.text}, Align::right);
            if (!ui.is_active(slider_id) && settings_sliders_.contains(draft_key)) {
                set_setting_(section, key, std::format("{:.2f}", result));
                settings_sliders_.erase(draft_key);
            }
        };
        auto field = [&](std::string_view id, Rect control, std::string_view current, std::string_view placeholder, std::string_view section, std::string_view key,
                         std::string_view color_alias = {}, bool color = false, bool reset = false) {
            ui::Id input_id = ui.id(id);
            auto [entry, inserted] = settings_text_.try_emplace(std::string(id), std::string(current), std::string(current));
            auto &[saved, draft] = entry->second;
            if (saved != current && ui.focus != input_id) {
                saved = draft = current;
            }
            float preview = color ? 28.f : 0;
            float buttons = reset ? 70.f : 34.f;
            if (color) {
                ui::Color swatch = ui::parse_color(draft).value_or(t.accent);
                p.rect({control.x, control.y + 7, 20, 20}, swatch, 10, t.border, 1);
            }
            bool apply = ui.text_box(input_id, {control.x + preview, control.y, control.w - preview - buttons, control.h}, draft, placeholder);
            apply |= ui.icon_button(ui.id(std::string(id) + "-apply"), {control.right() - buttons + 2, control.y + 1, 32, 32}, "check", 16);
            if (apply) {
                if (color && !ui::parse_color(draft) && draft != color_alias) {
                    ui.toast(std::string(tr("Enter a color such as #3b82f6 or #3b82f680")), "circle-alert", true);
                } else if (!draft.empty()) {
                    set_setting_(section, key, json_string(draft));
                    ui.focus = 0;
                }
            }
            if (reset && ui.icon_button(ui.id(std::string(id) + "-reset"), {control.right() - 32, control.y + 1, 32, 32}, "rotate-ccw", 16)) {
                set_setting_(section, key, {});
                ui.focus = 0;
            }
        };

        switch (settings_tab_) {
            case 0: {
                const std::array theme_labels{tr("Dark"), tr("Light")};
                const Rect themes = row(tr("Theme"), tr("Choose the window appearance"), 32);
                for (uint8_t i = 0; i < 2; ++i) {
                    const Rect tile{themes.x + static_cast<float>(i) * (themes.w + 10) / 2, themes.y, (themes.w - 10) / 2, 76};
                    const auto id = ui.id(0x54484500 + i);
                    const auto interaction = ui.interact(id, tile);
                    const bool selected = static_cast<uint8_t>(config_.theme.mode) == i;
                    const float hover = ui.anim(id, interaction.hovered ? 1 : 0, 140);
                    p.rect(tile, t.bg.mix(t.elevated, 0.6f).mix(t.accent, hover * 0.04f), 10, selected ? t.accent : t.border.with_alpha(0.55f), selected ? 1.5f : 1);
                    const Rect preview{tile.x + 9, tile.y + 9, tile.w - 18, 35};
                    const ui::Color base = i == 0 ? ui::Color{0.11f, 0.11f, 0.13f, 1} : ui::Color{0.95f, 0.95f, 0.97f, 1};
                    const ui::Color ink = i == 0 ? ui::Color{1, 1, 1, 1} : ui::Color{0.12f, 0.13f, 0.16f, 1};
                    p.rect(preview, base, 5);
                    p.rect({preview.x + 5, preview.y + 6, 15, 23}, ink.with_alpha(0.08f), 3);
                    p.rect({preview.x + 25, preview.y + 7, 17, 17}, t.accent.with_alpha(i == 0 ? 0.9f : 0.75f), 3);
                    p.rect({preview.x + 47, preview.y + 10, preview.w - 55, 3}, ink.with_alpha(0.5f), 1.5f);
                    p.rect({preview.x + 47, preview.y + 17, preview.w - 64, 3}, ink.with_alpha(0.2f), 1.5f);
                    ui.label({tile.x, tile.y + 47, tile.w, 22}, theme_labels[i], {.size = t.fs(12), .color = t.text}, Align::center);
                    if (interaction.clicked) {
                        set_setting_("theme", "mode", i == 0 ? "\"dark\"" : "\"light\"");
                    }
                }
                const std::array language_labels{tr("English"), tr("Chinese")};
                const std::array<std::string_view, 2> language_values{"en", "zh"};
                choice("language", row(tr("Language"), tr("Switch the interface immediately")), static_cast<int>(config_.interface.language), language_labels, language_values,
                       "interface", "language");
                field("font", row(tr("Font family"), tr("Use an installed font family")), config_.theme.font, "sans-serif", "theme", "font");
                slider("font-size", row(tr("Text size"), tr("Scale text throughout the player")), config_.theme.font_size, 9, 32, 1, "", "theme", "font_size");
                slider("radius", row(tr("Corner radius"), tr("Adjust the shape of panels and buttons")), config_.theme.radius, 0, 24, 1, "", "theme", "radius");
                slider("motion", row(tr("Animation duration"), tr("0 disables motion; higher values slow it down")), config_.theme.motion, 0, 4, 0.25, "x", "theme", "motion");
                slider("opacity", row(tr("Window opacity"), tr("Transparency enables background blur")), config_.window.opacity, 0.3, 1, 0.05, "%", "window", "opacity");
                break;
            }
            case 1: {
                const std::array quality_labels{tr("Standard"), tr("Higher"), tr("Very high"), tr("Lossless"), tr("Hi-Res")};
                const std::array<std::string_view, 5> quality_values{"standard", "higher", "exhigh", "lossless", "hires"};
                choice("quality", row(tr("Audio quality"), tr("Availability depends on your account")), static_cast<int>(config_.playback.quality), quality_labels, quality_values,
                       "playback", "quality");
                toggle("trial", row(tr("Skip previews"), tr("Skip tracks that only offer a short preview")), skip_trials(), "playback", "trial", "\"skip\"", "\"play\"");
                toggle("normalize", row(tr("Volume normalization"), tr("Keep track loudness consistent")), config_.playback.normalize, "playback", "normalize", "true", "false");
                break;
            }
            case 2:
                slider("lyric-size", row(tr("Desktop text size"), tr("Size of the floating lyric text")), config_.desktop_lyrics.font_size, 16, 72, 1, "", "desktop_lyrics",
                       "font_size");
                field("lyric-highlight", row(tr("Sung words"), tr("Use accent to follow the player color")), config_.desktop_lyrics.highlight, "accent", "desktop_lyrics",
                      "highlight", "accent", true);
                field("lyric-color", row(tr("Remaining words"), tr("Color before each word is sung")), config_.desktop_lyrics.color, "#ffffff", "desktop_lyrics", "color", {},
                      true);
                field("lyric-outline", row(tr("Text outline"), tr("The last two hex digits control opacity")), config_.desktop_lyrics.outline, "#000000a0", "desktop_lyrics",
                      "outline", {}, true);
                break;
            case 3:
                toggle("island-cover", row(tr("Cover artwork"), tr("Show the album cover")), config_.desktop_lyrics.island.cover, "desktop_lyrics.island", "cover", "true",
                       "false");
                toggle("island-progress", row(tr("Progress ring"), tr("Show playback progress around the cover")), config_.desktop_lyrics.island.progress, "desktop_lyrics.island",
                       "progress", "true", "false");
                toggle("island-details", row(tr("Track details"), tr("Show song and artist when expanded")), config_.desktop_lyrics.island.details, "desktop_lyrics.island",
                       "details", "true", "false");
                toggle("island-controls", row(tr("Playback controls"), tr("Show previous, play and next when expanded")), config_.desktop_lyrics.island.controls,
                       "desktop_lyrics.island", "controls", "true", "false");
                toggle("island-volume", row(tr("Volume control"), tr("Adjust volume from the expanded island")), config_.desktop_lyrics.island.volume, "desktop_lyrics.island",
                       "volume", "true", "false");
                break;
            case 4: {
                field("accent", row(tr("Accent color"), tr("Use auto to follow the album cover")), config_.theme.accent, "auto", "theme", "accent", "auto", true);
                Config base = config_;
                base.theme.colors = {};
                std::string errors;
                const auto defaults = ui::Theme::from(base, errors);
                auto hex = [](ui::Color color) {
                    return std::format("#{:02x}{:02x}{:02x}", static_cast<int>(std::lround(color.r * 255)), static_cast<int>(std::lround(color.g * 255)),
                                       static_cast<int>(std::lround(color.b * 255)));
                };
                auto override_color = [&](std::string_view key, std::string_view label, const std::optional<std::string> &value, ui::Color fallback) {
                    field(key, row(label, value ? tr("Custom color; reset to follow the theme") : tr("Following the selected theme")), value.value_or(hex(fallback)), "#rrggbb",
                          "theme.colors", key, {}, true, true);
                };
                override_color("bg", tr("Window background"), config_.theme.colors.bg, defaults.bg);
                override_color("surface", tr("Content surface"), config_.theme.colors.surface, defaults.surface);
                override_color("elevated", tr("Menus and dialogs"), config_.theme.colors.elevated, defaults.elevated);
                override_color("border", tr("Borders"), config_.theme.colors.border, defaults.border);
                override_color("text", tr("Primary text"), config_.theme.colors.text, defaults.text);
                override_color("text_dim", tr("Secondary text"), config_.theme.colors.text_dim, defaults.text_dim);
                override_color("danger", tr("Error color"), config_.theme.colors.danger, defaults.danger);
                break;
            }
            case 5: {
                const std::array frame_rates{0, 30, 60, 90, 120, 144};
                const std::string custom_frame = std::format("{} fps", config_.render.max_fps);
                const std::string custom_value = std::to_string(config_.render.max_fps);
                std::vector<std::string_view> frame_labels{tr("Display refresh rate"), "30 fps", "60 fps", "90 fps", "120 fps", "144 fps"};
                std::vector<std::string_view> frame_values{"0", "30", "60", "90", "120", "144"};
                auto selected = std::ranges::find(frame_rates, config_.render.max_fps);
                int frame = static_cast<int>(selected - frame_rates.begin());
                if (selected == frame_rates.end()) {
                    frame_labels.push_back(custom_frame);
                    frame_values.push_back(custom_value);
                }
                choice("frame-rate", row(tr("Animation frame limit"), tr("Lower limits reduce GPU usage")), frame, frame_labels, frame_values, "render", "max_fps", false);
                const std::array decoration_labels{tr("Integrated title bar"), tr("System title bar")};
                const std::array<std::string_view, 2> decoration_values{"client", "server"};
                choice("decorations", row(tr("Window decorations"), tr("Choose how the title bar is drawn")), static_cast<int>(config_.window.decorations), decoration_labels,
                       decoration_values, "window", "decorations");
                const std::array gpu_labels{tr("Integrated graphics"), tr("Dedicated graphics")};
                const std::array<std::string_view, 2> gpu_values{"integrated", "discrete"};
                choice("gpu", row(tr("Graphics device"), tr("Takes effect after restarting Lyren")), static_cast<int>(config_.gpu.device), gpu_labels, gpu_values, "gpu", "device");
                Rect reset = row(tr("Reset appearance"), tr("Restore text, colors, and animation defaults"));
                if (ui.button(ui.id("reset-appearance"), reset, tr("Restore defaults"), ui::ButtonKind::secondary, "rotate-ccw")) {
                    set_setting_("theme", "accent", "\"auto\"");
                    set_setting_("theme", "font", "\"sans-serif\"");
                    set_setting_("theme", "font_size", "14");
                    set_setting_("theme", "radius", "10");
                    set_setting_("theme", "motion", "1.0");
                    set_setting_("window", "opacity", "1.0");
                    for (std::string_view key : {"bg", "surface", "elevated", "border", "text", "text_dim", "danger"}) {
                        set_setting_("theme.colors", key, {});
                    }
                }
                break;
            }
        }
        ui.end_scroll();
        ui.pop_id();
        ui.pop_id();
        p.set_alpha(alpha);
        ui.label({r.x, r.bottom() - 20, r.w, 20}, tr("Changes are saved automatically"), {.size = t.fs(11), .color = t.text_dim});
    }

} // namespace lyren
