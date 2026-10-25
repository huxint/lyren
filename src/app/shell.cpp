#include "app/shell.hpp"

#include <linux/input-event-codes.h>
#include <qrencode.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>

#include "core/log.hpp"
#include "core/utf8.hpp"
#include "sys/creds.hpp"

namespace lyren {

using ui::Align;
using ui::Rect;
using ui::TextStyle;
using State = audio::Player::State;
namespace ease = ui::ease;

constexpr ui::Color white{1, 1, 1, 1};

// Helpers.

static uint64_t next(uint64_t& s) {
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static ui::Color hsl(float h, float s, float l) {
    h -= std::floor(h);
    auto f = [&](float n) {
        float k = std::fmod(n + h * 12, 12.f);
        return l - s * std::min(l, 1 - l) * std::max(-1.f, std::min({k - 3, 9 - k, 1.f}));
    };
    return {f(0), f(8), f(4), 1};
}

static std::string mmss(double seconds) {
    int v = int(std::max(0.0, seconds));
    return std::format("{}:{:02}", v / 60, v % 60);
}

// Artwork for lists that have no cover of their own (liked, recent, cloud, daily): a gradient with two soft discs.
static void art(ui::Painter& p, Rect r, float hue, float radius, std::string_view icon) {
    p.gradient(r, hsl(hue, 0.42f, 0.54f), hsl(hue + 0.08f, 0.4f, 0.22f), radius);
    float a = std::fmod(hue * 7.31f, 1.f), b = std::fmod(hue * 13.7f, 1.f);
    float d1 = r.w * (0.5f + 0.35f * a), d2 = r.w * (0.3f + 0.25f * b);
    p.rect({r.x + (r.w - d1) * b, r.y + (r.h - d1) * (1 - a), d1, d1}, hsl(hue + 0.1f, 0.5f, 0.72f).with_alpha(0.26f), d1 / 2);
    p.rect({r.x + (r.w - d2) * (1 - a), r.y + (r.h - d2) * b, d2, d2}, hsl(hue - 0.12f, 0.55f, 0.62f).with_alpha(0.42f), d2 / 2);
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

Shell::Shell(ShellDeps d, SavedState saved)
    : loop_(d.loop),
      http_(d.http),
      player_(d.player),
      ncm_(d.ncm),
      images_(d.images),
      config_(d.config),
      redraw_(std::move(d.redraw)),
      now_(std::move(d.now)),
      silent_(d.silent),
      quality_(int(d.config.playback.quality)),
      skip_trials_(d.config.playback.trial == TrialMode::skip) {
    std::time_t now = std::time(nullptr);
    std::tm lt;
    localtime_r(&now, &lt);
    today_ = std::format("{} 月 {} 日", lt.tm_mon + 1, lt.tm_mday);

    if (saved.width > 0) window_ = std::pair{saved.width, saved.height};
    volume_ = std::clamp(saved.volume, 0.f, 1.f);
    muted_ = saved.muted;
    mode_ = Mode(std::clamp(saved.mode, 0, 2));
    queue_ = std::move(saved.queue);
    current_ = saved.current;
    resume_at_ = last_pos_ = saved.position;
    recent_ = std::move(saved.recent);
    rng_ ^= uint64_t(now);

    player_.on_change = [this] { on_player(); };
    apply_volume();
    ncm_.client.on_cookies = [this] { save_credentials(); };
    if (current()) spawn(load_lyrics(*current()));
    spawn(startup());
}

void Shell::notify(std::string text, std::string icon, bool error) {
    toasts_.push_back({std::move(text), std::move(icon), error});
    redraw_();
}

void Shell::changed() {
    loop_.cancel(save_timer_);
    save_timer_ = loop_.after(2s, [this] {
        save_timer_ = 0;
        save();
    });
}

void Shell::save(std::optional<std::pair<int, int>> window) {
    if (window) window_ = window;
    SavedState s{.volume = volume_, .muted = muted_, .mode = int(mode_), .queue = queue_, .current = current_, .recent = recent_};
    if (window_) s.width = window_->first, s.height = window_->second;
    auto st = player_.state();
    s.position = st == State::playing || st == State::paused ? player_.position() : resume_at_ > 0 ? resume_at_ : last_pos_;
    save_state(s);
}

ui::Color Shell::accent(double now, bool dark, bool& animating) {
    // -2: not known yet (cover still loading), -1: a colourless cover.
    float hue = current() ? cover_hue(current()->cover, bar_cover_px_) : 0.98f;
    auto color = [&](float h) { return h == -1 ? ui::Color{0.86f, 0.86f, 0.88f, 1} : hsl(h < 0 ? 0.98f : h, 0.62f, dark ? 0.64f : 0.46f); };
    if (accent_hue_ == -2 && hue == -2) hue = 0.98f;
    if (hue != -2 && hue != accent_hue_) {
        bool first = accent_hue_ == -2;
        accent_from_ = first ? color(hue) : accent_from_.mix(accent_to_, ease::standard(std::clamp(float((now - accent_time_) / 0.8), 0.f, 1.f)));
        accent_hue_ = hue;
        accent_time_ = first ? -10 : now;
    }
    accent_to_ = color(accent_hue_);
    float t = float((now - accent_time_) / 0.8);
    if (t >= 1) return accent_to_;
    animating = true;
    return accent_from_.mix(accent_to_, ease::standard(t));
}

float Shell::cover_hue(const std::string& url, float size) {
    int px = Images::tier(size);
    const auto& e = images_.get(ncm_.cover_url(url, px), uint32_t(px));
    return e.ready ? e.hue : e.failed ? -1 : -2;
}

float Shell::special_hue(Page page) const {
    switch (page) {
        case Page::liked: return 0.97f;
        case Page::recent: return 0.72f;
        case Page::cloud: return 0.58f;
        default: return 0.98f;
    }
}

bool Shell::liked(const Track& t) const { return t.source == Source::netease && liked_ids_.contains(t.id); }

void Shell::toggle_like(Track t) {
    if (!ncm_.logged_in()) return notify("登录网易云音乐后才能收藏喜欢的歌", "heart");
    bool on = !liked(t);
    if (on) liked_ids_.insert(t.id);
    else liked_ids_.erase(t.id);
    if (route_.page == Page::liked && route_.list) {
        auto& v = route_.list->tracks;
        if (on) v.insert(v.begin(), t);
        else std::erase_if(v, [&](const Track& x) { return same(x, t); });
    }
    notify(on ? "已添加到我喜欢的音乐" : "已取消喜欢", on ? "heart-fill" : "heart-off");
    spawn([](Shell* self, Track t, bool on) -> Task<void> {
        auto r = co_await self->ncm_.like(t.id, on);
        if (r) co_return;
        if (on) self->liked_ids_.erase(t.id);
        else self->liked_ids_.insert(t.id);
        self->notify(std::format("操作没有成功：{}", r.error()), "triangle-alert", true);
    }(this, std::move(t), on));
}

// Library and account.

Task<void> Shell::startup() {
    auto saved = co_await creds::load(loop_);
    if (saved) {
        ncm_.client.cookies = creds::from_json(*saved).netease;
        can_save_creds_ = true;
    } else {
        notify(std::format("无法读取保存的登录信息：{}", saved.error()), "triangle-alert", true);
    }
    bool fresh = ncm_.client.cookies.empty();
    ncm_.client.ensure_fingerprint();
    creds_loaded_ = true;
    if (fresh) save_credentials();
    spawn(load_recommended());
    co_await load_library();
}

void Shell::save_credentials() {
    if (!can_save_creds_) return;
    // Responses often set several cookies in a row; write once they settle.
    loop_.cancel(creds_timer_);
    creds_timer_ = loop_.after(1s, [this] {
        creds_timer_ = 0;
        spawn([](Shell* self) -> Task<void> {
            auto r = co_await creds::save(self->loop_, creds::to_json({.netease = self->ncm_.client.cookies}));
            if (!r) self->notify(std::format("登录信息没有保存：{}", r.error()), "triangle-alert", true);
        }(this));
    });
}

Task<void> Shell::load_library() {
    if (!ncm_.logged_in()) co_return;
    auto a = co_await ncm_.account();
    if (!a) {
        notify(a.error(), "user", true);
        if (!ncm_.logged_in()) save_credentials();
        co_return;
    }
    account_ = *a;
    redraw_();
    auto pl = co_await ncm_.user_playlists(a->id);
    if (pl) {
        liked_card_ = pl->first;
        playlists_ = std::move(pl->second);
    }
    auto ids = co_await ncm_.liked_ids(a->id);
    if (ids) liked_ids_ = {ids->begin(), ids->end()};
    // A page that waited for the login loads now.
    ensure_route_data();
    redraw_();
}

Task<void> Shell::load_recommended() {
    recommended_loading_ = true;
    auto r = co_await ncm_.recommended_playlists();
    recommended_loading_ = false;
    if (r) recommended_ = std::move(*r);
    else recommended_error_ = r.error();
    page_time_ = route_.page == Page::recommend ? now_() : page_time_;
    redraw_();
}

Task<void> Shell::load_toplists() {
    toplists_loading_ = true;
    toplists_error_.clear();
    auto r = co_await ncm_.toplists();
    toplists_loading_ = false;
    if (r) toplists_ = std::move(*r);
    else toplists_error_ = r.error();
    page_time_ = route_.page == Page::toplist ? now_() : page_time_;
    redraw_();
}

Task<void> Shell::load_fm() {
    fm_loading_ = true;
    fm_error_.clear();
    auto r = co_await ncm_.fm();
    fm_loading_ = false;
    if (r) fm_.insert(fm_.end(), r->begin(), r->end());
    else fm_error_ = r.error();
    redraw_();
}

void Shell::next_fm() {
    if (fm_pos_ + 1 < int(fm_.size())) ++fm_pos_;
    if (fm_pos_ + 2 >= int(fm_.size()) && !fm_loading_) spawn(load_fm());
    page_time_ = now_(), page_dir_ = 1;
    if (fm_playing_) play(fm_, fm_pos_);
}

Task<void> Shell::qr_login() {
    uint64_t gen = ++qr_gen_;
    qr_active_ = true;
    qr_.clear();
    qr_status_.reset();
    qr_error_.clear();
    auto url = co_await ncm_.qr_start();
    if (gen != qr_gen_) co_return;
    if (!url) {
        qr_error_ = url.error();
        co_return redraw_();
    }
    QRcode* q = QRcode_encodeString(url->c_str(), 0, QR_ECLEVEL_H, QR_MODE_8, 1);
    qr_size_ = q->width;
    qr_.resize(size_t(q->width) * size_t(q->width));
    for (size_t i = 0; i < qr_.size(); ++i) qr_[i] = q->data[i] & 1;
    QRcode_free(q);
    qr_status_ = QrStatus{};
    redraw_();
    while (true) {
        co_await sleep{loop_, 2s};
        if (gen != qr_gen_) co_return;
        auto s = co_await ncm_.qr_poll();
        if (gen != qr_gen_) co_return;
        if (!s) {
            qr_error_ = s.error();
            co_return redraw_();
        }
        qr_status_ = *s;
        redraw_();
        if (s->state == QrState::expired) co_return;
        if (s->state == QrState::confirmed) break;
    }
    qr_active_ = false;
    can_save_creds_ = true;
    save_credentials();
    co_await load_library();
    if (gen != qr_gen_) co_return;
    login_step_ = -1;
    login_time_ = now_();
    if (account_) notify(std::format("已登录：{}", account_->name), "circle-check");
}

Task<void> Shell::logout() {
    co_await ncm_.logout();
    account_.reset();
    playlists_.clear();
    liked_ids_.clear();
    liked_card_ = {};
    fm_.clear();
    fm_pos_ = 0;
    save_credentials();
    notify("已退出网易云音乐", "log-out");
}

// Navigation.

void Shell::search(ui::Ui& ui) {
    if (query_.empty()) return;
    searched_ = query_;
    ++search_gen_;
    for (auto& r : results_) r = {};
    search_tab_ = 0;
    for (const char* id : {"search-list", "search-albums", "search-artists", "search-playlists"}) ui.scroll_to(ui.id(id), 0);
    load_results(0);
    open(ui, Page::search);
}

void Shell::load_results(int tab) {
    Results& res = results_[tab];
    if (res.loading || (res.loaded && (tab != 0 || int(res.tracks.size()) >= res.total))) return;
    res.loading = true;
    res.error.clear();
    spawn(fetch_results(search_gen_, tab, searched_, int(tab == 0 ? res.tracks.size() : 0)));
}

Task<void> Shell::fetch_results(uint64_t gen, int tab, std::string query, int offset) {
    auto r = co_await ncm_.search(query, SearchKind(tab), offset, tab == 0 ? 30 : 40);
    if (gen != search_gen_) co_return;
    Results& res = results_[tab];
    res.loading = false;
    if (!r) {
        res.error = r.error();
        co_return redraw_();
    }
    if (!res.loaded) res.since = now_();
    res.loaded = true;
    res.total = r->total;
    res.tracks.insert(res.tracks.end(), std::make_move_iterator(r->tracks.begin()), std::make_move_iterator(r->tracks.end()));
    res.cards.insert(res.cards.end(), std::make_move_iterator(r->cards.begin()), std::make_move_iterator(r->cards.end()));
    // The server can report more songs than it hands out.
    if (tab == 0 && r->tracks.empty()) res.total = int(res.tracks.size());
    redraw_();
}

void Shell::open(ui::Ui& ui, Page page, const Card& card, int playlist) {
    if (route_.page == page && route_.card.id == card.id && route_.card.kind == card.kind && page != Page::search) return;
    history_.push_back(route_);
    if (history_.size() > 50) history_.erase(history_.begin());
    // Older pages reload their tracks if they are visited again.
    if (history_.size() > 8) history_[history_.size() - 9].list.reset();
    enter(ui, {page, card, playlist}, 1);
}

// Back closes the now-playing view first.
void Shell::back(ui::Ui& ui) {
    if (np_open_) {
        np_open_ = false;
        np_time_ = ui.now;
        return;
    }
    if (history_.empty()) return;
    Route r = std::move(history_.back());
    history_.pop_back();
    enter(ui, std::move(r), -1);
}

void Shell::enter(ui::Ui& ui, Route route, int direction) {
    route_ = std::move(route);
    page_time_ = ui.now;
    page_dir_ = direction;
    selected_ = -1;
    ui.scroll_to(ui.id("tracks"), 0);
    switch (route_.page) {
        case Page::daily: route_.card = {.title = "每日推荐", .subtitle = std::format("{} · 根据你的口味生成", today_)}; break;
        case Page::cloud: route_.card = {.title = "我的云盘", .subtitle = "上传到云端的歌曲"}; break;
        case Page::liked: route_.card = {.title = "我喜欢的音乐"}; break;
        case Page::recent: route_.card = {.title = "最近播放"}; break;
        default: break;
    }
    ensure_route_data();
}

bool Shell::needs_login(Page p) { return p == Page::daily || p == Page::fm || p == Page::liked || p == Page::cloud; }

void Shell::ensure_route_data() {
    Page page = route_.page;
    bool listed = page == Page::daily || page == Page::cloud || page == Page::liked || page == Page::playlist;
    if (listed && !route_.list && !(needs_login(page) && !ncm_.logged_in())) {
        if (page == Page::liked) {
            if (liked_card_.id.empty()) return;  // the library is still loading
            route_.card.id = liked_card_.id;
        }
        route_.list = std::make_shared<List>();
        spawn(load_list(route_.list, page, route_.card));
    }
    if (page == Page::toplist && toplists_.empty() && !toplists_loading_) spawn(load_toplists());
    if (page == Page::fm && fm_.empty() && !fm_loading_ && ncm_.logged_in()) spawn(load_fm());
}

Task<void> Shell::load_list(std::shared_ptr<List> list, Page page, Card card) {
    list->loading = true;
    // Not a ?: chain: with co_await in its branches GCC 16 also starts the tasks of branches not taken.
    Result<std::vector<Track>> r;
    if (page == Page::daily) r = co_await ncm_.daily();
    else if (page == Page::cloud) r = co_await ncm_.cloud();
    else r = co_await ncm_.tracks(card);
    list->loading = false;
    if (r) {
        list->tracks = std::move(*r);
        list->since = now_();
    } else {
        list->error = r.error();
    }
    redraw_();
}

// Playback.

bool Shell::playing() const {
    auto s = player_.state();
    return want_play_ && (fetching_ || s == State::loading || s == State::playing);
}

void Shell::apply_volume() { player_.set_volume(silent_ || muted_ ? 0.f : volume_); }

void Shell::play(const std::vector<Track>& list, int i) {
    if (i < 0 || i >= int(list.size())) return;
    fm_playing_ = &list == &fm_;
    set_current(i, &list != &queue_);
    if (&list != &queue_) {
        queue_ = list;
        shuffle_history_.clear();
    }
    shuffle_next_ = -1;
    start();
}

void Shell::set_current(int i, bool replacing_queue) {
    if (i == current_ && !replacing_queue) return;
    prev_cover_ = current() ? current()->cover : "";
    cover_time_ = now_();
    current_ = i;
}

void Shell::toggle_play() {
    if (!current()) return;
    auto s = player_.state();
    if (s == State::idle && !fetching_) {
        start(std::exchange(resume_at_, 0));
    } else if (s == State::failed) {
        start(last_pos_);
    } else if (playing()) {
        want_play_ = false;
        player_.pause();
        changed();
    } else {
        want_play_ = true;
        player_.play();
    }
}

int Shell::next_index() {
    int n = int(queue_.size());
    if (!n) return -1;
    if (mode_ != Mode::shuffle) return (current_ + 1) % n;
    if (shuffle_next_ < 0 || shuffle_next_ >= n) {
        shuffle_next_ = n == 1 ? 0 : int(next(rng_) % uint64_t(n - 1));
        if (shuffle_next_ >= current_ && n > 1) ++shuffle_next_;
    }
    return shuffle_next_;
}

void Shell::skip(int delta, bool automatic) {
    if (fm_playing_ && delta > 0) return next_fm();
    if (queue_.empty()) return;
    int n = int(queue_.size()), i;
    if (mode_ == Mode::shuffle && delta > 0) {
        i = next_index();
        shuffle_history_.push_back(current_);
        shuffle_next_ = -1;
    } else if (mode_ == Mode::shuffle && !shuffle_history_.empty()) {
        i = shuffle_history_.back();
        shuffle_history_.pop_back();
    } else {
        i = ((current_ + delta) % n + n) % n;
    }
    set_current(i);
    start(0, automatic ? want_play_ || player_.state() == State::ended : true);
}

void Shell::start(double at, bool play) {
    if (!current()) return;
    uint64_t gen = ++play_gen_;
    retried_ = counted_ = false;
    want_play_ = play;
    fetching_ = true;
    last_pos_ = at;
    resume_at_ = 0;
    if (prefetch_.id != current()->id) prefetch_ = {};
    player_.stop();
    if (lyrics_id_ != current()->id) {
        lyrics_id_ = current()->id;
        lyrics_.clear();
        lyric_layout_.clear();
        lyrics_loading_ = true;
    }
    spawn(load_track(gen, *current(), at));
    changed();
}

Task<void> Shell::load_track(uint64_t gen, Track t, double at) {
    std::shared_ptr<audio::Spool> spool;
    if (prefetch_.spool && prefetch_.id == t.id) {
        spool = std::move(prefetch_.spool);
        clip_start_ = prefetch_.clip_start;
        prefetch_ = {};
    } else {
        auto s = co_await provider(t).stream(t, Quality(quality_));
        if (gen != play_gen_) co_return;
        if (!s || (s->trial && skip_trials_)) {
            fetching_ = false;
            // A run of unplayable tracks is reported once, when one finally plays.
            if (!failures_) notify(s ? std::format("《{}》只能试听，已跳过", t.title) : std::format("《{}》{}", t.title, s.error()), "circle-alert", !s);
            if (++failures_ < queue_.size()) {
                skip(1, true);
            } else {
                failures_ = 0;
                want_play_ = false;
            }
            co_return redraw_();
        }
        spool = audio::Spool::url(http_, s->url);
        clip_start_ = s->trial_start;
    }
    fetching_ = false;
    player_.open(std::move(spool), want_play_);
    if (at > 0) player_.seek(at);
    player_.set_title(std::format("{} - {}", t.title, t.artist));
    if (lyrics_.empty() && lyrics_id_ == t.id) spawn(load_lyrics(t));
    redraw_();
}

Task<void> Shell::prefetch(Track t) {
    uint64_t gen = play_gen_;
    auto s = co_await provider(t).stream(t, Quality(quality_));
    if (gen != play_gen_ || !s || (s->trial && skip_trials_)) co_return;
    prefetch_.id = t.id;
    prefetch_.spool = audio::Spool::url(http_, s->url);
    prefetch_.clip_start = s->trial_start;
}

Task<void> Shell::load_lyrics(Track t) {
    lyrics_id_ = t.id;
    lyrics_loading_ = true;
    auto r = co_await provider(t).lyrics(t);
    if (lyrics_id_ != t.id) co_return;
    lyrics_loading_ = false;
    if (r) lyrics_ = parse_lyrics(r->lrc, r->translation);
    redraw_();
}

void Shell::on_player() {
    auto s = player_.state();
    if (s != last_state_) {
        if (s == State::playing && !counted_ && current()) {
            counted_ = true;
            if (failures_ > 1) notify(std::format("跳过了 {} 首不能播放的歌", failures_), "skip-forward");
            failures_ = 0;
            std::erase_if(recent_, [&](const Track& x) { return same(x, *current()); });
            recent_.insert(recent_.begin(), *current());
            if (recent_.size() > 200) recent_.pop_back();
            changed();
        }
        if (s == State::paused) changed();
        if (s == State::ended) {
            if (mode_ == Mode::one && !fm_playing_) {
                player_.seek(0);
                player_.play();
            } else {
                skip(1, true);
            }
        }
        if (s == State::failed) {
            const audio::Spool* sp = player_.spool();
            // Stream URLs expire; fetch a new one and carry on from here.
            if (sp && sp->status() == 403 && !retried_) {
                start(last_pos_, want_play_);
                retried_ = true;
            } else {
                notify(std::format("播放失败：{}", player_.error()), "circle-alert", true);
                want_play_ = false;
            }
        }
    }
    last_state_ = s;
    if (silent_ && player_.volume() > 0) apply_volume();
    else if (!silent_ && !muted_ && std::abs(player_.volume() - volume_) > 1e-3f) {
        volume_ = player_.volume();
        changed();
    }
    redraw_();
}

// Drawing helpers.

void Shell::cover(ui::Ui& ui, Rect r, const std::string& url, float radius, std::string_view placeholder) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    int px = Images::tier(r.w * p.scale());
    const auto& e = images_.get(ncm_.cover_url(url, px), uint32_t(px));
    float a = e.ready ? ui.progress(e.ready_at, 320, ease::standard) : 0;
    if (a < 1) {
        p.rect(r, t.text.with_alpha(0.07f), radius);
        if (!placeholder.empty() && r.w >= 28) {
            float s = std::round(std::min(r.w, r.h) * 0.36f);
            p.icon(placeholder, ui::center(r, s, s), t.text_dim.with_alpha(0.5f * (1 - a)));
        }
    }
    if (e.ready) {
        Fade fade(p, a);
        p.image(r, e.tex, radius);
    }
}

// Greedy line breaking at spaces and around CJK characters, from a single shaping pass.
static std::vector<std::string> wrap(ui::Painter& p, const std::string& s, float size, float width) {
    const text::Shaped& sh = p.shape(s, {.size = size});
    if (sh.width * size <= width || sh.glyphs.size() < 2) return {s};
    auto wide = [&](uint32_t byte) {
        size_t i = byte;
        return utf8::decode(s, i) >= 0x2e80;
    };
    std::vector<std::string> out;
    auto emit = [&](size_t from, size_t to) {
        std::string_view v = std::string_view(s).substr(from, to - from);
        while (v.ends_with(' ')) v.remove_suffix(1);
        while (v.starts_with(' ')) v.remove_prefix(1);
        if (!v.empty()) out.emplace_back(v);
    };
    const auto& g = sh.glyphs;
    size_t start = 0, candidate = 0;
    for (size_t i = 1; i < g.size(); ++i) {
        uint32_t c = g[i].cluster, pc = g[i - 1].cluster;
        if (c != pc && c > 0 && (s[c - 1] == ' ' || wide(c) || wide(pc))) candidate = i;
        float end = (i + 1 < g.size() ? g[i + 1].x : sh.width) - g[start].x;
        if (end * size > width && candidate > start) {
            emit(g[start].cluster, g[candidate].cluster);
            start = candidate;
        }
    }
    emit(g[start].cluster, s.size());
    return out;
}

// Layout.

void Shell::top_bar(ui::Ui& ui, Rect bar, float center_x) {
    Rect r = bar;
    Rect nav = ui::cut_left(r, 40);
    {
        Fade fade(ui.painter(), ui.anim(ui.id("back-a"), history_.empty() && !np_open_ ? 0.3f : 1.f, 200));
        if (ui.icon_button(ui.id("back"), ui::center(nav, 32, 32), "chevron-left", 20)) back(ui);
    }
    Rect settings = ui::cut_right(r, 40);
    if (ui.icon_button(ui.id("settings"), ui::center(settings, 32, 32), "settings", 18))
        ui.open_modal(460, 384, [this](ui::Ui& u, Rect body) { settings_modal(u, body); });
    Rect account = ui::cut_right(r, 40);
    if (account_) {
        // Signed in: the avatar replaces the icon.
        Rect a = ui::center(account, 28, 28);
        auto it = ui.interact(ui.id("account"), a.inset(-2));
        float hot = ui.anim(ui.id("account"), it.hovered ? 1 : 0, 150);
        if (hot > 0) ui.painter().rect(a.inset(-3), ui.theme().text.with_alpha(0.12f * hot), 17);
        cover(ui, a, account_->avatar, 14, "user");
        if (it.clicked) open_account(ui);
    } else if (ui.icon_button(ui.id("account"), ui::center(account, 32, 32), "user", 18)) {
        open_account(ui);
    }
    float w = std::min(440.f, r.w - 32);
    float x = std::clamp(center_x - w / 2, r.x + 16, r.right() - 16 - w);
    if (ui.text_box(ui.id("search"), {x, bar.y + (bar.h - 34) / 2, w, 34}, query_, "搜索歌曲、歌手、专辑", "search")) {
        if (np_open_) np_open_ = false, np_time_ = ui.now;
        search(ui);
    }
}

void Shell::open_account(ui::Ui& ui) {
    login_step_ = -1;
    ui.open_modal(420, 330, [this](ui::Ui& u, Rect body) { account_modal(u, body); });
}

void Shell::draw(ui::Ui& ui, Rect area) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    dark_ = t.dark;
    for (auto& m : toasts_) ui.toast(std::move(m.text), std::move(m.icon), m.error);
    toasts_.clear();
    // Closing the account dialog stops the QR polling.
    if (qr_active_ && !ui.modal_open()) ++qr_gen_, qr_active_ = false;

    auto st = player_.state();
    if (st == State::playing || st == State::paused) last_pos_ = player_.position();
    double dur = player_.duration() > 0 ? player_.duration() : current() ? current()->duration : 0;
    // Shortly before the end, fetch the next track's URL and first bytes.
    if (st == State::playing && dur > 0 && dur - last_pos_ < 15 && prefetch_.gen != play_gen_ && mode_ != Mode::one && !fm_playing_) {
        prefetch_.gen = play_gen_;
        if (int i = next_index(); i >= 0 && i != current_) spawn(prefetch(queue_[size_t(i)]));
    }

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

    // The now-playing view rises over the sidebar and the page; once it covers them they are not drawn.
    float np = np_open_ ? ui.progress(np_time_, 460) : 1 - ui.progress(np_time_, 260, ease::exit);
    if (!(np_open_ && np >= 1)) {
        // Content panel with a wash of the page's colour; the page slides in from the side it was reached from.
        p.rect(area, t.surface, t.radius_l());
        float enter = ui.progress(page_time_, 420);
        float hue = route_.page == Page::playlist ? cover_hue(route_.card.cover, 148 * p.scale()) : -1;
        ui::Color tint = hue >= 0 ? hsl(hue, 0.55f, 0.5f) : t.accent;
        if (route_.page != Page::search) p.gradient({area.x, area.y, area.w, 320}, tint.with_alpha(0.16f * enter), tint.with_alpha(0), t.radius_l());
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
                case Page::recent:
                    route_.card.subtitle = std::format("{} 首 · 保存在本机", recent_.size());
                    list_page(ui, r, recent_, false, "", page_time_);
                    break;
                default: {
                    // No list yet: the library is still loading, or the page waits for a login.
                    static List pending{.loading = true};
                    List& list = route_.list ? *route_.list : pending;
                    if (route_.page == Page::liked && route_.list) route_.card.subtitle = std::format("{} 首", list.tracks.size());
                    else if (route_.page == Page::cloud && route_.list) route_.card.subtitle = std::format("上传到云端的歌曲 · {} 首", list.tracks.size());
                    list_page(ui, r, list.tracks, list.loading, list.error, list.since);
                    break;
                }
            }
        }
        ui.pop_clip();
        sidebar(ui, side);
    }
    if (np > 0) now_playing(ui, {side.x + 8, area.y, area.right() - side.x - 8, area.h}, np);
    player_bar(ui, player);
    handle_keys(ui);
    images_.end_frame();

    // Next frame when the progress bar moves a pixel or the time label changes.
    if (playing() && dur > 0) {
        double step = std::min(dur / std::max(progress_w_, 1.f), 1 - std::fmod(last_pos_, 1.0));
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
        if (k->keysym == XKB_KEY_space && !k->repeat) toggle_play();
        else if (k->keysym == XKB_KEY_Escape && np_open_ && !ui.menu_open()) np_open_ = false, np_time_ = ui.now;
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

    float content = item_y(6 + playlists_.size()) + 8 + (playlists_.empty() ? 40 : 0);
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
    auto item = [&](ui::Id id, size_t k, std::string_view label, std::string_view icon, const std::string* thumb) {
        Rect row = row0;
        row.y = r.y + item_y(k) - off;
        auto it = ui.interact(id, row);
        float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
        float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
        bool selected = sel == int(k);
        if (hot > 0 && !selected) p.rect(row.inset(down), t.hover.with_alpha(hot), t.radius_s());
        float ix = row.x + 10 + ((row.w - 20) / 2 - 10) * (1 - labels);
        Rect ic{ix, row.y + 10, 18, 18};
        if (thumb) cover(ui, ic.inset(-2), *thumb, 5, "");
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
        if (item(ui.id(nav[k].label), k, nav[k].label, nav[k].icon, nullptr)) open(ui, nav[k].page);
    ui.push_id(ui.id("playlists"));
    for (size_t i = 0; i < playlists_.size(); ++i)
        if (item(ui.id(uint64_t(i)), 6 + i, playlists_[i].title, {}, &playlists_[i].cover)) open(ui, Page::playlist, playlists_[i], int(i));
    ui.pop_id();
    if (playlists_.empty() && labels > 0.01f)
        ui.label({r.x + 24, r.y + item_y(6) - off, r.w - 48, 20}, account_ ? "还没有歌单" : "登录后显示你的歌单",
                 {.size = t.fs(12), .color = t.text_dim.with_alpha(0.7f * labels)});
    ui.end_scroll();
}

bool Shell::page_status(ui::Ui& ui, Rect r, bool loading, const std::string& error, bool login) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    Rect c = ui::center(r, std::min(r.w, 360.f), 150);
    if (login) {
        Rect ic = {c.x + (c.w - 44) / 2, c.y, 44, 44};
        p.rect(ic, t.text.with_alpha(0.06f), 22);
        p.icon("user", ui::center(ic, 20, 20), t.text_dim);
        ui.label({c.x, c.y + 58, c.w, 22}, "登录网易云音乐后可用", {.size = t.fs(15), .color = t.text}, Align::center);
        if (ui.button(ui.id("page-login"), {c.x + (c.w - 120) / 2, c.y + 96, 120, 36}, "扫码登录", ui::ButtonKind::primary, "qr-code")) {
            open_account(ui);
            login_step_ = 0;
            login_time_ = ui.now;
            spawn(qr_login());
        }
        return true;
    }
    if (loading) {
        ui.spinner(ui::center(r, 24, 24), t.text_dim, 2);
        return true;
    }
    if (!error.empty()) {
        p.icon("cloud-off", {c.x + (c.w - 28) / 2, c.y + 20, 28, 28}, t.text_dim);
        ui.label({c.x, c.y + 64, c.w, 20}, error, {.size = t.fs(13), .color = t.text_dim}, Align::center);
        return true;
    }
    return false;
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
    cover(ui, cov, c.cover, radius, round ? "user" : c.kind == CardKind::album ? "disc-album" : "list-music");
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
    float content = 28 + 76 + hero_h + 40 + 44 + std::max(g.height(recommended_.size()), 120.f) + 32;
    float off = ui.begin_scroll(ui.id("recommend"), r, content);
    float y = r.y + 28 - off;
    page_title(ui, body, y, "推荐", std::format("{}，{}", today_, account_ ? std::format("{}，为你挑了这些", account_->name) : "为你挑了这些"));
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
    if (!page_status(ui, {body.x, y, body.w, 120}, recommended_loading_ && recommended_.empty(), recommended_error_, false))
        if (int i = card_grid(ui, {body.x, y, body.w, 0}, ui.id("rec-cards"), recommended_, false, page_time_ + 0.08); i >= 0)
            open(ui, Page::playlist, recommended_[size_t(i)]);
    ui.end_scroll();
}

