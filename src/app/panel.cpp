#include <algorithm>

#include "app/draw.hpp"
#include "app/shell.hpp"
#include "core/i18n.hpp"

// The panel beside the page and the stage: the play queue, or the comments on a song, an album or a playlist.
namespace lyren {

    using ui::Align;
    using ui::Rect;
    using ui::TextStyle;

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
            self->notify(std::string(lyren::tr("Comment sent")), "circle-check");
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

    float Shell::comment_composer(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto &provider = this->provider(thread_.source);
        if (!provider.logged_in()) {
            constexpr float height = 58;
            p.rect({r.x + 18, r.bottom() - height, r.w - 36, 1}, t.border);
            auto label = lyren::trf("Sign in to {} to comment", lyren::tr(platform(thread_.source).short_name));
            if (ui.button(ui.id("comment-login"), {r.x + 18, r.bottom() - 46, r.w - 36, 34}, label, ui::ButtonKind::secondary, "qr-code")) {
                open_account(ui);
                login_step_ = static_cast<int>(thread_.source);
                login_time_ = ui.now;
                spawn(qr_login());
            }
            return height;
        }
        auto view = thread_.replies;
        auto draft = view ? view->draft : thread_.draft;
        const Comment *target = draft->reply ? &*draft->reply : view ? &view->root : nullptr;
        float line_h = std::round(t.fs(13) * 1.55f);
        float input_h = (r.h < 420 ? line_h : line_h * 2) + 20;
        float height = input_h + 58 + (target ? 26 : 0);
        float top = r.bottom() - height;
        p.rect({r.x + 18, top, r.w - 36, 1}, t.border);
        top += 12;
        ui.disabled = draft->sending || panel_ != Panel::comments;
        if (target) {
            ui.label({r.x + 18, top, r.w - 68, 22}, lyren::trf("Replying to {}", target->user), {.size = t.fs(12), .color = t.text_dim});
            if (draft->reply && ui.icon_button(ui.id("comment-cancel-reply"), {r.right() - 42, top - 2, 24, 24}, "x", 13)) {
                draft->reply.reset();
            }
            top += 26;
        }
        ui::Id field = ui.id(view ? "reply-draft" : "comment-draft");
        if (std::exchange(draft->focus, false)) {
            ui.focus = field;
        }
        {
            Fade fade(p, draft->sending ? 0.5f : 1.f);
            ui.text_area(field, {r.x + 18, top, r.w - 36, input_h}, draft->text, lyren::tr(target ? "Write a reply…" : "Write a comment…"));
        }
        float bottom = top + input_h + 8;
        ui.label({r.x + 18, bottom, r.w - 150, 28}, lyren::tr("Enter for a new line"), {.size = t.fs(11), .color = t.text_dim});
        std::string_view label = lyren::tr(draft->sending ? "Sending…" : "Send");
        float send_w = std::max(76.f, button_width(ui, label, false));
        ui.disabled = draft->sending || panel_ != Panel::comments || draft->text.find_first_not_of(" \t\r\n") == std::string::npos;
        {
            Fade dim(p, ui.disabled ? 0.4f : 1.f);
            if (ui.button(ui.id("comment-send"), {r.right() - 18 - send_w, bottom, send_w, 28}, label, ui::ButtonKind::primary)) {
                send_comment();
            }
        }
        ui.disabled = false;
        return height;
    }

