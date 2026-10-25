#include "app/shell.hpp"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>

namespace lyren {

using ui::Align;
using ui::Rect;
using ui::TextStyle;
namespace ease = ui::ease;

constexpr ui::Color white{1, 1, 1, 1};

// Generated catalogue.

static constexpr const char* titles[] = {
    "晴天", "夜曲", "稻香", "七里香", "告白气球", "青花瓷", "以父之名", "简单爱", "说好不哭", "Mojito", "起风了", "光年之外", "后来",
    "十年", "富士山下", "红豆", "小幸运", "平凡之路", "海阔天空", "成都", "Shape of You", "Yesterday Once More", "Hotel California",
    "Bohemian Rhapsody", "夜に駆ける", "Lemon", "打上花火", "Dynamite", "いつも何度でも", "See You Again", "Viva La Vida",
    "Fly Me to the Moon", "一个很长很长的歌名，用来看看列表里的省略号 (Extended Club Mix)", "演员", "消愁", "像我这样的人", "年少有为",
    "倒数", "泡沫", "孤勇者", "如愿", "达尔文", "Someone Like You", "Anti-Hero", "Yellow", "群青", "アイドル", "봄날", "Ελπίδα"};
static constexpr const char* artists[] = {"周杰伦", "林俊杰", "陈奕迅", "邓紫棋", "王菲", "朴树", "毛不易", "李荣浩", "Taylor Swift",
                                          "Coldplay", "YOASOBI", "米津玄師", "BTS", "Adele", "Eagles", "Queen", "久石譲", "许嵩",
                                          "周深", "五月天", "薛之谦", "蔡健雅"};
static constexpr const char* albums[] = {"叶惠美", "范特西", "十一月的萧邦", "我很忙", "魔杰座", "U87", "寓言", "Midnights",
                                         "Parachutes", "THE BOOK", "STRAY SHEEP", "25", "Hotel California", "A Night at the Opera",
                                         "千と千尋の神隠し サウンドトラック", "平凡之路", "如也", "新的心跳", "绅士", "Viva la Vida"};
static constexpr const char* playlist_names[] = {
    "夜跑 · 160 BPM", "华语经典 100 首", "写代码时听的纯音乐", "周末的咖啡馆", "日系 City Pop", "通勤路上", "睡前 · 轻音乐",
    "2010 年代 KTV 必点", "Lo-fi Beats to Relax", "老歌 · 一听就是青春", "电影原声精选", "摇滚不死", "我的私藏", "国风 · 古风",
    "冬天的歌", "Jazz for Rainy Days"};
static constexpr const char* toplist_names[] = {"飙升榜", "新歌榜", "原创榜", "热歌榜", "流行指数榜", "说唱榜",
                                                "ACG 榜", "欧美榜", "日语榜", "韩语榜", "电音榜", "古典榜"};

static uint64_t next(uint64_t& s) {
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

template <size_t N>
static const char* pick(const char* const (&a)[N], uint64_t v) {
    return a[v % N];
}

static std::vector<Track> fake_tracks(std::string_view seed, int n) {
    uint64_t s = std::hash<std::string_view>{}(seed);
    std::vector<Track> out;
    out.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        uint64_t a = next(s), b = next(s);
        Track t{.source = (a >> 48) % 5 < 3 ? Source::netease : Source::qq,
                .title = pick(titles, a),
                .artist = pick(artists, a >> 32),
                .album = pick(albums, b),
                .duration = 150 + int((b >> 16) % 200),
                .hue = float((a >> 8) % 1000) / 1000};
        if (((a >> 20) & 7) == 0) t.title += " (Live)";
        if (((b >> 8) & 15) == 0) t.artist = std::format("{} / {}", t.artist, pick(artists, b >> 40));
        out.push_back(std::move(t));
    }
    return out;
}

enum class CardKind { playlist, album, artist };

static std::vector<Card> fake_cards(std::string_view seed, int n, CardKind kind) {
    uint64_t s = std::hash<std::string_view>{}(seed);
    uint64_t first = next(s);  // consecutive names, so a grid has no repeats
    std::vector<Card> out;
    for (int i = 0; i < n; ++i) {
        uint64_t a = next(s);
        Card c{.hue = float(a % 1000) / 1000};
        switch (kind) {
            case CardKind::playlist:
                c.title = pick(playlist_names, first + uint64_t(i));
                c.count = 20 + int((a >> 24) % 180);
                c.subtitle = std::format("{} 首", c.count);
                break;
            case CardKind::album:
                c.title = pick(albums, first + uint64_t(i));
                c.count = 8 + int((a >> 24) % 8);
                c.subtitle = std::format("{} · {}", pick(artists, a >> 32), 1998 + (a >> 50) % 27);
                break;
            case CardKind::artist:
                c.title = pick(artists, first + uint64_t(i));
                c.count = 50;
                c.subtitle = std::format("单曲 {}", 40 + (a >> 24) % 600);
                break;
        }
        out.push_back(std::move(c));
    }
    return out;
}

static std::string key_of(const Track& t) { return t.title + '\x1f' + t.artist; }

// Drawing helpers.

static ui::Color hsl(float h, float s, float l) {
    h -= std::floor(h);
    auto f = [&](float n) {
        float k = std::fmod(n + h * 12, 12.f);
        return l - s * std::min(l, 1 - l) * std::max(-1.f, std::min({k - 3, 9 - k, 1.f}));
    };
    return {f(0), f(8), f(4), 1};
}

static std::string mmss(double seconds) {
    int v = int(seconds);
    return std::format("{}:{:02}", v / 60, v % 60);
}

// Stand-in cover art: a gradient with two soft discs placed by the hue's digits.
static void cover(ui::Painter& p, Rect r, float hue, float radius, std::string_view icon = {}) {
    p.gradient(r, hsl(hue, 0.42f, 0.54f), hsl(hue + 0.08f, 0.4f, 0.22f), radius);
    if (radius < r.w / 2 - 1) {
        float a = std::fmod(hue * 7.31f, 1.f), b = std::fmod(hue * 13.7f, 1.f);
        float d1 = r.w * (0.5f + 0.35f * a), d2 = r.w * (0.3f + 0.25f * b);
        p.rect({r.x + (r.w - d1) * b, r.y + (r.h - d1) * (1 - a), d1, d1}, hsl(hue + 0.1f, 0.5f, 0.72f).with_alpha(0.26f), d1 / 2);
        p.rect({r.x + (r.w - d2) * (1 - a), r.y + (r.h - d2) * b, d2, d2}, hsl(hue - 0.12f, 0.55f, 0.62f).with_alpha(0.42f), d2 / 2);
    }
    if (icon.empty()) return;
    float s = std::round(r.w * 0.36f);
    p.icon(icon, ui::center(r, s, s), white.with_alpha(0.85f));
}

// Multiplies the painter's alpha for one scope.
struct Fade {
    ui::Painter& p;
    float saved;
    Fade(ui::Painter& painter, float a) : p(painter), saved(painter.alpha()) { p.set_alpha(saved * a); }
    ~Fade() { p.set_alpha(saved); }
};

// A 25×25 code with real finder patterns and noise for data.
static void fake_qr(ui::Painter& p, Rect r, uint64_t seed) {
    constexpr int n = 25;
    p.rect(r, white, 14);
    Rect q = r.inset(14);
    float m = q.w / n;
    auto finder = [](int x, int y) {
        int d = std::max(std::abs(x - 3), std::abs(y - 3));
        return d != 2 && d <= 3;
    };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            bool on;
            if (x < 8 && y < 8) on = finder(x, y);
            else if (x >= n - 8 && y < 8) on = finder(x - (n - 7), y);
            else if (x < 8 && y >= n - 8) on = finder(x, y - (n - 7));
            else {
                uint64_t s = seed * 1000 + uint64_t(y * n + x);
                on = next(s) & 1;
            }
            if (on) p.rect({q.x + float(x) * m, q.y + float(y) * m, m, m}, {0.07f, 0.07f, 0.08f, 1});
        }
}

