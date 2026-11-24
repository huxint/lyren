#pragma once
#include <array>
#include <string>

#include "app/view/draw.hpp"
#include "app/view/part.hpp"
#include "audio/meter.hpp"
#include "core/string_map.hpp"

namespace lyren {

    class SettingsPage : public Part {
    public:
        explicit SettingsPage(Shell &shell)
        : Part(shell) {}

        void draw(ui::Ui &ui, ui::Rect r);

    private:
        void ensure_preview_themes();
        void theme_mock(ui::Ui &ui, ui::Rect r, const ui::Theme &look);
        void desktop_mock(ui::Ui &ui, ui::Rect r, bool island);
        void wallpaper(ui::Ui &ui, ui::Rect r, float radius);
        void quality_header(ui::Ui &ui, ui::Rect r, int quality);

        int tab_ = 0;
        int tab_dir_ = 1;
        double tab_since_ = -10;
        bool rail_ = true;
        SharedSlot title_slot_;
        int lyrics_mode_ = -1;
        double lyrics_mode_since_ = -10;
        std::array<float, 5> heights_{};
        StringMap<float> sliders_; // values being dragged, written on release

        // Cached Dark / Light preview themes so Appearance never reparses config colours every frame.
        ui::Theme dark_preview_{}, light_preview_{};
        bool previews_ready_ = false;
        double preview_fs_ = 0, preview_radius_ = 0;
        std::string preview_accent_;
        std::array<std::optional<std::string>, 7> preview_colors_;
        audio::Follower meter_{};
        double meter_time_ = 0;
    };

} // namespace lyren
