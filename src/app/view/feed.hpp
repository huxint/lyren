#pragma once
#include <memory>
#include <optional>
#include <string>

#include "app/view/part.hpp"

namespace lyren {

    // Posts: a listener's timeline or the feed of the people the account follows, and writing, liking, passing on and
    // deleting them. A post is who and when, the words with their topics lit, and what it carries: a song to play on
    // the spot, a playlist, pictures, or another post passed on.
    class PostFeed : public Part {
    public:
        // The widest a column of posts gets.
        static constexpr float column = 680;

        explicit PostFeed(Shell &shell)
        : Part(shell) {}

        // Lays the posts out down from r's top inside the scrolling view around it, loading more as the end comes into
        // sight; returns the height they take. author: whose timeline, empty for the feed. byline: posts name their
        // authors, which one listener's timeline does not need.
        float draw(ui::Ui &ui, ui::Rect r, const std::shared_ptr<Social::Posts> &list, Source source, const std::string &author, bool byline);
        // Where a post starts on the account's own page: a line to click, and the song playing to share. Returns its
        // height.
        float prompt(ui::Ui &ui, ui::Rect r, Source source);
        // Opens the composer for a post that shares the song or passes the post on.
        void compose(ui::Ui &ui, Source source, std::optional<Track> track = {}, std::optional<Post> forward = {});

    private:
        // A post at y, or only its height when it is not drawn.
        float post(ui::Ui &ui, Source source, const Post &post, float x, float y, float w, bool draw, bool byline);
        // The song, playlist, pictures or forwarded post a post carries; returns the height used.
        float attachments(ui::Ui &ui, Source source, const Post &post, float x, float y, float w, bool draw);
        void menu(ui::Ui &ui, ui::Rect anchor, Source source, const Post &post, bool byline);
        void composer(ui::Ui &ui, ui::Rect r);
        float composer_height() const;
    };

} // namespace lyren
