#include "app/shell.hpp"

#include <linux/input-event-codes.h>
#include <qrencode.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <ranges>

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

static std::string short_count(int n) { return n >= 10000 ? std::format("{:.1f}万", n / 1e4) : std::to_string(n); }

// A page of a listing arrived; an empty one ends it, whatever the total said.
static void arrived(auto& res, Result<Batch> r, double now) {
    res.loading = false;
    if (!r) {
        res.error = r.error();
        return;
    }
    if (!res.loaded) res.since = now;
    res.loaded = true;
    bool empty = r->tracks.empty() && r->cards.empty();
    res.tracks.insert(res.tracks.end(), std::make_move_iterator(r->tracks.begin()), std::make_move_iterator(r->tracks.end()));
    res.cards.insert(res.cards.end(), std::make_move_iterator(r->cards.begin()), std::make_move_iterator(r->cards.end()));
    res.total = empty ? int(res.size()) : std::max(r->total, int(res.size()));
}

// Puts a card first in a list or takes it out.
static void place(std::vector<Card>& cards, const Card& c, bool on) {
    std::erase_if(cards, [&](const Card& x) { return same(x, c); });
    if (on) cards.insert(cards.begin(), c);
}

static Result<std::vector<Card>> combined(Result<std::vector<Card>> a, Result<std::vector<Card>> b) {
    if (a && b) return interleave(*a, *b);
    if (a) return std::move(*a);
    if (b) return std::move(*b);
    return std::unexpected(a.error() + "；" + b.error());
}

static Result<std::vector<Track>> combined(Result<std::vector<Track>> a, Result<std::vector<Track>> b) {
    if (a && b) return merge_tracks(*a, *b);
    if (a) return std::move(*a);
    if (b) return std::move(*b);
    return std::unexpected(a.error() + "；" + b.error());
}

static Task<Result<std::vector<Card>>> collection(Provider& p, bool albums) {
    if (!p.logged_in()) co_return std::vector<Card>{};
    if (albums) co_return co_await p.collected_albums();
    co_return co_await p.followed_artists();
}

static Task<Result<std::vector<Track>>> library_tracks(Provider& p, Card card) {
    if (card.id.empty()) co_return std::vector<Track>{};
    co_return co_await p.tracks(std::move(card));
}

// A filter pill.
static float chip_w(ui::Ui& ui, std::string_view label, bool icon = false) {
    return ui.painter().measure(label, {.size = ui.theme().fs(13)}) + 28 + (icon ? 18 : 0);
}

static bool chip(ui::Ui& ui, float x, float y, std::string_view label, bool selected, std::string_view icon = {}) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    Rect c{x, y, chip_w(ui, label, !icon.empty()), 30};
    ui::Id id = ui.id(label);
    auto it = ui.interact(id, c);
    float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
    float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
    c = c.inset(down * 1.5f);
    p.rect(c, selected ? t.accent.with_alpha(0.2f + 0.06f * hot) : t.text.with_alpha(0.06f + 0.05f * hot), c.h / 2);
    ui::Color fg = selected ? t.accent : t.text_dim.mix(t.text, hot);
    TextStyle st{.size = t.fs(13), .color = fg};
    float tw = p.measure(label, st);
    ui.label({c.x + 14, c.y, tw + 1, c.h}, label, st);
    if (!icon.empty()) p.icon(icon, {c.x + 18 + tw, c.y + (c.h - 14) / 2, 14, 14}, fg);
    return it.clicked;
}

// State.

Shell::Shell(ShellDeps d, SavedState saved)
    : loop_(d.loop),
      http_(d.http),
      player_(d.player),
      ncm_(d.ncm),
      qq_(d.qq),
      images_(d.images),
      config_(d.config),
      set_setting_(std::move(d.set_setting)),
      redraw_(std::move(d.redraw)),
      now_(std::move(d.now)),
      silent_(d.silent) {
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
    dl_x_ = saved.lyrics_x, dl_bottom_ = std::max(0.f, saved.lyrics_bottom), dl_locked_ = saved.lyrics_locked;
    rng_ ^= uint64_t(now);

    player_.on_change = [this] { on_player(); };
    apply_volume();
    ncm_.client.on_cookies = [this] { save_credentials(); };
    qq_.client.on_session = [this] { save_credentials(); };
    if (current()) spawn(load_lyrics(*current()));
    spawn(startup());

    if (!d.bus.get()) return;
    Mpris::Handlers h{
        .raise = std::move(d.raise),
        .quit = [this] { loop_.quit(); },
        .play = [this] { if (!playing()) toggle_play(); },
        .pause = [this] { if (playing()) toggle_play(); },
        .toggle = [this] { toggle_play(); },
        .stop = [this] { stop(); },
        .next = [this] { skip(1); },
        .previous = [this] { skip(-1); },
        .seek = [this](double at) { seek(at); },
        .volume =
            [this](double v) {
                volume_ = float(v), muted_ = false;
                apply_volume();
                changed();
            },
        .loop_one =
            [this](bool on) {
                if (on) set_mode(Mode::one);
                else if (mode_ == Mode::one) set_mode(Mode::list);
            },
        .shuffle =
            [this](bool on) {
                if (on) set_mode(Mode::shuffle);
                else if (mode_ == Mode::shuffle) set_mode(Mode::list);
            },
        .position = [this] { return position(); },
    };
    mpris_ = Mpris::create(d.bus, std::move(h));
    publish();
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
    SavedState s{.volume = volume_,
                 .muted = muted_,
                 .mode = int(mode_),
                 .queue = queue_,
                 .current = current_,
                 .recent = recent_,
                 .lyrics_x = dl_x_,
                 .lyrics_bottom = dl_bottom_,
                 .lyrics_locked = dl_locked_};
    if (window_) s.width = window_->first, s.height = window_->second;
    auto st = player_.state();
    s.position = st == State::playing || st == State::paused ? player_.position() : resume_at_ > 0 ? resume_at_ : last_pos_;
    save_state(s);
}

void Shell::config_changed() { apply_gain(); }

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
    const auto& e = images_.get(cover_url(url, px), uint32_t(px));
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

const Track* Shell::recording(const Track& t, Source source) {
    if (t.source == source) return &t;
    auto it = std::ranges::find(t.alts, source, &Track::source);
    return it == t.alts.end() ? nullptr : &*it;
}

bool Shell::liked(const Track& t) const {
    if (liked_ids_[int(t.source)].contains(t.id)) return true;
    return std::ranges::any_of(t.alts, [&](const Track& a) { return liked_ids_[int(a.source)].contains(a.id); });
}

void Shell::toggle_like(Track t) {
    bool on = !liked(t);
    std::vector<Track> targets;
    for (const Track& a : interleave(std::vector{t}, t.alts)) {
        if (!provider(a).logged_in()) continue;
        if (on || liked_ids_[int(a.source)].contains(a.id)) targets.push_back(a);
        if (on && !targets.empty()) break;
    }
    if (targets.empty()) return notify("登录这首歌所属的账号后才能收藏", "heart");
    for (const auto& a : targets) {
        if (on) liked_ids_[int(a.source)].insert(a.id);
        else liked_ids_[int(a.source)].erase(a.id);
    }
    auto list = route_.page == Page::liked ? route_.list : nullptr;
    if (list) {
        auto& v = list->tracks;
        if (on) v.insert(v.begin(), t);
        else std::erase_if(v, [&](const Track& x) { return same(x, t); });
    }
    notify(on ? "已添加到我喜欢的音乐" : "已取消喜欢", on ? "heart-fill" : "heart-off");
    spawn([](Shell* self, Track t, std::vector<Track> targets, bool on, std::shared_ptr<List> list) -> Task<void> {
        bool qq = false;
        for (const auto& a : targets) {
            auto r = co_await self->provider(a).like(a, on);
            qq |= a.source == Source::qq;
            if (r) continue;
            if (on) self->liked_ids_[int(a.source)].erase(a.id);
            else self->liked_ids_[int(a.source)].insert(a.id);
            if (list) {
                std::erase_if(list->tracks, [&](const Track& x) { return same(x, t); });
                if (self->liked(t)) list->tracks.insert(list->tracks.begin(), t);
            }
            self->notify(std::format("操作没有成功：{}", r.error()), "triangle-alert", true);
        }
        if (qq && list) co_await self->load_list(list, Page::liked, {});
    }(this, std::move(t), std::move(targets), on, std::move(list)));
}

// Following, collecting and editing playlists show at once; a failed request puts things back.

void Shell::toggle_follow(std::shared_ptr<ArtistPage> page, Card artist) {
    if (!provider(artist).logged_in()) return notify("登录这位歌手所属的账号后才能关注", "user-plus");
    bool on = !page->info->followed;
    page->info->followed = on;
    if (collection_[1].loaded) place(collection_[1].cards, artist, on);
    notify(on ? std::format("已关注{}", artist.title) : "已取消关注", on ? "user-check" : "user");
    spawn([](Shell* self, std::shared_ptr<ArtistPage> page, Card artist, bool on) -> Task<void> {
        auto r = co_await self->provider(artist).follow(artist.id, on);
        if (r) co_return;
        page->info->followed = !on;
        if (self->collection_[1].loaded) place(self->collection_[1].cards, artist, !on);
        self->notify(std::format("操作没有成功：{}", r.error()), "triangle-alert", true);
    }(this, std::move(page), std::move(artist), on));
}

void Shell::toggle_collect(std::shared_ptr<List> list, Card album) {
    if (!provider(album).logged_in()) return notify("登录这张专辑所属的账号后才能收藏", "star");
    bool on = !list->album.collected;
    list->album.collected = on;
    if (collection_[0].loaded) place(collection_[0].cards, album, on);
    notify(on ? "已收藏专辑" : "已取消收藏", on ? "star-fill" : "star");
    spawn([](Shell* self, std::shared_ptr<List> list, Card album, bool on) -> Task<void> {
        auto r = co_await self->provider(album).collect(album.id, on);
        if (r) co_return;
        list->album.collected = !on;
        if (self->collection_[0].loaded) place(self->collection_[0].cards, album, !on);
        self->notify(std::format("操作没有成功：{}", r.error()), "triangle-alert", true);
    }(this, std::move(list), std::move(album), on));
}

void Shell::recount(const Card& playlist, int delta) {
    for (Card& c : playlists_)
        if (same(c, playlist)) {
            c.count = std::max(0, c.count + delta);
            c.subtitle = std::format("{} 首", c.count);
        }
}

void Shell::add_to_playlist(Track t, Card playlist) {
    Track chosen = *recording(t, playlist.source);
    t = std::move(chosen);
    auto list = route_.page == Page::playlist && same(route_.card, playlist) ? route_.list : nullptr;
    if (list && std::ranges::any_of(list->tracks, [&](const Track& x) { return same(x, t); }))
        return notify("这首歌已经在歌单里了", "folder-plus");
    recount(playlist, 1);
    // New songs go to the top, as in NetEase's own apps.
    if (list) list->tracks.insert(list->tracks.begin(), t);
    notify(std::format("已添加到「{}」", playlist.title), "folder-plus");
    spawn([](Shell* self, Track t, Card playlist, std::shared_ptr<List> list) -> Task<void> {
        auto r = co_await self->provider(playlist).edit_playlist(playlist.id, t, true);
        if (r) {
            if (playlist.source == Source::qq) {
                if (list) co_await self->load_list(list, Page::playlist, playlist);
                co_await self->load_library(Source::qq);
            }
            co_return;
        }
        self->recount(playlist, -1);
        if (list) std::erase_if(list->tracks, [&](const Track& x) { return same(x, t); });
        self->notify(std::format("没有添加到「{}」：{}", playlist.title, r.error()), "triangle-alert", true);
    }(this, std::move(t), std::move(playlist), std::move(list)));
}

void Shell::remove_from_playlist(std::shared_ptr<List> list, Card playlist, Track t) {
    auto it = std::ranges::find_if(list->tracks, [&](const Track& x) { return same(x, t); });
    if (it == list->tracks.end()) return;
    size_t at = size_t(it - list->tracks.begin());
    list->tracks.erase(it);
    recount(playlist, -1);
    notify(std::format("已从「{}」中删除", playlist.title), "list-minus");
    spawn([](Shell* self, std::shared_ptr<List> list, Card playlist, Track t, size_t at) -> Task<void> {
        auto r = co_await self->provider(playlist).edit_playlist(playlist.id, t, false);
        if (r) {
            if (playlist.source == Source::qq) {
                co_await self->load_list(list, Page::playlist, playlist);
                co_await self->load_library(Source::qq);
            }
            co_return;
        }
        list->tracks.insert(list->tracks.begin() + std::min(at, list->tracks.size()), std::move(t));
        self->recount(playlist, 1);
        self->notify(std::format("没有删除：{}", r.error()), "triangle-alert", true);
    }(this, std::move(list), std::move(playlist), std::move(t), at));
}