    void Shell::comments_panel(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto reply = thread_.replies;
        if (std::exchange(thread_.reset_scroll, false)) {
            ui.scroll_to(ui.id(reply ? "comment-replies" : thread_.tab ? "comments-new" : "comments-hot"), 0);
        }
        Feed &f = reply ? *reply->feed : *thread_.feeds[thread_.tab];
        if (reply) {
            if (ui.icon_button(ui.id("replies-back"), {r.x + 14, r.y + 16, 32, 32}, "chevron-left", 18)) {
                thread_.replies.reset();
                return ui.redraw();
            }
            ui.label({r.x + 56, r.y + 18, r.w - 154, 28}, lyren::trf("Replies · {}", f.page ? f.total : reply->root.replies), {.size = t.fs(15), .color = t.text});
            Rect quote{r.x + 18, r.y + 64, r.w - 36, 52};
            p.rect(quote, t.text.with_alpha(0.05f), t.radius_s());
            p.text(quote.x + 10, quote.y + 17, reply->root.user, {.size = t.fs(12), .color = t.text_dim}, quote.w - 20);
            auto first_line = std::string_view(reply->root.text).substr(0, reply->root.text.find('\n'));
            p.text(quote.x + 10, quote.y + 38, first_line, {.size = t.fs(13), .color = t.text}, quote.w - 20);
        } else {
            Rect art{r.x + 18, r.y + 18, 44, 44};
            cover(ui, art, thread_.cover, t.radius_s(), thread_.subject == Subject::song ? "music" : thread_.subject == Subject::album ? "disc-album" : "list-music");
            float tx = art.right() + 12, tw = r.right() - tx - 88;
            float head = p.text(tx, art.y + 18, lyren::tr("Comments"), {.size = t.fs(15), .color = t.text}, tw);
            if (f.total > 0) {
                p.text(tx + head + 8, art.y + 18, grouped(f.total), {.size = t.fs(12), .color = t.text_dim, .features = text::tabular_nums}, tw - head - 8);
            }
            p.text(tx, art.y + 38, thread_.title, {.size = t.fs(12), .color = t.text_dim}, tw);
            const std::array<std::string_view, 2> tabs = {lyren::tr("Popular"), lyren::tr("Newest")};
            float tabs_w = button_width(ui, tabs[0], false) + button_width(ui, tabs[1], false) - 16;
            if (ui.tabs(ui.id("comment-tabs"), {r.x + 18, r.y + 78, tabs_w, 32}, tabs, thread_.tab, ui::TabStyle::pill)) {
                thread_.time = ui.now;
                load_comments(thread_.tab);
            }
        }
        ui.disabled = f.loading;
        if (ui.icon_button(ui.id("comments-refresh"), {r.right() - 82, r.y + 16, 32, 32}, "refresh-cw", 16)) {
            if (reply) {
                reply->feed = std::make_shared<Feed>();
                load_replies(reply);
            } else {
                refresh_comments();
            }
            ui.disabled = false;
            return ui.redraw();
        }
        ui.disabled = false;
        if (ui.icon_button(ui.id("comments-close"), {r.right() - 46, r.y + 16, 32, 32}, "x", 16)) {
            show_panel(Panel::none);
        }
        p.rect({r.x + 18, r.y + 124, r.w - 36, 1}, t.border);
        float composer_h = comment_composer(ui, r);

        float enter = ui.progress(reply ? reply->opened : thread_.time, 380);
        Fade fade(p, enter);
        Rect view{r.x, r.y + 125, r.w, r.h - 125 - 8 - composer_h};
        const float x = r.x + 18 + (1 - enter) * 20 * (reply || thread_.tab ? 1 : -1), w = r.w - 36;
        if (f.comments.empty()) {
            page_status(ui, view, f.loading, f.error.empty() && !f.loading ? std::string(lyren::tr(reply ? "No replies yet" : "No comments yet")) : f.error, false);
            return;
        }
        // Heights are kept, the wrapped lines are not: they would point into comments that move as more arrive.
        float size = t.fs(14);
        if (f.heights_w != w || f.heights_size != size) {
            f.heights.clear(), f.heights_w = w, f.heights_size = size;
        }
        while (f.heights.size() < f.comments.size()) {
            f.heights.push_back(comment(ui, f.comments[f.heights.size()], 0, 0, w, false));
        }
        // Below the last comment: the spinner while more load, or a mark that there are no more.
        float content = f.loading || !f.more || !f.error.empty() ? 72 : 24;
        for (float h : f.heights) {
            content += h;
        }
        float off = ui.begin_scroll(ui.id(reply ? "comment-replies" : thread_.tab ? "comments-new" : "comments-hot"), view, content);
        Rect clip = ui.clip();
        float y = view.y - off;
        for (size_t i = 0; i < f.comments.size(); ++i) {
            float h = f.heights[i];
            if (y + h > clip.y && y < clip.bottom()) {
                // The first page arrives one comment after another.
                float a = ui.progress(f.since + 0.03 * static_cast<double>(std::min<size_t>(i, 10)), 380);
                Fade arrive(p, a);
                comment(ui, f.comments[i], x, y + (1 - a) * 12, w, true);
            }
            y += h;
        }
        if (f.loading) {
            ui.spinner(ui::center({view.x, y, view.w, 64}, 20, 20), t.text_dim, 2);
        } else if (!f.error.empty()) {
            ui.label({view.x + 18, y + 4, view.w - 36, 24}, f.error, {.size = t.fs(12), .color = t.text_dim});
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
            const TextStyle end{.size = t.fs(12), .color = t.text_dim.with_alpha(0.7f)};
            const std::string_view words = lyren::tr("No more comments");
            const float ww = p.measure(words, end), cx = view.x + view.w / 2;
            ui.label({cx - ww / 2, y + 14, ww + 1, 40}, words, end);
            for (float side : {-1.f, 1.f}) {
                p.rect({cx + side * (ww / 2 + 12) - (side < 0 ? 24 : 0), y + 34, 24, 1}, t.border);
            }
        }
        ui.end_scroll();
    }

