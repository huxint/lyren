#include "app/shell.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <utility>

#include "app/draw.hpp"
#include "core/i18n.hpp"

// Desktop lyrics on their own layer surface: a floating line near the bottom of the screen, or the Dynamic Island at the
// top. The settings page draws both as live previews.
namespace lyren {

    using ui::Rect;
    namespace ease = ui::ease;

    namespace {
        constexpr ui::Color white{1, 1, 1, 1}, ink{0.03f, 0.03f, 0.035f, 1};
        constexpr float toolbar_height = 44, tau = 2 * std::numbers::pi_v<float>;
        constexpr float island_surface = 264; // the open card and its shadow
        constexpr double lead = 0.2;          // a line lights up a little early, with the voice rather than after it

        float reveal(float value, float from, float to) {
            return smooth01((value - from) / (to - from));
        }

        // A level meter that dances while the music plays: each bar mixes two slow waves.
        float level(double now, int bar) {
            auto b = static_cast<double>(bar);
            double a = 0.5 + 0.5 * std::sin(now * (6.1 + 1.9 * b) + 1.3 * b), c = 0.5 + 0.5 * std::sin(now * (3.7 + 1.1 * b) + 2.6 * b);
            return static_cast<float>(0.2 + 0.8 * a * c);
        }
    } // namespace

    float Shell::desktop_height(float size) const {
        if (dl_island_) {
            return island_surface;
        }
        return toolbar_height + std::round(size * 1.35f) + std::round(size * 0.58f * 1.35f) + 12;
    }

    void Shell::set_desktop_enabled(bool enabled) {
        dl_enabled_ = enabled;
        changed();
        desktop_changed_();
    }

    void Shell::set_desktop_island(bool island) {
        if (dl_island_ != island) {
            dl_island_ = island;
            dl_locked_ = dl_dragging_ = false;
            changed();
            desktop_changed_();
        }
    }

    void Shell::toggle_translation() {
        lyric_translation_ = !lyric_translation_;
        lyric_layout_.clear();
        changed();
        redraw_();
    }

    float Shell::show_line(ui::Ui &ui, LineChange &c, std::string_view text, std::string_view sub, bool timed) {
        if (text != c.shown.text || sub != c.shown.sub || timed != c.shown.timed) {
            c.prev = std::exchange(c.shown, {std::string(text), std::string(sub), timed});
            c.prev_since = c.since, c.since = ui.now;
        }
        return ui.progress(c.since, 360, ease::standard);
    }

    void Shell::lyric_frames(ui::Ui &ui, int line, bool timed) {
        if (!playing()) {
            return;
        }
        if (timed) {
            ui.keep_animating();
        } else if (const auto next = static_cast<size_t>(line + 1); next < lyrics_.size()) {
            ui.wake(ui.now + lyrics_[next].time - lead - timeline().to_song(position()));
        }
    }

    Rect Shell::desktop_lyrics(ui::Ui &ui, float width, float height) {
        return dl_island_ ? island(ui, width / 2, 8) : floating(ui, width, height);
    }

    Rect Shell::island(ui::Ui &ui, float center, float top) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &options = config_.desktop_lyrics.island;
        const bool art = options.cover, ring = art && options.progress;
        const Track *track = current();
        const auto clock = timeline();
        const double elapsed = position(), now = clock.to_song(elapsed);
        const int at = lyric_at(lyrics_, now + lead);
        const LyricLine *line = at >= 0 ? &lyrics_[static_cast<size_t>(at)] : nullptr;
        const std::string_view text = line ? std::string_view(line->text) : track ? std::string_view(track->title) : lyren::tr("Nothing playing");
        const bool timed = line && !line->words.empty();
        const float change = show_line(ui, island_line_, text, {}, timed);
        const double duration = clock.duration();
        const float done = clock.progress(elapsed);

