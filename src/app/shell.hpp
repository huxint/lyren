#pragma once
#include <cmath>
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
#include "ncm/netease.hpp"
#include "sys/mpris.hpp"
#include "ui/ui.hpp"

namespace lyren {

// What the window content needs from the app.
struct ShellDeps {
    Loop& loop;
    net::Http& http;
    audio::Player& player;
    ncm::Netease& ncm;
    Images& images;
    const Config& config;
    // Changes a setting in the config file; value is TOML. config reflects it once this returns.
    std::function<void(std::string_view section, std::string_view key, std::string_view value)> set_setting;
    std::function<void()> redraw;  // async results arrived
    std::function<double()> now;   // the UI clock
    bool silent;                   // scripted runs: the stream volume stays 0
    Bus& bus;                      // for the media controls; get() is null without a session bus
    std::function<void(std::string_view token)> raise;  // bring the window forward
};

// Window content: search and account in the top bar, sidebar, pages, queue, the player bar and the now-playing view.
class Shell {
public:
    static constexpr float player_h = 84;

    Shell(ShellDeps deps, SavedState saved);
    // bar is the top strip beside the window controls; the search box centres on center_x.
    void top_bar(ui::Ui& ui, ui::Rect bar, float center_x);
    void draw(ui::Ui& ui, ui::Rect area);
    // Accent taken from the current cover, cross-faded after a change.
    ui::Color accent(double now, bool dark, bool& animating);
    // window: the size to remember, unless the window is maximized.
    void save(std::optional<std::pair<int, int>> window = {});
    void config_changed();

    // Desktop lyrics, which App puts on a layer surface across the bottom of the screen: the surface height for a
    // font size, its distance from the bottom, and its content. Returns the part that takes the pointer.
    static float desktop_height(float font_size);
    int desktop_bottom() const { return int(std::lround(dl_bottom_)); }
    ui::Rect desktop_lyrics(ui::Ui& ui, float w, float h);

private:
    enum class Page { recommend, daily, toplist, square, fm, liked, recent, cloud, collection, playlist, album, artist, similar, search };
    // Tracks behind a page, loaded when it is entered.
    struct List {
        std::vector<Track> tracks;
        bool loading = false;
        std::string error;
        double since = -10;  // when the rows arrived, for the entrance
        AlbumInfo album;     // album pages: the details; the tracks are above
    };
    // A listing that loads a page at a time.
    struct Results {
        std::vector<Track> tracks;
        std::vector<Card> cards;
        int total = 0;
        bool loading = false, loaded = false;
        std::string error;
        double since = -10;
        size_t size() const { return tracks.size() + cards.size(); }
        bool more() const { return !loaded || int(size()) < total; }
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
        Card card;  // what a playlist-like page shows
        std::shared_ptr<List> list;
        std::shared_ptr<ArtistPage> artist;
    };
    // The comments dialog, hot and newest comments a page at a time.
    struct Feed {
        std::vector<Comment> comments;
        std::vector<float> heights;  // laid out for heights_w
        float heights_w = 0, heights_size = 0;
        int total = 0, page = 0;
        bool more = true, loading = false;
        std::string cursor, error;
        double since = -10;
    };
    enum class Mode { list, one, shuffle };

    // Pages and parts.
    void sidebar(ui::Ui& ui, ui::Rect r);
    void recommend_page(ui::Ui& ui, ui::Rect r);
    void toplist_page(ui::Ui& ui, ui::Rect r);
    void square_page(ui::Ui& ui, ui::Rect r);
    void fm_page(ui::Ui& ui, ui::Rect r);
    void collection_page(ui::Ui& ui, ui::Rect r);
    void artist_page(ui::Ui& ui, ui::Rect r);
    void search_page(ui::Ui& ui, ui::Rect r);
    void list_page(ui::Ui& ui, ui::Rect r, std::vector<Track>& tracks, bool loading, const std::string& error, double since);
    // since: when the list appeared, for the staggered entrance. Returns true when the last rows are visible.
    bool track_list(ui::Ui& ui, ui::Rect r, ui::Id id, std::vector<Track>& tracks, double since);
    void track_row(ui::Ui& ui, ui::Rect r, std::vector<Track>& tracks, int i);
    void track_menu(ui::Ui& ui, float x, float y, std::vector<Track>& tracks, int i);
    // Draws cards in columns from r's top-left; returns the clicked index or -1.
    int card_grid(ui::Ui& ui, ui::Rect r, ui::Id id, const std::vector<Card>& cards, bool round, double since);
    // card_grid in its own scrolling view; near_end tells when the last rows are close.
    int grid_view(ui::Ui& ui, ui::Rect r, ui::Id id, const std::vector<Card>& cards, bool round, double since, bool more, bool& near_end);
    bool card(ui::Ui& ui, ui::Id id, ui::Rect r, const Card& c, bool round);
    bool hero(ui::Ui& ui, ui::Id id, ui::Rect r, std::string_view big, std::string_view title, std::string_view subtitle, float hue);
    // Loading, error and signed-out states of a page; true when one was drawn.
    bool page_status(ui::Ui& ui, ui::Rect r, bool loading, const std::string& error, bool needs_login);
    void queue_panel(ui::Ui& ui, ui::Rect r);
    void player_bar(ui::Ui& ui, ui::Rect r);
    void now_playing(ui::Ui& ui, ui::Rect r, float k);
    void lyrics_view(ui::Ui& ui, ui::Rect r, bool centered);
    // The buttons after "play all" and "shuffle" on album and playlist pages, from x.
    void list_actions(ui::Ui& ui, float x, float y);
    void account_modal(ui::Ui& ui, ui::Rect r);
    void settings_modal(ui::Ui& ui, ui::Rect r);
    void categories_modal(ui::Ui& ui, ui::Rect r);
    void comments_modal(ui::Ui& ui, ui::Rect r);
    // Lays out a comment at y, or only measures it; returns its height.
    float comment(ui::Ui& ui, const Comment& c, float x, float y, float w, bool draw);
    void editor_modal(ui::Ui& ui, ui::Rect r, Card card);
    void picker_modal(ui::Ui& ui, ui::Rect r, Track t);
    void open_account(ui::Ui& ui);
    void open_comments(ui::Ui& ui, Subject subject, std::string id, std::string title);
    // A new playlist when card is empty, otherwise a rename.
    void open_editor(ui::Ui& ui, Card card = {});
    void confirm_delete(ui::Ui& ui, Card card);
    void handle_keys(ui::Ui& ui);

