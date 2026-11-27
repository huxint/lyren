#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "app/view/part.hpp"

namespace lyren {

    // Songs in rows: playing, liking, picking several to act on together, filtering, sorting, and putting a playlist in order.
    class Tracks : public Part {
    public:
        explicit Tracks(Shell &shell)
        : Part(shell) {}

        // Clears per-frame active-list tracking before the page draws.
        void begin_frame() {
            active_tracks_ = nullptr;
        }
        // since: when the list appeared, for the staggered entrance. Returns true when the last rows are visible.
        // show_source: show a quiet platform badge on each row (e.g. cross-platform aggregated search).
        bool list(ui::Ui &ui, ui::Rect r, ui::Id id, const std::vector<Track> &tracks, double since, bool show_source = false);
        // What can be done with the i-th song, or with the picked ones when it is among them.
        void menu(ui::Ui &ui, float x, float y, const std::vector<Track> &tracks, int i, bool in_queue = false);

        // Keyboard navigation on the track list drawn this frame.
        bool has_list() const {
            return active_tracks_ != nullptr && !active_tracks_->empty();
        }
        bool has_selected() const;
        void move_selection(ui::Ui &ui, int delta);
        bool play_selected();
        void select_all();

    private:
        enum class SortKey : uint8_t {
            none,
            title,
            album,
            duration
        };

        bool is_picked_list(const std::vector<Track> &tracks) const;
        const std::vector<Track> &displayed_tracks(const std::vector<Track> &tracks);
        void row(ui::Ui &ui, ui::Rect r, const std::vector<Track> &tracks, int i, std::optional<Track> &toggled_like, bool show_source);

        ui::Id view_id_ = 0;
        const std::vector<Track> *view_source_ = nullptr;
        uint64_t view_route_serial_ = 0;
        double view_tab_since_ = -10;
        SortKey sort_key_ = SortKey::none;
        bool sort_desc_ = false;
        bool filter_open_ = false, filter_focus_ = false;
        std::string filter_;

        std::vector<Track> view_tracks_;
        std::string cached_filter_;
        SortKey cached_sort_ = SortKey::none;
        bool cached_desc_ = false;
        uint64_t cached_fingerprint_ = 0;
        size_t cached_count_ = 0;
        const Track *cached_data_ = nullptr;

        const std::vector<Track> *active_tracks_ = nullptr;
        ui::Id active_scroll_id_ = 0;
        float active_view_h_ = 0;
    };

} // namespace lyren
