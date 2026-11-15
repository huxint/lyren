#pragma once
#include <vector>

#include "app/view/part.hpp"

namespace lyren {

    // Songs in rows: playing, liking, picking several to act on together, putting a playlist in order.
    class Tracks : public Part {
    public:
        explicit Tracks(Shell &shell)
        : Part(shell) {}

        // since: when the list appeared, for the staggered entrance. Returns true when the last rows are visible.
        bool list(ui::Ui &ui, ui::Rect r, ui::Id id, std::vector<Track> &tracks, double since);
        // What can be done with the i-th song, or with the picked ones when it is among them.
        void menu(ui::Ui &ui, float x, float y, std::vector<Track> &tracks, int i);

    private:
        void row(ui::Ui &ui, ui::Rect r, std::vector<Track> &tracks, int i);
    };

} // namespace lyren