void Shell::toplist_page(ui::Ui& ui, Rect r) {
    Rect body = r.inset(32, 0);
    Grid g(body.w);
    float off = ui.begin_scroll(ui.id("toplists"), r, 28 + 84 + g.height(toplists_.size()) + 32);
    float y = r.y + 28 - off;
    page_title(ui, body, y, "排行榜", "官方榜单，每天或每周更新");
    if (!page_status(ui, {body.x, y + 84, body.w, r.h - 140}, toplists_loading_, toplists_error_, false))
        if (int i = card_grid(ui, {body.x, y + 84, body.w, 0}, ui.id("top-cards"), toplists_, false, page_time_ + 0.05); i >= 0)
            open(ui, Page::playlist, toplists_[size_t(i)]);
    ui.end_scroll();
}

void Shell::fm_page(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    page_title(ui, r.inset(32, 0), r.y + 28, "私人 FM", "根据你的收听记录推荐，不喜欢的歌会少出现");
    Rect below{r.x, r.y + 100, r.w, r.h - 100};
    if (page_status(ui, below, fm_loading_ && fm_.empty(), fm_.empty() ? fm_error_ : "", !ncm_.logged_in())) return;
    if (fm_.empty()) return;
    fm_pos_ = std::min(fm_pos_, int(fm_.size()) - 1);
    const Track& tr = fm_[size_t(fm_pos_)];
    bool on_air = is_current(tr);
    float size = std::clamp(std::min(r.w * 0.4f, r.h - 310), 160.f, 280.f);
    Rect col = ui::center(r, size, size + 150);
    col.y += 36;
    Rect cov{col.x, col.y, size, size};
    p.shadow({cov.x, cov.y + 14, cov.w, cov.h}, t.radius_l(), 44, t.shadow);
    cover(ui, cov, tr.cover, t.radius_l());
    ui.label({col.x - 100, cov.bottom() + 22, size + 200, 30}, tr.title, {.size = t.fs(20), .color = t.text}, Align::center);
    ui.label({col.x - 100, cov.bottom() + 54, size + 200, 20}, std::format("{} · {}", tr.artist, tr.album), {.size = t.fs(13), .color = t.text_dim},
             Align::center);
    float by = cov.bottom() + 90, cx = col.x + size / 2;
    // thumbs-down, like, play, next as one centred row
    float x0 = cx - 102;
    if (ui.icon_button(ui.id("fm-trash"), {x0, by + 4, 40, 40}, "thumbs-down", 20)) {
        spawn([](Shell* self, std::string id) -> Task<void> { co_await self->ncm_.fm_trash(std::move(id)); }(this, tr.id));
        notify("以后会少推荐类似的歌", "thumbs-down");
        next_fm();
        return;
    }
    bool like = liked(tr);
    if (ui.icon_button(ui.id("fm-like"), {x0 + 52, by + 4, 40, 40}, like ? "heart-fill" : "heart", 20, like)) toggle_like(tr);
    ui::Id pid = ui.id("fm-play");
    Rect play_r{x0 + 104, by, 48, 48};
    auto it = ui.interact(pid, play_r);
    float hot = ui.anim(pid, it.hovered ? 1 : 0, 150), down = ui.anim(ui::sub_id(pid, 0xd0), it.held ? 1 : 0, 90);
    Rect pr = play_r.inset(-2 * hot + 2 * down);
    p.rect(pr, t.accent.mix(white, 0.1f * hot), pr.w / 2);
    p.icon(on_air && playing() ? "pause-fill" : "play-fill", ui::center(pr, 20, 20), white);
    if (it.clicked) {
        if (on_air) toggle_play();
        else play(fm_, fm_pos_);
    }
    if (ui.icon_button(ui.id("fm-next"), {x0 + 164, by + 4, 40, 40}, "skip-forward", 20)) {
        fm_playing_ = fm_playing_ || on_air;
        next_fm();
    }
}

