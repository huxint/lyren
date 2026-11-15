#pragma once
#include "app/view/part.hpp"

namespace lyren {

    // The panel beside the page and the stage: the play queue, or the comments on something.
    class SidePanel : public Part {
    public:
        enum class Kind : uint8_t {
            none,
            queue,
            comments
        };

        explicit SidePanel(Shell &shell)
        : Part(shell) {}

        Kind kind() const {
            return panel_;
        }
        static float width(Kind kind);
        // The kind on show again, or none, closes it.
        void show(Kind kind);
        void toggle(Kind kind) {
            show(panel_ == kind ? Kind::none : kind);
        }
        // slot: the space beside the page, which grows as the panel slides in; stage: how far the stage is up.
        void draw(ui::Ui &ui, ui::Rect slot, float stage);

    private:
        void queue(ui::Ui &ui, ui::Rect r);

        Kind panel_ = Kind::none, panel_last_ = Kind::queue, panel_from_ = Kind::none; // last: the one drawn while it closes
        double panel_time_ = -10;
    };

} // namespace lyren
