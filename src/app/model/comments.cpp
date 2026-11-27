#include "app/model/comments.hpp"

#include <iterator>

#include "app/model/library.hpp"
#include "core/i18n.hpp"

namespace lyren {

    Comments::Comments(Env &env, Library &library)
    : env_(env) {
        library.subscribe([this](const Library::Event &event) {
            using Kind = Library::Event::Kind;
            const bool mine = thread_.source == event.source;
            switch (event.kind) {
                case Kind::left:
                    likes_.forget(event.source);
                    removals_.forget(event.source);
                    if (mine) {
                        ++thread_.gen;
                        for (auto &feed : thread_.feeds) {
                            feed = std::make_shared<Feed>();
                        }
                        thread_.draft = std::make_shared<Draft>();
                        thread_.replies.reset();
                        if (!thread_.id.empty()) {
                            load(thread_.tab);
                        }
                    }
                    break;
                case Kind::account:
                    // What was being written was the other account's to send.
                    if (mine) {
                        thread_.draft = std::make_shared<Draft>();
                        thread_.replies.reset();
                        if (!thread_.id.empty()) {
                            refresh();
                        }
                    }
                    break;
                case Kind::signed_out:
                    if (mine && !thread_.id.empty()) {
                        refresh();
                    }
                    break;
                default:
                    break;
            }
        });
    }

    void Comments::open(Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows) {
        thread_.follows = follows;
        const bool same = thread_.is(source, subject, id);
        // The same thread stays unless its first page failed, which this retries.
        if (same && (thread_.feeds[0]->loading || thread_.feeds[0]->page > 0)) {
            return;
        }
        if (!same) {
            thread_.opened = env_.now();
            thread_.draft = std::make_shared<Draft>();
            ++thread_.serial;
        }
        thread_.source = source;
        thread_.subject = subject;
        thread_.id = std::move(id);
        thread_.title = std::move(title);
        thread_.cover = std::move(cover);
        thread_.tab = 0;
        thread_.tab_since = env_.now();
        thread_.replies.reset();
        thread_.to_top = true;
        refresh();
    }

    void Comments::load(int tab) {
        Feed &feed = *thread_.feeds[tab];
        // A first page that failed is asked for again; later pages wait for their retry.
        if (!feed.open(true)) {
            return;
        }
        feed.loading = true;
        feed.error.clear();
        spawn(fetch(thread_.gen, tab, feed.page + 1, feed.cursor));
    }

    void Comments::append(Feed &feed, CommentPage page) {
        settle_likes(page.comments);
        feed.append(std::move(page.comments), std::move(page.cursor), page.more, env_.now(), page.total);
    }

    Task<void> Comments::fetch(uint64_t gen, int tab, int page, std::string cursor) {
        auto r = co_await env_.provider(thread_.source).comments(thread_.subject, thread_.id, tab == 0, page, std::move(cursor));
        if (gen != thread_.gen) {
            co_return;
        }
        Feed &feed = *thread_.feeds[tab];
        feed.loading = false;
        if (r) {
            append(feed, std::move(*r));
        } else {
            feed.error = r.error();
        }
        env_.redraw();
    }

    void Comments::refresh() {
        ++thread_.gen;
        for (auto &feed : thread_.feeds) {
            feed = std::make_shared<Feed>();
        }
        load(thread_.tab);
        env_.redraw();
    }

    void Comments::set_tab(int tab) {
        thread_.tab = tab;
        thread_.tab_since = env_.now();
        load(tab);
    }

    void Comments::open_replies(Comment root) {
        thread_.replies = std::make_shared<Replies>(std::move(root));
        thread_.replies->opened = env_.now();
        thread_.to_top = true;
        load_replies(thread_.replies);
    }

    void Comments::load_replies(std::shared_ptr<Replies> view) {
        Feed &feed = *view->feed;
        if (!feed.open(true)) {
            return;
        }
        feed.loading = true;
        feed.error.clear();
        spawn(fetch_replies(std::move(view), feed.page + 1, feed.cursor));
    }

    Task<void> Comments::fetch_replies(std::shared_ptr<Replies> view, int page, std::string cursor) {
        auto feed = view->feed;
        auto r = co_await env_.provider(thread_.source).comment_replies(thread_.subject, thread_.id, view->root.id, page, std::move(cursor));
        if (thread_.replies != view || view->feed != feed) {
            co_return;
        }
        feed->loading = false;
        if (r) {
            append(*feed, std::move(*r));
        } else {
            feed->error = r.error();
        }
        env_.redraw();
    }

