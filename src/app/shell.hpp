#pragma once
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "app/images.hpp"
#include "app/lyrics.hpp"
#include "app/provider.hpp"
#include "app/state.hpp"
#include "audio/player.hpp"
#include "sys/mpris.hpp"
#include "sys/creds.hpp"
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

    // Window content: search and account in the top bar, sidebar, pages, queue, the player bar and the now-playing view.
    class Shell {
    public:
        static constexpr float player_h = 84;

        Shell(ShellDeps deps, SavedState saved);
        // bar is the top strip beside the window controls; the search box centres on center_x.
        void top_bar(ui::Ui &ui, ui::Rect bar, float center_x);
        // area: below the title bar. The now-playing stage covers the whole window in the stage theme.
        void draw(ui::Ui &ui, ui::Rect area, const ui::Theme &stage);
        // How far the now-playing stage is up, 0..1.
        float stage(ui::Ui &ui);
        // Accent taken from the current cover, cross-faded after a change.
        ui::Color accent(double now, bool dark, bool &animating);
        // window: the size to remember, unless the window is maximized.
        void save(std::optional<std::pair<int, int>> window = {});
        void config_changed();

        // The overlay reserves no workspace and never takes keyboard focus.
        float desktop_height(float font_size) const;
        bool desktop_enabled() const {
            return dl_enabled_;
        }
        bool desktop_island() const {
            return dl_island_;
        }
        int desktop_bottom() const {
            return dl_island_ ? 10 : static_cast<int>(std::lround(dl_bottom_));
        }
        void set_desktop_enabled(bool enabled);
        ui::Rect desktop_lyrics(ui::Ui &ui, float w, float h);

    private:
        enum class Page : uint8_t {
            recommend,
            daily,
            toplist,
            square,
            fm,
            settings,
            liked,
            recent,
            cloud,
            collection,
            playlist,
            album,
            artist,
            similar,
            search
        };
        // Tracks behind a page, loaded when it is entered.
        struct List {
            std::vector<Track> tracks;
            bool loading = false;
            std::string error;
            double since = -10; // when the rows arrived, for the entrance
            AlbumInfo album;    // album pages: the details; the tracks are above
        };
        // A listing that loads a page at a time.
        struct Results {
            std::vector<Track> tracks;
            std::vector<Card> cards;
            int total = 0;
            bool loading = false, loaded = false;
            std::string error;
            double since = -10;
            size_t size() const {
                return tracks.size() + cards.size();
            }
            bool more() const {
                return !loaded || static_cast<int>(size()) < total;
            }
        };
        struct SearchResults : Results {
            std::array<Batch, platforms.size()> sources;
            int pending = 0;
            std::array<bool, platforms.size()> done{};
            std::array<std::string, platforms.size()> errors;
            bool more() const {
                return std::ranges::find(done, false) != done.end();
            }
        };
        // Each platform keeps its own account, cached pages and pagination state.
        struct Library {
            std::optional<Account> account;
            uint64_t gen = 0;
            std::vector<Card> playlists, daily_lists;
            double daily_since = -10;
            Card liked;
            std::unordered_set<std::string> liked_ids;
            Results recommended, toplists;
            std::vector<Track> recent, fm;
            int fm_pos = 0, recent_tab = 0;
            bool fm_loading = false;
            std::string fm_error;
            Categories categories;
            bool categories_loading = false;
            Category category;
            Results square;
            uint64_t square_gen = 0;
            double square_time = -10;
            std::array<Results, 2> collection;
            int collection_tab = 0;
            uint64_t collection_gen = 0;
        };
        // An artist's page: the head, songs and albums a page at a time, the introduction (intro holds only its status).
        struct ArtistPage {
            std::optional<ArtistInfo> info;
            std::string error;
            int tab = 0;
            Results songs, albums, intro;
            std::vector<std::pair<std::string, std::string>> sections;
        };
        struct Route {
            Page page;
            Card card; // what a playlist-like page shows
            std::shared_ptr<List> list;
            std::shared_ptr<ArtistPage> artist;
        };
        // The comments dialog, hot and newest comments a page at a time.
        struct Feed {
            std::vector<Comment> comments;
            std::vector<float> heights; // laid out for heights_w
            float heights_w = 0, heights_size = 0;
            int total = 0, page = 0;
            bool more = true, loading = false;
            std::string cursor, error;
            double since = -10;
        };
        enum class Mode : uint8_t {
            list,
            one,
            shuffle
        };
        // What the panel beside the page shows.
        enum class Panel : uint8_t {
            none,
            queue,
            comments
        };

    public:
        static std::vector<std::string_view> wrap(ui::Painter &p, std::string_view text, float size, float width);
        // wrap() for text with line breaks; blank lines stay.
        static std::vector<std::string_view> wrap_lines(ui::Painter &p, std::string_view text, float size, float width);

    private:
        // Pages and parts.
        void sidebar(ui::Ui &ui, ui::Rect r);
        void recommend_page(ui::Ui &ui, ui::Rect r);
        void toplist_page(ui::Ui &ui, ui::Rect r);
        void square_page(ui::Ui &ui, ui::Rect r);
        void fm_page(ui::Ui &ui, ui::Rect r);
        void collection_page(ui::Ui &ui, ui::Rect r);
        void artist_page(ui::Ui &ui, ui::Rect r);
        void search_page(ui::Ui &ui, ui::Rect r);
        void list_page(ui::Ui &ui, ui::Rect r, std::vector<Track> &tracks, bool loading, const std::string &error, double since);
        // since: when the list appeared, for the staggered entrance. Returns true when the last rows are visible.
        bool track_list(ui::Ui &ui, ui::Rect r, ui::Id id, std::vector<Track> &tracks, double since);
        void track_row(ui::Ui &ui, ui::Rect r, std::vector<Track> &tracks, int i);
        void track_menu(ui::Ui &ui, float x, float y, std::vector<Track> &tracks, int i);
        // Draws cards in columns from r's top-left; returns the clicked index or -1.
        int card_grid(ui::Ui &ui, ui::Rect r, ui::Id id, const std::vector<Card> &cards, bool round, double since);
        // card_grid in its own scrolling view; near_end tells when the last rows are close.
        int grid_view(ui::Ui &ui, ui::Rect r, ui::Id id, const std::vector<Card> &cards, bool round, double since, bool more, bool &near_end);
        bool card(ui::Ui &ui, ui::Id id, ui::Rect r, const Card &c, bool round);
        bool hero(ui::Ui &ui, ui::Id id, ui::Rect r, std::string_view big, std::string_view title, std::string_view subtitle, float hue);
        // Loading, error and signed-out states of a page; true when one was drawn.
        bool page_status(ui::Ui &ui, ui::Rect r, bool loading, const std::string &error, bool needs_login);
        static float panel_width(Panel panel);
        void show_panel(Panel panel); // the same one again, or none, closes it
        // slot: the space beside the page, which grows as the panel slides in; stage: how far the stage is up.
        void side_panel(ui::Ui &ui, ui::Rect slot, float stage);
        void queue_panel(ui::Ui &ui, ui::Rect r);
        void comments_panel(ui::Ui &ui, ui::Rect r);
        // stage: how far the stage is up; on it the bar also holds the comments and translation of the song on show.
        void player_bar(ui::Ui &ui, ui::Rect r, float stage);
        // The cover, title, artist and heart of the current track, which move between the player bar (np 0) and the
        // now-playing view (np 1), whose layout is wide (1) or narrow (0).
        void player_art(ui::Ui &ui, ui::Rect player, ui::Rect page, float np, float wide);
        // The stage over the window; page is where its cover and lyrics go.
        void now_playing(ui::Ui &ui, ui::Rect window, ui::Rect page, float k, float wide);
        // centered: 0 lines start at the left, 1 they are centred.
        void lyrics_view(ui::Ui &ui, ui::Rect r, float centered);
        // The size of a heart that pops when its track has just been liked.
        float heart_size(ui::Ui &ui, const Track &t, float size) const;
        // The buttons after "play all" and "shuffle" on album and playlist pages, from x.
        void list_actions(ui::Ui &ui, float x, float y);
        void account_modal(ui::Ui &ui, ui::Rect r);
        void settings_page(ui::Ui &ui, ui::Rect r);
        void theme_mock(ui::Ui &ui, ui::Rect r, const ui::Theme &look);
        void desktop_mock(ui::Ui &ui, ui::Rect r, bool island);
        void wallpaper(ui::Ui &ui, ui::Rect r, float radius);
        void categories_modal(ui::Ui &ui, ui::Rect r);
        // Lays out a comment at y, or only measures it; returns its height.
        float comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw);
        void editor_modal(ui::Ui &ui, ui::Rect r, Card card);
        void picker_modal(ui::Ui &ui, ui::Rect r, Track t);
        void open_account(ui::Ui &ui);
        // Opens the comments on something in the panel. follows: the current song's, which the stage keeps up with.
        void open_comments(ui::Ui &ui, Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows = false);
        // Points the comments at something and loads their first page, without showing them.
        void set_thread(ui::Ui &ui, Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows);
        // Leaving the stage closes the comments it opened.
        void show_stage(bool open);
        // A new playlist when card is empty, otherwise a rename.
        void open_editor(ui::Ui &ui, Card card = {}, std::optional<Track> track = {});
        void confirm_delete(ui::Ui &ui, Card card);
        void handle_keys(ui::Ui &ui);

        // Drawing helpers.
        void cover(ui::Ui &ui, ui::Rect r, const std::string &url, float radius, std::string_view placeholder = "music");
        float cover_hue(const std::string &url, float size);
        float special_hue(Page page) const;

        // Navigation.
        void search(ui::Ui &ui);
        void load_results(int tab);
        void switch_platform(ui::Ui &ui, Source source);
        bool supports(Page page, Source source) const;
        void open(ui::Ui &ui, Page page);
        void open(ui::Ui &ui, Page page, const Card &card);
        void open(ui::Ui &ui, const Card &card); // the page for a card
        void back(ui::Ui &ui);
        void enter(ui::Ui &ui, Route route, int direction);
        static bool needs_login(Page p);
        bool signed_in() const {
            return provider().logged_in();
        }
        void ensure_route_data();
        Task<void> load_list(std::shared_ptr<List> list, Page page, Card card);
        Task<void> fetch_results(uint64_t gen, int tab, Source source, std::string query, int offset);
        Task<void> load_artist(std::shared_ptr<ArtistPage> page, Card card);
        void load_artist_tab(int tab);
        Task<void> fetch_artist(std::shared_ptr<ArtistPage> page, Card card, int tab, int offset);
        Task<void> load_categories(Source source);
        void load_square();
        Task<void> fetch_square(Source source, uint64_t gen, std::string category, int offset);
        Task<void> load_collection(Source source);
        void load_comments(int tab);
        Task<void> fetch_comments(uint64_t gen, int tab, int page, std::string cursor);

        // Playback.
        void play(const std::vector<Track> &list, int i);
        void set_current(int i, bool replacing_queue = false);
        void toggle_play();
        void stop();
        void stop_player(); // reports how far the track got, then stops
        void report_listen(bool ended);
        void skip(int delta, bool automatic = false);
        void seek(double at);
        void set_mode(Mode m);
        int next_index();
        void start(double at = 0, bool play = true);
        Task<void> load_track(uint64_t gen, Track t, double at);
        Task<void> load_lyrics(Track t);
        Task<void> prefetch(Track t);
        void schedule_prefetch();
        void on_player();
        void apply_volume();
        void apply_gain();
        double position();
        void publish(); // the player state for the media controls
        bool skip_trials() const {
            return config_.playback.trial == TrialMode::skip;
        }
        const Track *current() const {
            return current_ >= 0 ? &queue_[static_cast<size_t>(current_)] : nullptr;
        }
        bool is_current(const Track &t) const {
            return current() && same(*current(), t);
        }
        bool playing() const;
        Provider &provider(Source source) const {
            return *providers_[static_cast<size_t>(source)];
        }
        Provider &provider() const {
            return provider(active_);
        }
        template <class T>
        Provider &provider(const T &t) const {
            return provider(t.source);
        }
        Library &library(Source source) {
            return libraries_[static_cast<size_t>(source)];
        }
        const Library &library(Source source) const {
            return libraries_[static_cast<size_t>(source)];
        }
        Library &library() {
            return library(active_);
        }
        const Library &library() const {
            return library(active_);
        }

        // Library and account.
        Task<void> startup();
        Task<void> initialize_platform(Source source);
        Task<void> load_library(Source source);
        Task<void> load_recommended(Source source);
        Task<void> load_daily_lists(Source source);
        Task<void> load_toplists(Source source);
        Task<void> load_fm(Source source);
        void refresh_recent();
        void next_fm();
        Task<void> qr_login();
        void cancel_login();
        Task<void> logout(Source source);
        void save_credentials();
        bool liked(const Track &t) const;
        void toggle_like(Track t);
        void dislike_daily(std::shared_ptr<List> list, Track t);
        // Plays the heartbeat list that follows seed.
        Task<void> heartbeat(Track seed);
        void toggle_follow(std::shared_ptr<ArtistPage> page, Card artist);
        void toggle_collect(std::shared_ptr<List> list, Card album);
        void add_to_playlist(Track t, Card playlist);
        void remove_from_playlist(std::shared_ptr<List> list, Card playlist, Track t);
        void recount(const Card &playlist, int delta); // the sidebar's track count
        void retitle(const Card &playlist, const std::string &title);
        void set_category(ui::Ui &ui, Category category);
        void notify(std::string text, std::string icon = "info", bool error = false);
        void changed(); // persisted state changed

        Loop &loop_;
        net::Http &http_;
        audio::Player &player_;
        Providers providers_;
        Images &images_;
        const Config &config_;
        std::move_only_function<void(std::string_view, std::string_view, std::string_view)> set_setting_;
        std::move_only_function<void()> redraw_;
        std::move_only_function<double()> now_;
        bool silent_;

        Route route_{Page::recommend};
        Source active_ = Source::netease, fm_source_ = Source::netease;
        std::optional<Source> pending_source_;
        std::vector<Route> history_;
        double page_time_ = -10, tab_time_ = -10;
        int page_dir_ = 1, tab_dir_ = 1;
        bool compact_ = false;
        std::string today_;
        struct Toast {
            std::string text, icon;
            bool error;
        };
        std::vector<Toast> toasts_;

        // Library.
        std::array<Library, platforms.size()> libraries_;
        bool can_save_creds_ = false;
        creds::Credentials credentials_;
        Loop::TimerId creds_timer_ = 0;
        std::vector<Track> recent_;

        // Search.
        std::string query_, searched_;
        int search_tab_ = 0;
        std::array<SearchResults, 4> results_;
        uint64_t search_gen_ = 0;
        int selected_ = -1;

        // Playback.
        std::vector<Track> queue_;
        int current_ = -1;
        uint64_t play_gen_ = 0;
        audio::Player::State last_state_ = audio::Player::State::idle;
        bool want_play_ = false, fetching_ = false, fm_playing_ = false;
        bool muted_ = false, retried_ = false, counted_ = false;
        Panel panel_ = Panel::none, panel_last_ = Panel::queue, panel_from_ = Panel::none; // last: the one drawn while it closes
        double panel_time_ = -10;
        size_t failures_ = 0;
        Mode mode_ = Mode::list;
        std::vector<int> shuffle_history_;
        int shuffle_next_ = -1;
        double resume_at_ = 0, last_pos_ = 0;
        float volume_ = 0.7f, progress_w_ = 400;
        bool dragging_ = false;
        float drag_v_ = 0;
        struct Prefetched {
            std::string key;
            std::shared_ptr<audio::Spool> spool;
            uint64_t gen = 0;
            Stream stream;
        } prefetch_;
        Loop::TimerId prefetch_timer_ = 0;
        float track_gain_ = 0, track_peak_ = 0; // loudness of the current stream
        bool trial_ = false;                    // the current stream is a preview clip
        std::optional<Track> listen_;           // the track whose listen is reported to the account
        uint64_t rng_ = 0x2545f4914f6cdd1dull;
        Loop::TimerId save_timer_ = 0;
        std::optional<std::pair<int, int>> window_;
        Track prev_; // the track before a change, fading out
        double cover_time_ = -10;
        std::string pop_key_; // the track liked last, when
        double pop_time_ = -10;

        std::unique_ptr<Mpris> mpris_;

        // Lyrics and the now-playing view.
        std::vector<LyricLine> lyrics_;
        std::string lyrics_id_;
        double clip_start_ = 0; // where a trial clip sits in the song the lyrics are timed for
        bool lyrics_loading_ = false;
        bool np_open_ = false, np_wide_ = true;
        double drift_ = 0, drift_last_ = 0; // how far the backdrop has drifted: it moves only while the music plays
        struct LyricBlock {
            std::vector<std::string_view> lines, translation; // wrapped, pointing into lyrics_
            float y = 0, h = 0;
        };
        std::vector<LyricBlock> lyric_layout_;
        float lyric_layout_w_ = 0, lyric_layout_size_ = 0;
        std::string lyric_layout_font_;
        // Each line scrolls on its own spring towards the offset it aims at. When the next line comes up, the lines below
        // it set off a little later than the ones above, so the change ripples down; jumps and the wheel move them as one.
        std::vector<ui::Spring> lyric_y_;
        std::vector<float> lyric_aim_;
        float lyric_manual_ = 0;
        int lyric_to_ = 0; // what the view centres on: 2i is line i, 2i - 1 the pause before it
        bool lyric_ripple_ = false;
        double lyric_switch_ = -10, lyric_manual_until_ = 0, lyric_last_ = 0, lyrics_since_ = -10;

        // Desktop lyrics: offset from the screen's centre, height above the bottom, click-through.
        float dl_x_ = 0, dl_bottom_ = 48;
        bool dl_locked_ = false, dl_dragging_ = false;
        bool dl_enabled_ = false, dl_island_ = false, lyric_translation_ = true;
        std::move_only_function<void()> desktop_changed_;
        static float sung_x(ui::Painter &p, const LyricLine &line, std::string_view text, float size, float soft, double position);
        void toggle_translation();
        void set_desktop_island(bool island);
        float dl_grab_x_ = 0, dl_grab_y_ = 0, dl_last_y_ = 0;
        // The line on show and the one it replaced, for the change between them.
        struct ShownLine {
            std::string text, sub;
            bool timed = false;
        };
        struct LineChange {
            ShownLine shown, prev;
            double since = -10, prev_since = -10;
        };
        LineChange island_line_, floating_line_;
        bool island_dragging_ = false;
        float island_drag_v_ = 0;
        // Notes the line on show; returns how far the change to it has gone, 0..1.
        float show_line(ui::Ui &ui, LineChange &change, std::string_view text, std::string_view sub, bool timed);
        // Frames for desktop lyrics: continuous while a word-timed line is sung, otherwise one when the next line starts.
        void lyric_frames(ui::Ui &ui, int line, bool timed);
        // The island hanging from top at center; floating lyrics across a surface. Both return where they take input.
        ui::Rect island(ui::Ui &ui, float center, float top);
        ui::Rect floating(ui::Ui &ui, float width, float height);
        // A line of floating lyrics, outlined; a word-timed live line lights up from color to rest as it is sung.
        void floating_text(ui::Ui &ui, float center, float baseline, std::string_view text, const LyricLine *live, float size, float max_width, ui::Color color, ui::Color rest);

        // Accent.
        float accent_hue_ = -2;
        std::array<ui::Color, 2> accent_from_{}; // where a change set off from, on light and on dark backgrounds
        double accent_time_ = -10;
        float bar_cover_px_ = 64;

        // Account modal.
        int login_step_ = -1; // -1 account list, otherwise the service being scanned
        double login_time_ = -10;
        uint64_t qr_gen_ = 0;
        bool qr_active_ = false;
        std::vector<uint8_t> qr_; // modules, row by row
        int qr_size_ = 0;
        bool qr_image_ = false;
        std::optional<QrStatus> qr_status_;
        std::string qr_error_;

        // Comments panel and the playlist editor.
        Subject thread_subject_ = Subject::song;
        Source thread_source_ = Source::netease;
        std::string thread_id_, thread_title_, thread_cover_;
        bool thread_follows_ = false;
        int thread_tab_ = 0;
        double thread_time_ = -10;
        std::array<Feed, 2> feeds_; // hot, newest
        uint64_t thread_gen_ = 0;
        std::string edit_name_;
        bool edit_hidden_ = false, edit_focus_ = false;
        Source edit_source_ = Source::netease;
        std::optional<Track> edit_track_;

        // Settings page: its height last frame, where each section starts, the tab it is gliding to.
        float settings_height_ = 1000;
        std::array<float, 4> settings_tops_{};
        int settings_jump_ = 0;
        double settings_jump_until_ = 0;
        std::map<std::string, float> settings_sliders_; // values being dragged, written on release
        std::move_only_function<void()> open_config_;
        const ui::Theme *stage_ = nullptr; // this frame's stage theme, for the desktop previews
    };

} // namespace lyren
