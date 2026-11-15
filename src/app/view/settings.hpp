#pragma once
#include <array>

#include "app/view/part.hpp"
#include "core/string_map.hpp"

namespace lyren {

    class SettingsPage : public Part {
    public:
        explicit SettingsPage(Shell &shell)
        : Part(shell) {}

        void draw(ui::Ui &ui, ui::Rect r);

    private:
        void theme_mock(ui::Ui &ui, ui::Rect r, const ui::Theme &look);
        void desktop_mock(ui::Ui &ui, ui::Rect r, bool island);
        void wallpaper(ui::Ui &ui, ui::Rect r, float radius);

        // Where each section starts, the tab it is gliding to and the one on show, and the layout: the cards' heights as
        // last drawn, the columns, where the headings, cards and title were drawn, and the move between one column and
        // two (from where, since when).
        std::array<float, 4> settings_tops_{};
        int settings_jump_ = 0, settings_section_ = 0, settings_columns_ = 0;
        double settings_jump_until_ = 0, settings_drawn_ = -10, settings_moved_ = -10;
        std::array<float, 9> settings_heights_{196, 240, 58, 132, 234, 386, 250, 190, 132};
        std::array<ui::Rect, 14> settings_shown_{}, settings_from_{};
        StringMap<float> settings_sliders_; // values being dragged, written on release
    };

} // namespace lyren