void Shell::search_page(ui::Ui& ui, Rect r) {
    Rect body = r.inset(32, 0);
    Results& res = results_[search_tab_];
    static constexpr const char* kinds[] = {"首单曲", "张专辑", "位歌手", "个歌单"};
    page_title(ui, body, r.y + 28, std::format("“{}”", searched_), res.loaded ? std::format("找到 {} {}", res.total, kinds[search_tab_]) : "正在搜索");
    static constexpr std::string_view tabs[] = {"单曲", "专辑", "歌手", "歌单"};
    int before = search_tab_;
    if (ui.tabs(ui.id("search-tabs"), {body.x, r.y + 100, body.w, 36}, tabs, search_tab_)) {
        selected_ = -1;
        tab_time_ = ui.now;
        tab_dir_ = search_tab_ > before ? 1 : -1;
        load_results(search_tab_);
    }
    // Tab content slides toward the tab that was picked.
    float enter = ui.progress(tab_time_, 380);
    Fade fade(ui.painter(), enter);
    Rect content{r.x + 16 + (1 - enter) * 36 * float(tab_dir_), r.y + 148, r.w - 32, r.h - 156};
    Results& shown = results_[search_tab_];
    double since = std::max({page_time_, tab_time_, shown.since});
    bool empty = shown.tracks.empty() && shown.cards.empty();
    std::string error = empty ? shown.error : "";
    if (empty && shown.loaded && error.empty()) error = "没有找到相关结果";
    if (empty && page_status(ui, content, shown.loading, error, false)) return;
    if (search_tab_ == 0) {
        if (track_list(ui, content, ui.id("search-list"), shown.tracks, since)) load_results(0);
        return;
    }
    static constexpr const char* ids[] = {"", "search-albums", "search-artists", "search-playlists"};
    const std::vector<Card>& cards = shown.cards;
    bool round = search_tab_ == 2;
    Rect inner = content.inset(12, 0);
    Grid g(inner.w, round ? 130 : 150);
    ui::Id id = ui.id(ids[search_tab_]);
    float off = ui.begin_scroll(id, content, 12 + g.height(cards.size()) + 20);
    if (int i = card_grid(ui, {inner.x, content.y + 12 - off, inner.w, 0}, id, cards, round, since); i >= 0) {
        Card c = cards[size_t(i)];
        if (round) c.subtitle = "热门 50 首";
        open(ui, Page::playlist, c);
    }
    ui.end_scroll();
}