struct Columns {
    Rect index, title, album, like, duration;
};

static Columns columns(Rect r) {
    Columns c;
    r = r.inset(12, 0);
    c.index = ui::cut_left(r, 40);
    ui::cut_left(r, 4);
    c.duration = ui::cut_right(r, 48);
    c.like = ui::cut_right(r, 44);
    if (r.w > 460) {
        c.album = ui::cut_right(r, std::round(r.w * 0.36f));
        ui::cut_right(r, 16);
    }
    c.title = r;
    return c;
}

// Equal columns of at least min_w.
struct Grid {
    static constexpr float gap = 20;
    int cols;
    float cw, ch;
    explicit Grid(float width, float min_w = 150) {
        cols = std::max(1, int((width + gap) / (min_w + gap)));
        cw = (width - gap * float(cols - 1)) / float(cols);
        ch = cw + 54;
    }
    float height(size_t n) const {
        size_t rows = (n + size_t(cols) - 1) / size_t(cols);
        return rows ? float(rows) * ch + float(rows - 1) * gap : 0;
    }
    Rect cell(float x, float y, size_t i) const {
        return {x + float(i % size_t(cols)) * (cw + gap), y + float(i / size_t(cols)) * (ch + gap), cw, ch};
    }
};

static void page_title(ui::Ui& ui, Rect body, float y, std::string_view title, std::string_view subtitle) {
    const auto& t = ui.theme();
    ui.label({body.x, y, body.w, 38}, title, {.size = t.fs(28), .color = t.text});
    ui.label({body.x, y + 42, body.w, 20}, subtitle, {.size = t.fs(13), .color = t.text_dim});
}

// State.

Shell::Shell() {
    std::time_t now = std::time(nullptr);
    std::tm lt;
    localtime_r(&now, &lt);
    today_ = std::format("{} 月 {} 日", lt.tm_mon + 1, lt.tm_mday);

    playlists_ = fake_cards("my playlists", 14, CardKind::playlist);
    recommended_ = fake_cards("recommended", 12, CardKind::playlist);
    for (size_t i = 0; i < std::size(toplist_names); ++i)
        toplists_.push_back({toplist_names[i], i % 3 ? "每日更新" : "每周四更新", float(i) * 0.083f, 100});
    liked_ = fake_tracks("liked", 24);
    for (auto& t : liked_) liked_keys_.insert(key_of(t));
    recent_ = fake_tracks("recent", 30);
    fm_ = fake_tracks("fm", 50);
    queue_ = fake_tracks("daily", 30);
    current_ = 0;
    position_ = 83;
}

ui::Color Shell::accent(double now, bool dark, bool& animating) const {
    float hue = current() ? current()->hue : 0.98f;
    ui::Color to = hsl(hue, 0.62f, dark ? 0.64f : 0.46f);
    float t = float((now - accent_time_) / 0.8);
    if (t >= 1) return to;
    animating = true;
    return accent_from_.mix(to, ease::standard(t));
}

bool Shell::liked(const Track& t) const { return liked_keys_.contains(key_of(t)); }

void Shell::toggle_like(ui::Ui& ui, Track t) {
    std::string key = key_of(t);
    if (liked_keys_.erase(key)) {
        std::erase_if(liked_, [&](const Track& x) { return key_of(x) == key; });
        ui.toast("已取消喜欢", "heart-off");
    } else {
        liked_keys_.insert(key);
        liked_.insert(liked_.begin(), std::move(t));
        ui.toast("已添加到我喜欢的音乐", "heart-fill");
    }
}

void Shell::play(const std::vector<Track>& list, int i) {
    if (i < 0 || i >= int(list.size())) return;
    bool unused = false;
    accent_from_ = accent(last_now_, dark_, unused);
    accent_time_ = cover_time_ = last_now_;
    prev_hue_ = current() ? current()->hue : -1;
    if (&list != &queue_) queue_ = list;
    current_ = i;
    position_ = 0;
    playing_ = true;
    const Track& t = queue_[size_t(i)];
    std::string key = key_of(t);
    std::erase_if(recent_, [&](const Track& x) { return key_of(x) == key; });
    recent_.insert(recent_.begin(), t);
    if (recent_.size() > 100) recent_.pop_back();
}

void Shell::skip(int delta) {
    if (queue_.empty()) return;
    int n = int(queue_.size());
    int i = mode_ == 2 ? int(next(rng_) % uint64_t(n)) : ((current_ + delta) % n + n) % n;
    play(queue_, i);
}

void Shell::advance(double dt) {
    if (!playing_ || !current()) return;
    position_ += dt;
    if (position_ < current()->duration) return;
    if (mode_ == 1) position_ = 0;
    else skip(1);
}

void Shell::search(ui::Ui& ui) {
    if (query_.empty()) return;
    searched_ = query_;
    results_ = fake_tracks(query_, 500);
    albums_ = fake_cards(query_ + "/albums", 24, CardKind::album);
    artists_ = fake_cards(query_ + "/artists", 12, CardKind::artist);
    result_lists_ = fake_cards(query_ + "/playlists", 24, CardKind::playlist);
    search_tab_ = 0;
    for (const char* id : {"search-list", "search-albums", "search-artists", "search-playlists"}) ui.scroll_to(ui.id(id), 0);
    open(ui, Page::search);
}

void Shell::open(ui::Ui& ui, Page page, const Card& card, int playlist) {
    if (route_.page == page && route_.card.title == card.title && page != Page::search) return;
    history_.push_back(route_);
    if (history_.size() > 50) history_.erase(history_.begin());
    enter(ui, {page, card, playlist}, 1);
}

void Shell::back(ui::Ui& ui) {
    if (history_.empty()) return;
    Route r = std::move(history_.back());
    history_.pop_back();
    enter(ui, r, -1);
}

