#pragma once
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
    std::function<void()> redraw;  // async results arrived
    std::function<double()> now;   // the UI clock
    bool silent;                   // scripted runs: the stream volume stays 0
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

private:
    enum class Page { recommend, daily, toplist, fm, liked, recent, cloud, playlist, search };
    // Tracks behind a page, loaded when it is entered.
    struct List {
        std::vector<Track> tracks;
        bool loading = false;
        std::string error;
        double since = -10;  // when the rows arrived, for the entrance
    };
    struct Route {
        Page page;
        Card card;  // what a playlist-like page shows
        int playlist = -1;  // sidebar entry, if it came from there
        std::shared_ptr<List> list;
    };
    struct Results {
        std::vector<Track> tracks;
        std::vector<Card> cards;
        int total = 0;
        bool loading = false, loaded = false;
        std::string error;
        double since = -10;
    };
    enum class Mode { list, one, shuffle };

    // Pages and parts.
    void sidebar(ui::Ui& ui, ui::Rect r);
    void recommend_page(ui::Ui& ui, ui::Rect r);
    void toplist_page(ui::Ui& ui, ui::Rect r);
    void fm_page(ui::Ui& ui, ui::Rect r);
    void search_page(ui::Ui& ui, ui::Rect r);
    void list_page(ui::Ui& ui, ui::Rect r, std::vector<Track>& tracks, bool loading, const std::string& error, double since);
    // since: when the list appeared, for the staggered entrance. Returns true when the last rows are visible.
    bool track_list(ui::Ui& ui, ui::Rect r, ui::Id id, std::vector<Track>& tracks, double since);
    void track_row(ui::Ui& ui, ui::Rect r, std::vector<Track>& tracks, int i);
    void track_menu(ui::Ui& ui, float x, float y, std::vector<Track>& tracks, int i);
    // Draws cards in columns from r's top-left; returns the clicked index or -1.
    int card_grid(ui::Ui& ui, ui::Rect r, ui::Id id, const std::vector<Card>& cards, bool round, double since);
    bool card(ui::Ui& ui, ui::Id id, ui::Rect r, const Card& c, bool round);
    bool hero(ui::Ui& ui, ui::Id id, ui::Rect r, std::string_view big, std::string_view title, std::string_view subtitle, float hue);
    // Loading, error and signed-out states of a page; true when one was drawn.
    bool page_status(ui::Ui& ui, ui::Rect r, bool loading, const std::string& error, bool needs_login);
    void queue_panel(ui::Ui& ui, ui::Rect r);
    void player_bar(ui::Ui& ui, ui::Rect r);
    void now_playing(ui::Ui& ui, ui::Rect r, float k);
    void lyrics_view(ui::Ui& ui, ui::Rect r, bool centered);
    void account_modal(ui::Ui& ui, ui::Rect r);
    void settings_modal(ui::Ui& ui, ui::Rect r);
    void open_account(ui::Ui& ui);
    void handle_keys(ui::Ui& ui);

    // Drawing helpers.
    void cover(ui::Ui& ui, ui::Rect r, const std::string& url, float radius, std::string_view placeholder = "music");
    float cover_hue(const std::string& url, float size);
    float special_hue(Page page) const;

    // Navigation.
    void search(ui::Ui& ui);
    void load_results(int tab);
    void open(ui::Ui& ui, Page page, const Card& card = {}, int playlist = -1);
    void back(ui::Ui& ui);
    void enter(ui::Ui& ui, Route route, int direction);
    static bool needs_login(Page p);
    void ensure_route_data();
    Task<void> load_list(std::shared_ptr<List> list, Page page, Card card);
    Task<void> fetch_results(uint64_t gen, int tab, std::string query, int offset);

    // Playback.
    void play(const std::vector<Track>& list, int i);
    void set_current(int i, bool replacing_queue = false);
    void toggle_play();
    void skip(int delta, bool automatic = false);
    int next_index();
    void start(double at = 0, bool play = true);
    Task<void> load_track(uint64_t gen, Track t, double at);
    Task<void> load_lyrics(Track t);
    Task<void> prefetch(Track t);
    void on_player();
    void apply_volume();
    const Track* current() const { return current_ >= 0 ? &queue_[size_t(current_)] : nullptr; }
    bool is_current(const Track& t) const { return current() && same(*current(), t); }
    bool playing() const;
    Provider& provider(const Track&) { return ncm_; }

    // Library and account.
    Task<void> startup();
    Task<void> load_library();
    Task<void> load_recommended();
    Task<void> load_toplists();
    Task<void> load_fm();
    void next_fm();
    Task<void> qr_login();
    Task<void> logout();
    void save_credentials();
    bool liked(const Track& t) const;
    void toggle_like(Track t);
    void notify(std::string text, std::string icon = "info", bool error = false);
    void changed();  // persisted state changed

    Loop& loop_;
    net::Http& http_;
    audio::Player& player_;
    ncm::Netease& ncm_;
    Images& images_;
    const Config& config_;
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
    std::vector<Card> playlists_, recommended_, toplists_;
    Card liked_card_;
    bool recommended_loading_ = false, toplists_loading_ = false;
    std::string recommended_error_, toplists_error_;
    std::unordered_set<std::string> liked_ids_;
    std::vector<Track> recent_, fm_;
    int fm_pos_ = 0;
    bool fm_loading_ = false;
    std::string fm_error_;

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
        double clip_start = 0;
    } prefetch_;
    uint64_t rng_ = 0x2545f4914f6cdd1dull;
    Loop::TimerId save_timer_ = 0;
    std::optional<std::pair<int, int>> window_;
    std::string prev_cover_;
    double cover_time_ = -10;

    // Lyrics and the now-playing view.
    std::vector<LyricLine> lyrics_;
    std::string lyrics_id_;
    double clip_start_ = 0;  // where a trial clip sits in the song the lyrics are timed for
    bool lyrics_loading_ = false;
    bool np_open_ = false;
    double np_time_ = -10;
    struct LyricBlock {
        std::vector<std::string> lines, translation;  // wrapped
        float y = 0, h = 0;
    };
    std::vector<LyricBlock> lyric_layout_;
    float lyric_layout_w_ = 0, lyric_layout_size_ = 0;
    std::string lyric_layout_font_;
    float lyric_y_ = 0;
    double lyric_manual_until_ = 0, lyric_last_ = 0;

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

    // Settings (written to the config file from M5 on; desktop lyrics and loudness come with M5).
    int quality_;
    bool skip_trials_;
    bool desktop_lyrics_ = false, normalize_ = false;
};

}  // namespace lyren
