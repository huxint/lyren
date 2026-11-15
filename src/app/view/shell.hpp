#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "app/env.hpp"
#include "app/model/accounts.hpp"
#include "app/model/browser.hpp"
#include "app/model/comments.hpp"
#include "app/model/library.hpp"
#include "app/model/playback.hpp"
#include "app/model/social.hpp"
#include "app/state.hpp"
#include "app/view/account_dialog.hpp"
#include "app/view/chrome.hpp"
#include "app/view/comments_view.hpp"
#include "app/view/desktop.hpp"
#include "app/view/dialogs.hpp"
#include "app/view/feed.hpp"
#include "app/view/pages.hpp"
#include "app/view/player_bar.hpp"
#include "app/view/profile.hpp"
#include "app/view/settings.hpp"
#include "app/view/side_panel.hpp"
#include "app/view/stage.hpp"
#include "app/view/tracks.hpp"
#include "audio/player.hpp"
#include "sys/bus.hpp"
#include "ui/ui.hpp"

namespace lyren {

    // What the window content needs from the app.
    struct ShellDeps {
        Loop &loop;
        net::Http &http;
        audio::Player &player;
        Providers providers;
        Images &images;
        const Config &config;
        // Changes a setting in the config file; value is TOML. config reflects it once this returns.
        std::move_only_function<void(std::string_view section, std::string_view key, std::string_view value)> set_setting;
        std::move_only_function<void()> desktop_changed;
        std::move_only_function<void()> open_config;                 // in the user's editor
        std::move_only_function<void()> redraw;                      // async results arrived
        std::move_only_function<double()> now;                       // the UI clock
        bool silent;                                                 // scripted runs: the stream volume stays 0
        Bus &bus;                                                    // for the media controls; get() is null without a session bus
        std::move_only_function<void(std::string_view token)> raise; // bring the window forward
    };

    // The window's content. It owns what the app knows (the models) and what it shows (the parts), lays the parts out,
    // and is where one part reaches another.
    class Shell {
    public:
        Shell(ShellDeps deps, SavedState saved);
        Shell(const Shell &) = delete;
        Shell &operator=(const Shell &) = delete;

        // area: below the title bar. The now-playing stage covers the whole window in the stage theme.
        void draw(ui::Ui &ui, ui::Rect area, const ui::Theme &stage_theme);
        // Accent taken from the current cover, cross-faded after a change.
        ui::Color accent(double now, bool dark, bool &animating);
        // window: the size to remember, unless the window is maximized.
        void save(std::optional<std::pair<int, int>> window = {});
        void config_changed();

        // What the parts ask of the window and of each other.
        void changed(); // something worth saving changed
        // Back closes the listening view first, then returns through the pages.
        void back();
        bool compact() const {
            return compact_;
        }
        // This frame's stage theme.
        const ui::Theme &stage_theme() const {
            return *stage_theme_;
        }
        // Opens the comments on something in the panel. follows: the current song's, which the stage keeps up with.
        void open_comments(Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows = false);
        void set_setting(std::string_view section, std::string_view key, std::string_view value) {
            set_setting_(section, key, value);
        }
        void open_config() {
            open_config_();
        }
        void desktop_changed() {
            desktop_changed_();
        }

        Env env;
        Playback playback;
        Library library;
        Social social;
        Comments comments;
        Browser browser;
        Accounts accounts;

        Chrome chrome;
        Pages pages;
        Tracks tracks;
        Dialogs dialogs;
        AccountDialog account;
        SettingsPage settings;
        ProfileView profile;
        PostFeed feed;
        SidePanel panel;
        CommentsView comments_view;
        PlayerBar bar;
        Stage stage;
        DesktopLyrics desktop;

    private:
        void page(ui::Ui &ui, ui::Rect area);
        void keys(ui::Ui &ui);

        std::move_only_function<void(std::string_view, std::string_view, std::string_view)> set_setting_;
        std::move_only_function<void()> open_config_, desktop_changed_;
        const ui::Theme *stage_theme_ = nullptr;
        bool compact_ = false;
        uint64_t visit_ = 0; // the page visit on show last frame
        Loop::TimerId save_timer_ = 0;
        std::optional<std::pair<int, int>> window_;

        // The accent: the hue it follows, and where a change set off from, on light and on dark backgrounds.
        float accent_hue_ = -2;
        std::array<ui::Color, 2> accent_from_{};
        double accent_time_ = -10;
    };

} // namespace lyren
