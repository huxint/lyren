#pragma once
#include <string>

#include "app/view/part.hpp"

namespace lyren {

    // The comments on a song, an album, a playlist or a post: a calm subject header, one clean typographic column of
    // voices, and a docked bottom composer that expands in place.
    class CommentsView : public Part {
    public:
        explicit CommentsView(Shell &shell)
        : Part(shell) {}

        void draw(ui::Ui &ui, ui::Rect r);

    private:
        Comments::Thread &thread() const {
            return comments_.thread();
        }
        // The subject header and sort tabs, or the replies header and pinned root comment. Returns its height.
        float header(ui::Ui &ui, ui::Rect r, bool compact);
        float replies_header(ui::Ui &ui, ui::Rect r, const std::shared_ptr<Comments::Replies> &reply, bool compact);
        // Docked bottom writing tray, smoothly expanding upward within r. Returns its occupied height.
        float composer(ui::Ui &ui, ui::Rect r);
        // Lays out a comment at y, or only measures it; returns its height.
        float comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw);
        void picture(ui::Ui &ui, const Comment &c, ui::Rect r);
        void confirm_delete(ui::Ui &ui, Source source, Subject subject, std::string resource, Comment comment);
        // The lyric line sung now, when the comments are on the current song.
        const LyricLine *lyric();
    };

} // namespace lyren