        // Closed it is a pill: the cover in its progress ring, the line being sung and a level meter. While the pointer
        // rests on it, it opens into a card with the track, the whole line, progress and controls.
        constexpr float pill_h = 40, line_size = 14, open_w = 400, pad = 18;
        const float left = art ? 46.f : 20.f, right = 42;
        const float text_w = p.measure(text, {.size = line_size});
        const float pill_w = ui.spring(ui.id("island-w"), std::clamp(left + text_w + right, 230.f, 460.f), 420, 0.2f);
        const auto lines = p.wrap(text, 16, open_w - 2 * pad);
        const bool lyric_text = line || !track;
        const size_t shown_lines = lyric_text ? std::min(lines.size(), size_t{2}) : 0;
        const float head_h = art ? 52.f : 40.f, lines_h = static_cast<float>(shown_lines) * 23;
        const float open_h = pad + head_h + 14 + lines_h + 16 + 14 + 12 + 40 + 16;
        const Rect pill{center - pill_w / 2, top, pill_w, pill_h};
        const Rect card{center - open_w / 2, top, open_w, open_h};

        const float was = std::clamp(ui.spring_value(ui.id("island-open"), 0), 0.f, 1.f);
        // Pressing one of its buttons still counts as pointing at it, so it stays open for the click.
        const bool pointed = ui.pointing(mix(pill, card, was).inset(-2)) || island_dragging_;
        // It opens once the pointer rests on it, so passing over it on the way to something else leaves it be, and
        // closes a moment after the pointer leaves, so slipping past its edge does not.
        const float rest = ui.anim(ui.id("island-rest"), pointed ? 1 : 0, pointed ? 220 : 180, ease::linear);
        const bool opened = rest >= 1 || (rest > 0 && was > 0.5f);
        const float k = ui.spring(ui.id("island-open"), opened ? 1 : 0, 480, 0.08f);
        const float open = std::clamp(k, 0.f, 1.f);
        const Rect panel = mix(pill, card, k);
        const float radius = std::lerp(pill_h / 2, 28.f, open);
        p.shadow({panel.x, panel.y + 6, panel.w, panel.h}, radius, 22, {0, 0, 0, 0.28f + 0.1f * open});
        p.rect(panel, ink.with_alpha(0.97f), radius);
        p.rect(panel.inset(0.5f), {}, radius, white.with_alpha(0.05f), 1);

        const float closed_a = 1 - reveal(open, 0.08f, 0.68f), open_a = reveal(open, 0.3f, 0.9f);
        const float head_x = art ? card.x + pad + 52 + 14 : card.x + pad, head_w = card.right() - pad - 30 - head_x;
        const float head_y = card.y + pad + (art ? 6.f : 0.f);
        // Details grow out of the pill with the same motion as its frame; their hit areas follow their drawn bounds.
        auto unfold = [&](Rect end) {
            return mix(Rect{center, pill.y + pill_h / 2, 0, 0}, end, open);
        };
        ui.push_clip(panel.inset(1));
        const float live = ui.anim(ui.id("island-live"), playing() ? 1 : 0, 420);
        auto meter = [&](float cx, float cy, float alpha) {
            for (int i = 0; i < 4; ++i) {
                float h = 3 + 11 * live * level(ui.now, i);
                p.rect({cx - 8.75f + 5 * static_cast<float>(i), cy - h / 2, 2.5f, h}, t.accent.with_alpha(alpha), 1.25f);
            }
        };
        if (live > 0 && playing()) {
            ui.keep_animating();
        }

