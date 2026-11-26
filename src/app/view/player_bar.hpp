#pragma once
#include <vector>

#include "app/view/part.hpp"
#include "audio/meter.hpp"
#include "audio/spool.hpp"

namespace lyren {

    // The player: a console resting at the bottom of the window. What plays on its left, the transport over the song's
    // string in the middle, and the lyrics on the desktop, the queue and the volume on its right.
    class PlayerBar : public Part {
    public:
        // The strip the window gives it, and the margin the console keeps inside the strip.
        static constexpr float height = 88, margin = 8;
        struct Layout {
            ui::Rect card;    // the console
            ui::Rect cover;   // the current song's cover
            ui::Rect caption; // its title and artist, the heart at their right end
            ui::Rect centre;  // the transport and the string
            ui::Rect tools;
        };
        static Layout layout(ui::Rect strip);

        explicit PlayerBar(Shell &shell)
        : Part(shell) {}

        // stage: how far the now-playing stage is up; on it the console also holds the comments and the translation of
        // the song on show.
        void draw(ui::Ui &ui, ui::Rect strip, float stage);

    private:
        void transport(ui::Ui &ui, ui::Rect r);
        // The song as a string: played up to where it stands, plucked where it is let go. above: where the note about
        // the time under the pointer may stand, clear of the console.
        void string(ui::Ui &ui, ui::Rect r, float above, float stage);
        void tools(ui::Ui &ui, ui::Rect r, float stage);

        bool dragging_ = false, dragged_ = false;
        float press_x_ = 0;
        float drag_v_ = 0; // normalized song position, [0, 1]
        audio::Follower meter_;
        std::vector<audio::Ranges::Span> buffered_;
        double meter_time_ = 0, pluck_time_ = -10;
        float pluck_at_ = 0;
    };

} // namespace lyren