void Shell::enter(ui::Ui& ui, const Route& route, int direction) {
    route_ = route;
    page_time_ = ui.now;
    page_dir_ = direction;
    selected_ = -1;
    ui.scroll_to(ui.id("tracks"), 0);
    switch (route.page) {
        case Page::daily:
            route_.card = {"每日推荐", std::format("{} · 根据你的口味生成", today_), 0.98f, 30};
            page_tracks_ = fake_tracks("daily", 30);
            break;
        case Page::cloud:
            route_.card = {"我的云盘", "上传到云端的歌曲", 0.58f, 42};
            page_tracks_ = fake_tracks("cloud", 42);
            break;
        case Page::liked: route_.card = {"我喜欢的音乐", "", 0.97f}; break;
        case Page::recent: route_.card = {"最近播放", "", 0.72f}; break;
        case Page::playlist: page_tracks_ = fake_tracks(route.card.title, route.card.count); break;
        default: break;
    }
}

// Layout.

void Shell::top_bar(ui::Ui& ui, Rect bar, float center_x) {
    Rect r = bar;
    Rect nav = ui::cut_left(r, 40);
    {
        Fade fade(ui.painter(), ui.anim(ui.id("back-a"), history_.empty() ? 0.3f : 1.f, 200));
        if (ui.icon_button(ui.id("back"), ui::center(nav, 32, 32), "chevron-left", 20)) back(ui);
    }
    Rect settings = ui::cut_right(r, 40);
    if (ui.icon_button(ui.id("settings"), ui::center(settings, 32, 32), "settings", 18))
        ui.open_modal(460, 384, [this](ui::Ui& u, Rect body) { settings_modal(u, body); });
    Rect account = ui::cut_right(r, 40);
    if (ui.icon_button(ui.id("account"), ui::center(account, 32, 32), "user", 18)) {
        login_step_ = -1;
        ui.open_modal(420, 330, [this](ui::Ui& u, Rect body) { account_modal(u, body); });
    }
    float w = std::min(440.f, r.w - 32);
    float x = std::clamp(center_x - w / 2, r.x + 16, r.right() - 16 - w);
    if (ui.text_box(ui.id("search"), {x, bar.y + (bar.h - 34) / 2, w, 34}, query_, "搜索歌曲、歌手、专辑", "search")) search(ui);
}

void Shell::draw(ui::Ui& ui, Rect area) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    advance(ui.now - last_now_);
    last_now_ = ui.now;
    dark_ = t.dark;

    Rect player = ui::cut_bottom(area, player_h);
    // Below 900 px the sidebar folds into an icon rail.
    float side_w = ui.anim(ui.id("side-w"), area.w < 900 ? 72 : 220, 360, ease::emphasized);
    Rect side = ui::cut_left(area, side_w);
    ui::cut_right(area, 8);
    float qw = ui.anim(ui.id("queue-w"), queue_open_ ? 328 : 0, 380, ease::emphasized);
    if (qw > 0.5f) {
        Rect q = ui::cut_right(area, qw);
        ui.push_clip(q);
        queue_panel(ui, {q.x + 8, q.y, 320, q.h});
        ui.pop_clip();
    }

    // Content panel with a wash of the page's colour; the page slides in from the side it was reached from.
    p.rect(area, t.surface, t.radius_l());
    float enter = ui.progress(page_time_, 420);
    bool tinted = route_.page != Page::search;
    ui::Color tint = route_.page == Page::recommend || route_.page == Page::toplist || route_.page == Page::fm
                         ? t.accent
                         : hsl(route_.card.hue, 0.55f, 0.5f);
    if (tinted) p.gradient({area.x, area.y, area.w, 320}, tint.with_alpha(0.16f * enter), tint.with_alpha(0), t.radius_l());
    ui.push_clip(area);
    {
        Fade fade(p, enter);
        Rect r = area;
        r.x += (1 - enter) * 28 * float(page_dir_);
        switch (route_.page) {
            case Page::recommend: recommend_page(ui, r); break;
            case Page::toplist: toplist_page(ui, r); break;
            case Page::fm: fm_page(ui, r); break;
            case Page::search: search_page(ui, r); break;
            case Page::liked:
                route_.card.subtitle = std::format("{} 首", liked_.size());
                list_page(ui, r, liked_);
                break;
            case Page::recent:
                route_.card.subtitle = std::format("{} 首", recent_.size());
                list_page(ui, r, recent_);
                break;
            default: list_page(ui, r, page_tracks_); break;
        }
    }
    ui.pop_clip();
    sidebar(ui, side);
    player_bar(ui, player);
    handle_keys(ui);

    // Next frame when the progress bar moves a pixel or the time label changes.
    if (playing_ && current()) {
        double dur = current()->duration;
        double step = std::min(dur / std::max(progress_w_, 1.f), 1 - std::fmod(position_, 1.0));
        ui.wake(ui.now + std::max(step, 0.02));
    }
}

void Shell::handle_keys(ui::Ui& ui) {
    if (ui.modal_open()) return;
    for (auto& a : ui.input().actions)
        if (a.press && !a.consumed && a.button == BTN_SIDE) {
            a.consumed = true;
            back(ui);
            ui.redraw();
        }
    auto& keys = ui.input().keys;
    for (auto k = keys.begin(); k != keys.end();) {
        bool ctrl = k->mods & wl::mod_ctrl, alt = k->mods & wl::mod_alt, used = true;
        if (k->keysym == XKB_KEY_space && !k->repeat) playing_ = current() && !playing_;
        else if (ctrl && (k->keysym == XKB_KEY_f || k->keysym == XKB_KEY_F)) ui.focus = ui.id("search");
        else if (ctrl && k->keysym == XKB_KEY_Right) skip(1);
        else if (ctrl && k->keysym == XKB_KEY_Left) skip(-1);
        else if (alt && k->keysym == XKB_KEY_Left) back(ui);
        else used = false;
        if (used) {
            k = keys.erase(k);
            ui.redraw();
        } else {
            ++k;
        }
    }
}

