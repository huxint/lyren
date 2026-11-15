#include "app/view/player_bar.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    namespace ease = ui::ease;
    namespace type = ui::type;
    namespace pace = ui::pace;

    PlayerBar::Layout PlayerBar::layout(Rect strip) {
        Layout l;
        l.card = {strip.x + margin, strip.y + margin, strip.w - 2 * margin, strip.h - 2 * margin};
        Rect r = l.card.inset(14, 0);
        // Both sides are equally wide, so the transport stands in the middle of the window.
        const float side = std::min(320.f, std::round(l.card.w * 0.3f));
        const Rect left = ui::cut_left(r, side);
        l.tools = ui::cut_right(r, side);
        l.centre = r;
        l.cover = {left.x, l.card.y + (l.card.h - 48) / 2, 48, 48};
        l.caption = {l.cover.right() + 12, l.cover.y, left.right() - l.cover.right() - 20, l.cover.h};
        return l;
    }

    void PlayerBar::draw(ui::Ui &ui, Rect strip, float stage) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Layout l = layout(strip);
        // A card on the window; on the stage, a pane of glass over the cover.
        if (const float lift = smooth01(1 - 2 * stage); lift > 0 && !t.dark) {
            p.shadow({l.card.x, l.card.y + 1, l.card.w, l.card.h}, t.radius_l(), 10, t.shadow.with_alpha(0.45f * lift));
        }
        p.rect(l.card, t.surface, t.radius_l(), t.line(stage), 1);
        const Rect centre = ui::center(l.centre, std::min(l.centre.w, 640.f), l.centre.h);
        transport(ui, {centre.x, l.card.y + 10, centre.w, 36});
        string(ui, {centre.x, l.card.y + 49, centre.w, 20}, l.card.y - 6, stage);
        tools(ui, l.tools, stage);
    }

    void PlayerBar::transport(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const float cx = std::round(r.x + r.w / 2), cy = r.y + r.h / 2;
        static constexpr std::array<std::string_view, 3> mode_icons{"repeat", "repeat-1", "shuffle"}, mode_names{"Repeat all", "Repeat one", "Shuffle"};
        const auto mode = static_cast<size_t>(playback_.mode());
        if (ui.icon_button(ui.id("mode"), {cx - 110, cy - 16, 32, 32}, mode_icons[mode], 17, playback_.mode() != PlayMode::list, lyren::tr(mode_names[mode]))) {
            const size_t next = (mode + 1) % mode_icons.size();
            playback_.set_mode(static_cast<PlayMode>(next));
            ui.toast(std::string(lyren::tr(mode_names[next])), std::string(mode_icons[next]));
        }
        // With nothing queued the rest of the transport is dimmed and does not respond.
        ui::Enabled live(ui, playback_.current() != nullptr);
        if (ui.icon_button(ui.id("prev"), {cx - 66, cy - 16, 32, 32}, "skip-back-fill", 17, false, lyren::tr("Previous"))) {
            playback_.skip(-1);
        }
        {
            // Play and pause turn into each other; a ring turns round them while the song is on its way.
            const ui::Id id = ui.id("play");
            const Rect at{cx - 18, cy - 18, 36, 36};
            const auto it = ui.interact(id, at);
            const auto [hot, down] = ui.feel(id, it);
            const Rect disc = at.inset(-1.5f * hot + 2.5f * down);
            p.rect(disc, t.text, disc.w / 2);
            const float morph = ui.anim(ui.id("play-morph"), playback_.playing() ? 1 : 0, 240, ease::emphasized);
            {
                // The triangle's weight sits left of its box, so it stands a little right of the centre.
                Fade fade(p, 1 - morph);
                Rect glyph = ui::center(disc, 15 - 5 * morph, 15 - 5 * morph);
                glyph.x += 1;
                p.icon("play-fill", glyph, t.bg);
            }
            {
                Fade fade(p, morph);
                p.icon("pause-fill", ui::center(disc, 10 + 5 * morph, 10 + 5 * morph), t.bg);
            }
            if (const float ring = ui.anim(ui.id("play-ring"), playback_.waiting() ? 1 : 0, 200); ring > 0.01f) {
                Fade fade(p, ring);
                ui.spinner(disc.inset(-4), t.accent, 2);
            }
            if (it.clicked) {
                playback_.toggle();
            }
        }
        if (ui.icon_button(ui.id("next"), {cx + 34, cy - 16, 32, 32}, "skip-forward-fill", 17, false, lyren::tr("Next"))) {
            playback_.skip(1);
        }
        if (ui.icon_button(ui.id("lyrics"), {cx + 78, cy - 16, 32, 32}, "mic-vocal", 17, shell_.stage.open(), lyren::tr("Lyrics"))) {
            shell_.stage.show(!shell_.stage.open());
        }
    }

    void PlayerBar::string(ui::Ui &ui, Rect r, float above, float stage) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto clock = playback_.timeline();
        const double duration = clock.duration(), file_duration = clock.file_duration(), position = playback_.position();
        const bool seekable = playback_.current() && duration > 0;

        // The clock stands at the string's ends: where the song is, and how long it is.
        const std::string whole = mmss(duration);
        const auto numbers = t.style(type::micro, t.faint(), text::tabular_nums);
        const float clock_w = std::ceil(std::max(p.measure(whole, numbers), p.measure("0:00", numbers))) + 2;
        const Rect elapsed = ui::cut_left(r, clock_w), total = ui::cut_right(r, clock_w);
        r = r.inset(10, 0);
        const auto [from, to] = clock.playable_span();
        const float sx = r.x + r.w * from, width = r.w * (to - from), cy = r.y + r.h / 2;

        const ui::Id id = ui.id("progress");
        ui::Interaction it;
        {
            ui::Enabled possible(ui, seekable, 1);
            it = ui.interact(id, r);
        }
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
            pluck_time_ = ui.now, pluck_at_ = drag_v_;
            playback_.seek(drag_v_ * duration);
        }
        const bool reaching = it.hovered || it.held;
        if (reaching) {
            ui.cursor = wl::Cursor::pointer;
        }
        const float hot = ui.anim(ui.id("string-hover"), reaching ? 1.f : 0.f, 150);
        const bool held = dragging_ || released;
        const float value = ui.spring(ui.id("progress-v"), held ? drag_v_ : clock.progress(position), held ? 0 : 240);
        const float filled = std::clamp(r.w * (value - from), 0.f, width);
        const float thickness = 2 + hot;

        // While the music sounds the played part is a wave that swells with it, and letting go plucks the string.
        const bool wave = env_.config.interface.progress == ProgressStyle::wave && t.motion > 0;
        meter_.follow(playback_.player().levels(), static_cast<float>(ui.now - meter_time_));
        meter_time_ = ui.now;
        const float live = ui.anim(ui.id("string-live"), playback_.playing() && !playback_.player().buffering() && wave ? 1.f : 0.f, 500, ease::standard);
        const float amplitude = ui.spring(ui.id("string-amplitude"), (2 + 0.6f * stage + 1.6f * meter_.all()) * live, 240);
        const float phase = -static_cast<float>(std::fmod(ui.now * 0.7, 1.0)) * 2 * std::numbers::pi_v<float>;
        ui::Pluck pluck;
        const float age = t.motion > 0 ? static_cast<float>((ui.now - pluck_time_) / t.motion) : 1.f;
        if (wave && age < 0.7f) {
            pluck = {r.w * (pluck_at_ - from), 7 * std::exp(-age * 7) * std::sin(age * 30), 32};
            ui.keep_animating();
        }
        if (amplitude > 0.01f && filled > 0) {
            ui.keep_animating();
        }
        // What a preview's file leaves out of the song is dotted.
        for (float x = r.x; x + 3 <= sx; x += 7) {
            p.rect({x, cy - 1, 3, 2}, t.text.with_alpha(0.12f), 1);
        }
        for (float x = sx + width + 4; x + 3 <= r.right(); x += 7) {
            p.rect({x, cy - 1, 3, 2}, t.text.with_alpha(0.12f), 1);
        }
        p.wave({sx, cy - thickness / 2, width, thickness}, filled, amplitude, 42, phase, t.accent, t.text.with_alpha(0.14f), pluck);
        // What has been fetched ahead of the song.
        if (const audio::Spool *spool = playback_.player().spool(); spool && file_duration > 0) {
            const auto [have, size] = spool->progress();
            if (size && have < size) {
                for (const auto [begin, end] : spool->ranges()) {
                    auto x = [&](uint64_t byte) {
                        return r.x + r.w * clock.progress(file_duration * static_cast<double>(byte) / static_cast<double>(size));
                    };
                    const float left = std::max({x(begin), sx + filled, sx}), right = std::min(x(end), sx + width);
                    if (right > left) {
                        p.rect({left, cy - thickness / 2, right - left, thickness}, t.text.with_alpha(0.14f), thickness / 2);
                    }
                }
            }
        }
        // The chorus, marked under the string.
        if (const auto [chorus_from, chorus_to] = playback_.highlight(); chorus_to > chorus_from && duration > 0) {
            const float left = r.x + r.w * clock.fraction(chorus_from), right = r.x + r.w * clock.fraction(chorus_to);
            p.rect({left, cy + 4, std::max(right - left, 2.f), 2}, t.accent.with_alpha(0.3f + 0.15f * hot), 1);
        }
        if (seekable) {
            // A bead rides the string where the song stands, and grows under the pointer.
            const float bead = 4 + 6 * hot;
            const auto pin = [](float x) {
                x = std::clamp(x, 0.f, 1.f);
                return x * x * (3 - 2 * x);
            };
            const float u = (filled - pluck.at) / pluck.spread;
            const float lift = pluck.lift * std::exp(-u * u) * pin((filled - thickness / 2) / (2 * pluck.spread)) * pin((width - thickness / 2 - filled) / (2 * pluck.spread));
            const Rect knob{sx + filled - bead / 2, cy - lift - bead / 2, bead, bead};
            if (hot > 0) {
                p.shadow(knob, bead / 2, 8, t.shadow.with_alpha(hot));
            }
            p.rect(knob, t.accent.mix(white, hot), bead / 2);
        }
        const double song = held ? drag_v_ * duration : clock.to_song(position);
        ui.label(elapsed, mmss(song), t.style(type::micro, t.faint().mix(t.text, 0.5f * hot), text::tabular_nums));
        ui.label(total, whole, numbers, Align::right);
        // The next frame is due when the string has moved a pixel or the clock a second.
        if (playback_.playing() && duration > 0) {
            ui.wake(ui.now + std::max(std::min(duration / std::max(r.w, 1.f), 1 - std::fmod(song, 1.0)), 0.02));
        }

        // The time under the pointer and the line sung then stand over the console, clear of the transport.
        const float preview = ui.anim(ui.id("seek-preview"), reaching && seekable ? 1.f : 0.f, 140);
        if (preview <= 0) {
            return;
        }
        const double at = std::clamp(pointer, 0.f, 1.f) * duration;
        const std::string time = mmss(at);
        const LyricLine *line = playback_.line_at(*playback_.current(), at);
        const std::string_view words = line ? std::string_view(line->text) : std::string_view{};
        const auto time_style = t.style(type::caption, t.text_dim, text::tabular_nums), words_style = t.style(type::label, t.text, text::medium);
        const float w = std::clamp(std::max(p.measure(time, time_style), p.measure(words, words_style)) + 28, 74.f, std::min(360.f, ui.width - 32));
        const float h = words.empty() ? 30.f : 54.f;
        const Rect bubble{std::round(std::clamp(ui.input().x - w / 2, 16.f, ui.width - 16 - w)), std::round(above - h + 4 * (1 - preview)), w, h};
        Fade fade(p, preview);
        p.shadow({bubble.x, bubble.y + 4, bubble.w, bubble.h}, t.radius, 18, t.shadow.with_alpha(0.6f));
        ui::Color paper = t.elevated;
        paper.a = 1;
        p.rect(bubble, paper, t.radius, t.border, 1);
        ui.label({bubble.x + 14, bubble.y + 5, bubble.w - 28, 20}, time, time_style, Align::center);
        if (!words.empty()) {
            ui.label({bubble.x + 14, bubble.y + 27, bubble.w - 28, 20}, words, words_style, Align::center);
        }
        p.rect({std::clamp(ui.input().x, r.x, r.right()), cy - 7, 1, 14}, t.text.with_alpha(0.3f));
    }

    void PlayerBar::tools(ui::Ui &ui, Rect r, float stage) {
        auto &p = ui.painter();
        const float cy = r.y + r.h / 2;
        const Track *track = playback_.current();

        // The volume: a narrow window keeps only its icon, and the wheel turns it over either.
        const float slider_w = ui.spring(ui.id("volume-w"), shell_.compact() ? 0 : 88, pace::glide);
        const Rect slider{r.right() - 2 - slider_w, cy - 10, slider_w, 20};
        float volume = playback_.volume();
        if (slider_w > 8) {
            Fade fade(p, std::clamp(slider_w / 88, 0.f, 1.f));
            if (ui.slider(ui.id("volume"), slider, volume, {.accent = false})) {
                playback_.set_volume(volume);
            }
        }
        const Rect speaker{slider.x - (slider_w > 8 ? 38.f : 32.f), cy - 16, 32, 32};
        const bool silent = volume == 0;
        if (ui.icon_button(ui.id("mute"), speaker, silent ? "volume-x" : volume < 0.5f ? "volume-1" : "volume-2", 18, false, lyren::tr(silent ? "Unmute" : "Mute"))) {
            playback_.toggle_mute();
        }
        if (auto &wheel = ui.input().scroll_y; wheel != 0 && ui.hovering({speaker.x, speaker.y, r.right() - speaker.x, speaker.h})) {
            // A notch of the wheel is a twentieth of the range.
            playback_.set_volume(playback_.volume() - wheel / 48 * 0.05f);
            wheel = 0;
            ui.redraw();
        }
        float x = speaker.x - 4;
        if (ui.icon_button(ui.id("queue"), {x - 32, cy - 16, 32, 32}, "list-music", 18, shell_.panel.kind() == SidePanel::Kind::queue, lyren::tr("Queue"))) {
            shell_.panel.toggle(SidePanel::Kind::queue);
        }
        x -= 36;
        {
            // The lyrics on the desktop: a click turns them on and off, a right click picks how they show.
            const Rect at{x - 32, cy - 16, 32, 32};
            auto &desktop = shell_.desktop;
            const bool pick = ui.context_click(at);
            if (ui.icon_button(ui.id("desktop-lyrics"), at, "captions", 18, desktop.enabled(), lyren::tr("Desktop lyrics"))) {
                desktop.set_enabled(!desktop.enabled());
            }
            if (pick) {
                std::vector<ui::MenuItem> menu;
                for (bool island : {false, true}) {
                    const bool on = desktop.enabled() && desktop.is_island() == island;
                    menu.push_back({on       ? "check"
                                    : island ? "panel-top"
                                             : "panel-bottom",
                                    std::string(lyren::tr(island ? "Dynamic Island" : "Floating lyrics")), [this, island] {
                                        shell_.desktop.set_island(island);
                                        shell_.desktop.set_enabled(true);
                                    }});
                }
                if (desktop.enabled()) {
                    menu.push_back({"x", std::string(lyren::tr("Hide desktop lyrics")), [this] {
                                        shell_.desktop.set_enabled(false);
                                    }});
                }
                ui.open_menu(at.x, at.y - 6, std::move(menu));
            }
            x -= 36;
        }

        // On the stage the console also holds what belongs to the song on show: its comments, and the translation of
        // its lyrics when they have one. They come in once the stage is nearly up.
        const float shown = smooth01((stage - 0.7f) / 0.3f);
        if (!track || shown <= 0) {
            return;
        }
        Fade fade(p, shown);
        ui::Enabled reachable(ui, shown >= 0.9f, 1);
        x += 8 * (1 - shown);
        if (provider(*track).capabilities.comments[0]) {
            const auto &thread = comments_.thread();
            const bool on = shell_.panel.kind() == SidePanel::Kind::comments && thread.follows && thread.is(track->source, Subject::song, track->id);
            if (ui.icon_button(ui.id("comments"), {x - 32, cy - 16, 32, 32}, "message-square", 17, on, lyren::tr("Comments"))) {
                if (on) {
                    shell_.panel.show(SidePanel::Kind::none);
                } else {
                    shell_.open_comments(track->source, Subject::song, track->id, track->title + " · " + track->artist, track->cover, true);
                }
            }
            x -= 36;
        }
        // The button keeps its place while the next song's lyrics load.
        const bool translated = std::ranges::any_of(playback_.lyrics(), [](const LyricLine &line) {
            return !line.translation.empty();
        });
        const ui::Id id = ui.id("translation"), present_id = ui::sub_id(id, 1);
        const float present = ui.spring(present_id, playback_.lyrics_loading() ? ui.spring_value(present_id, 0) : translated ? 1 : 0, 260);
        if (present > 0.01f) {
            Fade appear(p, std::clamp(present, 0.f, 1.f));
            ui::Enabled there(ui, present >= 0.5f, 1);
            if (ui.icon_button(id, {x - 32, cy - 16, 32, 32}, "languages", 17, shell_.stage.translation(), lyren::tr("Translation"))) {
                shell_.stage.toggle_translation();
            }
        }
    }

} // namespace lyren
