#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

#include "app/draw.hpp"
#include "app/shell.hpp"
#include "core/i18n.hpp"

// Posts: a listener's timeline, the feed of the people the user follows, and writing, liking, forwarding and deleting
// them. A post is a card: who and when, the words with their topics lit, and what it carries — a song to play on the
// spot, a playlist, pictures, or another post passed on.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;

    namespace {
        constexpr ui::Color white{1, 1, 1, 1};

        struct PostStyle {
            static constexpr float pad = 18, avatar = 38, gap = 10, chip = 62, actions = 34, spacing = 12;
            static constexpr float text = 15, nested = 14, name = 14, meta = 12;
            static constexpr float picture_w = 380, picture_h = 300, thumb = 150;
        };

        CacheKey::View post_key(Source source, std::string_view id) {
            return {id, static_cast<uint32_t>(source)};
        }

        float action_width(ui::Ui &ui, std::string_view label) {
            return 16 + (label.empty() ? 0 : 7 + std::ceil(ui.painter().measure(label, {.size = ui.theme().fs(PostStyle::meta), .features = text::tabular_nums}))) + 16;
        }

        // An icon and a count, as a quiet button; scale swells the icon for a like landing.
        bool post_action(ui::Ui &ui, ui::Id id, Rect r, std::string_view icon, std::string_view label, ui::Color color, float scale = 1) {
            auto &p = ui.painter();
            const auto &t = ui.theme();
            auto it = ui.interact(id, r);
            const auto [hot, down] = ui.feel(id, it, 140);
            if (hot > 0) {
                p.rect(r.inset(down), t.text.with_alpha(0.06f * hot), r.h / 2);
            }
            const ui::Color c = color.mix(t.text, 0.5f * hot);
            const float s = 16 * scale;
            p.icon(icon, {r.x + 8 + (16 - s) / 2, r.y + (r.h - s) / 2, s, s}, c);
            if (!label.empty()) {
                ui.label({r.x + 31, r.y, r.w - 31, r.h}, label, {.size = t.fs(PostStyle::meta), .color = c, .features = text::tabular_nums});
            }
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            return it.clicked;
        }

        // Pictures: one at its own shape, more as square thumbnails three to a row (two for two or four).
        std::vector<Rect> picture_layout(const std::vector<Picture> &pictures, float x, float y, float w) {
            using S = PostStyle;
            std::vector<Rect> out;
            if (pictures.size() == 1) {
                const Picture &pic = pictures.front();
                const float aspect = std::clamp(pic.w > 0 && pic.h > 0 ? static_cast<float>(pic.w) / static_cast<float>(pic.h) : 4.f / 3, 0.56f, 2.2f);
                const float mw = std::min(w, S::picture_w), mh = S::picture_h;
                out.push_back(aspect >= mw / mh ? Rect{x, y, mw, std::round(mw / aspect)} : Rect{x, y, std::round(mh * aspect), mh});
                return out;
            }
            const size_t cols = pictures.size() == 2 || pictures.size() == 4 ? 2 : 3;
            const float size = std::floor(std::min(S::thumb, (w - 6 * static_cast<float>(cols - 1)) / static_cast<float>(cols)));
            for (size_t i = 0; i < pictures.size(); ++i) {
                out.push_back({x + static_cast<float>(i % cols) * (size + 6), y + static_cast<float>(i / cols) * (size + 6), size, size});
            }
            return out;
        }
    } // namespace

    void Shell::play_now(Track track) {
        if (is_current(track)) {
            if (!playing()) {
                toggle_play();
            }
            return;
        }
        const int at = current_ < 0 ? 0 : current_ + 1;
        queue_.insert(queue_.begin() + at, std::move(track));
        shuffle_history_.clear();
        shuffle_next_ = -1;
        fm_playing_ = false;
        set_current(at);
        start();
        changed();
    }

    void Shell::load_posts(std::shared_ptr<PostList> list, Source source, std::string author) {
        if (list->loading || !list->more || !list->error.empty()) {
            return;
        }
        list->loading = true;
        spawn(fetch_posts(list, source, std::move(author), ++list->gen));
    }

    Task<void> Shell::fetch_posts(std::shared_ptr<PostList> list, Source source, std::string author, uint64_t gen) {
        // Not a ?: chain: GCC 16 would start both tasks.
        Result<PostPage> r;
        if (author.empty()) {
            r = co_await provider(source).feed(list->cursor);
        } else {
            r = co_await provider(source).posts(author, list->cursor);
        }
        if (gen != list->gen) {
            co_return;
        }
        list->loading = false;
        if (!r) {
            list->error = r.error();
            co_return redraw_();
        }
        if (list->posts.empty()) {
            list->since = now_();
        }
        for (const auto &post : r->posts) {
            images_.prefetch(post.author.avatar, 64);
            if (post.track) {
                images_.prefetch(post.track->cover, 64);
            }
        }
        list->posts.append_range(std::views::as_rvalue(r->posts));
        list->more = r->more;
        list->cursor = std::move(r->cursor);
        redraw_();
    }

    void Shell::refresh_own_posts(Source source) {
        auto refresh = [&](Route &route) {
            if (route.page != Page::profile || !route.profile || route.card.source != source || !own_profile(route.card)) {
                return;
            }
            route.profile->posts = std::make_shared<PostList>();
            load_posts(route.profile->posts, source, route.card.id);
            spawn([](Shell *self, std::shared_ptr<ProfilePage> page, Card card) -> Task<void> {
                auto r = co_await self->provider(card).profile(card.id);
                if (r) {
                    page->info = std::move(*r);
                    self->redraw_();
                }
            }(this, route.profile, route.card));
        };
        refresh(route_);
        for (auto &route : history_) {
            refresh(route);
        }
        if (auto &feed = library(source).feed) {
            feed = std::make_shared<PostList>();
        }
        redraw_();
    }

    void Shell::toggle_post_like(Source source, const Post &post) {
        if (!provider(source).logged_in()) {
            return notify(lyren::trf("Sign in to {} to like posts", lyren::tr(platform(source).name)), "thumbs-up");
        }
        auto k = post_key(source, post.id);
        auto mine = post_likes_.find(k);
        if (mine != post_likes_.end() && mine->second.pending) {
            return;
        }
        std::optional<Like> previous;
        if (mine != post_likes_.end()) {
            previous = mine->second;
        }
        const bool liked = !(previous ? previous->liked : post.liked);
        const int likes = std::max(0, (previous ? previous->likes : post.likes) + (liked ? 1 : -1));
        const uint64_t gen = ++post_write_gen_;
        post_likes_.insert_or_assign(CacheKey(k), Like{.liked = liked, .likes = likes, .pending = gen});
        redraw_();
        spawn([](Shell *self, Source source, Post post, bool like, std::optional<Like> previous, uint64_t gen) -> Task<void> {
            auto r = co_await self->provider(source).like_post(post, like);
            auto mine = self->post_likes_.find(post_key(source, post.id));
            if (mine == self->post_likes_.end() || mine->second.pending != gen) {
                co_return;
            }
            if (r) {
                mine->second.pending = 0;
            } else {
                if (previous) {
                    mine->second = *previous;
                } else {
                    self->post_likes_.erase(mine);
                }
                self->notify(lyren::trf("Could not update the like: {}", lyren::tr(r.error())), "triangle-alert", true);
            }
            self->redraw_();
        }(this, source, post, liked, previous, gen));
    }

    void Shell::confirm_post_delete(ui::Ui &ui, Source source, Post post) {
        ui.open_modal(400, 232, [this, source, post](ui::Ui &u, Rect r) {
            const auto &t = u.theme();
            u.label({r.x, r.y, r.w - 40, 30}, lyren::tr("Delete this post?"), {.size = t.fs(20), .color = t.text});
            const std::string_view text = post.text.empty() && post.track ? std::string_view(post.track->title) : std::string_view(post.text);
            auto lines = u.painter().wrap_lines(text, t.fs(13), r.w);
            for (size_t i = 0; i < std::min<size_t>(lines.size(), 3); ++i) {
                u.label({r.x, r.y + 46 + static_cast<float>(i) * 22, r.w, 22}, lines[i], {.size = t.fs(13), .color = t.text_dim});
            }
            const float ok_w = std::max(96.f, button_width(u, lyren::tr("Delete"), true)), cancel_w = std::max(84.f, button_width(u, lyren::tr("Cancel"), false));
            Rect ok{r.right() - ok_w, r.bottom() - 38, ok_w, 38};
            if (u.button(u.id("post-delete-cancel"), {ok.x - cancel_w - 8, ok.y, cancel_w, 38}, lyren::tr("Cancel"), ui::ButtonKind::ghost)) {
                u.close_modal();
            }
            if (!u.button(u.id("post-delete-confirm"), ok, lyren::tr("Delete"), ui::ButtonKind::primary, "trash")) {
                return;
            }
            u.close_modal();
            const uint64_t gen = ++post_write_gen_;
            post_deletes_.insert_or_assign(CacheKey(post_key(source, post.id)), gen);
            spawn([](Shell *self, Source source, Post post, uint64_t gen) -> Task<void> {
                auto r = co_await self->provider(source).delete_post(post.id);
                auto pending = self->post_deletes_.find(post_key(source, post.id));
                if (pending == self->post_deletes_.end() || pending->second != gen) {
                    co_return;
                }
                self->post_deletes_.erase(pending);
                if (!r) {
                    co_return self->notify(lyren::trf("Could not delete the post: {}", lyren::tr(r.error())), "triangle-alert", true);
                }
                self->notify(std::string(lyren::tr("Post deleted")), "trash");
                // Out of every list that shows it.
                auto drop = [&](const std::shared_ptr<PostList> &list) {
                    if (list && std::erase_if(list->posts, [&](const Post &p) {
                            return p.id == post.id;
                        })) {
                        list->heights.clear();
                    }
                };
                auto clean = [&](Route &route) {
                    if (route.profile && route.card.source == source) {
                        drop(route.profile->posts);
                        if (route.profile->info && route.profile->info->id == post.author.id && route.profile->info->posts > 0) {
                            --route.profile->info->posts;
                        }
                    }
                };
                clean(self->route_);
                for (auto &route : self->history_) {
                    clean(route);
                }
                drop(self->library(source).feed);
                self->redraw_();
            }(this, source, std::move(post), gen));
        });
    }

    void Shell::open_composer(ui::Ui &ui, Source source, std::optional<Track> track, std::optional<Post> forward) {
        if (!provider(source).logged_in()) {
            return notify(lyren::trf("Sign in to {} to post", lyren::tr(platform(source).name)), "user");
        }
        if (post_draft_.sending) {
            return notify(std::string(lyren::tr("The last post is still sending")), "send");
        }
        // Words already written stay for the next post of the same kind.
        if (post_draft_.source != source || post_draft_.forward.has_value() != forward.has_value()) {
            post_draft_.text.clear();
        }
        post_draft_.source = source;
        post_draft_.track = forward ? std::nullopt : std::move(track);
        post_draft_.forward = std::move(forward);
        post_draft_.focus = true;
        ui.open_modal(540, composer_height(), [this](ui::Ui &u, Rect r) {
            composer_modal(u, r);
        });
    }

    // The composer fits what the post carries: the post passed on, the song, or the offer of the one playing.
    float Shell::composer_height() {
        const auto &d = post_draft_;
        const Track *tr = current();
        const float chip = d.forward ? 74 : d.track ? 60 : tr && tr->source == d.source ? 44 : 0;
        return 48 + 196 + (chip > 0 ? chip + 18 : 6) + 38;
    }

    void Shell::composer_modal(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &d = post_draft_;
        ui.resize_modal(540, composer_height());
        ui.label({r.x, r.y, r.w - 40, 30}, lyren::tr(d.forward ? "Forward post" : "New post"), {.size = t.fs(20), .color = t.text});
        ui.label({r.x, r.y + 30, r.w - 40, 18}, lyren::tr(platform(d.source).name), {.size = t.fs(12), .color = t.text_dim});
        const ui::Id field = ui.id("post-draft");
        if (std::exchange(d.focus, false)) {
            ui.focus = field;
        }
        Rect editor{r.x, r.y + 60, r.w, 124};
        ui.text_area(field, editor, d.text, lyren::tr(d.forward ? "Add your thoughts…" : "What are you listening to?"));
        float y = editor.bottom() + 12;
        // What the post carries: the post passed on, the song, or the song playing now, offered.
        if (d.forward) {
            const Post &f = *d.forward;
            Rect quote{r.x, y, r.w, 74};
            p.rect(quote, t.text.with_alpha(0.05f), t.radius);
            p.rect({quote.x + 12, quote.y + 12, 2, quote.h - 24}, t.accent.with_alpha(0.6f), 1);
            ui.label({quote.x + 24, quote.y + 10, quote.w - 40, 20}, "@" + f.author.name, {.size = t.fs(13), .color = t.accent});
            const std::string_view text = f.text.empty() && f.track ? std::string_view(f.track->title) : std::string_view(f.text);
            const auto lines = p.wrap_lines(text, t.fs(13), quote.w - 40);
            for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i) {
                ui.label({quote.x + 24, quote.y + 32 + static_cast<float>(i) * 19, quote.w - 40, 19}, lines[i], {.size = t.fs(13), .color = t.text_dim});
            }
        } else if (d.track) {
            Rect chip{r.x, y, r.w, 60};
            p.rect(chip, t.text.with_alpha(0.05f), t.radius);
            cover(ui, {chip.x + 8, chip.y + 8, 44, 44}, d.track->cover, t.radius_s());
            p.text(chip.x + 64, chip.y + 26, d.track->title, {.size = t.fs(14), .color = t.text}, chip.w - 110);
            p.text(chip.x + 64, chip.y + 45, d.track->artist, {.size = t.fs(12), .color = t.text_dim}, chip.w - 110);
            if (ui.icon_button(ui.id("post-untrack"), {chip.right() - 40, chip.y + 14, 32, 32}, "x", 15)) {
                d.track.reset();
            }
        } else if (const Track *tr = current(); tr && tr->source == d.source) {
            Rect chip{r.x, y, r.w, 44};
            auto it = ui.interact(ui.id("post-attach"), chip);
            const float hot = ui.anim(ui.id("post-attach"), it.hovered ? 1 : 0, 140);
            p.rect(chip, t.text.with_alpha(0.03f + 0.04f * hot), t.radius, t.border.mix(t.accent, hot * 0.6f), 1);
            p.icon("plus", {chip.x + 14, chip.y + 14, 16, 16}, t.accent);
            p.text(chip.x + 40, chip.y + 27, lyren::trf("Attach the song playing: {} · {}", tr->title, tr->artist), {.size = t.fs(13), .color = t.text_dim.mix(t.text, hot)},
                   chip.w - 56);
            if (it.clicked) {
                d.track = *tr;
            }
        }

        size_t n = 0;
        for (size_t i = 0; i < d.text.size(); i = utf8::next(d.text, i)) {
            ++n;
        }
        const bool blank = d.text.find_first_not_of(" \t\r\n") == std::string::npos;
        ui.label({r.x, r.bottom() - 30, 120, 20}, std::to_string(n), {.size = t.fs(11), .color = t.text_dim, .features = text::tabular_nums});
        const float ok_w = std::max(96.f, button_width(ui, lyren::tr("Post"), true)), cancel_w = std::max(84.f, button_width(ui, lyren::tr("Cancel"), false));
        Rect ok{r.right() - ok_w, r.bottom() - 38, ok_w, 38};
        if (ui.button(ui.id("post-cancel"), {ok.x - cancel_w - 8, ok.y, cancel_w, 38}, lyren::tr("Cancel"), ui::ButtonKind::ghost)) {
            ui.close_modal();
        }
        // A post needs words unless it shares a song or passes one on.
        ui.disabled = d.sending || (blank && !d.track && !d.forward);
        bool send;
        {
            Fade dim(p, ui.disabled ? 0.45f : 1.f);
            send = ui.button(ui.id("post-send"), ok, lyren::tr("Post"), ui::ButtonKind::primary, "send");
        }
        ui.disabled = false;
        if (send) {
            ui.close_modal();
            spawn(send_post());
        }
    }

    Task<void> Shell::send_post() {
        auto &d = post_draft_;
        const Source source = d.source;
        const uint64_t account_gen = library(source).account_gen;
        d.sending = true;
        Result<void> r;
        if (d.forward) {
            r = co_await provider(source).forward(*d.forward, d.text);
        } else {
            r = co_await provider(source).share(d.text, d.track);
        }
        d.sending = false;
        if (account_gen != library(source).account_gen) {
            co_return;
        }
        if (!r) {
            co_return notify(lyren::trf("Could not post: {}", lyren::tr(r.error())), "triangle-alert", true);
        }
        d.text.clear();
        d.track.reset();
        d.forward.reset();
        notify(std::string(lyren::tr("Posted")), "circle-check");
        refresh_own_posts(source);
    }

    float Shell::post_attachments(ui::Ui &ui, Source source, const Post &post, float x, float y, float w, bool draw) {
        using S = PostStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        float h = 0;
        // A song plays where it is, after the one playing; a playlist or an album opens.
        if (post.track || post.card) {
            Rect chip{x, y, std::min(w, 460.f), S::chip};
            if (draw) {
                const ui::Id id = ui.id(post.track ? "track" : "card");
                auto it = ui.interact(id, chip);
                const auto [hot, down] = ui.feel(id, it, 140);
                Rect c = chip.inset(down);
                p.rect(c, t.text.with_alpha(0.05f + 0.04f * hot), t.radius);
                Rect art{c.x + 8, c.y + 8, 46, 46};
                const std::string &image = post.track ? post.track->cover : post.card->cover;
                cover(ui, art, image, t.radius_s(), post.track ? "music" : post.card->kind == CardKind::album ? "disc-album" : "list-music");
                const float tx = art.right() + 12, tw = c.right() - tx - 52;
                const bool on = post.track && is_current(*post.track);
                p.text(tx, c.y + 27, post.track ? post.track->title : post.card->title, {.size = t.fs(14), .color = on ? t.accent : t.text}, tw);
                std::string sub;
                if (post.track) {
                    sub = post.track->artist;
                } else {
                    sub = std::string(lyren::tr(post.card->kind == CardKind::album ? "Album" : "Playlist"));
                    if (const auto more = card_subtitle(*post.card); !more.empty()) {
                        sub += " · " + more;
                    }
                }
                p.text(tx, c.y + 46, sub, {.size = t.fs(12), .color = t.text_dim}, tw);
                Rect knob{c.right() - 44, c.y + (c.h - 32) / 2, 32, 32};
                if (post.track) {
                    p.rect(knob, t.accent.with_alpha(0.14f + 0.2f * hot), 16);
                    if (on && playing()) {
                        ui.equalizer(ui.id("eq"), ui::center(knob, 13, 13), t.accent, true);
                    } else {
                        p.icon("play-fill", ui::center(knob, 13, 13), t.accent);
                    }
                } else {
                    p.icon("chevron-right", ui::center(knob, 16, 16), t.text_dim.mix(t.text, hot));
                }
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                if (it.clicked) {
                    if (post.track) {
                        on ? toggle_play() : play_now(*post.track);
                    } else {
                        open(ui, *post.card);
                    }
                }
            }
            h += S::chip + S::gap;
        } else if (!post.resource.empty()) {
            if (draw) {
                const TextStyle st{.size = t.fs(13), .color = t.text_dim};
                Rect chip{x, y, std::min(w, p.measure(post.resource, st) + 46), 36};
                p.rect(chip, t.text.with_alpha(0.05f), chip.h / 2);
                p.icon("link", {chip.x + 13, chip.y + 10, 16, 16}, t.text_dim);
                p.text(chip.x + 36, chip.y + 23, post.resource, st, chip.w - 46);
            }
            h += 36 + S::gap;
        }
        if (!post.pictures.empty()) {
            const auto boxes = picture_layout(post.pictures, x, y + h, w);
            float bottom = y + h;
            for (const auto &b : boxes) {
                bottom = std::max(bottom, b.bottom());
            }
            if (draw) {
                ui.push_id(ui.id("pictures"));
                for (size_t i = 0; i < boxes.size(); ++i) {
                    const Picture &pic = post.pictures[i];
                    const Rect &b = boxes[i];
                    const ui::Id id = ui.id(static_cast<uint64_t>(i));
                    auto it = ui.interact(id, b);
                    const float hot = ui.anim(id, it.hovered ? 1 : 0, 160);
                    float aspect = pic.w > 0 && pic.h > 0 ? static_cast<float>(pic.w) / static_cast<float>(pic.h) : 1;
                    if (boxes.size() == 1) {
                        // Whole, at its own shape.
                        const auto &e = images_.picture(pic.url, static_cast<uint32_t>(Images::tier(std::max(b.w, b.h) * p.scale())));
                        const float a = e.ready ? ui.progress(e.ready_at, 320, ui::ease::standard) : 0;
                        if (a < 1) {
                            p.rect(b, t.text.with_alpha(0.06f), t.radius);
                        }
                        if (e.ready) {
                            aspect = e.h ? static_cast<float>(e.w) / static_cast<float>(e.h) : aspect;
                            Fade fade(p, a);
                            p.image(b, e.tex, t.radius, white.mix({0.86f, 0.86f, 0.86f, 1}, hot * 0.3f), cover_uv(aspect, b.w / b.h));
                        }
                    } else {
                        cover(ui, b, pic.url, t.radius_s(), "image");
                        if (hot > 0) {
                            p.rect(b, ui::Color{0, 0, 0, 0.12f * hot}, t.radius_s());
                        }
                    }
                    if (it.hovered) {
                        ui.cursor = wl::Cursor::pointer;
                    }
                    if (it.clicked) {
                        show_picture(ui, pic.url, aspect);
                    }
                }
                ui.pop_id();
            }
            h = bottom - y + S::gap;
        }
        // A post passed on: its author, its words and what it carries, set in.
        if (!post.forwarded.empty()) {
            const Post &f = post.forwarded.front();
            constexpr float inset = 14;
            const TextStyle body{.size = t.fs(S::nested), .color = t.text.mix(t.text_dim, 0.2f)};
            const float lh = std::round(body.size * 1.55f), iw = w - inset * 2;
            const std::string text = "@" + f.author.name + (f.text.empty() ? "" : "：" + f.text);
            const auto lines = p.wrap_lines(text, body.size, iw);
            ui.push_id(ui.id("forwarded"));
            const float inner_h = post_attachments(ui, source, f, x + inset, 0, iw, false);
            const float block_h = inset + static_cast<float>(lines.size()) * lh + (inner_h > 0 ? 8 + inner_h - S::gap : 0) + inset;
            if (draw) {
                Rect block{x, y + h, w, block_h};
                p.rect(block, t.text.with_alpha(0.045f), t.radius);
                p.rect({block.x, block.y + 12, 3, block.h - 24}, t.accent.with_alpha(0.5f), 1.5f);
                const auto marks = highlights(text);
                // The author's name opens their page.
                const float nw = p.measure("@" + f.author.name, body);
                Rect name{block.x + inset, block.y + inset, nw, lh};
                auto it = ui.interact(ui.id("author"), name);
                if (it.hovered) {
                    ui.cursor = wl::Cursor::pointer;
                }
                if (it.clicked) {
                    open_profile(ui, source, f.author.id, f.author.name, f.author.avatar);
                }
                float ly = block.y + inset;
                for (auto line : lines) {
                    rich_line(p, block.x + inset, ly + lh * 0.72f, text, line, marks, body, t.accent);
                    ly += lh;
                }
                if (inner_h > 0) {
                    post_attachments(ui, source, f, block.x + inset, ly + 8, iw, true);
                }
            }
            ui.pop_id();
            h += block_h + S::gap;
        }
        return h;
    }

    float Shell::post_card(ui::Ui &ui, Source source, const Post &post, float x, float y, float w, bool draw, bool byline) {
        using S = PostStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const float inner = w - S::pad * 2, cx = x + S::pad;
        const TextStyle body{.size = t.fs(S::text), .color = t.text};
        const float lh = std::round(body.size * 1.62f);
        const float head_y = y + S::pad, head_h = byline ? S::avatar : 18;
        float cy = head_y + head_h + (byline ? 12 : 8);
        const auto lines = post.text.empty() ? std::vector<std::string_view>{} : p.wrap_lines(post.text, body.size, inner);
        const float text_y = cy;
        cy += static_cast<float>(lines.size()) * lh + (lines.empty() ? 0 : 8);
        const float attach_y = cy;
        ui.push_id(ui.id(post.id));
        cy += post_attachments(ui, source, post, cx, attach_y, inner, false);
        const float action_y = cy;
        const float h = action_y + S::actions + 10 - y + S::spacing;
        if (!draw) {
            ui.pop_id();
            return h;
        }

        Rect card{x, y, w, h - S::spacing};
        p.rect(card, t.text.with_alpha(t.dark ? 0.04f : 0.03f), t.radius_l(), t.text.with_alpha(0.05f), 1);
        const TextStyle meta{.size = t.fs(S::meta - 1), .color = t.text_dim, .features = text::tabular_nums};
        if (byline) {
            // Who posted, which opens their page.
            Rect face{cx, head_y, S::avatar, S::avatar};
            const float nw = std::min(p.measure(post.author.name, {.size = t.fs(S::name)}) + 1, inner - S::avatar - 60);
            Rect who{face.x, face.y, S::avatar + 12 + nw, S::avatar};
            auto it = ui.interact(ui.id("author"), who);
            const float hot = ui.anim(ui.id("author"), it.hovered ? 1 : 0, 140);
            cover(ui, face, post.author.avatar, S::avatar / 2, "user");
            ui.label({face.right() + 12, head_y - 1, nw, 22}, post.author.name, {.size = t.fs(S::name), .color = t.text});
            if (hot > 0) {
                p.rect({face.right() + 12, head_y + 19, nw, 1}, t.text.with_alpha(0.4f * hot));
            }
            ui.label({face.right() + 12, head_y + 20, inner - S::avatar - 12, 18}, post_time(post.time), meta);
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            if (it.clicked) {
                open_profile(ui, source, post.author.id, post.author.name, post.author.avatar, face);
            }
        } else {
            ui.label({cx, head_y, inner, 18}, post_time(post.time), meta);
        }
        const auto marks = highlights(post.text);
        float ly = text_y;
        for (auto line : lines) {
            rich_line(p, cx, ly + lh * 0.72f, post.text, line, marks, body, t.accent);
            ly += lh;
        }
        post_attachments(ui, source, post, cx, attach_y, inner, true);

        // Likes, comments and forwards along the bottom; the rest behind the menu.
        const auto &caps = provider(source).capabilities;
        auto mine = post_likes_.find(post_key(source, post.id));
        const bool liked = mine != post_likes_.end() ? mine->second.liked : post.liked;
        const int likes = mine != post_likes_.end() ? mine->second.likes : post.likes;
        float ax = cx - 8;
        const float ay = action_y;
        const std::string like_n = likes > 0 ? short_number(likes) : "";
        const float pop = ui.spring(ui.id("post-like-pop"), liked ? 1 : 0, 320, 0.45f);
        ui.disabled = mine != post_likes_.end() && mine->second.pending;
        float w1 = action_width(ui, like_n);
        if (post_action(ui, ui.id("like"), {ax, ay, w1, S::actions}, liked ? "thumbs-up-fill" : "thumbs-up", like_n, liked ? t.accent : t.text_dim,
                        1 + 0.4f * std::abs(pop - (liked ? 1.f : 0.f)))) {
            toggle_post_like(source, post);
        }
        ui.disabled = false;
        ax += w1 + 4;
        if (caps.comments[3]) {
            const std::string n = post.comments > 0 ? short_number(post.comments) : "";
            const float w2 = action_width(ui, n);
            if (post_action(ui, ui.id("comments"), {ax, ay, w2, S::actions}, "message-square", n, t.text_dim)) {
                const std::string &art = post.track ? post.track->cover : !post.pictures.empty() ? post.pictures.front().url : post.author.avatar;
                open_comments(ui, source, Subject::post, post.thread, lyren::trf("{}'s post", post.author.name), art);
            }
            ax += w2 + 4;
        }
        if (caps.post_write) {
            const std::string n = post.forwards > 0 ? short_number(post.forwards) : "";
            const float w3 = action_width(ui, n);
            if (post_action(ui, ui.id("forward"), {ax, ay, w3, S::actions}, "repeat-2", n, t.text_dim)) {
                open_composer(ui, source, {}, post);
            }
        }
        const bool deleting = post_deletes_.contains(post_key(source, post.id));
        if (ui.icon_button(ui.id("more"), {card.right() - S::pad - 26, ay + 3, 28, 28}, deleting ? "trash" : "ellipsis", 16)) {
            std::vector<ui::MenuItem> items;
            if (!post.text.empty()) {
                items.push_back({"copy", std::string(lyren::tr("Copy text")), [&ui, text = post.text] {
                                     ui.copy = text;
                                     ui.toast(std::string(lyren::tr("Copied to clipboard")), "copy");
                                 }});
            }
            if (post.track) {
                items.push_back({"list-plus", std::string(lyren::tr("Play next")), [this, track = *post.track] {
                                     if (current_ < 0) {
                                         play_now(track);
                                         return;
                                     }
                                     queue_.insert(queue_.begin() + (current_ + 1), track);
                                     shuffle_next_ = -1;
                                     changed();
                                     notify(std::string(lyren::tr("Added to play next")), "list-plus");
                                 }});
            }
            if (!post.author.id.empty() && byline) {
                items.push_back({"user", std::string(lyren::tr("View profile")), [this, &ui, a = post.author, source] {
                                     open_profile(ui, source, a.id, a.name, a.avatar);
                                 }});
            }
            if (post.owned && !deleting) {
                if (!items.empty()) {
                    items.push_back({});
                }
                items.push_back({"trash", std::string(lyren::tr("Delete post")),
                                 [this, &ui, source, post] {
                                     confirm_post_delete(ui, source, post);
                                 },
                                 true});
            }
            if (!items.empty()) {
                ui.open_menu(card.right() - S::pad - 26, ay + 34, std::move(items));
            }
        }
        ui.pop_id();
        return h;
    }

    void Shell::post_list(ui::Ui &ui, Rect r, ui::Id id, std::shared_ptr<PostList> list, Source source, const std::string &author, bool byline, double since) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        PostList &l = *list;
        if (l.posts.empty()) {
            if (l.more && !l.loading && l.error.empty()) {
                load_posts(list, source, author);
            }
            if (l.loading || (l.more && l.error.empty()) || !l.error.empty()) {
                page_status(ui, r, l.loading || l.error.empty(), l.error, false);
                return;
            }
            Rect c = ui::center(r, std::min(r.w, 340.f), 120);
            p.icon("messages-square", {c.x + (c.w - 30) / 2, c.y + 4, 30, 30}, t.text_dim);
            ui.label({c.x, c.y + 50, c.w, 22}, lyren::tr(byline ? "No posts from people you follow" : "No posts yet"), {.size = t.fs(15), .color = t.text}, Align::center);
            return;
        }
        const float w = std::min(r.w - 24, 700.f), x = r.x + (r.w - w) / 2;
        const float size = t.fs(PostStyle::text);
        if (l.heights_w != w || l.heights_size != size) {
            l.heights.clear(), l.heights_w = w, l.heights_size = size;
        }
        while (l.heights.size() < l.posts.size()) {
            l.heights.push_back(post_card(ui, source, l.posts[l.heights.size()], 0, 0, w, false, byline));
        }
        float content = 64;
        for (float h : l.heights) {
            content += h;
        }
        const float off = ui.begin_scroll(id, r, content);
        const Rect clip = ui.clip();
        float y = r.y + 2 - off;
        int shown = 0;
        for (size_t i = 0; i < l.posts.size(); ++i) {
            const float h = l.heights[i];
            if (y + h > clip.y && y < clip.bottom()) {
                const float a = ui.progress(since + 0.04 * std::min(shown++, 8), 460);
                Fade fade(p, a);
                post_card(ui, source, l.posts[i], x, y + (1 - a) * 14, w, true, byline);
            }
            y += h;
        }
        if (l.loading) {
            ui.spinner(ui::center({r.x, y, r.w, 56}, 20, 20), t.text_dim, 2);
        } else if (!l.error.empty()) {
            ui.label({r.x, y + 6, r.w, 20}, lyren::tr(l.error), {.size = t.fs(12), .color = t.text_dim}, Align::center);
            if (ui.button(ui.id("posts-retry"), {r.x + (r.w - 100) / 2, y + 30, 100, 28}, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
                l.error.clear();
                load_posts(list, source, author);
            }
        } else if (l.more && y < r.bottom() + 600) {
            load_posts(list, source, author);
        } else if (!l.more) {
            const TextStyle end{.size = t.fs(12), .color = t.text_dim.with_alpha(0.7f)};
            ui.label({r.x, y + 14, r.w, 24}, lyren::tr("That's everything"), end, Align::center);
        }
        if (off > 0.5f) {
            p.gradient({r.x, r.y, r.w, 14}, t.surface.with_alpha(t.surface.a * 0.9f), t.surface.with_alpha(0));
        }
        ui.end_scroll();
    }

    void Shell::moments_page(ui::Ui &ui, Rect r) {
        const auto &t = ui.theme();
        Rect body = r.inset(32, 0);
        ui.label({body.x, r.y + 28, body.w, 38}, lyren::tr("Moments"), {.size = t.fs(28), .color = t.text});
        ui.label({body.x, r.y + 70, body.w, 20}, lyren::tr("From people you follow"), {.size = t.fs(13), .color = t.text_dim});
        if (page_status(ui, {r.x, r.y + 100, r.w, r.h - 100}, false, "", !signed_in())) {
            return;
        }
        auto &feed = library().feed;
        if (!feed) {
            feed = std::make_shared<PostList>();
        }
        float right = body.right();
        if (provider().capabilities.post_write) {
            const auto label = lyren::tr("New post");
            const float w = button_width(ui, label, true);
            if (ui.button(ui.id("moments-write"), {right - w, r.y + 36, w, 38}, label, ui::ButtonKind::primary, "square-pen")) {
                open_composer(ui, active_);
            }
            right -= w + 8;
        }
        ui.disabled = feed->loading;
        if (ui.icon_button(ui.id("moments-refresh"), {right - 38, r.y + 39, 32, 32}, "refresh-cw", 18)) {
            feed = std::make_shared<PostList>();
            ui.scroll_to(ui.id("moments"), 0);
        }
        ui.disabled = false;
        post_list(ui, {r.x + 16, r.y + 104, r.w - 32, r.h - 112}, ui.id("moments"), feed, active_, "", true, std::max(page_time_, feed->since));
    }

} // namespace lyren