void Shell::sidebar(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    struct Nav {
        Page page;
        const char* icon;
        const char* label;
    };
    static constexpr Nav nav[] = {{Page::recommend, "compass", "推荐"},     {Page::toplist, "list-ordered", "排行榜"},
                                  {Page::fm, "radio", "私人 FM"},           {Page::liked, "heart", "我喜欢的音乐"},
                                  {Page::recent, "history", "最近播放"},    {Page::cloud, "cloud", "我的云盘"}};
    constexpr float item_h = 38, step = 40, head_h = 36;
    // Labels fade as the sidebar folds into the rail.
    float labels = std::clamp((r.w - 72) / 148, 0.f, 1.f);
    auto item_y = [&](size_t k) { return 8 + head_h * float(1 + (k >= 3) + (k >= 6)) + step * float(k); };

    int sel = -1;
    for (size_t k = 0; k < std::size(nav); ++k)
        if (nav[k].page == route_.page || (route_.page == Page::daily && k == 0)) sel = int(k);
    if (route_.page == Page::playlist && route_.playlist >= 0) sel = 6 + route_.playlist;

    float content = item_y(6 + playlists_.size()) + 8;
    float off = ui.begin_scroll(ui.id("sidebar"), r, content);
    Rect row0{r.x + 12, 0, r.w - 24, item_h};

    // One highlight glides to the selected entry.
    float hy = ui.anim(ui.id("side-sel"), sel >= 0 ? item_y(size_t(sel)) : 0, 420, ease::emphasized);
    float ha = ui.anim(ui.id("side-sel-a"), sel >= 0 ? 1 : 0, 200);
    if (ha > 0) p.rect({row0.x, r.y + hy - off, row0.w, item_h}, t.text.with_alpha(0.09f * ha), t.radius_s());

    auto heading = [&](size_t before, std::string_view s) {
        float y = r.y + item_y(before) - head_h - off;
        if (labels > 0.01f) ui.label({r.x + 24, y + 12, r.w - 48, 20}, s, {.size = t.fs(12), .color = t.text_dim.with_alpha(labels)});
        if (labels < 0.99f && before > 0) p.rect({r.x + 24, y + head_h / 2, r.w - 48, 1}, t.border.with_alpha(1 - labels));
    };
    auto item = [&](ui::Id id, size_t k, std::string_view label, std::string_view icon, float thumb_hue) {
        Rect row = row0;
        row.y = r.y + item_y(k) - off;
        auto it = ui.interact(id, row);
        float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
        float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
        bool selected = sel == int(k);
        if (hot > 0 && !selected) p.rect(row.inset(down), t.hover.with_alpha(hot), t.radius_s());
        float ix = row.x + 10 + ((row.w - 20) / 2 - 10) * (1 - labels);
        Rect ic{ix, row.y + 10, 18, 18};
        if (thumb_hue >= 0) cover(p, ic.inset(-2), thumb_hue, 5, "");
        else p.icon(icon, ic, selected ? t.accent : t.text_dim.mix(t.text, hot * 0.6f));
        if (labels > 0.01f)
            ui.label({row.x + 42, row.y, row.w - 50, row.h}, label,
                     {.size = t.fs(14), .color = (selected ? t.text : t.text.mix(t.text_dim, 0.3f * (1 - hot))).with_alpha(labels)});
        return it.clicked;
    };

    heading(0, "发现");
    heading(3, "我的音乐");
    heading(6, "歌单");
    for (size_t k = 0; k < std::size(nav); ++k)
        if (item(ui.id(nav[k].label), k, nav[k].label, nav[k].icon, -1)) open(ui, nav[k].page);
    if (labels > 0.5f && ui.icon_button(ui.id("new-playlist"), {r.right() - 48, r.y + item_y(6) - head_h + 8 - off, 24, 24}, "plus", 16)) {
        playlists_.push_back({std::format("新建歌单 {}", playlists_.size() + 1), "0 首", float(next(rng_) % 1000) / 1000, 0});
        ui.toast("已新建歌单", "folder-plus");
    }
    ui.push_id(ui.id("playlists"));
    for (size_t i = 0; i < playlists_.size(); ++i)
        if (item(ui.id(uint64_t(i)), 6 + i, playlists_[i].title, {}, playlists_[i].hue)) open(ui, Page::playlist, playlists_[i], int(i));
    ui.pop_id();
    ui.end_scroll();
}

bool Shell::hero(ui::Ui& ui, ui::Id id, Rect r, std::string_view big, std::string_view title, std::string_view subtitle, float hue) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    auto it = ui.interact(id, r);
    float hot = ui.anim(id, it.hovered ? 1 : 0, 240);
    float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
    r = r.inset(down * 3);
    r.y -= 3 * hot;
    p.shadow({r.x, r.y + 10, r.w, r.h}, t.radius_l(), 32, t.shadow.with_alpha(0.35f + 0.65f * hot));
    p.gradient(r, hsl(hue, 0.6f, 0.52f), hsl(hue + 0.07f, 0.55f, 0.27f), t.radius_l());
    ui.label({r.x + 24, r.y + 16, r.w - 48, 54}, big, {.size = t.fs(42), .color = white});
    ui.label({r.x + 24, r.y + 74, r.w - 48, 24}, title, {.size = t.fs(18), .color = white});
    ui.label({r.x + 24, r.y + 98, r.w - 100, 20}, subtitle, {.size = t.fs(12), .color = white.with_alpha(0.75f)});
    Rect b = Rect{r.right() - 64, r.bottom() - 64, 44, 44}.inset(-2 * hot);
    p.rect(b, white.with_alpha(0.2f + 0.8f * hot), b.w / 2);
    p.icon("play-fill", ui::center(b, 18, 18), hsl(hue, 0.6f, 0.4f).mix(white, 1 - hot));
    return it.clicked;
}

bool Shell::card(ui::Ui& ui, ui::Id id, Rect r, const Card& c, bool round) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    auto it = ui.interact(id, r);
    float hot = ui.anim(id, it.hovered ? 1 : 0, 240);
    float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
    Rect cov = Rect{r.x, r.y - 4 * hot, r.w, r.w}.inset(down * 3);
    float radius = round ? cov.w / 2 : t.radius;
    if (hot > 0) p.shadow({cov.x, cov.y + 8, cov.w, cov.h}, radius, 24, t.shadow.with_alpha(hot));
    cover(p, cov, c.hue, radius, round ? "user" : "");
    if (hot > 0 && !round) {
        // The play button rises into place.
        Rect b{cov.right() - 48, cov.bottom() - 48 + 8 * (1 - hot), 36, 36};
        p.shadow(b, 18, 10, t.shadow.with_alpha(hot));
        p.rect(b, t.accent.with_alpha(hot), 18);
        p.icon("play-fill", ui::center(b, 16, 16), white.with_alpha(hot));
    }
    Align align = round ? Align::center : Align::left;
    ui.label({r.x, r.y + r.w + 10, r.w, 20}, c.title, {.size = t.fs(14), .color = t.text}, align);
    ui.label({r.x, r.y + r.w + 32, r.w, 18}, c.subtitle, {.size = t.fs(12), .color = t.text_dim}, align);
    return it.clicked;
}

int Shell::card_grid(ui::Ui& ui, Rect r, ui::Id id, const std::vector<Card>& cards, bool round, double since) {
    Grid g(r.w, round ? 130 : 150);
    Rect clip = ui.clip();
    int clicked = -1, shown = 0;
    ui.push_id(id);
    for (size_t i = 0; i < cards.size(); ++i) {
        Rect c = g.cell(r.x, r.y, i);
        if (c.bottom() < clip.y || c.y > clip.bottom()) continue;
        // Cards arrive one after another.
        float a = ui.progress(since + 0.03 * std::min(shown++, 14), 420);
        Fade fade(ui.painter(), a);
        c.y += (1 - a) * 16;
        if (card(ui, ui.id(uint64_t(i)), c, cards[i], round)) clicked = int(i);
    }
    ui.pop_id();
    return clicked;
}