void Shell::list_page(ui::Ui& ui, Rect r, std::vector<Track>& tracks, bool loading, const std::string& list_error, double since) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    const Card& c = route_.card;
    Rect head{r.x + 32, r.y + 32, r.w - 64, 148};
    Rect cov{head.x, head.y, 148, 148};
    p.shadow({cov.x, cov.y + 10, cov.w, cov.h}, c.kind == CardKind::artist ? 74 : t.radius_l(), 32, t.shadow.with_alpha(0.8f));
    switch (route_.page) {
        case Page::liked: art(p, cov, special_hue(Page::liked), t.radius_l(), "heart-fill"); break;
        case Page::recent: art(p, cov, special_hue(Page::recent), t.radius_l(), "history"); break;
        case Page::cloud: art(p, cov, special_hue(Page::cloud), t.radius_l(), "cloud"); break;
        case Page::daily: {
            art(p, cov, 0.98f, t.radius_l(), "");
            std::string day = today_.substr(today_.find("月") + 4);
            ui.label(cov, day.substr(0, day.find(' ')), {.size = t.fs(54), .color = white}, Align::center);
            break;
        }
        default: cover(ui, cov, c.cover, c.kind == CardKind::artist ? 74 : t.radius_l(), c.kind == CardKind::artist ? "user" : "list-music");
    }
    float tx = cov.right() + 28, tw = head.right() - tx;
    static constexpr const char* kinds[] = {"歌单", "专辑", "歌手"};
    if (route_.page == Page::playlist) ui.label({tx, head.y + 2, tw, 18}, kinds[int(c.kind)], {.size = t.fs(12), .color = t.text_dim});
    ui.label({tx, head.y + 18, tw, 38}, c.title, {.size = t.fs(28), .color = t.text});
    ui.label({tx, head.y + 62, tw, 20}, c.subtitle, {.size = t.fs(13), .color = t.text_dim});
    if (ui.button(ui.id("play-all"), {tx, head.bottom() - 40, 124, 40}, "播放全部", ui::ButtonKind::primary, "play-fill") && !tracks.empty()) {
        if (mode_ == Mode::shuffle) mode_ = Mode::list;
        play(tracks, 0);
    }
    if (ui.button(ui.id("shuffle-all"), {tx + 136, head.bottom() - 40, 112, 40}, "随机播放", ui::ButtonKind::secondary, "shuffle") &&
        !tracks.empty()) {
        mode_ = Mode::shuffle;
        play(tracks, int(next(rng_) % tracks.size()));
    }
    Rect body{r.x + 16, head.bottom() + 28, r.w - 32, r.bottom() - head.bottom() - 36};
    bool login = needs_login(route_.page) && !ncm_.logged_in();
    std::string error = list_error;
    if (tracks.empty() && error.empty() && !loading && !login) error = "这里还没有歌曲";
    if (tracks.empty() && page_status(ui, body, loading, error, login)) return;
    track_list(ui, body, ui.id("tracks"), tracks, std::max(page_time_ + 0.06, since));
}