    float Shell::comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Source source = thread_.source;
        TextStyle body{.size = t.fs(14), .color = t.text}, quote{.size = t.fs(13), .color = t.text_dim}, small{.size = t.fs(12), .color = t.text_dim};
        const float lh = std::round(body.size * 1.6f), qh = std::round(quote.size * 1.55f), tx = x + 44, tw = w - 44;
        std::string replied = c.reply.empty() ? "" : '@' + c.reply_user + "：" + c.reply;
        auto lines = p.wrap_lines(c.text, body.size, tw);
        std::vector<std::string_view> quoted;
        if (!replied.empty()) {
            quoted = p.wrap_lines(replied, quote.size, tw - 24);
        }
        // Avatar beside the name and likes, then the text, the comment it answers and the time.
        float quote_h = quoted.empty() ? 0 : static_cast<float>(quoted.size()) * qh + 22;
        float h = 16 + 24 + static_cast<float>(lines.size()) * lh + quote_h + 22 + 16;
        if (!draw) {
            return h;
        }
        ui.push_id(ui.id(key(source, c.id)));
        float top = y + 16;
        cover(ui, {x, top, 32, 32}, c.avatar, 16, "user");
        auto mine = comment_likes_.find(comment_key(source, c.id));
        bool liked = mine != comment_likes_.end() ? mine->second.liked : c.liked;
        std::string n = short_count(mine != comment_likes_.end() ? mine->second.likes : c.likes);
        float likes_w = p.measure(n, small) + 34;
        ui.disabled = mine != comment_likes_.end() && mine->second.pending;
        Rect button{x + w - likes_w, top - 5, likes_w, 28};
        {
            Fade dim(p, ui.disabled ? 0.4f : 1.f);
            auto it = ui.interact(ui.id("comment-like"), button);
            if (it.hovered) {
                p.rect(button, t.text.with_alpha(0.06f), t.radius_s());
            }
            if (it.clicked) {
                toggle_comment_like(source, c);
            }
            p.icon("thumbs-up", {button.x + 4, top + 2, 14, 14}, liked ? t.accent : t.text_dim);
            ui.label({button.x + 24, top, likes_w - 24, 18}, n, {.size = small.size, .color = liked ? t.accent : t.text_dim});
        }
        ui.disabled = false;
        p.text(tx, top + 13, c.user, {.size = t.fs(13), .color = t.text_dim.mix(t.text, 0.35f)}, tw - likes_w - 8);
        float ly = top + 22;
        for (auto l : lines) {
            p.text(tx, ly + lh * 0.72f, l, body, tw);
            ly += lh;
        }
        if (!quoted.empty()) {
            Rect q{tx, ly + 8, tw, quote_h - 10};
            p.rect(q, t.text.with_alpha(0.05f), t.radius_s());
            p.rect({q.x, q.y + 6, 2, q.h - 12}, t.text.with_alpha(0.14f), 1);
            float qy = q.y + 6;
            for (auto l : quoted) {
                p.text(q.x + 12, qy + qh * 0.72f, l, quote, tw - 24);
                qy += qh;
            }
            ly += quote_h;
        }
        float time_w = tw;
        float actions_right = x + w;
        if (c.owned && provider(source).logged_in()) {
            ui.disabled = comment_deletes_.contains(comment_key(source, c.id));
            {
                Fade dim(p, ui.disabled ? 0.4f : 1.f);
                if (ui.icon_button(ui.id("comment-delete"), {actions_right - 24, ly + 2, 24, 24}, "trash", 13)) {
                    confirm_comment_delete(ui, c);
                }
            }
            ui.disabled = false;
            actions_right -= 30;
            time_w -= 30;
        }
        {
            auto draft = thread_.replies ? thread_.replies->draft : thread_.draft;
            std::string_view label = lyren::tr("Reply");
            float rw = p.measure(label, small) + 16;
            ui.disabled = draft->sending;
            if (ui.button(ui.id("comment-reply"), {actions_right - rw, ly + 2, rw, 24}, label, ui::ButtonKind::ghost)) {
                draft->reply = c;
                draft->focus = true;
            }
            ui.disabled = false;
            actions_right -= rw + 8;
            time_w -= rw + 8;
        }
        if (c.replies > 0) {
            std::string label = lyren::trf("{} replies", c.replies);
            float rw = p.measure(label, small) + 20;
            Rect button{actions_right - rw, ly + 2, rw, 24};
            if (ui.interact(ui.id("comment-replies"), button).clicked) {
                open_replies(c);
            }
            ui.label(button, label, {.size = small.size, .color = t.accent}, Align::right);
            time_w -= rw + 12;
        }
        p.text(tx, ly + 18, c.time, {.size = small.size, .color = t.text_dim.with_alpha(0.75f)}, time_w);
        p.rect({tx, y + h - 1, tw, 1}, t.border.with_alpha(0.7f));
        ui.pop_id();
        return h;
    }

} // namespace lyren