void Shell::recommend_page(ui::Ui& ui, Rect r) {
    const auto& t = ui.theme();
    Rect body = r.inset(32, 0);
    Grid g(body.w);
    constexpr float hero_h = 136;
    float content = 28 + 76 + hero_h + 40 + 44 + g.height(recommended_.size()) + 32;
    float off = ui.begin_scroll(ui.id("recommend"), r, content);
    float y = r.y + 28 - off;
    page_title(ui, body, y, "推荐", std::format("{}，为你挑了这些", today_));
    y += 76;
    float hw = (body.w - Grid::gap) / 2;
    std::string day = today_.substr(today_.find("月") + 4);
    day.resize(day.find(' '));
    if (hero(ui, ui.id("hero-daily"), {body.x, y, hw, hero_h}, day, "每日推荐", "根据你的口味生成，每天 6:00 更新", 0.98f)) open(ui, Page::daily);
    if (hero(ui, ui.id("hero-fm"), {body.x + hw + Grid::gap, y, hw, hero_h}, "FM", "私人 FM", "听不腻的随机电台，不喜欢就跳过", 0.62f))
        open(ui, Page::fm);
    y += hero_h + 40;
    ui.label({body.x, y, body.w, 28}, "推荐歌单", {.size = t.fs(18), .color = t.text});
    y += 44;
    if (int i = card_grid(ui, {body.x, y, body.w, 0}, ui.id("rec-cards"), recommended_, false, page_time_ + 0.08); i >= 0)
        open(ui, Page::playlist, recommended_[size_t(i)]);
    ui.end_scroll();
}

void Shell::toplist_page(ui::Ui& ui, Rect r) {
    Rect body = r.inset(32, 0);
    Grid g(body.w);
    float off = ui.begin_scroll(ui.id("toplists"), r, 28 + 84 + g.height(toplists_.size()) + 32);
    float y = r.y + 28 - off;
    page_title(ui, body, y, "排行榜", "官方榜单，按更新时间排列");
    if (int i = card_grid(ui, {body.x, y + 84, body.w, 0}, ui.id("top-cards"), toplists_, false, page_time_ + 0.05); i >= 0)
        open(ui, Page::playlist, toplists_[size_t(i)]);
    ui.end_scroll();
}

void Shell::fm_page(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    page_title(ui, r.inset(32, 0), r.y + 28, "私人 FM", "根据你的收听记录推荐，不喜欢的歌会少出现");
    const Track& tr = fm_[size_t(fm_pos_)];
    bool on_air = current() && key_of(*current()) == key_of(tr);
    float size = std::clamp(std::min(r.w * 0.4f, r.h - 310), 160.f, 280.f);
    Rect col = ui::center(r, size, size + 150);
    col.y += 36;
    Rect cov{col.x, col.y, size, size};
    p.shadow({cov.x, cov.y + 14, cov.w, cov.h}, t.radius_l(), 44, t.shadow);
    cover(p, cov, tr.hue, t.radius_l());
    ui.label({col.x - 100, cov.bottom() + 22, size + 200, 30}, tr.title, {.size = t.fs(20), .color = t.text}, Align::center);
    ui.label({col.x - 100, cov.bottom() + 54, size + 200, 20}, std::format("{} · {}", tr.artist, tr.album),
             {.size = t.fs(13), .color = t.text_dim}, Align::center);
    float by = cov.bottom() + 90, cx = col.x + size / 2;
    auto next_fm = [&] {
        fm_pos_ = (fm_pos_ + 1) % int(fm_.size());
        page_time_ = ui.now, page_dir_ = 1;
        if (on_air) play(fm_, fm_pos_);
    };
    // thumbs-down, like, play, next as one centred row
    float x0 = cx - 102;
    if (ui.icon_button(ui.id("fm-trash"), {x0, by + 4, 40, 40}, "thumbs-down", 20)) {
        next_fm();
        ui.toast("以后会少推荐类似的歌", "thumbs-down");
    }
    bool like = liked(tr);
    if (ui.icon_button(ui.id("fm-like"), {x0 + 52, by + 4, 40, 40}, like ? "heart-fill" : "heart", 20, like)) toggle_like(ui, tr);
    ui::Id pid = ui.id("fm-play");
    Rect play_r{x0 + 104, by, 48, 48};
    auto it = ui.interact(pid, play_r);
    float hot = ui.anim(pid, it.hovered ? 1 : 0, 150), down = ui.anim(ui::sub_id(pid, 0xd0), it.held ? 1 : 0, 90);
    Rect pr = play_r.inset(-2 * hot + 2 * down);
    p.rect(pr, t.accent.mix(white, 0.1f * hot), pr.w / 2);
    p.icon(on_air && playing_ ? "pause-fill" : "play-fill", ui::center(pr, 20, 20), white);
    if (it.clicked) {
        if (on_air) playing_ = !playing_;
        else play(fm_, fm_pos_);
    }
    if (ui.icon_button(ui.id("fm-next"), {x0 + 164, by + 4, 40, 40}, "skip-forward", 20)) next_fm();
}

void Shell::search_page(ui::Ui& ui, Rect r) {
    Rect body = r.inset(32, 0);
    size_t counts[] = {results_.size(), albums_.size(), artists_.size(), result_lists_.size()};
    static constexpr const char* kinds[] = {"首单曲", "张专辑", "位歌手", "个歌单"};
    page_title(ui, body, r.y + 28, std::format("“{}”", searched_), std::format("找到 {} {}", counts[search_tab_], kinds[search_tab_]));
    static constexpr std::string_view tabs[] = {"单曲", "专辑", "歌手", "歌单"};
    int before = search_tab_;
    if (ui.tabs(ui.id("search-tabs"), {body.x, r.y + 100, body.w, 36}, tabs, search_tab_)) {
        selected_ = -1;
        tab_time_ = ui.now;
        tab_dir_ = search_tab_ > before ? 1 : -1;
    }
    // Tab content slides toward the tab that was picked.
    float enter = ui.progress(tab_time_, 380);
    Fade fade(ui.painter(), enter);
    Rect content{r.x + 16 + (1 - enter) * 36 * float(tab_dir_), r.y + 148, r.w - 32, r.h - 156};
    double since = std::max(page_time_, tab_time_);
    if (search_tab_ == 0) {
        track_list(ui, content, ui.id("search-list"), results_, since);
        return;
    }
    static constexpr const char* ids[] = {"", "search-albums", "search-artists", "search-playlists"};
    const std::vector<Card>& cards = search_tab_ == 1 ? albums_ : search_tab_ == 2 ? artists_ : result_lists_;
    bool round = search_tab_ == 2;
    Rect inner = content.inset(12, 0);
    Grid g(inner.w, round ? 130 : 150);
    ui::Id id = ui.id(ids[search_tab_]);
    float off = ui.begin_scroll(id, content, 12 + g.height(cards.size()) + 20);
    if (int i = card_grid(ui, {inner.x, content.y + 12 - off, inner.w, 0}, id, cards, round, since); i >= 0) {
        Card c = cards[size_t(i)];
        if (round) c.title += " · 热门歌曲";
        open(ui, Page::playlist, c);
    }
    ui.end_scroll();
}

