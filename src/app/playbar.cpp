#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "app/draw.hpp"
#include "app/shell.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    using State = audio::Player::State;
    namespace ease = ui::ease;
    constexpr ui::Color white{1, 1, 1, 1};

    void Shell::seek_string(ui::Ui &ui, Rect r, float stage) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto clock = timeline();
        const double duration = clock.duration(), file_duration = clock.file_duration();
        const double position = player_.state() == State::idle && !fetching_ ? resume_at_ : last_pos_;
        const auto [from, to] = clock.playable_span();
        const float sx = r.x + r.w * from, width = r.w * (to - from), cy = r.y + r.h / 2;
        progress_w_ = r.w;

        const ui::Id id = ui.id("progress");
        ui.disabled = !current() || duration <= 0;
        const auto it = ui.interact(id, r);
        const float pointer = (ui.input().x - r.x) / r.w;
        if (it.pressed && pointer >= from && pointer <= to) {
            dragging_ = true;
        }
        if (dragging_ && (it.pressed || it.held)) {
            drag_v_ = std::clamp(pointer, from, to);
        }
        const bool released = dragging_ && !ui.is_active(id);
        if (released) {
            dragging_ = false;
            pluck_time_ = ui.now;
            pluck_at_ = drag_v_;
            seek_song(drag_v_ * duration);
        }
        ui.disabled = false;
        if (it.hovered || it.held) {
            ui.cursor = wl::Cursor::pointer;
        }
        const float hot = ui.anim(ui.id("string-hover"), it.hovered || it.held ? 1.f : 0.f, 150);
        const float value = ui.spring(ui.id("progress-v"), dragging_ || released ? drag_v_ : clock.progress(position), dragging_ || released ? 0 : 240);
        const float filled = std::clamp(r.w * (value - from), 0.f, width);
        const float thickness = 1.6f + 1.2f * hot;
        const bool wave = config_.interface.progress == ProgressStyle::wave && t.motion > 0;
        meter_.follow(player_.levels(), static_cast<float>(ui.now - meter_time_));
        meter_time_ = ui.now;
        const float live = ui.anim(ui.id("string-live"), playing() && !player_.buffering() && wave ? 1.f : 0.f, 500, ease::standard);
        const float amplitude = ui.spring(ui.id("string-amplitude"), (2.4f + stage + 1.8f * meter_.all()) * live, 240);
        const float phase = -static_cast<float>(std::fmod(ui.now * 0.7, 1.0)) * 2 * std::numbers::pi_v<float>;
        ui::Pluck pluck;
        const float age = t.motion > 0 ? static_cast<float>((ui.now - pluck_time_) / t.motion) : 1.f;
        if (wave && age < 0.7f) {
            pluck = {r.w * (pluck_at_ - from), 8 * std::exp(-age * 7) * std::sin(age * 30), 32};
            ui.keep_animating();
        }
        if (amplitude > 0.01f && filled > 0) {
            ui.keep_animating();
        }
        for (float x = r.x; x + 3 <= sx; x += 7) {
            p.rect({x, cy - 1, 3, 2}, t.text.with_alpha(0.12f), 1);
        }
        for (float x = sx + width + 4; x + 3 <= r.right(); x += 7) {
            p.rect({x, cy - 1, 3, 2}, t.text.with_alpha(0.12f), 1);
        }
        p.wave({sx, cy - thickness / 2, width, thickness}, filled, amplitude, 42, phase, t.accent, t.text.with_alpha(0.16f), pluck);
        if (const audio::Spool *spool = player_.spool(); spool && file_duration > 0) {
            const auto [have, size] = spool->progress();
            if (size && have < size) {
                for (const auto [begin, end] : spool->ranges()) {
                    auto x = [&](uint64_t byte) {
                        return r.x + r.w * clock.progress(file_duration * static_cast<double>(byte) / static_cast<double>(size));
                    };
                    const float left = std::max({x(begin), sx + filled, sx}), right = std::min(x(end), sx + width);
                    if (right > left) {
                        p.rect({left, cy - thickness / 2, right - left, thickness}, t.text.with_alpha(0.15f), thickness / 2);
                    }
                }
            }
        }
        if (highlight_to_ > highlight_from_ && duration > 0) {
            const float left = r.x + r.w * clock.fraction(highlight_from_), right = r.x + r.w * clock.fraction(highlight_to_);
            p.rect({left, cy + 4, std::max(right - left, 2.f), 2}, t.accent.with_alpha(0.3f + 0.15f * hot), 1);
        }
        if (current() && duration > 0) {
            const float dot = 4 + 6 * hot;
            const auto pin = [](float x) {
                x = std::clamp(x, 0.f, 1.f);
                return x * x * (3 - 2 * x);
            };
            const float u = (filled - pluck.at) / pluck.spread;
            const float lift = pluck.lift * std::exp(-u * u) * pin((filled - thickness / 2) / (2 * pluck.spread)) * pin((width - thickness / 2 - filled) / (2 * pluck.spread));
            Rect knob{sx + filled - dot / 2, cy - lift - dot / 2, dot, dot};
            if (hot > 0) {
                p.shadow(knob, dot / 2, 8, t.shadow.with_alpha(hot));
            }
            p.rect(knob, t.accent.mix(white, hot), dot / 2);
        }

        // The seek preview stays above the page and its side panel, with the lyric at the pointed-to song time.
        const float preview = ui.anim(ui.id("seek-preview"), (it.hovered || it.held) && duration > 0 ? 1.f : 0.f, 140);
        if (preview > 0) {
            const double at = std::clamp(pointer, 0.f, 1.f) * duration;
            const std::string time = mmss(at);
            const int line = lyric_at(lyrics_, at);
            const std::string_view words = line >= 0 ? std::string_view(lyrics_[static_cast<size_t>(line)].text) : std::string_view{};
            const TextStyle time_style{.size = t.fs(12), .color = t.text_dim, .features = text::tabular_nums};
            const TextStyle lyric_style{.size = t.fs(13), .color = t.text, .features = text::medium};
            const float w = std::clamp(std::max(p.measure(time, time_style), p.measure(words, lyric_style)) + 28, 74.f, std::min(360.f, r.w));
            const float h = words.empty() ? 32.f : 56.f;
            const float x = std::clamp(ui.input().x - w / 2, r.x, r.right() - w);
            const Rect bubble{x, r.y - h - 8 - 3 * preview, w, h};
            Fade fade(p, preview);
            p.shadow({bubble.x, bubble.y + 4, bubble.w, bubble.h}, t.radius, 18, t.shadow.with_alpha(0.6f));
            p.rect(bubble, t.elevated, t.radius, t.border, 1);
            ui.label({bubble.x + 14, bubble.y + 6, bubble.w - 28, 20}, time, time_style, Align::center);
            if (!words.empty()) {
                ui.label({bubble.x + 14, bubble.y + 29, bubble.w - 28, 20}, words, lyric_style, Align::center);
            }
            p.rect({std::clamp(ui.input().x, r.x, r.right()), cy - 7, 1, 14}, t.text.with_alpha(0.3f));
        }
    }

    void Shell::player_bar(ui::Ui &ui, Rect r, float stage) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        seek_string(ui, {r.x + 20, r.y, r.w - 40, player_string_h}, stage);
        float side_w = std::min(330.f, std::round(r.w * 0.3f));
        ui::cut_left(r, side_w);
        Rect right = ui::cut_right(r, side_w), mid = r;
        const Track *tr = current();
        auto st = player_.state();

        // Transport; play and pause morph into each other, a ring turns while the track loads or buffers.
        float cx = std::round(mid.x + mid.w / 2), cy = mid.y + player_string_h + (player_h - player_string_h) / 2;
        static constexpr std::array mode_icons = {"repeat", "repeat-1", "shuffle"};
        const std::array mode_names = {lyren::tr("Repeat all"), lyren::tr("Repeat one"), lyren::tr("Shuffle")};
        if (ui.icon_button(ui.id("mode"), {cx - 124, cy - 16, 32, 32}, mode_icons[static_cast<int>(mode_)], 17, mode_ != Mode::list)) {
            set_mode(static_cast<Mode>((static_cast<int>(mode_) + 1) % 3));
            ui.toast(std::string(mode_names[static_cast<int>(mode_)]), mode_icons[static_cast<int>(mode_)]);
        }
        // Without a track the rest of the transport and the progress bar are dimmed and do not respond.
        ui.disabled = !tr;
        {
            Fade dim(p, tr ? 1 : 0.4f);
            if (ui.icon_button(ui.id("prev"), {cx - 72, cy - 16, 32, 32}, "skip-back", 18)) {
                skip(-1);
            }
            ui::Id pid = ui.id("play");
            Rect play_r{cx - 21, cy - 21, 42, 42};
            auto it = ui.interact(pid, play_r);
            const auto [hot, down] = ui.feel(pid, it);
            Rect pr = play_r.inset(-2 * hot + 2.5f * down);
            p.shadow({pr.x, pr.y + 3, pr.w, pr.h}, pr.w / 2, 10, t.shadow.with_alpha(0.5f * hot));
            p.rect(pr, t.text.mix(t.accent, 0.12f), pr.w / 2);
            float morph = ui.anim(ui.id("play-morph"), playing() ? 1 : 0, 260, ease::emphasized);
            {
                Fade fade(p, 1 - morph);
                p.icon("play-fill", ui::center(pr, 18 - 6 * morph, 18 - 6 * morph), t.bg);
            }
            {
                Fade fade(p, morph);
                p.icon("pause-fill", ui::center(pr, 12 + 6 * morph, 12 + 6 * morph), t.bg);
            }
            bool waiting = want_play_ && (fetching_ || st == State::loading || player_.buffering());
            float ring = ui.anim(ui.id("play-ring"), waiting ? 1 : 0, 200);
            if (ring > 0.01f) {
                Fade fade(p, ring);
                ui.spinner(pr.inset(-5), t.accent, 2);
            }
            if (it.clicked) {
                toggle_play();
            }
            if (ui.icon_button(ui.id("next"), {cx + 40, cy - 16, 32, 32}, "skip-forward", 18)) {
                skip(1);
            }
            if (ui.icon_button(ui.id("lyrics"), {cx + 92, cy - 16, 32, 32}, "mic-vocal", 17, np_open_)) {
                show_stage(!np_open_);
            }
        }

        // Volume and panels; narrow windows keep only the icons.
        float vol_w = ui.spring(ui.id("vol-w"), compact_ ? 0 : 100, 280);
        Rect vol{right.right() - 20 - vol_w, cy - 8, vol_w, 16};
        float shown = muted_ ? 0 : volume_;
        if (vol_w > 8) {
            Fade fade(p, vol_w / 100);
            if (ui.slider(ui.id("volume"), vol, shown, {.accent = false})) {
                volume_ = shown, muted_ = false;
                apply_volume();
                changed();
            }
        }
        const char *vol_icon = shown == 0 ? "volume-x" : shown < 0.5f ? "volume-1" : "volume-2";
        if (ui.icon_button(ui.id("mute"), {vol.x - 40, cy - 16, 32, 32}, vol_icon, 18)) {
            muted_ = !muted_;
            apply_volume();
            changed();
        }
        if (ui.icon_button(ui.id("queue"), {vol.x - 80, cy - 16, 32, 32}, "list-music", 18, panel_ == Panel::queue)) {
            show_panel(panel_ == Panel::queue ? Panel::none : Panel::queue);
        }
        const Rect desktop{vol.x - 120, cy - 16, 32, 32};
        const auto toggle = ui.interact(ui.id("desktop-lyrics"), desktop);
        const float hover = ui.anim(ui.id("desktop-hover"), toggle.hovered ? 1 : 0, 150);
        p.rect(desktop, (dl_enabled_ ? t.accent : t.text).with_alpha((dl_enabled_ ? 0.12f : 0) + hover * 0.08f), t.radius_s());
        p.icon("captions", ui::center(desktop, 18, 18), dl_enabled_ ? t.accent : t.text_dim.mix(t.text, hover));
        if (toggle.clicked) {
            set_desktop_enabled(!dl_enabled_);
        }
        if (toggle.right_clicked) {
            std::vector<ui::MenuItem> menu;
            for (bool island : {false, true}) {
                menu.push_back({dl_enabled_ && dl_island_ == island ? "check"
                                : island                            ? "panel-top"
                                                                    : "panel-bottom",
                                std::string(lyren::tr(island ? "Dynamic Island" : "Floating lyrics")), [this, island] {
                                    set_desktop_island(island);
                                    set_desktop_enabled(true);
                                }});
            }
            if (dl_enabled_) {
                menu.push_back({"x", std::string(lyren::tr("Hide desktop lyrics")), [this] {
                                    set_desktop_enabled(false);
                                }});
            }
            ui.open_menu(desktop.x, desktop.y, std::move(menu));
        }

        float time_right = desktop.x - 10;

        // On the stage, before the desktop lyrics: the comments on the song on show, under the panel they open, and the
        // translation of its lyrics when they have one, which keeps its place while the next song's lyrics load. They
        // come in once the stage is nearly up.
        if (const float shown = smooth01((stage - 0.7f) / 0.3f); tr && shown > 0) {
            Fade fade(p, shown);
            ui.disabled = shown < 0.9f;
            const float x = desktop.x + 8 * (1 - shown);
            time_right -= 40;
            const bool following = thread_.follows && thread_.subject == Subject::song && thread_.id == tr->id && thread_.source == tr->source;
            const bool on = panel_ == Panel::comments && following;
            if (provider(*tr).capabilities.comments[0] && ui.icon_button(ui.id("comments"), {x - 40, cy - 16, 32, 32}, "message-square", 17, on)) {
                if (on) {
                    show_panel(Panel::none);
                } else {
                    open_comments(ui, tr->source, Subject::song, tr->id, tr->title + " · " + tr->artist, tr->cover, true);
                }
            }
            const bool translated = std::ranges::any_of(lyrics_, [](const LyricLine &line) {
                return !line.translation.empty();
            });
            const ui::Id tid = ui.id("translation"), present_id = ui::sub_id(tid, 1);
            const float present = ui.spring(present_id, lyrics_loading_ ? ui.spring_value(present_id, 0) : translated ? 1 : 0, 260);
            if (present > 0.01f) {
                time_right -= 40 * present;
                Fade appear(p, std::clamp(present, 0.f, 1.f));
                ui.disabled = ui.disabled || present < 0.5f;
                if (ui.icon_button(tid, {x - 80, cy - 16, 32, 32}, "languages", 17, lyric_translation_)) {
                    toggle_translation();
                }
            }
            ui.disabled = false;
        }
        const auto clock = timeline();
        const double position = st == State::idle && !fetching_ ? resume_at_ : last_pos_;
        const std::string time = mmss(dragging_ ? drag_v_ * clock.duration() : clock.to_song(position)) + " / " + mmss(clock.duration());
        const TextStyle time_style{.size = t.fs(11), .color = t.text_dim, .features = text::tabular_nums};
        const float time_w = p.measure(time, time_style);
        if (time_right - time_w > cx + 136) {
            ui.label({time_right - time_w, cy - 10, time_w, 20}, time, time_style);
        }
    }

    float Shell::heart_size(ui::Ui &ui, const Track &t, float size) const {
        if (ui.theme().motion == 0) {
            return size;
        }
        float k = static_cast<float>(ui.now - pop_time_) / (0.45f * ui.theme().motion);
        if (!(k >= 0 && k < 1) || pop_key_ != key(t)) {
            return size;
        }
        ui.keep_animating();
        // Up past its size and back, then a little under it.
        float s = k < 0.55f ? 1 + 0.32f * std::sin(std::numbers::pi_v<float> * k / 0.55f) : 1 - 0.06f * std::sin(std::numbers::pi_v<float> * (k - 0.55f) / 0.45f);
        return size * s;
    }

} // namespace lyren