        // The cover travels from the pill into the card's corner, a disc becoming a rounded square.
        if (art) {
            const Rect small = ring ? Rect{pill.x + 8, top + 8, 24, 24} : Rect{pill.x + 7, top + 7, 26, 26};
            const Rect big{card.x + pad, card.y + pad, 52, 52};
            const Rect a = mix(small, big, open);
            const float r = std::lerp(a.w / 2, 12.f, open);
            if (track) {
                cover(ui, a, track->cover, r, "music");
            } else {
                p.rect(a, white.with_alpha(0.08f), r);
                p.icon("music", ui::center(a, a.w * 0.5f, a.w * 0.5f), white.with_alpha(0.6f));
            }
            if (ring && closed_a > 0) {
                Fade fade(p, closed_a);
                const float shown = ui.spring(ui.id("island-progress"), done, 260);
                p.arc(a.inset(-4), 2, 0, tau, white.with_alpha(0.14f));
                p.arc(a.inset(-4), 2, 0, tau * shown, t.accent);
            }
            const auto press = ui.interact(ui.id("island-artwork"), a.inset(-4));
            if (press.hovered && track && open < 0.5f) {
                ui.cursor = wl::Cursor::pointer;
                p.rect(a, {0, 0, 0, 0.35f}, r);
                p.icon(playing() ? "pause-fill" : "play-fill", ui::center(a, 12, 12), white);
            }
            if (press.clicked && track && open < 0.5f) {
                toggle_play();
            }
        }

        // The line, sung words bright: in the pill it slides along under the voice when it is too long to fit.
        auto sung = [&](std::string_view value, float x, float baseline, float size, float limit) {
            if (line && timed) {
                const float soft = size * 0.7f;
                p.set_wipe(x + sung_x(p, *line, value, size, soft, now), soft, white.with_alpha(0.42f));
            }
            p.text(x, baseline, value, {.size = size, .color = white.with_alpha(0.96f)}, limit);
            p.clear_wipe();
        };
        {
            // The line itself travels into the card. With no lyrics, the track title becomes the heading instead.
            const float avail = pill.w - left - right, size = lyric_text ? 16.f : 15.f;
            const float end_x = lyric_text ? card.x + pad : head_x, end_y = lyric_text ? card.y + pad + head_h + 14 + 17 : head_y + 15;
            const float end_w = lyric_text ? card.w - 2 * pad : head_w;
            // A lyric first drops below the travelling cover, then moves left into the wider line.
            const float across = lyric_text && art ? reveal(open, 0.25f, 1) : open;
            const float x = std::lerp(pill.x + left, end_x, across), baseline = std::lerp(pill.y + 25, end_y, open);
            const float zoom = std::lerp(line_size, size, open) / size;
            const Rect area{x, std::lerp(pill.y, end_y - 20, open), std::lerp(avail, end_w, open), std::lerp(pill_h, lyric_text ? lines_h + 10 : 28.f, open)};
            ui.push_clip(area);
            auto slide = [&](std::string_view value, double since, bool current) {
                const float overflow = p.measure(value, {.size = line_size}) - avail;
                if (overflow <= 0) {
                    return 0.f;
                }
                float target = 0;
                if (current && line && timed) {
                    target = std::clamp(sung_x(p, *line, value, line_size, 8, now) - avail * 0.6f, 0.f, overflow);
                } else if (current && line) {
                    const double from = line->time, to = static_cast<size_t>(at + 1) < lyrics_.size() ? lyrics_[static_cast<size_t>(at + 1)].time : from + 5;
                    target = overflow * smooth01((static_cast<float>((now + lead - from) / std::max(to - from, 0.5)) - 0.15f) / 0.6f);
                }
                return ui.spring(ui::sub_id(ui.id("island-slide"), std::bit_cast<uint64_t>(since)), target, 700);
            };
            if (change < 1) {
                Fade old(p, 1 - change);
                const float ox = x - slide(island_line_.prev.text, island_line_.prev_since, false) * (1 - open), oy = baseline - 7 * change;
                p.set_zoom(zoom, ox, oy);
                p.text(ox, oy, island_line_.prev.text, {.size = size, .color = white.with_alpha(0.96f)});
                p.set_zoom(1);
            }
            {
                Fade incoming(p, change);
                const float shift = slide(text, island_line_.since, true) * (1 - open), tx = x - shift, ty = baseline + 7 * (1 - change);
                const float folded = lyric_text && lines.size() > 1 ? reveal(open, 0.2f, 0.85f) : 0;
                if (folded < 1) {
                    Fade single(p, 1 - folded);
                    const float limit = lyric_text ? 1e9f : std::lerp(p.measure(text, {.size = size}), end_w / zoom, open);
                    p.set_zoom(zoom, tx, ty);
                    sung(text, tx, ty, size, limit);
                    p.set_zoom(1);
                }
                if (folded > 0) {
                    Fade wrapped(p, folded);
                    for (size_t i = 0; i < shown_lines; ++i) {
                        std::string_view value = lines[i];
                        if (i == 1 && lines.size() > 2) {
                            value = text.substr(static_cast<size_t>(value.data() - text.data()));
                        }
                        const float ly = ty + static_cast<float>(i) * 23 * open;
                        p.set_zoom(zoom, tx, ly);
                        sung(value, tx, ly, size, end_w / zoom);
                        p.set_zoom(1);
                    }
                }
                // Soft edges where the line runs on past the pill.
                if (closed_a > 0) {
                    Fade edge(p, closed_a);
                    if (shift > 0.5f) {
                        p.hgradient({area.x, area.y, 16, area.h}, ink, ink.with_alpha(0));
                    }
                    if (text_w - shift > avail + 0.5f) {
                        p.hgradient({area.right() - 16, area.y, 16, area.h}, ink.with_alpha(0), ink);
                    }
                }
            }
            ui.pop_clip();
        }
        meter(std::lerp(pill.right() - 22, card.right() - pad - 9, open), std::lerp(pill.y + pill_h / 2, head_y + 12, open), 1);