void Shell::list_page(ui::Ui& ui, Rect r, std::vector<Track>& tracks) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    const Card& c = route_.card;
    Rect head{r.x + 32, r.y + 32, r.w - 64, 148};
    Rect cov{head.x, head.y, 148, 148};
    p.shadow({cov.x, cov.y + 10, cov.w, cov.h}, t.radius_l(), 32, t.shadow.with_alpha(0.8f));
    cover(p, cov, c.hue, t.radius_l(),
          route_.page == Page::liked    ? "heart-fill"
          : route_.page == Page::recent ? "history"
          : route_.page == Page::cloud  ? "cloud"
                                        : "");
    float tx = cov.right() + 28, tw = head.right() - tx;
    ui.label({tx, head.y + 12, tw, 38}, c.title, {.size = t.fs(28), .color = t.text});
    ui.label({tx, head.y + 58, tw, 20}, c.subtitle, {.size = t.fs(13), .color = t.text_dim});
    if (ui.button(ui.id("play-all"), {tx, head.bottom() - 40, 124, 40}, "播放全部", ui::ButtonKind::primary, "play-fill")) play(tracks, 0);
    if (ui.button(ui.id("shuffle-all"), {tx + 136, head.bottom() - 40, 112, 40}, "随机播放", ui::ButtonKind::secondary, "shuffle") &&
        !tracks.empty()) {
        mode_ = 2;
        play(tracks, int(next(rng_) % tracks.size()));
    }
    Rect list{r.x + 16, head.bottom() + 28, r.w - 32, r.bottom() - head.bottom() - 36};
    if (tracks.empty()) {
        ui.label({list.x, list.y + 60, list.w, 24}, "这里还没有歌曲", {.size = t.fs(14), .color = t.text_dim}, Align::center);
        return;
    }
    track_list(ui, list, ui.id("tracks"), tracks, page_time_ + 0.06);
}

void Shell::track_list(ui::Ui& ui, Rect r, ui::Id id, std::vector<Track>& tracks, double since) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    constexpr float row_h = 56;
    Rect head = ui::cut_top(r, 34);
    Columns c = columns(head);
    TextStyle hs{.size = t.fs(12), .color = t.text_dim};
    ui.label(c.index, "#", hs, Align::center);
    ui.label(c.title, "标题", hs);
    if (c.album.w > 0) ui.label(c.album, "专辑", hs);
    ui.label(c.duration, "时长", hs, Align::right);
    p.rect({head.x + 12, head.bottom() - 1, head.w - 24, 1}, t.border);
    ui::cut_top(r, 4);

    float off = ui.begin_scroll(id, r, float(tracks.size()) * row_h + 8);
    int first = int(off / row_h), last = int((off + r.h) / row_h) + 1;
    ui.push_id(id);
    for (int i = first; i <= last && i < int(tracks.size()); ++i) {
        // Rows cascade in when the list appears.
        float a = ui.progress(since + 0.022 * (i - first), 380);
        Fade fade(p, a);
        track_row(ui, {r.x, r.y + float(i) * row_h - off + (1 - a) * 14, r.w, row_h}, tracks, i);
    }
    ui.pop_id();
    ui.end_scroll();
}

void Shell::track_row(ui::Ui& ui, Rect r, std::vector<Track>& tracks, int i) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    const Track& tr = tracks[size_t(i)];
    ui.push_id(ui.id(uint64_t(i)));
    ui::Id row_id = ui.id("row");
    bool hot = ui.hovering(r) || ui.is_active(row_id);
    bool now_playing = current() && key_of(*current()) == key_of(tr);
    float h = ui.anim(row_id, hot ? 1 : 0, 140);
    float s = ui.anim(ui::sub_id(row_id, 0x5e), selected_ == i ? 1 : 0, 200);
    Rect bg = r.inset(4, 2);
    if (h > 0 || s > 0) p.rect(bg, t.text.with_alpha(0.06f * h + 0.05f * s), t.radius_s());

    Columns c = columns(r);
    TextStyle num{.size = t.fs(13), .color = t.text_dim, .features = text::tabular_nums};
    if (hot) {
        if (ui.icon_button(ui.id("play"), ui::center(c.index, 30, 30), now_playing && playing_ ? "pause-fill" : "play-fill", 14)) {
            if (now_playing) playing_ = !playing_;
            else play(tracks, i);
        }
    } else if (now_playing) {
        p.icon(playing_ ? "volume-2" : "pause", ui::center(c.index, 16, 16), t.accent);
    } else {
        ui.label(c.index, std::to_string(i + 1), num, Align::center);
    }
    Rect cov{c.title.x, r.y + (r.h - 40) / 2, 40, 40};
    cover(p, cov, tr.hue, t.radius_s());
    float tx = cov.right() + 14, tw = c.title.right() - tx - 8;
    p.text(tx, r.y + 25, tr.title, {.size = t.fs(14), .color = now_playing ? t.accent : t.text}, tw);
    p.text(tx, r.y + 43, tr.artist, {.size = t.fs(12), .color = t.text_dim}, tw);
    if (c.album.w > 0) ui.label(c.album, tr.album, {.size = t.fs(13), .color = t.text_dim});
    bool like = liked(tr);
    if ((hot || like) && ui.icon_button(ui.id("like"), ui::center(c.like, 32, 32), like ? "heart-fill" : "heart", 16, like)) toggle_like(ui, tr);
    ui.label(c.duration, mmss(tr.duration), num, Align::right);

    auto it = ui.interact(row_id, r);
    if (it.pressed) selected_ = i;
    if (it.double_clicked) play(tracks, i);
    if (it.right_clicked) {
        selected_ = i;
        track_menu(ui, it.px, it.py, tracks, i);
    }
    ui.pop_id();
}

void Shell::track_menu(ui::Ui& ui, float x, float y, std::vector<Track>& tracks, int i) {
    Track tr = tracks[size_t(i)];
    bool like = liked(tr);
    auto* list = &tracks;
    ui.open_menu(x, y,
                 {
                     {"play", "播放", [this, list, i] { play(*list, i); }},
                     {"list-plus", "下一首播放",
                      [this, &ui, tr] {
                          queue_.insert(queue_.begin() + (current_ + 1), tr);
                          if (current_ < 0) current_ = 0;
                          ui.toast("已添加到下一首播放", "list-plus");
                      }},
                     {"disc-album", "查看专辑",
                      [this, &ui, tr] { open(ui, Page::playlist, {tr.album, std::format("{} · 专辑", tr.artist), tr.hue, 12}); }},
                     {},
                     {like ? "heart-off" : "heart", like ? "取消喜欢" : "喜欢", [this, &ui, tr] { toggle_like(ui, tr); }},
                     {"copy", "复制歌名和歌手",
                      [&ui, tr] {
                          ui.copy = std::format("{} - {}", tr.title, tr.artist);
                          ui.toast("已复制到剪贴板", "copy");
                      }},
                 });
}

