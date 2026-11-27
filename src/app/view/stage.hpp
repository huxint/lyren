#pragma once
#include <string>
#include <vector>

#include "app/state.hpp"
#include "app/view/part.hpp"

namespace lyren {

    // The now-playing stage: the cover-lit backdrop that comes up over the window, the lyrics, and the cover and title
    // that travel between the player and the stage.
    class Stage : public Part {
    public:
        Stage(Shell &shell, const SavedState &saved);

        bool open() const {
            return open_;
        }
        // Leaving the stage closes the comments it opened.
        void show(bool open);
        // How far the stage is up, 0..1.
        float raised(ui::Ui &ui);
        // The lyrics show their translation where they have one.
        bool translation() const {
            return translation_;
        }
        void toggle_translation();
        void save(SavedStateView &to) const;

        // page: the space the stage has for its cover and lyrics. Returns how far its wide layout is in, 0..1: a large
        // cover beside the lyrics, rather than a small one above them.
        float fit(ui::Ui &ui, ui::Rect page);
        // The stage over the window, k of the way up.
        void draw(ui::Ui &ui, ui::Rect window, ui::Rect page, float k, float wide);
        // The cover, title, artist and heart of the current song, which move between the player (np 0) and the stage
        // (np 1). strip: the player's.
        void art(ui::Ui &ui, ui::Rect strip, ui::Rect page, float np, float wide);
        // The player's cover in buffer pixels, the size the accent is taken from.
        float cover_px() const {
            return cover_px_;
        }

    private:
        struct Block {
            std::vector<std::string_view> lines, translation; // wrapped, pointing into the lyrics
            float y = 0, h = 0;
        };

        // centered: 0 lines start at the left, 1 they are centred.
        void lyrics(ui::Ui &ui, ui::Rect r, float centered);
        void show_audio_info(ui::Ui &ui);

        bool open_ = false, wide_ = true, translation_ = true;
        float cover_px_ = 64;
        std::vector<Block> layout_;
        float layout_w_ = 0, layout_size_ = 0;
        std::string layout_font_;
        uint64_t layout_serial_ = 0; // the lyrics the layout was made from
        bool layout_translated_ = true;
        // Each line scrolls on its own spring towards the offset it aims at. When the next line comes up, the lines below
        // it set off a little later than the ones above, so the change ripples down; jumps and the wheel move them as one.
        std::vector<ui::Spring> line_y_;
        std::vector<float> line_aim_;
        std::vector<ui::Painter::Lift> lyric_lifts_;
        float manual_ = 0;
        int focus_ = 0; // what the view centres on: 2i is line i, 2i - 1 the pause before it
        bool ripple_ = false;
        double switched_ = -10, manual_until_ = 0, last_frame_ = 0;
    };

} // namespace lyren