bool Shell::track_list(ui::Ui& ui, Rect r, ui::Id id, std::vector<Track>& tracks, double since) {
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
        float a = ui.progress(since + 0.022 * std::max(0, i - first), 380);
        Fade fade(p, a);
        track_row(ui, {r.x, r.y + float(i) * row_h - off + (1 - a) * 14, r.w, row_h}, tracks, i);
    }
    ui.pop_id();
    ui.end_scroll();
    return last + 10 >= int(tracks.size());
}

void Shell::track_row(ui::Ui& ui, Rect r, std::vector<Track>& tracks, int i) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    const Track& tr = tracks[size_t(i)];
    ui.push_id(ui.id(uint64_t(i)));
    ui::Id row_id = ui.id("row");
    bool hot = ui.hovering(r) || ui.is_active(row_id);
    bool now_playing = is_current(tr);
    float h = ui.anim(row_id, hot ? 1 : 0, 140);
    float s = ui.anim(ui::sub_id(row_id, 0x5e), selected_ == i ? 1 : 0, 200);
    Rect bg = r.inset(4, 2);
    if (h > 0 || s > 0) p.rect(bg, t.text.with_alpha(0.06f * h + 0.05f * s), t.radius_s());

    Columns c = columns(r);
    TextStyle num{.size = t.fs(13), .color = t.text_dim, .features = text::tabular_nums};
    if (hot) {
        if (ui.icon_button(ui.id("play"), ui::center(c.index, 30, 30), now_playing && playing() ? "pause-fill" : "play-fill", 14)) {
            if (now_playing) toggle_play();
            else play(tracks, i);
        }
    } else if (now_playing) {
        p.icon(playing() ? "volume-2" : "pause", ui::center(c.index, 16, 16), t.accent);
    } else {
        ui.label(c.index, std::to_string(i + 1), num, Align::center);
    }
    Rect cov{c.title.x, r.y + (r.h - 40) / 2, 40, 40};
    cover(ui, cov, tr.cover, t.radius_s());
    float tx = cov.right() + 14, tw = c.title.right() - tx - 8;
    float w = p.text(tx, r.y + 25, tr.title, {.size = t.fs(14), .color = now_playing ? t.accent : t.text}, tw);
    if (!tr.alias.empty() && w + 24 < tw) p.text(tx + w + 6, r.y + 25, std::format("（{}）", tr.alias), {.size = t.fs(14), .color = t.text_dim}, tw - w - 6);
    p.text(tx, r.y + 43, tr.artist, {.size = t.fs(12), .color = t.text_dim}, tw);
    if (c.album.w > 0) ui.label(c.album, tr.album, {.size = t.fs(13), .color = t.text_dim});
    bool like = liked(tr);
    if ((hot || like) && ui.icon_button(ui.id("like"), ui::center(c.like, 32, 32), like ? "heart-fill" : "heart", 16, like)) toggle_like(tr);
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
                          if (current_ < 0) {
                              queue_.insert(queue_.begin(), tr);
                              current_ = 0;
                              resume_at_ = 0;
                          } else {
                              queue_.insert(queue_.begin() + (current_ + 1), tr);
                          }
                          shuffle_next_ = mode_ == Mode::shuffle ? current_ + 1 : -1;
                          shuffle_history_.clear();
                          changed();
                          ui.toast("已添加到下一首播放", "list-plus");
                      }},
                     {"disc-album", "查看专辑",
                      [this, &ui, tr] {
                          open(ui, Page::playlist, {.kind = CardKind::album, .id = tr.album_id, .title = tr.album, .subtitle = tr.artist, .cover = tr.cover});
                      }},
                     {"mic-vocal", "查看歌手",
                      [this, &ui, tr] {
                          std::string name = tr.artist.substr(0, tr.artist.find(" / "));
                          open(ui, Page::playlist, {.kind = CardKind::artist, .id = tr.artist_id, .title = name, .subtitle = "热门 50 首"});
                      }},
                     {},
                     {like ? "heart-off" : "heart", like ? "取消喜欢" : "喜欢", [this, tr] { toggle_like(tr); }},
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
        ++play_gen_;
        want_play_ = fetching_ = false;
        player_.stop();
        shuffle_history_.clear();
        changed();
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
        cover(ui, {row.x + 10, row.y + 9, 36, 36}, tr.cover, t.radius_s());
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
        shuffle_history_.clear();
        shuffle_next_ = -1;
        if (remove < current_) {
            --current_;
        } else if (remove == current_) {
            // The playing track goes: carry on with the one after it.
            bool was_playing = playing();
            current_ = queue_.empty() ? -1 : std::min(current_, int(queue_.size()) - 1);
            ++play_gen_;
            player_.stop();
            fetching_ = false;
            if (current_ >= 0 && was_playing) start();
            else want_play_ = false;
        }
        changed();
    }
}