    // Drawing helpers.
    void cover(ui::Ui& ui, ui::Rect r, const std::string& url, float radius, std::string_view placeholder = "music");
    float cover_hue(const std::string& url, float size);
    float special_hue(Page page) const;

    // Navigation.
    void search(ui::Ui& ui);
    void load_results(int tab);
    void open(ui::Ui& ui, Page page, const Card& card = {});
    void open(ui::Ui& ui, const Card& card);  // the page for a card
    void back(ui::Ui& ui);
    void enter(ui::Ui& ui, Route route, int direction);
    static bool needs_login(Page p);
    void ensure_route_data();
    Task<void> load_list(std::shared_ptr<List> list, Page page, Card card);
    Task<void> fetch_results(uint64_t gen, int tab, std::string query, int offset);
    Task<void> load_artist(std::shared_ptr<ArtistPage> page, std::string id);
    void load_artist_tab(int tab);
    Task<void> fetch_artist(std::shared_ptr<ArtistPage> page, std::string id, int tab, int offset);
    Task<void> load_categories();
    void load_square();
    Task<void> fetch_square(uint64_t gen, std::string category, int offset);
    Task<void> load_collection();
    void load_comments(int tab);
    Task<void> fetch_comments(uint64_t gen, int tab, int page, std::string cursor);

    // Playback.
    void play(const std::vector<Track>& list, int i);
    void set_current(int i, bool replacing_queue = false);
    void toggle_play();
    void stop();
    void stop_player();  // reports how far the track got, then stops
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
    void publish();  // the player state for the media controls
    bool skip_trials() const { return config_.playback.trial == TrialMode::skip; }
    const Track* current() const { return current_ >= 0 ? &queue_[size_t(current_)] : nullptr; }
    bool is_current(const Track& t) const { return current() && same(*current(), t); }
    bool playing() const;
    Provider& provider(const Track&) { return ncm_; }

    // Library and account.
    Task<void> startup();
    Task<void> load_library();
    Task<void> load_recommended();
    Task<void> load_daily_lists();
    Task<void> load_toplists();
    Task<void> load_fm();
    void next_fm();
    Task<void> qr_login();
    Task<void> logout();
    void save_credentials();
    bool liked(const Track& t) const;
    void toggle_like(Track t);
    void dislike_daily(std::shared_ptr<List> list, Track t);
    // Plays the heartbeat list that follows seed.
    Task<void> heartbeat(Track seed);
    void toggle_follow(std::shared_ptr<ArtistPage> page, Card artist);
    void toggle_collect(std::shared_ptr<List> list, Card album);
    void add_to_playlist(Track t, Card playlist);
    void remove_from_playlist(std::shared_ptr<List> list, Card playlist, Track t);
    void recount(const std::string& playlist, int delta);  // the sidebar's track count
    void retitle(const std::string& playlist, const std::string& title);
    void set_category(ui::Ui& ui, std::string category);
    void notify(std::string text, std::string icon = "info", bool error = false);
    void changed();  // persisted state changed

    Loop& loop_;
    net::Http& http_;
    audio::Player& player_;
    ncm::Netease& ncm_;
    Images& images_;
    const Config& config_;
    std::function<void(std::string_view, std::string_view, std::string_view)> set_setting_;
    std::function<void()> redraw_;
    std::function<double()> now_;
    bool silent_;

