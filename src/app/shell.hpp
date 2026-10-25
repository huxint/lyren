#pragma once
#include <string>
#include <unordered_set>
#include <vector>

#include "ui/ui.hpp"

namespace lyren {

enum class Source : uint8_t { netease, qq };

struct Track {
    Source source;
    std::string title, artist, album;
    int duration;  // seconds
    float hue;     // placeholder cover colour
};

// An album, artist or playlist tile.
struct Card {
    std::string title, subtitle;
    float hue = 0;
    int count = 0;  // tracks behind it
};

// Window content: search and account in the top bar, sidebar, pages, queue and the player bar. Runs on
// generated data until the sources are connected.
class Shell {
public:
    static constexpr float player_h = 84;

    Shell();
    // bar is the top strip beside the window controls; the search box centres on center_x.
    void top_bar(ui::Ui& ui, ui::Rect bar, float center_x);
    void draw(ui::Ui& ui, ui::Rect area);
    // Accent taken from the current cover, cross-faded after a track change.
    ui::Color accent(double now, bool dark, bool& animating) const;

private:
    enum class Page { recommend, daily, toplist, fm, liked, recent, cloud, playlist, search };
    struct Route {
        Page page;
        Card card;  // what a playlist-like page shows
        int playlist = -1;  // sidebar entry, if it came from there
    };

    void sidebar(ui::Ui& ui, ui::Rect r);
    void recommend_page(ui::Ui& ui, ui::Rect r);
    void toplist_page(ui::Ui& ui, ui::Rect r);
    void fm_page(ui::Ui& ui, ui::Rect r);
    void search_page(ui::Ui& ui, ui::Rect r);
    void list_page(ui::Ui& ui, ui::Rect r, std::vector<Track>& tracks);
    // since: when the list appeared, for the staggered entrance.
    void track_list(ui::Ui& ui, ui::Rect r, ui::Id id, std::vector<Track>& tracks, double since);
    void track_row(ui::Ui& ui, ui::Rect r, std::vector<Track>& tracks, int i);
    void track_menu(ui::Ui& ui, float x, float y, std::vector<Track>& tracks, int i);
    // Draws cards in columns from r's top-left; returns the clicked index or -1.
    int card_grid(ui::Ui& ui, ui::Rect r, ui::Id id, const std::vector<Card>& cards, bool round, double since);
    bool card(ui::Ui& ui, ui::Id id, ui::Rect r, const Card& c, bool round);
    bool hero(ui::Ui& ui, ui::Id id, ui::Rect r, std::string_view big, std::string_view title, std::string_view subtitle, float hue);
    void queue_panel(ui::Ui& ui, ui::Rect r);
    void player_bar(ui::Ui& ui, ui::Rect r);
    void account_modal(ui::Ui& ui, ui::Rect r);
    void settings_modal(ui::Ui& ui, ui::Rect r);
    void handle_keys(ui::Ui& ui);

    void search(ui::Ui& ui);
    void open(ui::Ui& ui, Page page, const Card& card = {}, int playlist = -1);
    void back(ui::Ui& ui);
    void enter(ui::Ui& ui, const Route& route, int direction);
    void play(const std::vector<Track>& list, int i);
    void skip(int delta);
    void advance(double dt);
    bool liked(const Track& t) const;
    void toggle_like(ui::Ui& ui, Track t);
    const Track* current() const { return current_ >= 0 ? &queue_[size_t(current_)] : nullptr; }

    Route route_{Page::recommend};
    std::vector<Route> history_;
    double page_time_ = -10, tab_time_ = -10;
    int page_dir_ = 1, tab_dir_ = 1;
    std::string today_;
    std::vector<Track> page_tracks_, liked_, recent_, fm_;
    std::unordered_set<std::string> liked_keys_;
    std::vector<Card> playlists_, recommended_, toplists_;
    int fm_pos_ = 0;

    std::string query_, searched_;
    int search_tab_ = 0;
    std::vector<Track> results_;
    std::vector<Card> albums_, artists_, result_lists_;
    int selected_ = -1;

    std::vector<Track> queue_;
    int current_ = -1;
    bool playing_ = false, queue_open_ = false, muted_ = false;
    int mode_ = 0;  // 0 repeat list, 1 repeat one, 2 shuffle
    double position_ = 0, last_now_ = 0;
    float volume_ = 0.7f, progress_w_ = 400;
    uint64_t rng_ = 0x2545f4914f6cdd1dull;
    bool dark_ = true;
    ui::Color accent_from_{};
    double accent_time_ = -10, cover_time_ = -10;
    float prev_hue_ = -1;

    int login_step_ = -1;  // -1 account list, otherwise the service being scanned
    double login_time_ = -10;
    uint64_t qr_seed_ = 1;
    int quality_ = 3;
    bool desktop_lyrics_ = false, skip_trials_ = true, normalize_ = false;
};

}  // namespace lyren