void Shell::player_bar(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    bool compact = r.w < 900;
    float side_w = std::min(330.f, std::round(r.w * 0.3f));
    Rect left = ui::cut_left(r, side_w), right = ui::cut_right(r, side_w), mid = r;
    const Track* tr = current();
    auto st = player_.state();

    // Now playing; the cover cross-fades and the text rises in after a track change. The cover opens the
    // now-playing view.
    Rect cov{left.x + 16, left.y + 14, 56, 56};
    bar_cover_px_ = cov.w * p.scale();
    float k = ui.progress(cover_time_, 520);
    if (tr) {
        ui::Id cid = ui.id("bar-cover");
        auto it = ui.interact(cid, cov);
        float hot = ui.anim(cid, it.hovered ? 1 : 0, 150);
        p.shadow({cov.x, cov.y + 4, cov.w, cov.h}, t.radius_s(), 12, t.shadow.with_alpha(0.6f));
        if (k < 1 && !prev_cover_.empty()) cover(ui, cov, prev_cover_, t.radius_s());
        {
            Fade fade(p, k);
            cover(ui, cov, tr->cover, t.radius_s());
        }
        if (hot > 0) {
            p.rect(cov, ui::Color{0, 0, 0, 0.45f * hot}, t.radius_s());
            p.icon(np_open_ ? "chevron-down" : "chevron-up", ui::center(cov, 22, 22), white.with_alpha(hot));
        }
        if (it.clicked) np_open_ = !np_open_, np_time_ = ui.now;
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
        if (ui.icon_button(ui.id("bar-like"), {hx, left.y + 26, 32, 32}, like ? "heart-fill" : "heart", 17, like)) toggle_like(*tr);
    }

    // Transport; play and pause morph into each other, a ring turns while the track loads or buffers.
    float cx = std::round(mid.x + mid.w / 2), cy = mid.y + 30;
    static constexpr const char* mode_icons[] = {"repeat", "repeat-1", "shuffle"};
    static constexpr const char* mode_names[] = {"列表循环", "单曲循环", "随机播放"};
    if (ui.icon_button(ui.id("mode"), {cx - 124, cy - 16, 32, 32}, mode_icons[int(mode_)], 17, mode_ != Mode::list)) {
        mode_ = Mode((int(mode_) + 1) % 3);
        shuffle_next_ = -1;
        shuffle_history_.clear();
        changed();
        ui.toast(mode_names[int(mode_)], mode_icons[int(mode_)]);
    }
    if (ui.icon_button(ui.id("prev"), {cx - 72, cy - 16, 32, 32}, "skip-back", 18)) skip(-1);
    ui::Id pid = ui.id("play");
    Rect play_r{cx - 21, cy - 21, 42, 42};
    auto it = ui.interact(pid, play_r);
    float hot = ui.anim(pid, it.hovered ? 1 : 0, 150), down = ui.anim(ui::sub_id(pid, 0xd0), it.held ? 1 : 0, 90);
    Rect pr = play_r.inset(-2 * hot + 2.5f * down);
    p.shadow({pr.x, pr.y + 3, pr.w, pr.h}, pr.w / 2, 10, t.shadow.with_alpha(0.5f * hot));
    p.rect(pr, t.text, pr.w / 2);
    float morph = ui.anim(ui.id("play-morph"), playing() ? 1 : 0, 260, ease::emphasized);
    {
        Fade fade(p, 1 - morph);
        p.icon("play-fill", ui::center(pr, 18 - 6 * morph, 18 - 6 * morph), t.bg);
    }
    {
        Fade fade(p, morph);
        p.icon("pause-fill", ui::center(pr, 12 + 6 * morph, 12 + 6 * morph), t.bg);
    }
    bool waiting = want_play_ && (fetching_ || st == State::loading || player_.buffering());
    float ring = ui.anim(ui.id("play-ring"), waiting ? 1 : 0, 200);
    if (ring > 0.01f) {
        Fade fade(p, ring);
        ui.spinner(pr.inset(-5), t.accent, 2);
    }
    if (it.clicked) toggle_play();
    if (ui.icon_button(ui.id("next"), {cx + 40, cy - 16, 32, 32}, "skip-forward", 18)) skip(1);
    if (ui.icon_button(ui.id("lyrics"), {cx + 92, cy - 16, 32, 32}, "mic-vocal", 17, np_open_) && tr) np_open_ = !np_open_, np_time_ = ui.now;

    // Progress: a sine wave runs through the played part while the music plays and settles flat on pause.
    // Dragging previews the position and seeks on release; the lighter track shows what is downloaded.
    TextStyle ts{.size = t.fs(11), .color = t.text_dim, .features = text::tabular_nums};
    float py = mid.y + 66;
    double dur = player_.duration() > 0 ? player_.duration() : tr ? tr->duration : 0;
    double pos = st == State::idle && !fetching_ ? resume_at_ : last_pos_;
    Rect bar{mid.x + 52, py - 9, mid.w - 104, 18};
    progress_w_ = bar.w;
    std::vector<std::pair<float, float>> buffered;
    if (const audio::Spool* sp = player_.spool()) {
        auto [have, size] = sp->progress();
        if (size && have < size)
            for (auto [b, e] : sp->ranges()) buffered.emplace_back(float(double(b) / double(size)), float(double(e) / double(size)));
    }
    ui::Id prog = ui.id("progress");
    float v = dragging_ ? drag_v_ : dur > 0 ? float(std::min(pos / dur, 1.0)) : 0;
    float wave = ui.anim(ui.id("wave"), st == State::playing ? 3.f : 0.f, 600, ease::standard);
    bool moved = ui.slider(prog, bar, v, {.wave = wave, .marked = buffered});
    if (tr && dur > 0 && (moved || ui.is_active(prog))) dragging_ = true, drag_v_ = v;
    if (dragging_ && !ui.is_active(prog)) {
        dragging_ = false;
        double at = drag_v_ * dur;
        if (st == State::idle || st == State::failed) {
            if (fetching_) last_pos_ = at;
            else resume_at_ = last_pos_ = at;
        } else {
            player_.seek(at);
            last_pos_ = at;
        }
        changed();
    }
    ui.label({mid.x, py - 8, 44, 16}, mmss(dragging_ ? drag_v_ * dur : tr ? pos : 0), ts, Align::right);
    ui.label({bar.right() + 8, py - 8, 44, 16}, mmss(dur), ts);

    // Volume and panels; narrow windows keep only the icons.
    float vol_w = ui.anim(ui.id("vol-w"), compact ? 0 : 100, 320, ease::emphasized);
    Rect vol{right.right() - 20 - vol_w, cy - 8, vol_w, 16};
    float shown = muted_ ? 0 : volume_;
    if (vol_w > 8) {
        Fade fade(p, vol_w / 100);
        if (ui.slider(ui.id("volume"), vol, shown, {.accent = false})) {
            volume_ = shown, muted_ = false;
            apply_volume();
            changed();
        }
    }
    const char* vol_icon = shown == 0 ? "volume-x" : shown < 0.5f ? "volume-1" : "volume-2";
    if (ui.icon_button(ui.id("mute"), {vol.x - 40, cy - 16, 32, 32}, vol_icon, 18)) {
        muted_ = !muted_;
        apply_volume();
        changed();
    }
    if (ui.icon_button(ui.id("queue"), {vol.x - 80, cy - 16, 32, 32}, "list-music", 18, queue_open_)) queue_open_ = !queue_open_;
}

