#include "app/view/stage.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "app/state.hpp"
#include "app/view/draw.hpp"
#include "app/view/shell.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    namespace ease = ui::ease;
    namespace type = ui::type;

    namespace {
        // The size the lyrics are set in, before the theme's text size scales it.
        constexpr float lyric_size = 26;

        // Wide, a large cover with the lyrics beside it; narrow, a small cover above them.
        struct Layout {
            Rect cover, lyrics;
        };

        Layout layout(Rect page, bool wide) {
            Rect body = page.inset(56, 36);
            if (!wide) {
                return {{body.x, body.y + 4, 64, 64}, {body.x, body.y + 96, body.w, body.h - 96}};
            }
            const Rect left = ui::cut_left(body, std::round(body.w * 0.42f));
            ui::cut_left(body, 40);
            const float side = std::floor(std::min({left.w - 20, left.h - 132, 400.f}));
            return {{left.x + (left.w - side) / 2, left.y + (left.h - side - 110) / 2, side, side}, body};
        }

        // Where the title and the artist stand: the title's baseline origin, the two sizes, the artist's baseline below
        // the title's, and the width the lines may take.
        struct Caption {
            float x, y, title, sub, gap, w;

            Caption mix(const Caption &to, float k) const {
                return {std::lerp(x, to.x, k), std::lerp(y, to.y, k), std::lerp(title, to.title, k), std::lerp(sub, to.sub, k), std::lerp(gap, to.gap, k), std::lerp(w, to.w, k)};
            }
        };

        // The quality a service granted, in the listener's words.
        std::string_view quality_name(std::string_view level) {
            return level == "standard" || level == "medium"  ? lyren::tr("Standard")
                   : level == "higher"                       ? lyren::tr("High")
                   : level == "exhigh" || level == "highest" ? lyren::tr("Very high")
                   : level == "lossless"                     ? lyren::tr("Lossless")
                   : level == "hires" || level == "hi_res"   ? "Hi-Res"
                   : level == "aac96"                        ? "AAC 96 kbps"
                                                             : level;
        }
    } // namespace

    Stage::Stage(Shell &shell, const SavedState &saved)
    : Part(shell),
      translation_(saved.lyrics_translation) {}

    void Stage::save(SavedState &to) const {
        to.lyrics_translation = translation_;
    }

    float Stage::raised(ui::Ui &ui) {
        return ui.spring(ui.id("stage-up"), open_ ? 1 : 0, 400);
    }

    void Stage::show(bool open) {
        if (!open && shell_.panel.kind() == SidePanel::Kind::comments && comments_.thread().follows) {
            shell_.panel.show(SidePanel::Kind::none);
        }
        if (open_ != open) {
            open_ = open;
            env_.redraw();
        }
    }

    void Stage::toggle_translation() {
        translation_ = !translation_;
        shell_.changed();
        env_.redraw();
    }

    float Stage::fit(ui::Ui &ui, Rect page) {
        // The slack keeps resizing near the breakpoint from flipping the layout back and forth.
        if (page.w >= 912) {
            wide_ = true;
        } else if (page.w < 888) {
            wide_ = false;
        }
        return ui.spring(ui.id("stage-wide"), wide_ ? 1 : 0, 420);
    }

    void Stage::art(ui::Ui &ui, Rect strip, Rect page, float np, float wide) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto console = PlayerBar::layout(strip);
        const Rect small = console.cover;
        cover_px_ = small.w * p.scale();
        const Track *track = playback_.current();
        if (!track) {
            if (np < 1) {
                Fade fade(p, 1 - np);
                p.rect(small, t.fill(1.4f), t.radius_s());
                p.icon("music", ui::center(small, 20, 20), t.faint());
                ui.label({console.caption.x, small.y, console.caption.w, small.h}, lyren::tr("Nothing playing"), t.style(type::label, t.text_dim));
            }
            return;
        }

        // On the stage the text sits under a large cover and beside a small one; it goes round the cover's corner
        // while the cover is small.
        Layout large = layout(page, true);
        const Layout stacked = layout(page, false);
        const Rect body = page.inset(56, 44);
        const float presence = t.motion > 0 ? ui.spring(ui.id("cover-presence"), playback_.playing() ? 1.f : 0.f, 520) : 1.f;
        const float resting = 0.96f + 0.04f * presence;
        large.cover = ui::center(large.cover, large.cover.w * resting, large.cover.h * resting);
        const Rect staged = mix(stacked.cover, large.cover, wide);
        const float under = smooth01((staged.w - stacked.cover.w) / (std::min(160.f, large.cover.w) - stacked.cover.w));
        const Caption beside{staged.right() + 20, staged.y + 24, t.fs(type::title), t.fs(type::label), 24, body.right() - staged.right() - 20};
        const Caption below{staged.x, staged.bottom() + 46, t.fs(type::display), t.fs(type::lead), 28, staged.w};
        Caption on_stage = beside.mix(below, under);
        on_stage.x = std::lerp(beside.x, below.x, smooth01((under - 0.4f) / 0.6f));
        on_stage.y = std::lerp(beside.y, below.y, smooth01(under / 0.6f));

        // From the console to a small cover the text stays beside it. To a large one the cover first rises out of the
        // console and then grows, and the text sets off once the cover is out of its way.
        const Caption in_console{console.caption.x, small.y + 20, t.fs(type::label), t.fs(type::caption), 18, console.caption.w};
        const float rise = std::lerp(np, 1 - (1 - np) * (1 - np), under), grow = std::lerp(np, np * np, under);
        const float size = std::lerp(small.w, staged.w, grow);
        const Rect cover{std::lerp(small.x, staged.x, rise), std::lerp(small.bottom(), staged.bottom(), rise) - size, size, size};
        const float late = smooth01((np - 0.35f) / 0.65f), f = std::lerp(np, late, under);
        Caption cap = in_console.mix(on_stage, f);
        cap.x = std::lerp(cover.right() + std::lerp(12.f, 20.f, np) + (on_stage.x - beside.x) * np, std::lerp(in_console.x, on_stage.x, late), under);
        cap.y = std::lerp(cover.y + std::lerp(20.f, 24.f, np) + (on_stage.y - beside.y) * np, std::lerp(in_console.y, on_stage.y, late), under);

        // The larger the cover, the higher it floats over the stage.
        const float depth = smooth01((size - 64) / 136);
        const float radius = std::lerp(t.radius_s(), t.radius_l(), depth);
        if (depth > 0) {
            p.shadow({cover.x, cover.y + 12, cover.w, cover.h}, radius, 64, t.accent.with_alpha(0.08f * depth * presence));
        }
        p.shadow({cover.x, cover.y + std::lerp(2.f, 18.f, depth), cover.w, cover.h}, radius, std::lerp(8.f, 48.f, depth), t.shadow.with_alpha(std::lerp(0.4f, 0.9f, depth)));

        // The thumbnail stays through the move; the stage's larger picture fades in over it once it has loaded.
        const float change = ui.progress(playback_.changed_at(), 520);
        const int preview_px = Images::tier(small.w * p.scale()), sharp_px = Images::tier((wide_ ? 420.f : stacked.cover.w) * p.scale());
        auto artwork = [&](const std::string &url) {
            const auto &preview = env_.images.get(url, static_cast<uint32_t>(preview_px));
            const float shown = preview.ready ? ui.progress(preview.ready_at, 320, ease::standard) : 0;
            if (shown < 1) {
                p.rect(cover, t.fill(1.4f), radius);
                p.set_zoom(cover.w * 0.36f / 24, cover.x + cover.w / 2, cover.y + cover.h / 2);
                p.icon("music", ui::center(cover, 24, 24), t.text_dim.with_alpha(0.5f * (1 - shown)));
                p.set_zoom(1);
            }
            if (preview.ready) {
                Fade fade(p, shown);
                p.image(cover, preview.tex, radius);
            }
            if (np > 0 && sharp_px > preview_px) {
                if (const auto &sharp = env_.images.get(url, static_cast<uint32_t>(sharp_px)); sharp.ready) {
                    Fade fade(p, ui.progress(sharp.ready_at, 240, ease::standard));
                    p.image(cover, sharp.tex, radius);
                }
            }
        };
        const Track &before = playback_.previous();
        if (change < 1 && !before.cover.empty()) {
            artwork(before.cover);
        }
        {
            Fade fade(p, change);
            artwork(track->cover);
        }

        // While it moves, the text is scaled from glyphs at the stage's sizes instead of being rasterized at a new
        // size every frame. A line that does not fit fades out at its end.
        const bool settled = np == 0 || (np == 1 && (under == 0 || under == 1));
        auto line = [&](float x, float y, std::string_view s, float size, float raster, ui::Color color, float w, uint8_t features = 0) {
            if (settled) {
                raster = size;
            }
            const float zoom = size / raster, limit = w / zoom;
            p.set_zoom(zoom, x, y);
            const TextStyle style{.size = raster, .color = color, .features = features};
            const float width = p.measure(s, style);
            if (width > limit) {
                p.set_wipe(x + limit - 16 / zoom, 32 / zoom, color.with_alpha(0));
            }
            p.text(x, y, s, style);
            p.clear_wipe();
            p.set_zoom(1);
            return std::min(width, limit) * zoom;
        };
        // The album and the stream's format belong to the stage and come in as the text arrives there.
        const float detail = smooth01((f - 0.6f) / 0.4f);
        float text_w = 0;
        auto caption = [&](const Track &song, float alpha, float dy, bool live) {
            Fade fade(p, alpha);
            const float y = cap.y + dy, k = cap.sub / t.fs(type::lead);
            const bool likes = provider(song).capabilities.likes;
            const float title_w = line(cap.x, y, song.title, cap.title, t.fs(type::display), t.text, cap.w - (likes ? 40 : 0), f > 0.5f ? text::semibold : text::medium);
            const float artist_w = line(cap.x, y + cap.gap, song.artist, cap.sub, t.fs(type::lead), t.text_dim, cap.w);
            if (live) {
                text_w = std::max(title_w + (likes ? 40 : 0), artist_w);
            }
            if (detail > 0 && under > 0 && !song.album.empty()) {
                Fade more(p, detail * under);
                line(cap.x + artist_w, y + cap.gap, " · " + song.album, cap.sub, t.fs(type::lead), t.text_dim, cap.w - artist_w);
            }
            if (const auto &player = playback_.player(); detail > 0 && live && !player.codec().empty()) {
                Fade more(p, detail);
                std::string format = std::format("{} · {:.1f} kHz", player.codec(), player.format().rate / 1000.0);
                if (!playback_.level().empty()) {
                    format = std::string(quality_name(playback_.level())) + " · " + format;
                }
                if (playback_.trial()) {
                    format += " · ";
                    format += lyren::tr("Preview");
                }
                line(cap.x, y + cap.gap + 24 * k, format, t.fs(type::caption) * k, t.fs(type::caption), t.faint(), cap.w);
            }
            // The heart follows the title.
            if (!likes) {
                return;
            }
            const float heart_px = std::lerp(16.f, 20.f, f);
            const Rect at{cap.x + std::min(title_w + 8, cap.w - 32), y - 0.35f * cap.title - 16, 32, 32};
            if (live && settled) {
                heart(ui, ui.id("np-like"), at, song, heart_px);
                return;
            }
            const bool liked = library_.liked(song);
            p.set_zoom(heart_size(ui, song, heart_px) / 20, at.x + 16, at.y + 16);
            p.icon(liked ? "heart-fill" : "heart", ui::center(at, 20, 20), liked ? t.accent : t.text_dim);
            p.set_zoom(1);
        };
        // A new song's text rises in as the old one leaves upwards.
        const float out = smooth01(change / 0.45f), in = smooth01((change - 0.25f) / 0.75f);
        if (out < 1 && !before.title.empty()) {
            caption(before, 1 - out, -8 * out, false);
        }
        caption(*track, in, 8 * (1 - in), true);

        // In the console, what plays is the way up to the stage.
        if (np > 0) {
            return;
        }
        const ui::Id id = ui.id("bar-cover");
        const auto it = ui.interact(id, {cover.x, cover.y, console.caption.x - cover.x + std::min(text_w, console.caption.w), cover.h});
        if (const float hot = ui.anim(id, it.hovered ? 1 : 0, 150); hot > 0) {
            p.rect(cover, ui::Color{0, 0, 0, 0.45f * hot}, radius);
            p.icon("chevron-up", ui::center(cover, 20, 20), white.with_alpha(hot));
            ui.cursor = wl::Cursor::pointer;
        }
        if (it.clicked) {
            show(true);
        }
    }

    void Stage::draw(ui::Ui &ui, Rect window, Rect page, float k, float wide) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Track *track = playback_.current();
        Fade fade(p, k);
        const float radius = ui.window_radius;

        // The cover supplies a quiet wash behind a steady, dark listening surface.
        p.rect(window, t.bg.mix(t.accent, 0.045f), radius);
        const Rect uv = cover_uv(1, window.w / window.h);
        auto backdrop = [&](const std::string &url, float alpha) {
            if (const auto &e = env_.images.get(url, 128, true); e.ready) {
                Fade art(p, alpha * ui.progress(e.ready_at, 480, ease::standard) * 0.24f);
                p.image(window, e.tex, radius, white, uv);
            }
        };
        const float change = ui.progress(playback_.changed_at(), 620, ease::standard);
        if (change < 1 && !playback_.previous().cover.empty()) {
            backdrop(playback_.previous().cover, 1 - change);
        }
        if (track) {
            backdrop(track->cover, change);
        }
        p.hgradient(window, t.bg.with_alpha(0.12f), t.bg.with_alpha(0.68f), radius);
        p.gradient(window, t.bg.with_alpha(0), t.bg.with_alpha(0.45f), radius);

        // The title strip holds the way back down, on a disc that shows on light covers too.
        {
            const ui::Id id = ui.id("stage-close");
            const Rect at{window.x + 14, window.y + 8, 36, 36};
            const auto it = ui.interact(id, at);
            const auto [hot, down] = ui.feel(id, it);
            const Rect disc = at.inset(down * 2);
            p.rect(disc, white.with_alpha(0.08f + 0.1f * hot), disc.h / 2);
            p.icon("chevron-down", ui::center(at, 20 - down * 1.5f, 20 - down * 1.5f), white.with_alpha(0.72f + 0.28f * hot));
            if (it.hovered && !it.held) {
                ui.tooltip(id, at, lyren::tr("Close"));
            }
            if (it.clicked) {
                show(false);
            }
        }
        ui.label({window.x + 62, window.y + 12, 240, 28}, lyren::tr("Now playing"), t.style(type::label, t.text_dim, text::medium));
        page.y += (1 - k) * 24;
        if (!track) {
            return empty(ui, page, "music", lyren::tr("Nothing playing"), lyren::tr("Play a song to see its lyrics here"));
        }
        // The lyrics move between the column beside the cover and the space under it.
        lyrics(ui, mix(layout(page, false).lyrics, layout(page, true).lyrics, wide), 1 - wide);
    }

    void Stage::lyrics(ui::Ui &ui, Rect r, float centered) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &lines = playback_.lyrics();
        const TextStyle main{.size = t.fs(lyric_size), .features = text::semibold}, sub{.size = t.fs(type::lead)};
        const float main_lh = std::round(main.size * 1.45f), sub_lh = std::round(sub.size * 1.5f), gap = 20, soft = main.size * 0.8f;
        if (playback_.lyrics_loading()) {
            return ui.spinner(ui::center(r, 22, 22), t.text_dim, 2);
        }
        if (lines.empty()) {
            const auto style = t.style(type::heading, t.text_dim);
            const float w = p.measure(lyren::tr("No lyrics available"), style);
            return ui.label({r.x + (r.w - w) / 2 * centered, r.y, w + 1, r.h}, lyren::tr("No lyrics available"), style);
        }
        if (const uint64_t serial = playback_.lyrics_serial(); layout_serial_ != serial || layout_.size() != lines.size() || layout_w_ != r.w || layout_size_ != main.size ||
                                                               layout_font_ != env_.config.theme.font || layout_translated_ != translation_) {
            if (layout_serial_ != serial) {
                line_y_.clear(); // another song's lines start where they belong
            }
            layout_.clear();
            float y = 0;
            for (const auto &l : lines) {
                Block block{.lines = p.wrap(l.text, main.size, r.w, main.features), .y = y};
                if (translation_ && !l.translation.empty()) {
                    block.translation = p.wrap(l.translation, sub.size, r.w);
                }
                block.h = static_cast<float>(block.lines.size()) * main_lh + static_cast<float>(block.translation.size()) * sub_lh;
                y += block.h + gap;
                layout_.push_back(std::move(block));
            }
            layout_serial_ = serial, layout_w_ = r.w, layout_size_ = main.size, layout_font_ = env_.config.theme.font, layout_translated_ = translation_;
        }
        const auto &blocks = layout_;
        const float total = blocks.back().y + blocks.back().h;
        // The line a little ahead, so its highlight lands with the voice rather than after it.
        const double now = playback_.song_time(), lead = 0.2;
        const int current = lyric_at(lines, now + lead);
        // A long pause before a line, at the start or after a word-timed line (the others do not say where they end):
        // three dots take the stage and fill up until the line comes.
        int pause = -1;
        double pause_from = 0, pause_to = 0;
        if (const auto next = static_cast<size_t>(current + 1); next < lines.size()) {
            const LyricLine *before = current >= 0 ? &lines[static_cast<size_t>(current)] : nullptr;
            const double from = !before ? 0 : before->words.empty() ? -1 : before->words.back().time() + before->words.back().duration() + 0.3;
            const double to = lines[next].time - lead;
            if (from >= 0 && to - from >= 5 && now >= from) {
                pause = static_cast<int>(next), pause_from = from, pause_to = to;
            }
        }
        // What the view centres on: 2i is line i, 2i - 1 the dots before it, which push it and the lines after it down.
        const float dots_h = main_lh, room = dots_h + gap;
        const int focus = pause >= 0 ? 2 * pause - 1 : 2 * std::max(current, 0);
        const float anchor = r.h * 0.4f;
        auto aim = [&](int at, size_t i) {
            const int n = (at + 1) / 2;
            const Block &block = blocks[static_cast<size_t>(n)];
            const float centre = (at % 2 ? block.y + dots_h / 2 : block.y + block.h / 2) - anchor;
            return at % 2 && static_cast<int>(i) >= n ? centre - room : centre;
        };
        if (line_y_.size() != blocks.size()) {
            line_y_.clear();
            line_aim_.clear();
            for (size_t i = 0; i < blocks.size(); ++i) {
                line_y_.push_back({aim(focus, i)});
                line_aim_.push_back(aim(focus, i));
            }
            focus_ = focus;
        } else if (focus != focus_) {
            // Only the next line coming up ripples down the lyrics; a jump (a click, a seek, going back) moves them as one.
            ripple_ = focus > focus_ && focus - focus_ <= 2 && ui.now >= manual_until_;
            focus_ = focus, switched_ = ui.now;
        }

        // The wheel browses freely for a few seconds, then the view glides back to the current line.
        if (ui.hovering(r) && ui.input().scroll_y != 0) {
            if (ui.now >= manual_until_) {
                manual_ = line_y_[static_cast<size_t>((focus + 1) / 2)].value;
            }
            manual_ = std::clamp(manual_ + ui.input().scroll_y, -anchor, total - anchor);
            ui.input().scroll_y = 0;
            manual_until_ = ui.now + 3;
            ripple_ = false;
            ui.redraw();
        }
        const bool manual = ui.now < manual_until_;
        if (manual) {
            ui.wake(manual_until_);
        }
        // With a ripple each line below the current one sets off a little later, until then still heading where it
        // was; frames can be far apart, so the time before and after that moment is stepped separately.
        const double last = std::exchange(last_frame_, ui.now);
        bool moving = false;
        const float response = (ripple_ ? 0.5f : 0.6f) * t.motion;
        for (size_t i = 0; i < blocks.size(); ++i) {
            ui::Spring &spring = line_y_[i];
            float &held = line_aim_[i];
            const float target = manual ? manual_ : aim(focus_, i);
            if (t.motion == 0) {
                spring = {target}, held = target;
                continue;
            }
            if (manual) {
                spring.advance(target, static_cast<float>(ui.now - last), 0.18f * t.motion);
                held = target;
            } else {
                const double at = switched_ + (ripple_ ? 0.028 * std::clamp(static_cast<int>(i) - (focus_ + 1) / 2, 0, 5) * t.motion : 0);
                if (at > last) {
                    spring.advance(held, static_cast<float>(std::min(ui.now, at) - last), response);
                }
                if (at < ui.now) {
                    spring.advance(target, static_cast<float>(ui.now - std::max(last, at)), response);
                    held = target;
                }
            }
            if (std::abs(spring.value - held) < 0.25f && std::abs(spring.velocity) < 4) {
                spring = {held};
            }
            if (spring.value != target) {
                moving = true;
            }
        }
        if (moving) {
            ui.keep_animating();
        }

        ui.push_clip({r.x - 12, r.y, r.w + 24, r.h});
        ui.push_id(ui.id("lyrics"));
        int shown = 0;
        std::vector<ui::Painter::Lift> lifts;
        // Lines fade out towards the top and the bottom of the view.
        auto edge = [&](float top, float h) {
            return std::clamp(std::min(top - r.y, r.bottom() - top - h) / 80, 0.f, 1.f);
        };
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block &block = blocks[i];
            float top = r.y + block.y - line_y_[i].value;
            if (static_cast<int>(i) == pause) {
                // The dots grow in, breathe, light up one by one over the pause and shrink away just before the line.
                const auto dt = static_cast<float>(now - pause_from), done = static_cast<float>((now - pause_from) / (pause_to - pause_from));
                const float show = std::min(smooth01(dt / 0.4f), smooth01(static_cast<float>(pause_to - now) / 0.45f));
                const float d = std::round(main.size * 0.5f), step = d * 1.75f, w = d + 2 * step;
                const float cy = top - gap - dots_h / 2;
                const float s = (0.6f + 0.4f * show) * (1 + (t.motion > 0 ? 0.06f : 0) * std::sin(dt * std::numbers::pi_v<float> * 1.25f));
                const float x0 = r.x + (r.w - w) / 2 * centered + d / 2;
                for (int dot = 0; dot < 3; ++dot) {
                    const float fill = smooth01(done * 3 - static_cast<float>(dot)), size = d * s;
                    const float cx = x0 + (static_cast<float>(dot) * step) * s + (1 - s) * (w / 2 - d / 2) * centered;
                    p.rect({cx - size / 2, cy - size / 2, size, size}, t.text.with_alpha((0.3f + 0.65f * fill) * show * edge(cy - dots_h / 2, dots_h)), size / 2);
                }
            }
            if (top + block.h < r.y || top > r.bottom()) {
                continue;
            }
            // When the lyrics arrive, the lines come in one after another.
            const float in = ui.cascade(playback_.lyrics_since(), shown++, 420);
            top += (1 - in) * 14;
            const ui::Id id = ui.id(static_cast<uint64_t>(i));
            const Rect hit{r.x - 12, top - gap / 2, r.w + 24, block.h + gap};
            const auto it = ui.interact(id, hit);
            const float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
            const bool sung = static_cast<int>(i) == current && pause < 0;
            const float on = ui.anim(ui::sub_id(id, 1), sung ? 1 : 0, 420, ease::standard);
            // The current line stands a little larger than the rest.
            const float scale = ui.spring(ui::sub_id(id, 2), sung ? 1 : 0.94f, 480);
            // Lines fade with their distance from the current one.
            const auto distance = static_cast<float>(std::abs(static_cast<int>(i) - (focus + 1) / 2));
            const float base = std::max(0.2f, 0.5f - 0.06f * (distance - 1));
            const float alpha = (base + (1 - base) * on) * edge(top, block.h) * in;
            // A word-timed current line lights up as it is sung, and each word rises a little as it starts.
            const LyricLine &l = lines[i];
            const bool wipe = sung && !l.words.empty();
            const ui::Color unsung = t.text.with_alpha((base + (1 - base) * 0.25f * on) * edge(top, block.h) * in);
            if (hot > 0) {
                p.rect(hit, t.fill(hot), t.radius_s());
            }
            p.set_zoom(scale, r.x + r.w / 2 * centered, top + block.h / 2);
            float y = top;
            auto row = [&](std::string_view s, const TextStyle &style, float lh, ui::Color color, bool wiped) {
                const float x = r.x + (r.w - std::min(p.measure(s, style), r.w)) / 2 * centered;
                lifts.clear();
                if (wiped) {
                    p.set_wipe(x + sung_x(p, l, s, style.size, soft, now, style.features), soft, unsung);
                }
                if (&style == &main && on > 0 && !l.words.empty() && t.motion > 0) {
                    fill_lyric_wave(lifts, p.shape(l.text, style), l, s, style.size, now, on);
                }
                p.set_lifts(lifts);
                p.text(x, y + lh * 0.72f, s, {.size = style.size, .color = color, .features = style.features}, r.w);
                p.set_lifts({});
                p.clear_wipe();
                y += lh;
            };
            for (auto s : block.lines) {
                row(s, main, main_lh, t.text.with_alpha(alpha), wipe);
            }
            for (auto s : block.translation) {
                row(s, sub, sub_lh, t.text.with_alpha(alpha * 0.75f), false);
            }
            p.set_zoom(1);
            if (it.clicked) {
                // A preview clip ends long before the song does; a line beyond it must not end the song.
                const auto clock = playback_.timeline();
                if (clock.file_duration() <= 0 || l.time < clock.to_song(clock.file_duration())) {
                    playback_.seek(l.time);
                }
                manual_until_ = 0;
            }
        }
        ui.pop_id();
        ui.pop_clip();

        // A wipe or the dots move with the music; otherwise nothing changes before the next line.
        if (!playback_.playing()) {
            return;
        }
        if (pause >= 0 || (current >= 0 && !lines[static_cast<size_t>(current)].words.empty())) {
            ui.keep_animating();
        } else if (const auto next = static_cast<size_t>(current + 1); next < lines.size()) {
            ui.wake(ui.now + (lines[next].time - lead - now));
        }
    }

    float Stage::sung_x(ui::Painter &p, const LyricLine &line, std::string_view text, float size, float soft, double time, uint8_t features) {
        const int k = word_at(line, time);
        if (k < 0) {
            return -soft;
        }
        const LyricWord &word = line.words[static_cast<size_t>(k)];
        const auto from = static_cast<uint32_t>(text.data() - line.text.data()), to = from + static_cast<uint32_t>(text.size());
        const text::Shaped &shaped = p.shape(text, {.size = size, .features = features});
        auto x = [&](uint32_t byte) {
            if (byte <= from) {
                return -soft;
            }
            if (byte >= to) {
                return shaped.width * size + soft;
            }
            auto glyph = std::ranges::find_if(shaped.glyphs, [&](const text::Glyph &g) {
                return g.cluster >= byte - from;
            });
            return (glyph == shaped.glyphs.end() ? shaped.width : glyph->x) * size;
        };
        const float sung = word.duration() > 0 ? static_cast<float>(std::clamp((time - word.time()) / word.duration(), 0.0, 1.0)) : 1;
        return x(word.from) + (x(word.to) - x(word.from)) * sung;
    }

} // namespace lyren
