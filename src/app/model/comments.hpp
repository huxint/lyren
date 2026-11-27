#pragma once
#include <array>
#include <memory>
#include <optional>
#include <string>

#include "app/env.hpp"
#include "app/model/fetch.hpp"
#include "app/model/writes.hpp"

namespace lyren {

    class Library;

    // The comments on one thing at a time: a song, an album, a playlist or a post. Hot and newest comments arrive a page
    // at a time, a comment's replies open over them, and what the listener writes, likes and deletes shows at once.
    class Comments {
    public:
        using Feed = Paged<Comment>;
        struct Draft {
            std::string text;
            std::optional<Comment> reply;
            std::string insert; // text the editor takes at its caret on the next frame
            bool sending = false, focus = false, expanded = false;
        };
        struct Replies {
            Comment root;
            std::shared_ptr<Feed> feed = std::make_shared<Feed>();
            std::shared_ptr<Draft> draft = std::make_shared<Draft>();
            double opened = -10;
        };
        // What is being commented on, its two feeds and the replies open over them.
        struct Thread {
            Source source = Source::netease;
            Subject subject = Subject::song;
            std::string id, title, cover;
            bool follows = false; // it is the playing song's, and moves on with it
            int tab = 0;
            double tab_since = -10, opened = -10;
            uint64_t gen = 0;                                                                               // disowns the pages under way
            uint64_t serial = 0;                                                                            // changes with the subject: scroll positions and focus start over
            std::array<std::shared_ptr<Feed>, 2> feeds{std::make_shared<Feed>(), std::make_shared<Feed>()}; // hot, newest
            std::shared_ptr<Replies> replies;
            std::shared_ptr<Draft> draft = std::make_shared<Draft>();
            bool to_top = false; // the list on show should start over from its top

            bool is(Source s, Subject what, std::string_view which) const {
                return source == s && subject == what && id == which;
            }
        };

        Comments(Env &env, Library &library);
        Comments(const Comments &) = delete;
        Comments &operator=(const Comments &) = delete;

        Thread &thread() {
            return thread_;
        }
        const Thread &thread() const {
            return thread_;
        }
        // Points the thread at something and loads its first page. The same thing again only retries a failed start.
        void open(Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows);
        void load(int tab);
        void refresh();
        void set_tab(int tab);
        void open_replies(Comment root);
        void close_replies() {
            thread_.replies.reset();
        }
        void load_replies(std::shared_ptr<Replies> view);

        Likes::Mark like(const Comment &comment) const {
            return likes_.shown(thread_.source, comment.id, comment.liked, comment.likes);
        }
        bool removing(const Comment &comment) const {
            return removals_.contains(thread_.source, comment.id);
        }
        void toggle_like(const Comment &comment);
        // Sends the draft on show: a new comment, or a reply to the draft's target or the open thread's root.
        void send();
        void remove(Source source, Subject subject, std::string resource, Comment comment);

    private:
        Task<void> fetch(uint64_t gen, int tab, int page, std::string cursor);
        Task<void> fetch_replies(std::shared_ptr<Replies> view, int page, std::string cursor);
        void append(Feed &feed, CommentPage page);
        // Reconcile cached copies before returning a confirmed like to the service's own numbers.
        void settle_likes(const std::vector<Comment> &comments);

        Env &env_;
        Thread thread_;
        Likes likes_;
        Removals removals_;
    };

} // namespace lyren