void Shell::now_playing(ui::Ui& ui, Rect r, float k) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    Fade fade(p, k);
    r.y += (1 - k) * 56;
    const Track* tr = current();
    float radius = t.radius_l();

    // Background: the cover, blurred and stretched to fill, under a veil that keeps the text readable.
    p.rect(r, t.bg.mix(t.surface, 0.5f), radius);
    float aspect = r.w / r.h;
    Rect uv = aspect > 1 ? Rect{0, (1 - 1 / aspect) / 2, 1, 1 / aspect} : Rect{(1 - aspect) / 2, 0, aspect, 1};
    auto backdrop = [&](const std::string& url, float a) {
        const auto& e = images_.get(ncm_.cover_url(url, 128), 128, true);
        if (!e.ready) return;
        Fade f(p, a * ui.progress(e.ready_at, 600, ease::standard));
        p.image(r, e.tex, radius, white, uv);
    };
    float change = ui.progress(cover_time_, 800, ease::standard);
    if (change < 1 && !prev_cover_.empty()) backdrop(prev_cover_, 1 - change);
    if (tr) backdrop(tr->cover, change);
    p.gradient(r, t.bg.with_alpha(0.42f), t.bg.with_alpha(0.78f), radius);

    // Over a bright cover a plain icon button disappears, so this one keeps a backing circle.
    Rect close{r.x + 16, r.y + 16, 36, 36};
    ui::Id cid = ui.id("np-close");
    auto cl = ui.interact(cid, close);
    float hot = ui.anim(cid, cl.hovered ? 1 : 0, 150);
    float down = ui.anim(ui::sub_id(cid, 0xd0), cl.held && cl.hovered ? 1 : 0, 90);
    p.rect(close.inset(down * 2), white.with_alpha(0.12f + 0.1f * hot), 18 - down * 2);
    p.icon("chevron-down", ui::center(close, 22 - down * 1.5f, 22 - down * 1.5f), t.text);
    if (cl.clicked) np_open_ = false, np_time_ = ui.now;
    if (!tr) {
        ui.label(r, "没有正在播放的歌曲", {.size = t.fs(14), .color = t.text_dim}, Align::center);
        return;
    }

    Rect body = r.inset(56, 44);
    bool wide = body.w >= 720;
    std::string format;
    if (!player_.codec().empty())
        format = std::format("{} · {:.1f} kHz", player_.codec(), player_.format().rate / 1000.0);
    if (wide) {
        Rect left = ui::cut_left(body, std::round(body.w * 0.42f));
        ui::cut_left(body, 40);
        float cs = std::floor(std::min({left.w - 20, left.h - 120, 420.f}));
        Rect cov{left.x + (left.w - cs) / 2, left.y + (left.h - cs - 110) / 2, cs, cs};
        float in = ui.progress(cover_time_, 520);
        cov.y += (1 - in) * 10;
        p.shadow({cov.x, cov.y + 18, cov.w, cov.h}, radius, 48, t.shadow.with_alpha(0.9f));
        cover(ui, cov, tr->cover, radius);
        Fade f(p, in);
        p.text(cov.x, cov.bottom() + 46, tr->title, {.size = t.fs(22), .color = t.text}, cs);
        p.text(cov.x, cov.bottom() + 72, tr->album.empty() ? tr->artist : std::format("{} · {}", tr->artist, tr->album),
               {.size = t.fs(14), .color = t.text_dim}, cs);
        if (!format.empty()) p.text(cov.x, cov.bottom() + 96, format, {.size = t.fs(12), .color = t.text_dim.with_alpha(0.7f)}, cs);
        lyrics_view(ui, body, false);
    } else {
        ui.label({body.x, body.y - 4, body.w, 30}, tr->title, {.size = t.fs(20), .color = t.text}, Align::center);
        ui.label({body.x, body.y + 28, body.w, 20}, tr->artist, {.size = t.fs(13), .color = t.text_dim}, Align::center);
        lyrics_view(ui, {body.x, body.y + 76, body.w, body.h - 76}, true);
    }
}