    Route route_{Page::recommend};
    std::vector<Route> history_;
    double page_time_ = -10, tab_time_ = -10;
    int page_dir_ = 1, tab_dir_ = 1;
    std::string today_;
    struct Toast {
        std::string text, icon;
        bool error;
    };
    std::vector<Toast> toasts_;

    // Library.
    std::optional<Account> account_;
    bool creds_loaded_ = false, can_save_creds_ = false;
    Loop::TimerId creds_timer_ = 0;
    std::vector<Card> playlists_, recommended_, toplists_, daily_lists_;
    double daily_lists_since_ = -10;
    Card liked_card_;
    bool recommended_loading_ = false, toplists_loading_ = false;
    std::string recommended_error_, toplists_error_;
    std::unordered_set<std::string> liked_ids_;
    std::vector<Track> recent_, fm_;
    int fm_pos_ = 0;
    int recent_tab_ = 0;  // this machine's plays, or the account's
    bool fm_loading_ = false;
    std::string fm_error_;
    ncm::Categories categories_;
    bool categories_loading_ = false;
    std::string square_category_ = "全部";
    Results square_;
    uint64_t square_gen_ = 0;
    double square_time_ = -10;
    Results collection_[2];  // albums, artists
    int collection_tab_ = 0;
    double collection_time_ = -10;

    // Search.
    std::string query_, searched_;
    int search_tab_ = 0;
    Results results_[4];
    uint64_t search_gen_ = 0;
    int selected_ = -1;

    // Playback.
    std::vector<Track> queue_;
    int current_ = -1;
    uint64_t play_gen_ = 0;
    audio::Player::State last_state_ = audio::Player::State::idle;
    bool want_play_ = false, fetching_ = false, fm_playing_ = false;
    bool queue_open_ = false, muted_ = false, retried_ = false, counted_ = false;
    size_t failures_ = 0;
    Mode mode_ = Mode::list;
    std::vector<int> shuffle_history_;
    int shuffle_next_ = -1;
    double resume_at_ = 0, last_pos_ = 0;
    float volume_ = 0.7f, progress_w_ = 400;
    bool dragging_ = false;
    float drag_v_ = 0;
    struct Prefetched {
        std::string id;
        std::shared_ptr<audio::Spool> spool;
        uint64_t gen = 0;
        Stream stream;
    } prefetch_;
    Loop::TimerId prefetch_timer_ = 0;
    float track_gain_ = 0, track_peak_ = 0;  // loudness of the current stream
    bool trial_ = false;  // the current stream is a preview clip
    std::optional<Track> listen_;  // the track whose listen is reported to the account
    uint64_t rng_ = 0x2545f4914f6cdd1dull;
    Loop::TimerId save_timer_ = 0;
    std::optional<std::pair<int, int>> window_;
    std::string prev_cover_;
    double cover_time_ = -10;

    std::unique_ptr<Mpris> mpris_;

    // Lyrics and the now-playing view.
    std::vector<LyricLine> lyrics_;
    std::string lyrics_id_;
    double clip_start_ = 0;  // where a trial clip sits in the song the lyrics are timed for
    bool lyrics_loading_ = false;
    bool np_open_ = false;
    double np_time_ = -10;
    struct LyricBlock {
        std::vector<std::string_view> lines, translation;  // wrapped, pointing into lyrics_
        float y = 0, h = 0;
    };
    std::vector<LyricBlock> lyric_layout_;
    float lyric_layout_w_ = 0, lyric_layout_size_ = 0;
    std::string lyric_layout_font_;
    float lyric_y_ = 0;
    double lyric_manual_until_ = 0, lyric_last_ = 0;

    // Desktop lyrics: offset from the screen's centre, height above the bottom, click-through; the line shown and
    // the one fading out.
    struct ShownLine {
        std::string text, sub;
        bool timed = false;
    };
    float dl_x_ = 0, dl_bottom_ = 48;
    bool dl_locked_ = false, dl_dragging_ = false;
    float dl_grab_x_ = 0, dl_grab_y_ = 0, dl_last_y_ = 0;
    ShownLine dl_line_, dl_prev_;
    double dl_changed_ = -10;

    // Accent.
    bool dark_ = true;
    float accent_hue_ = -2;
    ui::Color accent_from_{}, accent_to_{};
    double accent_time_ = -10;
    float bar_cover_px_ = 64;

    // Account modal.
    int login_step_ = -1;  // -1 account list, otherwise the service being scanned
    double login_time_ = -10;
    uint64_t qr_gen_ = 0;
    bool qr_active_ = false;
    std::vector<uint8_t> qr_;  // modules, row by row
    int qr_size_ = 0;
    std::optional<QrStatus> qr_status_;
    std::string qr_error_;

    // Comments dialog and the playlist editor.
    Subject thread_subject_ = Subject::song;
    std::string thread_id_, thread_title_;
    int thread_tab_ = 0;
    double thread_time_ = -10;
    Feed feeds_[2];  // hot, newest
    uint64_t thread_gen_ = 0;
    std::string edit_name_;
    bool edit_hidden_ = false, edit_focus_ = false;
};

}  // namespace lyren
