#pragma once
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "app/desktop_snap.hpp"
#include "app/state.hpp"
#include "app/view/part.hpp"

namespace lyren {

    // Lyrics on the desktop, outside the window: floating lines, or an island hanging from the top of the screen. The
    // overlay reserves no workspace and never takes keyboard focus.
    class DesktopLyrics : public Part {
    public:
        DesktopLyrics(Shell &shell, const SavedState &saved);

        bool enabled() const {
            return enabled_;
        }
        bool is_island() const {
            return is_island_;
        }
        void set_enabled(bool enabled);
        void set_island(bool island);
        void save(SavedStateView &to) const;
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

        struct FloatingState {
            float x = 0, bottom = 48; // offset from centre, height above bottom
            DesktopSnap snap;
            bool locked = false, dragging = false;
            float grab_x = 0, grab_y = 0;
            FloatingLines lines;
        };

        struct IslandState {
            float x = 0, y = 10; // offset from centre and top edge
            DesktopSnap snap;
            bool moving = false, dragged = false;
            float grab_x = 0, grab_y = 0, from_x = 0, from_y = 0;
            bool seeking = false;
            float seek_v = 0; // normalized song position, [0, 1]
            LineChange line;
        };

        // Notes the line on show; returns how far the change to it has gone, 0..1.
        float show_line(ui::Ui &ui, LineChange &change, std::string_view text, std::string_view sub, bool timed);
        // Frames for desktop lyrics: continuous while a word-timed line is sung, otherwise one when the next line starts.
        void lyric_frames(ui::Ui &ui, int line, bool timed);
        // A line of floating lyrics, outlined; a word-timed live line lights up from color to rest as it is sung.
        void floating_text(ui::Ui &ui, float center, float baseline, std::string_view text, const LyricLine *live, float size, float max_width, ui::Color color, ui::Color rest,
                           float focus, uint8_t features = 0, float outline_scale = 1.f);

        bool enabled_ = false, is_island_ = false;
        FloatingState floating_;
        IslandState island_;
        std::vector<ui::Painter::Lift> lyric_lifts_; // the wave under a word-timed line
    };

} // namespace lyren