void Shell::lyrics_view(ui::Ui& ui, Rect r, bool centered) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    TextStyle main{.size = t.fs(20)}, sub{.size = t.fs(14)};
    const float main_lh = std::round(main.size * 1.5f), sub_lh = std::round(sub.size * 1.5f), gap = 18;
    if (lyrics_loading_) {
        ui.spinner(ui::center(r, 22, 22), t.text_dim, 2);
        return;
    }
    if (lyrics_.empty()) {
        ui.label(r, "暂无歌词", {.size = t.fs(16), .color = t.text_dim}, centered ? Align::center : Align::left);
        return;
    }
    if (lyric_layout_.size() != lyrics_.size() || lyric_layout_w_ != r.w || lyric_layout_size_ != main.size || lyric_layout_font_ != config_.theme.font) {
        lyric_layout_.clear();
        float y = 0;
        for (const auto& l : lyrics_) {
            LyricBlock b{.lines = wrap(p, l.text, main.size, r.w), .y = y};
            if (!l.translation.empty()) b.translation = wrap(p, l.translation, sub.size, r.w);
            b.h = float(b.lines.size()) * main_lh + float(b.translation.size()) * sub_lh;
            y += b.h + gap;
            lyric_layout_.push_back(std::move(b));
        }
        lyric_layout_w_ = r.w;
        lyric_layout_size_ = main.size;
        lyric_layout_font_ = config_.theme.font;
    }
    const auto& blocks = lyric_layout_;
    float total = blocks.back().y + blocks.back().h;
    // A little ahead, so the highlight lands with the voice rather than after it.
    int cur = lyric_at(lyrics_, clip_start_ + last_pos_ + 0.2);
    const LyricBlock& focus = blocks[size_t(std::max(cur, 0))];
    float anchor = r.h * 0.4f;
    float target = focus.y + focus.h / 2 - anchor;

    // The wheel browses freely for a few seconds, then the view glides back to the current line.
    if (ui.hovering(r) && ui.input().scroll_y != 0) {
        lyric_y_ = std::clamp(lyric_y_ + ui.input().scroll_y, -anchor, total - anchor);
        ui.input().scroll_y = 0;
        lyric_manual_until_ = ui.now + 3;
        ui.redraw();
    }
    if (ui.now >= lyric_manual_until_) {
        double dt = std::clamp(ui.now - lyric_last_, 0.0, 0.1);
        lyric_y_ += (target - lyric_y_) * float(1 - std::exp(-dt / 0.14));
        if (std::abs(target - lyric_y_) > 0.5f) ui.keep_animating();
        else lyric_y_ = target;
    } else {
        ui.wake(lyric_manual_until_);
    }
    lyric_last_ = ui.now;

    ui.push_clip({r.x - 12, r.y, r.w + 24, r.h});
    ui.push_id(ui.id("lyrics"));
    for (size_t i = 0; i < blocks.size(); ++i) {
        const LyricBlock& b = blocks[i];
        float top = r.y + b.y - lyric_y_;
        if (top + b.h < r.y || top > r.bottom()) continue;
        ui::Id id = ui.id(uint64_t(i));
        Rect hit{r.x - 12, top - gap / 2, r.w + 24, b.h + gap};
        auto it = ui.interact(id, hit);
        float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
        float on = ui.anim(ui::sub_id(id, 1), int(i) == cur ? 1 : 0, 420, ease::standard);
        // Lines fade with distance from the current one and towards the edges.
        float d = float(std::abs(int(i) - std::max(cur, 0)));
        float base = std::max(0.2f, 0.5f - 0.06f * (d - 1));
        float edge = std::min(top - r.y, r.bottom() - top - b.h);
        float a = (base + (1 - base) * on) * std::clamp(edge / 80, 0.f, 1.f);
        if (hot > 0) p.rect(hit, t.text.with_alpha(0.06f * hot), t.radius_s());
        float y = top;
        auto line = [&](const std::string& s, const TextStyle& st, float lh, ui::Color c) {
            float w = centered ? std::min(p.measure(s, st), r.w) : 0;
            p.text(centered ? r.x + (r.w - w) / 2 : r.x, y + lh * 0.72f, s, {.size = st.size, .color = c}, r.w);
            y += lh;
        };
        for (const auto& s : b.lines) line(s, main, main_lh, t.text.with_alpha(a));
        for (const auto& s : b.translation) line(s, sub, sub_lh, t.text.with_alpha(a * 0.75f));
        if (it.clicked) {
            last_pos_ = std::max(0.0, lyrics_[i].time - clip_start_);
            player_.seek(last_pos_);
            lyric_manual_until_ = 0;
        }
    }
    ui.pop_id();
    ui.pop_clip();
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
            bool signed_in = i == 0 && account_;
            if (signed_in) cover(ui, avatar, account_->avatar, 20, "user");
            else {
                p.rect(avatar, t.text.with_alpha(0.08f), 20);
                p.icon("user", ui::center(avatar, 18, 18), t.text_dim);
            }
            ui.label({avatar.right() + 14, row.y + 12, 180, 22}, services[i], {.size = t.fs(15), .color = t.text});
            std::string status = signed_in ? account_->name : i == 0 && ncm_.logged_in() ? "正在读取账号" : "未登录";
            ui.label({avatar.right() + 14, row.y + 34, 180, 18}, status, {.size = t.fs(12), .color = signed_in ? t.accent : t.text_dim});
            ui::Id id = ui.id(services[i]);
            Rect b{row.right() - 108, row.y + 15, 94, 34};
            if (i == 0 && ncm_.logged_in()) {
                if (ui.button(id, b, "退出登录", ui::ButtonKind::ghost, "log-out")) spawn(logout());
            } else if (ui.button(id, b, "扫码登录", ui::ButtonKind::secondary, "qr-code")) {
                if (i == 1) {
                    ui.toast("QQ 音乐会在之后的版本接入", "info");
                } else {
                    login_step_ = i;
                    login_time_ = ui.now;
                    spawn(qr_login());
                }
            }
        }
        ui.label({r.x, r.bottom() - 18, r.w, 18}, "凭据加密保存在本机，只有你的账户能解开", {.size = t.fs(12), .color = t.text_dim});
        return;
    }

    ui.resize_modal(400, 540);
    if (ui.icon_button(ui.id("login-back"), {r.x - 8, r.y - 2, 32, 32}, "arrow-left", 18)) {
        login_step_ = -1;
        login_time_ = ui.now;
        ++qr_gen_;
        qr_active_ = false;
        return;
    }
    ui.label({r.x + 32, r.y, r.w - 64, 30}, "扫码登录", {.size = t.fs(20), .color = t.text});
    ui.label({r.x, r.y + 40, r.w, 20}, std::format("用{} App 扫一扫", services[login_step_]), {.size = t.fs(13), .color = t.text_dim},
             Align::center);
    Rect qr = ui::center({r.x, r.y + 76, r.w, 240}, 228, 228);
    p.shadow({qr.x, qr.y + 8, qr.w, qr.h}, 14, 30, t.shadow.with_alpha(0.6f));
    p.rect(qr, white, 14);
    QrState state = qr_status_ ? qr_status_->state : QrState::waiting;
    if (qr_size_ > 0 && !qr_.empty()) {
        // Modules as horizontal runs, so a row is a few rectangles.
        Rect q = qr.inset(14);
        float m = q.w / float(qr_size_);
        ui::Color ink{0.07f, 0.07f, 0.08f, 1};
        for (int y = 0; y < qr_size_; ++y)
            for (int x = 0; x < qr_size_;) {
                if (!qr_[size_t(y * qr_size_ + x)]) {
                    ++x;
                    continue;
                }
                int x1 = x;
                while (x1 < qr_size_ && qr_[size_t(y * qr_size_ + x1)]) ++x1;
                p.rect({q.x + float(x) * m, q.y + float(y) * m, float(x1 - x) * m, m}, ink);
                x = x1;
            }
        Rect logo = ui::center(qr, 40, 40);
        p.rect(logo.inset(-4), white, 12);
        p.rect(logo, t.accent, 10);
        p.icon("music", ui::center(logo, 22, 22), white);
        // Scanned or expired: the code fades under the state.
        float veil = ui.anim(ui.id("qr-veil"), state == QrState::scanned || state == QrState::expired ? 1 : 0, 240);
        if (veil > 0) {
            p.rect(qr, white.with_alpha(0.9f * veil), 14);
            Fade f(p, veil);
            if (state == QrState::scanned && !qr_status_->avatar.empty()) cover(ui, ui::center(qr, 72, 72), qr_status_->avatar, 36, "user");
            else p.icon(state == QrState::expired ? "rotate-ccw" : "circle-check", ui::center(qr, 48, 48), ink.with_alpha(0.7f));
        }
    } else if (qr_error_.empty()) {
        ui.spinner(ui::center(qr, 28, 28), ui::Color{0.07f, 0.07f, 0.08f, 0.6f}, 2.5f);
    }

    float sy = qr.bottom() + 30;
    TextStyle st{.size = t.fs(14), .color = t.text};
    std::string line1, line2;
    bool spin = false;
    if (!qr_error_.empty()) line1 = "没有拿到二维码", line2 = qr_error_;
    else if (qr_.empty()) line1 = "正在生成二维码", spin = true;
    else if (state == QrState::waiting) line1 = "等待扫码", line2 = "扫码后在手机上确认登录", spin = true;
    else if (state == QrState::scanned) line1 = "已扫码", line2 = std::format("{}，请在手机上确认登录", qr_status_->nickname), spin = true;
    else if (state == QrState::expired) line1 = "二维码已过期", line2 = "刷新后重新扫码";
    else line1 = "正在登录", spin = true;
    float tw = p.measure(line1, st);
    float sx = r.x + (r.w - tw - (spin ? 26 : 0)) / 2;
    if (spin) ui.spinner({sx, sy + 2, 16, 16}, t.accent, 2);
    ui.label({sx + (spin ? 26 : 0), sy, tw + 1, 20}, line1, st);
    ui.label({r.x, sy + 30, r.w, 20}, line2, {.size = t.fs(12), .color = t.text_dim}, Align::center);
    if (ui.button(ui.id("qr-refresh"), {r.x + (r.w - 132) / 2, r.bottom() - 36, 132, 34}, "刷新二维码", ui::ButtonKind::secondary, "refresh-cw"))
        spawn(qr_login());
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
