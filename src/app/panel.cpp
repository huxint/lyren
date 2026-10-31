#include <algorithm>
#include <numbers>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "app/draw.hpp"
#include "app/shell.hpp"
#include "core/i18n.hpp"
#include "core/utf8.hpp"

// The panel beside the page and the stage: the play queue, or the comments on a song, an album or a playlist.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;

    namespace {
        constexpr ui::Color white{1, 1, 1, 1};

        // The comments read like a record's sleeve notes: the record up top, the voices in one column, one quote set
        // large, and a round button floating over them that opens into the card to write in.
        struct CommentStyle {
            static constexpr float inset = 20, padding = 16, gap = 8, small_gap = 4;
            static constexpr float avatar = 34, art = 56, icon = 16, small_icon = 14, control = 30, send = 36, fab = 52;
            static constexpr float title = 15, quote = 15, body = 14, name = 13, meta = 12, time = 11;
            static constexpr float picture_w = 240, picture_h = 220, sticker = 76;
        };

        float comment_action_width(ui::Ui &ui, std::string_view label) {
            using C = CommentStyle;
            float text = label.empty() ? 0 : std::ceil(ui.painter().measure(label, {.size = ui.theme().fs(C::meta), .features = text::tabular_nums})) + C::small_gap + 2;
            return C::small_icon + text;
        }

        // An icon and a quiet label. icon_scale swells the icon about its centre, for a like landing.
        bool comment_action(ui::Ui &ui, ui::Id id, Rect r, std::string_view label, std::string_view icon, ui::Color color, float icon_scale = 1) {
            using C = CommentStyle;
            auto &p = ui.painter();
            const auto &t = ui.theme();
            auto it = ui.interact(id, r.inset(-C::small_gap, 0));
            float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
            color = color.mix(t.text, hot * 0.6f);
            float s = C::small_icon * icon_scale;
            p.icon(icon, {r.x + (C::small_icon - s) / 2, r.y + (r.h - s) / 2, s, s}, color);
            ui.label({r.x + C::small_icon + C::small_gap + 2, r.y, r.w, r.h}, label, {.size = t.fs(C::meta), .color = color, .features = text::tabular_nums});
            return it.clicked;
        }

        float thread_chip_width(ui::Ui &ui, std::string_view label) {
            using C = CommentStyle;
            return std::ceil(ui.painter().measure(label, {.size = ui.theme().fs(C::meta)})) + C::small_icon * 2 + C::gap * 3;
        }

        // The way into a comment's replies: a tinted tab hanging off the comment like the start of a thread.
        bool thread_chip(ui::Ui &ui, ui::Id id, Rect r, std::string_view label) {
            using C = CommentStyle;
            auto &p = ui.painter();
            const auto &t = ui.theme();
            auto it = ui.interact(id, r);
            float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
            float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
            Rect b = r.inset(down, down);
            p.rect(b, t.accent.with_alpha(0.1f + 0.07f * hot), b.h / 2);
            p.icon("corner-down-right", {b.x + C::gap, b.y + (b.h - C::small_icon) / 2, C::small_icon, C::small_icon}, t.accent);
            ui.label({b.x + C::gap * 1.5f + C::small_icon, b.y, b.w, b.h}, label, {.size = t.fs(C::meta), .color = t.accent});
            float nudge = 2 * hot;
            p.icon("chevron-right", {b.right() - C::gap - C::small_icon + nudge, b.y + (b.h - C::small_icon) / 2, C::small_icon, C::small_icon}, t.accent.with_alpha(0.75f));
            if (it.hovered) {
                ui.cursor = wl::Cursor::pointer;
            }
            return it.clicked;
        }

        // Where a comment's picture goes in a column w wide: its own shape within bounds, or 4:3 until it is known.
        ui::Rect picture_box(const Comment &c, float w) {
            using C = CommentStyle;
            if (c.image.empty()) {
                return {};
            }
            if (c.sticker) {
                return {0, 0, C::sticker, C::sticker};
            }
            float aspect = c.image_w > 0 && c.image_h > 0 ? static_cast<float>(c.image_w) / static_cast<float>(c.image_h) : 4.f / 3;
            aspect = std::clamp(aspect, 0.56f, 2.2f);
            float mw = std::min(w, C::picture_w), mh = C::picture_h;
            return aspect >= mw / mh ? Rect{0, 0, mw, std::round(mw / aspect)} : Rect{0, 0, std::round(mh * aspect), mh};
        }

        // The part of an image of aspect `image` that fills a frame of aspect `frame`, centred.
        ui::Rect cover_uv(float image, float frame) {
            if (image > frame) {
                float u = frame / image;
                return {(1 - u) / 2, 0, u, 1};
            }
            float v = image / frame;
            return {0, (1 - v) / 2, 1, v};
        }

        // A record in its own colours, grooves catching the light at angle (radians).
        void vinyl(ui::Painter &p, Rect disc, ui::Color label, float angle) {
            float r = disc.w / 2;
            p.shadow({disc.x, disc.y + 2, disc.w, disc.h}, r, 8, {0, 0, 0, 0.35f});
            p.rect(disc, {0.07f, 0.07f, 0.08f, 1}, r);
            for (float k : {0.9f, 0.78f, 0.66f, 0.54f}) {
                p.rect(disc.inset(r * (1 - k)), {0, 0, 0, 0}, r * k, white.with_alpha(0.06f), 1);
            }
            for (float side : {0.f, std::numbers::pi_v<float>}) {
                p.arc(disc.inset(r * 0.2f), r * 0.42f, angle + side, 0.55f, white.with_alpha(0.08f));
            }
            Rect centre = disc.inset(r * 0.64f);
            p.rect(centre, label, centre.w / 2);
            p.rect(ui::center(disc, 3, 3), {0.07f, 0.07f, 0.08f, 1}, 1.5f);
        }
    } // namespace

    static CacheKey::View comment_key(Source source, std::string_view id) {
        return {id, static_cast<uint32_t>(source)};
    }

    float Shell::panel_width(Panel panel) {
        return panel == Panel::queue ? 336 : panel == Panel::comments ? 408 : 0;
    }

    void Shell::show_panel(Panel panel) {
        if (panel == panel_) {
            return;
        }
        if (panel_ != Panel::none && panel != Panel::none) {
            panel_from_ = panel_, panel_time_ = now_();
        }
        panel_ = panel;
        if (panel != Panel::none) {
            panel_last_ = panel;
        }
    }

    void Shell::side_panel(ui::Ui &ui, Rect slot, float stage) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // On the stage the panel is a pane of glass over the cover, standing clear of the controls below it.
        Rect frame{slot.x + 8, slot.y, slot.w - 8, slot.h - std::round(12 * stage)};
        p.rect(frame, t.surface, t.radius_l(), t.border.with_alpha(stage), 1);
        ui.push_clip(frame);
        auto content = [&](Panel kind, Rect r) {
            if (kind == Panel::queue) {
                queue_panel(ui, r);
            } else {
                comments_panel(ui, r);
            }
        };
        // Switching between the two: the old one fades out as the new one comes in from the right.
        float in = ui.progress(panel_time_, 300);
        if (in < 1 && panel_from_ != Panel::none && panel_from_ != panel_last_) {
            Fade out(p, std::max(0.f, 1 - in * 2.5f));
            content(panel_from_, {frame.x, frame.y, panel_width(panel_from_) - 8, frame.h});
        }
        {
            Fade fade(p, in);
            content(panel_last_, {frame.x + (1 - in) * 16, frame.y, panel_width(panel_last_) - 8, frame.h});
        }
        ui.pop_clip();
    }

    void Shell::queue_panel(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ui.label({r.x + 20, r.y + 18, 160, 28}, lyren::tr("Queue"), {.size = t.fs(16), .color = t.text});
        float qw = p.measure(lyren::tr("Queue"), {.size = t.fs(16)});
        ui.label({r.x + 28 + qw, r.y + 19, 100, 28}, std::to_string(queue_.size()), {.size = t.fs(13), .color = t.text_dim});
        std::string_view clear = lyren::tr("Clear");
        float cw = button_width(ui, clear, false) - 12;
        if (ui.button(ui.id("queue-clear"), {r.right() - 16 - cw, r.y + 18, cw, 28}, clear, ui::ButtonKind::ghost) && !queue_.empty()) {
            queue_.clear();
            current_ = -1;
            ++play_gen_;
            want_play_ = fetching_ = false;
            stop_player();
            shuffle_history_.clear();
            changed();
            ui.toast(std::string(lyren::tr("Queue cleared")), "trash");
        }
        constexpr float row_h = 54;
        Rect view{r.x + 8, r.y + 60, r.w - 16, r.h - 68};
        if (queue_.empty()) {
            ui.label({view.x, view.y + 40, view.w, 20}, lyren::tr("Nothing queued"), {.size = t.fs(13), .color = t.text_dim}, Align::center);
            return;
        }
        float off = ui.begin_scroll(ui.id("queue"), view, static_cast<float>(queue_.size()) * row_h);
        int remove = -1;
        // One highlight glides to the playing row.
        float hy = ui.spring(ui.id("queue-current"), static_cast<float>(current_) * row_h, 260);
        float ha = ui.anim(ui.id("queue-current-a"), current_ >= 0 ? 1 : 0, 200);
        if (ha > 0) {
            p.rect({view.x + 2, view.y + hy - off + 2, view.w - 4, row_h - 4}, t.accent.with_alpha(0.14f * ha), t.radius_s());
        }
        ui.push_id(ui.id("queue-rows"));
        int first = static_cast<int>(off / row_h), last = static_cast<int>((off + view.h) / row_h) + 1;
        for (int i = first; i <= last && i < static_cast<int>(queue_.size()); ++i) {
            const Track &tr = queue_[static_cast<size_t>(i)];
            Rect row{view.x, view.y + static_cast<float>(i) * row_h - off, view.w, row_h};
            ui.push_id(ui.id(static_cast<uint64_t>(i)));
            ui::Id row_id = ui.id("row"), remove_id = ui.id("remove");
            // Held, the row stops counting as hovered, so the button has to keep itself alive until its release lands.
            bool hot = ui.hovering(row) || ui.is_active(row_id) || ui.is_active(remove_id), cur = i == current_;
            float h = ui.anim(row_id, hot ? 1 : 0, 140);
            if (!cur && h > 0) {
                p.rect(row.inset(2), t.hover.with_alpha(h), t.radius_s());
            }
            cover(ui, {row.x + 10, row.y + 9, 36, 36}, tr.cover, t.radius_s());
            float tw = row.w - 58 - (hot ? 40 : 12);
            p.text(row.x + 58, row.y + 23, tr.title, {.size = t.fs(13), .color = cur ? t.accent : t.text}, tw);
            p.text(row.x + 58, row.y + 41, tr.artist, {.size = t.fs(12), .color = t.text_dim}, tw);
            if (hot && ui.icon_button(remove_id, {row.right() - 38, row.y + 13, 28, 28}, "x", 14)) {
                remove = i;
            }
            if (ui.interact(row_id, row).double_clicked) {
                play(queue_, i);
            }
            ui.pop_id();
        }
        ui.pop_id();
        ui.end_scroll();
        if (remove >= 0) {
            queue_.erase(queue_.begin() + remove);
            shuffle_history_.clear();
            shuffle_next_ = -1;
            if (remove < current_) {
                --current_;
            } else if (remove == current_) {
                // The playing track goes: carry on with the one after it.
                bool was_playing = playing();
                current_ = queue_.empty() ? -1 : std::min(current_, static_cast<int>(queue_.size()) - 1);
                ++play_gen_;
                stop_player();
                fetching_ = false;
                if (current_ >= 0 && was_playing) {
                    start();
                } else {
                    want_play_ = false;
                }
            }
            changed();
        }
    }

    void Shell::open_comments(ui::Ui &ui, Source source, Subject subject, std::string id, std::string title, std::string cover_url, bool follows) {
        thread_.replies.reset();
        set_thread(ui, source, subject, std::move(id), std::move(title), std::move(cover_url), follows);
        show_panel(Panel::comments);
    }

    void Shell::set_thread(ui::Ui &ui, Source source, Subject subject, std::string id, std::string title, std::string cover_url, bool follows) {
        thread_.follows = follows;
        bool same = thread_.source == source && thread_.subject == subject && thread_.id == id;
        // The same thread stays unless its first page failed, which this retries.
        if (same && (thread_.feeds[0]->loading || thread_.feeds[0]->page > 0)) {
            return;
        }
        if (!same) {
            thread_.opened = ui.now;
            thread_.draft = std::make_shared<CommentDraft>();
            if (ui.focus == ui.id("comment-draft") || ui.focus == ui.id("reply-draft")) {
                ui.focus = 0;
            }
        }
        thread_.source = source;
        thread_.subject = subject;
        thread_.id = std::move(id);
        thread_.title = std::move(title);
        thread_.cover = std::move(cover_url);
        thread_.tab = 0;
        thread_.time = ui.now;
        ++thread_.gen;
        thread_.replies.reset();
        for (auto &f : thread_.feeds) {
            f = std::make_shared<Feed>();
        }
        for (const char *sid : {"comments-hot", "comments-new"}) {
            ui.scroll_to(ui.id(sid), 0);
        }
        load_comments(0);
    }

    void Shell::load_comments(int tab) {
        Feed &f = *thread_.feeds[tab];
        if (f.loading || !f.more || (f.page && !f.error.empty())) {
            return;
        }
        f.loading = true;
        f.error.clear();
        spawn(fetch_comments(thread_.gen, tab, f.page + 1, f.cursor));
    }

    Task<void> Shell::fetch_comments(uint64_t gen, int tab, int page, std::string cursor) {
        auto r = co_await provider(thread_.source).comments(thread_.subject, thread_.id, tab == 0, page, std::move(cursor));
        if (gen != thread_.gen) {
            co_return;
        }
        Feed &f = *thread_.feeds[tab];
        f.loading = false;
        if (!r) {
            f.error = r.error();
            co_return redraw_();
        }
        if (!f.page) {
            f.since = now_();
        }
        f.page = page;
        if (page == 1 || r->total > 0) {
            f.total = r->total;
        }
        f.more = r->more && !r->comments.empty();
        f.cursor = std::move(r->cursor);
        settle_likes(thread_.source, r->comments);
        f.comments.insert(f.comments.end(), std::make_move_iterator(r->comments.begin()), std::make_move_iterator(r->comments.end()));
        redraw_();
    }

    void Shell::refresh_comments() {
        ++thread_.gen;
        for (auto &feed : thread_.feeds) {
            feed = std::make_shared<Feed>();
        }
        load_comments(thread_.tab);
        redraw_();
    }

    void Shell::open_replies(Comment root) {
        thread_.replies = std::make_shared<ReplyView>(std::move(root));
        thread_.replies->opened = now_();
        thread_.reset_scroll = true;
        load_replies(thread_.replies);
    }

    void Shell::load_replies(std::shared_ptr<ReplyView> view) {
        auto &feed = *view->feed;
        if (feed.loading || !feed.more || (feed.page && !feed.error.empty())) {
            return;
        }
        feed.loading = true;
        feed.error.clear();
        spawn(fetch_replies(std::move(view), feed.page + 1, feed.cursor));
    }

    Task<void> Shell::fetch_replies(std::shared_ptr<ReplyView> view, int page, std::string cursor) {
        auto feed = view->feed;
        auto r = co_await provider(thread_.source).comment_replies(thread_.subject, thread_.id, view->root.id, page, std::move(cursor));
        if (thread_.replies != view || view->feed != feed) {
            co_return;
        }
        feed->loading = false;
        if (!r) {
            feed->error = r.error();
            co_return redraw_();
        }
        if (!feed->page) {
            feed->since = now_();
        }
        feed->page = page;
        if (page == 1 || r->total > 0) {
            feed->total = r->total;
        }
        feed->more = r->more && !r->comments.empty();
        feed->cursor = std::move(r->cursor);
        settle_likes(thread_.source, r->comments);
        feed->comments.insert(feed->comments.end(), std::make_move_iterator(r->comments.begin()), std::make_move_iterator(r->comments.end()));
        redraw_();
    }

    void Shell::settle_likes(Source source, const std::vector<Comment> &comments) {
        for (const auto &comment : comments) {
            auto like = comment_likes_.find(comment_key(source, comment.id));
            if (like != comment_likes_.end() && !like->second.pending && like->second.liked == comment.liked) {
                auto update = [&](Comment &cached) {
                    if (cached.id == comment.id) {
                        cached.liked = comment.liked;
                        cached.likes = comment.likes;
                    }
                };
                for (auto &feed : thread_.feeds) {
                    for (auto &cached : feed->comments) {
                        update(cached);
                    }
                }
                if (thread_.replies) {
                    update(thread_.replies->root);
                    for (auto &cached : thread_.replies->feed->comments) {
                        update(cached);
                    }
                }
                comment_likes_.erase(like);
            }
        }
    }

    void Shell::clear_comment_writes(Source source) {
        std::erase_if(comment_likes_, [&](const auto &like) {
            return like.first.variant == static_cast<uint32_t>(source);
        });
        std::erase_if(comment_deletes_, [&](const auto &entry) {
            return entry.first.variant == static_cast<uint32_t>(source);
        });
    }

    void Shell::toggle_comment_like(Source source, const Comment &comment) {
        if (!provider(source).logged_in()) {
            return notify(lyren::trf("Sign in to {} to like comments", lyren::tr(platform(source).name)), "thumbs-up");
        }
        auto k = comment_key(source, comment.id);
        auto mine = comment_likes_.find(k);
        if (mine != comment_likes_.end() && mine->second.pending) {
            return;
        }
        std::optional<Like> previous;
        if (mine != comment_likes_.end()) {
            previous = mine->second;
        }
        bool liked = !(previous ? previous->liked : comment.liked);
        int likes = previous ? previous->likes : comment.likes;
        likes = std::max(0, likes + (liked ? 1 : -1));
        uint64_t gen = ++comment_write_gen_;
        if (mine == comment_likes_.end()) {
            comment_likes_.emplace(CacheKey(k), Like{.liked = liked, .likes = likes, .pending = gen});
        } else {
            mine->second = {.liked = liked, .likes = likes, .pending = gen};
        }
        redraw_();
        spawn([](Shell *self, Source source, Subject subject, std::string resource, std::string id, bool like, std::optional<Like> previous, uint64_t gen) -> Task<void> {
            auto r = co_await self->provider(source).like_comment(subject, std::move(resource), id, like);
            auto mine = self->comment_likes_.find(comment_key(source, id));
            if (mine == self->comment_likes_.end() || mine->second.pending != gen) {
                co_return;
            }
            if (!r) {
                if (previous) {
                    mine->second = *previous;
                } else {
                    self->comment_likes_.erase(mine);
                }
                self->notify(lyren::trf("Could not update comment like: {}", r.error()), "triangle-alert", true);
            } else {
                mine->second.pending = 0;
            }
            self->redraw_();
        }(this, source, thread_.subject, thread_.id, comment.id, liked, previous, gen));
    }

    void Shell::send_comment() {
        auto view = thread_.replies;
        auto &draft = view ? *view->draft : *thread_.draft;
        std::optional<Comment> target = draft.reply ? draft.reply : view ? std::optional(view->root) : std::nullopt;
        draft.sending = true;
        spawn([](Shell *self, Source source, Subject subject, std::string resource, std::shared_ptr<CommentDraft> thread_draft, std::shared_ptr<ReplyView> view,
                 std::optional<Comment> target) -> Task<void> {
            auto &draft = view ? *view->draft : *thread_draft;
            auto r = co_await self->provider(source).post_comment(subject, std::move(resource), draft.text, target ? target->id : "");
            draft.sending = false;
            // The thread draft changes only when the resource or account changes.
            if (self->thread_.draft != thread_draft) {
                co_return;
            }
            if (!r) {
                co_return self->notify(lyren::trf("Could not send comment: {}", lyren::tr(r.error())), "triangle-alert", true);
            }
            draft.text.clear();
            draft.reply.reset();
            draft.expanded = draft.focus = false;
            self->notify(std::string(lyren::tr("Comment posted")), "circle-check");
            auto current = self->thread_.replies;
            if (current && target && current->root.id == (view ? view->root.id : target->id)) {
                current->feed = std::make_shared<Feed>();
                self->load_replies(current);
            }
            if (current == view) {
                if (!view && target) {
                    self->open_replies(std::move(*target));
                } else if (!view) {
                    self->thread_.tab = 1;
                    self->thread_.time = self->now_();
                }
                self->thread_.reset_scroll = true;
            }
            self->refresh_comments();
        }(this, thread_.source, thread_.subject, thread_.id, thread_.draft, std::move(view), std::move(target)));
    }

    void Shell::confirm_comment_delete(ui::Ui &ui, Comment comment) {
        ui.open_modal(400, 232, [this, source = thread_.source, subject = thread_.subject, resource = thread_.id, comment](ui::Ui &u, Rect r) {
            const auto &t = u.theme();
            u.label({r.x, r.y, r.w - 40, 30}, lyren::tr("Delete comment?"), {.size = t.fs(20), .color = t.text});
            auto lines = u.painter().wrap_lines(comment.text, t.fs(13), r.w);
            for (size_t i = 0; i < std::min<size_t>(lines.size(), 3); ++i) {
                u.label({r.x, r.y + 46 + static_cast<float>(i) * 22, r.w, 22}, lines[i], {.size = t.fs(13), .color = t.text_dim});
            }
            float ok_w = std::max(96.f, button_width(u, lyren::tr("Delete"), true)), cancel_w = std::max(84.f, button_width(u, lyren::tr("Cancel"), false));
            Rect ok{r.right() - ok_w, r.bottom() - 38, ok_w, 38};
            if (u.button(u.id("comment-delete-cancel"), {ok.x - cancel_w - 8, ok.y, cancel_w, 38}, lyren::tr("Cancel"), ui::ButtonKind::ghost)) {
                u.close_modal();
            }
            if (!u.button(u.id("comment-delete-confirm"), ok, lyren::tr("Delete"), ui::ButtonKind::primary, "trash")) {
                return;
            }
            u.close_modal();
            uint64_t gen = ++comment_write_gen_;
            comment_deletes_.insert_or_assign(CacheKey(comment_key(source, comment.id)), gen);
            spawn([](Shell *self, Source source, Subject subject, std::string resource, std::string id, uint64_t gen) -> Task<void> {
                auto result = co_await self->provider(source).delete_comment(subject, resource, id);
                auto pending = self->comment_deletes_.find(comment_key(source, id));
                if (pending == self->comment_deletes_.end() || pending->second != gen) {
                    co_return;
                }
                self->comment_deletes_.erase(pending);
                if (!result) {
                    co_return self->notify(lyren::trf("Could not delete comment: {}", lyren::tr(result.error())), "triangle-alert", true);
                }
                self->comment_likes_.erase(comment_key(source, id));
                self->notify(std::string(lyren::tr("Comment deleted")), "trash");
                if (self->thread_.source != source || self->thread_.subject != subject || self->thread_.id != resource) {
                    co_return;
                }
                auto clear_target = [&](CommentDraft &draft) {
                    if (draft.reply && draft.reply->id == id) {
                        draft.reply.reset();
                    }
                };
                clear_target(*self->thread_.draft);
                if (auto view = self->thread_.replies) {
                    if (view->root.id == id) {
                        self->thread_.replies.reset();
                    } else {
                        clear_target(*view->draft);
                        view->feed = std::make_shared<Feed>();
                        self->load_replies(view);
                    }
                }
                self->refresh_comments();
            }(this, source, subject, resource, comment.id, gen));
        });
    }

    bool Shell::thread_playing() const {
        const Track *tr = current();
        return tr && thread_.subject == Subject::song && tr->source == thread_.source && tr->id == thread_.id && playing();
    }

    const LyricLine *Shell::thread_lyric() {
        const Track *tr = current();
        if (!tr || thread_.subject != Subject::song || tr->source != thread_.source || tr->id != thread_.id || lyrics_id_ != key(*tr)) {
            return nullptr;
        }
        int at = lyric_at(lyrics_, timeline().to_song(position()) + 0.2);
        return at >= 0 ? &lyrics_[static_cast<size_t>(at)] : nullptr;
    }

    // A round button resting over the comments; it opens into the card it writes in, the button staying in its corner
    // as the card's send button. Laid out before the list so it takes input first, raised over it once the list is drawn.
    float Shell::comment_composer(ui::Ui &ui, Rect r) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto view = thread_.replies;
        auto draft = view ? view->draft : thread_.draft;
        bool signed_in = provider(thread_.source).logged_in();
        const Comment *target = draft->reply ? &*draft->reply : view ? &view->root : nullptr;
        const LyricLine *lyric = signed_in ? thread_lyric() : nullptr;
        ui::Id field = ui.id(view ? "reply-draft" : "comment-draft");
        if (!signed_in) {
            draft->expanded = draft->focus = false;
        }
        if (draft->focus) {
            draft->expanded = true;
        }
        auto collapse = [&] {
            draft->expanded = draft->focus = false;
            if (ui.focus == field) {
                ui.focus = 0;
            }
            ui.redraw();
        };
        if (draft->expanded && !draft->sending && !ui.menu_open() && !ui.modal_open()) {
            auto &keys = ui.input().keys;
            auto escape = std::ranges::find_if(keys, [](const auto &key) {
                return key.keysym == XKB_KEY_Escape;
            });
            if (escape != keys.end()) {
                keys.erase(escape);
                collapse();
            }
        }
        if (!draft->expanded && ui.focus == field) {
            ui.focus = 0;
        }
        const bool editing = draft->expanded;
        float open = ui.spring(ui.id(view ? "reply-composer-open" : "comment-composer-open"), editing ? 1 : 0, 420, 0.18f);
        float reveal = smooth01(open);
        if (draft->focus) {
            ui.focus = field;
            draft->focus = false;
        }
        bool has_text = draft->text.find_first_not_of(" \t\r\n") != std::string::npos;

        // The card's measure: a line of context, the excerpt being answered, the editor growing with the text, the tools.
        const float line_h = std::round(t.fs(13) * 1.55f), pad = 14;
        Rect card{r.x + 12, 0, r.w - 24, 0};
        const float input_w = card.w - pad * 2;
        bool compact = r.h < 330;
        float quote_h = target && !compact ? line_h + C::gap : 0;
        float lines = static_cast<float>(p.wrap_lines(draft->text.empty() ? std::string_view(" ") : std::string_view(draft->text), t.fs(13), input_w).size());
        float rows = ui.spring(ui.id("comment-composer-rows"), std::clamp(lines, compact ? 1.f : 2.f, compact ? 2.f : 6.f), 220);
        card.h = pad + C::control + C::small_gap + quote_h + rows * line_h + C::gap + C::send + 12;
        card.y = r.bottom() - 12 - card.h;
        Rect fab{r.right() - 16 - C::fab, r.bottom() - 16 - C::fab, C::fab, C::fab};
        Rect sheet = mix(fab, card, open);
        sheet.h = std::max(sheet.h, C::fab), sheet.w = std::max(sheet.w, C::fab);
        const float radius = std::lerp(C::fab / 2, t.radius_l(), std::clamp(open, 0.f, 1.f));

        // The send button is the round button itself: it glides to the card's corner and stretches to fit its label.
        auto label = lyren::tr("Post");
        const float label_w = std::ceil(p.measure(label, {.size = t.fs(C::meta)}));
        const float wide = ui.anim(ui.id("comment-send-wide"), editing && has_text ? 1 : 0, 220);
        const float send_w = C::send + (label_w + C::gap) * wide;
        Rect docked{card.right() - 12 - send_w, card.bottom() - 12 - C::send, send_w, C::send};
        Rect send = mix(fab, docked, open);
        ui.disabled = panel_ != Panel::comments || draft->sending || (editing && (!has_text || open < 0.9f));
        auto post = ui.interact(ui.id(editing ? "comment-send" : "comment-fab"), send);
        ui.disabled = false;
        float hot = ui.anim(ui.id("comment-send-hover"), post.hovered ? 1 : 0, 160);
        float down = ui.anim(ui.id("comment-send-press"), post.held && post.hovered ? 1 : 0, 90);
        if (post.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }

        const size_t layer = p.mark();
        const bool listening = !editing && thread_playing() && t.motion > 0;
        if (reveal < 0.99f) {
            Fade resting(p, 1 - reveal);
            // While the song plays beside its comments the button breathes with it.
            if (listening) {
                float k = static_cast<float>(std::fmod(ui.now, 2.4) / 2.4);
                Rect ring = fab.inset(-10 * smooth01(k));
                p.rect(ring, {0, 0, 0, 0}, ring.w / 2, t.accent.with_alpha(0.45f * (1 - k)), 1.5f);
                ui.wake(ui.now + 1.0 / 30);
            }
            // What the button does, on hover: a tab sliding out of it.
            float tip = ui.anim(ui.id("comment-fab-tip"), post.hovered && !editing ? 1 : 0, 200);
            if (tip > 0) {
                auto words = !signed_in ? lyren::trf("Sign in to {} to comment", lyren::tr(platform(thread_.source).short_name))
                                        : std::string(lyren::tr(has_text ? "Continue writing" : "Write a comment"));
                float ww = std::ceil(p.measure(words, {.size = t.fs(C::meta)})) + C::padding * 2;
                Rect pill{fab.x - C::gap - ww + (1 - tip) * C::gap * 2, fab.y + (fab.h - C::control) / 2, ww, C::control};
                Fade shown(p, tip);
                auto paper = t.elevated;
                paper.a = std::max(paper.a, 0.94f);
                p.shadow({pill.x, pill.y + 2, pill.w, pill.h}, pill.h / 2, 10, t.shadow.with_alpha(0.35f));
                p.rect(pill, paper, pill.h / 2, t.border, 1);
                ui.label(pill, words, {.size = t.fs(C::meta), .color = t.text}, Align::center);
            }
        }
        if (open > 0.02f) {
            auto paper = t.elevated;
            paper.a = 1;
            float focus = ui.anim(ui.id("comment-composer-focus"), ui.focus == field && editing ? 1 : 0, 160);
            p.shadow({sheet.x, sheet.y + 8, sheet.w, sheet.h}, radius, 28, t.shadow.with_alpha(0.55f * std::min(1.f, open * 2)));
            p.rect(sheet, paper, radius, t.border.mix(t.accent, focus * 0.6f), 1);
        }
        if (reveal > 0.02f) {
            ui.push_clip(sheet);
            Fade writing(p, smooth01((reveal - 0.3f) / 0.7f));
            ui.disabled = panel_ != Panel::comments || !editing || draft->sending;
            Rect head{card.x + pad, card.y + pad, input_w, C::control};
            if (ui.icon_button(ui.id("comment-collapse"), {head.right() - C::control + 6, head.y, C::control, C::control}, "chevron-down", C::icon)) {
                collapse();
                ui.disabled = true;
            }
            if (target) {
                // Who this answers, as a tag; a reply chosen by hand can be dropped back to the thread.
                auto who = lyren::trf("Replying to {}", target->user);
                float ww = std::min(head.w - C::control - C::gap, std::ceil(p.measure(who, {.size = t.fs(C::meta)})) + C::padding * 2 + (draft->reply ? C::icon : 0));
                Rect tag{head.x, head.y + 2, ww, C::control - 4};
                p.rect(tag, t.accent.with_alpha(0.12f), tag.h / 2);
                ui.label({tag.x + C::padding - 4, tag.y, tag.w - C::padding - (draft->reply ? C::icon + 4 : 0), tag.h}, who, {.size = t.fs(C::meta), .color = t.accent});
                if (draft->reply && ui.icon_button(ui.id("comment-cancel-reply"), {tag.right() - C::icon - 8, tag.y + (tag.h - 20) / 2, 20, 20}, "x", 12)) {
                    draft->reply.reset();
                }
            } else {
                p.icon("pencil", {head.x, head.y + (head.h - C::small_icon) / 2, C::small_icon, C::small_icon}, t.accent);
                ui.label({head.x + C::small_icon + C::gap, head.y, head.w - C::control * 2, head.h}, lyren::tr("Write a comment"), {.size = t.fs(C::meta), .color = t.text_dim});
            }
            float y = head.bottom() + C::small_gap;
            if (quote_h > 0) {
                p.rect({head.x, y + 3, 2, line_h - 6}, t.accent.with_alpha(0.5f), 1);
                auto first = std::string_view(target->text).substr(0, target->text.find('\n'));
                ui.label({head.x + C::gap + 2, y, head.w - C::gap - 2, line_h}, first, {.size = t.fs(C::meta), .color = t.text_dim});
                y += quote_h;
            }
            Rect input{head.x, y, input_w, rows * line_h};
            if (!draft->insert.empty() && editing) {
                ui.focus = field;
                ui.input().commit.insert(0, std::exchange(draft->insert, {}));
            }
            {
                Fade enabled(p, draft->sending ? 0.5f : 1.f);
                ui.text_area(field, input, draft->text, lyren::tr(target ? "Write a reply…" : "What does this music bring to mind?"), false);
            }
            // The tools along the bottom: the line playing now, to quote, or the reminder that Enter is a new line.
            Rect tools{head.x, card.bottom() - 12 - C::send, docked.x - head.x - C::gap, C::send};
            if (lyric) {
                auto quote_label = lyren::tr("Quote lyric");
                float qw = std::min(tools.w, std::ceil(p.measure(quote_label, {.size = t.fs(C::meta)})) + C::small_icon + C::padding * 2);
                Rect chip{tools.x - 4, tools.y + (tools.h - 28) / 2, qw, 28};
                auto it = ui.interact(ui.id("comment-quote-lyric"), chip);
                float h = ui.anim(ui.id("comment-quote-lyric"), it.hovered ? 1 : 0, 140);
                p.rect(chip, t.text.with_alpha(0.04f + 0.05f * h), chip.h / 2, t.border, 1);
                p.icon("music", {chip.x + C::padding - 4, chip.y + (chip.h - C::small_icon) / 2, C::small_icon, C::small_icon}, t.accent);
                ui.label({chip.x + C::padding + C::small_icon, chip.y, chip.w, chip.h}, quote_label, {.size = t.fs(C::meta), .color = t.text_dim.mix(t.text, h)});
                if (it.clicked) {
                    draft->insert = "「" + lyric->text + "」";
                }
            } else {
                ui.label(tools, lyren::tr("Enter for a new line"), {.size = t.fs(C::time), .color = t.text_dim});
            }
            if (has_text) {
                size_t n = 0;
                for (size_t i = 0; i < draft->text.size(); i = utf8::next(draft->text, i)) {
                    ++n;
                }
                ui.label({tools.x, tools.y, tools.w, tools.h}, std::to_string(n), {.size = t.fs(C::time), .color = t.text_dim, .features = text::tabular_nums}, Align::right);
            }
            ui.disabled = false;
            ui.pop_clip();
        }

        // The button, drawn last: solid while it has something to do, softened while an open card is still empty.
        bool ready = !editing || has_text;
        ui::Color ink = t.accent.luminance() > 0.6f ? ui::Color{0.07f, 0.07f, 0.08f, 1} : white;
        Rect b = send.inset(down * 2 - hot * (1 - open) * 2);
        float b_radius = b.h / 2;
        if (open < 0.5f) {
            p.shadow({b.x, b.y + 4, b.w, b.h}, b_radius, 14 + 6 * hot, t.shadow.with_alpha(0.5f + 0.3f * hot));
        }
        p.rect(b, ready ? t.accent.mix(t.text, hot * 0.1f) : t.accent.with_alpha(0.28f), b_radius);
        ui::Color fg = ready ? ink : ink.with_alpha(0.75f);
        if (draft->sending) {
            ui.spinner(ui::center(b, C::icon, C::icon), fg, 1.5f);
        } else {
            float is = std::lerp(20.f, C::icon, reveal);
            float icon_x = b.right() - std::lerp(b.w / 2, C::send / 2, wide) - is / 2;
            Rect glyph{icon_x, b.y + (b.h - is) / 2, is, is};
            {
                Fade pen(p, 1 - reveal);
                p.icon(signed_in ? "pencil" : "qr-code", glyph, fg);
            }
            {
                Fade plane(p, reveal);
                p.icon("send", glyph, fg);
            }
            if (wide > 0) {
                Fade words(p, wide * reveal);
                ui.label({b.x + C::padding, b.y, label_w + 2, b.h}, label, {.size = t.fs(C::meta), .color = fg});
            }
            // A draft waits behind the closed button.
            if (has_text && reveal < 0.5f) {
                p.rect({b.right() - 13, b.y + 3, 10, 10}, t.surface, 5);
                p.rect({b.right() - 11, b.y + 5, 6, 6}, t.text, 3);
            }
        }
        const size_t layer_end = p.mark();

        if (post.clicked) {
            if (!signed_in) {
                open_account(ui);
                login_step_ = static_cast<int>(thread_.source);
                login_time_ = ui.now;
                spawn(qr_login());
            } else if (!editing) {
                draft->expanded = draft->focus = true;
            } else {
                send_comment();
            }
        }
        if (open > 0.02f) {
            // The card's blank space keeps its clicks and scrolling from the comments under it.
            ui.interact(ui.id("comment-composer-surface"), sheet);
            if (ui.pointing(sheet)) {
                ui.input().scroll_y = 0;
                ui.input().scroll_stop = false;
            }
        }
        composer_layer_ = {layer, layer_end};
        return r.bottom() - std::min(sheet.y, fab.y) + C::gap;
    }

    float Shell::comment_sleeve(ui::Ui &ui, Rect r, bool compact) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const float art = compact ? 44 : C::art;
        const bool record = thread_.subject != Subject::playlist;
        Rect sleeve{r.x + C::inset, r.y + C::padding, art, art};
        const float slide = record ? art * 0.46f * ui.progress(thread_.opened, 900, ui::ease::emphasized) : 0;
        const float tab_h = 36;
        const Rect bar{r.x + C::inset, sleeve.bottom() + (compact ? 6 : 12), r.w - C::inset * 2, tab_h};
        const float header_h = bar.bottom() + 1 - r.y;

        // The cover, blurred, lights the top of the panel and fades into it under the tabs.
        if (t.surface.a > 0.9f && !thread_.cover.empty()) {
            const auto &e = images_.get(thread_.cover, 128, true);
            if (e.ready) {
                Rect wash{r.x, r.y, r.w, header_h + 28};
                float a = ui.progress(e.ready_at, 500);
                {
                    Fade lit(p, a * (t.dark ? 0.6f : 0.4f));
                    p.image(wash, e.tex, t.radius_l(), white, cover_uv(1, wash.w / wash.h));
                }
                p.rect(wash, t.surface.with_alpha(0.25f * a), t.radius_l());
                p.gradient({wash.x, wash.y + wash.h * 0.3f, wash.w, wash.h * 0.7f + 1}, t.surface.with_alpha(0), t.surface);
            }
        }
        if (record) {
            // The record slides out of its sleeve when the thread opens, and turns while its song plays.
            Rect disc = sleeve.inset(art * 0.04f);
            disc.x += slide;
            float hue = cover_hue(thread_.cover, art);
            ui::Color label = hue < 0 ? t.accent : ui::accent_for_hue(hue, true);
            float angle = 0.6f;
            if (thread_playing() && t.motion > 0) {
                angle = static_cast<float>(std::fmod(ui.now, 3.6) / 3.6) * 2 * std::numbers::pi_v<float>;
                ui.wake(ui.now + 1.0 / 30);
            }
            vinyl(p, disc, label, angle);
        }
        p.shadow({sleeve.x, sleeve.y + 3, art, art}, t.radius_s(), 12, t.shadow.with_alpha(0.6f));
        cover(ui, sleeve, thread_.cover, t.radius_s(), thread_.subject == Subject::song ? "music" : thread_.subject == Subject::album ? "disc-album" : "list-music");

        Rect close{r.right() - C::inset - C::control + 6, r.y + C::padding - 4, C::control, C::control};
        const float tx = sleeve.right() + slide + 14, tw = close.x - tx - C::gap;
        const float title_h = std::ceil(t.fs(C::title) * 1.45f), sub_h = std::ceil(t.fs(C::meta) * 1.5f);
        const float ty = sleeve.y + (art - title_h - sub_h - 2) / 2;
        ui.label({tx, ty, tw, title_h}, thread_.title, {.size = t.fs(C::title), .color = t.text});
        std::string sub(lyren::tr(platform(thread_.source).name));
        // Either list knows the count once its first page is in.
        const Feed &shown = thread_.feeds[thread_.tab]->page ? *thread_.feeds[thread_.tab] : *thread_.feeds[1 - thread_.tab];
        if (shown.page || shown.total > 0) {
            sub += " · " + lyren::trf("{} comments", grouped(shown.total));
        }
        ui.label({tx, ty + title_h + 2, tw, sub_h}, sub, {.size = t.fs(C::meta), .color = t.text_dim, .features = text::tabular_nums});
        if (ui.icon_button(ui.id("comments-close"), close, "x", C::icon)) {
            show_panel(Panel::none);
        }

        // Two sides of the record: the loved and the latest, a short bar gliding under the one playing.
        const std::array<std::string_view, 2> labels = {lyren::tr("Popular"), lyren::tr("Newest")};
        const TextStyle tab_style{.size = t.fs(14)};
        float x = bar.x, centre = 0;
        for (int i = 0; i < 2; ++i) {
            float lw = p.measure(labels[static_cast<size_t>(i)], tab_style);
            Rect cell{x, bar.y, lw, bar.h};
            auto it = ui.interact(ui.id(i ? "comment-tab-new" : "comment-tab-hot"), cell.inset(-10, 0));
            float h = ui.anim(ui.id(i ? "comment-tab-new" : "comment-tab-hot"), it.hovered ? 1 : 0, 140);
            bool on = thread_.tab == i;
            ui.label(cell, labels[static_cast<size_t>(i)], {.size = tab_style.size, .color = on ? t.text : t.text_dim.mix(t.text, h * 0.6f)});
            if (on) {
                centre = cell.x + cell.w / 2;
            }
            if (it.clicked && !on) {
                thread_.tab = i;
                thread_.time = ui.now;
                load_comments(i);
                ui.redraw();
            }
            x += lw + 24;
        }
        float cx = ui.spring(ui.id("comment-tab-bar"), centre - bar.x, 300, 0.2f) + bar.x;
        p.rect({cx - 8, bar.bottom() - 4, 16, 3}, t.accent, 1.5f);
        ui.disabled = thread_.feeds[thread_.tab]->loading;
        if (ui.icon_button(ui.id("comments-refresh"), {bar.right() - C::control + 6, bar.y + (bar.h - C::control) / 2, C::control, C::control}, "refresh-cw", C::small_icon)) {
            refresh_comments();
        }
        ui.disabled = false;
        p.rect({r.x + C::inset, bar.bottom(), r.w - C::inset * 2, 1}, t.border.with_alpha(0.6f));
        return header_h;
    }

    void Shell::comments_panel(ui::Ui &ui, Rect r) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto reply = thread_.replies;
        ui::Id list_id = ui.id(reply ? "comment-replies" : thread_.tab ? "comments-new" : "comments-hot");
        if (std::exchange(thread_.reset_scroll, false)) {
            ui.scroll_to(list_id, 0);
        }
        bool compact = r.h < 420;
        float header_h;
        if (reply) {
            Rect bar{r.x + C::inset, r.y + C::padding - 4, r.w - C::inset * 2, C::control};
            if (ui.icon_button(ui.id("replies-back"), {bar.x - 6, bar.y, C::control, C::control}, "chevron-left", C::icon + 2)) {
                thread_.replies.reset();
                return ui.redraw();
            }
            int total = !reply->feed->page ? reply->root.replies : reply->feed->total;
            auto heading = lyren::tr("Replies");
            float hw = p.measure(heading, {.size = t.fs(C::title)});
            ui.label({bar.x + C::control, bar.y, hw + 1, bar.h}, heading, {.size = t.fs(C::title), .color = t.text});
            if (total > 0) {
                ui.label({bar.x + C::control + hw + C::gap, bar.y + 1, 80, bar.h}, grouped(total), {.size = t.fs(C::meta), .color = t.text_dim, .features = text::tabular_nums});
            }
            ui.disabled = reply->feed->loading;
            if (ui.icon_button(ui.id("comments-refresh"), {bar.right() - C::control * 2 - C::small_gap + 6, bar.y, C::control, C::control}, "refresh-cw", C::small_icon)) {
                reply->feed = std::make_shared<Feed>();
                load_replies(reply);
            }
            ui.disabled = false;
            if (ui.icon_button(ui.id("comments-close"), {bar.right() - C::control + 6, bar.y, C::control, C::control}, "x", C::icon)) {
                show_panel(Panel::none);
            }
            // The comment answered, pinned above its replies as a small quote.
            const float line_h = std::round(t.fs(C::name) * 1.6f);
            const float qw = bar.w - C::padding * 2;
            auto lines = p.wrap_lines(reply->root.text, t.fs(C::name), qw);
            size_t shown = std::min(lines.size(), compact ? size_t{1} : size_t{3});
            Rect quote{bar.x, bar.bottom() + C::gap + 2, bar.w, C::padding * 2 + 24 + C::small_gap + static_cast<float>(shown) * line_h};
            p.gradient(quote, t.accent.with_alpha(0.1f), t.accent.with_alpha(0.03f), t.radius_l());
            p.icon("quote-fill", {quote.right() - C::padding - 28, quote.y + C::gap + 2, 28, 28}, t.accent.with_alpha(0.18f));
            cover(ui, {quote.x + C::padding, quote.y + C::padding, 24, 24}, reply->root.avatar, 12, "user");
            ui.label({quote.x + C::padding + 24 + C::gap, quote.y + C::padding, qw - 24 - C::gap - 32, 24}, reply->root.user, {.size = t.fs(C::meta), .color = t.text_dim});
            for (size_t i = 0; i < shown; ++i) {
                ui.label({quote.x + C::padding, quote.y + C::padding + 24 + C::small_gap + static_cast<float>(i) * line_h, qw, line_h}, lines[i],
                         {.size = t.fs(C::name), .color = t.text});
            }
            header_h = quote.bottom() + C::gap - r.y;
        } else {
            header_h = comment_sleeve(ui, r, compact);
        }
        Feed &f = reply ? *reply->feed : *thread_.feeds[thread_.tab];
        Rect view{r.x, r.y + header_h, r.w, r.h - header_h};
        float composer_h = comment_composer(ui, view);

        float enter = ui.progress(reply ? reply->opened : thread_.time, 240);
        const float x = r.x + C::inset + (1 - enter) * C::padding * (reply || thread_.tab ? 1 : -1), w = r.w - C::inset * 2;
        {
            Fade fade(p, enter * ui.progress(f.since, 200));
            if (f.comments.empty()) {
                ui.push_clip(view);
                Rect room{view.x, view.y, view.w, view.h - composer_h};
                if (f.loading || !f.error.empty()) {
                    page_status(ui, room, f.loading, f.error, false);
                } else if (room.h < 150) {
                    ui.label(room, lyren::tr(reply ? "No replies yet" : "No comments yet"), {.size = t.fs(C::name), .color = t.text_dim}, Align::center);
                } else {
                    // Nothing said yet: a blank record waiting for its first words.
                    Rect c = ui::center(room, std::min(room.w, 320.f), 150);
                    Rect disc{c.x + (c.w - 64) / 2, c.y, 64, 64};
                    vinyl(p, disc, t.accent.with_alpha(0.85f), 0.6f);
                    ui.label({c.x, c.y + 80, c.w, 22}, lyren::tr(reply ? "No replies yet" : "No comments yet"), {.size = t.fs(15), .color = t.text}, Align::center);
                    ui.label({c.x, c.y + 106, c.w, 20}, lyren::tr("Join the conversation"), {.size = t.fs(C::meta), .color = t.text_dim}, Align::center);
                }
                ui.pop_clip();
            } else {
                // Heights are kept, the wrapped lines are not: they would point into comments that move as more arrive.
                const bool featured = !reply && thread_.tab == 0;
                float size = t.fs(C::body);
                if (f.heights_w != w || f.heights_size != size) {
                    f.heights.clear(), f.heights_w = w, f.heights_size = size;
                }
                while (f.heights.size() < f.comments.size()) {
                    size_t i = f.heights.size();
                    f.heights.push_back(comment(ui, f.comments[i], 0, 0, w, false, featured && i == 0));
                }
                // Below the last comment: the spinner while more load, or a mark that there are no more; then room to
                // scroll the last one clear of the writing button.
                float tail = f.loading || !f.more || !f.error.empty() ? 64 : C::padding;
                float content = tail + composer_h;
                for (float h : f.heights) {
                    content += h;
                }
                float off = ui.begin_scroll(list_id, view, content);
                Rect clip = ui.clip();
                float y = view.y - off;
                for (size_t i = 0; i < f.comments.size(); ++i) {
                    float h = f.heights[i];
                    if (y + h > clip.y && y < clip.bottom()) {
                        comment(ui, f.comments[i], x, y, w, true, featured && i == 0);
                    }
                    y += h;
                }
                if (f.loading) {
                    ui.spinner(ui::center({view.x, y, view.w, 64}, 20, 20), t.text_dim, 2);
                } else if (!f.error.empty()) {
                    ui.label({view.x + C::inset, y + C::small_gap, view.w - C::inset * 2, C::control}, f.error, {.size = t.fs(C::meta), .color = t.text_dim});
                    if (ui.button(ui.id("comments-retry"), {view.x + (view.w - 100) / 2, y + 32, 100, 28}, lyren::tr("Retry"), ui::ButtonKind::secondary, "refresh-cw")) {
                        f.error.clear();
                        if (reply) {
                            load_replies(reply);
                        } else {
                            load_comments(thread_.tab);
                        }
                    }
                } else if (f.more && y < view.bottom() + 400) {
                    if (reply) {
                        load_replies(reply);
                    } else {
                        load_comments(thread_.tab);
                    }
                } else if (!f.more) {
                    // The run-out groove: the end of the side.
                    const TextStyle end{.size = t.fs(C::meta), .color = t.text_dim.with_alpha(0.7f)};
                    const std::string_view end_words = lyren::tr("No more comments");
                    const float ew = p.measure(end_words, end), cx = view.x + view.w / 2, ey = y + 14;
                    ui.label({cx - ew / 2, ey, ew + 1, 40}, end_words, end);
                    for (float side : {-1.f, 1.f}) {
                        Rect rail{side < 0 ? cx - ew / 2 - C::gap * 5 : cx + ew / 2 + C::gap, ey + 20, C::gap * 4, 1};
                        p.hgradient(rail, t.border.with_alpha(side < 0 ? 0 : 1), t.border.with_alpha(side < 0 ? 1 : 0), 0.5f);
                    }
                }
                if (t.surface.a > 0.9f && off > 0.5f) {
                    p.gradient({view.x + 1, view.y, view.w - 2, C::padding}, t.surface, t.surface.with_alpha(0));
                }
                ui.end_scroll();
            }
        }
        p.raise(composer_layer_.first, composer_layer_.second);
    }

    void Shell::comment_picture(ui::Ui &ui, const Comment &c, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const auto &e = images_.picture(c.image, static_cast<uint32_t>(Images::tier(std::max(r.w, r.h) * p.scale())));
        float a = e.ready ? ui.progress(e.ready_at, 320, ui::ease::standard) : 0;
        float aspect = e.ready && e.h ? static_cast<float>(e.w) / static_cast<float>(e.h) : 1;
        if (c.sticker) {
            // Stickers stand on the page as they are, whole and unframed.
            if (e.ready) {
                Rect fit = aspect > 1 ? Rect{r.x, r.y + (r.h - r.w / aspect) / 2, r.w, r.w / aspect} : Rect{r.x, r.y, r.h * aspect, r.h};
                Fade fade(p, a);
                p.image(fit, e.tex, 0);
            }
            return;
        }
        auto it = ui.interact(ui.id("comment-picture"), r);
        float hot = ui.anim(ui.id("comment-picture"), it.hovered ? 1 : 0, 160);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        if (a < 1) {
            p.rect(r, t.text.with_alpha(0.06f), t.radius);
            p.icon("image", ui::center(r, 22, 22), t.text_dim.with_alpha(0.6f * (1 - a)));
        }
        if (e.ready) {
            Fade fade(p, a);
            p.image(r, e.tex, t.radius, white.mix({0.86f, 0.86f, 0.86f, 1}, hot * 0.4f), cover_uv(aspect, r.w / r.h));
        }
        p.rect(r, {0, 0, 0, 0}, t.radius, t.text.with_alpha(0.08f + 0.1f * hot), 1);
        std::string_view url = c.image;
        if (url.substr(0, url.find('?')).ends_with(".gif")) {
            Rect badge{r.x + 8, r.bottom() - 8 - 18, 32, 18};
            p.rect(badge, {0, 0, 0, 0.55f}, 9);
            ui.label(badge, "GIF", {.size = t.fs(10), .color = white}, Align::center);
        }
        if (it.clicked) {
            show_picture(ui, c.image, c.image_w > 0 && c.image_h > 0 ? static_cast<float>(c.image_w) / static_cast<float>(c.image_h) : e.ready ? aspect : 4.f / 3);
        }
    }

    void Shell::show_picture(ui::Ui &ui, std::string url, float aspect) {
        float w = ui.width * 0.84f, h = w / aspect;
        if (h > ui.height * 0.84f) {
            h = ui.height * 0.84f, w = h * aspect;
        }
        ui.open_modal(
            std::round(w), std::round(h),
            [this, url = std::move(url)](ui::Ui &u, Rect r) {
                auto &p = u.painter();
                const auto &t = u.theme();
                const auto &e = images_.picture(url, 1024);
                p.shadow({r.x, r.y + 12, r.w, r.h}, t.radius_l(), 48, t.shadow);
                if (e.ready) {
                    p.image(r, e.tex, t.radius_l(), white, cover_uv(static_cast<float>(e.w) / static_cast<float>(std::max(e.h, 1u)), r.w / r.h));
                } else {
                    p.rect(r, t.elevated, t.radius_l());
                    u.spinner(ui::center(r, 24, 24), t.text_dim, 2);
                }
                if (u.interact(u.id("picture-view"), r).clicked) {
                    u.close_modal();
                }
            },
            true);
    }

    float Shell::comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw, bool featured) {
        using C = CommentStyle;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Source source = thread_.source;
        const float pad = featured ? 18 : 0;
        const TextStyle body{.size = t.fs(featured ? C::quote : C::body), .color = t.text}, quote{.size = t.fs(C::name), .color = t.text_dim};
        const float lh = std::round(body.size * (featured ? 1.7f : 1.65f)), qh = std::round(quote.size * 1.55f);
        const float name_h = std::ceil(t.fs(C::name) * 1.45f), time_h = std::ceil(t.fs(C::time) * 1.45f);
        const float action_h = std::max(C::control, std::ceil(t.fs(C::meta) * 1.6f));
        const float tx = featured ? x + pad : x + C::avatar + 12, tw = x + w - pad - tx;
        std::string replied = c.reply.empty() ? "" : '@' + c.reply_user + "：" + c.reply;
        auto lines = c.text.empty() ? std::vector<std::string_view>{} : p.wrap_lines(c.text, body.size, tw);
        auto quoted = replied.empty() ? std::vector<std::string_view>{} : p.wrap_lines(replied, quote.size, tw - C::padding - C::gap);
        const float quote_h = quoted.empty() ? 0 : static_cast<float>(quoted.size()) * qh + C::gap * 2;
        Rect picture = picture_box(c, tw);

        // One pass lays out both: a quote card with its author underneath, or a row with its author on top.
        float top = featured ? y + C::gap + pad : y + 14;
        float badge_y = top, ident_y = top;
        float cy = featured ? top + std::ceil(t.fs(C::time) * 1.6f) + C::gap : top + std::max(C::avatar, name_h + time_h) + 6;
        float body_y = cy;
        cy += static_cast<float>(lines.size()) * lh;
        float quote_y = cy + C::gap;
        cy += quote_h > 0 ? C::gap + quote_h : 0;
        picture.x = tx, picture.y = cy + C::gap;
        cy += picture.h > 0 ? C::gap + picture.h : 0;
        float author_y = cy + 12;
        if (featured) {
            cy += 12 + 26;
        }
        float action_y = cy + (featured ? 2 : 4);
        cy = action_y + action_h;
        float h = cy + (featured ? 12 + C::gap : 10) - y;
        if (!draw) {
            return h;
        }

        auto mine = comment_likes_.find(comment_key(source, c.id));
        bool liked = mine != comment_likes_.end() ? mine->second.liked : c.liked;
        bool owned = c.owned && provider(source).logged_in();
        int count = mine != comment_likes_.end() ? mine->second.likes : c.likes;
        std::string n = count > 0 ? short_count(count) : "";
        ui.push_id(ui.id(key(source, c.id)));
        if (featured) {
            // The top comment, set as a pull quote on a card tinted by the record.
            Rect card{x, y + C::gap, w, h - C::gap * 2};
            p.gradient(card, t.accent.with_alpha(0.14f), t.accent.with_alpha(0.035f), t.radius_l());
            p.rect(card, {0, 0, 0, 0}, t.radius_l(), t.accent.with_alpha(0.2f), 1);
            p.icon("quote-fill", {card.right() - pad - 36, card.y + 12, 36, 36}, t.accent.with_alpha(0.16f));
            float bh = std::ceil(t.fs(C::time) * 1.6f);
            p.icon("sparkles", {tx, badge_y + (bh - 12) / 2, 12, 12}, t.accent);
            ui.label({tx + 12 + C::small_gap + 2, badge_y, tw, bh}, lyren::tr("Top comment"), {.size = t.fs(C::time), .color = t.accent});
        } else {
            cover(ui, {x, ident_y, C::avatar, C::avatar}, c.avatar, C::avatar / 2, "user");
        }
        // The like: a count to read before anything else on a row, beside the thanks on a card.
        float pop = ui.spring(ui.id("comment-like-pop"), liked ? 1 : 0, 320, 0.45f);
        float like_w = comment_action_width(ui, n);
        Rect like = featured ? Rect{x + w - pad - like_w, action_y, like_w, action_h} : Rect{x + w - like_w, ident_y, like_w, name_h};
        ui.disabled = mine != comment_likes_.end() && mine->second.pending;
        {
            Fade dim(p, ui.disabled ? 0.4f : 1.f);
            if (comment_action(ui, ui.id("comment-like"), like, n, liked ? "thumbs-up-fill" : "thumbs-up", liked ? t.accent : t.text_dim,
                               1 + 0.4f * std::abs(pop - (liked ? 1.f : 0.f)))) {
                toggle_comment_like(source, c);
            }
        }
        ui.disabled = false;
        if (featured) {
            cover(ui, {tx, author_y + 1, 24, 24}, c.avatar, 12, "user");
            float nw = std::min(p.measure(c.user, {.size = t.fs(C::name)}), tw - 24 - C::gap * 2 - 80);
            ui.label({tx + 24 + C::gap, author_y, nw + 1, 26}, c.user, {.size = t.fs(C::name), .color = t.text});
            ui.label({tx + 24 + C::gap * 2 + nw, author_y + 1, tw, 26}, c.time, {.size = t.fs(C::time), .color = t.text_dim});
        } else {
            ui.label({tx, ident_y, like.x - tx - C::gap, name_h}, c.user, {.size = t.fs(C::name), .color = t.text.mix(t.text_dim, 0.25f)});
            ui.label({tx, ident_y + name_h, tw, time_h}, c.time, {.size = t.fs(C::time), .color = t.text_dim});
        }
        float ly = body_y;
        for (auto line : lines) {
            ui.label({tx, ly, tw, lh}, line, body);
            ly += lh;
        }
        if (!quoted.empty()) {
            Rect q{tx, quote_y, tw, quote_h};
            p.rect(q, t.text.with_alpha(featured ? 0.05f : 0.04f), t.radius_s());
            p.rect({q.x + C::gap, q.y + C::gap, 2, q.h - C::gap * 2}, t.accent.with_alpha(0.5f), 1);
            float qy = q.y + C::gap;
            for (auto line : quoted) {
                ui.label({q.x + C::padding + 2, qy, tw - C::padding - C::gap, qh}, line, quote);
                qy += qh;
            }
        }
        if (picture.h > 0) {
            comment_picture(ui, c, picture);
        }
        // Answering it, and the thread already under it.
        std::string_view reply_label = lyren::tr("Reply");
        Rect reply_button{tx, action_y, comment_action_width(ui, reply_label), action_h};
        {
            auto draft = thread_.replies ? thread_.replies->draft : thread_.draft;
            ui.disabled = draft->sending;
            if (comment_action(ui, ui.id("comment-reply"), reply_button, reply_label, "message-circle", t.text_dim)) {
                draft->reply = c;
                draft->focus = true;
            }
            ui.disabled = false;
        }
        float end = featured ? like.x - C::gap : x + w;
        if (owned) {
            Rect bin{end - C::control + 6, action_y + (action_h - C::control) / 2, C::control, C::control};
            ui.disabled = comment_deletes_.contains(comment_key(source, c.id));
            {
                Fade dim(p, ui.disabled ? 0.4f : 1.f);
                if (ui.icon_button(ui.id("comment-delete"), bin, "trash", C::small_icon)) {
                    confirm_comment_delete(ui, c);
                }
            }
            ui.disabled = false;
            end = bin.x - C::small_gap;
        }
        if (c.replies > 0) {
            std::string label = lyren::trf("{} replies", c.replies);
            float cw = std::min(thread_chip_width(ui, label), end - reply_button.right() - C::padding);
            if (cw > C::small_icon * 3 && thread_chip(ui, ui.id("comment-replies"), {reply_button.right() + C::padding, action_y + (action_h - 26) / 2, cw, 26}, label)) {
                open_replies(c);
            }
        }
        if (!featured) {
            p.rect({tx, y + h - 1, tw, 1}, t.border.with_alpha(0.35f));
        }
        ui.pop_id();
        return h;
    }

} // namespace lyren
