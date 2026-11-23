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

        int tab_ = 0;
        int tab_dir_ = 1;
        double tab_since_ = -10;
        bool rail_ = true;
        int lyrics_mode_ = -1;
        double lyrics_mode_since_ = -10;
        std::array<float, 4> heights_{};
        StringMap<float> sliders_; // values being dragged, written on release
    };

} // namespace lyren
