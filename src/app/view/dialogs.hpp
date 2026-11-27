#pragma once
#include <memory>
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
        void pick_playlist(ui::Ui &ui, std::vector<Track> tracks, uint64_t selection_tag = 0);
        // "Copy link" for something that has one.
        void link_menu(ui::Ui &ui, float x, float y, const Card &card);
        // A picture shown large over the window; aspect is width over height.
        void show_picture(ui::Ui &ui, std::string url, float aspect);
        // Right-click menu to copy or save an image.
        void picture_menu(ui::Ui &ui, float x, float y, std::string url);
        void copy_image(std::string url);
        void save_image(std::string url);
        // Searches other registered platforms for matching tracks and probes playback availability.
        void find_alternatives(ui::Ui &ui, Track track);

    private:
        struct Alternatives;
        void editor(ui::Ui &ui, ui::Rect r, const Card &card, std::vector<Track> &tracks);
        void picker(ui::Ui &ui, ui::Rect r, Source source, std::string_view sub, std::vector<Track> &tracks, uint64_t selection_tag);
        void alternatives_modal(ui::Ui &ui, ui::Rect r, const std::shared_ptr<Alternatives> &state);
        Task<void> probe_alternative(std::weak_ptr<Alternatives> state, size_t index, Track target, SourceKey source, Quality quality);

        std::shared_ptr<int> life_ = std::make_shared<int>(0);
        std::string edit_name_;
        bool edit_hidden_ = false, edit_focus_ = false;
        Source edit_source_ = Source::netease;
    };

} // namespace lyren
