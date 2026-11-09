#pragma once
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "app/images.hpp"
#include "media/lyrics.hpp"
#include "media/provider.hpp"
#include "media/timeline.hpp"
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
        bool desktop_enabled() const {
            return dl_enabled_;
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
            radar,
            releases,
            settings,
            liked,
            recent,
            cloud,
            collection,
            dislikes,
            playlist,
            album,
            artist,
            similar,
            search,
            profile,
            moments
        };
        // Tracks behind a page, loaded when it is entered.
        struct List {
            std::vector<Track> tracks;
            bool loading = false;
            std::string error;
            double since = -10; // when the rows arrived, for the entrance
            AlbumInfo album;    // album pages: the details; the tracks are above
            uint64_t gen = 0;
            bool ordering = false, saving_order = false;
        };
        // A listing that loads a page at a time.
        struct Results {
            std::vector<Track> tracks;
            std::vector<Card> cards;
            int total = 0;
            int next_offset = 0;
            bool loading = false, loaded = false;
            std::string error;
            double since = -10;
            size_t size() const {
                return tracks.size() + cards.size();
            }
            bool more() const {
                return next_offset >= 0;
            }
        };
        struct DislikeList {
            std::vector<Dislike> items;
            std::unordered_set<std::string> pending;
            bool loading = false, loaded = false;
            std::string error;
            double since = -10;
            uint64_t gen = 0;
        };
        enum class Preference : uint8_t {
            song,
            artist,
            album,
            playlist,
            user
        };
        // One writer per resource. Further clicks change the desired value while the current request finishes.
        struct PreferenceEdit {
            bool value = false, confirmed = false, pending = false;
            uint64_t revision = 0;
            Track track;
            Card card;
        };
        // Posts a page at a time: someone's timeline or the feed. Heights are laid out for heights_w.
        struct PostList {
            std::vector<Post> posts;
            std::vector<float> heights;
            float heights_w = 0, heights_size = 0;
            bool more = true, loading = false;
            std::string cursor, error;
            double since = -10;
            uint64_t gen = 0;
        };
        // Each platform keeps its own account, cached pages and pagination state.
        struct Library {
            std::optional<Account> account;
            uint64_t gen = 0, account_gen = 0, preference_gen = 0, playlist_gen = 0;
            std::array<StringMap<PreferenceEdit>, 5> preferences;
            std::unordered_set<std::string> playlist_writes;
            std::vector<Card> playlists, daily_lists;
            double daily_since = -10;
            Card liked;
            std::unordered_set<std::string> liked_ids;
            Results recommended, toplists;
            std::array<Results, 2> releases;
            std::array<int, 2> release_category{};
            std::array<uint64_t, 2> release_gen{};
            int release_tab = 0;
            std::vector<Track> recent, fm;
            int fm_pos = 0, recent_tab = 0;
            bool fm_loading = false, playlists_loaded = false;
            std::string fm_error, library_error;
            Categories categories;
            bool categories_loading = false;
            Category category;
            Results square;
            uint64_t square_gen = 0;
            double square_time = -10;
            std::array<Results, 2> collection;
            int collection_tab = 0;
            uint64_t collection_gen = 0;
            std::array<std::shared_ptr<DislikeList>, 3> dislikes;
            int dislike_tab = 0;
            std::shared_ptr<PostList> feed; // posts by the people the account follows
        };
        // An artist's page: the head, songs and albums a page at a time, the introduction (intro holds only its status).
        struct ArtistPage {
            std::optional<ArtistInfo> info;
            bool loading = false;
            std::string error;
            int tab = 0;
            Results songs, albums, intro;
            std::vector<std::pair<std::string, std::string>> sections;
        };
        struct PeopleList {
            std::vector<Person> people;
            int total = -1;
            bool more = true, loading = false;
            std::string cursor, error;
            double since = -10;
            uint64_t gen = 0;
        };
        struct RankingList {
            Ranking ranking;
            bool loading = false, loaded = false;
            std::string error;
            double since = -10;
        };
        // What a listener's page opens into from its board of tiles.
        enum class ProfileView : uint8_t {
            board,
            posts,
            ranking,
            follows,
            fans,
            playlists,
            feed
        };
        // A listener's page: their head and record, the board of tiles, and the view a tile opened into.
        struct ProfilePage {
            std::optional<Profile> info;
            bool loading = false;
            std::string error;
            ProfileView view = ProfileView::board, shown = ProfileView::posts; // shown: the view drawn while it closes
            std::array<ui::Rect, 7> tiles{};                                   // where the board last drew each view's tile
            int ranking_tab = 0;                                               // the week or all time
            std::array<PeopleList, 2> people;                                  // follows, fans
            std::shared_ptr<PostList> posts = std::make_shared<PostList>();
            std::array<RankingList, 2> rankings;
            Results playlists;
            int created = 0; // how many of the playlists they made; the rest they saved
            ui::Rect origin; // the picture the page was opened from, which flies to the record's label
            double since = -10, view_time = -10;
            float narrow_h = 0; // the scrolling column's height in narrow windows, as last laid out
        };
        struct Route {
            Page page;
            Card card; // what a playlist-like page shows
            std::shared_ptr<List> list;
            std::shared_ptr<ArtistPage> artist;
            std::shared_ptr<ProfilePage> profile;
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
        struct CommentDraft {
            std::string text;
            std::optional<Comment> reply;
            std::string insert; // text the editor takes at its caret on the next frame
            bool sending = false, focus = false, expanded = false;
        };
        struct ReplyView {
            Comment root;
            std::shared_ptr<Feed> feed = std::make_shared<Feed>();
            std::shared_ptr<CommentDraft> draft = std::make_shared<CommentDraft>();
            double opened = -10;
        };
        // The comments panel's current thread: what it comments on, its two feeds and the open replies.
        struct Thread {
            Source source = Source::netease;
            Subject subject = Subject::song;
            std::string id, title, cover;
            bool follows = false;
            int tab = 0;
            double time = -10, opened = -10; // tab switched, subject changed
            uint64_t gen = 0;
            std::array<std::shared_ptr<Feed>, 2> feeds{std::make_shared<Feed>(), std::make_shared<Feed>()}; // hot, newest
            std::shared_ptr<ReplyView> replies;
            std::shared_ptr<CommentDraft> draft = std::make_shared<CommentDraft>();
            bool reset_scroll = false;
        };
        // A like the user set on a comment, shown until the service's own pages echo it back.
        struct Like {
            bool liked = false;
            int likes = 0;
            uint64_t pending = 0; // write ID, or zero when settled
        };
        using Mode = PlayMode;
        // What the panel beside the page shows.
        enum class Panel : uint8_t {
            none,
            queue,
            comments
        };

        // Pages and parts.
        void sidebar(ui::Ui &ui, ui::Rect r);
        void recommend_page(ui::Ui &ui, ui::Rect r);
        void toplist_page(ui::Ui &ui, ui::Rect r);
        void square_page(ui::Ui &ui, ui::Rect r);
        void fm_page(ui::Ui &ui, ui::Rect r);
        void releases_page(ui::Ui &ui, ui::Rect r);
        void collection_page(ui::Ui &ui, ui::Rect r);
        void dislikes_page(ui::Ui &ui, ui::Rect r);
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
        bool hero(ui::Ui &ui, ui::Id id, ui::Rect r, std::string_view big, std::string_view title, std::string_view subtitle, float hue, std::string_view icon = {});
        // What stands in for a page's content while it loads: the shape of what is coming, or a spinner.
        enum class Placeholder : uint8_t {
            spinner,
            rows,
            cards,
            faces
        };
        // Loading, error and signed-out states of a page; true when one was drawn.
        bool page_status(ui::Ui &ui, ui::Rect r, bool loading, const std::string &error, bool needs_login, Placeholder look = Placeholder::spinner);
        static float panel_width(Panel panel);
        void show_panel(Panel panel); // the same one again, or none, closes it
        // slot: the space beside the page, which grows as the panel slides in; stage: how far the stage is up.
        void side_panel(ui::Ui &ui, ui::Rect slot, float stage);
        void queue_panel(ui::Ui &ui, ui::Rect r);
        void comments_panel(ui::Ui &ui, ui::Rect r);
        float comment_composer(ui::Ui &ui, ui::Rect r);
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
        // "Play all" and "Shuffle" from x; returns where the next button goes.
        float play_buttons(ui::Ui &ui, float x, float y, std::vector<Track> &tracks);
        // A page's tabs: picking one starts the slide of its content, which tab_enter applies to content and returns
        // the fade of.
        bool page_tabs(ui::Ui &ui, ui::Id id, ui::Rect r, std::span<const std::string_view> labels, int &selected, std::span<const bool> enabled = {});
        float tab_enter(ui::Ui &ui, ui::Rect &content) const;
        // The buttons after "play all" and "shuffle" on album and playlist pages, from x.
        void list_actions(ui::Ui &ui, float x, float y);
        void account_modal(ui::Ui &ui, ui::Rect r);
        void settings_page(ui::Ui &ui, ui::Rect r);
        void theme_mock(ui::Ui &ui, ui::Rect r, const ui::Theme &look);
        void desktop_mock(ui::Ui &ui, ui::Rect r, bool island);
        void wallpaper(ui::Ui &ui, ui::Rect r, float radius);
        void categories_modal(ui::Ui &ui, ui::Rect r);
        // Listener pages: the page over the window, their record and head, the board of tiles and the views they open.
        void profile_page(ui::Ui &ui, ui::Rect r);
        void profile_backdrop(ui::Ui &ui, ui::Rect r, const Profile &who, float in);
        // The record: their most played songs as grooves around their picture. aside: how far it has rolled to the edge
        // to make room for a view, where a click brings it back.
        void profile_record(ui::Ui &ui, ui::Rect disc, ProfilePage &page, const Profile &who, float in, float aside);
        // Their name, words and actions beside or under the record.
        void profile_head(ui::Ui &ui, ui::Rect r, ProfilePage &page, const Profile &who, bool self, bool centered);
        // The tiles fill r's height (fill) or stand at a fixed height; returns the height they take.
        float profile_board(ui::Ui &ui, ui::Rect r, ProfilePage &page, const Profile &who, bool self, bool fill);
        // A tile's glass and heading; returns the room for its content. id 0: a tile that opens nothing.
        ui::Rect profile_tile(ui::Ui &ui, ui::Id id, ui::Rect r, std::string_view icon, std::string_view title, std::string_view note, bool &clicked);
        void profile_view(ui::Ui &ui, ui::Rect r, ProfilePage &page, ProfileView view, const Profile &who, bool self);
        void people_view(ui::Ui &ui, ui::Rect r, ProfilePage &page, bool self);
        void ranking_view(ui::Ui &ui, ui::Rect r, ProfilePage &page);
        void playlists_view(ui::Ui &ui, ui::Rect r, ProfilePage &page, bool self);
        void person_card(ui::Ui &ui, ui::Rect r, Source source, const Person &person);
        void follow_button(ui::Ui &ui, ui::Id id, ui::Rect r, Source source, const Person &person);
        static float follow_button_width(ui::Ui &ui);
        void open_profile_view(ui::Ui &ui, ProfilePage &page, ProfileView view);
        // The fans shown for someone, counting a follow the user has just made or taken back.
        int shown_fans(Source source, const Profile &who) const;
        int people_count(const ProfilePage &page, Source source, const Profile &who, bool fans) const;
        // from: the picture clicked, which the record's label grows out of.
        void open_profile(ui::Ui &ui, Source source, std::string id, std::string name, std::string avatar, ui::Rect from = {});
        void open_own_profile(ui::Ui &ui, ui::Rect from = {});
        bool own_profile(const Card &card) const;
        bool listening_tile(const Card &card) const;
        std::vector<ProfileView> profile_tiles(const Card &card) const;
        Task<void> load_profile(std::shared_ptr<ProfilePage> page, Card card);
        void load_people(std::shared_ptr<ProfilePage> page, Card card, int tab);
        Task<void> fetch_people(std::shared_ptr<ProfilePage> page, Card card, int tab, uint64_t gen);
        void load_ranking(std::shared_ptr<ProfilePage> page, Card card, int tab);
        Task<void> fetch_ranking(std::shared_ptr<ProfilePage> page, Card card, int tab);
        Task<void> load_profile_playlists(std::shared_ptr<ProfilePage> page, Card card);
        void toggle_follow_user(Source source, const Person &person);
        bool following(Source source, const Person &person) const;

        // Posts: the feed page, a list of posts, one post, and writing, liking, forwarding and deleting them.
        void moments_page(ui::Ui &ui, ui::Rect r);
        // byline: posts name their authors (the feed); a listener's own timeline does not need to.
        void post_list(ui::Ui &ui, ui::Rect r, ui::Id id, std::shared_ptr<PostList> list, Source source, const std::string &author, bool byline, double since);
        float post_card(ui::Ui &ui, Source source, const Post &post, float x, float y, float w, bool draw, bool byline);
        // The song, playlist, pictures or forwarded post a post carries; returns the height used.
        float post_attachments(ui::Ui &ui, Source source, const Post &post, float x, float y, float w, bool draw);
        void load_posts(std::shared_ptr<PostList> list, Source source, std::string author);
        Task<void> fetch_posts(std::shared_ptr<PostList> list, Source source, std::string author, uint64_t gen);
        void refresh_own_posts(Source source);
        void toggle_post_like(Source source, const Post &post);
        void confirm_post_delete(ui::Ui &ui, Source source, Post post);
        void open_composer(ui::Ui &ui, Source source, std::optional<Track> track = {}, std::optional<Post> forward = {});
        void composer_modal(ui::Ui &ui, ui::Rect r);
        float composer_height();
        Task<void> send_post();
        // Starts a song right away after the one playing, keeping the rest of the queue.
        void play_now(Track track);

        // Lays out a comment at y, or only measures it; returns its height. featured: the top hot comment, set as a quote.
        float comment(ui::Ui &ui, const Comment &c, float x, float y, float w, bool draw, bool featured = false);
        // The comments header over the subject's cover: the subject, its record and the tabs. Returns its height.
        float comment_sleeve(ui::Ui &ui, ui::Rect r, bool compact);
        void comment_picture(ui::Ui &ui, const Comment &c, ui::Rect r);
        // A picture shown large over the window; aspect is width over height.
        void show_picture(ui::Ui &ui, std::string url, float aspect);
        // The comments are on the song that plays now.
        bool thread_playing() const;
        // The lyric line playing now, when the comments are on the song that is current.
        const LyricLine *thread_lyric();
        void editor_modal(ui::Ui &ui, ui::Rect r, Card card);
        void picker_modal(ui::Ui &ui, ui::Rect r, const std::vector<Track> &tracks);
        void open_picker(ui::Ui &ui, std::vector<Track> tracks);
        void open_account(ui::Ui &ui);
        // Opens the comments on something in the panel. follows: the current song's, which the stage keeps up with.
        void open_comments(ui::Ui &ui, Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows = false);
        // Points the comments at something and loads their first page, without showing them.
        void set_thread(ui::Ui &ui, Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows);
        // Leaving the stage closes the comments it opened.
        void show_stage(bool open);
        // A new playlist when card is empty, otherwise a rename.
        void open_editor(ui::Ui &ui, Card card = {}, std::vector<Track> tracks = {});
        void confirm_delete(ui::Ui &ui, Card card);
        void handle_keys(ui::Ui &ui);

        // Drawing helpers.
        void cover(ui::Ui &ui, ui::Rect r, const std::string &url, float radius, std::string_view placeholder = "music");
        float cover_hue(const std::string &url, float size);
        float special_hue(Page page) const;
        std::string date_label() const;

        // Navigation.
        static ui::Id search_scroll_id(ui::Ui &ui, Source source, int tab);
        void search(ui::Ui &ui);
        void load_results(int tab, bool retry = false);
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
        void arrived(Results &res, Result<Batch> result);
        Task<void> warm_pages(Source source);
        Task<void> load_list(std::shared_ptr<List> list, Page page, Card card);
        Task<void> fetch_results(uint64_t gen, int tab, Source source, std::string query, int offset);
        Task<void> load_artist(std::shared_ptr<ArtistPage> page, Card card);
        void load_artist_tab(int tab, bool retry = false);
        Task<void> fetch_artist(std::shared_ptr<ArtistPage> page, Card card, int tab, int offset);
        Task<void> load_categories(Source source);
        void load_square();
        Task<void> fetch_square(Source source, uint64_t gen, std::string category, int offset);
        void load_releases();
        void set_release_category(ui::Ui &ui, int category);
        Task<void> fetch_releases(Source source, int tab, uint64_t gen, std::string category, int offset);
        Task<void> load_collection(Source source);
        void load_comments(int tab);
        Task<void> fetch_comments(uint64_t gen, int tab, int page, std::string cursor);
        void refresh_comments();
        void open_replies(Comment root);
        void load_replies(std::shared_ptr<ReplyView> view);
        Task<void> fetch_replies(std::shared_ptr<ReplyView> view, int page, std::string cursor);
        void toggle_comment_like(Source source, const Comment &comment);
        void send_comment();
        void confirm_comment_delete(ui::Ui &ui, Comment comment);
        // Reconcile cached copies before returning a confirmed like to the service's own numbers.
        void settle_likes(Source source, const std::vector<Comment> &comments);
        void clear_comment_writes(Source source);

        // Playback.
        void play(const std::vector<Track> &list, int i);
        void set_current(int i, bool replacing_queue = false);
        void toggle_play();
        void stop();
        void stop_player(); // stops output and reports the actual listening time
        void report_listen(bool ended);
        void skip(int delta, bool automatic = false);
        void seek(double at);
        void seek_song(double at); // song time; resolves the file offset once the stream is known
        void set_mode(Mode m);
        int next_index();
        uint64_t random();
        void start(double at = 0, bool play = true);
        Task<void> load_track(uint64_t gen, Track t);
        Task<void> load_lyrics(Track t);
        Task<void> prefetch(Track t);
        void schedule_prefetch();
        void on_player();
        void apply_volume();
        void apply_gain();
        double position();
        Timeline timeline() const;
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
            return providers_[static_cast<size_t>(source)].get();
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
        void apply_playlists(Source source, std::pair<Card, std::vector<Card>> playlists, uint64_t revision);
        Task<bool> refresh_playlists(Source source, uint64_t account_gen);
        Task<void> load_cards(Source source, Results &res, Task<Result<std::vector<Card>>> request);
        Task<void> load_daily_lists(Source source);
        Task<void> load_fm(Source source);
        Task<void> load_dislikes(Source source, DislikeKind kind);
        void set_dislike(Dislike item, bool on);
        void refresh_recent();
        void next_fm();
        Task<void> qr_login();
        void start_login(ui::Ui &ui, Source source);
        Task<void> import_login(Source source);
        void cancel_login();
        Task<void> logout(Source source);
        void save_credentials();
        void reset_account(Source source);
        bool liked(const Track &t) const;
        bool preference_value(Source source, Preference kind, std::string_view id, bool value) const;
        void set_preference(Source source, Preference kind, Track track, Card card, bool on);
        Task<void> write_preference(Source source, Preference kind, std::string id, uint64_t account_gen);
        void apply_preference(Source source, Preference kind, const PreferenceEdit &edit);
        bool reconcile_preference(Source source, Preference kind, std::string_view id, bool value, uint64_t revision);
        void reconcile_preferences(Source source, Preference kind, uint64_t revision);
        void toggle_like(Track t);
        void dislike_daily(std::shared_ptr<List> list, Track t);
        // Plays the heartbeat list that follows seed.
        Task<void> heartbeat(Track seed);
        void toggle_follow(std::shared_ptr<ArtistPage> page, Card artist);
        void toggle_collect(std::shared_ptr<List> list, Card album);
        void toggle_collect_playlist(Card playlist);
        bool begin_playlist_write(const Card &playlist);
        void finish_playlist_write(const Card &playlist);
        Task<void> create_playlist(Source source, std::string name, bool hidden, std::vector<Track> tracks);
        Task<void> rename_playlist(Card playlist, std::string name);
        Task<void> delete_playlist(Card playlist);
        void add_to_playlist(std::vector<Track> tracks, Card playlist);
        void remove_from_playlist(std::shared_ptr<List> list, Card playlist, std::vector<Track> tracks);
        Task<void> write_playlist_tracks(std::shared_ptr<List> list, Card playlist, std::vector<Track> tracks, bool add);
        void apply_playlist_tracks(std::shared_ptr<List> list, const Card &playlist, std::vector<Track> tracks);
        std::vector<Track> chosen_tracks(const std::vector<Track> &tracks) const;
        void choose_track(const std::vector<Track> &tracks, int i, bool range);
        void reset_selection();
        void move_playlist_track(std::shared_ptr<List> list, Card playlist, int from, int to);
        void link_menu(ui::Ui &ui, float x, float y, const Card &card);
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
        std::array<std::array<Results, 5>, platforms.size()> results_;
        uint64_t search_gen_ = 0;
        int selected_ = -1;
        bool choosing_ = false;
        int selection_anchor_ = -1;
        const std::vector<Track> *selection_list_ = nullptr;
        std::unordered_set<std::string> chosen_;

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
        double resume_at_ = 0, last_pos_ = 0;     // nonnegative seconds within the current file
        std::optional<double> pending_song_seek_; // nonnegative song seconds, only while fetching the stream
        float volume_ = 0.7f, progress_w_ = 400;
        bool dragging_ = false;
        float drag_v_ = 0; // normalized song position, [0, 1]
        struct Prefetched {
            std::string key;
            std::shared_ptr<audio::Spool> spool;
            uint64_t gen = 0;
            Stream stream;
        } prefetch_;
        Loop::TimerId prefetch_timer_ = 0;
        float track_gain_ = 0, track_peak_ = 0; // loudness of the current stream
        bool trial_ = false;                    // the current stream is a preview clip
        std::string stream_level_;              // the quality actually granted by the service
        std::optional<Track> listen_;           // the track whose listen is reported to the account
        double listen_started_ = 0;             // cumulative output time at the start of this listen
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
        double clip_start_ = 0;                        // where a trial clip sits in the song the lyrics are timed for
        double highlight_from_ = 0, highlight_to_ = 0; // the stream's chorus window, s in song time; empty when the end is not after the start
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
        // The island: its offset from the centre and from the top edge, and where a drag on it started.
        float island_x_ = 0, island_y_ = 10;
        DesktopSnap dl_snap_, island_snap_;
        bool island_moving_ = false, island_dragged_ = false;
        float island_grab_x_ = 0, island_grab_y_ = 0, island_from_x_ = 0, island_from_y_ = 0;
        bool dl_locked_ = false, dl_dragging_ = false;
        bool dl_enabled_ = false, dl_island_ = false, lyric_translation_ = true;
        std::move_only_function<void()> desktop_changed_;
        static float sung_x(ui::Painter &p, const LyricLine &line, std::string_view text, float size, float soft, double position);
        void toggle_translation();
        void set_desktop_island(bool island);
        float dl_grab_x_ = 0, dl_grab_y_ = 0;
        // The line on show and the one it replaced, for the change between them.
        struct ShownLine {
            std::string text, sub;
            bool timed = false;
        };
        struct LineChange {
            ShownLine shown, prev;
            double since = -10, prev_since = -10;
        };
        LineChange island_line_;
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
        } floating_lines_;
        std::vector<ui::Painter::Lift> lyric_lifts_; // the wave under a word-timed line, reused by the desktop lyrics
        bool island_dragging_ = false;
        float island_drag_v_ = 0; // normalized song position, [0, 1]
        // Notes the line on show; returns how far the change to it has gone, 0..1.
        float show_line(ui::Ui &ui, LineChange &change, std::string_view text, std::string_view sub, bool timed);
        // Frames for desktop lyrics: continuous while a word-timed line is sung, otherwise one when the next line starts.
        void lyric_frames(ui::Ui &ui, int line, bool timed);
        // The island hanging from top at center; floating lyrics across a surface. Both return where they take input.
        // movable: lyrics on the desktop, which a drag repositions, as opposed to the settings page's previews.
        ui::Rect island(ui::Ui &ui, float center, float top, bool movable);
        ui::Rect floating(ui::Ui &ui, ui::Rect area, bool movable);
        // A line of floating lyrics, outlined; a word-timed live line lights up from color to rest as it is sung.
        void floating_text(ui::Ui &ui, float center, float baseline, std::string_view text, const LyricLine *live, float size, float max_width, ui::Color color, ui::Color rest,
                           float focus);

        // Accent.
        float accent_hue_ = -2;
        std::array<ui::Color, 2> accent_from_{}; // where a change set off from, on light and on dark backgrounds
        double accent_time_ = -10;
        float bar_cover_px_ = 64;

        // Account modal.
        int login_step_ = -1; // -1 account list, otherwise the service being connected
        double login_time_ = -10;
        uint64_t qr_gen_ = 0;
        bool qr_active_ = false;
        std::vector<uint8_t> qr_; // modules, row by row
        int qr_size_ = 0;
        bool qr_image_ = false;
        std::optional<QrStatus> qr_status_;
        std::string qr_error_;
        std::string account_config_;
        bool account_importing_ = false;

        // Listener pages and posts: likes and deletions the user made, the post being written, and how far records
        // have turned while music played.
        CacheMap<Like> post_likes_;
        CacheMap<uint64_t> post_deletes_;
        uint64_t post_write_gen_ = 0;
        struct PostDraft {
            Source source = Source::netease;
            std::string text;
            std::optional<Track> track;
            std::optional<Post> forward;
            bool sending = false, focus = false;
        } post_draft_;
        double record_turn_ = 0, record_last_ = 0;
        ui::Rect profile_from_; // the picture a profile is being opened from

        // Comments panel and the playlist editor.
        Thread thread_;
        // The user's own like per comment, key(source, id): the panel shows it over the service's until a read echoes it.
        CacheMap<Like> comment_likes_;
        CacheMap<uint64_t> comment_deletes_;
        uint64_t comment_write_gen_ = 0;
        std::pair<size_t, size_t> composer_layer_; // the comment composer's primitives, raised over the list
        std::string edit_name_;
        bool edit_hidden_ = false, edit_focus_ = false;
        Source edit_source_ = Source::netease;
        std::vector<Track> edit_tracks_;

        // Settings page: where each section starts, the tab it is gliding to and the one on show, and the layout: the
        // cards' heights as last drawn, the columns, where the headings, cards and title were drawn, and the move
        // between one column and two (from where, since when).
        std::array<float, 4> settings_tops_{};
        int settings_jump_ = 0, settings_section_ = 0, settings_columns_ = 0;
        double settings_jump_until_ = 0, settings_drawn_ = -10, settings_moved_ = -10;
        std::array<float, 9> settings_heights_{196, 240, 58, 132, 234, 386, 250, 190, 132};
        std::array<ui::Rect, 14> settings_shown_{}, settings_from_{};
        StringMap<float> settings_sliders_; // values being dragged, written on release
        std::move_only_function<void()> open_config_;
        const ui::Theme *stage_ = nullptr; // this frame's stage theme, for the desktop previews
    };

} // namespace lyren