void Shell::queue_panel(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    p.rect(r, t.surface, t.radius_l());
    ui.label({r.x + 20, r.y + 18, 160, 28}, "播放队列", {.size = t.fs(16), .color = t.text});
    ui.label({r.x + 104, r.y + 22, 80, 24}, std::format("{} 首", queue_.size()), {.size = t.fs(12), .color = t.text_dim});
    if (ui.button(ui.id("queue-clear"), {r.right() - 76, r.y + 18, 60, 28}, "清空", ui::ButtonKind::ghost) && !queue_.empty()) {
        queue_.clear();
        current_ = -1;
        playing_ = false;
        ui.toast("播放队列已清空", "trash");
    }
    constexpr float row_h = 54;
    Rect view{r.x + 8, r.y + 60, r.w - 16, r.h - 68};
    float off = ui.begin_scroll(ui.id("queue"), view, float(queue_.size()) * row_h);
    int remove = -1;
    ui.push_id(ui.id("queue-rows"));
    int first = int(off / row_h), last = int((off + view.h) / row_h) + 1;
    for (int i = first; i <= last && i < int(queue_.size()); ++i) {
        const Track& tr = queue_[size_t(i)];
        Rect row{view.x, view.y + float(i) * row_h - off, view.w, row_h};
        ui.push_id(ui.id(uint64_t(i)));
        ui::Id row_id = ui.id("row");
        bool hot = ui.hovering(row) || ui.is_active(row_id), cur = i == current_;
        float h = ui.anim(row_id, hot ? 1 : 0, 140);
        if (cur) p.rect(row.inset(2), t.accent.with_alpha(0.14f), t.radius_s());
        else if (h > 0) p.rect(row.inset(2), t.hover.with_alpha(h), t.radius_s());
        cover(p, {row.x + 10, row.y + 9, 36, 36}, tr.hue, t.radius_s());
        float tw = row.w - 58 - (hot ? 40 : 12);
        p.text(row.x + 58, row.y + 23, tr.title, {.size = t.fs(13), .color = cur ? t.accent : t.text}, tw);
        p.text(row.x + 58, row.y + 41, tr.artist, {.size = t.fs(12), .color = t.text_dim}, tw);
        if (hot && ui.icon_button(ui.id("remove"), {row.right() - 38, row.y + 13, 28, 28}, "x", 14)) remove = i;
        if (ui.interact(row_id, row).double_clicked) play(queue_, i);
        ui.pop_id();
    }
    ui.pop_id();
    ui.end_scroll();
    if (remove >= 0) {
        queue_.erase(queue_.begin() + remove);
        if (remove < current_) --current_;
        else if (remove == current_) {
            current_ = queue_.empty() ? -1 : std::min(current_, int(queue_.size()) - 1);
            position_ = 0;
            playing_ = playing_ && current_ >= 0;
        }
    }
}

void Shell::player_bar(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    bool compact = r.w < 900;
    float side_w = std::min(330.f, std::round(r.w * 0.3f));
    Rect left = ui::cut_left(r, side_w), right = ui::cut_right(r, side_w), mid = r;
    const Track* tr = current();

    // Now playing; the cover cross-fades and the text rises in after a track change.
    Rect cov{left.x + 16, left.y + 14, 56, 56};
    float k = ui.progress(cover_time_, 520);
    if (tr) {
        p.shadow({cov.x, cov.y + 4, cov.w, cov.h}, t.radius_s(), 12, t.shadow.with_alpha(0.6f));
        if (k < 1 && prev_hue_ >= 0) cover(p, cov, prev_hue_, t.radius_s());
        {
            Fade fade(p, k);
            cover(p, cov, tr->hue, t.radius_s());
        }
        float tx = cov.right() + 14, tw = left.right() - tx - 48, dy = (1 - k) * 8;
        Fade fade(p, k);
        p.text(tx, left.y + 38 + dy, tr->title, {.size = t.fs(14), .color = t.text}, tw);
        p.text(tx, left.y + 57 + dy, tr->artist, {.size = t.fs(12), .color = t.text_dim}, tw);
    } else {
        p.rect(cov, t.text.with_alpha(0.06f), t.radius_s());
        p.icon("music", ui::center(cov, 22, 22), t.text_dim);
        ui.label({cov.right() + 14, left.y, left.w - 100, left.h}, "没有正在播放的歌曲", {.size = t.fs(13), .color = t.text_dim});
    }
    if (tr) {
        // The heart follows the end of the longer line.
        float tx = cov.right() + 14;
        float text_w = std::max(p.measure(tr->title, {.size = t.fs(14)}), p.measure(tr->artist, {.size = t.fs(12)}));
        float hx = std::min(tx + text_w + 8, left.right() - 44);
        bool like = liked(*tr);
        if (ui.icon_button(ui.id("bar-like"), {hx, left.y + 26, 32, 32}, like ? "heart-fill" : "heart", 17, like)) toggle_like(ui, *tr);
    }

    // Transport; play and pause morph into each other.
    float cx = std::round(mid.x + mid.w / 2), cy = mid.y + 30;
    static constexpr const char* mode_icons[] = {"repeat", "repeat-1", "shuffle"};
    static constexpr const char* mode_names[] = {"列表循环", "单曲循环", "随机播放"};
    if (ui.icon_button(ui.id("mode"), {cx - 124, cy - 16, 32, 32}, mode_icons[mode_], 17, mode_ != 0)) {
        mode_ = (mode_ + 1) % 3;
        ui.toast(mode_names[mode_], mode_icons[mode_]);
    }
    if (ui.icon_button(ui.id("prev"), {cx - 72, cy - 16, 32, 32}, "skip-back", 18)) skip(-1);
    ui::Id pid = ui.id("play");
    Rect play_r{cx - 21, cy - 21, 42, 42};
    auto it = ui.interact(pid, play_r);
    float hot = ui.anim(pid, it.hovered ? 1 : 0, 150), down = ui.anim(ui::sub_id(pid, 0xd0), it.held ? 1 : 0, 90);
    Rect pr = play_r.inset(-2 * hot + 2.5f * down);
    p.shadow({pr.x, pr.y + 3, pr.w, pr.h}, pr.w / 2, 10, t.shadow.with_alpha(0.5f * hot));
    p.rect(pr, t.text, pr.w / 2);
    float morph = ui.anim(ui.id("play-morph"), playing_ ? 1 : 0, 260, ease::emphasized);
    {
        Fade fade(p, 1 - morph);
        p.icon("play-fill", ui::center(pr, 18 - 6 * morph, 18 - 6 * morph), t.bg);
    }
    {
        Fade fade(p, morph);
        p.icon("pause-fill", ui::center(pr, 12 + 6 * morph, 12 + 6 * morph), t.bg);
    }
    if (it.clicked && tr) playing_ = !playing_;
    if (ui.icon_button(ui.id("next"), {cx + 40, cy - 16, 32, 32}, "skip-forward", 18)) skip(1);
    if (ui.icon_button(ui.id("lyrics"), {cx + 92, cy - 16, 32, 32}, "mic-vocal", 17, desktop_lyrics_)) {
        desktop_lyrics_ = !desktop_lyrics_;
        ui.toast(desktop_lyrics_ ? "桌面歌词已打开" : "桌面歌词已关闭", "mic-vocal");
    }

    // Progress: a sine wave runs through the played part while the music plays and settles flat on pause.
    TextStyle ts{.size = t.fs(11), .color = t.text_dim, .features = text::tabular_nums};
    float py = mid.y + 66, dur = tr ? float(tr->duration) : 0;
    Rect bar{mid.x + 52, py - 9, mid.w - 104, 18};
    progress_w_ = bar.w;
    float v = tr ? float(position_) / dur : 0;
    float wave = ui.anim(ui.id("wave"), playing_ ? 3.f : 0.f, 600, ease::standard);
    if (ui.slider(ui.id("progress"), bar, v, {.wave = wave}) && tr) position_ = v * dur;
    ui.label({mid.x, py - 8, 44, 16}, mmss(tr ? position_ : 0), ts, Align::right);
    ui.label({bar.right() + 8, py - 8, 44, 16}, mmss(dur), ts);

    // Volume and panels; narrow windows keep only the icons.
    float vol_w = ui.anim(ui.id("vol-w"), compact ? 0 : 100, 320, ease::emphasized);
    Rect vol{right.right() - 20 - vol_w, cy - 8, vol_w, 16};
    float shown = muted_ ? 0 : volume_;
    if (vol_w > 8) {
        Fade fade(p, vol_w / 100);
        if (ui.slider(ui.id("volume"), vol, shown, {.accent = false})) volume_ = shown, muted_ = false;
    }
    const char* vol_icon = shown == 0 ? "volume-x" : shown < 0.5f ? "volume-1" : "volume-2";
    if (ui.icon_button(ui.id("mute"), {vol.x - 40, cy - 16, 32, 32}, vol_icon, 18)) muted_ = !muted_;
    if (ui.icon_button(ui.id("queue"), {vol.x - 80, cy - 16, 32, 32}, "list-music", 18, queue_open_)) queue_open_ = !queue_open_;
}