        if (open_a > 0) {
            Fade fade(p, open_a);
            ui.disabled = open_a < 0.6f;
            auto detail = [&](Rect end, std::string_view value, ui::TextStyle style) {
                const Rect at = unfold(end);
                p.set_zoom(open, at.x, at.y);
                p.text(at.x, at.y, value, style, end.w);
                p.set_zoom(1);
            };
            if (lyric_text) {
                detail({head_x, head_y + 15, head_w, 0}, track ? std::string_view(track->title) : std::string_view("Lyren"), {.size = 15, .color = white.with_alpha(0.95f)});
            }
            detail({head_x, head_y + 35, head_w, 0}, track ? std::string_view(track->artist) : lyren::tr("Nothing playing"), {.size = 12.5f, .color = white.with_alpha(0.55f)});

            // Progress: drag to preview, release to seek.
            const float py = card.y + pad + head_h + 14 + lines_h + 16;
            const ui::TextStyle times{.size = 11, .color = white.with_alpha(0.5f), .features = text::tabular_nums};
            const Rect bar = unfold({card.x + pad + 42, py, card.w - 2 * pad - 84, 14});
            ui.disabled = ui.disabled || !track;
            float v = island_dragging_ ? island_drag_v_ : done;
            if (ui.slider(ui.id("island-seek"), bar, v) || ui.is_active(ui.id("island-seek"))) {
                island_dragging_ = true, island_drag_v_ = v;
            }
            if (island_dragging_ && !ui.is_active(ui.id("island-seek"))) {
                island_dragging_ = false;
                seek_song(island_drag_v_ * duration);
            }
            detail({card.x + pad, py + 11, 38, 0}, mmss(island_dragging_ ? island_drag_v_ * duration : now), times);
            const std::string total = mmss(duration);
            detail({card.right() - pad - p.measure(total, times), py + 11, 38, 0}, total, times);

            // Controls: the floating mode and hiding on the sides, the transport in the middle.
            const float cy = py + 14 + 12 + 20;
            auto button = [&](std::string_view name, Rect end, std::string_view icon, bool primary = false) {
                const Rect b = unfold(end);
                const auto id = ui.id(name);
                const auto it = ui.interact(id, b);
                const float hot = ui.anim(id, it.hovered ? 1 : 0, 150), down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
                const Rect face = b.inset(down * 1.5f * open);
                p.rect(face, primary ? white.with_alpha(0.95f) : white.with_alpha(0.1f * hot), face.h / 2);
                const float s = primary ? 16 : 17;
                p.set_zoom(open, face.x + face.w / 2, face.y + face.h / 2);
                p.icon(icon, ui::center(face, s, s), primary ? ink : white.with_alpha(0.7f + 0.3f * hot));
                p.set_zoom(1);
                return it.clicked;
            };
            if (button("island-previous", {center - 64, cy - 16, 32, 32}, "skip-back")) {
                skip(-1);
            }
            if (button("island-play", {center - 20, cy - 20, 40, 40}, playing() ? "pause-fill" : "play-fill", true)) {
                toggle_play();
            }
            if (button("island-next", {center + 32, cy - 16, 32, 32}, "skip-forward")) {
                skip(1);
            }
            ui.disabled = open_a < 0.6f;
            if (button("island-floating", {card.x + pad - 6, cy - 16, 32, 32}, "panel-bottom")) {
                set_desktop_island(false);
            }
            if (button("island-hide", {card.right() - pad - 26, cy - 16, 32, 32}, "x")) {
                set_desktop_enabled(false);
            }
            ui.disabled = false;
        }
        ui.pop_clip();
        lyric_frames(ui, at, timed);
        return panel;
    }

    void Shell::floating_text(ui::Ui &ui, float center, float baseline, std::string_view text, const LyricLine *live, float size, float max_width, ui::Color color,
                              ui::Color rest) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // Long lines shrink to fit, down to 70%, before they are cut short.
        const float font = std::max(size * 0.7f, std::min(size, size * max_width / std::max(p.measure(text, {.size = size}), 1.f)));
        const float x = center - std::min(p.measure(text, {.size = font}), max_width) / 2;
        // The outline is eight offset copies under the text, so it reads over any wallpaper.
        const float outline = std::max(1.f, font / 22);
        for (int i = 0; i < 8; ++i) {
            const float angle = static_cast<float>(i) * tau / 8;
            p.text(x + outline * std::cos(angle), baseline + outline * std::sin(angle), text, {.size = font, .color = t.lyric_outline, .silhouette = true}, max_width);
        }
        if (live && !live->words.empty()) {
            const float soft = font * 0.7f;
            p.set_wipe(x + sung_x(p, *live, text, font, soft, timeline().to_song(position())), soft, rest);
        }
        p.text(x, baseline, text, {.size = font, .color = color}, max_width);
        p.clear_wipe();
    }

    Rect Shell::floating(ui::Ui &ui, float width, float height) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Track *track = current();
        const double now = timeline().to_song(position());
        const int at = lyric_at(lyrics_, now + lead);
        const LyricLine *line = at >= 0 ? &lyrics_[static_cast<size_t>(at)] : nullptr;
        const std::string_view text = line ? std::string_view(line->text) : track ? std::string_view(track->title) : lyren::tr("Nothing playing");
        const std::string_view below = line ? (lyric_translation_ ? std::string_view(line->translation) : std::string_view{}) : track ? track->artist : std::string_view{};
        const bool timed = line && !line->words.empty();
        const float change = show_line(ui, floating_line_, text, below, timed);
        const bool translated = std::ranges::any_of(lyrics_, [](const LyricLine &l) {
            return !l.translation.empty();
        });

        const float size = t.lyric_size, sub = std::round(size * 0.58f), line_height = std::round(size * 1.35f);
        const float max_width = width - 64;
        const float text_width = std::max(p.measure(text, {.size = size}), p.measure(below, {.size = sub}));
        const float panel_width = ui.spring(ui.id("floating-width"), std::clamp(std::min(text_width, max_width) + 64, 360.f, width - 16), 320);
        const float center = std::clamp(width / 2 + dl_x_, panel_width / 2 + 8, width - panel_width / 2 - 8);
        const Rect panel{center - panel_width / 2, 4, panel_width, height - 8};
        auto button = [&](std::string_view name, Rect rect, std::string_view icon, bool on = false) {
            const auto id = ui.id(name);
            const auto it = ui.interact(id, rect);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            const float hot = ui.anim(id, it.hovered ? 1 : 0, 150), down = ui.anim(ui::sub_id(id, 1), it.held ? 1 : 0, 100);
            const Rect face = rect.inset(down * 1.5f);
            p.rect(face, white.with_alpha((on ? 0.16f : 0) + 0.12f * hot), face.h / 2);
            p.icon(icon, ui::center(face, 16, 16), white.with_alpha(on ? 1 : 0.72f + 0.28f * hot));
            return it.clicked;
        };

        Rect input = panel;
        const Rect lock{center - 16, 6, 32, 32};
        if (dl_locked_) {
            // Locked, clicks fall through everywhere but the unlock button, which shows when pointed at.
            input = lock;
            Fade visible(p, ui.anim(ui.id("floating-unlock"), ui.pointing(lock) ? 1 : 0, 150));
            p.rect(lock, ink.with_alpha(0.8f), 16);
            if (button("floating-unlock-button", lock, "lock-open")) {
                dl_locked_ = false;
                changed();
            }
        } else {
            const float visible = ui.anim(ui.id("floating-tools"), ui.pointing(panel) || dl_dragging_ ? 1 : 0, 180);
            if (visible > 0) {
                // The toolbar: the transport, then translation, the island, locking and hiding.
                Fade tools(p, visible);
                const float n = translated ? 7 : 6, bar_w = n * 34 + 26;
                const Rect bar{center - bar_w / 2, 4, bar_w, 36};
                p.rect(bar, ink.with_alpha(0.82f), 18);
                float x = bar.x + 8;
                auto next_slot = [&](float gap = 0) {
                    const Rect r{x + gap, 6, 32, 32};
                    x += 34 + gap;
                    return r;
                };
                ui.disabled = !track;
                if (button("floating-previous", next_slot(), "skip-back")) {
                    skip(-1);
                }
                if (button("floating-play", next_slot(), playing() ? "pause-fill" : "play-fill")) {
                    toggle_play();
                }
                if (button("floating-next", next_slot(), "skip-forward")) {
                    skip(1);
                }
                ui.disabled = false;
                p.rect({x + 4, 14, 1, 16}, white.with_alpha(0.16f));
                x += 10;
                if (translated && button("floating-translation", next_slot(), "languages", lyric_translation_)) {
                    toggle_translation();
                }
                if (button("floating-island", next_slot(), "panel-top")) {
                    set_desktop_island(true);
                }
                if (button("floating-lock", next_slot(), "lock")) {
                    dl_locked_ = true;
                    changed();
                }
                if (button("floating-close", next_slot(), "x")) {
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

        const ui::Color sung = t.lyric_sung.value_or(t.accent);
        auto draw = [&](const ShownLine &shown, const LyricLine *live, float dy, float alpha) {
            Fade fade(p, alpha);
            const float top = toolbar_height + dy;
            floating_text(ui, center, top + line_height * 0.76f, live && shown.timed ? std::string_view(live->text) : std::string_view(shown.text),
                          live && shown.timed ? live : nullptr, size, max_width, shown.timed ? sung : t.lyric_text, t.lyric_text);
            if (!shown.sub.empty()) {
                floating_text(ui, center, top + line_height + sub * 1.1f, shown.sub, nullptr, sub, max_width, t.lyric_text.with_alpha(0.85f), t.lyric_text);
            }
        };
        ui.push_clip({0, toolbar_height - 2, width, height - toolbar_height + 2});
        if (change < 1) {
            draw(floating_line_.prev, nullptr, -10 * change, 1 - change);
        }
        draw(floating_line_.shown, line, 10 * (1 - change), change);
        ui.pop_clip();
        lyric_frames(ui, at, timed);
        return input;
    }

} // namespace lyren