// The song leaves the daily list at once; the one the server recommends instead takes its place.
void Shell::dislike_daily(std::shared_ptr<List> list, Track t) {
    auto it = std::ranges::find_if(list->tracks, [&](const Track& x) { return same(x, t); });
    if (it == list->tracks.end()) return;
    size_t at = size_t(it - list->tracks.begin());
    list->tracks.erase(it);
    spawn([](Shell* self, std::shared_ptr<List> list, Track t, size_t at) -> Task<void> {
        auto r = co_await self->ncm_.dislike_daily(t.id);
        auto pos = list->tracks.begin() + std::min(at, list->tracks.size());
        if (!r) {
            list->tracks.insert(pos, std::move(t));
            co_return self->notify(std::format("没有标记为不感兴趣：{}", r.error()), "triangle-alert", true);
        }
        if (*r) list->tracks.insert(pos, std::move(**r));
        self->notify("以后会少推荐类似的歌", "thumbs-down");
    }(this, std::move(list), std::move(t), at));
}

Task<void> Shell::heartbeat(Track seed) {
    auto r = co_await ncm_.heartbeat(seed.id, liked_cards_[0].id);
    if (!r) co_return notify(std::format("心动模式没有开始：{}", r.error()), "triangle-alert", true);
    std::vector<Track> list = std::move(*r);
    std::erase_if(list, [&](const Track& x) { return same(x, seed); });
    list.insert(list.begin(), std::move(seed));
    if (mode_ == Mode::shuffle) set_mode(Mode::list);
    play(list, 0);
    notify("心动模式：从你喜欢的歌出发，穿插相似的歌", "heart-pulse");
}

void Shell::retitle(const Card& playlist, const std::string& title) {
    for (Card& c : playlists_)
        if (same(c, playlist)) c.title = title;
    if (same(route_.card, playlist)) route_.card.title = title;
    for (Route& r : history_)
        if (same(r.card, playlist)) r.card.title = title;
}

// Library and account.

Task<void> Shell::startup() {
    auto saved = co_await creds::load(loop_);
    if (saved) {
        auto c = creds::from_json(*saved);
        ncm_.client.cookies = std::move(c.netease);
        qq_.client.session = std::move(c.qq);
        can_save_creds_ = true;
    } else {
        notify(std::format("无法读取保存的登录信息：{}", saved.error()), "triangle-alert", true);
    }
    bool fresh = ncm_.client.cookies.empty();
    ncm_.client.ensure_fingerprint();
    creds_loaded_ = true;
    if (fresh) save_credentials();
    spawn(load_recommended());
    spawn(load_library(Source::netease));
    if (qq_.logged_in()) {
        auto renewed = co_await qq_.renew();
        if (!renewed) notify(renewed.error(), "user", true);
        if (qq_.logged_in()) co_await load_library(Source::qq);
    }
}

void Shell::save_credentials() {
    if (!can_save_creds_) return;
    // Responses often set several cookies in a row; write once they settle.
    loop_.cancel(creds_timer_);
    creds_timer_ = loop_.after(1s, [this] {
        creds_timer_ = 0;
        spawn([](Shell* self) -> Task<void> {
            auto r = co_await creds::save(self->loop_, creds::to_json({.netease = self->ncm_.client.cookies, .qq = self->qq_.client.session}));
            if (!r) self->notify(std::format("登录信息没有保存：{}", r.error()), "triangle-alert", true);
        }(this));
    });
}

Task<void> Shell::load_library(Source source) {
    Provider& p = provider(source);
    if (!p.logged_in()) co_return;
    int i = int(source);
    uint64_t gen = ++library_gen_[i];
    auto a = co_await p.account();
    if (gen != library_gen_[i]) co_return;
    if (!a) {
        notify(a.error(), "user", true);
        if (!p.logged_in()) save_credentials();
        co_return;
    }
    accounts_[i] = *a;
    redraw_();
    if (source == Source::netease) spawn(load_daily_lists());
    auto pl = co_await p.user_playlists(a->id);
    if (gen != library_gen_[i]) co_return;
    if (pl) {
        liked_cards_[i] = pl->first;
        std::vector<Card> other;
        for (auto& c : playlists_)
            if (c.source != source) other.push_back(std::move(c));
        if (source == Source::netease) playlists_ = interleave(pl->second, other);
        else playlists_ = interleave(other, pl->second);
    } else notify(pl.error(), "list-music", true);
    auto ids = co_await p.liked_ids(a->id);
    if (gen != library_gen_[i]) co_return;
    if (ids) liked_ids_[i] = {ids->begin(), ids->end()};
    else notify(ids.error(), "heart", true);
    // A page that waited for the login loads now.
    if (route_.page == Page::liked) route_.list.reset();
    ensure_route_data();
    redraw_();
}

Task<void> Shell::load_recommended() {
    recommended_loading_ = true;
    auto [a, b] = co_await both(ncm_.recommended_playlists(), qq_.recommended_playlists());
    if (!a && b) notify(a.error(), "cloud-off", true);
    if (!b && a) notify(b.error(), "cloud-off", true);
    auto r = combined(std::move(a), std::move(b));
    recommended_loading_ = false;
    if (r) recommended_ = std::move(*r);
    else recommended_error_ = r.error();
    page_time_ = route_.page == Page::recommend ? now_() : page_time_;
    redraw_();
}

Task<void> Shell::load_daily_lists() {
    auto r = co_await ncm_.daily_playlists();
    if (!r) co_return;
    daily_lists_ = std::move(*r);
    daily_lists_since_ = now_();
    redraw_();
}

