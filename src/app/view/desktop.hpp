#pragma once
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "app/desktop_snap.hpp"
#include "app/view/part.hpp"

namespace lyren {

    struct SavedState;

    // Lyrics on the desktop, outside the window: floating lines, or an island hanging from the top of the screen. The
    // overlay reserves no workspace and never takes keyboard focus.
    class DesktopLyrics : public Part {
    public:
        DesktopLyrics(Shell &shell, const SavedState &saved);

        bool enabled() const {
            return dl_enabled_;
        }
        bool is_island() const {
            return dl_island_;
        }
        void set_enabled(bool enabled);
        void set_island(bool island);
        void save(SavedState &to) const;
        // The overlay's content on a surface w by h; returns where it takes input.
        ui::Rect draw(ui::Ui &ui, float w, float h);
        // The island hanging from top at center; floating lyrics across a surface. Both return where they take input.
        // movable: lyrics on the desktop, which a drag repositions, as opposed to the settings page's previews.
        ui::Rect island(ui::Ui &ui, float center, float top, bool movable);
        ui::Rect floating(ui::Ui &ui, ui::Rect area, bool movable);

    private:
        // The line on show and the one it replaced, for the change between them.
        struct ShownLine {
            std::string text, sub;
            bool timed = false;
        };
        struct LineChange {
            ShownLine shown, prev;
            double since = -10, prev_since = -10;
        };
        struct FloatingLines {
            struct Pose {
                float y = 0, alpha = 0, scale = 1, focus = 0;
                Pose mix(Pose to, float k) const {
                    return {std::lerp(y, to.y, k), std::lerp(alpha, to.alpha, k), std::lerp(scale, to.scale, k), std::lerp(focus, to.focus, k)};
                }
            };
            std::array<ShownLine, 2> shown, prev;
            std::array<Pose, 2> pose, from; // row-relative positions survive promotion and interrupted transitions
            int line = -2;
            bool advance = false;
            double since = -10;
            float duration = 620;
        };

        // Notes the line on show; returns how far the change to it has gone, 0..1.
        float show_line(ui::Ui &ui, LineChange &change, std::string_view text, std::string_view sub, bool timed);
        // Frames for desktop lyrics: continuous while a word-timed line is sung, otherwise one when the next line starts.
        void lyric_frames(ui::Ui &ui, int line, bool timed);
        // A line of floating lyrics, outlined; a word-timed live line lights up from color to rest as it is sung.
        void floating_text(ui::Ui &ui, float center, float baseline, std::string_view text, const LyricLine *live, float size, float max_width, ui::Color color, ui::Color rest,
                           float focus);

        // Floating lyrics: offset from the screen's centre, height above the bottom, click-through.
        float dl_x_ = 0, dl_bottom_ = 48;
        // The island: its offset from the centre and from the top edge, and where a drag on it started.
        float island_x_ = 0, island_y_ = 10;
        DesktopSnap dl_snap_, island_snap_;
        bool island_moving_ = false, island_dragged_ = false;
        float island_grab_x_ = 0, island_grab_y_ = 0, island_from_x_ = 0, island_from_y_ = 0;
        bool dl_locked_ = false, dl_dragging_ = false;
        bool dl_enabled_ = false, dl_island_ = false;
        float dl_grab_x_ = 0, dl_grab_y_ = 0;
        LineChange island_line_;
        FloatingLines floating_lines_;
        std::vector<ui::Painter::Lift> lyric_lifts_; // the wave under a word-timed line
        bool island_dragging_ = false;
        float island_drag_v_ = 0; // normalized song position, [0, 1]
    };

} // namespace lyren
