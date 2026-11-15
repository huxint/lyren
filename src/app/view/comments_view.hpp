#pragma once
#include <string>
#include <utility>

#include "app/view/part.hpp"

namespace lyren {

    // The comments on a song, an album, a playlist or a post: the subject up top, the voices in one column, and a
    // button floating over them that opens into the card to write in.
    class CommentsView : public Part {
    public:
        explicit CommentsView(Shell &shell)
        : Part(shell) {}

        void draw(ui::Ui &ui, ui::Rect r);
        // A picture shown large over the window; aspect is width over height.
        void show_picture(ui::Ui &ui, std::string url, float aspect);

    private:
        Comments::Thread &thread() const {
            return comments_.thread();
        }
        float composer(ui::Ui &ui, ui::Rect r);
        // The header over the subject's cover: the subject, its record and the tabs. Returns its height.
        float sleeve(ui::Ui &ui, ui::Rect r, bool compact);
        // Lays out a comment at y, or only measures it; returns its height. featured: the top hot comment, set as a quote.
        float comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw, bool featured = false);
        void picture(ui::Ui &ui, const Comment &c, ui::Rect r);
        void confirm_delete(ui::Ui &ui, Comment comment);
        // The comments are on the song that plays now.
        bool playing() const;
        // The lyric line sung now, when the comments are on the current song.
        const LyricLine *lyric();

        std::pair<size_t, size_t> composer_layer_; // the composer's primitives, raised over the list
    };

} // namespace lyren
