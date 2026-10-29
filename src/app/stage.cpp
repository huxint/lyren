#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "app/draw.hpp"
#include "app/shell.hpp"
#include "core/i18n.hpp"

// The now-playing stage: the cover-lit backdrop, the lyrics, and the cover and title that travel between the player bar
// and the stage.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;
    namespace ease = ui::ease;

    constexpr ui::Color white{1, 1, 1, 1};

    // The now-playing view: wide, a large cover with the lyrics beside it; narrow, a small cover above them.
    struct NowPlayingLayout {
        Rect cover, lyrics;
    };

    static NowPlayingLayout now_playing_layout(Rect r, bool wide) {
        Rect body = r.inset(56, 44);
        if (!wide) {
            return {{body.x, body.y + 4, 64, 64}, {body.x, body.y + 96, body.w, body.h - 96}};
        }
        Rect left = ui::cut_left(body, std::round(body.w * 0.42f));
        ui::cut_left(body, 40);
        float cs = std::floor(std::min({left.w - 20, left.h - 120, 420.f}));
        return {{left.x + (left.w - cs) / 2, left.y + (left.h - cs - 110) / 2, cs, cs}, body};
    }

    // The title's baseline origin, the two sizes, the artist's baseline below the title's, and the width the artist
    // line may take; the title leaves room for the heart.
    struct Caption {
        float x, y, title, sub, gap, w;
    };

    static Caption mix(const Caption &a, const Caption &b, float k) {
        return {std::lerp(a.x, b.x, k), std::lerp(a.y, b.y, k), std::lerp(a.title, b.title, k), std::lerp(a.sub, b.sub, k), std::lerp(a.gap, b.gap, k), std::lerp(a.w, b.w, k)};
    }

    void Shell::player_art(ui::Ui &ui, Rect player, Rect page, float np, float wide) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Rect left{player.x, player.y, std::min(330.f, std::round(player.w * 0.3f)), player.h};
        Rect small{left.x + 16, left.y + 14, 56, 56};
        bar_cover_px_ = small.w * p.scale();
        const Track *tr = current();
        if (!tr) {
            if (np == 0) {
                p.rect(small, t.text.with_alpha(0.06f), t.radius_s());
                p.icon("music", ui::center(small, 22, 22), t.text_dim);
                ui.label({small.right() + 14, left.y, left.w - 100, left.h}, lyren::tr("Nothing playing"), {.size = t.fs(13), .color = t.text_dim});
            }
            return;
        }

        // In the view the text sits under a large cover and beside a small one; it goes round the cover's corner while
        // the cover is small.
        NowPlayingLayout wl = now_playing_layout(page, true), nl = now_playing_layout(page, false);
        Rect body = page.inset(56, 44);
        Rect vc = mix(nl.cover, wl.cover, wide);
        float under = smooth01((vc.w - nl.cover.w) / (std::min(160.f, wl.cover.w) - nl.cover.w));
        Caption beside{vc.right() + 20, vc.y + 22, t.fs(20), t.fs(13), 24, body.right() - vc.right() - 20};
        Caption below{vc.x, vc.bottom() + 46, t.fs(22), t.fs(14), 26, vc.w};
        Caption view = mix(beside, below, under);
        view.x = std::lerp(beside.x, below.x, smooth01((under - 0.4f) / 0.6f));
        view.y = std::lerp(beside.y, below.y, smooth01(under / 0.6f));

        // From the bar to a small cover the text stays beside it. To a large one the cover first rises out of the bar
        // and then grows, and the text sets off once the cover is out of its way.
        Caption bar{small.right() + 14, small.y + 24, t.fs(14), t.fs(12), 19, left.right() - small.right() - 18};
        float rise = std::lerp(np, 1 - (1 - np) * (1 - np), under), grow = std::lerp(np, np * np, under);
        float size = std::lerp(small.w, vc.w, grow);
        Rect cov{std::lerp(small.x, vc.x, rise), std::lerp(small.bottom(), vc.bottom(), rise) - size, size, size};
        float late = smooth01((np - 0.35f) / 0.65f), f = std::lerp(np, late, under);
        Caption cap = mix(bar, view, f);
        float ax = cov.right() + std::lerp(14.f, 20.f, np) + (view.x - beside.x) * np;
        float ay = cov.y + std::lerp(24.f, 22.f, np) + (view.y - beside.y) * np;
        cap.x = std::lerp(ax, std::lerp(bar.x, view.x, late), under);
        cap.y = std::lerp(ay, std::lerp(bar.y, view.y, late), under);

        float depth = smooth01((size - 64) / 136);
        float radius = std::lerp(t.radius_s(), t.radius_l(), depth);
        p.shadow({cov.x, cov.y + std::lerp(4.f, 18.f, depth), cov.w, cov.h}, radius, std::lerp(12.f, 48.f, depth), t.shadow.with_alpha(std::lerp(0.6f, 0.9f, depth)));

        // The thumbnail stays through the move; the view's larger picture fades in over it once it has loaded.
        float change = ui.progress(cover_time_, 520);
        int preview_px = Images::tier(small.w * p.scale()), sharp_px = Images::tier((np_wide_ ? 420.f : nl.cover.w) * p.scale());
        auto artwork = [&](const std::string &url) {
            const auto &preview = images_.get(cover_url(url, preview_px), static_cast<uint32_t>(preview_px));
            float shown = preview.ready ? ui.progress(preview.ready_at, 320, ease::standard) : 0;
            if (shown < 1) {
                p.rect(cov, t.text.with_alpha(0.07f), radius);
                p.set_zoom(cov.w * 0.36f / 24, cov.x + cov.w / 2, cov.y + cov.h / 2);
                p.icon("music", ui::center(cov, 24, 24), t.text_dim.with_alpha(0.5f * (1 - shown)));
                p.set_zoom(1);
            }
            if (preview.ready) {
                Fade fade(p, shown);
                p.image(cov, preview.tex, radius);
            }
            if (np > 0 && sharp_px > preview_px) {
                const auto &sharp = images_.get(cover_url(url, sharp_px), static_cast<uint32_t>(sharp_px));
                if (sharp.ready) {
                    Fade fade(p, ui.progress(sharp.ready_at, 240, ease::standard));
                    p.image(cov, sharp.tex, radius);
                }
            }
        };
        if (change < 1 && !prev_.cover.empty()) {
            artwork(prev_.cover);
        }
        {
            Fade fade(p, change);
            artwork(tr->cover);
        }
        if (np == 0) {
            ui::Id id = ui.id("bar-cover");
            auto it = ui.interact(id, cov);
            float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
            if (hot > 0) {
                p.rect(cov, ui::Color{0, 0, 0, 0.45f * hot}, radius);
                p.icon("chevron-up", ui::center(cov, 22, 22), white.with_alpha(hot));
            }
            if (it.clicked) {
                show_stage(true);
            }
        }

        // While it moves, the text is scaled from glyphs at the view's sizes instead of being rasterized at a new size
        // every frame. A line that does not fit fades out at its end.
        bool settled = np == 0 || (np == 1 && (under == 0 || under == 1));
        auto line = [&](float x, float y, std::string_view s, float size, float raster, ui::Color c, float w) {
            if (settled) {
                raster = size;
            }
            float z = size / raster, limit = w / z;
            p.set_zoom(z, x, y);
            TextStyle st{.size = raster, .color = c};
            float width = p.measure(s, st);
            if (width > limit) {
                p.set_wipe(x + limit - 16 / z, 32 / z, c.with_alpha(0));
            }
            p.text(x, y, s, st);
            p.clear_wipe();
            p.set_zoom(1);
            return std::min(width, limit) * z;
        };
        // The album and the stream format belong to the view and come in as the text arrives there.
        float detail = smooth01((f - 0.6f) / 0.4f);
        auto caption = [&](const Track &track, float alpha, float dy, bool live) {
            Fade fade(p, alpha);
            float y = cap.y + dy, k = cap.sub / t.fs(14);
            float tw = line(cap.x, y, track.title, cap.title, t.fs(22), t.text, cap.w - 44);
            float aw = line(cap.x, y + cap.gap, track.artist, cap.sub, t.fs(14), t.text_dim, cap.w);
            if (detail > 0 && under > 0 && !track.album.empty()) {
                Fade more(p, detail * under);
                line(cap.x + aw, y + cap.gap, " · " + track.album, cap.sub, t.fs(14), t.text_dim, cap.w - aw);
            }
            if (detail > 0 && live && !player_.codec().empty()) {
                Fade more(p, detail);
                std::string text = std::format("{} · {:.1f} kHz", player_.codec(), player_.format().rate / 1000.0);
                if (trial_) {
                    text += " · ";
                    text += lyren::tr("Preview");
                }
                line(cap.x, y + cap.gap + 24 * k, text, t.fs(12) * k, t.fs(12), t.text_dim.with_alpha(0.7f), cap.w);
            }
            // The heart follows the title.
            bool like = liked(track);
            float hs = heart_size(ui, track, std::lerp(17.f, 20.f, f));
            Rect hr{cap.x + std::min(tw + 10, cap.w - 34), y - 0.35f * cap.title - 16, 32, 32};
            if (live && settled) {
                if (ui.icon_button(ui.id("np-like"), hr, like ? "heart-fill" : "heart", hs, like)) {
                    toggle_like(track);
                }
                return;
            }
            p.set_zoom(hs / 20, hr.x + 16, hr.y + 16);
            p.icon(like ? "heart-fill" : "heart", ui::center(hr, 20, 20), like ? t.accent : t.text_dim);
            p.set_zoom(1);
        };
        // A new track's text rises in as the old one leaves upwards.
        float out = smooth01(change / 0.45f), in = smooth01((change - 0.25f) / 0.75f);
        if (out < 1 && !prev_.title.empty()) {
            caption(prev_, 1 - out, -10 * out, false);
        }
        caption(*tr, in, 10 * (1 - in), true);
    }

    void Shell::now_playing(ui::Ui &ui, Rect window, Rect r, float k, float wide) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Track *tr = current();
        Fade fade(p, k);
        const float radius = ui.window_radius;

        // The stage fills the window: the cover, blurred and stretched, drifting slowly while the music plays, over a
        // deep shade of the accent and under a veil that keeps white text readable on light covers.
        p.rect(window, t.accent.mix({0.02f, 0.02f, 0.03f, 1}, 0.84f), radius);
        if (playing() && t.motion > 0) {
            drift_ += ui.now - drift_last_;
        }
        drift_last_ = ui.now;
        constexpr float zoom = 0.8f, tau = 2 * std::numbers::pi_v<float>;
        const float aspect = window.w / window.h;
        Rect uv = aspect > 1 ? Rect{0, (1 - 1 / aspect) / 2, 1, 1 / aspect} : Rect{(1 - aspect) / 2, 0, aspect, 1};
        const auto drift = static_cast<float>(drift_);
        uv = {uv.x + uv.w * (1 - zoom) * (0.5f + 0.5f * std::sin(drift * tau / 41)), uv.y + uv.h * (1 - zoom) * (0.5f + 0.5f * std::cos(drift * tau / 53)), uv.w * zoom,
              uv.h * zoom};
        float luma = 0.3f;
        auto backdrop = [&](const std::string &url, float a) {
            const auto &e = images_.get(cover_url(url, 128), 128, true);
            if (!e.ready) {
                return;
            }
            float shown = a * ui.progress(e.ready_at, 600, ease::standard);
            luma = std::lerp(luma, e.luma, shown);
            Fade f(p, shown);
            p.image(window, e.tex, radius, white, uv);
        };
        float change = ui.progress(cover_time_, 800, ease::standard);
        if (change < 1 && !prev_.cover.empty()) {
            backdrop(prev_.cover, 1 - change);
        }
        if (tr) {
            backdrop(tr->cover, change);
        }
        p.gradient(window, {0, 0, 0, 0.14f + 0.36f * luma}, {0, 0, 0, 0.4f + 0.34f * luma}, radius);

        // The title strip holds the way back down, on a disc that shows on light covers too.
        {
            const ui::Id id = ui.id("np-close");
            const Rect b{window.x + 14, window.y + 8, 36, 36};
            const auto it = ui.interact(id, b);
            const float hot = ui.anim(id, it.hovered ? 1 : 0, 150), down = ui.anim(ui::sub_id(id, 0xd0), it.held && it.hovered ? 1 : 0, 90);
            const Rect face = b.inset(down * 2);
            p.rect(face, white.with_alpha(0.08f + 0.1f * hot), face.h / 2);
            p.icon("chevron-down", ui::center(b, 20 - down * 1.5f, 20 - down * 1.5f), white.with_alpha(0.72f + 0.28f * hot));
            if (it.clicked) {
                show_stage(false);
            }
        }

        r.y += (1 - k) * 40;
        if (!tr) {
            ui.label(r, lyren::tr("Nothing playing"), {.size = t.fs(15), .color = t.text_dim}, Align::center);
            return;
        }
        // The lyrics move between the column beside the cover and the space under it.
        lyrics_view(ui, mix(now_playing_layout(r, false).lyrics, now_playing_layout(r, true).lyrics, wide), 1 - wide);
    }

    void Shell::lyrics_view(ui::Ui &ui, Rect r, float centered) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        TextStyle main{.size = t.fs(22)}, sub{.size = t.fs(15)};
        const float main_lh = std::round(main.size * 1.45f), sub_lh = std::round(sub.size * 1.5f), gap = 20, soft = main.size * 0.8f;
        if (lyrics_loading_) {
            ui.spinner(ui::center(r, 22, 22), t.text_dim, 2);
            return;
        }
        if (lyrics_.empty()) {
            TextStyle st{.size = t.fs(16), .color = t.text_dim};
            float w = p.measure(lyren::tr("No lyrics available"), st);
            ui.label({r.x + (r.w - w) / 2 * centered, r.y, w + 1, r.h}, lyren::tr("No lyrics available"), st);
            return;
        }
        if (lyric_layout_.size() != lyrics_.size() || lyric_layout_w_ != r.w || lyric_layout_size_ != main.size || lyric_layout_font_ != config_.theme.font) {
            lyric_layout_.clear();
            float y = 0;
            for (const auto &l : lyrics_) {
                LyricBlock b{.lines = wrap(p, l.text, main.size, r.w), .y = y};
                if (lyric_translation_ && !l.translation.empty()) {
                    b.translation = wrap(p, l.translation, sub.size, r.w);
                }
                b.h = static_cast<float>(b.lines.size()) * main_lh + static_cast<float>(b.translation.size()) * sub_lh;
                y += b.h + gap;
                lyric_layout_.push_back(std::move(b));
            }
            lyric_layout_w_ = r.w;
            lyric_layout_size_ = main.size;
            lyric_layout_font_ = config_.theme.font;
        }
        const auto &blocks = lyric_layout_;
        float total = blocks.back().y + blocks.back().h;
        // The line a little ahead, so its highlight lands with the voice rather than after it.
        const double now = timeline().to_song(last_pos_), lead = 0.2;
        int cur = lyric_at(lyrics_, now + lead);
        // A long pause before a line, at the start or after a word-timed line (the others do not say where they end):
        // three dots take the stage and fill up until the line comes.
        int pause = -1;
        double pause_from = 0, pause_to = 0;
        if (size_t n = static_cast<size_t>(cur + 1); n < lyrics_.size()) {
            const LyricLine *before = cur >= 0 ? &lyrics_[static_cast<size_t>(cur)] : nullptr;
            double from = !before ? 0 : before->words.empty() ? -1 : before->words.back().time() + before->words.back().duration() + 0.3;
            double to = lyrics_[n].time - lead;
            if (from >= 0 && to - from >= 5 && now >= from) {
                pause = static_cast<int>(n), pause_from = from, pause_to = to;
            }
        }
        // What the view centres on: 2i is line i, 2i - 1 the dots before it, which push it and the lines after it down.
        const float dots_h = main_lh, room = dots_h + gap;
        int focus = pause >= 0 ? 2 * pause - 1 : 2 * std::max(cur, 0);
        float anchor = r.h * 0.4f;
        auto aim = [&](int f, size_t i) {
            int n = (f + 1) / 2;
            const LyricBlock &b = blocks[static_cast<size_t>(n)];
            float centre = (f % 2 ? b.y + dots_h / 2 : b.y + b.h / 2) - anchor;
            return f % 2 && static_cast<int>(i) >= n ? centre - room : centre;
        };
        if (lyric_y_.size() != blocks.size()) {
            lyric_y_.clear();
            lyric_aim_.clear();
            for (size_t i = 0; i < blocks.size(); ++i) {
                lyric_y_.push_back({aim(focus, i)});
                lyric_aim_.push_back(aim(focus, i));
            }
            lyric_to_ = focus;
        } else if (focus != lyric_to_) {
            // Only the next line coming up ripples down the lyrics; a jump (a click, a seek, going back) moves them as one.
            lyric_ripple_ = focus > lyric_to_ && focus - lyric_to_ <= 2 && ui.now >= lyric_manual_until_;
            lyric_to_ = focus, lyric_switch_ = ui.now;
        }

        // The wheel browses freely for a few seconds, then the view glides back to the current line.
        if (ui.hovering(r) && ui.input().scroll_y != 0) {
            if (ui.now >= lyric_manual_until_) {
                lyric_manual_ = lyric_y_[static_cast<size_t>((focus + 1) / 2)].value;
            }
            lyric_manual_ = std::clamp(lyric_manual_ + ui.input().scroll_y, -anchor, total - anchor);
            ui.input().scroll_y = 0;
            lyric_manual_until_ = ui.now + 3;
            lyric_ripple_ = false;
            ui.redraw();
        }
        bool manual = ui.now < lyric_manual_until_;
        if (manual) {
            ui.wake(lyric_manual_until_);
        }
        // With a ripple each line below the current one sets off a little later, until then still heading where it
        // was; frames can be far apart, so the time before and after that moment is stepped separately.
        double last = lyric_last_;
        lyric_last_ = ui.now;
        bool moving = false;
        const float response = (lyric_ripple_ ? 0.5f : 0.6f) * t.motion;
        for (size_t i = 0; i < blocks.size(); ++i) {
            ui::Spring &s = lyric_y_[i];
            float &held = lyric_aim_[i];
            float target = manual ? lyric_manual_ : aim(lyric_to_, i);
            if (t.motion == 0) {
                s = {target}, held = target;
                continue;
            }
            if (manual) {
                s.advance(target, static_cast<float>(ui.now - last), 0.18f * t.motion);
                held = target;
            } else {
                double at = lyric_switch_ + (lyric_ripple_ ? 0.028 * std::clamp(static_cast<int>(i) - (lyric_to_ + 1) / 2, 0, 5) * t.motion : 0);
                if (at > last) {
                    s.advance(held, static_cast<float>(std::min(ui.now, at) - last), response);
                }
                if (at < ui.now) {
                    s.advance(target, static_cast<float>(ui.now - std::max(last, at)), response);
                    held = target;
                }
            }
            if (std::abs(s.value - held) < 0.25f && std::abs(s.velocity) < 4) {
                s = {held};
            }
            if (s.value != target) {
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
        auto edge = [&](float top, float h) {
            return std::clamp(std::min(top - r.y, r.bottom() - top - h) / 80, 0.f, 1.f);
        };
        for (size_t i = 0; i < blocks.size(); ++i) {
            const LyricBlock &b = blocks[i];
            float top = r.y + b.y - lyric_y_[i].value;
            if (static_cast<int>(i) == pause) {
                // The dots grow in, breathe, light up one by one over the pause and shrink away just before the line.
                float dt = static_cast<float>(now - pause_from), done = static_cast<float>((now - pause_from) / (pause_to - pause_from));
                float show = std::min(smooth01(dt / 0.4f), smooth01(static_cast<float>(pause_to - now) / 0.45f));
                float d = std::round(main.size * 0.5f), step = d * 1.75f, w = d + 2 * step;
                float cy = top - gap - dots_h / 2;
                float s = (0.6f + 0.4f * show) * (1 + (t.motion > 0 ? 0.06f : 0) * std::sin(dt * std::numbers::pi_v<float> * 1.25f));
                float x0 = r.x + (r.w - w) / 2 * centered + d / 2;
                for (int k = 0; k < 3; ++k) {
                    float fill = smooth01(done * 3 - static_cast<float>(k)), dd = d * s;
                    float cx = x0 + (static_cast<float>(k) * step) * s + (1 - s) * (w / 2 - d / 2) * centered;
                    p.rect({cx - dd / 2, cy - dd / 2, dd, dd}, t.text.with_alpha((0.3f + 0.65f * fill) * show * edge(cy - dots_h / 2, dots_h)), dd / 2);
                }
            }
            if (top + b.h < r.y || top > r.bottom()) {
                continue;
            }
            // When the lyrics arrive, the lines come in one after another.
            float in = ui.progress(lyrics_since_ + 0.03 * std::min(shown++, 12), 420);
            top += (1 - in) * 14;
            ui::Id id = ui.id(static_cast<uint64_t>(i));
            Rect hit{r.x - 12, top - gap / 2, r.w + 24, b.h + gap};
            auto it = ui.interact(id, hit);
            float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
            bool current = static_cast<int>(i) == cur && pause < 0;
            float on = ui.anim(ui::sub_id(id, 1), current ? 1 : 0, 420, ease::standard);
            // The current line stands a little larger than the rest.
            float scale = ui.spring(ui::sub_id(id, 2), current ? 1 : 0.94f, 480);
            // Lines fade with distance from the current one and towards the edges.
            float d = static_cast<float>(std::abs(static_cast<int>(i) - (focus + 1) / 2));
            float base = std::max(0.2f, 0.5f - 0.06f * (d - 1));
            float a = (base + (1 - base) * on) * edge(top, b.h) * in;
            // A word-timed current line lights up as it is sung, and each word rises a little as it starts.
            const LyricLine &l = lyrics_[i];
            bool wipe = current && !l.words.empty();
            ui::Color unsung = t.text.with_alpha((base + (1 - base) * 0.25f * on) * edge(top, b.h) * in);
            if (hot > 0) {
                p.rect(hit, t.text.with_alpha(0.06f * hot), t.radius_s());
            }
            p.set_zoom(scale, r.x + r.w / 2 * centered, top + b.h / 2);
            float y = top;
            auto line = [&](std::string_view s, const TextStyle &st, float lh, ui::Color c, bool wiped) {
                float x = r.x + (r.w - std::min(p.measure(s, st), r.w)) / 2 * centered;
                if (wiped) {
                    p.set_wipe(x + sung_x(p, l, s, st.size, soft, now), soft, unsung);
                }
                lifts.clear();
                if (on > 0 && &st == &main) {
                    auto from = static_cast<uint32_t>(s.data() - l.text.data());
                    for (const LyricWord &w : l.words) {
                        if (w.to <= from || w.from >= from + s.size()) {
                            continue;
                        }
                        double sung = w.duration() > 0 ? std::clamp((now - w.time()) / w.duration(), 0.0, 1.0) : now >= w.time();
                        lifts.push_back({w.from > from ? w.from - from : 0, main.size * 0.1f * on * static_cast<float>(1 - (1 - sung) * (1 - sung))});
                    }
                }
                p.set_lifts(lifts);
                p.text(x, y + lh * 0.72f, s, {.size = st.size, .color = c}, r.w);
                p.set_lifts({});
                p.clear_wipe();
                y += lh;
            };
            for (auto s : b.lines) {
                line(s, main, main_lh, t.text.with_alpha(a), wipe);
            }
            for (auto s : b.translation) {
                line(s, sub, sub_lh, t.text.with_alpha(a * 0.75f), false);
            }
            p.set_zoom(1);
            if (it.clicked) {
                // A preview clip ends long before the song does; a line beyond it must not end the track.
                const auto clock = timeline();
                if (clock.file_duration() <= 0 || lyrics_[i].time < clock.to_song(clock.file_duration())) {
                    seek_song(lyrics_[i].time);
                }
                lyric_manual_until_ = 0;
            }
        }
        ui.pop_id();
        ui.pop_clip();

        // A wipe or the dots move with the music; otherwise nothing changes before the next line.
        if (!playing()) {
            return;
        }
        if (pause >= 0 || (cur >= 0 && !lyrics_[static_cast<size_t>(cur)].words.empty())) {
            ui.keep_animating();
        } else if (size_t next = static_cast<size_t>(cur + 1); next < lyrics_.size()) {
            ui.wake(ui.now + (lyrics_[next].time - lead - now));
        }
    }

} // namespace lyren