    void Comments::settle_likes(const std::vector<Comment> &comments) {
        for (const auto &comment : comments) {
            if (!likes_.echoed(thread_.source, comment.id, comment.liked)) {
                continue;
            }
            auto update = [&](Comment &cached) {
                if (cached.id == comment.id) {
                    cached.liked = comment.liked;
                    cached.likes = comment.likes;
                }
            };
            for (auto &feed : thread_.feeds) {
                for (auto &cached : feed->items) {
                    update(cached);
                }
            }
            if (thread_.replies) {
                update(thread_.replies->root);
                for (auto &cached : thread_.replies->feed->items) {
                    update(cached);
                }
            }
        }
    }

    void Comments::toggle_like(const Comment &comment) {
        const Source source = thread_.source;
        if (!env_.signed_in(source)) {
            return env_.notify(lyren::trf("Sign in to {} to like comments", lyren::tr(platform(source).name)), "thumbs-up");
        }
        auto write = likes_.flip(source, comment.id, comment.liked, comment.likes);
        if (!write) {
            return;
        }
        env_.redraw();
        spawn([](Comments *self, Source source, Subject subject, std::string resource, std::string id, Likes::Write write) -> Task<void> {
            auto r = co_await self->env_.provider(source).like_comment(subject, std::move(resource), id, write.liked);
            if (!self->likes_.settle(source, id, write, r.has_value())) {
                co_return;
            }
            if (!r) {
                self->env_.notify(lyren::trf("Could not update comment like: {}", r.error()), "triangle-alert", true);
            }
            self->env_.redraw();
        }(this, source, thread_.subject, thread_.id, comment.id, *write));
    }

    void Comments::send() {
        auto view = thread_.replies;
        Draft &draft = view ? *view->draft : *thread_.draft;
        std::optional<Comment> target = draft.reply ? draft.reply : view ? std::optional(view->root) : std::nullopt;
        draft.sending = true;
        spawn([](Comments *self, Source source, Subject subject, std::string resource, std::shared_ptr<Draft> thread_draft, std::shared_ptr<Replies> view,
                 std::optional<Comment> target) -> Task<void> {
            Draft &draft = view ? *view->draft : *thread_draft;
            auto r = co_await self->env_.provider(source).post_comment(subject, std::move(resource), draft.text, target ? target->id : "", view ? view->root.id : "");
            draft.sending = false;
            Thread &thread = self->thread_;
            // The thread draft changes only when the resource or account changes.
            if (thread.draft != thread_draft) {
                co_return;
            }
            if (!r) {
                co_return self->env_.notify(lyren::trf("Could not send comment: {}", lyren::tr(r.error())), "triangle-alert", true);
            }
            draft.text.clear();
            draft.reply.reset();
            draft.expanded = draft.focus = false;
            self->env_.notify(std::string(lyren::tr("Comment posted")), "circle-check");
            auto current = thread.replies;
            if (current && target && current->root.id == (view ? view->root.id : target->id)) {
                current->feed = std::make_shared<Feed>();
                self->load_replies(current);
            }
            if (current == view) {
                if (!view && target) {
                    self->open_replies(std::move(*target));
                } else if (!view) {
                    thread.tab = self->env_.provider(source).capabilities.comment_sort ? 1 : 0;
                    thread.tab_since = self->env_.now();
                }
                thread.to_top = true;
            }
            self->refresh();
        }(this, thread_.source, thread_.subject, thread_.id, thread_.draft, std::move(view), std::move(target)));
    }

    void Comments::remove(Source source, Subject subject, std::string resource, Comment comment) {
        const uint64_t ticket = removals_.begin(source, comment.id);
        env_.redraw();
        spawn([](Comments *self, Source source, Subject subject, std::string resource, std::string id, uint64_t ticket) -> Task<void> {
            auto result = co_await self->env_.provider(source).delete_comment(subject, resource, id);
            if (!self->removals_.end(source, id, ticket)) {
                co_return;
            }
            if (!result) {
                co_return self->env_.notify(lyren::trf("Could not delete comment: {}", lyren::tr(result.error())), "triangle-alert", true);
            }
            self->likes_.erase(source, id);
            self->env_.notify(std::string(lyren::tr("Comment deleted")), "trash");
            Thread &thread = self->thread_;
            if (!thread.is(source, subject, resource)) {
                co_return;
            }
            auto clear_target = [&](Draft &draft) {
                if (draft.reply && draft.reply->id == id) {
                    draft.reply.reset();
                }
            };
            clear_target(*thread.draft);
            if (auto view = thread.replies) {
                if (view->root.id == id) {
                    thread.replies.reset();
                } else {
                    clear_target(*view->draft);
                    view->feed = std::make_shared<Feed>();
                    self->load_replies(view);
                }
            }
            self->refresh();
        }(this, source, subject, std::move(resource), std::move(comment.id), ticket));
    }

} // namespace lyren
