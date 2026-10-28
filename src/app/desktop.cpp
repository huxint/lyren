#include "app/shell.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/i18n.hpp"

namespace lyren {
    namespace {
        constexpr ui::Color white{1, 1, 1, 1};
        constexpr float toolbar_height = 40;

        struct Fade {
            ui::Painter &p;
            float previous;
            Fade(ui::Painter &painter, float alpha)
            : p(painter),
              previous(p.alpha()) {
                p.set_alpha(previous * alpha);
            }
            ~Fade() {
                p.set_alpha(previous);
            }
        };

        float reveal(float value, float from, float to) {
            const float t = std::clamp((value - from) / (to - from), 0.f, 1.f);
            return t * t * (3 - 2 * t);
        }
    } // namespace

    float Shell::desktop_height(float size) const {
        if (dl_island_) {
            const auto &options = config_.desktop_lyrics.island;
            return options.details ? 196 : options.controls || options.volume ? 156 : 96;
        }
        return toolbar_height + std::round(size * 1.35f) + std::round(size * 0.58f * 1.35f) + 12;
    }

    void Shell::set_desktop_enabled(bool enabled) {
        dl_enabled_ = enabled;
        changed();
        desktop_changed_();
    }

    void Shell::toggle_desktop_mode() {
        dl_island_ = !dl_island_;
        dl_locked_ = dl_dragging_ = false;
        changed();
        desktop_changed_();
    }

    void Shell::toggle_translation() {
        lyric_translation_ = !lyric_translation_;
        lyric_layout_.clear();
        changed();
        redraw_();
    }

