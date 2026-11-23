#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/view/part.hpp"

namespace lyren {

    // The pages of the platform on show: what it recommends, its charts, playlists and releases, the radio, the
    // account's collection, search results, and the songs of a playlist, an album or an artist.
    class Pages : public Part {
    public:
        explicit Pages(Shell &shell)
        : Part(shell) {}

        // The page the browser is on.
        void draw(ui::Ui &ui, ui::Rect r);
        // The hue a page washes its top with, 0..1: its cover's; negative for the accent.
        float hue(ui::Ui &ui);

        // A page's tabs: picking one starts the slide of its content, which tab_enter applies to content and returns
        // the fade of.
        bool page_tabs(ui::Ui &ui, ui::Id id, ui::Rect r, std::span<const std::string_view> labels, int &selected, std::span<const bool> enabled = {});
        float tab_enter(ui::Ui &ui, ui::Rect &content) const;
        // "5/17", for today's mix.
        std::string date_label() const;

    private:
        void recommend_page(ui::Ui &ui, ui::Rect r);
        void toplist_page(ui::Ui &ui, ui::Rect r);
        void square_page(ui::Ui &ui, ui::Rect r);
        void fm_page(ui::Ui &ui, ui::Rect r);
        void releases_page(ui::Ui &ui, ui::Rect r);
        void collection_page(ui::Ui &ui, ui::Rect r);
        void dislikes_page(ui::Ui &ui, ui::Rect r);
        void artist_page(ui::Ui &ui, ui::Rect r);
        void search_page(ui::Ui &ui, ui::Rect r);
        void list_page(ui::Ui &ui, ui::Rect r, const std::vector<Track> &tracks, bool loading, const std::string &error, double since);
        // card_grid in its own scrolling view; near_end tells when the last rows are close.
        int grid_view(ui::Ui &ui, ui::Rect r, ui::Id id, const std::vector<Card> &cards, bool round, double since, bool more, bool &near_end);
        bool hero(ui::Ui &ui, ui::Id id, ui::Rect r, std::string_view big, std::string_view badge, std::string_view title, std::string_view subtitle, float hue,
                  std::string_view icon = {}, std::string_view action_icon = "play-fill");
        // "Play all" and "Shuffle" from x; returns where the next button goes.
        float play_buttons(ui::Ui &ui, float x, float y, const std::vector<Track> &tracks, float height = ui::control::large, bool compact = false);
        // The buttons after "play all" and "shuffle" on album and playlist pages, from x.
        void list_actions(ui::Ui &ui, float x, float y, float height = ui::control::large, bool compact = false);
        void categories_modal(ui::Ui &ui, ui::Rect r);
        static float special_hue(Page page);
        ui::Id search_scroll_id(ui::Ui &ui, int tab) const;
    };

} // namespace lyren
