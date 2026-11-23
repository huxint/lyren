#pragma once
#include <string>
#include <vector>

#include "app/view/part.hpp"

namespace lyren {

    // The dialogs that edit playlists: naming a new one or an old one, deleting one, picking the one to add songs to.
    class Dialogs : public Part {
    public:
        explicit Dialogs(Shell &shell)
        : Part(shell) {}

        // A new playlist when card is empty, otherwise a rename. tracks go into the new playlist.
        void edit_playlist(ui::Ui &ui, Card card = {}, std::vector<Track> tracks = {});
        void confirm_delete(ui::Ui &ui, Card card);
        void pick_playlist(ui::Ui &ui, std::vector<Track> tracks);
        // "Copy link" for something that has one.
        void link_menu(ui::Ui &ui, float x, float y, const Card &card);
        // A picture shown large over the window; aspect is width over height.
        void show_picture(ui::Ui &ui, std::string url, float aspect);

    private:
        void editor(ui::Ui &ui, ui::Rect r, Card card);
        void picker(ui::Ui &ui, ui::Rect r, const std::vector<Track> &tracks);

        std::string edit_name_;
        bool edit_hidden_ = false, edit_focus_ = false;
        Source edit_source_ = Source::netease;
        std::vector<Track> edit_tracks_;
    };

} // namespace lyren
