#pragma once
#include <cstdint>
#include <inplace_vector>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "app/view/draw.hpp"
#include "app/view/part.hpp"

namespace lyren {

    // A listener's page: hero header and interactive metrics, personal music hub, posts, listening ranking,
    // playlists, collection, and social circle.
    class ProfileView : public Part {
    public:
        explicit ProfileView(Shell &shell);

        void draw(ui::Ui &ui, ui::Rect r);
        // A follow button for someone: as wide as its widest state, or its icon alone in a rect too narrow for words.
        // prominent: the page's main action rather than one of a list of them.
        void follow_button(ui::Ui &ui, ui::Id id, ui::Rect r, Source source, const Person &person, bool prominent = false);
        static float follow_width(ui::Ui &ui);

    private:
        using Section = Social::Section;
        using Listener = std::shared_ptr<Social::Page>;

        // The parts of the page, each laid out down from r's top inside the page's scrolling view; they return their
        // height.
        struct RailPlan {
            size_t repeat_rows = 0;
            float repeat_h = 0;
            bool show_follows = false;
            bool follows_list = false;
            size_t follow_rows = 0;
            float follows_body_h = 0;
            float total_h = 0;
        };

        // A count in the head, and where a click on it leads.
        struct Stat {
            std::string_view label;
            int value; // negative while unknown
            std::optional<Section> opens;
            int tab = -1; // the sub-tab of the section it opens
            std::optional<Page> opens_page;
            float number_w = 0, w = 0; // as measured: the number alone, and the whole with its padding
        };
        using Stats = std::inplace_vector<Stat, 4>;

        float header(ui::Ui &ui, ui::Rect r, const Listener &page, const Profile &who, bool self);
        // The counts worth showing for this listener, measured; none when there would be only one.
        Stats stat_list(ui::Ui &ui, const Listener &page, const Profile &who, bool self);
        // Lays the counts out from r's left, breaking into rows within its width.
        float stats(ui::Ui &ui, ui::Rect r, std::span<const Stat> all, const Listener &page);
        // pinned: how far the tabs have come to rest at the top of the view, 0..1.
        void tabs(ui::Ui &ui, ui::Rect r, const Listener &page, const Profile &who, float pinned);
        float section(ui::Ui &ui, ui::Rect r, float rail_budget_h, const Listener &page, bool self);
        float overview(ui::Ui &ui, ui::Rect r, float rail_budget_h, const Listener &page);
        float posts(ui::Ui &ui, ui::Rect r, float rail_budget_h, const Listener &page, bool self);
        float chart(ui::Ui &ui, ui::Rect r, const Listener &page);
        float playlists(ui::Ui &ui, ui::Rect r, const Listener &page, bool self);
        float collection(ui::Ui &ui, ui::Rect r, const Listener &page);
        float people(ui::Ui &ui, ui::Rect r, const Listener &page, bool self);
        // Plans and draws what they listen to and whom they follow, beside the main column when there is room.
        RailPlan plan_rail(float budget_h, const Listener &page);
        float rail(ui::Ui &ui, ui::Rect r, const Listener &page, const RailPlan &plan, bool include_repeat, float repeat_h_override = -1);
        // Framed "On repeat" track card, shared between the studio rail and the narrow overview.
        float repeat_height(const Listener &page, size_t max_rows) const;
        float repeat_card(ui::Ui &ui, ui::Rect r, const Listener &page, size_t max_rows);
        void person(ui::Ui &ui, ui::Rect r, Source source, const Person &person);
        void ensure_own_playlists(Source source);
        // Turns the page to a section, its top in sight.
        void show(ui::Ui &ui, const Listener &page, Section section);

        uint64_t visit_ = 0;                 // the visit the page was last laid out for
        ui::Id scroll_ = 0;                  // scroll container id for the current visit, outside any nested id scope
        int section_dir_ = 1;                // -1 or +1 for directional tab slide
        bool wide_ = true;                   // hysteresis state for two-column studio rail
        bool compact_ = false;               // hysteresis state for compact header
        int rail_repeat_rows_ = 0;           // hysteresis state for studio rail "On repeat" row count
        int rail_follow_rows_ = -1;          // hysteresis state for studio rail "Following" (-1 unset, 0 avatars, 1..4 rows)
        SharedSlot repeat_slot_;             // shared-element flight for "On repeat" (rail <-> inline)
        float content_h_ = 0, header_h_ = 0; // as last laid out
        Source playlists_source_ = Source::netease;
        bool playlists_dirty_ = true;
        std::vector<Card> own_made_, own_saved_, own_preview_;
    };

} // namespace lyren