void Shell::account_modal(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    static constexpr const char* services[] = {"网易云音乐", "QQ 音乐"};
    // Steps slide sideways: forward into the code, back to the list.
    float in = ui.progress(login_time_, 380);
    Fade fade(p, in);
    r.x += (1 - in) * 32 * (login_step_ >= 0 ? 1 : -1);

    if (login_step_ < 0) {
        ui.resize_modal(420, 330);
        ui.label({r.x, r.y, r.w, 30}, "账号", {.size = t.fs(20), .color = t.text});
        ui.label({r.x, r.y + 32, r.w, 20}, "登录后，歌单、喜欢和推荐会合在一起", {.size = t.fs(12), .color = t.text_dim});
        for (int i = 0; i < 2; ++i) {
            Rect row{r.x, r.y + 76 + float(i) * 72, r.w, 64};
            p.rect(row, t.text.with_alpha(0.04f), t.radius);
            Rect avatar{row.x + 14, row.y + 12, 40, 40};
            p.rect(avatar, t.text.with_alpha(0.08f), 20);
            p.icon("user", ui::center(avatar, 18, 18), t.text_dim);
            ui.label({avatar.right() + 14, row.y + 12, 160, 22}, services[i], {.size = t.fs(15), .color = t.text});
            ui.label({avatar.right() + 14, row.y + 34, 160, 18}, "未登录", {.size = t.fs(12), .color = t.text_dim});
            if (ui.button(ui.id(services[i]), {row.right() - 108, row.y + 15, 94, 34}, "扫码登录", ui::ButtonKind::secondary, "qr-code")) {
                login_step_ = i;
                login_time_ = ui.now;
                ++qr_seed_;
            }
        }
        ui.label({r.x, r.bottom() - 18, r.w, 18}, "凭据加密保存在本机，只有你的账户能解开", {.size = t.fs(12), .color = t.text_dim});
        return;
    }

    ui.resize_modal(400, 540);
    if (ui.icon_button(ui.id("login-back"), {r.x - 8, r.y - 2, 32, 32}, "arrow-left", 18)) {
        login_step_ = -1;
        login_time_ = ui.now;
        return;
    }
    ui.label({r.x + 32, r.y, r.w - 64, 30}, "扫码登录", {.size = t.fs(20), .color = t.text});
    ui.label({r.x, r.y + 40, r.w, 20}, std::format("用{} App 扫一扫", services[login_step_]), {.size = t.fs(13), .color = t.text_dim},
             Align::center);
    Rect qr = ui::center({r.x, r.y + 76, r.w, 240}, 228, 228);
    p.shadow({qr.x, qr.y + 8, qr.w, qr.h}, 14, 30, t.shadow.with_alpha(0.6f));
    fake_qr(p, qr, qr_seed_ * 2 + uint64_t(login_step_));
    Rect logo = ui::center(qr, 40, 40);
    p.rect(logo.inset(-4), white, 12);
    p.rect(logo, t.accent, 10);
    p.icon("music", ui::center(logo, 22, 22), white);

    float sy = qr.bottom() + 30;
    TextStyle st{.size = t.fs(14), .color = t.text};
    float tw = p.measure("等待扫码", st);
    float sx = r.x + (r.w - tw - 26) / 2;
    ui.spinner({sx, sy + 2, 16, 16}, t.accent, 2);
    ui.label({sx + 26, sy, tw + 1, 20}, "等待扫码", st);
    ui.label({r.x, sy + 30, r.w, 20}, "扫码后在手机上确认登录", {.size = t.fs(12), .color = t.text_dim}, Align::center);
    if (ui.button(ui.id("qr-refresh"), {r.x + (r.w - 132) / 2, r.bottom() - 36, 132, 34}, "刷新二维码", ui::ButtonKind::secondary, "refresh-cw"))
        ++qr_seed_;
}

void Shell::settings_modal(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    ui.label({r.x, r.y, r.w, 30}, "设置", {.size = t.fs(20), .color = t.text});
    float y = r.y + 52;
    auto row = [&](std::string_view label, std::string_view hint) {
        ui.label({r.x, y + 8, r.w - 200, 20}, label, {.size = t.fs(14), .color = t.text});
        ui.label({r.x, y + 30, r.w - 120, 18}, hint, {.size = t.fs(12), .color = t.text_dim});
        Rect control{r.x, y, r.w, 60};
        y += 60;
        p.rect({r.x, y - 1, r.w, 1}, t.border);
        return control;
    };
    static constexpr std::string_view qualities[] = {"标准", "较高", "极高", "无损", "Hi-Res"};
    Rect q = row("音质", "账号没有权限时自动降档");
    ui.tabs(ui.id("quality"), {q.right() - 240, q.y + 14, 240, 30}, qualities, quality_, ui::TabStyle::pill);
    Rect c = row("桌面歌词", "在其他窗口上方显示当前歌词");
    ui.toggle(ui.id("set-lyrics"), {c.right() - 40, c.y + 19, 40, 22}, desktop_lyrics_);
    c = row("跳过试听片段", "只能试听的歌曲直接跳过");
    ui.toggle(ui.id("set-trial"), {c.right() - 40, c.y + 19, 40, 22}, skip_trials_);
    c = row("音量均衡", "让不同歌曲的响度保持一致");
    ui.toggle(ui.id("set-normalize"), {c.right() - 40, c.y + 19, 40, 22}, normalize_);
    ui.label({r.x, r.bottom() - 20, r.w, 20}, "其他选项在 ~/.config/lyren/config.toml，保存后立即生效", {.size = t.fs(12), .color = t.text_dim});
}

}  // namespace lyren
