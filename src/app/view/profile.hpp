#pragma once
#include <cstdint>
#include <memory>

#include "app/view/part.hpp"

namespace lyren {

    // A listener's page: who they are and the people around them, what they post, what they play most, and the
    // playlists they keep.
    class ProfileView : public Part {
    public:
        explicit ProfileView(Shell &shell)
        : Part(shell) {}

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
        float header(ui::Ui &ui, ui::Rect r, const Listener &page, const Profile &who, bool self);
        float stats(ui::Ui &ui, ui::Rect r, const Listener &page, const Profile &who);
        // pinned: how far the tabs have come to rest at the top of the view, 0..1.
        void tabs(ui::Ui &ui, ui::Rect r, const Listener &page, const Profile &who, float pinned);
        float section(ui::Ui &ui, ui::Rect r, const Listener &page, bool self);
        float posts(ui::Ui &ui, ui::Rect r, const Listener &page, bool self);
        float chart(ui::Ui &ui, ui::Rect r, const Listener &page);
        float playlists(ui::Ui &ui, ui::Rect r, const Listener &page, bool self);
        float people(ui::Ui &ui, ui::Rect r, const Listener &page, bool fans, bool self);
        // What they listen to and whom they follow, beside the timeline when there is room. It fills what r holds.
        float rail(ui::Ui &ui, ui::Rect r, const Listener &page);
        void person(ui::Ui &ui, ui::Rect r, Source source, const Person &person);
        // Turns the page to a section, its top in sight.
        void show(ui::Ui &ui, const Listener &page, Section section);

        uint64_t visit_ = 0;                 // the visit the page was last laid out for
        float content_h_ = 0, header_h_ = 0; // as last laid out
    };

} // namespace lyren