Task<void> Shell::load_toplists() {
    toplists_loading_ = true;
    toplists_error_.clear();
    auto [a, b] = co_await both(ncm_.toplists(), qq_.toplists());
    if (!a && b) notify(a.error(), "cloud-off", true);
    if (!b && a) notify(b.error(), "cloud-off", true);
    auto r = combined(std::move(a), std::move(b));
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
    Source source = Source(login_step_);
    qr_active_ = true;
    qq_.cancel_qr();
    qr_.clear();
    qr_image_ = false;
    images_.erase("qr:qq");
    qr_status_.reset();
    qr_error_.clear();
    auto code = co_await provider(source).qr_start();
    if (gen != qr_gen_) co_return;
    if (!code) {
        qr_error_ = code.error();
        co_return redraw_();
    }
    if (!code->image.empty()) {
        auto img = gfx::decode_square({reinterpret_cast<const uint8_t*>(code->image.data()), code->image.size()}, 512, qr_error_);
        if (!img.w) co_return redraw_();
        images_.put("qr:qq", img);
        qr_image_ = true;
    } else {
        QRcode* q = QRcode_encodeString(code->text.c_str(), 0, QR_ECLEVEL_H, QR_MODE_8, 1);
        qr_size_ = q->width;
        qr_.resize(size_t(q->width) * size_t(q->width));
        for (size_t i = 0; i < qr_.size(); ++i) qr_[i] = q->data[i] & 1;
        QRcode_free(q);
    }
    qr_status_ = QrStatus{};
    redraw_();
    while (true) {
        co_await sleep{loop_, 2s};
        if (gen != qr_gen_) co_return;
        auto s = co_await provider(source).qr_poll();
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
    co_await load_library(source);
    if (gen != qr_gen_) co_return;
    login_step_ = -1;
    login_time_ = now_();
    if (accounts_[int(source)]) notify(std::format("已登录：{}", accounts_[int(source)]->name), "circle-check");
}

Task<void> Shell::logout(Source source) {
    ++library_gen_[int(source)];
    ++collection_gen_;
    co_await provider(source).logout();
    accounts_[int(source)].reset();
    std::erase_if(playlists_, [&](const Card& c) { return c.source == source; });
    liked_ids_[int(source)].clear();
    liked_cards_[int(source)] = {};
    if (source == Source::netease) {
        daily_lists_.clear();
        fm_.clear();
        fm_pos_ = 0;
        listen_.reset();
    }
    for (auto& c : collection_) {
        std::erase_if(c.cards, [&](const Card& card) { return card.source == source; });
        c.total = int(c.cards.size());
        c.loading = false;
    }
    if (route_.page == Page::liked) route_.list.reset();
    ensure_route_data();
    save_credentials();
    notify(source == Source::netease ? "已退出网易云音乐" : "已退出 QQ 音乐", "log-out");
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

// Card tabs show the first page only. A failed next page is not retried on its own, or it would be every frame.
void Shell::load_results(int tab) {
    SearchResults& res = results_[tab];
    if (res.loading || (res.loaded && (tab != 0 || !res.more() || !res.error.empty()))) return;
    res.loading = true;
    res.error.clear();
    for (int i = 0; i < 2; ++i) {
        if (res.done[i]) continue;
        ++res.pending;
        int offset = int(res.sources[i].tracks.size() + res.sources[i].cards.size());
        spawn(fetch_results(search_gen_, tab, Source(i), searched_, offset));
    }
}

Task<void> Shell::fetch_results(uint64_t gen, int tab, Source source, std::string query, int offset) {
    auto r = co_await provider(source).search(query, SearchKind(tab), offset, tab == 0 ? 30 : 40);
    if (gen != search_gen_) co_return;
    SearchResults& res = results_[tab];
    int i = int(source);
    res.loading = --res.pending != 0;
    if (!res.loaded) res.since = now_();
    res.loaded = true;
    if (r) {
        Batch& part = res.sources[i];
        int count = int(r->tracks.size() + r->cards.size());
        res.done[i] = count == 0 || offset + count >= r->total;
        part.total = r->total;
        part.tracks.insert(part.tracks.end(), std::make_move_iterator(r->tracks.begin()), std::make_move_iterator(r->tracks.end()));
        part.cards.insert(part.cards.end(), std::make_move_iterator(r->cards.begin()), std::make_move_iterator(r->cards.end()));
    } else {
        res.done[i] = true;
        res.errors[i] = r.error();
        notify(r.error(), "cloud-off", true);
    }
    std::optional<Track> selected;
    if (tab == search_tab_ && selected_ >= 0 && selected_ < int(res.tracks.size())) selected = res.tracks[size_t(selected_)];
    res.tracks = merge_tracks(res.sources[0].tracks, res.sources[1].tracks);
    res.cards = interleave(res.sources[0].cards, res.sources[1].cards);
    if (selected) {
        auto at = std::ranges::find_if(res.tracks, [&](const Track& t) { return same(t, *selected); });
        selected_ = int(at - res.tracks.begin());
    }
    int raw = int(res.sources[0].tracks.size() + res.sources[1].tracks.size() + res.sources[0].cards.size() + res.sources[1].cards.size());
    res.total = res.more() ? std::max(int(res.size()), res.sources[0].total + res.sources[1].total - raw + int(res.size())) : int(res.size());
    if (!res.loading && res.size() == 0)
        res.error = res.errors[0] + (res.errors[0].empty() || res.errors[1].empty() ? "" : "；") + res.errors[1];
    redraw_();
}

void Shell::open(ui::Ui& ui, Page page, const Card& card) {
    if (route_.page == page && same(route_.card, card) && page != Page::search) return;
    history_.push_back(route_);
    if (history_.size() > 50) history_.erase(history_.begin());
    // Older pages reload if they are visited again.
    if (history_.size() > 8) {
        Route& old = history_[history_.size() - 9];
        old.list.reset();
        old.artist.reset();
    }
    enter(ui, {page, card}, 1);
}

void Shell::open(ui::Ui& ui, const Card& card) {
    open(ui, card.kind == CardKind::album ? Page::album : card.kind == CardKind::artist ? Page::artist : Page::playlist, card);
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
    for (const char* id : {"tracks", "artist-songs", "artist-albums", "artist-intro"}) ui.scroll_to(ui.id(id), 0);
    switch (route_.page) {
        case Page::daily: route_.card = {.title = "每日推荐", .subtitle = std::format("{} · 根据你的口味生成", today_)}; break;
        case Page::cloud: route_.card = {.title = "我的云盘", .subtitle = "上传到云端的歌曲"}; break;
        case Page::liked: route_.card = {.title = "我喜欢的音乐"}; break;
        case Page::recent: route_.card = {.title = "最近播放"}; break;
        default: break;
    }
    ensure_route_data();
}

bool Shell::needs_login(Page p) { return p == Page::daily || p == Page::fm || p == Page::liked || p == Page::cloud || p == Page::collection; }

bool Shell::signed_in(Page p) const { return p == Page::liked || p == Page::collection ? signed_in() : ncm_.logged_in(); }

void Shell::ensure_route_data() {
    Page page = route_.page;
    bool listed = page == Page::daily || page == Page::cloud || page == Page::liked || page == Page::playlist || page == Page::album ||
                  page == Page::similar || (page == Page::recent && recent_tab_ == 1 && ncm_.logged_in());
    if (listed && !route_.list && !(needs_login(page) && !signed_in(page))) {
        if (page == Page::liked) {
            if (liked_cards_[0].id.empty() && liked_cards_[1].id.empty()) return;
        }
        route_.list = std::make_shared<List>();
        spawn(load_list(route_.list, page, route_.card));
    }
    if (page == Page::artist && !route_.artist) {
        route_.artist = std::make_shared<ArtistPage>();
        spawn(load_artist(route_.artist, route_.card));
    }
    if (page == Page::toplist && toplists_.empty() && !toplists_loading_) spawn(load_toplists());
    if (page == Page::square) {
        if (categories_.groups.empty() && !categories_loading_) spawn(load_categories());
        if (!square_.loaded) load_square();
    }
    if (page == Page::fm && fm_.empty() && !fm_loading_ && ncm_.logged_in()) spawn(load_fm());
    // Collections can change elsewhere, so they reload whenever the page is entered.
    if (page == Page::collection && signed_in() && !collection_[0].loading) spawn(load_collection());
}

Task<void> Shell::load_list(std::shared_ptr<List> list, Page page, Card card) {
    list->loading = true;
    // Not a ?: chain: with co_await in its branches GCC 16 also starts the tasks of branches not taken.
    Result<std::vector<Track>> r;
    if (page == Page::album) {
        auto a = co_await provider(card).album(card.id);
        if (a) {
            r = std::move(a->tracks);
            list->album = std::move(*a);
        } else {
            r = std::unexpected(a.error());
        }
    } else if (page == Page::liked) {
        auto [a, b] = co_await both(library_tracks(ncm_, liked_cards_[0]), library_tracks(qq_, liked_cards_[1]));
        if (!a && b) notify(a.error(), "heart", true);
        if (!b && a) notify(b.error(), "heart", true);
        r = combined(std::move(a), std::move(b));
    } else if (page == Page::daily) {
        r = co_await ncm_.daily();
    } else if (page == Page::cloud) {
        r = co_await ncm_.cloud();
    } else if (page == Page::recent) {
        r = co_await ncm_.recent_plays();
    } else if (page == Page::similar) {
        r = co_await ncm_.similar(card.id);
    } else {
        r = co_await provider(card).tracks(card);
    }
    list->loading = false;
    if (r) {
        list->tracks = std::move(*r);
        list->since = now_();
    } else {
        list->error = r.error();
    }
    redraw_();
}

Task<void> Shell::load_artist(std::shared_ptr<ArtistPage> page, Card card) {
    page->songs.loading = true;
    spawn(fetch_artist(page, card, 0, 0));
    auto r = co_await provider(card).artist(card.id);
    if (r) page->info = std::move(*r);
    else page->error = r.error();
    redraw_();
}

void Shell::load_artist_tab(int tab) {
    ArtistPage& a = *route_.artist;
    Results& res = tab == 0 ? a.songs : tab == 1 ? a.albums : a.intro;
    if (res.loading || !res.more() || (res.loaded && !res.error.empty())) return;
    res.loading = true;
    res.error.clear();
    spawn(fetch_artist(route_.artist, route_.card, tab, int(res.size())));
}

Task<void> Shell::fetch_artist(std::shared_ptr<ArtistPage> page, Card card, int tab, int offset) {
    if (tab == 2) {
        auto r = co_await provider(card).artist_intro(card.id);
        page->intro.loading = false;
        page->intro.loaded = true;
        page->intro.since = now_();
        if (r) page->sections = std::move(*r);
        else page->intro.error = r.error();
        co_return redraw_();
    }
    Result<Batch> r;
    if (tab == 0) r = co_await provider(card).artist_songs(card.id, offset);
    else r = co_await provider(card).artist_albums(card.id, offset);
    arrived(tab == 0 ? page->songs : page->albums, std::move(r), now_());
    redraw_();
}

Task<void> Shell::load_categories() {
    categories_loading_ = true;
    auto r = co_await ncm_.categories();
    categories_loading_ = false;
    if (r) categories_ = std::move(*r);
    redraw_();
}

void Shell::set_category(ui::Ui& ui, std::string category) {
    if (category == square_category_) return;
    square_category_ = std::move(category);
    ++square_gen_;
    square_ = {};
    square_time_ = ui.now;
    ui.scroll_to(ui.id("square"), 0);
    load_square();
}

void Shell::load_square() {
    if (square_.loading || !square_.more() || (square_.loaded && !square_.error.empty())) return;
    square_.loading = true;
    square_.error.clear();
    spawn(fetch_square(square_gen_, square_category_, int(square_.cards.size())));
}

Task<void> Shell::fetch_square(uint64_t gen, std::string category, int offset) {
    auto r = co_await ncm_.square(std::move(category), offset);
    if (gen != square_gen_) co_return;
    arrived(square_, std::move(r), now_());
    redraw_();
}

Task<void> Shell::load_collection() {
    uint64_t gen = ++collection_gen_;
    for (auto& c : collection_) c.loading = true;
    auto [a, b] = co_await both(both(collection(ncm_, true), collection(ncm_, false)), both(collection(qq_, true), collection(qq_, false)));
    if (gen != collection_gen_) co_return;
    auto albums = combined(std::move(a.first), std::move(b.first));
    auto artists = combined(std::move(a.second), std::move(b.second));
    auto batch = [](std::vector<Card> v) { return Batch{.cards = std::move(v)}; };
    Results fresh[2];
    arrived(fresh[0], albums.transform(batch), now_());
    arrived(fresh[1], artists.transform(batch), now_());
    // A reload does not replay the cards' entrance.
    for (int i = 0; i < 2; ++i) {
        if (collection_[i].loaded) fresh[i].since = collection_[i].since;
        collection_[i] = std::move(fresh[i]);
    }
    redraw_();
}

// Playback.

bool Shell::playing() const {
    auto s = player_.state();
    return want_play_ && (fetching_ || s == State::loading || s == State::playing);
}

void Shell::apply_volume() {
    player_.set_volume(silent_ || muted_ ? 0.f : volume_);
    publish();
}

void Shell::apply_gain() {
    // The track gain is in dB; the peak caps it so nothing clips, and without a peak it only ever lowers.
    float g = 1;
    if (config_.playback.normalize) g = std::min(std::pow(10.f, track_gain_ / 20), track_peak_ > 0 ? 1 / track_peak_ : 1.f);
    log::debug("loudness: {:+.2f} dB, peak {:.3f}: x{:.3f}", track_gain_, track_peak_, g);
    player_.set_gain(g);
}

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

void Shell::stop() {
    if (!current()) return;
    want_play_ = fetching_ = false;
    resume_at_ = last_pos_ = 0;
    ++play_gen_;
    stop_player();
    changed();
}

void Shell::stop_player() {
    report_listen(false);
    player_.stop();
}

// A listen counts once it played 30 s, or to the end; the server adds up every report, so each listen is sent once.
void Shell::report_listen(bool ended) {
    auto st = player_.state();
    if (!listen_ || !(ended || st == State::playing || st == State::paused)) return;
    int seconds = int(ended ? player_.duration() : player_.position());
    if (ended || seconds >= 30) spawn(ncm_.report_play(listen_->id, listen_->album_id, seconds));
    listen_.reset();
}

void Shell::seek(double at) {
    player_.seek(at);
    last_pos_ = at;
    schedule_prefetch();
    if (mpris_) mpris_->seeked(at);
}

void Shell::set_mode(Mode m) {
    mode_ = m;
    shuffle_next_ = -1;
    shuffle_history_.clear();
    changed();
    publish();
}

double Shell::position() {
    auto st = player_.state();
    if (st == State::playing || st == State::paused) return player_.position();
    return st == State::idle && !fetching_ ? resume_at_ : last_pos_;
}

void Shell::publish() {
    if (!mpris_) return;
    MprisState s{.volume = muted_ ? 0 : volume_, .loop_one = mode_ == Mode::one, .shuffle = mode_ == Mode::shuffle};
    if (const Track* t = current()) {
        using S = MprisState::Status;
        auto st = player_.state();
        s.status = playing() ? S::playing : st == State::idle && !fetching_ && resume_at_ == 0 ? S::stopped : S::paused;
        s.track = std::format("/lyren/track/{}_{}", t->source == Source::netease ? "ncm" : "qq", t->id);
        s.title = t->title, s.artist = t->artist, s.album = t->album;
        s.art = cover_url(t->cover, 512);
        s.length = player_.duration() > 0 ? player_.duration() : t->duration;
        s.can_play = true;
        s.can_next = queue_.size() > 1 || fm_playing_;
        s.can_prev = queue_.size() > 1;
        s.can_seek = (st == State::playing || st == State::paused) && s.length > 0;
    }
    mpris_->update(s);
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
    if (prefetch_.key != key(*current())) prefetch_ = {};
    stop_player();
    if (lyrics_id_ != key(*current())) {
        lyrics_id_ = key(*current());
        lyrics_.clear();
        lyric_layout_.clear();
        lyrics_loading_ = true;
    }
    spawn(load_track(gen, *current(), at));
    changed();
}

Task<void> Shell::load_track(uint64_t gen, Track t, double at) {
    std::shared_ptr<audio::Spool> spool;
    Stream s;
    if (prefetch_.spool && prefetch_.key == key(t)) {
        spool = std::move(prefetch_.spool);
        s = std::move(prefetch_.stream);
        t = std::move(prefetch_.track);
        prefetch_ = {};
    } else {
        auto r = co_await resolve_stream(t, config_.playback.quality, [this](Track track, Quality quality) {
            return provider(track).stream(std::move(track), quality);
        });
        if (gen != play_gen_) co_return;
        if (!r || (r->stream.trial && skip_trials())) {
            fetching_ = false;
            // A run of unplayable tracks is reported once, when one finally plays.
            if (!failures_) notify(r ? std::format("《{}》只能试听，已跳过", t.title) : std::format("《{}》{}", t.title, r.error()), "circle-alert", !r);
            if (++failures_ < queue_.size()) {
                skip(1, true);
            } else {
                failures_ = 0;
                want_play_ = false;
            }
            co_return redraw_();
        }
        t = std::move(r->track);
        s = std::move(r->stream);
        spool = audio::Spool::url(http_, s.url);
    }
    if (key(t) != key(*current())) {
        log::debug("recording: {} -> {}", key(*current()), key(t));
        lyrics_id_ = key(t);
        lyrics_.clear();
        lyric_layout_.clear();
        lyrics_loading_ = true;
    }
    queue_[size_t(current_)] = t;
    fetching_ = false;
    clip_start_ = s.trial_start;
    trial_ = s.trial;
    track_gain_ = s.gain, track_peak_ = s.peak;
    apply_gain();
    player_.open(std::move(spool), want_play_);
    if (at > 0) seek(at);
    player_.set_title(std::format("{} - {}", t.title, t.artist));
    if (lyrics_.empty() && lyrics_id_ == key(t)) spawn(load_lyrics(t));
    redraw_();
}

Task<void> Shell::prefetch(Track t) {
    uint64_t gen = play_gen_;
    auto p = co_await resolve_stream(t, config_.playback.quality, [this](Track track, Quality quality) {
        return provider(track).stream(std::move(track), quality);
    });
    if (gen != play_gen_ || !p || (p->stream.trial && skip_trials())) co_return;
    log::debug("prefetch: {}", t.title);
    prefetch_.key = key(t);
    prefetch_.track = std::move(p->track);
    prefetch_.spool = audio::Spool::url(http_, p->stream.url);
    prefetch_.stream = std::move(p->stream);
}

// Shortly before the end, fetch the next track's URL and first bytes. On a timer rather than in draw(), so it also
// happens while the window is hidden.
void Shell::schedule_prefetch() {
    loop_.cancel(prefetch_timer_);
    prefetch_timer_ = 0;
    double dur = player_.duration();
    if (player_.state() != State::playing || dur <= 0 || prefetch_.gen == play_gen_) return;
    auto wait = std::chrono::duration<double>(std::max(0.0, dur - 15 - player_.position()));
    prefetch_timer_ = loop_.after(std::chrono::duration_cast<Clock::duration>(wait), [this] {
        prefetch_timer_ = 0;
        if (mode_ == Mode::one || fm_playing_) return;
        prefetch_.gen = play_gen_;
        if (int i = next_index(); i >= 0 && i != current_) spawn(prefetch(queue_[size_t(i)]));
    });
}

Task<void> Shell::load_lyrics(Track t) {
    std::string id = key(t);
    lyrics_id_ = id;
    lyrics_loading_ = true;
    auto r = co_await provider(t).lyrics(t);
    if (lyrics_id_ != id) co_return;
    lyrics_loading_ = false;
    if (r) lyrics_ = std::move(*r);
    lyric_layout_.clear();
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
            if (ncm_.logged_in() && !trial_ && current()->source == Source::netease) {
                listen_ = *current();
                spawn(ncm_.report_start(listen_->id, listen_->album_id));
            }
        }
        if (s == State::paused) changed();
        if (s == State::ended) {
            std::optional<Track> listened = listen_;
            report_listen(true);
            if (mode_ == Mode::one && !fm_playing_) {
                listen_ = listened;  // each time round is another listen
                seek(0);
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
    schedule_prefetch();
    publish();
    redraw_();
}

// Drawing helpers.

void Shell::cover(ui::Ui& ui, Rect r, const std::string& url, float radius, std::string_view placeholder) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    int px = Images::tier(r.w * p.scale());
    const auto& e = images_.get(cover_url(url, px), uint32_t(px));
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

// Greedy line breaking at spaces and around CJK characters, from a single shaping pass. The lines point into s.
static std::vector<std::string_view> wrap(ui::Painter& p, std::string_view s, float size, float width) {
    const text::Shaped& sh = p.shape(s, {.size = size});
    if (sh.width * size <= width || sh.glyphs.size() < 2) return {s};
    auto at = [&](uint32_t byte) {
        size_t i = byte;
        return utf8::decode(s, i);
    };
    auto wide = [&](uint32_t byte) { return at(byte) >= 0x2e80; };
    // Closing punctuation stays at the end of the line before.
    auto closing = [&](uint32_t byte) { return std::u32string_view(U"，。、；：？！）」』》〉】”’…").contains(at(byte)); };
    std::vector<std::string_view> out;
    auto emit = [&](size_t from, size_t to) {
        std::string_view v = s.substr(from, to - from);
        while (v.ends_with(' ')) v.remove_suffix(1);
        while (v.starts_with(' ')) v.remove_prefix(1);
        if (!v.empty()) out.push_back(v);
    };
    const auto& g = sh.glyphs;
    size_t start = 0, candidate = 0;
    for (size_t i = 1; i < g.size(); ++i) {
        uint32_t c = g[i].cluster, pc = g[i - 1].cluster;
        if (c != pc && c > 0 && (s[c - 1] == ' ' || wide(c) || wide(pc)) && !closing(c)) candidate = i;
        float end = (i + 1 < g.size() ? g[i + 1].x : sh.width) - g[start].x;
        if (end * size > width && candidate > start) {
            emit(g[start].cluster, g[candidate].cluster);
            start = candidate;
        }
    }
    emit(g[start].cluster, s.size());
    return out;
}

// wrap() for text with line breaks; blank lines stay.
static std::vector<std::string_view> wrap_lines(ui::Painter& p, std::string_view s, float size, float width) {
    std::vector<std::string_view> out;
    for (auto part : std::views::split(s, '\n')) {
        auto lines = wrap(p, std::string_view(part.begin(), part.end()), size, width);
        out.insert(out.end(), lines.begin(), lines.end());
    }
    return out;
}

// Headed paragraphs in their own scrolling view: an artist's introduction, an album's notes. Laid out every frame;
// there are only a few, and the shaping is cached.
static void prose(ui::Ui& ui, Rect r, ui::Id id, const std::vector<std::pair<std::string, std::string>>& sections) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    TextStyle body{.size = t.fs(14), .color = t.text.mix(t.text_dim, 0.35f)};
    const float lh = std::round(body.size * 1.75f), w = r.w - 16, head_h = 36, gap = 24;
    std::vector<std::vector<std::string_view>> wrapped;
    float content = 0;
    for (const auto& [head, text] : sections) {
        wrapped.push_back(wrap_lines(p, text, body.size, w));
        content += (head.empty() ? 0 : head_h) + float(wrapped.back().size()) * lh + gap;
    }
    float off = ui.begin_scroll(id, r, content);
    float y = r.y - off;
    Rect clip = ui.clip();
    for (size_t i = 0; i < sections.size(); ++i) {
        if (!sections[i].first.empty()) {
            ui.label({r.x, y + 4, w, 24}, sections[i].first, {.size = t.fs(16), .color = t.text});
            y += head_h;
        }
        for (auto line : wrapped[i]) {
            if (y + lh > clip.y && y < clip.bottom()) p.text(r.x, y + lh * 0.7f, line, body, w);
            y += lh;
        }
        y += gap;
    }
    ui.end_scroll();
}

// Where the wipe of a word-timed line stands at t within s, one of its wrapped lines: px from the start of s,
// a soft edge's width before it while s is unsung and past its end once sung.
static float sung_x(ui::Painter& p, const LyricLine& l, std::string_view s, float size, float soft, double t) {
    int k = word_at(l, t);
    if (k < 0) return -soft;
    const LyricWord& w = l.words[size_t(k)];
    auto from = uint32_t(s.data() - l.text.data()), to = from + uint32_t(s.size());
    const text::Shaped& sh = p.shape(s, {.size = size});
    auto x = [&](uint32_t b) {
        if (b <= from) return -soft;
        if (b >= to) return sh.width * size + soft;
        auto g = std::ranges::find_if(sh.glyphs, [&](const text::Glyph& g) { return g.cluster >= b - from; });
        return (g == sh.glyphs.end() ? sh.width : g->x) * size;
    };
    float sung = w.duration > 0 ? float(std::clamp((t - w.time) / w.duration, 0.0, 1.0)) : 1;
    return x(w.from) + (x(w.to) - x(w.from)) * sung;
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
    const auto& profile = accounts_[0] ? accounts_[0] : accounts_[1];
    if (profile) {
        // Signed in: the avatar replaces the icon.
        Rect a = ui::center(account, 28, 28);
        auto it = ui.interact(ui.id("account"), a.inset(-2));
        float hot = ui.anim(ui.id("account"), it.hovered ? 1 : 0, 150);
        if (hot > 0) ui.painter().rect(a.inset(-3), ui.theme().text.with_alpha(0.12f * hot), 17);
        cover(ui, a, profile->avatar, 14, "user");
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
    if (qr_active_ && !ui.modal_open()) {
        ++qr_gen_;
        qr_active_ = false;
        qq_.cancel_qr();
        images_.erase("qr:qq");
        qr_image_ = false;
    }

    auto st = player_.state();
    if (st == State::playing || st == State::paused) last_pos_ = player_.position();
    double dur = player_.duration() > 0 ? player_.duration() : current() ? current()->duration : 0;

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
        float hue = -1;
        if (route_.page == Page::playlist || route_.page == Page::album || route_.page == Page::similar) hue = cover_hue(route_.card.cover, 148 * p.scale());
        else if (route_.page == Page::artist && route_.artist && route_.artist->info) hue = cover_hue(route_.artist->info->avatar, 148 * p.scale());
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
                case Page::square: square_page(ui, r); break;
                case Page::fm: fm_page(ui, r); break;
                case Page::collection: collection_page(ui, r); break;
                case Page::artist: artist_page(ui, r); break;
                case Page::search: search_page(ui, r); break;
                case Page::recent:
                    if (recent_tab_ == 0 || !ncm_.logged_in()) {
                        route_.card.subtitle = std::format("{} 首 · 保存在本机", recent_.size());
                        list_page(ui, r, recent_, false, "", std::max(page_time_, tab_time_));
                        break;
                    }
                    [[fallthrough]];
                default: {
                    // No list yet: the library is still loading, or the page waits for a login.
                    static List pending{.loading = true};
                    List& list = route_.list ? *route_.list : pending;
                    if (route_.page == Page::liked && route_.list) route_.card.subtitle = std::format("{} 首", list.tracks.size());
                    else if (route_.page == Page::cloud && route_.list) route_.card.subtitle = std::format("上传到云端的歌曲 · {} 首", list.tracks.size());
                    else if (route_.page == Page::recent) route_.card.subtitle = std::format("{} 首 · 账号在各个设备上的播放记录", list.tracks.size());
                    list_page(ui, r, list.tracks, list.loading, list.error, std::max(list.since, tab_time_));
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
    publish();
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
    static constexpr Nav nav[] = {{Page::recommend, "compass", "推荐"},      {Page::toplist, "list-ordered", "排行榜"},
                                  {Page::square, "layout-grid", "歌单广场"}, {Page::fm, "radio", "私人 FM"},
                                  {Page::liked, "heart", "我喜欢的音乐"},    {Page::recent, "history", "最近播放"},
                                  {Page::cloud, "cloud", "我的云盘"},        {Page::collection, "star", "我的收藏"}};
    constexpr size_t mine = 4, lists = std::size(nav);  // where the sections start
    constexpr float item_h = 38, step = 40, head_h = 36;
    // Labels fade as the sidebar folds into the rail.
    float labels = std::clamp((r.w - 72) / 148, 0.f, 1.f);
    auto item_y = [&](size_t k) { return 8 + head_h * float(1 + (k >= mine) + (k >= lists)) + step * float(k); };

    int sel = -1;
    for (size_t k = 0; k < std::size(nav); ++k)
        if (nav[k].page == route_.page || (route_.page == Page::daily && k == 0)) sel = int(k);
    if (route_.page == Page::playlist)
        for (size_t i = 0; i < playlists_.size(); ++i)
            if (same(playlists_[i], route_.card)) sel = int(lists + i);

    float content = item_y(lists + playlists_.size()) + 8 + (playlists_.empty() ? 40 : 0);
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
    heading(mine, "我的音乐");
    heading(lists, "歌单");
    if (signed_in() && labels > 0.5f) {
        Fade fade(p, labels);
        float y = r.y + item_y(lists) - head_h - off;
        if (ui.icon_button(ui.id("new-playlist"), {r.right() - 46, y + 8, 26, 26}, "plus", 15)) open_editor(ui);
    }
    for (size_t k = 0; k < std::size(nav); ++k)
        if (item(ui.id(nav[k].label), k, nav[k].label, nav[k].icon, nullptr)) open(ui, nav[k].page);
    ui.push_id(ui.id("playlists"));
    for (size_t i = 0; i < playlists_.size(); ++i)
        if (item(ui.id(key(playlists_[i])), lists + i, playlists_[i].title, {}, &playlists_[i].cover)) open(ui, Page::playlist, playlists_[i]);
    ui.pop_id();
    if (playlists_.empty() && labels > 0.01f)
        ui.label({r.x + 24, r.y + item_y(lists) - off, r.w - 48, 20}, signed_in() ? "还没有歌单" : "登录后显示你的歌单",
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
        ui.label({c.x, c.y + 58, c.w, 22}, "登录账号后可用", {.size = t.fs(15), .color = t.text}, Align::center);
        if (ui.button(ui.id("page-login"), {c.x + (c.w - 120) / 2, c.y + 96, 120, 36}, "扫码登录", ui::ButtonKind::primary, "qr-code")) {
            open_account(ui);
            if (route_.page != Page::liked && route_.page != Page::collection) {
                login_step_ = 0;
                login_time_ = ui.now;
                spawn(qr_login());
            }
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
        if (card(ui, ui.id(key(cards[i])), c, cards[i], round)) clicked = int(i);
    }
    ui.pop_id();
    return clicked;
}

void Shell::recommend_page(ui::Ui& ui, Rect r) {
    const auto& t = ui.theme();
    Rect body = r.inset(32, 0);
    Grid g(body.w);
    constexpr float hero_h = 136;
    float daily_h = daily_lists_.empty() ? 0 : 44 + g.height(daily_lists_.size()) + 40;
    float content = 28 + 76 + hero_h + 40 + daily_h + 44 + std::max(g.height(recommended_.size()), 120.f) + 32;
    float off = ui.begin_scroll(ui.id("recommend"), r, content);
    float y = r.y + 28 - off;
    const auto& profile = accounts_[0] ? accounts_[0] : accounts_[1];
    page_title(ui, body, y, "推荐", std::format("{}，{}", today_, profile ? std::format("{}，为你挑了这些", profile->name) : "为你挑了这些"));
    y += 76;
    float hw = (body.w - Grid::gap) / 2;
    std::string day = today_.substr(today_.find("月") + 4);
    day.resize(day.find(' '));
    if (hero(ui, ui.id("hero-daily"), {body.x, y, hw, hero_h}, day, "每日推荐", "根据你的口味生成，每天 6:00 更新", 0.98f)) open(ui, Page::daily);
    if (hero(ui, ui.id("hero-fm"), {body.x + hw + Grid::gap, y, hw, hero_h}, "FM", "私人 FM", "听不腻的随机电台，不喜欢就跳过", 0.62f))
        open(ui, Page::fm);
    y += hero_h + 40;
    if (!daily_lists_.empty()) {
        ui.label({body.x, y, body.w, 28}, "每日推荐歌单", {.size = t.fs(18), .color = t.text});
        if (int i = card_grid(ui, {body.x, y + 44, body.w, 0}, ui.id("daily-cards"), daily_lists_, false, std::max(page_time_ + 0.08, daily_lists_since_)); i >= 0)
            open(ui, daily_lists_[size_t(i)]);
        y += daily_h;
    }
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

void Shell::square_page(ui::Ui& ui, Rect r) {
    const auto& t = ui.theme();
    Rect body = r.inset(32, 0);
    Grid g(body.w);
    constexpr float chips_y = 84, grid_y = chips_y + 54;
    float content = 28 + grid_y + std::max(g.height(square_.cards.size()), 120.f) + (square_.more() ? 64 : 32);
    float off = ui.begin_scroll(ui.id("square"), r, content);
    float y = r.y + 28 - off;
    page_title(ui, body, y, "歌单广场", "按分类浏览，最热的排在前面");

    // The popular categories as far as they fit, the picked one among them, the rest behind 更多分类.
    std::vector<std::string_view> names{"全部"};
    if (square_category_ != "全部" && std::ranges::find(categories_.hot, square_category_) == categories_.hot.end()) names.push_back(square_category_);
    names.insert(names.end(), categories_.hot.begin(), categories_.hot.end());
    float x = body.x, more_w = categories_.groups.empty() ? 0 : chip_w(ui, "更多分类", true);
    std::optional<std::string> picked;
    ui.push_id(ui.id("square-chips"));
    for (auto name : names) {
        float w = chip_w(ui, name);
        if (x + w + 8 + more_w > body.right()) break;
        if (chip(ui, x, y + chips_y, name, name == square_category_)) picked = std::string(name);
        x += w + 8;
    }
    if (more_w > 0 && chip(ui, x, y + chips_y, "更多分类", false, "chevron-down"))
        ui.open_modal(600, 480, [this](ui::Ui& u, Rect m) { categories_modal(u, m); });
    ui.pop_id();
    if (picked) set_category(ui, *picked);

    float gy = y + grid_y;
    if (!page_status(ui, {body.x, gy, body.w, 120}, square_.loading && square_.cards.empty(), square_.cards.empty() ? square_.error : "", false)) {
        double since = std::max({page_time_ + 0.05, square_time_, square_.since});
        if (int i = card_grid(ui, {body.x, gy, body.w, 0}, ui.id("square-cards"), square_.cards, false, since); i >= 0) open(ui, square_.cards[size_t(i)]);
        float end = gy + g.height(square_.cards.size());
        if (square_.loading) ui.spinner(ui::center({body.x, end, body.w, 64}, 20, 20), t.text_dim, 2);
        else if (end < r.bottom() + 400) load_square();
    }
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
        spawn([](Shell* self, std::string id) -> Task<void> {
            auto r = co_await self->ncm_.fm_trash(std::move(id));
            if (r) self->notify("以后会少推荐类似的歌", "thumbs-down");
            else self->notify(std::format("没有标记为不喜欢：{}", r.error()), "triangle-alert", true);
        }(this, tr.id));
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

void Shell::collection_page(ui::Ui& ui, Rect r) {
    Rect body = r.inset(32, 0);
    page_title(ui, body, r.y + 28, "我的收藏", "收藏的专辑和关注的歌手");
    if (page_status(ui, {r.x, r.y + 100, r.w, r.h - 100}, false, "", !signed_in())) return;
    static constexpr std::string_view tabs[] = {"专辑", "歌手"};
    int before = collection_tab_;
    if (ui.tabs(ui.id("collection-tabs"), {body.x, r.y + 100, body.w, 36}, tabs, collection_tab_)) {
        tab_time_ = ui.now;
        tab_dir_ = collection_tab_ > before ? 1 : -1;
    }
    float enter = ui.progress(tab_time_, 380);
    Fade fade(ui.painter(), enter);
    Rect content{r.x + 16 + (1 - enter) * 36 * float(tab_dir_), r.y + 148, r.w - 32, r.h - 156};
    const Results& res = collection_[collection_tab_];
    std::string error = res.cards.empty() ? res.error : "";
    if (res.cards.empty() && res.loaded && error.empty()) error = collection_tab_ ? "还没有关注的歌手" : "还没有收藏的专辑";
    if (res.cards.empty() && page_status(ui, content, res.loading && !res.loaded, error, false)) return;
    bool near_end;
    ui::Id id = ui.id(collection_tab_ ? "collection-artists" : "collection-albums");
    if (int i = grid_view(ui, content, id, res.cards, collection_tab_ == 1, std::max({page_time_, tab_time_, res.since}), false, near_end); i >= 0)
        open(ui, res.cards[size_t(i)]);
}

void Shell::artist_page(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    ArtistPage& a = *route_.artist;
    const Card& c = route_.card;
    Rect head{r.x + 32, r.y + 32, r.w - 64, 148};
    Rect cov{head.x, head.y, 148, 148};
    p.shadow({cov.x, cov.y + 10, cov.w, cov.h}, 74, 32, t.shadow.with_alpha(0.8f));
    cover(ui, cov, a.info ? a.info->avatar : c.cover, 74, "user");
    float tx = cov.right() + 28, tw = head.right() - tx;
    ui.label({tx, head.y + 2, tw, 18}, "歌手", {.size = t.fs(12), .color = t.text_dim});
    ui.label({tx, head.y + 18, tw, 38}, a.info ? a.info->name : c.title, {.size = t.fs(28), .color = t.text});
    std::string about = a.error;
    if (a.info) {
        about = std::format("单曲 {} · 专辑 {}", a.info->songs, a.info->albums);
        if (!a.info->alias.empty()) about = std::format("{} · {}", a.info->alias, about);
    }
    ui.label({tx, head.y + 62, tw, 20}, about, {.size = t.fs(13), .color = t.text_dim});
    std::vector<Track>& songs = a.songs.tracks;
    if (ui.button(ui.id("play-all"), {tx, head.bottom() - 40, 124, 40}, "播放全部", ui::ButtonKind::primary, "play-fill") && !songs.empty()) {
        if (mode_ == Mode::shuffle) set_mode(Mode::list);
        play(songs, 0);
    }
    if (a.info) {
        bool on = a.info->followed;
        if (ui.button(ui.id("follow"), {tx + 136, head.bottom() - 40, 100, 40}, on ? "已关注" : "关注", ui::ButtonKind::secondary, on ? "user-check" : "user-plus"))
            toggle_follow(route_.artist, {.kind = CardKind::artist, .source = c.source, .id = c.id, .title = a.info->name, .cover = a.info->avatar});
    }

    // Songs and albums load a page at a time as they scroll; each tab loads the first time it is picked.
    static constexpr std::string_view tabs[] = {"歌曲", "专辑", "简介"};
    Rect bar{head.x, head.bottom() + 24, head.w, 36};
    int before = a.tab;
    if (ui.tabs(ui.id("artist-tabs"), bar, tabs, a.tab)) {
        tab_time_ = ui.now;
        tab_dir_ = a.tab > before ? 1 : -1;
        load_artist_tab(a.tab);
    }
    float enter = ui.progress(tab_time_, 380);
    Fade fade(p, enter);
    Rect content{r.x + 16 + (1 - enter) * 36 * float(tab_dir_), bar.bottom() + 12, r.w - 32, r.bottom() - bar.bottom() - 20};
    Results& res = a.tab == 0 ? a.songs : a.tab == 1 ? a.albums : a.intro;
    bool empty = a.tab == 2 ? a.sections.empty() : res.size() == 0;
    std::string error = empty ? res.error : "";
    if (empty && res.loaded && error.empty()) error = a.tab == 2 ? "暂无介绍" : "这里还没有内容";
    if (empty && page_status(ui, content, res.loading || (!res.loaded && error.empty()), error, false)) return;
    double since = std::max({page_time_ + 0.06, tab_time_, res.since});
    bool near_end = false;
    int clicked = -1;
    switch (a.tab) {
        case 0: near_end = track_list(ui, content, ui.id("artist-songs"), songs, since); break;
        case 1: clicked = grid_view(ui, content, ui.id("artist-albums"), a.albums.cards, false, since, a.albums.loading, near_end); break;
        default: prose(ui, {content.x + 16, content.y + 8, content.w - 16, content.h - 8}, ui.id("artist-intro"), a.sections);
    }
    if (near_end) load_artist_tab(a.tab);
    // Last: opening an album replaces the route this page draws from.
    if (clicked >= 0) open(ui, a.albums.cards[size_t(clicked)]);
}

void Shell::search_page(ui::Ui& ui, Rect r) {
    Rect body = r.inset(32, 0);
    SearchResults& res = results_[search_tab_];
    static constexpr const char* kinds[] = {"首单曲", "张专辑", "位歌手", "个歌单"};
    page_title(ui, body, r.y + 28, std::format("“{}”", searched_),
               res.loaded ? std::format("找到{} {} {}", res.more() ? "约" : "", res.total, kinds[search_tab_]) : "正在搜索");
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
    bool near_end;
    if (int i = grid_view(ui, content, ui.id(ids[search_tab_]), shown.cards, search_tab_ == 2, since, false, near_end); i >= 0)
        open(ui, shown.cards[size_t(i)]);
}

int Shell::grid_view(ui::Ui& ui, Rect r, ui::Id id, const std::vector<Card>& cards, bool round, double since, bool loading, bool& near_end) {
    Rect inner = r.inset(12, 0);
    Grid g(inner.w, round ? 130 : 150);
    float h = 12 + g.height(cards.size()) + (loading ? 64 : 20);
    float off = ui.begin_scroll(id, r, h);
    int i = card_grid(ui, {inner.x, r.y + 12 - off, inner.w, 0}, id, cards, round, since);
    near_end = r.y + h - off < r.bottom() + 400;
    if (loading) ui.spinner(ui::center({inner.x, r.y + h - off - 64, inner.w, 64}, 20, 20), ui.theme().text_dim, 2);
    ui.end_scroll();
    return i;
}

void Shell::list_page(ui::Ui& ui, Rect r, std::vector<Track>& tracks, bool loading, const std::string& list_error, double since) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    const Card& c = route_.card;
    Rect head{r.x + 32, r.y + 32, r.w - 64, 148};
    Rect cov{head.x, head.y, 148, 148};
    p.shadow({cov.x, cov.y + 10, cov.w, cov.h}, t.radius_l(), 32, t.shadow.with_alpha(0.8f));
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
        default: cover(ui, cov, c.cover, t.radius_l(), route_.page == Page::album ? "disc-album" : "list-music");
    }
    float tx = cov.right() + 28, tw = head.right() - tx;
    TextStyle sub{.size = t.fs(13), .color = t.text_dim};
    if (route_.page == Page::playlist || route_.page == Page::album || route_.page == Page::similar) {
        std::string_view kind = route_.page == Page::album ? "专辑" : route_.page == Page::similar ? "相似歌曲" : "歌单";
        ui.label({tx, head.y + 2, tw, 18}, kind, {.size = t.fs(12), .color = t.text_dim});
    }
    ui.label({tx, head.y + 18, tw, 38}, c.title, {.size = t.fs(28), .color = t.text});
    const AlbumInfo* album = route_.page == Page::album && route_.list && !route_.list->loading ? &route_.list->album : nullptr;
    if (album && !album->artist.empty()) {
        // Album: the artist links to their page, then the date and the label; the notes open in full.
        Rect ar{tx, head.y + 62, std::min(p.measure(album->artist, sub) + 1, tw), 20};
        ui::Id aid = ui.id("album-artist");
        auto it = ui.interact(aid, ar);
        float hot = ui.anim(aid, it.hovered ? 1 : 0, 120);
        ui.label(ar, album->artist, {.size = sub.size, .color = t.text_dim.mix(t.text, hot)});
        if (hot > 0) p.rect({ar.x, ar.bottom() - 1, ar.w, 1}, t.text.with_alpha(0.4f * hot));
        if (it.clicked && !album->artist_id.empty())
            open(ui, Page::artist, {.kind = CardKind::artist, .source = c.source, .id = album->artist_id, .title = album->artist.substr(0, album->artist.find(" / "))});
        std::string rest;
        for (const std::string* part : {&album->date, &album->company})
            if (!part->empty()) rest += " · " + *part;
        ui.label({ar.right(), head.y + 62, tw - ar.w, 20}, rest, sub);
        if (!album->about.empty()) {
            Rect ab{tx, head.y + 84, tw, 20};
            ui::Id bid = ui.id("album-about");
            auto bt = ui.interact(bid, ab);
            float bh = ui.anim(bid, bt.hovered ? 1 : 0, 120);
            std::string_view first = album->about;
            first = first.substr(0, first.find('\n'));
            p.text(tx, ab.y + 15, std::format("简介：{}", first), {.size = t.fs(12), .color = t.text_dim.mix(t.text, 0.6f * bh)}, tw);
            if (bt.clicked)
                ui.open_modal(560, std::min(ui.height - 120, 600.f), [title = c.title, about = album->about](ui::Ui& u, Rect r) {
                    u.label({r.x, r.y, r.w - 40, 30}, title, {.size = u.theme().fs(20), .color = u.theme().text});
                    prose(u, {r.x, r.y + 52, r.w + 16, r.h - 52}, u.id("album-notes"), {{"", about}});
                });
        }
    } else {
        ui.label({tx, head.y + 62, tw, 20}, c.subtitle, sub);
    }
    if (ui.button(ui.id("play-all"), {tx, head.bottom() - 40, 124, 40}, "播放全部", ui::ButtonKind::primary, "play-fill") && !tracks.empty()) {
        if (mode_ == Mode::shuffle) set_mode(Mode::list);
        play(tracks, 0);
    }
    if (ui.button(ui.id("shuffle-all"), {tx + 136, head.bottom() - 40, 112, 40}, "随机播放", ui::ButtonKind::secondary, "shuffle") &&
        !tracks.empty()) {
        set_mode(Mode::shuffle);
        play(tracks, int(next(rng_) % tracks.size()));
    }
    list_actions(ui, tx + 260, head.bottom() - 40);
    Rect body{r.x + 16, head.bottom() + 28, r.w - 32, r.bottom() - head.bottom() - 36};
    bool login = needs_login(route_.page) && !signed_in(route_.page);
    std::string error = list_error;
    if (tracks.empty() && error.empty() && !loading && !login) error = route_.page == Page::similar ? "没有找到相似的歌" : "这里还没有歌曲";
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
    ui.push_id(ui.id(key(tr)));
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

void Shell::list_actions(ui::Ui& ui, float x, float y) {
    const Card& c = route_.card;
    auto square = [&](std::string_view id, std::string_view icon) {
        bool clicked = ui.button(ui.id(id), {x, y, 40, 40}, "", ui::ButtonKind::secondary, icon);
        x += 52;
        return clicked;
    };
    if (route_.page == Page::album) {
        if (!route_.list || route_.list->loading) return;
        bool on = route_.list->album.collected;
        if (ui.button(ui.id("collect"), {x, y, 100, 40}, on ? "已收藏" : "收藏", ui::ButtonKind::secondary, on ? "star-fill" : "star"))
            toggle_collect(route_.list, {.kind = CardKind::album, .source = c.source, .id = c.id, .title = c.title, .subtitle = route_.list->album.artist, .cover = c.cover});
        x += 112;
        if (square("album-comments", "message-square")) open_comments(ui, c.source, Subject::album, route_.list->album.thread, c.title);
    } else if (route_.page == Page::liked) {
        if (!ncm_.logged_in() || liked_cards_[0].id.empty() || !route_.list || route_.list->tracks.empty()) return;
        std::vector<Track> seeds;
        for (const Track& t : route_.list->tracks)
            if (const Track* n = recording(t, Source::netease)) seeds.push_back(*n);
        if (seeds.empty()) return;
        if (ui.button(ui.id("heartbeat"), {x, y, 124, 40}, "心动模式", ui::ButtonKind::secondary, "heart-pulse")) {
            // From the picked song, otherwise from any of them.
            const auto& v = route_.list->tracks;
            const Track* chosen = selected_ >= 0 && selected_ < int(v.size()) ? recording(v[size_t(selected_)], Source::netease) : nullptr;
            spawn(heartbeat(chosen ? *chosen : seeds[next(rng_) % seeds.size()]));
        }
    } else if (route_.page == Page::recent) {
        static constexpr std::string_view tabs[] = {"本机", "云端"};
        if (ncm_.logged_in() && ui.tabs(ui.id("recent-tabs"), {x, y, 140, 40}, tabs, recent_tab_, ui::TabStyle::pill)) {
            tab_time_ = ui.now;
            selected_ = -1;
            ui.scroll_to(ui.id("tracks"), 0);
            ensure_route_data();
        }
    } else if (route_.page == Page::playlist) {
        if (!(c.source == Source::qq && c.id.starts_with("dir:") && c.thread.empty()) && square("playlist-comments", "message-square"))
            open_comments(ui, c.source, Subject::playlist, c.thread.empty() ? c.id : c.thread, c.title);
        if (c.owned && square("playlist-more", "ellipsis"))
            ui.open_menu(x - 52, y + 46,
                         {
                             {"pencil", "重命名", [this, &ui, c] { open_editor(ui, c); }},
                             {"trash", "删除歌单", [this, &ui, c] { confirm_delete(ui, c); }, true},
                         });
    }
}

void Shell::track_menu(ui::Ui& ui, float x, float y, std::vector<Track>& tracks, int i) {
    Track tr = tracks[size_t(i)];
    bool like = liked(tr);
    auto* list = &tracks;
    std::vector<ui::MenuItem> items{
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
             open(ui, Page::album, {.kind = CardKind::album, .source = tr.source, .id = tr.album_id, .title = tr.album, .subtitle = tr.artist, .cover = tr.cover});
         }},
        {"mic-vocal", "查看歌手",
         [this, &ui, tr] {
             open(ui, Page::artist, {.kind = CardKind::artist, .source = tr.source, .id = tr.artist_id, .title = tr.artist.substr(0, tr.artist.find(" / "))});
         }},
        {"message-square", "查看评论",
         [this, &ui, tr] { open_comments(ui, tr.source, Subject::song, tr.id, std::format("{} · {}", tr.title, tr.artist)); }},
        {},
        {like ? "heart-off" : "heart", like ? "取消喜欢" : "喜欢", [this, tr] { toggle_like(tr); }},
        {"folder-plus", "添加到歌单",
         [this, &ui, tr] {
             bool available = provider(tr).logged_in() || std::ranges::any_of(tr.alts, [&](const Track& a) { return provider(a).logged_in(); });
             if (!available) return ui.toast("登录这首歌所属的账号后才能添加到歌单", "folder-plus");
             ui.open_modal(420, 480, [this, tr](ui::Ui& u, Rect r) { picker_modal(u, r, tr); });
         }},
        {"copy", "复制歌名和歌手",
         [&ui, tr] {
             ui.copy = std::format("{} - {}", tr.title, tr.artist);
             ui.toast("已复制到剪贴板", "copy");
         }},
    };
    if (const Track* n = recording(tr, Source::netease))
        items.insert(items.begin() + 4, {"sparkles", "相似歌曲",
                     [this, &ui, seed = *n] { open(ui, Page::similar, {.id = seed.id, .title = seed.title, .subtitle = seed.artist, .cover = seed.cover}); }});
    bool listed = route_.list && list == &route_.list->tracks;
    if (route_.page == Page::daily && listed)
        items.push_back({"thumbs-down", "不感兴趣", [this, l = route_.list, tr] { dislike_daily(l, tr); }});
    // One's own playlist can take songs out.
    if (route_.page == Page::playlist && route_.card.owned && listed)
        items.push_back({"list-minus", "从歌单中删除", [this, l = route_.list, c = route_.card, tr] { remove_from_playlist(l, c, tr); }, true});
    ui.open_menu(x, y, std::move(items));
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
        stop_player();
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
            stop_player();
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
        set_mode(Mode((int(mode_) + 1) % 3));
        ui.toast(mode_names[int(mode_)], mode_icons[int(mode_)]);
    }
    // Without a track the rest of the transport and the progress bar are dimmed and do not respond.
    ui.disabled = !tr;
    {
        Fade dim(p, tr ? 1 : 0.4f);
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
        if (ui.icon_button(ui.id("lyrics"), {cx + 92, cy - 16, 32, 32}, "mic-vocal", 17, np_open_)) np_open_ = !np_open_, np_time_ = ui.now;
    }

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
    ui.disabled = false;
    if (tr && dur > 0 && (moved || ui.is_active(prog))) dragging_ = true, drag_v_ = v;
    if (dragging_ && !ui.is_active(prog)) {
        dragging_ = false;
        double at = drag_v_ * dur;
        if (st == State::idle || st == State::failed) {
            if (fetching_) last_pos_ = at;
            else resume_at_ = last_pos_ = at;
        } else {
            seek(at);
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
    bool desktop = config_.desktop_lyrics.enabled;
    if (ui.icon_button(ui.id("desktop-lyrics"), {vol.x - 120, cy - 16, 32, 32}, "captions", 18, desktop))
        set_setting_("desktop_lyrics", "enabled", desktop ? "false" : "true");
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
        const auto& e = images_.get(cover_url(url, 128), 128, true);
        if (!e.ready) return;
        Fade f(p, a * ui.progress(e.ready_at, 600, ease::standard));
        p.image(r, e.tex, radius, white, uv);
    };
    float change = ui.progress(cover_time_, 800, ease::standard);
    if (change < 1 && !prev_cover_.empty()) backdrop(prev_cover_, 1 - change);
    if (tr) backdrop(tr->cover, change);
    p.gradient(r, t.bg.with_alpha(0.42f), t.bg.with_alpha(0.78f), radius);

    // Over a bright cover a plain icon button disappears, so these keep a backing circle.
    auto backed = [&](std::string_view id, Rect b, std::string_view icon, float size) {
        ui::Id bid = ui.id(id);
        auto it = ui.interact(bid, b);
        float hot = ui.anim(bid, it.hovered ? 1 : 0, 150);
        float down = ui.anim(ui::sub_id(bid, 0xd0), it.held && it.hovered ? 1 : 0, 90);
        p.rect(b.inset(down * 2), white.with_alpha(0.12f + 0.1f * hot), b.w / 2 - down * 2);
        p.icon(icon, ui::center(b, size - down * 1.5f, size - down * 1.5f), t.text);
        return it.clicked;
    };
    if (backed("np-close", {r.x + 16, r.y + 16, 36, 36}, "chevron-down", 22)) np_open_ = false, np_time_ = ui.now;
    if (tr && backed("np-comments", {r.right() - 52, r.y + 16, 36, 36}, "message-square", 18))
        open_comments(ui, tr->source, Subject::song, tr->id, std::format("{} · {}", tr->title, tr->artist));
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
    const float main_lh = std::round(main.size * 1.5f), sub_lh = std::round(sub.size * 1.5f), gap = 18, soft = main.size * 0.8f;
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
    // The line a little ahead, so its highlight lands with the voice rather than after it.
    const double now = clip_start_ + last_pos_, lead = 0.2;
    int cur = lyric_at(lyrics_, now + lead);
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
        float edge = std::clamp(std::min(top - r.y, r.bottom() - top - b.h) / 80, 0.f, 1.f);
        float a = (base + (1 - base) * on) * edge;
        // A word-timed current line lights up as it is sung; the rest stays a little above the other lines.
        const LyricLine& l = lyrics_[i];
        bool wipe = int(i) == cur && !l.words.empty();
        ui::Color unsung = t.text.with_alpha((base + (1 - base) * 0.25f * on) * edge);
        if (hot > 0) p.rect(hit, t.text.with_alpha(0.06f * hot), t.radius_s());
        float y = top;
        auto line = [&](std::string_view s, const TextStyle& st, float lh, ui::Color c, bool wiped) {
            float x = centered ? r.x + (r.w - std::min(p.measure(s, st), r.w)) / 2 : r.x;
            if (wiped) p.set_wipe(x + sung_x(p, l, s, st.size, soft, now), soft, unsung);
            p.text(x, y + lh * 0.72f, s, {.size = st.size, .color = c}, r.w);
            p.clear_wipe();
            y += lh;
        };
        for (auto s : b.lines) line(s, main, main_lh, t.text.with_alpha(a), wipe);
        for (auto s : b.translation) line(s, sub, sub_lh, t.text.with_alpha(a * 0.75f), false);
        if (it.clicked) {
            seek(std::max(0.0, lyrics_[i].time - clip_start_));
            lyric_manual_until_ = 0;
        }
    }
    ui.pop_id();
    ui.pop_clip();

    // A wipe moves with the music; otherwise nothing changes before the next line.
    if (!playing()) return;
    if (cur >= 0 && !lyrics_[size_t(cur)].words.empty()) ui.keep_animating();
    else if (size_t next = size_t(cur + 1); next < lyrics_.size()) ui.wake(ui.now + (lyrics_[next].time - lead - now));
}

// Desktop lyrics: a row for the controls, the line, the translation (or the artist under a title).
constexpr float dl_bar = 40;

float Shell::desktop_height(float size) { return dl_bar + std::round(size * 1.35f) + std::round(size * 0.58f * 1.35f) + 12; }

ui::Rect Shell::desktop_lyrics(ui::Ui& ui, float w, float h) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    const float size = t.lyric_size, sub = std::round(size * 0.58f), l1 = std::round(size * 1.35f), soft = size * 0.8f;
    const ui::Color sung = t.lyric_sung.value_or(t.accent);

    // The current line; before the first one, or without lyrics, the title and the artist.
    const Track* tr = current();
    const double now = clip_start_ + position(), lead = 0.2;
    int cur = lyrics_.empty() ? -1 : lyric_at(lyrics_, now + lead);
    const LyricLine* line = cur >= 0 ? &lyrics_[size_t(cur)] : nullptr;
    using sv = std::string_view;
    sv text = line ? sv(line->text) : tr ? sv(tr->title) : sv(), below = line ? sv(line->translation) : tr ? sv(tr->artist) : sv();
    bool timed = line && !line->words.empty();
    if (text != dl_line_.text || below != dl_line_.sub || timed != dl_line_.timed) {
        dl_prev_ = std::exchange(dl_line_, {std::string(text), std::string(below), timed});
        dl_changed_ = ui.now;
    }
    float k = ui.progress(dl_changed_, 360, ease::standard);

    // Long lines shrink to fit, down to 70 %, then get cut.
    float max_w = w - 64;
    auto fit = [&](std::string_view s, float base) { return std::max(base * 0.7f, std::min(base, base * max_w / std::max(p.measure(s, {.size = base}), 1.f))); };
    float cx = w / 2 + dl_x_;
    float text_w = std::max(p.measure(dl_line_.text, {.size = fit(dl_line_.text, size)}), p.measure(dl_line_.sub, {.size = fit(dl_line_.sub, sub)}));
    float pw = std::clamp(text_w + 64, 320.f, w - 16);
    cx = std::clamp(cx, pw / 2 + 8, w - pw / 2 - 8);
    Rect panel{cx - pw / 2, 4, pw, h - 8};
    Rect lock{cx - 36, 8, 32, 32}, close{cx + 4, 8, 32, 32};

    // Light buttons that read on the dark panel in either theme.
    auto button = [&](ui::Id id, Rect r, std::string_view icon) {
        auto it = ui.interact(id, r);
        float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
        p.rect(r, white.with_alpha(0.1f + 0.14f * hot), r.w / 2);
        p.icon(icon, ui::center(r, 17, 17), white.with_alpha(0.8f + 0.2f * hot));
        return it.clicked;
    };

    // Locked, the pointer passes through everywhere but a spot where the lock button would be; it shows on hover.
    Rect input = panel;
    if (dl_locked_) {
        input = lock;
        ui::Id id = ui.id("dl-unlock");
        float hot = ui.anim(ui::sub_id(id, 1), ui.hovering(lock) ? 1 : 0, 150);
        Fade fade(p, hot);
        if (button(id, lock, "lock-open")) dl_locked_ = false, changed();
    } else {
        // Unlocked, hovering shows the panel and its buttons; dragging it moves the lyrics. Sideways the text moves
        // within the surface; up and down the surface itself moves, so the pointer's y is relative to a moving origin.
        bool over = ui.hovering(panel) || dl_dragging_;
        float show = ui.anim(ui.id("dl-panel"), over ? 1 : 0, 180);
        if (show > 0) {
            Fade fade(p, show);
            p.rect(panel, ui::Color{0, 0, 0, 0.42f}, t.radius_l());
            if (button(ui.id("dl-lock"), lock, "lock")) dl_locked_ = true, changed();
            if (button(ui.id("dl-close"), close, "x")) set_setting_("desktop_lyrics", "enabled", "false");
        }
        auto it = ui.interact(ui.id("dl-drag"), panel);
        float px = ui.input().x, py = ui.input().y;
        if (it.pressed) dl_dragging_ = true, dl_grab_x_ = px - cx, dl_grab_y_ = dl_last_y_ = py;
        if (dl_dragging_ && it.held) {
            dl_x_ = px - dl_grab_x_ - w / 2;
            // Only a new pointer position counts: until then py is relative to where the surface was.
            if (py != dl_last_y_) dl_bottom_ = std::max(0.f, dl_bottom_ + dl_grab_y_ - py), dl_last_y_ = py;
            ui.cursor = wl::Cursor::grabbing;
        } else if (dl_dragging_) {
            dl_dragging_ = false;
            dl_x_ = cx - w / 2;
            changed();
        }
    }

    // Text with an outline, so it reads over any background. A word-timed current line lights up as it is sung.
    // The outline is eight overlapping copies, which would stay dark while the text fades; they fade with a³.
    float r = std::max(1.f, size / 22);
    auto draw = [&](const ShownLine& l, const LyricLine* live, float dy, float a) {
        Fade fade(p, a);
        auto one = [&](std::string_view s, float base, float y, ui::Color c, bool wiped) {
            float fs = fit(s, base), tw = std::min(p.measure(s, {.size = fs}), max_w), x = cx - tw / 2;
            for (int i = 0; i < 8; ++i) {
                float ang = float(i) * 0.785398f;
                p.text(x + r * std::cos(ang), y + r * std::sin(ang), s, {.size = fs, .color = t.lyric_outline.with_alpha(a * a)}, max_w);
            }
            if (wiped) p.set_wipe(x + sung_x(p, *live, s, fs, soft, now), soft, t.lyric_text);
            p.text(x, y, s, {.size = fs, .color = c}, max_w);
            p.clear_wipe();
        };
        float y = dl_bar + dy;
        if (live && l.timed) one(live->text, size, y + l1 * 0.76f, sung, true);
        else one(l.text, size, y + l1 * 0.76f, l.timed ? sung : t.lyric_text, false);
        if (!l.sub.empty()) one(l.sub, sub, y + l1 + sub * 1.1f, t.lyric_text.with_alpha(0.85f), false);
    };
    // The old line leaves upwards before the new one has risen in, so the two hardly overlap.
    float out = std::min(k / 0.45f, 1.f), in = std::clamp((k - 0.3f) / 0.7f, 0.f, 1.f);
    if (out < 1) draw(dl_prev_, nullptr, -14 * out, 1 - out);
    draw(dl_line_, line, 14 * (1 - in), in);

    if (playing()) {
        if (timed) ui.keep_animating();
        else if (size_t next = size_t(cur + 1); next < lyrics_.size()) ui.wake(ui.now + (lyrics_[next].time - lead - now));
    }
    return input;
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
            const auto& account = accounts_[i];
            bool signed_in = account.has_value();
            if (signed_in) cover(ui, avatar, account->avatar, 20, "user");
            else {
                p.rect(avatar, t.text.with_alpha(0.08f), 20);
                p.icon("user", ui::center(avatar, 18, 18), t.text_dim);
            }
            ui.label({avatar.right() + 14, row.y + 12, 180, 22}, services[i], {.size = t.fs(15), .color = t.text});
            std::string status = signed_in ? account->name : provider(Source(i)).logged_in() ? "正在读取账号" : "未登录";
            ui.label({avatar.right() + 14, row.y + 34, 180, 18}, status, {.size = t.fs(12), .color = signed_in ? t.accent : t.text_dim});
            ui::Id id = ui.id(services[i]);
            Rect b{row.right() - 108, row.y + 15, 94, 34};
            if (provider(Source(i)).logged_in()) {
                if (ui.button(id, b, "退出登录", ui::ButtonKind::ghost, "log-out")) spawn(logout(Source(i)));
            } else if (ui.button(id, b, "扫码登录", ui::ButtonKind::secondary, "qr-code")) {
                login_step_ = i;
                login_time_ = ui.now;
                spawn(qr_login());
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
        qq_.cancel_qr();
        images_.erase("qr:qq");
        qr_image_ = false;
        return;
    }
    ui.label({r.x + 32, r.y, r.w - 64, 30}, "扫码登录", {.size = t.fs(20), .color = t.text});
    ui.label({r.x, r.y + 40, r.w, 20}, login_step_ == 1 ? "用 QQ 扫一扫，确认登录 QQ 音乐" : "用网易云音乐 App 扫一扫", {.size = t.fs(13), .color = t.text_dim},
             Align::center);
    Rect qr = ui::center({r.x, r.y + 76, r.w, 240}, 228, 228);
    p.shadow({qr.x, qr.y + 8, qr.w, qr.h}, 14, 30, t.shadow.with_alpha(0.6f));
    p.rect(qr, white, 14);
    QrState state = qr_status_ ? qr_status_->state : QrState::waiting;
    if (qr_image_ || (qr_size_ > 0 && !qr_.empty())) {
        // Modules as horizontal runs, so a row is a few rectangles.
        Rect q = qr.inset(14);
        ui::Color ink{0.07f, 0.07f, 0.08f, 1};
        if (qr_image_) {
            p.image(q, images_.get("qr:qq", 512).tex, 0);
        } else {
            float m = q.w / float(qr_size_);
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
        }
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
    else if (qr_.empty() && !qr_image_) line1 = "正在生成二维码", spin = true;
    else if (state == QrState::waiting) line1 = "等待扫码", line2 = "扫码后在手机上确认登录", spin = true;
    else if (state == QrState::scanned) line1 = "已扫码", line2 = qr_status_->nickname.empty() ? "请在手机上确认登录" : std::format("{}，请在手机上确认登录", qr_status_->nickname), spin = true;
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
    // Changes go straight to the config file; what is drawn is always what the file says.
    static constexpr std::string_view qualities[] = {"标准", "较高", "极高", "无损", "Hi-Res"};
    static constexpr const char* quality_keys[] = {"standard", "higher", "exhigh", "lossless", "hires"};
    Rect q = row("音质", "账号没有权限时自动降档");
    int quality = int(config_.playback.quality);
    if (ui.tabs(ui.id("quality"), {q.right() - 240, q.y + 14, 240, 30}, qualities, quality, ui::TabStyle::pill))
        set_setting_("playback", "quality", std::format("\"{}\"", quality_keys[quality]));
    auto toggle = [&](Rect c, std::string_view id, bool on, std::string_view section, std::string_view key, std::string_view yes,
                      std::string_view no) {
        if (ui.toggle(ui.id(id), {c.right() - 40, c.y + 19, 40, 22}, on)) set_setting_(section, key, on ? yes : no);
    };
    toggle(row("桌面歌词", "在其他窗口上方显示当前歌词"), "set-lyrics", config_.desktop_lyrics.enabled, "desktop_lyrics", "enabled", "true", "false");
    toggle(row("跳过试听片段", "只能试听的歌曲直接跳过"), "set-trial", skip_trials(), "playback", "trial", "\"skip\"", "\"play\"");
    toggle(row("音量均衡", "让不同歌曲的响度保持一致"), "set-normalize", config_.playback.normalize, "playback", "normalize", "true", "false");
    ui.label({r.x, r.bottom() - 20, r.w, 20}, "设置保存在 ~/.config/lyren/config.toml，更多选项也在里面", {.size = t.fs(12), .color = t.text_dim});
}

void Shell::categories_modal(ui::Ui& ui, Rect r) {
    const auto& t = ui.theme();
    ui.label({r.x, r.y, r.w - 40, 30}, "全部分类", {.size = t.fs(20), .color = t.text});
    constexpr float row = 38, indent = 64;
    float y = r.y + 52;
    std::optional<std::string> picked;
    ui.push_id(ui.id("all-categories"));
    for (const auto& [heading, names] : categories_.groups) {
        ui.label({r.x, y + 5, indent - 8, 20}, heading, {.size = t.fs(13), .color = t.text_dim});
        float x = r.x + indent;
        for (const auto& name : names) {
            float w = chip_w(ui, name);
            if (x + w > r.right()) x = r.x + indent, y += row;
            if (chip(ui, x, y, name, name == square_category_)) picked = name;
            x += w + 8;
        }
        y += row + 12;
    }
    ui.pop_id();
    // The dialog takes the height of what is in it (the body is inset 24 from the panel).
    ui.resize_modal(600, y - 12 - r.y + 48);
    if (picked) {
        set_category(ui, std::move(*picked));
        ui.close_modal();
    }
}

void Shell::open_comments(ui::Ui& ui, Source source, Subject subject, std::string id, std::string title) {
    thread_source_ = source;
    thread_subject_ = subject;
    thread_id_ = std::move(id);
    thread_title_ = std::move(title);
    thread_tab_ = 0;
    thread_time_ = ui.now;
    ++thread_gen_;
    for (auto& f : feeds_) f = {};
    for (const char* sid : {"comments-hot", "comments-new"}) ui.scroll_to(ui.id(sid), 0);
    load_comments(0);
    ui.open_modal(600, 640, [this](ui::Ui& u, Rect r) { comments_modal(u, r); });
}

void Shell::load_comments(int tab) {
    Feed& f = feeds_[tab];
    if (f.loading || !f.more || (f.page && !f.error.empty())) return;
    f.loading = true;
    f.error.clear();
    spawn(fetch_comments(thread_gen_, tab, f.page + 1, f.cursor));
}

Task<void> Shell::fetch_comments(uint64_t gen, int tab, int page, std::string cursor) {
    auto r = co_await provider(thread_source_).comments(thread_subject_, thread_id_, tab == 0, page, std::move(cursor));
    if (gen != thread_gen_) co_return;
    Feed& f = feeds_[tab];
    f.loading = false;
    if (!r) {
        f.error = r.error();
        co_return redraw_();
    }
    if (!f.page) f.since = now_();
    f.page = page;
    f.total = r->total;
    f.more = r->more && !r->comments.empty();
    f.cursor = std::move(r->cursor);
    f.comments.insert(f.comments.end(), std::make_move_iterator(r->comments.begin()), std::make_move_iterator(r->comments.end()));
    redraw_();
}

void Shell::comments_modal(ui::Ui& ui, Rect r) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    ui.resize_modal(600, std::min(ui.height - 80, 720.f));
    ui.label({r.x, r.y, r.w - 40, 30}, "评论", {.size = t.fs(20), .color = t.text});
    ui.label({r.x, r.y + 32, r.w - 40, 20}, thread_title_, {.size = t.fs(13), .color = t.text_dim});
    static constexpr std::string_view tabs[] = {"热门", "最新"};
    if (ui.tabs(ui.id("comment-tabs"), {r.x, r.y + 66, 140, 34}, tabs, thread_tab_)) {
        thread_time_ = ui.now;
        load_comments(thread_tab_);
    }
    Feed& f = feeds_[thread_tab_];
    if (f.total > 0) ui.label({r.x, r.y + 74, r.w, 20}, std::format("共 {} 条", f.total), {.size = t.fs(12), .color = t.text_dim}, Align::right);
    p.rect({r.x, r.y + 100, r.w, 1}, t.border);

    float enter = ui.progress(thread_time_, 380);
    Fade fade(p, enter);
    // The view reaches into the panel's padding so the scrollbar sits by its edge.
    Rect view{r.x + (1 - enter) * 28 * (thread_tab_ ? 1 : -1), r.y + 101, r.w + 18, r.h - 101};
    const float w = r.w;
    if (f.comments.empty()) {
        page_status(ui, view, f.loading, f.error.empty() && !f.loading ? "还没有评论" : f.error, false);
        return;
    }
    // Heights are kept, the wrapped lines are not: they would point into comments that move as more arrive.
    float size = t.fs(14);
    if (f.heights_w != w || f.heights_size != size) f.heights.clear(), f.heights_w = w, f.heights_size = size;
    while (f.heights.size() < f.comments.size()) f.heights.push_back(comment(ui, f.comments[f.heights.size()], 0, 0, w, false));
    float content = (f.loading ? 64 : 16);
    for (float h : f.heights) content += h;
    float off = ui.begin_scroll(ui.id(thread_tab_ ? "comments-new" : "comments-hot"), view, content);
    Rect clip = ui.clip();
    float y = view.y - off;
    for (size_t i = 0; i < f.comments.size(); ++i) {
        float h = f.heights[i];
        if (y + h > clip.y && y < clip.bottom()) {
            // The first page arrives one comment after another.
            float a = ui.progress(f.since + 0.03 * double(std::min<size_t>(i, 10)), 380);
            Fade arrive(p, a);
            comment(ui, f.comments[i], view.x, y + (1 - a) * 12, w, true);
        }
        y += h;
    }
    if (f.loading) ui.spinner(ui::center({view.x, y, w, 64}, 20, 20), t.text_dim, 2);
    else if (f.more && y < view.bottom() + 400) load_comments(thread_tab_);
    ui.end_scroll();
}

float Shell::comment(ui::Ui& ui, const Comment& c, float x, float y, float w, bool draw) {
    auto& p = ui.painter();
    const auto& t = ui.theme();
    TextStyle body{.size = t.fs(14), .color = t.text}, quote{.size = t.fs(13), .color = t.text_dim}, small{.size = t.fs(12), .color = t.text_dim};
    const float lh = std::round(body.size * 1.6f), qh = std::round(quote.size * 1.6f), tx = x + 50, tw = w - 50;
    std::string replied = c.reply.empty() ? "" : std::format("@{}：{}", c.reply_user, c.reply);
    auto lines = wrap_lines(p, c.text, body.size, tw);
    std::vector<std::string_view> quoted;
    if (!replied.empty()) quoted = wrap_lines(p, replied, quote.size, tw - 24);
    // Avatar and name, the text, the comment it answers, the time.
    float quote_h = quoted.empty() ? 0 : float(quoted.size()) * qh + 26;
    float h = 18 + 22 + float(lines.size()) * lh + quote_h + 26 + 16;
    if (!draw) return h;
    float top = y + 18;
    cover(ui, {x, top, 36, 36}, c.avatar, 18, "user");
    if (c.likes > 0) {
        std::string n = short_count(c.likes);
        float nw = p.measure(n, small);
        ui.label({x + w - nw, top, nw + 1, 18}, n, small);
        p.icon("thumbs-up", {x + w - nw - 19, top + 2, 14, 14}, t.text_dim);
    }
    p.text(tx, top + 14, c.user, {.size = t.fs(13), .color = t.text_dim}, tw - 90);
    float ly = top + 22;
    for (auto l : lines) {
        p.text(tx, ly + lh * 0.72f, l, body, tw);
        ly += lh;
    }
    if (!quoted.empty()) {
        Rect q{tx, ly + 10, tw, quote_h - 10};
        p.rect(q, t.text.with_alpha(0.05f), t.radius_s());
        float qy = q.y + 8;
        for (auto l : quoted) {
            p.text(q.x + 12, qy + qh * 0.72f, l, quote, tw - 24);
            qy += qh;
        }
        ly += quote_h;
    }
    p.text(tx, ly + 20, c.time, {.size = small.size, .color = t.text_dim.with_alpha(0.7f)}, tw);
    p.rect({tx, y + h - 1, tw, 1}, t.border);
    return h;
}

void Shell::open_editor(ui::Ui& ui, Card card, std::optional<Track> track) {
    edit_name_ = card.title;
    edit_hidden_ = false;
    edit_focus_ = true;
    edit_track_ = std::move(track);
    bool ncm = ncm_.logged_in() && (!edit_track_ || recording(*edit_track_, Source::netease));
    bool qq = qq_.logged_in() && (!edit_track_ || recording(*edit_track_, Source::qq));
    edit_choose_source_ = card.id.empty() && ncm && qq;
    edit_source_ = card.id.empty() ? ncm ? Source::netease : Source::qq : card.source;
    ui.open_modal(400, card.id.empty() ? edit_choose_source_ ? 338 : 286 : 216, [this, card](ui::Ui& u, Rect r) { editor_modal(u, r, card); });
}

void Shell::editor_modal(ui::Ui& ui, Rect r, Card card) {
    const auto& t = ui.theme();
    bool create = card.id.empty();
    ui.label({r.x, r.y, r.w - 40, 30}, create ? "新建歌单" : "重命名歌单", {.size = t.fs(20), .color = t.text});
    ui::Id field = ui.id("playlist-name");
    if (std::exchange(edit_focus_, false)) ui.focus = field;
    bool submit = ui.text_box(field, {r.x, r.y + 52, r.w, 38}, edit_name_, "歌单名称");
    if (create) {
        float y = r.y + 110;
        if (edit_choose_source_) {
            static constexpr std::string_view services[] = {"网易云音乐", "QQ 音乐"};
            int source = int(edit_source_);
            ui.tabs(ui.id("playlist-account"), {r.x, y, r.w, 34}, services, source, ui::TabStyle::pill);
            edit_source_ = Source(source);
            y += 52;
        }
        ui.label({r.x, y, r.w - 60, 20}, "隐私歌单", {.size = t.fs(14), .color = t.text});
        ui.label({r.x, y + 22, r.w - 60, 18}, "只有你自己能看到", {.size = t.fs(12), .color = t.text_dim});
        ui.toggle(ui.id("playlist-hidden"), {r.right() - 40, y + 9, 40, 22}, edit_hidden_);
    }
    Rect ok{r.right() - 96, r.bottom() - 38, 96, 38};
    if (ui.button(ui.id("playlist-cancel"), {ok.x - 92, ok.y, 84, 38}, "取消", ui::ButtonKind::ghost)) ui.close_modal();
    submit |= ui.button(ui.id("playlist-save"), ok, create ? "创建" : "保存", ui::ButtonKind::primary);
    std::string name = edit_name_;
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    if (!submit || name.empty()) return;
    ui.close_modal();
    if (create) {
        spawn([](Shell* self, Source source, std::string name, bool hidden, std::optional<Track> track) -> Task<void> {
            auto r = co_await self->provider(source).create_playlist(name, hidden);
            if (!r) co_return self->notify(std::format("没有创建歌单：{}", r.error()), "triangle-alert", true);
            self->playlists_.insert(self->playlists_.begin(), *r);
            self->notify(std::format("已创建歌单「{}」", name), "circle-check");
            if (track) self->add_to_playlist(std::move(*track), *r);
            else if (source == Source::qq) co_await self->load_library(source);
        }(this, edit_source_, name, edit_hidden_, edit_track_));
        return;
    }
    if (name == card.title) return;
    retitle(card, name);
    spawn([](Shell* self, Card card, std::string name) -> Task<void> {
        auto r = co_await self->provider(card).rename_playlist(card.id, name);
        if (r) {
            if (card.source == Source::qq) co_await self->load_library(Source::qq);
            co_return;
        }
        self->retitle(card, card.title);
        self->notify(std::format("没有改名：{}", r.error()), "triangle-alert", true);
    }(this, std::move(card), std::move(name)));
}

void Shell::confirm_delete(ui::Ui& ui, Card card) {
    ui.open_modal(400, 196, [this, card](ui::Ui& u, Rect r) {
        const auto& t = u.theme();
        u.label({r.x, r.y, r.w - 40, 30}, "删除歌单", {.size = t.fs(20), .color = t.text});
        u.label({r.x, r.y + 48, r.w, 20}, std::format("「{}」会被删除，不能恢复。", card.title), {.size = t.fs(13), .color = t.text_dim});
        Rect ok{r.right() - 96, r.bottom() - 38, 96, 38};
        if (u.button(u.id("delete-cancel"), {ok.x - 92, ok.y, 84, 38}, "取消", ui::ButtonKind::ghost)) u.close_modal();
        if (!u.button(u.id("delete-ok"), ok, "删除", ui::ButtonKind::primary, "trash")) return;
        u.close_modal();
        auto it = std::ranges::find_if(playlists_, [&](const Card& c) { return same(c, card); });
        size_t at = size_t(it - playlists_.begin());
        if (it != playlists_.end()) playlists_.erase(it);
        if (route_.page == Page::playlist && same(route_.card, card)) back(u);
        spawn([](Shell* self, Card card, size_t at) -> Task<void> {
            auto r = co_await self->provider(card).delete_playlist(card.id);
            if (r) co_return self->notify(std::format("已删除歌单「{}」", card.title), "trash");
            self->playlists_.insert(self->playlists_.begin() + std::min(at, self->playlists_.size()), card);
            self->notify(std::format("没有删除歌单：{}", r.error()), "triangle-alert", true);
        }(this, card, at));
    });
}

void Shell::picker_modal(ui::Ui& ui, Rect r, Track t) {
    auto& p = ui.painter();
    const auto& th = ui.theme();
    ui.label({r.x, r.y, r.w - 40, 30}, "添加到歌单", {.size = th.fs(20), .color = th.text});
    ui.label({r.x, r.y + 32, r.w - 40, 20}, std::format("{} · {}", t.title, t.artist), {.size = th.fs(13), .color = th.text_dim});
    std::vector<const Card*> own;
    for (const Card& c : playlists_)
        if (c.owned && provider(c).logged_in() && recording(t, c.source)) own.push_back(&c);
    constexpr float row_h = 56, top = 68;
    float list_h = float(own.size() + 1) * row_h;
    ui.resize_modal(420, std::min(ui.height - 120, top + list_h + 48));
    Rect view{r.x - 8, r.y + top, r.w + 24, r.h - top};
    float off = ui.begin_scroll(ui.id("picker"), view, list_h);
    auto entry = [&](size_t i, std::string_view title, std::string_view sub, const std::string* art) {
        Rect e{view.x, view.y + float(i) * row_h - off, r.w + 16, row_h};
        ui::Id id = ui.id(uint64_t(i));
        auto it = ui.interact(id, e);
        float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
        if (hot > 0) p.rect(e.inset(2), th.hover.with_alpha(hot), th.radius_s());
        Rect cov{e.x + 10, e.y + 8, 40, 40};
        if (art) {
            cover(ui, cov, *art, th.radius_s(), "list-music");
        } else {
            p.rect(cov, th.text.with_alpha(0.07f), th.radius_s());
            p.icon("plus", ui::center(cov, 18, 18), th.text_dim);
        }
        p.text(cov.right() + 14, e.y + (sub.empty() ? 34 : 25), title, {.size = th.fs(14), .color = th.text}, e.w - 80);
        if (!sub.empty()) p.text(cov.right() + 14, e.y + 43, sub, {.size = th.fs(12), .color = th.text_dim}, e.w - 80);
        return it.clicked;
    };
    std::optional<Card> picked;
    ui.push_id(ui.id("picker-rows"));
    bool create = entry(0, "新建歌单", "", nullptr);
    for (size_t i = 0; i < own.size(); ++i)
        if (entry(i + 1, own[i]->title, own[i]->subtitle, &own[i]->cover)) picked = *own[i];
    ui.pop_id();
    ui.end_scroll();
    if (create) open_editor(ui, {}, t);
    if (picked) {
        ui.close_modal();
        add_to_playlist(std::move(t), std::move(*picked));
    }
}

}  // namespace lyren
