#pragma once
#include <span>
#include <string>
#include <string_view>

#include "app/env.hpp"
#include "app/model/accounts.hpp"
#include "app/model/browser.hpp"
#include "app/model/comments.hpp"
#include "app/model/library.hpp"
#include "app/model/playback.hpp"
#include "app/model/social.hpp"
#include "ui/ui.hpp"

namespace lyren {

    class Shell;

    // What stands in for a page's content while it loads: the shape of what is coming, or a spinner.
    enum class Placeholder : uint8_t {
        spinner,
        rows,
        cards,
        faces
    };

    // A part of the window: it draws from the models and reaches the other parts through the shell.
    class Part {
    public:
        Part(const Part &) = delete;
        Part &operator=(const Part &) = delete;

    protected:
        explicit Part(Shell &shell);

        Provider &provider() const {
            return env_.provider(browser_.active());
        }
        template <class T>
        Provider &provider(const T &of) const {
            return env_.provider(of);
        }
        Library::Shelf &shelf() const {
            return library_.shelf(browser_.active());
        }
        bool signed_in() const {
            return env_.signed_in(browser_.active());
        }
        // The id of something the page on show scrolls: each visit to a page starts at its top, and going back finds
        // the page where it was left.
        ui::Id scroll_id(ui::Ui &ui, std::string_view name) const {
            return ui::sub_id(ui.id(name), browser_.route().serial);
        }

        // A cover, an avatar or a playlist's picture, fading in over its placeholder once it has loaded.
        void cover(ui::Ui &ui, ui::Rect r, const std::string &url, float radius, std::string_view placeholder = "music");
        // The dominant hue of a cover, 0..1; -1 for a colourless one, -2 while it is not known.
        float cover_hue(const std::string &url, float size);
        // Loading, error and signed-out states of a page; true when one was drawn.
        bool page_status(ui::Ui &ui, ui::Rect r, bool loading, const std::string &error, bool needs_login, Placeholder look = Placeholder::spinner);
        // An empty state: an icon in a disc, a line and a quieter one under it.
        void empty(ui::Ui &ui, ui::Rect r, std::string_view icon, std::string_view title, std::string_view detail = {});
        // The size of a heart that pops when its song has just been liked.
        float heart_size(ui::Ui &ui, const Track &track, float size) const;
        // The heart of a song, where its platform keeps likes. True when it was drawn.
        bool heart(ui::Ui &ui, ui::Id id, ui::Rect r, const Track &track, float size = 18);
        // Draws cards in responsive columns from r's top-left; returns the clicked index or -1.
        int card_grid(ui::Ui &ui, ui::Rect r, ui::Id id, std::span<const Card> cards, bool round, double since);
        bool card(ui::Ui &ui, ui::Id id, ui::Rect r, const Card &c, bool round);

        Shell &shell_;
        Env &env_;
        Playback &playback_;
        Library &library_;
        Social &social_;
        Comments &comments_;
        Browser &browser_;
        Accounts &accounts_;
    };

} // namespace lyren