    ui::Rect Shell::desktop_lyrics(ui::Ui &ui, float width, float height) {
        using ui::Rect;
        auto &p = ui.painter();
        const auto &theme = ui.theme();
        const Track *track = current();
        const double elapsed = position();
        const double now = clip_start_ + elapsed;
        constexpr double lead = 0.2;
        const int current_line = lyric_at(lyrics_, now + lead);
        const LyricLine *line = current_line >= 0 ? &lyrics_[static_cast<size_t>(current_line)] : nullptr;
        const std::string_view text = line ? line->text : track ? track->title : tr("Nothing playing");
        const std::string_view below = dl_island_ ? std::string_view{}
                                       : line     ? (lyric_translation_ ? std::string_view(line->translation) : std::string_view{})
                                       : track    ? track->artist
                                                  : std::string_view{};
        const bool timed = line && !line->words.empty();
        if (text != dl_line_.text || below != dl_line_.sub || timed != dl_line_.timed) {
            dl_prev_ = std::exchange(dl_line_, {std::string(text), std::string(below), timed});
            dl_changed_ = ui.now;
        }
        const float change = ui.progress(dl_changed_, 300, ui::ease::standard);
        auto schedule = [&] {
            if (!playing()) {
                return;
            }
            if (timed) {
                ui.keep_animating();
            } else if (const size_t next = static_cast<size_t>(current_line + 1); next < lyrics_.size()) {
                ui.wake(ui.now + lyrics_[next].time - lead - now);
            }
        };
        auto button = [&](std::string_view name, Rect rect, std::string_view icon, bool active = false, bool primary = false) {
            const auto id = ui.id(name);
            const auto interaction = ui.interact(id, rect);
            if (interaction.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const float hot = ui.anim(id, interaction.hovered ? 1 : 0, 150);
            const float down = ui.anim(ui::sub_id(id, 1), interaction.held ? 1 : 0, 100);
            const Rect face = rect.inset(down * 1.5f);
            p.rect(face, primary ? white.with_alpha(0.94f) : white.with_alpha((active ? 0.14f : 0) + 0.1f * hot), face.h / 2);
            p.icon(icon, ui::center(face, primary ? 15.f : 16.f, primary ? 15.f : 16.f), primary ? ui::Color{0.04f, 0.04f, 0.05f, 1} : white.with_alpha(0.7f + 0.3f * hot));
            return interaction.clicked;
        };
        auto primary_text = [&](std::string_view value, const LyricLine *live, float x, float baseline, float size, float max_width, ui::Color color, ui::Color unsung) {
            if (live && !live->words.empty()) {
                const float soft = size * 0.7f;
                p.set_wipe(x + sung_x(p, *live, value, size, soft, now), soft, unsung);
            }
            p.text(x, baseline, value, {.size = size, .color = color}, max_width);
            p.clear_wipe();
        };

        if (dl_island_) {
            const auto &options = config_.desktop_lyrics.island;
            const bool show_art = options.cover || options.progress;
            const bool extras = options.details || options.controls || options.volume;
            constexpr float compact_size = 16, expanded_size = 18;
            const float center = width / 2;
            const float limit = std::min(560.f, width - 48);
            const float compact_target = std::clamp(p.measure(text, {.size = compact_size}) + (show_art ? 82 : 36), show_art ? 264.f : 220.f, std::min(400.f, limit));
            const float text_left = show_art ? (extras ? 80.f : 70.f) : 20.f;
            const float text_right = extras ? 18.f : 82.f;
            const float expanded_width = extras ? std::min(500.f, limit) : std::clamp(p.measure(text, {.size = expanded_size}) + text_left + text_right + 1, compact_target, limit);
            const float compact_width = ui.spring(ui.id("island-content-width"), compact_target, 320);
            constexpr float compact_height = 54;
            const bool hovered = ui.hovering({center - expanded_width / 2, 2, expanded_width, height - 4});
            const float open = std::clamp(ui.spring(ui.id("island-open"), hovered ? 1 : 0, 340), 0.f, 1.f);
            const float text_width = expanded_width - text_left - text_right;
            const auto main_lines = wrap(p, text, expanded_size, text_width);
            const float lines_height = static_cast<float>(std::min(main_lines.size(), size_t{2})) * 23;
            const float details_height = options.details && track ? 42 : 0;
            const float expanded_height = extras ? 18 + std::max(show_art ? 48.f : 0.f, lines_height + details_height) + 50 : std::max(compact_height, lines_height + 20);
            const float panel_width = std::lerp(compact_width, expanded_width, open);
            const float panel_height = std::lerp(compact_height, expanded_height, open);
            const Rect panel{center - panel_width / 2, 2, panel_width, panel_height};
            const float radius = std::min(std::lerp(compact_height / 2, 30.f, open), panel_height / 2);
            p.shadow({panel.x, panel.y + 6, panel.w, panel.h}, radius, 20, {0, 0, 0, 0.25f});
            p.rect(panel, {0.018f, 0.02f, 0.025f, 0.98f}, radius);
            p.rect(panel.inset(0.5f), {}, radius, white.with_alpha(0.045f), 1);

            const float art_size = std::lerp(34.f, extras ? 48.f : 38.f, open);
            const Rect artwork{panel.x + std::lerp(10.f, 18.f, open), panel.y + (extras ? std::lerp((compact_height - 34) / 2, 18.f, open) : (panel_height - art_size) / 2),
                               art_size, art_size};
            if (show_art) {
                if (options.cover && track) {
                    cover(ui, artwork, track->cover, art_size / 2, "music");
                } else {
                    p.rect(artwork, white.with_alpha(0.07f), art_size / 2);
                    p.icon("music", ui::center(artwork, 18, 18), white.with_alpha(0.7f));
                }
                if (options.progress) {
                    const double duration = player_.duration() > 0 ? player_.duration() : track ? track->duration : 0;
                    const float progress = ui.spring(ui.id("island-progress"), duration > 0 ? static_cast<float>(elapsed / duration) : 0, 260);
                    const Rect ring = artwork.inset(-3);
                    p.arc(ring, 1.5f, 0, 6.283185f, white.with_alpha(0.12f));
                    p.arc(ring, 1.8f, 0, 6.283185f * std::clamp(progress, 0.f, 1.f), theme.accent.mix(white, 0.65f));
                    if (playing()) {
                        ui.keep_animating();
                    }
                }
                const auto artwork_action = ui.interact(ui.id("island-artwork"), artwork.inset(-3));
                if (artwork_action.hovered && track) {
                    ui.cursor = wl::Cursor::pointer;
                    p.rect(artwork, {0, 0, 0, 0.3f}, art_size / 2);
                    p.icon(playing() ? "pause-fill" : "play-fill", ui::center(artwork, 14, 14), white);
                }
                if (artwork_action.clicked && track) {
                    toggle_play();
                }
            }
            ui.push_clip(panel.inset(3));
            const float compact_alpha = 1 - reveal(open, 0.12f, 0.6f);
            if (compact_alpha > 0) {
                Fade compact(p, compact_alpha);
                const float x = show_art ? artwork.right() + 12 : panel.x + 18;
                const float available = panel.right() - x - 18;
                const float baseline = panel.y + 33;
                if (change < 1) {
                    Fade old(p, 1 - change);
                    p.text(x, baseline - 6 * change, dl_prev_.text, {.size = compact_size, .color = white.with_alpha(0.94f)}, available);
                }
                {
                    Fade incoming(p, change);
                    primary_text(text, line, x, baseline + 6 * (1 - change), compact_size, available, white.with_alpha(0.96f), white.with_alpha(0.45f));
                }
            }
            const float expanded_alpha = reveal(open, 0.3f, 0.88f);
            if (expanded_alpha > 0) {
                Fade expanded(p, expanded_alpha);
                const float x = show_art ? artwork.right() + 14 : panel.x + 20;
                const float available = panel.right() - x - (extras ? 18 : 82);
                float baseline = panel.y + (extras || main_lines.size() == 1 ? 34 : 27) + 5 * (1 - open);
                if (options.details && track) {
                    p.text(x, baseline - 2, track->title, {.size = 13, .color = white.with_alpha(0.84f)}, available);
                    p.text(x, baseline + 15, track->artist, {.size = 11, .color = white.with_alpha(0.45f)}, available);
                    baseline += 42;
                }
                for (size_t i = 0; i < std::min(main_lines.size(), size_t{2}); ++i) {
                    auto value = main_lines[i];
                    if (i == 1 && main_lines.size() > 2) {
                        value = text.substr(static_cast<size_t>(value.data() - text.data()));
                    }
                    Fade incoming(p, change);
                    primary_text(value, line, x, baseline, expanded_size, available, white.with_alpha(0.96f), white.with_alpha(0.45f));
                    baseline += 23;
                }
                const float controls_y = extras ? panel.bottom() - 42 : panel.y + (panel.h - 32) / 2;
                const float mode_x = options.controls || options.volume ? panel.x + 14 : panel.right() - 76;
                if (button("island-mode", {mode_x, controls_y + 2, 28, 28}, "move")) {
                    toggle_desktop_mode();
                }
                if (options.controls) {
                    ui.disabled = track == nullptr;
                    if (button("island-previous", {center - 59, controls_y + 3, 28, 28}, "skip-back")) {
                        skip(-1);
                    }
                    if (button("island-play", {center - 17, controls_y, 34, 34}, playing() ? "pause-fill" : "play-fill", false, true)) {
                        toggle_play();
                    }
                    if (button("island-next", {center + 31, controls_y + 3, 28, 28}, "skip-forward")) {
                        skip(1);
                    }
                    ui.disabled = false;
                }
                if (options.volume) {
                    const Rect volume{panel.x + 60, controls_y + 4, 76, 24};
                    const auto interaction = ui.interact(ui.id("island-volume"), volume);
                    if (interaction.pressed || interaction.held) {
                        volume_ = std::clamp((ui.input().x - volume.x) / volume.w, 0.f, 1.f);
                        muted_ = false;
                        apply_volume();
                        changed();
                    }
                    const float value = muted_ ? 0 : volume_;
                    p.rect({volume.x, volume.y + 11, volume.w, 2}, white.with_alpha(0.15f), 1);
                    p.rect({volume.x, volume.y + 11, volume.w * value, 2}, white.with_alpha(0.8f), 1);
                    if (interaction.hovered || interaction.held) {
                        p.rect({volume.x + volume.w * value - 3, volume.y + 9, 6, 6}, white, 3);
                    }
                }
                if (button("island-close", {panel.right() - 42, controls_y + 2, 28, 28}, "x")) {
                    set_desktop_enabled(false);
                }
            }
            ui.pop_clip();
            schedule();
            return panel;
        }

        const bool translated = std::ranges::any_of(lyrics_, [](const LyricLine &lyric) {
            return !lyric.translation.empty();
        });
        const float size = theme.lyric_size, sub = std::round(size * 0.58f), line_height = std::round(size * 1.35f);
        const float max_width = width - 64;
        auto fit = [&](std::string_view value, float base) {
            return std::max(base * 0.7f, std::min(base, base * max_width / std::max(p.measure(value, {.size = base}), 1.f)));
        };
        const float text_width = std::max(p.measure(text, {.size = fit(text, size)}), p.measure(below, {.size = fit(below, sub)}));
        const float panel_width = ui.spring(ui.id("floating-width"), std::clamp(text_width + 64, 280.f, width - 16), 320);
        const float center = std::clamp(width / 2 + dl_x_, panel_width / 2 + 8, width - panel_width / 2 - 8);
        const Rect panel{center - panel_width / 2, 4, panel_width, height - 8};
        const Rect lock{center - 44, 6, 32, 32};
        Rect input = panel;
        if (dl_locked_) {
            input = lock;
            Fade visible(p, ui.anim(ui.id("floating-unlock"), ui.hovering(lock) ? 1 : 0, 150));
            p.rect(lock, {0, 0, 0, 0.72f}, 16);
            if (button("floating-unlock-button", lock, "lock-open")) {
                dl_locked_ = false;
                changed();
            }
        } else {
            const float visible = ui.anim(ui.id("floating-tools"), ui.hovering(panel) || dl_dragging_ ? 1 : 0, 180);
            if (visible > 0) {
                Fade tools(p, visible);
                p.rect({center - 112, 4, 224, 36}, {0.02f, 0.02f, 0.025f, 0.86f}, 18);
                if (button("floating-mode", {center - 100, 6, 32, 32}, "panel-top")) {
                    toggle_desktop_mode();
                }
                if (button("floating-lock", lock, "lock")) {
                    dl_locked_ = true;
                    changed();
                }
                if (translated && button("floating-translation", {center + 12, 6, 32, 32}, "languages", lyric_translation_)) {
                    toggle_translation();
                }
                if (button("floating-close", {center + 68, 6, 32, 32}, "x")) {
                    set_desktop_enabled(false);
                }
            }
            const auto drag = ui.interact(ui.id("floating-drag"), panel);
            const float x = ui.input().x, y = ui.input().y;
            if (drag.pressed) {
                dl_dragging_ = true;
                dl_grab_x_ = x - center;
                dl_grab_y_ = dl_last_y_ = y;
            }
            if (dl_dragging_ && drag.held) {
                dl_x_ = x - dl_grab_x_ - width / 2;
                if (y != dl_last_y_) {
                    dl_bottom_ = std::max(0.f, dl_bottom_ + dl_grab_y_ - y);
                    dl_last_y_ = y;
                }
                ui.cursor = wl::Cursor::grabbing;
            } else if (dl_dragging_) {
                dl_dragging_ = false;
                dl_x_ = center - width / 2;
                changed();
            }
        }
        const ui::Color sung = theme.lyric_sung.value_or(theme.accent);
        auto draw = [&](const ShownLine &shown, const LyricLine *live, float dy, float alpha) {
            Fade fade(p, alpha);
            auto one = [&](std::string_view value, float base, float baseline, ui::Color color, bool words) {
                const float font = fit(value, base), x = center - std::min(p.measure(value, {.size = font}), max_width) / 2;
                const float outline = std::max(1.f, font / 22);
                for (int i = 0; i < 8; ++i) {
                    const float angle = static_cast<float>(i) * 0.785398f;
                    p.text(x + outline * std::cos(angle), baseline + outline * std::sin(angle), value, {.size = font, .color = theme.lyric_outline}, max_width);
                }
                primary_text(value, words ? live : nullptr, x, baseline, font, max_width, color, theme.lyric_text);
            };
            const float top = toolbar_height + dy;
            one(live && shown.timed ? live->text : shown.text, size, top + line_height * 0.76f, shown.timed ? sung : theme.lyric_text, live && shown.timed);
            if (!shown.sub.empty()) {
                one(shown.sub, sub, top + line_height + sub * 1.1f, theme.lyric_text.with_alpha(0.85f), false);
            }
        };
        ui.push_clip({0, toolbar_height - 2, width, height - toolbar_height + 2});
        if (change < 1) {
            draw(dl_prev_, nullptr, -10 * change, 1 - change);
        }
        draw(dl_line_, line, 10 * (1 - change), change);
        ui.pop_clip();
        schedule();
        return input;
    }
} // namespace lyren
