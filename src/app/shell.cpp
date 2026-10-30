#include "app/shell.hpp"

#include <linux/input-event-codes.h>
#include <qrencode.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <ctime>
#include <format>
#include <iterator>
#include <inplace_vector>
#include <numbers>
#include <ranges>

#include "app/draw.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/utf8.hpp"
#include "sys/creds.hpp"

namespace lyren {

    using ui::Align;
    using ui::hsl;
    using ui::Rect;
    using ui::TextStyle;
    using State = audio::Player::State;
    namespace ease = ui::ease;

    constexpr ui::Color white{1, 1, 1, 1};

    // Helpers.

    static uint64_t next(uint64_t &s) {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    // Artwork for lists that have no cover of their own (liked, recent, cloud, daily): a gradient with two soft discs.
    static void art(ui::Painter &p, Rect r, float hue, float radius, std::string_view icon) {
        p.gradient(r, hsl(hue, 0.42f, 0.54f), hsl(hue + 0.08f, 0.4f, 0.22f), radius);
        float a = std::fmod(hue * 7.31f, 1.f), b = std::fmod(hue * 13.7f, 1.f);
        float d1 = r.w * (0.5f + 0.35f * a), d2 = r.w * (0.3f + 0.25f * b);
        p.rect({r.x + (r.w - d1) * b, r.y + (r.h - d1) * (1 - a), d1, d1}, hsl(hue + 0.1f, 0.5f, 0.72f).with_alpha(0.26f), d1 / 2);
        p.rect({r.x + (r.w - d2) * (1 - a), r.y + (r.h - d2) * b, d2, d2}, hsl(hue - 0.12f, 0.55f, 0.62f).with_alpha(0.42f), d2 / 2);
        float s = std::round(r.w * 0.36f);
        p.icon(icon, ui::center(r, s, s), white.with_alpha(0.85f));
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

    // Animate row/column changes; ordinary resizing and scrolling stay under the pointer.
    struct Grid {
        static constexpr float gap = 20;
        static constexpr float reflow_ms = 280;
        ui::Ui &ui;
        ui::Id id;
        int cols;
        float cw, ch;
        Grid(ui::Ui &ui, ui::Id id, float width, float min_w = 150)
        : ui(ui),
          id(id) {
            cols = std::max(1, static_cast<int>((width + gap) / (min_w + gap)));
            float count = ui.spring(ui::sub_id(id, 0xc0), static_cast<float>(cols), reflow_ms);
            cw = (width + gap) / count - gap;
            ch = cw + 54;
        }
        float height(size_t n) const {
            if (!n) {
                return 0;
            }
            float last = ui.spring(ui::sub_id(id, 0xc1), static_cast<float>((n - 1) / static_cast<size_t>(cols)), reflow_ms);
            return last * (ch + gap) + ch;
        }
        Rect cell(float x, float y, size_t i, ui::Id item) const {
            float col = ui.spring(ui::sub_id(item, 0xc0), static_cast<float>(i % static_cast<size_t>(cols)), reflow_ms);
            float row = ui.spring(ui::sub_id(item, 0xc1), static_cast<float>(i / static_cast<size_t>(cols)), reflow_ms);
            return {x + col * (cw + gap), y + row * (ch + gap), cw, ch};
        }
    };

    static void page_title(ui::Ui &ui, Rect body, float y, std::string_view title, std::string_view subtitle) {
        const auto &t = ui.theme();
        ui.label({body.x, y, body.w, 38}, title, {.size = t.fs(28), .color = t.text});
        ui.label({body.x, y + 42, body.w, 20}, subtitle, {.size = t.fs(13), .color = t.text_dim});
    }

    static std::string date_label(std::string_view date) {
        const int month = (date[0] - '0') * 10 + date[1] - '0';
        const int day = (date[3] - '0') * 10 + date[4] - '0';
        return lyren::trf("{}/{}", month, day);
    }

    static void prefetch_covers(Images &images, std::span<const Card> cards) {
        for (const auto &card : cards.first(std::min(cards.size(), 16uz))) {
            images.prefetch(card.cover, 128);
        }
    }

    static void prefetch_covers(Images &images, std::span<const Track> tracks) {
        for (const auto &track : tracks.first(std::min(tracks.size(), 32uz))) {
            images.prefetch(track.cover, 64);
        }
    }

    // A page of a listing arrived; an empty one ends it, whatever the total said.
    static void arrived(Images &images, auto &res, Result<Batch> r, double now) {
        res.loading = false;
        if (!r) {
            res.error = r.error();
            return;
        }
        if (!res.loaded) {
            res.since = now;
        }
        res.loaded = true;
        prefetch_covers(images, r->tracks);
        prefetch_covers(images, r->cards);
        bool empty = r->tracks.empty() && r->cards.empty();
        res.tracks.insert(res.tracks.end(), std::make_move_iterator(r->tracks.begin()), std::make_move_iterator(r->tracks.end()));
        res.cards.insert(res.cards.end(), std::make_move_iterator(r->cards.begin()), std::make_move_iterator(r->cards.end()));
        res.total = empty ? static_cast<int>(res.size()) : std::max(r->total, static_cast<int>(res.size()));
    }

    // Puts a card first in a list or takes it out.
    static void place(std::vector<Card> &cards, const Card &c, bool on) {
        std::erase_if(cards, [&](const Card &x) {
            return same(x, c);
        });
        if (on) {
            cards.insert(cards.begin(), c);
        }
    }

    // A filter pill.
    static float chip_w(ui::Ui &ui, std::string_view label, bool icon = false) {
        return ui.painter().measure(label, {.size = ui.theme().fs(13)}) + 28 + (icon ? 18 : 0);
    }

    static bool chip(ui::Ui &ui, float x, float y, std::string_view label, bool selected, std::string_view icon = {}) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
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
        if (!icon.empty()) {
            p.icon(icon, {c.x + 18 + tw, c.y + (c.h - 14) / 2, 14, 14}, fg);
        }
        return it.clicked;
    }

    // State.

    Shell::Shell(ShellDeps d, SavedState saved)
    : loop_(d.loop),
      http_(d.http),
      player_(d.player),
      providers_(d.providers),
      images_(d.images),
      config_(d.config),
      set_setting_(std::move(d.set_setting)),
      redraw_(std::move(d.redraw)),
      now_(std::move(d.now)),
      silent_(d.silent),
      open_config_(std::move(d.open_config)) {
        set_language(config_.interface.language);
        std::time_t now = std::time(nullptr);
        std::tm lt;
        localtime_r(&now, &lt);
        today_ = std::format("{:02}-{:02}", lt.tm_mon + 1, lt.tm_mday);

        if (saved.width > 0) {
            window_ = std::pair{saved.width, saved.height};
        }
        active_ = saved.source;
        route_.card.source = active_;
        volume_ = saved.volume;
        muted_ = saved.muted;
        mode_ = saved.mode;
        queue_ = std::move(saved.queue);
        current_ = saved.current;
        resume_at_ = last_pos_ = saved.position;
        recent_ = std::move(saved.recent);
        refresh_recent();
        prefetch_covers(images_, queue_);
        dl_x_ = saved.lyrics_x, dl_bottom_ = saved.lyrics_bottom, dl_locked_ = saved.lyrics_locked;
        dl_enabled_ = saved.lyrics_enabled;
        dl_island_ = saved.lyrics_island;
        lyric_translation_ = saved.lyrics_translation;
        desktop_changed_ = std::move(d.desktop_changed);
        rng_ ^= static_cast<uint64_t>(now);

        player_.on_change = [this] {
            on_player();
        };
        apply_volume();
        for (auto *p : providers_) {
            p->on_session_changed([this] {
                save_credentials();
            });
        }
        if (current()) {
            spawn(load_lyrics(*current()));
        }
        spawn(startup());

        if (!d.bus.get()) {
            return;
        }
        Mpris::Handlers h{
            .raise = std::move(d.raise),
            .quit =
                [this] {
                    loop_.quit();
                },
            .play =
                [this] {
                    if (!playing()) {
                        toggle_play();
                    }
                },
            .pause =
                [this] {
                    if (playing()) {
                        toggle_play();
                    }
                },
            .toggle =
                [this] {
                    toggle_play();
                },
            .stop =
                [this] {
                    stop();
                },
            .next =
                [this] {
                    skip(1);
                },
            .previous =
                [this] {
                    skip(-1);
                },
            .seek =
                [this](double at) {
                    seek_song(at);
                },
            .volume =
                [this](double v) {
                    volume_ = static_cast<float>(v), muted_ = false;
                    apply_volume();
                    changed();
                },
            .loop_one =
                [this](bool on) {
                    if (on) {
                        set_mode(Mode::one);
                    } else if (mode_ == Mode::one) {
                        set_mode(Mode::list);
                    }
                },
            .shuffle =
                [this](bool on) {
                    if (on) {
                        set_mode(Mode::shuffle);
                    } else if (mode_ == Mode::shuffle) {
                        set_mode(Mode::list);
                    }
                },
            .position =
                [this] {
                    return timeline().to_song(position());
                },
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
        if (window) {
            window_ = window;
        }
        SavedState s{.source = active_,
                     .volume = volume_,
                     .muted = muted_,
                     .mode = mode_,
                     .queue = queue_,
                     .current = current_,
                     .recent = recent_,
                     .lyrics_x = dl_x_,
                     .lyrics_bottom = dl_bottom_,
                     .lyrics_locked = dl_locked_,
                     .lyrics_enabled = dl_enabled_,
                     .lyrics_island = dl_island_,
                     .lyrics_translation = lyric_translation_};
        if (window_) {
            s.width = window_->first, s.height = window_->second;
        }
        auto st = player_.state();
        s.position = st == State::playing || st == State::paused ? player_.position() : resume_at_ > 0 ? resume_at_ : last_pos_;
        save_state(s);
    }

    void Shell::config_changed() {
        apply_gain();
    }

    ui::Color Shell::accent(double now, bool dark, bool &animating) {
        // -2: not known yet (cover still loading), -1: a colourless cover.
        float hue = current() ? cover_hue(current()->cover, bar_cover_px_) : 0.98f;
        auto color = [](float h, bool on_dark) {
            if (h == -1) {
                return on_dark ? ui::Color{0.86f, 0.86f, 0.88f, 1} : ui::Color{0.29f, 0.33f, 0.40f, 1};
            }
            return ui::accent_for_hue(h, on_dark);
        };
        const double duration = 0.8 * config_.theme.motion;
        auto progress = [&] {
            return duration > 0 ? ease::standard(std::clamp(static_cast<float>((now - accent_time_) / duration), 0.f, 1.f)) : 1.f;
        };
        if (accent_hue_ == -2 && hue == -2) {
            hue = 0.98f;
        }
        if (hue != -2 && hue != accent_hue_) {
            bool first = accent_hue_ == -2;
            for (bool on_dark : {false, true}) {
                auto &from = accent_from_[on_dark];
                from = first ? color(hue, on_dark) : from.mix(color(accent_hue_, on_dark), progress());
            }
            accent_hue_ = hue;
            accent_time_ = first ? -10 : now;
        }
        float t = progress();
        if (t < 1) {
            animating = true;
        }
        return accent_from_[dark].mix(color(accent_hue_, dark), t);
    }

    float Shell::cover_hue(const std::string &url, float size) {
        int px = Images::tier(size);
        const auto &e = images_.get(url, static_cast<uint32_t>(px));
        return e.ready ? e.hue : e.failed ? -1 : -2;
    }

    float Shell::special_hue(Page page) const {
        switch (page) {
            case Page::liked:
                return 0.97f;
            case Page::recent:
                return 0.72f;
            case Page::cloud:
                return 0.58f;
            default:
                return 0.98f;
        }
    }

    bool Shell::liked(const Track &t) const {
        return library(t.source).liked_ids.contains(t.id);
    }

    void Shell::toggle_like(Track t) {
        if (!provider(t).logged_in()) {
            return notify(lyren::trf("Sign in to {} to like this song", lyren::tr(platform(t.source).name)), "heart");
        }
        bool on = !liked(t);
        auto &ids = library(t.source).liked_ids;
        if (on) {
            ids.insert(t.id);
        } else {
            ids.erase(t.id);
        }
        if (on) {
            pop_key_ = key(t), pop_time_ = now_();
        }
        auto list = route_.page == Page::liked && active_ == t.source ? route_.list : nullptr;
        if (list) {
            auto &v = list->tracks;
            if (on) {
                v.insert(v.begin(), t);
            } else {
                std::erase_if(v, [&](const Track &x) {
                    return same(x, t);
                });
            }
        }
        notify(std::string(lyren::tr(on ? "Added to liked songs" : "Removed from liked songs")), on ? "heart-fill" : "heart-off");
        spawn([](Shell *self, Track t, bool on, std::shared_ptr<List> list) -> Task<void> {
            auto r = co_await self->provider(t).like(t, on);
            if (!r) {
                auto &ids = self->library(t.source).liked_ids;
                if (on) {
                    ids.erase(t.id);
                } else {
                    ids.insert(t.id);
                }
                if (list) {
                    std::erase_if(list->tracks, [&](const Track &x) {
                        return same(x, t);
                    });
                    if (!on) {
                        list->tracks.insert(list->tracks.begin(), t);
                    }
                }
                self->notify(lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
            } else if (self->provider(t).reload_after_edit && list) {
                co_await self->load_list(list, Page::liked, {.source = t.source});
            }
        }(this, std::move(t), on, std::move(list)));
    }

    // Following, collecting and editing playlists show at once; a failed request puts things back.

    void Shell::toggle_follow(std::shared_ptr<ArtistPage> page, Card artist) {
        if (!provider(artist).logged_in()) {
            return notify(std::string(lyren::tr("Sign in to this artist's platform to follow them")), "user-plus");
        }
        bool on = !page->info->followed;
        page->info->followed = on;
        auto &collection = library(artist.source).collection[1];
        if (collection.loaded) {
            place(collection.cards, artist, on);
        }
        notify(on ? lyren::trf("Following {}", artist.title) : std::string(lyren::tr("Unfollowed artist")), on ? "user-check" : "user");
        spawn([](Shell *self, std::shared_ptr<ArtistPage> page, Card artist, bool on) -> Task<void> {
            auto r = co_await self->provider(artist).follow(artist.id, on);
            if (r) {
                co_return;
            }
            page->info->followed = !on;
            auto &collection = self->library(artist.source).collection[1];
            if (collection.loaded) {
                place(collection.cards, artist, !on);
            }
            self->notify(lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
        }(this, std::move(page), std::move(artist), on));
    }

    void Shell::toggle_collect(std::shared_ptr<List> list, Card album) {
        if (!provider(album).logged_in()) {
            return notify(std::string(lyren::tr("Sign in to this album's platform to save it")), "star");
        }
        bool on = !list->album.collected;
        list->album.collected = on;
        auto &collection = library(album.source).collection[0];
        if (collection.loaded) {
            place(collection.cards, album, on);
        }
        notify(std::string(lyren::tr(on ? "Album saved" : "Album removed")), on ? "star-fill" : "star");
        spawn([](Shell *self, std::shared_ptr<List> list, Card album, bool on) -> Task<void> {
            auto r = co_await self->provider(album).collect(album.id, on);
            if (r) {
                co_return;
            }
            list->album.collected = !on;
            auto &collection = self->library(album.source).collection[0];
            if (collection.loaded) {
                place(collection.cards, album, !on);
            }
            self->notify(lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
        }(this, std::move(list), std::move(album), on));
    }

    void Shell::recount(const Card &playlist, int delta) {
        for (Card &c : library(playlist.source).playlists) {
            if (same(c, playlist)) {
                c.count = std::max(0, c.count + delta);
                c.count_kind = Card::Count::songs;
            }
        }
    }

    void Shell::toggle_collect_playlist(std::shared_ptr<List> list, Card playlist) {
        if (!provider(playlist).logged_in()) {
            return notify(std::string(lyren::tr("Sign in to this playlist's platform to save it")), "star");
        }
        auto &lib = library(playlist.source);
        bool on = !std::ranges::any_of(lib.playlists, [&](const Card &card) {
            return same(card, playlist);
        });
        uint64_t gen = lib.gen;
        place(lib.playlists, playlist, on);
        list->saving_playlist = true;
        spawn([](Shell *self, std::shared_ptr<List> list, Card playlist, bool on, uint64_t gen) -> Task<void> {
            auto &lib = self->library(playlist.source);
            auto r = co_await self->provider(playlist).features.library.collect_playlist(playlist.id, on);
            if (gen != lib.gen) {
                list->saving_playlist = false;
                co_return;
            }
            if (!r) {
                list->saving_playlist = false;
                place(lib.playlists, playlist, !on);
                co_return self->notify(lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
            }
            self->notify(std::string(lyren::tr(on ? "Playlist saved" : "Playlist removed")), on ? "star-fill" : "star");
            auto favorites = co_await self->provider(playlist).features.library.collected_playlists();
            list->saving_playlist = false;
            if (gen != lib.gen) {
                co_return;
            }
            if (favorites) {
                std::erase_if(lib.playlists, [](const Card &card) {
                    return !card.owned;
                });
                lib.playlists.insert(lib.playlists.end(), std::make_move_iterator(favorites->begin()), std::make_move_iterator(favorites->end()));
            } else {
                self->notify(favorites.error(), "list-music", true);
            }
            self->redraw_();
        }(this, std::move(list), std::move(playlist), on, gen));
    }

    void Shell::add_to_playlist(Track t, Card playlist) {
        auto list = route_.page == Page::playlist && same(route_.card, playlist) ? route_.list : nullptr;
        if (list && list->saving_order) {
            return notify(std::string(lyren::tr("The playlist order is still saving")), "list-ordered");
        }
        if (list && std::ranges::any_of(list->tracks, [&](const Track &x) {
                return same(x, t);
            })) {
            return notify(std::string(lyren::tr("This song is already in the playlist")), "folder-plus");
        }
        recount(playlist, 1);
        // New songs go to the top, as in NetEase's own apps.
        if (list) {
            list->tracks.insert(list->tracks.begin(), t);
        }
        notify(lyren::trf("Added to {}", playlist.title), "folder-plus");
        spawn([](Shell *self, Track t, Card playlist, std::shared_ptr<List> list) -> Task<void> {
            auto r = co_await self->provider(playlist).edit_playlist(playlist.id, t, true);
            if (r) {
                if (self->provider(playlist).reload_after_edit) {
                    if (list) {
                        co_await self->load_list(list, Page::playlist, playlist);
                    }
                    co_await self->load_library(playlist.source);
                }
                co_return;
            }
            self->recount(playlist, -1);
            if (list) {
                std::erase_if(list->tracks, [&](const Track &x) {
                    return same(x, t);
                });
            }
            self->notify(lyren::trf("Could not add to {}: {}", playlist.title, r.error()), "triangle-alert", true);
        }(this, std::move(t), std::move(playlist), std::move(list)));
    }

    void Shell::remove_from_playlist(std::shared_ptr<List> list, Card playlist, Track t) {
        if (list->saving_order) {
            return notify(std::string(lyren::tr("The playlist order is still saving")), "list-ordered");
        }
        auto it = std::ranges::find_if(list->tracks, [&](const Track &x) {
            return same(x, t);
        });
        if (it == list->tracks.end()) {
            return;
        }
        size_t at = static_cast<size_t>(it - list->tracks.begin());
        list->tracks.erase(it);
        recount(playlist, -1);
        notify(lyren::trf("Removed from {}", playlist.title), "list-minus");
        spawn([](Shell *self, std::shared_ptr<List> list, Card playlist, Track t, size_t at) -> Task<void> {
            auto r = co_await self->provider(playlist).edit_playlist(playlist.id, t, false);
            if (r) {
                if (self->provider(playlist).reload_after_edit) {
                    co_await self->load_list(list, Page::playlist, playlist);
                    co_await self->load_library(playlist.source);
                }
                co_return;
            }
            list->tracks.insert(list->tracks.begin() + std::min(at, list->tracks.size()), std::move(t));
            self->recount(playlist, 1);
            self->notify(lyren::trf("Could not remove: {}", r.error()), "triangle-alert", true);
        }(this, std::move(list), std::move(playlist), std::move(t), at));
    }

    void Shell::move_playlist_track(std::shared_ptr<List> list, Card playlist, int from, int to) {
        if (!provider(playlist).logged_in()) {
            return notify(lyren::trf("Sign in to {} to reorder this playlist", lyren::tr(platform(playlist.source).name)), "user");
        }
        if (list->saving_order) {
            return;
        }
        auto first = list->tracks.begin();
        if (from < to) {
            std::rotate(first + from, first + from + 1, first + to + 1);
        } else {
            std::rotate(first + to, first + from, first + from + 1);
        }
        selected_ = to;
        list->saving_order = true;
        uint64_t gen = library(playlist.source).gen;
        redraw_();
        spawn([](Shell *self, std::shared_ptr<List> list, Card playlist, std::vector<Track> order, int selected, uint64_t gen) -> Task<void> {
            auto r = co_await self->provider(playlist).features.library.reorder_playlist(playlist.id, order);
            auto &lib = self->library(playlist.source);
            if (gen != lib.gen) {
                list->saving_order = false;
                co_return self->redraw_();
            }
            // A lost write response may still have committed; read back before reporting the result.
            auto fresh = co_await self->provider(playlist).tracks(playlist);
            list->saving_order = false;
            if (gen != lib.gen) {
                co_return self->redraw_();
            }
            if (!fresh) {
                list->error = fresh.error();
                co_return self->notify(lyren::trf("Could not refresh playlist order: {}", fresh.error()), "triangle-alert", true);
            }
            bool confirmed = std::ranges::equal(*fresh, order, [](const Track &a, const Track &b) {
                return same(a, b) && a.type == b.type;
            });
            list->tracks = std::move(*fresh);
            list->error.clear();
            if (self->route_.list == list) {
                auto it = std::ranges::find(list->tracks, order[static_cast<size_t>(selected)].id, &Track::id);
                self->selected_ = it == list->tracks.end() ? -1 : static_cast<int>(it - list->tracks.begin());
            }
            if (confirmed) {
                self->notify(std::string(lyren::tr("Playlist order saved")), "list-ordered");
            } else {
                self->notify(r ? std::string(lyren::tr("Playlist order was not saved")) : lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
            }
            self->redraw_();
        }(this, list, std::move(playlist), list->tracks, to, gen));
    }

    void Shell::link_menu(ui::Ui &ui, float x, float y, const Card &card) {
        auto link = share_url(card);
        if (link.empty()) {
            return;
        }
        ui.open_menu(x, y, {{"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                 ui.copy = link;
                                 ui.toast(std::string(lyren::tr("Link copied")), "copy");
                             }}});
    }

    // The song leaves the daily list at once; the one the server recommends instead takes its place.
    void Shell::dislike_daily(std::shared_ptr<List> list, Track t) {
        auto it = std::ranges::find_if(list->tracks, [&](const Track &x) {
            return same(x, t);
        });
        if (it == list->tracks.end()) {
            return;
        }
        size_t at = static_cast<size_t>(it - list->tracks.begin());
        list->tracks.erase(it);
        spawn([](Shell *self, std::shared_ptr<List> list, Track t, size_t at) -> Task<void> {
            auto r = co_await self->provider(t).features.daily.dislike(t.id);
            auto pos = list->tracks.begin() + std::min(at, list->tracks.size());
            if (!r) {
                list->tracks.insert(pos, std::move(t));
                co_return self->notify(lyren::trf("Could not mark as not interested: {}", r.error()), "triangle-alert", true);
            }
            if (*r) {
                list->tracks.insert(pos, std::move(**r));
            }
            self->notify(std::string(lyren::tr("You will hear fewer songs like this")), "thumbs-down");
        }(this, std::move(list), std::move(t), at));
    }

    Task<void> Shell::heartbeat(Track seed) {
        auto r = co_await provider(seed).features.heartbeat(seed.id, library(seed.source).liked.id);
        if (!r) {
            co_return notify(lyren::trf("Could not start Heartbeat mode: {}", r.error()), "triangle-alert", true);
        }
        std::vector<Track> list = std::move(*r);
        std::erase_if(list, [&](const Track &x) {
            return same(x, seed);
        });
        list.insert(list.begin(), std::move(seed));
        if (mode_ == Mode::shuffle) {
            set_mode(Mode::list);
        }
        play(list, 0);
        notify(std::string(lyren::tr("Heartbeat mode mixes your favorites with similar songs")), "heart-pulse");
    }

    void Shell::retitle(const Card &playlist, const std::string &title) {
        for (Card &c : library(playlist.source).playlists) {
            if (same(c, playlist)) {
                c.title = title;
            }
        }
        if (same(route_.card, playlist)) {
            route_.card.title = title;
        }
        for (Route &r : history_) {
            if (same(r.card, playlist)) {
                r.card.title = title;
            }
        }
    }

    // Library and account.

    Task<void> Shell::startup() {
        auto saved = co_await creds::load(loop_);
        if (saved) {
            credentials_ = creds::from_json(*saved);
            for (const auto &entry : platforms) {
                provider(entry.source).session() = std::move(credentials_[entry.id]);
            }
            can_save_creds_ = true;
        } else {
            notify(lyren::trf("Could not read saved credentials: {}", saved.error()), "triangle-alert", true);
        }
        for (const auto &entry : platforms) {
            spawn(initialize_platform(entry.source));
        }
        save_credentials();
    }

    Task<void> Shell::initialize_platform(Source source) {
        auto r = co_await provider(source).initialize();
        if (!r) {
            notify(std::format("{}: {}", lyren::tr(platform(source).name), r.error()), "user", true);
        }
        if (source == active_) {
            ensure_route_data();
        }
        spawn(warm_pages(source));
        if (provider(source).logged_in()) {
            co_await load_library(source);
        }
    }

    void Shell::save_credentials() {
        if (!can_save_creds_) {
            return;
        }
        // Responses often set several cookies in a row; write once they settle.
        loop_.cancel(creds_timer_);
        creds_timer_ = loop_.after(1s, [this] {
            creds_timer_ = 0;
            spawn([](Shell *self) -> Task<void> {
                for (const auto &entry : platforms) {
                    self->credentials_[entry.id] = self->provider(entry.source).session();
                }
                auto r = co_await creds::save(self->loop_, creds::to_json(self->credentials_));
                if (!r) {
                    self->notify(lyren::trf("Could not save credentials: {}", r.error()), "triangle-alert", true);
                }
            }(this));
        });
    }

    Task<void> Shell::load_library(Source source) {
        Provider &p = provider(source);
        if (!p.logged_in()) {
            co_return;
        }
        Library &lib = library(source);
        uint64_t gen = ++lib.gen;
        auto a = co_await p.account();
        if (gen != lib.gen) {
            co_return;
        }
        if (!a) {
            notify(a.error(), "user", true);
            if (!p.logged_in()) {
                save_credentials();
            }
            co_return;
        }
        bool account_changed = !lib.account || lib.account->id != a->id;
        if (account_changed && lib.account) {
            clear_comment_writes(source);
        }
        lib.account = *a;
        images_.prefetch(a->avatar, 64);
        if (account_changed && thread_.source == source && !thread_.id.empty()) {
            thread_.draft = std::make_shared<CommentDraft>();
            thread_.replies.reset();
            refresh_comments();
        }
        redraw_();
        if (p.features.daily.playlists) {
            spawn(load_daily_lists(source));
        }
        auto pl = co_await p.user_playlists(a->id);
        if (gen != lib.gen) {
            co_return;
        }
        if (pl) {
            lib.liked = std::move(pl->first);
            lib.playlists = std::move(pl->second);
            prefetch_covers(images_, lib.playlists);
        } else {
            notify(pl.error(), "list-music", true);
        }
        auto ids = co_await p.liked_ids(a->id);
        if (gen != lib.gen) {
            co_return;
        }
        if (ids) {
            lib.liked_ids = {ids->begin(), ids->end()};
        } else {
            notify(ids.error(), "heart", true);
        }
        // A page that waited for the login loads now.
        if (source == active_) {
            if (route_.page == Page::liked) {
                route_.list.reset();
            }
            ensure_route_data();
        }
        redraw_();
    }

    Task<void> Shell::load_recommended(Source source) {
        auto &res = library(source).recommended;
        res.loading = true;
        auto r = co_await provider(source).recommended_playlists();
        Results fresh;
        arrived(images_, fresh, std::move(r).transform([](std::vector<Card> v) {
            return Batch{.cards = std::move(v)};
        }),
                now_());
        res = std::move(fresh);
        redraw_();
    }

    Task<void> Shell::load_daily_lists(Source source) {
        auto &lib = library(source);
        uint64_t gen = lib.gen;
        auto r = co_await provider(source).features.daily.playlists();
        if (gen != lib.gen || !r) {
            co_return;
        }
        lib.daily_lists = std::move(*r);
        prefetch_covers(images_, lib.daily_lists);
        lib.daily_since = now_();
        redraw_();
    }

    Task<void> Shell::load_toplists(Source source) {
        auto &res = library(source).toplists;
        res.loading = true;
        auto r = co_await provider(source).toplists();
        Results fresh;
        arrived(images_, fresh, std::move(r).transform([](std::vector<Card> v) {
            return Batch{.cards = std::move(v)};
        }),
                now_());
        res = std::move(fresh);
        redraw_();
    }

    Task<void> Shell::load_fm(Source source) {
        auto &lib = library(source);
        uint64_t gen = lib.gen;
        lib.fm_loading = true;
        lib.fm_error.clear();
        auto r = co_await provider(source).features.radio.tracks();
        if (gen != lib.gen) {
            co_return;
        }
        lib.fm_loading = false;
        if (r) {
            prefetch_covers(images_, *r);
            lib.fm.insert(lib.fm.end(), r->begin(), r->end());
        } else {
            lib.fm_error = r.error();
        }
        redraw_();
    }

    void Shell::refresh_recent() {
        for (auto &lib : libraries_) {
            lib.recent.clear();
        }
        for (const auto &track : recent_) {
            library(track.source).recent.push_back(track);
        }
        for (const auto &lib : libraries_) {
            prefetch_covers(images_, lib.recent);
        }
    }

    Task<void> Shell::load_dislikes(Source source, DislikeKind kind) {
        auto &cached = library(source).dislikes[static_cast<size_t>(kind)];
        if (!cached) {
            cached = std::make_shared<DislikeList>();
        }
        auto list = cached;
        uint64_t gen = ++list->gen;
        list->loading = true;
        list->error.clear();
        redraw_();
        auto r = co_await provider(source).features.exclusions.list(kind);
        if (gen != list->gen || library(source).dislikes[static_cast<size_t>(kind)] != list) {
            co_return;
        }
        list->loading = false;
        list->loaded = true;
        if (r) {
            list->items = std::move(*r);
            list->since = now_();
        } else {
            list->error = r.error();
        }
        redraw_();
    }

    void Shell::set_dislike(Dislike item, bool on) {
        if (!provider(item).logged_in()) {
            return notify(lyren::trf("Sign in to {} to change recommendations", lyren::tr(platform(item.source).name)), "user");
        }
        auto &cached = library(item.source).dislikes[static_cast<size_t>(item.kind)];
        if (!cached) {
            cached = std::make_shared<DislikeList>();
        }
        auto list = cached;
        if (!list->pending.insert(item.id).second) {
            return;
        }
        redraw_();
        spawn([](Shell *self, Dislike item, bool on, std::shared_ptr<DislikeList> list) -> Task<void> {
            auto r = co_await self->provider(item).features.exclusions.set(item, on);
            list->pending.erase(item.id);
            if (self->library(item.source).dislikes[static_cast<size_t>(item.kind)] != list) {
                co_return;
            }
            if (!r) {
                co_return self->notify(lyren::trf("Action failed: {}", r.error()), "triangle-alert", true);
            }
            self->notify(std::string(lyren::tr(on ? "Hidden from recommendations" : "Restored to recommendations")), on ? "thumbs-down" : "check");
            co_await self->load_dislikes(item.source, item.kind);
        }(this, std::move(item), on, std::move(list)));
    }

    void Shell::next_fm() {
        Source source = fm_playing_ ? fm_source_ : active_;
        auto &lib = library(source);
        if (lib.fm_pos + 1 < static_cast<int>(lib.fm.size())) {
            ++lib.fm_pos;
        }
        if (lib.fm_pos + 2 >= static_cast<int>(lib.fm.size()) && !lib.fm_loading) {
            spawn(load_fm(source));
        }
        page_time_ = now_(), page_dir_ = 1;
        if (fm_playing_) {
            play(lib.fm, lib.fm_pos);
        }
    }

    Task<void> Shell::qr_login() {
        uint64_t gen = ++qr_gen_;
        Source source = static_cast<Source>(login_step_);
        qr_active_ = true;
        provider(source).cancel_qr();
        qr_.clear();
        qr_image_ = false;
        images_.erase("qr");
        qr_status_.reset();
        qr_error_.clear();
        auto code = co_await provider(source).qr_start();
        if (gen != qr_gen_) {
            co_return;
        }
        if (!code) {
            qr_error_ = code.error();
            co_return redraw_();
        }
        if (!code->image.empty()) {
            auto img = gfx::decode_square({reinterpret_cast<const uint8_t *>(code->image.data()), code->image.size()}, 512, qr_error_);
            if (!img.w) {
                co_return redraw_();
            }
            images_.put("qr", 512, img);
            qr_image_ = true;
        } else {
            QRcode *q = QRcode_encodeString(code->text.c_str(), 0, QR_ECLEVEL_H, QR_MODE_8, 1);
            qr_size_ = q->width;
            qr_.resize(static_cast<size_t>(q->width) * static_cast<size_t>(q->width));
            for (size_t i = 0; i < qr_.size(); ++i) {
                qr_[i] = q->data[i] & 1;
            }
            QRcode_free(q);
        }
        qr_status_ = QrStatus{};
        redraw_();
        while (true) {
            co_await sleep{loop_, 2s};
            if (gen != qr_gen_) {
                co_return;
            }
            auto s = co_await provider(source).qr_poll();
            if (gen != qr_gen_) {
                co_return;
            }
            if (!s) {
                qr_error_ = s.error();
                co_return redraw_();
            }
            qr_status_ = *s;
            redraw_();
            if (s->state == QrState::expired) {
                co_return;
            }
            if (s->state == QrState::confirmed) {
                break;
            }
        }
        qr_active_ = false;
        can_save_creds_ = true;
        save_credentials();
        co_await load_library(source);
        if (gen != qr_gen_) {
            co_return;
        }
        login_step_ = -1;
        login_time_ = now_();
        if (library(source).account) {
            notify(lyren::trf("Signed in as {}", library(source).account->name), "circle-check");
        }
    }

    void Shell::cancel_login() {
        ++qr_gen_;
        provider(static_cast<Source>(login_step_)).cancel_qr();
        qr_active_ = false;
        images_.erase("qr");
        qr_image_ = false;
    }

    Task<void> Shell::logout(Source source) {
        auto &lib = library(source);
        ++lib.gen;
        ++lib.collection_gen;
        co_await provider(source).logout();
        lib.account.reset();
        clear_comment_writes(source);
        if (thread_.source == source) {
            thread_.draft = std::make_shared<CommentDraft>();
            thread_.replies.reset();
            if (!thread_.id.empty()) {
                refresh_comments();
            }
        }
        lib.playlists.clear();
        lib.liked_ids.clear();
        lib.liked = {};
        lib.daily_lists.clear();
        lib.fm.clear();
        lib.fm_pos = 0;
        lib.fm_loading = false;
        lib.dislikes = {};
        if (fm_source_ == source) {
            fm_playing_ = false;
        }
        if (listen_ && listen_->source == source) {
            listen_.reset();
        }
        for (auto &c : lib.collection) {
            c = {};
        }
        if (source == active_) {
            if (needs_login(route_.page)) {
                route_.list.reset();
            }
            ensure_route_data();
        }
        save_credentials();
        notify(lyren::trf("Signed out of {}", lyren::tr(platform(source).name)), "log-out");
    }

    // Navigation.

    static ui::Id search_scroll_id(ui::Ui &ui, Source source, int tab) {
        return ui::sub_id(ui.id("search-results"), static_cast<uint64_t>(source) * 4 + static_cast<uint64_t>(tab));
    }

    void Shell::search(ui::Ui &ui) {
        if (query_.empty()) {
            return;
        }
        searched_ = query_;
        ++search_gen_;
        results_ = {};
        search_tab_ = 0;
        for (const auto &entry : platforms) {
            for (int tab = 0; tab < 4; ++tab) {
                ui.scroll_to(search_scroll_id(ui, entry.source, tab), 0);
            }
        }
        load_results(0);
        open(ui, Page::search);
    }

    void Shell::load_results(int tab, bool retry) {
        SearchResults &res = results_[static_cast<size_t>(active_)][tab];
        if (res.loading || (!retry && (!res.more() || !res.error.empty()))) {
            return;
        }
        res.loading = true;
        res.error.clear();
        spawn(fetch_results(search_gen_, tab, active_, searched_, res.next_offset));
    }

    Task<void> Shell::fetch_results(uint64_t gen, int tab, Source source, std::string query, int offset) {
        auto r = co_await provider(source).search(query, static_cast<SearchKind>(tab), offset, tab == 0 ? 30 : 40);
        if (gen != search_gen_) {
            co_return;
        }
        SearchResults &res = results_[static_cast<size_t>(source)][tab];
        if (r) {
            res.next_offset = r->tracks.empty() && r->cards.empty() ? -1 : r->next_offset;
        }
        arrived(images_, res, std::move(r), now_());
        if (!res.more()) {
            res.total = static_cast<int>(res.size());
        }
        redraw_();
    }

    bool Shell::supports(Page page, Source source) const {
        const auto &features = provider(source).features;
        switch (page) {
            case Page::daily:
                return bool(features.daily.songs);
            case Page::square:
                return bool(features.square.playlists);
            case Page::fm:
                return bool(features.radio.tracks);
            case Page::radar:
                return bool(features.radar);
            case Page::releases:
                return bool(features.releases.songs) && bool(features.releases.albums);
            case Page::cloud:
                return bool(features.cloud);
            case Page::similar:
                return bool(features.similar);
            case Page::dislikes:
                return bool(features.exclusions.list) && bool(features.exclusions.set);
            default:
                return true;
        }
    }

    void Shell::switch_platform(ui::Ui &ui, Source source) {
        if (source == active_) {
            return;
        }
        // Search keeps each platform's cached results and scroll positions.
        if (route_.page == Page::search || route_.page == Page::settings) {
            tab_dir_ = source > active_ ? 1 : -1;
            active_ = route_.card.source = source;
            selected_ = -1;
            tab_time_ = ui.now;
            ensure_route_data();
            changed();
            ui.redraw();
            return;
        }
        Page page = route_.page;
        if (page == Page::playlist || page == Page::album || page == Page::artist || page == Page::similar || !supports(page, source)) {
            page = Page::recommend;
        }
        open(ui, page, {.source = source});
    }

    void Shell::open(ui::Ui &ui, Page page) {
        open(ui, page, {.source = active_});
    }

    void Shell::open(ui::Ui &ui, Page page, const Card &card) {
        if (route_.page == page && same(route_.card, card) && page != Page::search) {
            return;
        }
        history_.push_back(route_);
        if (history_.size() > 50) {
            history_.erase(history_.begin());
        }
        // Older pages reload if they are visited again.
        if (history_.size() > 8) {
            Route &old = history_[history_.size() - 9];
            old.list.reset();
            old.artist.reset();
        }
        Card target = card;
        if (page == Page::playlist) {
            const auto &playlists = library(card.source).playlists;
            if (auto it = std::ranges::find_if(playlists,
                                               [&](const Card &p) {
                                                   return same(p, card) && p.owned;
                                               });
                it != playlists.end()) {
                target.owned = true;
            }
        }
        enter(ui, {page, std::move(target)}, 1);
    }

    void Shell::open(ui::Ui &ui, const Card &card) {
        open(ui, card.kind == CardKind::album ? Page::album : card.kind == CardKind::artist ? Page::artist : Page::playlist, card);
    }

    // Back closes the now-playing view first.
    void Shell::back(ui::Ui &ui) {
        if (np_open_) {
            show_stage(false);
            return;
        }
        if (history_.empty()) {
            return;
        }
        Route r = std::move(history_.back());
        history_.pop_back();
        enter(ui, std::move(r), -1);
    }

    void Shell::enter(ui::Ui &ui, Route route, int direction) {
        route_ = std::move(route);
        if (active_ != route_.card.source) {
            active_ = route_.card.source;
            changed();
            for (const char *id : {"recommend", "toplists", "square", "sidebar", "collection-albums", "collection-artists", "release-songs", "release-albums"}) {
                ui.scroll_to(ui.id(id), 0);
            }
        }
        page_time_ = ui.now;
        page_dir_ = direction;
        selected_ = -1;
        for (const char *id : {"tracks", "artist-songs", "artist-albums", "artist-intro"}) {
            ui.scroll_to(ui.id(id), 0);
        }
        switch (route_.page) {
            case Page::daily:
                route_.card = {.source = active_, .title = "Daily mix", .subtitle = lyren::trf("{} · Made for your taste", date_label(today_))};
                break;
            case Page::radar:
                route_.card = {.source = active_, .title = "Radar", .subtitle = "Favorites and new discoveries"};
                break;
            case Page::cloud:
                route_.card = {.source = active_, .title = "My cloud", .subtitle = "Songs uploaded to your cloud"};
                break;
            case Page::liked:
                route_.card = {.source = active_, .title = "Liked songs"};
                break;
            case Page::recent:
                route_.card = {.source = active_, .title = "Recently played"};
                break;
            default:
                break;
        }
        if (needs_login(route_.page) && !signed_in()) {
            route_.list.reset();
        }
        ensure_route_data();
    }

    bool Shell::needs_login(Page p) {
        return p == Page::daily || p == Page::fm || p == Page::liked || p == Page::cloud || p == Page::collection || p == Page::dislikes;
    }

    Task<void> Shell::warm_pages(Source source) {
        // Current-page work starts first. Warm only small, reusable listings, not every playlist's tracks.
        co_await lyren::sleep{loop_, source == active_ ? 200ms : 800ms};
        auto &lib = library(source);
        auto &features = provider(source).features;
        if (!lib.recommended.loaded && !lib.recommended.loading) {
            co_await load_recommended(source);
        }
        if (!lib.toplists.loaded && !lib.toplists.loading) {
            co_await load_toplists(source);
        }
        if (features.square.playlists && !lib.square.loaded && !lib.square.loading) {
            lib.square.loading = true;
            co_await fetch_square(source, lib.square_gen, lib.category.id, 0);
        }
        if (features.square.categories && lib.categories.groups.empty() && !lib.categories_loading) {
            co_await load_categories(source);
        }
        if (features.releases.songs && features.releases.albums) {
            for (int tab = 0; tab < 2; ++tab) {
                auto &res = lib.releases[tab];
                if (!res.loaded && !res.loading) {
                    res.loading = true;
                    co_await fetch_releases(source, tab, lib.release_gen[tab], features.releases.categories[tab][lib.release_category[tab]].id, 0);
                }
            }
        }
        if (provider(source).logged_in() && !lib.collection[0].loaded && !lib.collection[0].loading) {
            co_await load_collection(source);
        }
    }

    void Shell::ensure_route_data() {
        Page page = route_.page;
        Library &lib = library();
        if (page == Page::search && !results_[static_cast<size_t>(active_)][search_tab_].loaded) {
            load_results(search_tab_);
        }
        bool listed = page == Page::daily || page == Page::radar || page == Page::cloud || page == Page::liked || page == Page::playlist || page == Page::album ||
                      page == Page::similar || (page == Page::recent && lib.recent_tab == 1 && signed_in());
        if (listed && !route_.list && !(needs_login(page) && !signed_in())) {
            if (page == Page::liked && lib.liked.id.empty()) {
                return;
            }
            route_.list = std::make_shared<List>();
            spawn(load_list(route_.list, page, route_.card));
        }
        if (page == Page::artist && !route_.artist) {
            route_.artist = std::make_shared<ArtistPage>();
            spawn(load_artist(route_.artist, route_.card));
        }
        if (page == Page::recommend && !lib.recommended.loaded && !lib.recommended.loading) {
            spawn(load_recommended(active_));
        }
        if (page == Page::toplist && !lib.toplists.loaded && !lib.toplists.loading) {
            spawn(load_toplists(active_));
        }
        if (page == Page::square) {
            if (provider().features.square.categories && lib.categories.groups.empty() && !lib.categories_loading) {
                spawn(load_categories(active_));
            }
            if (!lib.square.loaded) {
                load_square();
            }
        }
        if (page == Page::fm && lib.fm.empty() && !lib.fm_loading && signed_in()) {
            spawn(load_fm(active_));
        }
        if (page == Page::releases && !lib.releases[lib.release_tab].loaded) {
            load_releases();
        }
        if (page == Page::dislikes && signed_in()) {
            const auto &list = lib.dislikes[lib.dislike_tab];
            if (!list || (!list->loaded && !list->loading)) {
                spawn(load_dislikes(active_, static_cast<DislikeKind>(lib.dislike_tab)));
            }
        }
        // Collections can change elsewhere, so they reload whenever the page is entered.
        if (page == Page::collection && signed_in() && !lib.collection[0].loading) {
            spawn(load_collection(active_));
        }
    }

    Task<void> Shell::load_list(std::shared_ptr<List> list, Page page, Card card) {
        list->loading = true;
        list->error.clear();
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
            r = co_await provider(card).tracks(library(card.source).liked);
        } else if (page == Page::daily) {
            r = co_await provider(card).features.daily.songs();
        } else if (page == Page::radar) {
            r = co_await provider(card).features.radar();
        } else if (page == Page::cloud) {
            r = co_await provider(card).features.cloud();
        } else if (page == Page::recent) {
            r = co_await provider(card).features.recent_plays();
        } else if (page == Page::similar) {
            r = co_await provider(card).features.similar(card.id);
        } else {
            r = co_await provider(card).tracks(card);
        }
        list->loading = false;
        if (r) {
            list->tracks = std::move(*r);
            prefetch_covers(images_, list->tracks);
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
        if (r) {
            page->info = std::move(*r);
        } else {
            page->error = r.error();
        }
        redraw_();
    }

    void Shell::load_artist_tab(int tab) {
        ArtistPage &a = *route_.artist;
        Results &res = tab == 0 ? a.songs : tab == 1 ? a.albums : a.intro;
        if (res.loading || !res.more() || (res.loaded && !res.error.empty())) {
            return;
        }
        res.loading = true;
        res.error.clear();
        spawn(fetch_artist(route_.artist, route_.card, tab, static_cast<int>(res.size())));
    }

    Task<void> Shell::fetch_artist(std::shared_ptr<ArtistPage> page, Card card, int tab, int offset) {
        if (tab == 2) {
            auto r = co_await provider(card).artist_intro(card.id);
            page->intro.loading = false;
            page->intro.loaded = true;
            page->intro.since = now_();
            if (r) {
                page->sections = std::move(*r);
            } else {
                page->intro.error = r.error();
            }
            co_return redraw_();
        }
        Result<Batch> r;
        if (tab == 0) {
            r = co_await provider(card).artist_songs(card.id, offset);
        } else {
            r = co_await provider(card).artist_albums(card.id, offset);
        }
        arrived(images_, tab == 0 ? page->songs : page->albums, std::move(r), now_());
        redraw_();
    }

    Task<void> Shell::load_categories(Source source) {
        auto &lib = library(source);
        lib.categories_loading = true;
        auto r = co_await provider(source).features.square.categories();
        lib.categories_loading = false;
        if (r) {
            lib.categories = std::move(*r);
        }
        redraw_();
    }

    void Shell::set_category(ui::Ui &ui, Category category) {
        auto &lib = library();
        if (category.id == lib.category.id) {
            return;
        }
        lib.category = std::move(category);
        ++lib.square_gen;
        lib.square = {};
        lib.square_time = ui.now;
        ui.scroll_to(ui.id("square"), 0);
        load_square();
    }

    void Shell::load_square() {
        auto &lib = library();
        if (lib.square.loading || !lib.square.more() || (lib.square.loaded && !lib.square.error.empty())) {
            return;
        }
        lib.square.loading = true;
        lib.square.error.clear();
        spawn(fetch_square(active_, lib.square_gen, lib.category.id, static_cast<int>(lib.square.cards.size())));
    }

    Task<void> Shell::fetch_square(Source source, uint64_t gen, std::string category, int offset) {
        auto r = co_await provider(source).features.square.playlists(std::move(category), offset);
        auto &lib = library(source);
        if (gen != lib.square_gen) {
            co_return;
        }
        arrived(images_, lib.square, std::move(r), now_());
        redraw_();
    }

    void Shell::load_releases() {
        auto &lib = library();
        int tab = lib.release_tab;
        auto &res = lib.releases[tab];
        if (res.loading || !res.more() || (res.loaded && !res.error.empty())) {
            return;
        }
        res.loading = true;
        res.error.clear();
        const auto &category = provider().features.releases.categories[tab][lib.release_category[tab]];
        spawn(fetch_releases(active_, tab, lib.release_gen[tab], category.id, static_cast<int>(res.size())));
    }

    void Shell::set_release_category(ui::Ui &ui, int category) {
        auto &lib = library();
        int tab = lib.release_tab;
        lib.release_category[tab] = category;
        ++lib.release_gen[tab];
        lib.releases[tab] = {};
        selected_ = -1;
        ui.scroll_to(ui.id(tab ? "release-albums" : "release-songs"), 0);
        load_releases();
    }

    Task<void> Shell::fetch_releases(Source source, int tab, uint64_t gen, std::string category, int offset) {
        Result<Batch> r;
        if (tab == 0) {
            auto songs = co_await provider(source).features.releases.songs(std::move(category));
            r = std::move(songs).transform([](std::vector<Track> tracks) {
                int total = static_cast<int>(tracks.size());
                return Batch{.tracks = std::move(tracks), .total = total};
            });
        } else {
            r = co_await provider(source).features.releases.albums(std::move(category), offset);
        }
        auto &lib = library(source);
        if (gen != lib.release_gen[tab]) {
            co_return;
        }
        arrived(images_, lib.releases[tab], std::move(r), now_());
        redraw_();
    }

    Task<void> Shell::load_collection(Source source) {
        auto &lib = library(source);
        uint64_t gen = ++lib.collection_gen;
        for (auto &c : lib.collection) {
            c.loading = true;
        }
        auto &p = provider(source);
        auto [albums, artists] = co_await both(p.collected_albums(), p.followed_artists());
        if (gen != lib.collection_gen) {
            co_return;
        }
        auto batch = [](std::vector<Card> v) {
            return Batch{.cards = std::move(v)};
        };
        std::array<Results, 2> fresh;
        arrived(images_, fresh[0], std::move(albums).transform(batch), now_());
        arrived(images_, fresh[1], std::move(artists).transform(batch), now_());
        // A reload does not replay the cards' entrance.
        for (int i = 0; i < 2; ++i) {
            if (lib.collection[i].loaded) {
                fresh[i].since = lib.collection[i].since;
            }
            lib.collection[i] = std::move(fresh[i]);
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
        if (config_.playback.normalize) {
            g = std::min(std::pow(10.f, track_gain_ / 20), track_peak_ > 0 ? 1 / track_peak_ : 1.f);
        }
        log::debug("loudness: {:+.2f} dB, peak {:.3f}: x{:.3f}", track_gain_, track_peak_, g);
        player_.set_gain(g);
    }

    void Shell::play(const std::vector<Track> &list, int i) {
        if (i < 0 || i >= static_cast<int>(list.size())) {
            return;
        }
        Source source = list[static_cast<size_t>(i)].source;
        fm_playing_ = &list == &library(source).fm;
        if (fm_playing_) {
            fm_source_ = source;
        }
        set_current(i, &list != &queue_);
        if (&list != &queue_) {
            queue_ = list;
            shuffle_history_.clear();
        }
        shuffle_next_ = -1;
        start();
    }

    void Shell::set_current(int i, bool replacing_queue) {
        if (i == current_ && !replacing_queue) {
            return;
        }
        prev_ = current() ? *current() : Track{};
        cover_time_ = now_();
        current_ = i;
    }

    void Shell::toggle_play() {
        if (!current()) {
            return;
        }
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
        if (!current()) {
            return;
        }
        want_play_ = fetching_ = false;
        pending_song_seek_.reset();
        resume_at_ = last_pos_ = 0;
        trial_ = false, clip_start_ = 0, highlight_from_ = highlight_to_ = 0;
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
        if (!listen_ || !(ended || st == State::playing || st == State::paused)) {
            return;
        }
        int seconds = static_cast<int>(ended ? player_.duration() : player_.position());
        if (ended || seconds >= 30) {
            spawn(provider(*listen_).features.reporting.play(listen_->id, listen_->album_id, seconds));
        }
        listen_.reset();
    }

    void Shell::seek(double at) {
        last_pos_ = at;
        auto state = player_.state();
        if (state == State::idle || state == State::failed) {
            resume_at_ = at;
        } else {
            player_.seek(at);
        }
        schedule_prefetch();
        if (mpris_) {
            mpris_->seeked(timeline().to_song(at));
        }
        changed();
        redraw_();
    }

    void Shell::seek_song(double at) {
        if (fetching_) {
            pending_song_seek_ = std::clamp(at, 0.0, timeline().duration());
            last_pos_ = timeline().to_file(*pending_song_seek_);
            changed();
            redraw_();
        } else {
            seek(timeline().to_file(at));
        }
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
        if (st == State::playing || st == State::paused) {
            return player_.position();
        }
        return st == State::idle && !fetching_ ? resume_at_ : last_pos_;
    }

    Timeline Shell::timeline() const {
        return {current() ? static_cast<double>(current()->duration) : 0, player_.duration(), clip_start_};
    }

    void Shell::publish() {
        if (!mpris_) {
            return;
        }
        MprisState s{.volume = muted_ ? 0 : volume_, .loop_one = mode_ == Mode::one, .shuffle = mode_ == Mode::shuffle};
        if (const Track *t = current()) {
            using S = MprisState::Status;
            auto st = player_.state();
            s.status = playing() ? S::playing : st == State::idle && !fetching_ && resume_at_ == 0 ? S::stopped : S::paused;
            s.track = std::format("/lyren/track/{}_{}", platform(t->source).id, t->id);
            s.title = t->title, s.artist = t->artist, s.album = t->album;
            s.art = cover_url(t->cover, 512);
            s.length = timeline().duration();
            s.can_play = true;
            s.can_next = queue_.size() > 1 || fm_playing_;
            s.can_prev = queue_.size() > 1;
            s.can_seek = (st == State::playing || st == State::paused) && s.length > 0;
        }
        mpris_->update(s);
    }

    int Shell::next_index() {
        int n = static_cast<int>(queue_.size());
        if (!n) {
            return -1;
        }
        if (mode_ != Mode::shuffle) {
            return (current_ + 1) % n;
        }
        if (shuffle_next_ < 0 || shuffle_next_ >= n) {
            shuffle_next_ = n == 1 ? 0 : static_cast<int>(next(rng_) % static_cast<uint64_t>(n - 1));
            if (shuffle_next_ >= current_ && n > 1) {
                ++shuffle_next_;
            }
        }
        return shuffle_next_;
    }

    void Shell::skip(int delta, bool automatic) {
        if (fm_playing_ && delta > 0) {
            return next_fm();
        }
        if (queue_.empty()) {
            return;
        }
        int n = static_cast<int>(queue_.size()), i;
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
        if (!current()) {
            return;
        }
        uint64_t gen = ++play_gen_;
        retried_ = counted_ = false;
        want_play_ = play;
        fetching_ = true;
        // The old stream's window no longer applies until the new one arrives.
        trial_ = false, clip_start_ = 0, highlight_from_ = highlight_to_ = 0;
        last_pos_ = at;
        pending_song_seek_.reset();
        resume_at_ = 0;
        if (prefetch_.key != key(*current())) {
            prefetch_ = {};
        }
        stop_player();
        if (lyrics_id_ != key(*current())) {
            lyrics_id_ = key(*current());
            lyrics_.clear();
            lyric_layout_.clear();
            lyrics_loading_ = true;
        }
        spawn(load_track(gen, *current()));
        changed();
    }

    Task<void> Shell::load_track(uint64_t gen, Track t) {
        std::shared_ptr<audio::Spool> spool;
        Stream s;
        if (prefetch_.spool && prefetch_.key == key(t)) {
            spool = std::move(prefetch_.spool);
            s = std::move(prefetch_.stream);
            prefetch_ = {};
        } else {
            auto r = co_await provider(t).stream(t, config_.playback.quality);
            if (gen != play_gen_) {
                co_return;
            }
            if (!r || (r->trial && skip_trials())) {
                fetching_ = false;
                // A run of unplayable tracks is reported once, when one finally plays.
                if (!failures_) {
                    notify(r ? lyren::trf("Skipped {}: preview only", t.title) : t.title + ": " + r.error(), "circle-alert", !r);
                }
                if (++failures_ < queue_.size()) {
                    skip(1, true);
                } else {
                    failures_ = 0;
                    want_play_ = false;
                }
                co_return redraw_();
            }
            s = std::move(*r);
            spool = audio::Spool::url(http_, s.url);
        }
        fetching_ = false;
        clip_start_ = s.trial_start;
        trial_ = s.trial;
        highlight_from_ = s.highlight_from, highlight_to_ = s.highlight_to;
        track_gain_ = s.gain, track_peak_ = s.peak;
        const double at = pending_song_seek_ ? timeline().to_file(*pending_song_seek_) : last_pos_;
        pending_song_seek_.reset();
        last_pos_ = at;
        apply_gain();
        player_.open(std::move(spool), want_play_);
        if (at > 0) {
            seek(at);
        }
        player_.set_title(t.title + " - " + t.artist);
        if (lyrics_.empty() && lyrics_id_ == key(t)) {
            spawn(load_lyrics(t));
        }
        redraw_();
    }

    Task<void> Shell::prefetch(Track t) {
        uint64_t gen = play_gen_;
        auto p = co_await provider(t).stream(t, config_.playback.quality);
        if (gen != play_gen_ || !p || (p->trial && skip_trials())) {
            co_return;
        }
        log::debug("prefetch: {}", t.title);
        prefetch_.key = key(t);
        prefetch_.spool = audio::Spool::url(http_, p->url);
        prefetch_.stream = std::move(*p);
    }

    // Shortly before the end, fetch the next track's URL and first bytes. On a timer rather than in draw(), so it also
    // happens while the window is hidden.
    void Shell::schedule_prefetch() {
        loop_.cancel(prefetch_timer_);
        prefetch_timer_ = 0;
        double dur = player_.duration();
        if (player_.state() != State::playing || dur <= 0 || prefetch_.gen == play_gen_) {
            return;
        }
        auto wait = std::chrono::duration<double>(std::max(0.0, dur - 15 - player_.position()));
        prefetch_timer_ = loop_.after(std::chrono::duration_cast<Clock::duration>(wait), [this] {
            prefetch_timer_ = 0;
            if (mode_ == Mode::one || fm_playing_) {
                return;
            }
            prefetch_.gen = play_gen_;
            if (int i = next_index(); i >= 0 && i != current_) {
                spawn(prefetch(queue_[static_cast<size_t>(i)]));
            }
        });
    }

    Task<void> Shell::load_lyrics(Track t) {
        std::string id = key(t);
        lyrics_id_ = id;
        lyrics_loading_ = true;
        auto r = co_await provider(t).lyrics(t);
        if (lyrics_id_ != id) {
            co_return;
        }
        lyrics_loading_ = false;
        if (r) {
            lyrics_ = std::move(*r);
        }
        lyric_layout_.clear();
        lyric_y_.clear();
        lyrics_since_ = now_();
        redraw_();
    }

    void Shell::on_player() {
        auto s = player_.state();
        if (s != last_state_) {
            if (s == State::playing && !counted_ && current()) {
                counted_ = true;
                if (failures_ > 1) {
                    notify(lyren::trf("Skipped {} unavailable songs", failures_), "skip-forward");
                }
                failures_ = 0;
                std::erase_if(recent_, [&](const Track &x) {
                    return same(x, *current());
                });
                recent_.insert(recent_.begin(), *current());
                if (recent_.size() > 200) {
                    recent_.pop_back();
                }
                refresh_recent();
                changed();
                if (provider(*current()).logged_in() && !trial_ && provider(*current()).features.reporting.start) {
                    listen_ = *current();
                    spawn(provider(*listen_).features.reporting.start(listen_->id, listen_->album_id));
                }
            }
            if (s == State::paused) {
                changed();
            }
            if (s == State::ended) {
                std::optional<Track> listened = listen_;
                report_listen(true);
                if (mode_ == Mode::one && !fm_playing_) {
                    listen_ = listened; // each time round is another listen
                    seek(0);
                    player_.play();
                } else {
                    skip(1, true);
                }
            }
            if (s == State::failed) {
                const audio::Spool *sp = player_.spool();
                // Stream URLs expire; fetch a new one and carry on from here.
                if (sp && sp->status() == 403 && !retried_) {
                    start(last_pos_, want_play_);
                    retried_ = true;
                } else {
                    notify(lyren::trf("Playback failed: {}", player_.error()), "circle-alert", true);
                    want_play_ = false;
                }
            }
        }
        last_state_ = s;
        if (silent_ && player_.volume() > 0) {
            apply_volume();
        } else if (!silent_ && !muted_ && std::abs(player_.volume() - volume_) > 1e-3f) {
            volume_ = player_.volume();
            changed();
        }
        schedule_prefetch();
        publish();
        redraw_();
    }

    // Drawing helpers.

    void Shell::cover(ui::Ui &ui, Rect r, const std::string &url, float radius, std::string_view placeholder) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        int px = Images::tier(r.w * p.scale());
        const auto &e = images_.get(url, static_cast<uint32_t>(px));
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
    std::vector<std::string_view> Shell::wrap(ui::Painter &p, std::string_view s, float size, float width) {
        const text::Shaped &sh = p.shape(s, {.size = size});
        if (sh.width * size <= width || sh.glyphs.size() < 2) {
            return {s};
        }
        auto at = [&](uint32_t byte) {
            size_t i = byte;
            return utf8::decode(s, i);
        };
        auto wide = [&](uint32_t byte) {
            return at(byte) >= 0x2e80;
        };
        // Closing punctuation stays at the end of the line before.
        auto closing = [&](uint32_t byte) {
            return std::u32string_view(U"，。、；：？！）」』》〉】”’…").contains(at(byte));
        };
        std::vector<std::string_view> out;
        auto emit = [&](size_t from, size_t to) {
            std::string_view v = s.substr(from, to - from);
            while (v.ends_with(' ')) {
                v.remove_suffix(1);
            }
            while (v.starts_with(' ')) {
                v.remove_prefix(1);
            }
            if (!v.empty()) {
                out.push_back(v);
            }
        };
        const auto &g = sh.glyphs;
        size_t start = 0, candidate = 0;
        for (size_t i = 1; i < g.size(); ++i) {
            uint32_t c = g[i].cluster, pc = g[i - 1].cluster;
            if (c != pc && c > 0 && (s[c - 1] == ' ' || wide(c) || wide(pc)) && !closing(c)) {
                candidate = i;
            }
            float end = (i + 1 < g.size() ? g[i + 1].x : sh.width) - g[start].x;
            if (end * size > width && candidate > start) {
                emit(g[start].cluster, g[candidate].cluster);
                start = candidate;
            }
        }
        emit(g[start].cluster, s.size());
        return out;
    }

    std::vector<std::string_view> Shell::wrap_lines(ui::Painter &p, std::string_view s, float size, float width) {
        std::vector<std::string_view> out;
        for (auto part : std::views::split(s, '\n')) {
            auto lines = wrap(p, std::string_view(part.begin(), part.end()), size, width);
            out.insert(out.end(), lines.begin(), lines.end());
        }
        return out;
    }

    // Headed paragraphs in their own scrolling view: an artist's introduction, an album's notes. Laid out every frame;
    // there are only a few, and the shaping is cached.
    static void prose(ui::Ui &ui, Rect r, ui::Id id, const std::vector<std::pair<std::string, std::string>> &sections) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        TextStyle body{.size = t.fs(14), .color = t.text.mix(t.text_dim, 0.35f)};
        const float lh = std::round(body.size * 1.75f), w = r.w - 16, head_h = 36, gap = 24;
        std::vector<std::vector<std::string_view>> wrapped;
        float content = 0;
        for (const auto &[head, text] : sections) {
            wrapped.push_back(Shell::wrap_lines(p, text, body.size, w));
            content += (head.empty() ? 0 : head_h) + static_cast<float>(wrapped.back().size()) * lh + gap;
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
                if (y + lh > clip.y && y < clip.bottom()) {
                    p.text(r.x, y + lh * 0.7f, line, body, w);
                }
                y += lh;
            }
            y += gap;
        }
        ui.end_scroll();
    }

    // Where the wipe of a word-timed line stands at t within s, one of its wrapped lines: px from the start of s,
    // a soft edge's width before it while s is unsung and past its end once sung.
    float Shell::sung_x(ui::Painter &p, const LyricLine &l, std::string_view s, float size, float soft, double t) {
        int k = word_at(l, t);
        if (k < 0) {
            return -soft;
        }
        const LyricWord &w = l.words[static_cast<size_t>(k)];
        auto from = static_cast<uint32_t>(s.data() - l.text.data()), to = from + static_cast<uint32_t>(s.size());
        const text::Shaped &sh = p.shape(s, {.size = size});
        auto x = [&](uint32_t b) {
            if (b <= from) {
                return -soft;
            }
            if (b >= to) {
                return sh.width * size + soft;
            }
            auto g = std::ranges::find_if(sh.glyphs, [&](const text::Glyph &g) {
                return g.cluster >= b - from;
            });
            return (g == sh.glyphs.end() ? sh.width : g->x) * size;
        };
        float sung = w.duration() > 0 ? static_cast<float>(std::clamp((t - w.time()) / w.duration(), 0.0, 1.0)) : 1;
        return x(w.from) + (x(w.to) - x(w.from)) * sung;
    }

    // Layout.

    void Shell::top_bar(ui::Ui &ui, Rect bar, float center_x) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // The stage keeps only the window buttons in its title strip.
        float covered = stage(ui);
        if (covered >= 1) {
            return;
        }
        Fade under_stage(p, 1 - covered);
        ui.disabled = covered > 0.5f;
        Rect r = bar;
        Rect nav = ui::cut_left(r, 40);
        {
            Fade fade(ui.painter(), ui.anim(ui.id("back-a"), history_.empty() && !np_open_ ? 0.3f : 1.f, 200));
            if (ui.icon_button(ui.id("back"), ui::center(nav, 32, 32), "chevron-left", 20)) {
                back(ui);
            }
        }
        // A fixed-width platform capsule keeps the search field still when the name changes.
        float platform_w = 0;
        for (const auto &entry : platforms) {
            platform_w = std::max(platform_w, p.measure(lyren::tr(entry.short_name), {.size = t.fs(13)}));
        }
        Rect selector = ui::center(ui::cut_left(r, platform_w + 56), platform_w + 56, 32);
        auto selected = ui.interact(ui.id("platform"), selector);
        float hot = ui.anim(ui.id("platform"), selected.hovered ? 1 : 0, 150);
        float down = ui.anim(ui.id("platform-down"), selected.held ? 1 : 0, 90);
        Rect pill = selector.inset(down);
        p.rect(pill, t.text.with_alpha(0.045f + 0.055f * hot), pill.h / 2, t.border.with_alpha(0.65f), 1);
        p.icon("music", {pill.x + 11, pill.y + 9, 14, 14}, t.accent);
        ui.label({pill.x + 32, pill.y, platform_w, pill.h}, lyren::tr(platform(active_).short_name), {.size = t.fs(13), .color = t.text});
        p.icon("chevron-down", {pill.right() - 19, pill.y + 11, 10, 10}, t.text_dim);
        if (selected.clicked) {
            std::vector<ui::MenuItem> items;
            for (const auto &entry : platforms) {
                items.push_back({entry.source == active_ ? "check" : "music", std::string(lyren::tr(entry.name)), [this, &ui, source = entry.source] {
                                     show_stage(false);
                                     switch_platform(ui, source);
                                 }});
            }
            ui.open_menu(selector.x, selector.bottom() + 7, std::move(items));
        }
        ui::cut_left(r, 8);
        Rect settings = ui::cut_right(r, 40);
        if (ui.icon_button(ui.id("settings"), ui::center(settings, 32, 32), "settings", 18, route_.page == Page::settings)) {
            if (route_.page == Page::settings) {
                back(ui);
            } else {
                open(ui, Page::settings);
            }
        }
        Rect account = ui::cut_right(r, 40);
        const auto &profile = library().account;
        if (profile) {
            // Signed in: the avatar replaces the icon.
            Rect a = ui::center(account, 28, 28);
            auto it = ui.interact(ui.id("account"), a.inset(-2));
            float hot = ui.anim(ui.id("account"), it.hovered ? 1 : 0, 150);
            if (hot > 0) {
                ui.painter().rect(a.inset(-3), ui.theme().text.with_alpha(0.12f * hot), 17);
            }
            cover(ui, a, profile->avatar, 14, "user");
            if (it.clicked) {
                open_account(ui);
            }
        } else if (ui.icon_button(ui.id("account"), ui::center(account, 32, 32), "user", 18)) {
            open_account(ui);
        }
        float w = std::min(440.f, r.w - 32);
        float x = std::clamp(center_x - w / 2, r.x + 16, r.right() - 16 - w);
        if (ui.text_box(ui.id("search"), {x, bar.y + (bar.h - 34) / 2, w, 34}, query_, lyren::tr("Search music"), "search")) {
            show_stage(false);
            search(ui);
        }
        ui.disabled = false;
    }

    void Shell::open_account(ui::Ui &ui) {
        login_step_ = -1;
        ui.open_modal(420, 186 + 72 * static_cast<float>(platforms.size()), [this](ui::Ui &u, Rect body) {
            account_modal(u, body);
        });
    }

    float Shell::stage(ui::Ui &ui) {
        return ui.spring(ui.id("np-open"), np_open_ ? 1 : 0, 400);
    }

    void Shell::show_stage(bool open) {
        if (!open && panel_ == Panel::comments && thread_.follows) {
            show_panel(Panel::none);
        }
        np_open_ = open;
    }

    void Shell::draw(ui::Ui &ui, Rect area, const ui::Theme &stage_theme) {
        stage_ = &stage_theme;
        auto &p = ui.painter();
        const auto &t = ui.theme();
        for (auto &m : toasts_) {
            ui.toast(std::move(m.text), std::move(m.icon), m.error);
        }
        toasts_.clear();
        // Closing the account dialog stops the QR polling.
        if (qr_active_ && !ui.modal_open()) {
            cancel_login();
        }

        auto st = player_.state();
        if (st == State::playing || st == State::paused) {
            last_pos_ = player_.position();
        }
        double dur = player_.duration() > 0 ? player_.duration() : current() ? current()->duration : 0;

        Rect player = ui::cut_bottom(area, player_h);
        // A small dead band keeps resizing near the breakpoint from repeatedly folding the sidebar.
        if (area.w < 888) {
            compact_ = true;
        } else if (area.w > 912) {
            compact_ = false;
        }
        float side_w = ui.spring(ui.id("side-w"), compact_ ? 72 : 220, 320);
        Rect side = ui::cut_left(area, side_w);
        ui::cut_right(area, 8);
        // On the stage the comments follow the music, loaded ahead so they open at once, unless the panel is showing
        // others.
        if (const Track *tr = current(); tr && np_open_ && !(panel_ == Panel::comments && !thread_.follows) &&
                                         !(thread_.follows && thread_.subject == Subject::song && thread_.id == tr->id && thread_.source == tr->source)) {
            set_thread(ui, tr->source, Subject::song, tr->id, tr->title + " · " + tr->artist, tr->cover, true);
        }
        // The panel beside the page slides in from the right.
        float pw = ui.spring(ui.id("panel-w"), panel_width(panel_), 340);
        Rect slot = pw > 0.5f ? ui::cut_right(area, pw) : Rect{};

        // The now-playing stage comes up over the whole window; once it covers it the sidebar and page are not drawn.
        float np = stage(ui);
        if (!(np_open_ && np >= 1)) {
            // Content panel with a wash of the page's colour; the page slides in from the side it was reached from.
            if (!t.dark) {
                p.shadow({area.x, area.y + 1, area.w, area.h}, t.radius_l(), 10, t.shadow.with_alpha(0.45f));
            }
            p.rect(area, t.surface, t.radius_l());
            float enter = ui.progress(page_time_, 420);
            float hue = -1;
            if (route_.page == Page::playlist || route_.page == Page::album || route_.page == Page::similar) {
                hue = cover_hue(route_.card.cover, 148 * p.scale());
            } else if (route_.page == Page::artist && route_.artist && route_.artist->info) {
                hue = cover_hue(route_.artist->info->avatar, 148 * p.scale());
            }
            ui::Color tint = hue >= 0 ? hsl(hue, 0.55f, 0.5f) : t.accent;
            if (route_.page != Page::search && route_.page != Page::settings) {
                p.gradient({area.x, area.y, area.w, 320}, tint.with_alpha((t.dark ? 0.16f : 0.09f) * enter), tint.with_alpha(0), t.radius_l());
            }
            ui.push_clip(area);
            {
                Fade fade(p, enter);
                Rect r = area;
                r.x += (1 - enter) * 28 * static_cast<float>(page_dir_);
                switch (route_.page) {
                    case Page::recommend:
                        recommend_page(ui, r);
                        break;
                    case Page::toplist:
                        toplist_page(ui, r);
                        break;
                    case Page::square:
                        square_page(ui, r);
                        break;
                    case Page::fm:
                        fm_page(ui, r);
                        break;
                    case Page::releases:
                        releases_page(ui, r);
                        break;
                    case Page::collection:
                        collection_page(ui, r);
                        break;
                    case Page::dislikes:
                        dislikes_page(ui, r);
                        break;
                    case Page::artist:
                        artist_page(ui, r);
                        break;
                    case Page::search:
                        search_page(ui, r);
                        break;
                    case Page::settings:
                        settings_page(ui, r);
                        break;
                    case Page::recent:
                        if (library().recent_tab == 0 || !signed_in()) {
                            route_.card.subtitle = lyren::trf("{} songs · On this device", library().recent.size());
                            list_page(ui, r, library().recent, false, "", std::max(page_time_, tab_time_));
                            break;
                        }
                        [[fallthrough]];
                    default: {
                        // No list yet: the library is still loading, or the page waits for a login.
                        static List pending{.loading = true};
                        List &list = route_.list ? *route_.list : pending;
                        if (route_.page == Page::liked && route_.list) {
                            route_.card.subtitle = lyren::trf("{} songs", list.tracks.size());
                        } else if (route_.page == Page::cloud && route_.list) {
                            route_.card.subtitle = lyren::trf("Cloud uploads · {} songs", list.tracks.size());
                        } else if (route_.page == Page::recent) {
                            route_.card.subtitle = lyren::trf("{} songs · From all your devices", list.tracks.size());
                        }
                        list_page(ui, r, list.tracks, list.loading, list.error, std::max(list.since, tab_time_));
                        break;
                    }
                }
            }
            ui.pop_clip();
            sidebar(ui, side);
        }
        Rect np_page{side.x + 8, area.y, area.right() - side.x - 8, area.h};
        // Narrower than 900 px (a panel open beside it, say) the view stacks the lyrics under a small cover; the slack keeps
        // resizing near it from flipping it back and forth.
        if (np_page.w >= 912) {
            np_wide_ = true;
        } else if (np_page.w < 888) {
            np_wide_ = false;
        }
        float wide = ui.spring(ui.id("np-wide"), np_wide_ ? 1 : 0, 420);
        if (np > 0) {
            UseTheme on_stage(ui, stage_theme);
            now_playing(ui, {0, 0, ui.width, ui.height}, np_page, np, wide);
        }
        {
            // The panel and the player bar take the stage's colours as it comes up.
            ui::Theme over = t.mix(stage_theme, np);
            UseTheme use(ui, over);
            if (slot.w > 0) {
                side_panel(ui, slot, np);
            }
            player_bar(ui, player, np);
            player_art(ui, player, np_page, np, wide);
        }
        handle_keys(ui);
        images_.end_frame();

        // Next frame when the progress bar moves a pixel or the time label changes.
        if (playing() && dur > 0) {
            double step = std::min(dur / std::max(progress_w_, 1.f), 1 - std::fmod(last_pos_, 1.0));
            ui.wake(ui.now + std::max(step, 0.02));
        }
        publish();
    }

    void Shell::handle_keys(ui::Ui &ui) {
        if (ui.modal_open()) {
            return;
        }
        for (auto &a : ui.input().actions) {
            if (a.press && !a.consumed && a.button == BTN_SIDE) {
                a.consumed = true;
                back(ui);
                ui.redraw();
            }
        }
        auto &keys = ui.input().keys;
        for (auto k = keys.begin(); k != keys.end();) {
            bool ctrl = k->mods & wl::mod_ctrl, alt = k->mods & wl::mod_alt, used = true;
            if (k->keysym == XKB_KEY_space && !k->repeat) {
                toggle_play();
            } else if (k->keysym == XKB_KEY_Escape && panel_ == Panel::comments && !ui.menu_open()) {
                show_panel(Panel::none);
            } else if (k->keysym == XKB_KEY_Escape && np_open_ && !ui.menu_open()) {
                show_stage(false);
            } else if (ctrl && (k->keysym == XKB_KEY_f || k->keysym == XKB_KEY_F)) {
                ui.focus = ui.id("search");
            } else if (ctrl && k->keysym == XKB_KEY_Right) {
                skip(1);
            } else if (ctrl && k->keysym == XKB_KEY_Left) {
                skip(-1);
            } else if (alt && k->keysym == XKB_KEY_Left) {
                back(ui);
            } else {
                used = false;
            }
            if (used) {
                k = keys.erase(k);
                ui.redraw();
            } else {
                ++k;
            }
        }
    }

    void Shell::sidebar(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        struct Nav {
            Page page;
            std::string_view icon, label;
        };
        const auto entries = std::to_array<Nav>({{Page::recommend, "compass", lyren::tr("For you")},
                                                 {Page::toplist, "list-ordered", lyren::tr("Charts")},
                                                 {Page::square, "layout-grid", lyren::tr("Browse")},
                                                 {Page::fm, "radio", lyren::tr(provider().features.radio.title)},
                                                 {Page::liked, "heart", lyren::tr("Liked songs")},
                                                 {Page::recent, "history", lyren::tr("Recently played")},
                                                 {Page::cloud, "cloud", lyren::tr("My cloud")},
                                                 {Page::collection, "star", lyren::tr("My collection")},
                                                 {Page::dislikes, "thumbs-down", lyren::tr("Disliked")}});
        std::inplace_vector<Nav, entries.size()> nav;
        size_t mine = 0;
        for (size_t k = 0; k < entries.size(); ++k) {
            if (supports(entries[k].page, active_)) {
                nav.push_back(entries[k]);
                if (k < 4) {
                    ++mine;
                }
            }
        }
        size_t lists = nav.size();
        const auto &playlists = library().playlists;
        constexpr float item_h = 38, step = 40, head_h = 36;
        // Labels fade as the sidebar folds into the rail.
        float labels = std::clamp((r.w - 72) / 148, 0.f, 1.f);
        auto item_y = [&](size_t k) {
            return 8 + head_h * static_cast<float>(1 + (k >= mine) + (k >= lists)) + step * static_cast<float>(k);
        };

        int sel = -1;
        for (size_t k = 0; k < nav.size(); ++k) {
            if (nav[k].page == route_.page || ((route_.page == Page::daily || route_.page == Page::radar || route_.page == Page::releases) && k == 0)) {
                sel = static_cast<int>(k);
            }
        }
        if (route_.page == Page::playlist) {
            for (size_t i = 0; i < playlists.size(); ++i) {
                if (same(playlists[i], route_.card)) {
                    sel = static_cast<int>(lists + i);
                }
            }
        }

        float content = item_y(lists + playlists.size()) + 8 + (playlists.empty() ? 40 : 0);
        float off = ui.begin_scroll(ui.id("sidebar"), r, content);
        Rect row0{r.x + 12, 0, r.w - 24, item_h};

        // One highlight glides to the selected entry.
        float hy = ui.spring(ui.id("side-sel"), sel >= 0 ? item_y(static_cast<size_t>(sel)) : 0, 260);
        float ha = ui.anim(ui.id("side-sel-a"), sel >= 0 ? 1 : 0, 200);
        if (ha > 0) {
            p.rect({row0.x, r.y + hy - off, row0.w, item_h}, t.text.with_alpha(0.09f * ha), t.radius_s());
        }

        auto heading = [&](size_t before, std::string_view s) {
            float y = r.y + item_y(before) - head_h - off;
            if (labels > 0.01f) {
                ui.label({r.x + 24, y + 12, r.w - 48, 20}, s, {.size = t.fs(12), .color = t.text_dim.with_alpha(labels)});
            }
            if (labels < 0.99f && before > 0) {
                p.rect({r.x + 24, y + head_h / 2, r.w - 48, 1}, t.border.with_alpha(1 - labels));
            }
        };
        auto item = [&](ui::Id id, size_t k, std::string_view label, std::string_view icon, const std::string *thumb) {
            Rect row = row0;
            row.y = r.y + item_y(k) - off;
            Rect clip = ui.clip();
            if (row.bottom() < clip.y || row.y > clip.bottom()) {
                return false;
            }
            auto it = ui.interact(id, row);
            float hot = ui.anim(id, it.hovered ? 1 : 0, 150);
            float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
            bool selected = sel == static_cast<int>(k);
            if (hot > 0 && !selected) {
                p.rect(row.inset(down), t.hover.with_alpha(hot), t.radius_s());
            }
            float ix = row.x + 10 + ((row.w - 20) / 2 - 10) * (1 - labels);
            Rect ic{ix, row.y + 10, 18, 18};
            if (thumb) {
                cover(ui, ic.inset(-2), *thumb, 5, "");
            } else {
                p.icon(icon, ic, selected ? t.accent : t.text_dim.mix(t.text, hot * 0.6f));
            }
            if (labels > 0.01f) {
                ui.label({row.x + 42, row.y, row.w - 50, row.h}, label,
                         {.size = t.fs(14), .color = (selected ? t.text : t.text.mix(t.text_dim, 0.3f * (1 - hot))).with_alpha(labels)});
            }
            return it.clicked;
        };

        heading(0, lyren::tr("Discover"));
        heading(mine, lyren::tr("My music"));
        heading(lists, lyren::tr("Playlists"));
        if (signed_in() && labels > 0.5f) {
            Fade fade(p, labels);
            float y = r.y + item_y(lists) - head_h - off;
            if (ui.icon_button(ui.id("new-playlist"), {r.right() - 46, y + 8, 26, 26}, "plus", 15)) {
                open_editor(ui);
            }
        }
        for (size_t k = 0; k < nav.size(); ++k) {
            if (item(ui.id(nav[k].icon), k, nav[k].label, nav[k].icon, nullptr)) {
                open(ui, nav[k].page);
            }
        }
        ui.push_id(ui.id("playlists"));
        for (size_t i = 0; i < playlists.size(); ++i) {
            if (item(ui.id(key(playlists[i])), lists + i, playlists[i].title, {}, &playlists[i].cover)) {
                open(ui, playlists[i]);
            }
        }
        ui.pop_id();
        if (playlists.empty() && labels > 0.01f) {
            ui.label({r.x + 24, r.y + item_y(lists) - off, r.w - 48, 20}, signed_in() ? lyren::tr("No playlists yet") : lyren::tr("Sign in to see your playlists"),
                     {.size = t.fs(12), .color = t.text_dim.with_alpha(0.7f * labels)});
        }
        ui.end_scroll();
    }

    bool Shell::page_status(ui::Ui &ui, Rect r, bool loading, const std::string &error, bool login) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Rect c = ui::center(r, std::min(r.w, 360.f), 150);
        if (login) {
            Rect ic = {c.x + (c.w - 44) / 2, c.y, 44, 44};
            p.rect(ic, t.text.with_alpha(0.06f), 22);
            p.icon("user", ui::center(ic, 20, 20), t.text_dim);
            ui.label({c.x, c.y + 58, c.w, 22}, lyren::tr("Sign in to use this page"), {.size = t.fs(15), .color = t.text}, Align::center);
            float bw = button_width(ui, lyren::tr("Scan to sign in"), true);
            if (ui.button(ui.id("page-login"), {c.x + (c.w - bw) / 2, c.y + 96, bw, 36}, lyren::tr("Scan to sign in"), ui::ButtonKind::primary, "qr-code")) {
                open_account(ui);
                login_step_ = static_cast<int>(active_);
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

    bool Shell::hero(ui::Ui &ui, ui::Id id, Rect r, std::string_view big, std::string_view title, std::string_view subtitle, float hue, std::string_view icon) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto it = ui.interact(id, r);
        float hot = ui.anim(id, it.hovered ? 1 : 0, 240);
        float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
        r = r.inset(down * 3);
        r.y -= 3 * hot;
        p.shadow({r.x, r.y + 10, r.w, r.h}, t.radius_l(), 32, t.shadow.with_alpha(0.35f + 0.65f * hot));
        p.gradient(r, hsl(hue, 0.6f, 0.52f), hsl(hue + 0.07f, 0.55f, 0.27f), t.radius_l());
        if (icon.empty()) {
            ui.label({r.x + 24, r.y + 16, r.w - 48, 54}, big, {.size = t.fs(42), .color = white});
        } else {
            p.icon(icon, {r.x + 24, r.y + 20, 40, 40}, white);
        }
        ui.label({r.x + 24, r.y + 74, r.w - 48, 24}, title, {.size = t.fs(18), .color = white});
        ui.label({r.x + 24, r.y + 98, r.w - 100, 20}, subtitle, {.size = t.fs(12), .color = white.with_alpha(0.75f)});
        Rect b = Rect{r.right() - 64, r.bottom() - 64, 44, 44}.inset(-2 * hot);
        p.rect(b, white.with_alpha(0.2f + 0.8f * hot), b.w / 2);
        p.icon("play-fill", ui::center(b, 18, 18), hsl(hue, 0.6f, 0.4f).mix(white, 1 - hot));
        return it.clicked;
    }

    bool Shell::card(ui::Ui &ui, ui::Id id, Rect r, const Card &c, bool round) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        auto it = ui.interact(id, r);
        float hot = ui.anim(id, it.hovered ? 1 : 0, 240);
        float down = ui.anim(ui::sub_id(id, 0xd0), it.held ? 1 : 0, 90);
        Rect cov = Rect{r.x, r.y - 4 * hot, r.w, r.w}.inset(down * 3);
        float radius = round ? cov.w / 2 : t.radius;
        if (hot > 0) {
            p.shadow({cov.x, cov.y + 8, cov.w, cov.h}, radius, 24, t.shadow.with_alpha(hot));
        }
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
        ui.label({r.x, r.y + r.w + 32, r.w, 18}, card_subtitle(c), {.size = t.fs(12), .color = t.text_dim}, align);
        if (it.right_clicked) {
            link_menu(ui, it.px, it.py, c);
        }
        return it.clicked;
    }

    int Shell::card_grid(ui::Ui &ui, Rect r, ui::Id id, const std::vector<Card> &cards, bool round, double since) {
        Grid g(ui, id, r.w, round ? 130 : 150);
        Rect clip = ui.clip();
        int clicked = -1, shown = 0;
        ui.push_id(id);
        for (size_t i = 0; i < cards.size(); ++i) {
            ui::Id item = ui.id(key(cards[i]));
            Rect c = g.cell(r.x, r.y, i, item);
            if (c.bottom() < clip.y || c.y > clip.bottom()) {
                if (c.bottom() >= clip.y - clip.h && c.y <= clip.bottom() + clip.h) {
                    images_.prefetch(cards[i].cover, static_cast<uint32_t>(Images::tier(c.w * ui.painter().scale())));
                }
                continue;
            }
            // Cards arrive one after another.
            float a = ui.progress(since + 0.03 * std::min(shown++, 14), 420);
            Fade fade(ui.painter(), a);
            c.y += (1 - a) * 16;
            if (card(ui, item, c, cards[i], round)) {
                clicked = static_cast<int>(i);
            }
        }
        ui.pop_id();
        return clicked;
    }

    void Shell::recommend_page(ui::Ui &ui, Rect r) {
        const auto &t = ui.theme();
        auto &lib = library();
        const auto &features = provider().features;
        struct Hero {
            Page page;
            std::string_view id, big, title, subtitle, icon;
            float hue;
        };
        std::inplace_vector<Hero, 4> heroes;
        std::string day = today_.substr(3);
        if (features.daily.songs) {
            heroes.push_back({Page::daily, "hero-daily", day, "Daily mix", "Fresh picks, every day", {}, 0.98f});
        }
        if (features.radio.tracks) {
            heroes.push_back({Page::fm, "hero-fm", "FM", features.radio.title, "A station that learns your taste", {}, 0.62f});
        }
        if (features.radar) {
            heroes.push_back({Page::radar, "hero-radar", {}, "Radar", "Favorites and new discoveries", "sparkles", 0.78f});
        }
        if (features.releases.songs && features.releases.albums) {
            heroes.push_back({Page::releases, "hero-releases", {}, "New releases", "Latest songs and albums", "disc-3", 0.08f});
        }
        int hero_count = static_cast<int>(heroes.size());
        Rect body = r.inset(32, 0);
        Grid daily(ui, ui.id("daily-cards"), body.w);
        Grid g(ui, ui.id("rec-cards"), body.w);
        constexpr float hero_h = 136;
        int hero_cols = std::min(hero_count, std::max(1, static_cast<int>((body.w + Grid::gap) / 280)));
        float heroes_h = hero_count ? static_cast<float>((hero_count + hero_cols - 1) / hero_cols) * (hero_h + Grid::gap) + 20 : 0;
        float daily_h = lib.daily_lists.empty() ? 0 : 44 + daily.height(lib.daily_lists.size()) + 40;
        float content = 28 + 76 + heroes_h + daily_h + 44 + std::max(g.height(lib.recommended.cards.size()), 120.f) + 32;
        float off = ui.begin_scroll(ui.id("recommend"), r, content);
        float y = r.y + 28 - off;
        const auto &profile = lib.account;
        page_title(ui, body, y, lyren::tr("For you"),
                   std::format("{} · {}", date_label(today_), profile ? lyren::trf("Picked for you, {}", profile->name) : lyren::tr("Picked for you")));
        y += 76;
        if (hero_count) {
            float hw = (body.w - Grid::gap * static_cast<float>(hero_cols - 1)) / static_cast<float>(hero_cols);
            for (int i = 0; i < hero_count; ++i) {
                const auto &entry = heroes[i];
                Rect tile{body.x + static_cast<float>(i % hero_cols) * (hw + Grid::gap), y + static_cast<float>(i / hero_cols) * (hero_h + Grid::gap), hw, hero_h};
                if (hero(ui, ui.id(entry.id), tile, entry.big, lyren::tr(entry.title), lyren::tr(entry.subtitle), entry.hue, entry.icon)) {
                    open(ui, entry.page);
                }
            }
            y += heroes_h;
        }
        if (!lib.daily_lists.empty()) {
            ui.label({body.x, y, body.w, 28}, lyren::tr("Daily playlists"), {.size = t.fs(18), .color = t.text});
            if (int i = card_grid(ui, {body.x, y + 44, body.w, 0}, ui.id("daily-cards"), lib.daily_lists, false, std::max(page_time_ + 0.08, lib.daily_since)); i >= 0) {
                open(ui, lib.daily_lists[static_cast<size_t>(i)]);
            }
            y += daily_h;
        }
        ui.label({body.x, y, body.w, 28}, lyren::tr("Recommended playlists"), {.size = t.fs(18), .color = t.text});
        y += 44;
        const Results &res = lib.recommended;
        if (!page_status(ui, {body.x, y, body.w, 120}, res.loading && res.cards.empty(), res.error, false)) {
            if (int i = card_grid(ui, {body.x, y, body.w, 0}, ui.id("rec-cards"), res.cards, false, std::max(page_time_ + 0.08, res.since)); i >= 0) {
                open(ui, res.cards[static_cast<size_t>(i)]);
            }
        }
        ui.end_scroll();
    }

    void Shell::toplist_page(ui::Ui &ui, Rect r) {
        const Results &res = library().toplists;
        Rect body = r.inset(32, 0);
        Grid g(ui, ui.id("top-cards"), body.w);
        float off = ui.begin_scroll(ui.id("toplists"), r, 28 + 84 + g.height(res.cards.size()) + 32);
        float y = r.y + 28 - off;
        page_title(ui, body, y, lyren::tr("Charts"), lyren::tr("Official charts, updated daily or weekly"));
        if (!page_status(ui, {body.x, y + 84, body.w, r.h - 140}, res.loading, res.error, false)) {
            if (int i = card_grid(ui, {body.x, y + 84, body.w, 0}, ui.id("top-cards"), res.cards, false, std::max(page_time_ + 0.05, res.since)); i >= 0) {
                open(ui, res.cards[static_cast<size_t>(i)]);
            }
        }
        ui.end_scroll();
    }

    void Shell::square_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        const auto &t = ui.theme();
        Rect body = r.inset(32, 0);
        Grid g(ui, ui.id("square-cards"), body.w);
        constexpr float chips_y = 84, grid_y = chips_y + 54;
        float content = 28 + grid_y + std::max(g.height(lib.square.cards.size()), 120.f) + (lib.square.more() ? 64 : 32);
        float off = ui.begin_scroll(ui.id("square"), r, content);
        float y = r.y + 28 - off;
        page_title(ui, body, y, lyren::tr("Browse"), lyren::tr("Explore playlists by category"));

        // The popular categories as far as they fit, the picked one among them, the rest behind More categories.
        const Category all;
        std::vector<const Category *> categories{&all};
        if (!lib.category.id.empty() && std::ranges::find(lib.categories.hot, lib.category.id, &Category::id) == lib.categories.hot.end()) {
            categories.push_back(&lib.category);
        }
        for (const auto &category : lib.categories.hot) {
            categories.push_back(&category);
        }
        float x = body.x, more_w = lib.categories.groups.empty() ? 0 : chip_w(ui, lyren::tr("More categories"), true);
        const Category *picked = nullptr;
        ui.push_id(ui.id("square-chips"));
        for (const Category *category : categories) {
            const auto name = category->id.empty() ? lyren::tr("All") : std::string_view(category->name);
            float w = chip_w(ui, name);
            if (x + w + 8 + more_w > body.right()) {
                break;
            }
            ui.push_id(ui.id(category->id));
            if (chip(ui, x, y + chips_y, name, category->id == lib.category.id)) {
                picked = category;
            }
            ui.pop_id();
            x += w + 8;
        }
        if (more_w > 0 && chip(ui, x, y + chips_y, lyren::tr("More categories"), false, "chevron-down")) {
            ui.open_modal(600, 480, [this](ui::Ui &u, Rect m) {
                categories_modal(u, m);
            });
        }
        ui.pop_id();
        if (picked) {
            set_category(ui, *picked);
        }

        float gy = y + grid_y;
        if (!page_status(ui, {body.x, gy, body.w, 120}, lib.square.loading && lib.square.cards.empty(), lib.square.cards.empty() ? lib.square.error : "", false)) {
            double since = std::max({page_time_ + 0.05, lib.square_time, lib.square.since});
            if (int i = card_grid(ui, {body.x, gy, body.w, 0}, ui.id("square-cards"), lib.square.cards, false, since); i >= 0) {
                open(ui, lib.square.cards[static_cast<size_t>(i)]);
            }
            float end = gy + g.height(lib.square.cards.size());
            if (lib.square.loading) {
                ui.spinner(ui::center({body.x, end, body.w, 64}, 20, 20), t.text_dim, 2);
            } else if (end < r.bottom() + 400) {
                load_square();
            }
        }
        ui.end_scroll();
    }

    void Shell::fm_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        auto &p = ui.painter();
        const auto &t = ui.theme();
        page_title(ui, r.inset(32, 0), r.y + 28, lyren::tr(provider().features.radio.title), lyren::tr("Tuned to what you play"));
        Rect below{r.x, r.y + 100, r.w, r.h - 100};
        if (page_status(ui, below, lib.fm_loading && lib.fm.empty(), lib.fm.empty() ? lib.fm_error : "", !signed_in())) {
            return;
        }
        if (lib.fm.empty()) {
            return;
        }
        lib.fm_pos = std::min(lib.fm_pos, static_cast<int>(lib.fm.size()) - 1);
        const Track &tr = lib.fm[static_cast<size_t>(lib.fm_pos)];
        bool on_air = is_current(tr);
        float size = std::clamp(std::min(r.w * 0.4f, r.h - 310), 160.f, 280.f);
        Rect col = ui::center(r, size, size + 150);
        col.y += 36;
        Rect cov{col.x, col.y, size, size};
        p.shadow({cov.x, cov.y + 14, cov.w, cov.h}, t.radius_l(), 44, t.shadow);
        cover(ui, cov, tr.cover, t.radius_l());
        ui.label({col.x - 100, cov.bottom() + 22, size + 200, 30}, tr.title, {.size = t.fs(20), .color = t.text}, Align::center);
        ui.label({col.x - 100, cov.bottom() + 54, size + 200, 20}, tr.artist + " · " + tr.album, {.size = t.fs(13), .color = t.text_dim}, Align::center);
        float by = cov.bottom() + 90, cx = col.x + size / 2;
        // thumbs-down, like, play, next as one centred row
        float x0 = cx - 102;
        float like_x = provider().features.radio.trash ? x0 + 52 : cx - 76;
        if (provider().features.radio.trash && ui.icon_button(ui.id("fm-trash"), {x0, by + 4, 40, 40}, "thumbs-down", 20)) {
            spawn([](Shell *self, Track track, uint64_t gen) -> Task<void> {
                auto r = co_await self->provider(track).features.radio.trash(track);
                if (gen != self->library(track.source).gen) {
                    co_return;
                }
                if (r) {
                    if (self->provider(track).features.exclusions.list) {
                        spawn(self->load_dislikes(track.source, DislikeKind::song));
                    }
                    self->notify(std::string(lyren::tr("You will hear fewer songs like this")), "thumbs-down");
                } else {
                    self->notify(lyren::trf("Could not dislike this song: {}", r.error()), "triangle-alert", true);
                }
            }(this, tr, library().gen));
            next_fm();
            return;
        }
        bool like = liked(tr);
        if (ui.icon_button(ui.id("fm-like"), {like_x, by + 4, 40, 40}, like ? "heart-fill" : "heart", heart_size(ui, tr, 20), like)) {
            toggle_like(tr);
        }
        ui::Id pid = ui.id("fm-play");
        Rect play_r{like_x + 52, by, 48, 48};
        auto it = ui.interact(pid, play_r);
        float hot = ui.anim(pid, it.hovered ? 1 : 0, 150), down = ui.anim(ui::sub_id(pid, 0xd0), it.held ? 1 : 0, 90);
        Rect pr = play_r.inset(-2 * hot + 2 * down);
        p.rect(pr, t.accent.mix(white, 0.1f * hot), pr.w / 2);
        p.icon(on_air && playing() ? "pause-fill" : "play-fill", ui::center(pr, 20, 20), white);
        if (it.clicked) {
            if (on_air) {
                toggle_play();
            } else {
                play(lib.fm, lib.fm_pos);
            }
        }
        if (ui.icon_button(ui.id("fm-next"), {like_x + 112, by + 4, 40, 40}, "skip-forward", 20)) {
            fm_playing_ = fm_playing_ || on_air;
            fm_source_ = active_;
            next_fm();
        }
    }

    void Shell::releases_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        Rect body = r.inset(32, 0);
        page_title(ui, body, r.y + 28, lyren::tr("New releases"), lyren::tr("Latest songs and albums"));
        const std::array<std::string_view, 2> tabs = {lyren::tr("Songs"), lyren::tr("Albums")};
        if (ui.tabs(ui.id("release-tabs"), {body.x, r.y + 100, body.w, 36}, tabs, lib.release_tab)) {
            selected_ = -1;
            tab_time_ = ui.now;
            load_releases();
        }
        int tab = lib.release_tab;
        const auto &categories = provider().features.releases.categories[tab];
        std::string_view category = lyren::tr(categories[lib.release_category[tab]].name);
        float cw = button_width(ui, category, true);
        if (ui.button(ui.id("release-category"), {body.x, r.y + 148, cw, 36}, category, ui::ButtonKind::secondary, "chevron-down")) {
            std::vector<ui::MenuItem> items;
            for (size_t i = 0; i < categories.size(); ++i) {
                items.push_back({i == static_cast<size_t>(lib.release_category[tab]) ? "check" : "", std::string(lyren::tr(categories[i].name)), [this, &ui, i] {
                                     set_release_category(ui, static_cast<int>(i));
                                 }});
            }
            ui.open_menu(body.x, r.y + 190, std::move(items));
        }
        if (ui.icon_button(ui.id("release-refresh"), {body.right() - 36, r.y + 148, 36, 36}, "refresh-cw", 18)) {
            set_release_category(ui, lib.release_category[tab]);
        }
        Results &res = lib.releases[tab];
        if (tab == 0) {
            float play_w = button_width(ui, lyren::tr("Play all"), true);
            if (ui.button(ui.id("release-play-all"), {body.x, r.y + 200, play_w, 40}, lyren::tr("Play all"), ui::ButtonKind::primary, "play-fill") && !res.tracks.empty()) {
                if (mode_ == Mode::shuffle) {
                    set_mode(Mode::list);
                }
                play(res.tracks, 0);
            }
            float shuffle_w = button_width(ui, lyren::tr("Shuffle"), true);
            if (ui.button(ui.id("release-shuffle"), {body.x + play_w + 12, r.y + 200, shuffle_w, 40}, lyren::tr("Shuffle"), ui::ButtonKind::secondary, "shuffle") &&
                !res.tracks.empty()) {
                set_mode(Mode::shuffle);
                play(res.tracks, static_cast<int>(next(rng_) % res.tracks.size()));
            }
        }
        float top = r.y + (tab ? 200 : 256);
        Rect content{r.x + 16, top, r.w - 32, r.bottom() - top - 8};
        std::string error = res.error;
        if (res.loaded && res.size() == 0 && error.empty()) {
            error = tab ? lyren::tr("No albums found") : lyren::tr("No songs here yet");
        }
        if (res.size() == 0 && page_status(ui, content, res.loading, error, false)) {
            return;
        }
        if (tab == 0) {
            track_list(ui, content, ui.id("release-songs"), res.tracks, std::max({page_time_, tab_time_, res.since}));
        } else {
            bool near_end;
            if (int i = grid_view(ui, content, ui.id("release-albums"), res.cards, false, std::max({page_time_, tab_time_, res.since}), res.more(), near_end); i >= 0) {
                open(ui, res.cards[static_cast<size_t>(i)]);
            }
            if (near_end) {
                load_releases();
            }
        }
    }

    void Shell::collection_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        Rect body = r.inset(32, 0);
        page_title(ui, body, r.y + 28, lyren::tr("My collection"), lyren::tr("Saved albums and followed artists"));
        if (page_status(ui, {r.x, r.y + 100, r.w, r.h - 100}, false, "", !signed_in())) {
            return;
        }
        const std::array<std::string_view, 2> tabs = {lyren::tr("Albums"), lyren::tr("Artists")};
        int before = lib.collection_tab;
        if (ui.tabs(ui.id("collection-tabs"), {body.x, r.y + 100, body.w, 36}, tabs, lib.collection_tab)) {
            tab_time_ = ui.now;
            tab_dir_ = lib.collection_tab > before ? 1 : -1;
        }
        float enter = ui.progress(tab_time_, 380);
        Fade fade(ui.painter(), enter);
        Rect content{r.x + 16 + (1 - enter) * 36 * static_cast<float>(tab_dir_), r.y + 148, r.w - 32, r.h - 156};
        const Results &res = lib.collection[lib.collection_tab];
        std::string error = res.cards.empty() ? res.error : "";
        if (res.cards.empty() && res.loaded && error.empty()) {
            error = lib.collection_tab ? lyren::tr("No followed artists yet") : lyren::tr("No saved albums yet");
        }
        if (res.cards.empty() && page_status(ui, content, res.loading && !res.loaded, error, false)) {
            return;
        }
        bool near_end;
        ui::Id id = ui.id(lib.collection_tab ? "collection-artists" : "collection-albums");
        if (int i = grid_view(ui, content, id, res.cards, lib.collection_tab == 1, std::max({page_time_, tab_time_, res.since}), false, near_end); i >= 0) {
            open(ui, res.cards[static_cast<size_t>(i)]);
        }
    }

    void Shell::dislikes_page(ui::Ui &ui, Rect r) {
        auto &lib = library();
        auto &p = ui.painter();
        const auto &t = ui.theme();
        Rect body = r.inset(32, 0);
        page_title(ui, body, r.y + 28, lyren::tr("Disliked"), lyren::tr("Manage what you hear in recommendations"));
        if (page_status(ui, {r.x, r.y + 100, r.w, r.h - 100}, false, "", !signed_in())) {
            return;
        }
        const std::array<std::string_view, 3> tabs = {lyren::tr("Songs"), lyren::tr("Artists"), lyren::tr("Styles")};
        const std::array icons = {"music", "mic-vocal", "sparkles"};
        int before = lib.dislike_tab;
        if (ui.tabs(ui.id("dislike-tabs"), {body.x, r.y + 100, body.w - 48, 36}, tabs, lib.dislike_tab)) {
            tab_time_ = ui.now;
            tab_dir_ = lib.dislike_tab > before ? 1 : -1;
            if (!lib.dislikes[lib.dislike_tab]) {
                spawn(load_dislikes(active_, static_cast<DislikeKind>(lib.dislike_tab)));
            }
        }
        static const DislikeList pending{.loading = true};
        const auto &cached = lib.dislikes[lib.dislike_tab];
        const DislikeList &list = cached ? *cached : pending;
        ui.disabled = list.loading;
        if (ui.icon_button(ui.id("dislike-refresh"), {body.right() - 36, r.y + 100, 36, 36}, "refresh-cw", 18)) {
            spawn(load_dislikes(active_, static_cast<DislikeKind>(lib.dislike_tab)));
        }
        ui.disabled = false;
        float enter = ui.progress(tab_time_, 380);
        Fade fade(p, enter);
        Rect content{body.x + (1 - enter) * 36 * static_cast<float>(tab_dir_), r.y + 152, body.w, r.h - 164};
        if (!list.error.empty()) {
            page_status(ui, content, false, list.error, false);
            return;
        }
        if (list.items.empty()) {
            page_status(ui, content, list.loading, std::string(lyren::tr("Nothing disliked here")), false);
            return;
        }
        constexpr float row_h = 72;
        float off = ui.begin_scroll(ui.id("dislikes-" + std::to_string(lib.dislike_tab)), content, row_h * static_cast<float>(list.items.size()));
        const size_t first = static_cast<size_t>(off / row_h);
        const size_t last = std::min(list.items.size(), static_cast<size_t>((off + content.h) / row_h) + 1);
        const float restore_w = button_width(ui, lyren::tr("Restore"), true);
        for (size_t i = first; i < last; ++i) {
            const Dislike &item = list.items[i];
            ui.push_id(ui.id(item.id));
            Rect row{content.x, content.y - off + row_h * static_cast<float>(i), content.w, row_h};
            const float k = ui.progress(std::max({page_time_, tab_time_, list.since}) + std::min(i - first, size_t(12)) * 0.024, 380);
            Fade row_fade(p, k);
            row.y += (1 - k) * 8;
            if (ui.hovering(row)) {
                p.rect(row.inset(0, 2), t.text.with_alpha(0.04f), t.radius_s());
            }
            cover(ui, {row.x + 10, row.y + 14, 44, 44}, item.cover, lib.dislike_tab == 1 ? 22 : t.radius_s(), icons[lib.dislike_tab]);
            float text_x = row.x + 70, text_w = row.w - 90 - restore_w;
            ui.label({text_x, row.y + 14, text_w, 24}, item.name.empty() ? item.id : item.name, {.size = t.fs(15), .color = t.text});
            ui.label({text_x, row.y + 40, text_w, 18}, lyren::tr("Hidden from recommendations"), {.size = t.fs(12), .color = t.text_dim});
            ui.disabled = list.loading || list.pending.contains(item.id);
            if (ui.button(ui.id("restore"), {row.right() - restore_w - 10, row.y + 18, restore_w, 36}, lyren::tr("Restore"), ui::ButtonKind::secondary, "check")) {
                set_dislike(item, false);
            }
            ui.disabled = false;
            ui.pop_id();
        }
        ui.end_scroll();
    }

    void Shell::artist_page(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        ArtistPage &a = *route_.artist;
        const Card &c = route_.card;
        Rect head{r.x + 32, r.y + 32, r.w - 64, 148};
        Rect cov{head.x, head.y, 148, 148};
        p.shadow({cov.x, cov.y + 10, cov.w, cov.h}, 74, 32, t.shadow.with_alpha(0.8f));
        cover(ui, cov, a.info ? a.info->avatar : c.cover, 74, "user");
        if (auto it = ui.interact(ui.id("artist-link"), cov); it.right_clicked) {
            link_menu(ui, it.px, it.py, c);
        }
        float tx = cov.right() + 28, tw = head.right() - tx;
        ui.label({tx, head.y + 2, tw, 18}, lyren::tr("Artists"), {.size = t.fs(12), .color = t.text_dim});
        ui.label({tx, head.y + 18, tw, 38}, a.info ? a.info->name : c.title, {.size = t.fs(28), .color = t.text});
        std::string about = a.error;
        if (a.info) {
            about = lyren::trf("{} songs · {} albums", a.info->songs, a.info->albums);
            if (!a.info->alias.empty()) {
                about = a.info->alias + " · " + about;
            }
        }
        ui.label({tx, head.y + 62, tw, 20}, about, {.size = t.fs(13), .color = t.text_dim});
        std::vector<Track> &songs = a.songs.tracks;
        float play_w = button_width(ui, lyren::tr("Play all"), true);
        if (ui.button(ui.id("play-all"), {tx, head.bottom() - 40, play_w, 40}, lyren::tr("Play all"), ui::ButtonKind::primary, "play-fill") && !songs.empty()) {
            if (mode_ == Mode::shuffle) {
                set_mode(Mode::list);
            }
            play(songs, 0);
        }
        if (a.info) {
            bool on = a.info->followed;
            float follow_w = std::max(button_width(ui, lyren::tr("Following"), true), button_width(ui, lyren::tr("Follow"), true));
            if (ui.button(ui.id("follow"), {tx + play_w + 12, head.bottom() - 40, follow_w, 40}, on ? lyren::tr("Following") : lyren::tr("Follow"), ui::ButtonKind::secondary,
                          on ? "user-check" : "user-plus")) {
                toggle_follow(route_.artist, {.kind = CardKind::artist, .source = c.source, .id = c.id, .title = a.info->name, .cover = a.info->avatar});
            }
        }

        // Songs and albums load a page at a time as they scroll; each tab loads the first time it is picked.
        const std::array<std::string_view, 3> tabs = {lyren::tr("Songs"), lyren::tr("Albums"), lyren::tr("About")};
        Rect bar{head.x, head.bottom() + 24, head.w, 36};
        int before = a.tab;
        if (ui.tabs(ui.id("artist-tabs"), bar, tabs, a.tab)) {
            tab_time_ = ui.now;
            tab_dir_ = a.tab > before ? 1 : -1;
            load_artist_tab(a.tab);
        }
        float enter = ui.progress(tab_time_, 380);
        Fade fade(p, enter);
        Rect content{r.x + 16 + (1 - enter) * 36 * static_cast<float>(tab_dir_), bar.bottom() + 12, r.w - 32, r.bottom() - bar.bottom() - 20};
        Results &res = a.tab == 0 ? a.songs : a.tab == 1 ? a.albums : a.intro;
        bool empty = a.tab == 2 ? a.sections.empty() : res.size() == 0;
        std::string error = empty ? res.error : "";
        if (empty && res.loaded && error.empty()) {
            error = a.tab == 2 ? lyren::tr("No biography available") : lyren::tr("Nothing here yet");
        }
        if (empty && page_status(ui, content, res.loading || (!res.loaded && error.empty()), error, false)) {
            return;
        }
        double since = std::max({page_time_ + 0.06, tab_time_, res.since});
        bool near_end = false;
        int clicked = -1;
        switch (a.tab) {
            case 0:
                near_end = track_list(ui, content, ui.id("artist-songs"), songs, since);
                break;
            case 1:
                clicked = grid_view(ui, content, ui.id("artist-albums"), a.albums.cards, false, since, a.albums.loading, near_end);
                break;
            default:
                prose(ui, {content.x + 16, content.y + 8, content.w - 16, content.h - 8}, ui.id("artist-intro"), a.sections);
        }
        if (near_end) {
            load_artist_tab(a.tab);
        }
        // Last: opening an album replaces the route this page draws from.
        if (clicked >= 0) {
            open(ui, a.albums.cards[static_cast<size_t>(clicked)]);
        }
    }

    void Shell::search_page(ui::Ui &ui, Rect r) {
        Rect body = r.inset(32, 0);
        const std::array<std::string_view, 4> tabs = {lyren::tr("Tracks"), lyren::tr("Albums"), lyren::tr("Artists"), lyren::tr("Playlists")};
        float tabs_w = body.w - button_width(ui, lyren::tr("Refresh"), true) - 16;
        int before = search_tab_;
        if (ui.tabs(ui.id("search-tabs"), {body.x, r.y + 100, tabs_w, 36}, tabs, search_tab_)) {
            selected_ = -1;
            tab_time_ = ui.now;
            tab_dir_ = search_tab_ > before ? 1 : -1;
            ensure_route_data();
        }
        SearchResults &shown = results_[static_cast<size_t>(active_)][search_tab_];
        ui::Id scroll = search_scroll_id(ui, active_, search_tab_);
        if (shown.loading) {
            ui.spinner({body.right() - 26, r.y + 109, 18, 18}, ui.theme().text_dim, 2);
        } else {
            auto label = lyren::tr(shown.error.empty() ? "Refresh" : "Retry");
            float w = button_width(ui, label, true);
            if (ui.button(ui.id("search-refresh"), {body.right() - w, r.y + 100, w, 36}, label, ui::ButtonKind::secondary, "refresh-cw")) {
                if (shown.error.empty()) {
                    shown = {};
                    selected_ = -1;
                    ui.scroll_to(scroll, 0);
                }
                load_results(search_tab_, true);
            }
        }
        const std::array kinds = {lyren::tr("songs"), lyren::tr("albums"), lyren::tr("artists"), lyren::tr("playlists")};
        page_title(ui, body, r.y + 28, '"' + searched_ + '"',
                   shown.loaded ? lyren::trf("{}{} {} found", shown.more() ? lyren::tr("About ") : "", shown.total, kinds[search_tab_])
                                : lyren::trf("Searching {}", lyren::tr(platform(active_).short_name)));
        // Tab content slides toward the tab that was picked.
        float enter = ui.progress(tab_time_, 380);
        Fade fade(ui.painter(), enter);
        Rect content{r.x + 16 + (1 - enter) * 36 * static_cast<float>(tab_dir_), r.y + 148, r.w - 32, r.h - 156};
        double since = std::max({page_time_, tab_time_, shown.since});
        bool empty = shown.size() == 0;
        std::string error = empty ? shown.error : "";
        if (empty && shown.loaded && error.empty()) {
            error = lyren::tr("No results found");
        }
        if (empty && page_status(ui, content, shown.loading, error, false)) {
            return;
        }
        if (!shown.error.empty()) {
            ui.label(ui::cut_bottom(content, 32).inset(12, 0), shown.error, {.size = ui.theme().fs(12), .color = ui.theme().danger});
        }
        if (search_tab_ == 0) {
            if (track_list(ui, content, scroll, shown.tracks, since)) {
                load_results(0);
            }
            return;
        }
        bool near_end;
        int i = grid_view(ui, content, scroll, shown.cards, search_tab_ == 2, since, shown.loading, near_end);
        if (near_end) {
            load_results(search_tab_);
        }
        if (i >= 0) {
            open(ui, shown.cards[static_cast<size_t>(i)]);
        }
    }

    int Shell::grid_view(ui::Ui &ui, Rect r, ui::Id id, const std::vector<Card> &cards, bool round, double since, bool loading, bool &near_end) {
        Rect inner = r.inset(12, 0);
        Grid g(ui, id, inner.w, round ? 130 : 150);
        float h = 12 + g.height(cards.size()) + (loading ? 64 : 20);
        float off = ui.begin_scroll(id, r, h);
        int i = card_grid(ui, {inner.x, r.y + 12 - off, inner.w, 0}, id, cards, round, since);
        near_end = r.y + h - off < r.bottom() + 400;
        if (loading) {
            ui.spinner(ui::center({inner.x, r.y + h - off - 64, inner.w, 64}, 20, 20), ui.theme().text_dim, 2);
        }
        ui.end_scroll();
        return i;
    }

    void Shell::list_page(ui::Ui &ui, Rect r, std::vector<Track> &tracks, bool loading, const std::string &list_error, double since) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Card &c = route_.card;
        Rect head{r.x + 32, r.y + 32, r.w - 64, 148};
        Rect cov{head.x, head.y, 148, 148};
        p.shadow({cov.x, cov.y + 10, cov.w, cov.h}, t.radius_l(), 32, t.shadow.with_alpha(0.8f));
        switch (route_.page) {
            case Page::liked:
                art(p, cov, special_hue(Page::liked), t.radius_l(), "heart-fill");
                break;
            case Page::recent:
                art(p, cov, special_hue(Page::recent), t.radius_l(), "history");
                break;
            case Page::cloud:
                art(p, cov, special_hue(Page::cloud), t.radius_l(), "cloud");
                break;
            case Page::radar:
                art(p, cov, 0.78f, t.radius_l(), "sparkles");
                break;
            case Page::daily: {
                art(p, cov, 0.98f, t.radius_l(), "");
                std::string day = today_.substr(3);
                ui.label(cov, day.substr(0, day.find(' ')), {.size = t.fs(54), .color = white}, Align::center);
                break;
            }
            default:
                cover(ui, cov, c.cover, t.radius_l(), route_.page == Page::album ? "disc-album" : "list-music");
        }
        float tx = cov.right() + 28, tw = head.right() - tx;
        TextStyle sub{.size = t.fs(13), .color = t.text_dim};
        if (route_.page == Page::playlist || route_.page == Page::album) {
            if (auto it = ui.interact(ui.id("list-link"), cov); it.right_clicked) {
                link_menu(ui, it.px, it.py, c);
            }
        }
        if (route_.page == Page::playlist || route_.page == Page::album || route_.page == Page::similar) {
            std::string_view kind = route_.page == Page::album ? lyren::tr("Albums") : route_.page == Page::similar ? lyren::tr("Similar songs") : lyren::tr("Playlists");
            ui.label({tx, head.y + 2, tw, 18}, kind, {.size = t.fs(12), .color = t.text_dim});
        }
        const bool built_in = route_.page == Page::daily || route_.page == Page::radar || route_.page == Page::cloud || route_.page == Page::liked || route_.page == Page::recent;
        ui.label({tx, head.y + 18, tw, 38}, built_in ? lyren::tr(c.title) : std::string_view(c.title), {.size = t.fs(28), .color = t.text});
        const AlbumInfo *album = route_.page == Page::album && route_.list && !route_.list->loading ? &route_.list->album : nullptr;
        if (album && !album->artist.empty()) {
            // Album: the artist links to their page, then the date and the label; the notes open in full.
            Rect ar{tx, head.y + 62, std::min(p.measure(album->artist, sub) + 1, tw), 20};
            ui::Id aid = ui.id("album-artist");
            auto it = ui.interact(aid, ar);
            float hot = ui.anim(aid, it.hovered ? 1 : 0, 120);
            ui.label(ar, album->artist, {.size = sub.size, .color = t.text_dim.mix(t.text, hot)});
            if (hot > 0) {
                p.rect({ar.x, ar.bottom() - 1, ar.w, 1}, t.text.with_alpha(0.4f * hot));
            }
            if (it.clicked && !album->artist_id.empty()) {
                open(ui, Page::artist, {.kind = CardKind::artist, .source = c.source, .id = album->artist_id, .title = album->artist.substr(0, album->artist.find(" / "))});
            }
            std::string rest;
            for (const std::string *part : {&album->date, &album->company}) {
                if (!part->empty()) {
                    rest += " · " + *part;
                }
            }
            ui.label({ar.right(), head.y + 62, tw - ar.w, 20}, rest, sub);
            if (!album->about.empty()) {
                Rect ab{tx, head.y + 84, tw, 20};
                ui::Id bid = ui.id("album-about");
                auto bt = ui.interact(bid, ab);
                float bh = ui.anim(bid, bt.hovered ? 1 : 0, 120);
                std::string_view first = album->about;
                first = first.substr(0, first.find('\n'));
                p.text(tx, ab.y + 15, lyren::trf("About: {}", first), {.size = t.fs(12), .color = t.text_dim.mix(t.text, 0.6f * bh)}, tw);
                if (bt.clicked) {
                    ui.open_modal(560, std::min(ui.height - 120, 600.f), [title = c.title, about = album->about](ui::Ui &u, Rect r) {
                        u.label({r.x, r.y, r.w - 40, 30}, title, {.size = u.theme().fs(20), .color = u.theme().text});
                        prose(u, {r.x, r.y + 52, r.w + 16, r.h - 52}, u.id("album-notes"), {{"", about}});
                    });
                }
            }
        } else {
            const std::string subtitle = route_.page == Page::daily ? lyren::trf("{} · Made for your taste", date_label(today_)) : card_subtitle(c);
            ui.label({tx, head.y + 62, tw, 20}, built_in ? lyren::tr(subtitle) : std::string_view(subtitle), sub);
        }
        float bx = tx;
        float play_w = button_width(ui, lyren::tr("Play all"), true), shuffle_w = button_width(ui, lyren::tr("Shuffle"), true);
        if (ui.button(ui.id("play-all"), {bx, head.bottom() - 40, play_w, 40}, lyren::tr("Play all"), ui::ButtonKind::primary, "play-fill") && !tracks.empty()) {
            if (mode_ == Mode::shuffle) {
                set_mode(Mode::list);
            }
            play(tracks, 0);
        }
        bx += play_w + 12;
        if (ui.button(ui.id("shuffle-all"), {bx, head.bottom() - 40, shuffle_w, 40}, lyren::tr("Shuffle"), ui::ButtonKind::secondary, "shuffle") && !tracks.empty()) {
            set_mode(Mode::shuffle);
            play(tracks, static_cast<int>(next(rng_) % tracks.size()));
        }
        list_actions(ui, bx + shuffle_w + 12, head.bottom() - 40);
        Rect body{r.x + 16, head.bottom() + 28, r.w - 32, r.bottom() - head.bottom() - 36};
        bool login = needs_login(route_.page) && !signed_in();
        std::string error = list_error;
        if (tracks.empty() && error.empty() && !loading && !login) {
            error = route_.page == Page::similar ? lyren::tr("No similar songs found") : lyren::tr("No songs here yet");
        }
        if (tracks.empty() && page_status(ui, body, loading, error, login)) {
            return;
        }
        track_list(ui, body, ui.id("tracks"), tracks, std::max(page_time_ + 0.06, since));
    }

    bool Shell::track_list(ui::Ui &ui, Rect r, ui::Id id, std::vector<Track> &tracks, double since) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        constexpr float row_h = 56;
        Rect head = ui::cut_top(r, 34);
        Columns c = columns(head);
        TextStyle hs{.size = t.fs(12), .color = t.text_dim};
        ui.label(c.index, "#", hs, Align::center);
        ui.label(c.title, lyren::tr("Title"), hs);
        if (c.album.w > 0) {
            ui.label(c.album, lyren::tr("Album"), hs);
        }
        const bool ordering = route_.page == Page::playlist && route_.list && &tracks == &route_.list->tracks && route_.list->ordering;
        ui.label(c.duration, lyren::tr(ordering ? "Order" : "Time"), hs, Align::right);
        p.rect({head.x + 12, head.bottom() - 1, head.w - 24, 1}, t.border);
        ui::cut_top(r, 4);

        float off = ui.begin_scroll(id, r, static_cast<float>(tracks.size()) * row_h + 8);
        // One row past the bottom keeps its place, so it slides in with the rest when a row above it goes.
        int first = static_cast<int>(off / row_h), last = static_cast<int>((off + r.h) / row_h) + 2;
        for (int i = std::max(0, first - 8); i < std::min(last + 16, static_cast<int>(tracks.size())); ++i) {
            if (i < first || i > last) {
                images_.prefetch(tracks[static_cast<size_t>(i)].cover, static_cast<uint32_t>(Images::tier(40 * p.scale())));
            }
        }
        // Rows glide to a new place when the list changes under them; a list that arrives anew starts in place.
        ui.push_id(ui::sub_id(id, std::bit_cast<uint64_t>(since)));
        for (int i = first; i <= last && i < static_cast<int>(tracks.size()); ++i) {
            // Rows cascade in when the list appears.
            float a = ui.progress(since + 0.022 * std::max(0, i - first), 380);
            Fade fade(p, a);
            float y = ui.spring(ui::sub_id(ui.id(key(tracks[static_cast<size_t>(i)])), 0x70), static_cast<float>(i) * row_h, 280);
            track_row(ui, {r.x, r.y + y - off + (1 - a) * 14, r.w, row_h}, tracks, i);
        }
        ui.pop_id();
        ui.end_scroll();
        return last + 10 >= static_cast<int>(tracks.size());
    }

    void Shell::track_row(ui::Ui &ui, Rect r, std::vector<Track> &tracks, int i) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Track &tr = tracks[static_cast<size_t>(i)];
        ui.push_id(ui.id(key(tr)));
        ui::Id row_id = ui.id("row");
        bool hot = ui.hovering(r) || ui.is_active(row_id);
        bool now_playing = is_current(tr);
        float h = ui.anim(row_id, hot ? 1 : 0, 140);
        float s = ui.anim(ui::sub_id(row_id, 0x5e), selected_ == i ? 1 : 0, 200);
        Rect bg = r.inset(4, 2);
        if (h > 0 || s > 0) {
            p.rect(bg, t.text.with_alpha(0.06f * h + 0.05f * s), t.radius_s());
        }

        Columns c = columns(r);
        TextStyle num{.size = t.fs(13), .color = t.text_dim, .features = text::tabular_nums};
        if (hot) {
            if (ui.icon_button(ui.id("play"), ui::center(c.index, 30, 30), now_playing && playing() ? "pause-fill" : "play-fill", 14)) {
                if (now_playing) {
                    toggle_play();
                } else {
                    play(tracks, i);
                }
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
        if (!tr.alias.empty() && w + 24 < tw) {
            p.text(tx + w + 6, r.y + 25, "（" + tr.alias + "）", {.size = t.fs(14), .color = t.text_dim}, tw - w - 6);
        }
        p.text(tx, r.y + 43, tr.artist, {.size = t.fs(12), .color = t.text_dim}, tw);
        if (c.album.w > 0) {
            ui.label(c.album, tr.album, {.size = t.fs(13), .color = t.text_dim});
        }
        const bool ordering = route_.page == Page::playlist && route_.list && &tracks == &route_.list->tracks && route_.list->ordering;
        if (ordering) {
            auto move = [&](std::string_view id, Rect cell, std::string_view icon, int to) {
                ui.disabled = route_.list->saving_order || route_.list->loading || to < 0 || to >= static_cast<int>(tracks.size());
                bool clicked;
                {
                    Fade dim(p, ui.disabled ? 0.3f : 1.f);
                    clicked = ui.icon_button(ui.id(id), ui::center(cell, 32, 32), icon, 18);
                }
                ui.disabled = false;
                if (clicked) {
                    move_playlist_track(route_.list, route_.card, i, to);
                }
                return clicked;
            };
            if (move("move-up", c.like, "chevron-up", i - 1) || move("move-down", c.duration, "chevron-down", i + 1)) {
                ui.pop_id();
                return;
            }
        } else {
            bool like = liked(tr);
            if ((hot || like) && ui.icon_button(ui.id("like"), ui::center(c.like, 32, 32), like ? "heart-fill" : "heart", heart_size(ui, tr, 16), like)) {
                toggle_like(tr);
            }
            ui.label(c.duration, mmss(tr.duration), num, Align::right);
        }

        // Disabled ordering buttons still own their cells; a repeated click must not reach the row's play action.
        Rect row = r;
        if (ordering) {
            row.w = c.like.x - r.x;
        }
        auto it = ui.interact(row_id, row);
        if (it.pressed) {
            selected_ = i;
        }
        if (it.double_clicked) {
            play(tracks, i);
        }
        if (it.right_clicked) {
            selected_ = i;
            track_menu(ui, it.px, it.py, tracks, i);
        }
        ui.pop_id();
    }

    void Shell::list_actions(ui::Ui &ui, float x, float y) {
        const Card &c = route_.card;
        auto square = [&](std::string_view id, std::string_view icon) {
            bool clicked = ui.button(ui.id(id), {x, y, 40, 40}, "", ui::ButtonKind::secondary, icon);
            x += 52;
            return clicked;
        };
        auto labelled = [&](std::string_view id, std::string_view label, std::string_view icon, float w) {
            bool clicked = ui.button(ui.id(id), {x, y, w, 40}, label, ui::ButtonKind::secondary, icon);
            x += w + 12;
            return clicked;
        };
        if (route_.page == Page::album) {
            if (!route_.list || route_.list->loading) {
                return;
            }
            bool on = route_.list->album.collected;
            float w = std::max(button_width(ui, lyren::tr("Saved"), true), button_width(ui, lyren::tr("Save"), true));
            if (labelled("collect", on ? lyren::tr("Saved") : lyren::tr("Save"), on ? "star-fill" : "star", w)) {
                toggle_collect(route_.list, {.kind = CardKind::album, .source = c.source, .id = c.id, .title = c.title, .subtitle = route_.list->album.artist, .cover = c.cover});
            }
            if (square("album-comments", "message-square")) {
                open_comments(ui, c.source, Subject::album, route_.list->album.thread, c.title, c.cover);
            }
        } else if (route_.page == Page::radar) {
            if (route_.list && !route_.list->loading && square("radar-refresh", "refresh-cw")) {
                spawn(load_list(route_.list, Page::radar, c));
            }
        } else if (route_.page == Page::liked) {
            if (!provider().features.heartbeat || !signed_in() || library().liked.id.empty() || !route_.list || route_.list->tracks.empty()) {
                return;
            }
            if (labelled("heartbeat", lyren::tr("Heartbeat"), "heart-pulse", button_width(ui, lyren::tr("Heartbeat"), true))) {
                // From the picked song, otherwise from any of them.
                const auto &v = route_.list->tracks;
                size_t seed = selected_ >= 0 && selected_ < static_cast<int>(v.size()) ? static_cast<size_t>(selected_) : next(rng_) % v.size();
                spawn(heartbeat(v[seed]));
            }
        } else if (route_.page == Page::recent) {
            const std::array<std::string_view, 2> tabs = {lyren::tr("This device"), lyren::tr("Cloud")};
            const float tabs_w = button_width(ui, tabs[0], false) + button_width(ui, tabs[1], false);
            if (provider().features.recent_plays && signed_in() && ui.tabs(ui.id("recent-tabs"), {x, y, tabs_w, 40}, tabs, library().recent_tab, ui::TabStyle::pill)) {
                tab_time_ = ui.now;
                selected_ = -1;
                ui.scroll_to(ui.id("tracks"), 0);
                ensure_route_data();
            }
        } else if (route_.page == Page::playlist) {
            if (c.collectable && !c.owned && provider(c).features.library.collect_playlist && route_.list) {
                bool on = std::ranges::any_of(library(c.source).playlists, [&](const Card &card) {
                    return same(card, c);
                });
                float w = std::max(button_width(ui, lyren::tr("Saved"), true), button_width(ui, lyren::tr("Save"), true));
                if (route_.list->saving_playlist) {
                    ui.spinner(ui::center({x, y, w, 40}, 18, 18), ui.theme().text_dim, 2);
                    x += w + 12;
                } else if (labelled("collect-playlist", on ? lyren::tr("Saved") : lyren::tr("Save"), on ? "star-fill" : "star", w)) {
                    toggle_collect_playlist(route_.list, c);
                }
            }
            if (c.commentable && square("playlist-comments", "message-square")) {
                open_comments(ui, c.source, Subject::playlist, c.thread.empty() ? c.id : c.thread, c.title, c.cover);
            }
            if (c.owned && square("playlist-more", "ellipsis")) {
                std::vector<ui::MenuItem> items;
                if (provider(c).features.library.reorder_playlist && route_.list && !route_.list->loading) {
                    items.push_back({"list-ordered", std::string(lyren::tr(route_.list->ordering ? "Done reordering" : "Reorder songs")), [this, c, list = route_.list] {
                                         if (!provider(c).logged_in()) {
                                             return notify(lyren::trf("Sign in to {} to reorder this playlist", lyren::tr(platform(c.source).name)), "user");
                                         }
                                         list->ordering = !list->ordering;
                                         if (list->ordering) {
                                             notify(std::string(lyren::tr("Use the arrows to move songs")), "list-ordered");
                                         }
                                     }});
                }
                if (route_.list && !route_.list->loading && !route_.list->saving_order) {
                    items.push_back({"refresh-cw", std::string(lyren::tr("Refresh")), [this, c, list = route_.list] {
                                         spawn(load_list(list, Page::playlist, c));
                                     }});
                }
                if (auto link = share_url(c); !link.empty()) {
                    items.push_back({"copy", std::string(lyren::tr("Copy link")), [&ui, link = std::move(link)] {
                                         ui.copy = link;
                                         ui.toast(std::string(lyren::tr("Link copied")), "copy");
                                     }});
                }
                items.push_back({"pencil", std::string(lyren::tr("Rename")), [this, &ui, c] {
                                     open_editor(ui, c);
                                 }});
                items.push_back({"trash", std::string(lyren::tr("Delete playlist")),
                                 [this, &ui, c] {
                                     confirm_delete(ui, c);
                                 },
                                 true});
                ui.open_menu(x - 52, y + 46, std::move(items));
            }
        }
    }

    void Shell::track_menu(ui::Ui &ui, float x, float y, std::vector<Track> &tracks, int i) {
        Track tr = tracks[static_cast<size_t>(i)];
        bool like = liked(tr);
        auto *list = &tracks;
        std::vector<ui::MenuItem> items{
            {"play", std::string(lyren::tr("Play")),
             [this, list, i] {
                 play(*list, i);
             }},
            {"list-plus", std::string(lyren::tr("Play next")),
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
                 ui.toast(std::string(lyren::tr("Added to play next")), "list-plus");
             }},
            {"disc-album", std::string(lyren::tr("View album")),
             [this, &ui, tr] {
                 open(ui, Page::album, {.kind = CardKind::album, .source = tr.source, .id = tr.album_id, .title = tr.album, .subtitle = tr.artist, .cover = tr.cover});
             }},
            {"mic-vocal", std::string(lyren::tr("View artist")),
             [this, &ui, tr] {
                 open(ui, Page::artist, {.kind = CardKind::artist, .source = tr.source, .id = tr.artist_id, .title = tr.artist.substr(0, tr.artist.find(" / "))});
             }},
            {"message-square", std::string(lyren::tr("Comments")),
             [this, &ui, tr] {
                 open_comments(ui, tr.source, Subject::song, tr.id, tr.title + " · " + tr.artist, tr.cover);
             }},
            {},
            {like ? "heart-off" : "heart", like ? std::string(lyren::tr("Unlike")) : std::string(lyren::tr("Like")),
             [this, tr] {
                 toggle_like(tr);
             }},
            {"folder-plus", std::string(lyren::tr("Add to playlist")),
             [this, &ui, tr] {
                 if (!provider(tr).logged_in()) {
                     return ui.toast(lyren::trf("Sign in to {} to add songs to a playlist", lyren::tr(platform(tr.source).name)), "folder-plus");
                 }
                 ui.open_modal(420, 480, [this, tr](ui::Ui &u, Rect r) {
                     picker_modal(u, r, tr);
                 });
             }},
            {"copy", std::string(lyren::tr("Copy title and artist")),
             [&ui, tr] {
                 ui.copy = tr.title + " - " + tr.artist;
                 ui.toast(std::string(lyren::tr("Copied to clipboard")), "copy");
             }},
            {"copy", std::string(lyren::tr("Copy link")),
             [&ui, link = share_url(tr)] {
                 ui.copy = link;
                 ui.toast(std::string(lyren::tr("Link copied")), "copy");
             }},
        };
        if (provider(tr).features.similar) {
            items.insert(items.begin() + 4, {"sparkles", std::string(lyren::tr("Similar songs")), [this, &ui, tr] {
                                                 open(ui, Page::similar, {.source = tr.source, .id = tr.id, .title = tr.title, .subtitle = tr.artist, .cover = tr.cover});
                                             }});
        }
        bool listed = route_.list && list == &route_.list->tracks;
        if (route_.page == Page::daily && listed && provider(tr).features.daily.dislike) {
            items.push_back({"thumbs-down", std::string(lyren::tr("Not interested")), [this, l = route_.list, tr] {
                                 dislike_daily(l, tr);
                             }});
        }
        if (provider(tr).features.exclusions.set) {
            items.push_back({"thumbs-down", std::string(lyren::tr("Dislike this song")), [this, tr] {
                                 set_dislike({.source = tr.source, .id = tr.id, .name = tr.title, .cover = tr.cover}, true);
                             }});
        }
        // One's own playlist can take songs out.
        if (route_.page == Page::playlist && route_.card.owned && listed) {
            items.push_back({"list-minus", std::string(lyren::tr("Remove from playlist")),
                             [this, l = route_.list, c = route_.card, tr] {
                                 remove_from_playlist(l, c, tr);
                             },
                             true});
        }
        ui.open_menu(x, y, std::move(items));
    }

    void Shell::player_bar(ui::Ui &ui, Rect r, float stage) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        float side_w = std::min(330.f, std::round(r.w * 0.3f));
        ui::cut_left(r, side_w);
        Rect right = ui::cut_right(r, side_w), mid = r;
        const Track *tr = current();
        auto st = player_.state();

        // Transport; play and pause morph into each other, a ring turns while the track loads or buffers.
        float cx = std::round(mid.x + mid.w / 2), cy = mid.y + 30;
        static constexpr std::array mode_icons = {"repeat", "repeat-1", "shuffle"};
        const std::array mode_names = {lyren::tr("Repeat all"), lyren::tr("Repeat one"), lyren::tr("Shuffle")};
        if (ui.icon_button(ui.id("mode"), {cx - 124, cy - 16, 32, 32}, mode_icons[static_cast<int>(mode_)], 17, mode_ != Mode::list)) {
            set_mode(static_cast<Mode>((static_cast<int>(mode_) + 1) % 3));
            ui.toast(std::string(mode_names[static_cast<int>(mode_)]), mode_icons[static_cast<int>(mode_)]);
        }
        // Without a track the rest of the transport and the progress bar are dimmed and do not respond.
        ui.disabled = !tr;
        {
            Fade dim(p, tr ? 1 : 0.4f);
            if (ui.icon_button(ui.id("prev"), {cx - 72, cy - 16, 32, 32}, "skip-back", 18)) {
                skip(-1);
            }
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
            if (it.clicked) {
                toggle_play();
            }
            if (ui.icon_button(ui.id("next"), {cx + 40, cy - 16, 32, 32}, "skip-forward", 18)) {
                skip(1);
            }
            if (ui.icon_button(ui.id("lyrics"), {cx + 92, cy - 16, 32, 32}, "mic-vocal", 17, np_open_)) {
                show_stage(!np_open_);
            }
        }

        // Progress: a sine wave runs through the played part while the music plays and settles flat on pause.
        // The bar spans the whole song: the stretch a preview file holds is solid, the rest dotted, the lighter
        // marks are downloaded ranges, and the band under the rail is the service's chorus window.
        // Dragging previews the position and seeks on release.
        TextStyle ts{.size = t.fs(11), .color = t.text_dim, .features = text::tabular_nums};
        float py = mid.y + 66;
        const auto clock = timeline();
        const double file_dur = clock.file_duration(), song_dur = clock.duration();
        double pos = st == State::idle && !fetching_ ? resume_at_ : last_pos_;
        const double shown_pos = clock.to_song(pos);
        Rect bar{mid.x + 52, py - 9, mid.w - 104, 18};
        progress_w_ = bar.w;
        std::vector<std::pair<float, float>> buffered;
        if (const audio::Spool *sp = player_.spool(); sp && file_dur > 0) {
            auto [have, size] = sp->progress();
            if (size && have < size) {
                for (auto [b, e] : sp->ranges()) {
                    auto frac = [&](uint64_t at) {
                        return clock.progress(file_dur * static_cast<double>(at) / static_cast<double>(size));
                    };
                    buffered.emplace_back(frac(b), frac(e));
                }
            }
        }
        ui::Id prog = ui.id("progress");
        // A jump of the position (a seek, another track) glides; a drag follows the pointer.
        float v = ui.spring(ui.id("progress-v"), dragging_ ? drag_v_ : clock.progress(pos), dragging_ ? 0 : 260);
        float wave = ui.anim(ui.id("wave"), st == State::playing && config_.interface.progress == ProgressStyle::wave ? 3.f : 0.f, 600, ease::standard);
        ui::SliderStyle slide{.wave = wave, .marked = buffered};
        if (song_dur > 0) {
            const auto [from, to] = clock.playable_span();
            slide.span_from = from, slide.span_to = to;
            if (highlight_to_ > highlight_from_) {
                slide.band_from = clock.fraction(highlight_from_), slide.band_to = clock.fraction(highlight_to_);
            }
        }
        bool moved = ui.slider(prog, bar, v, slide);
        ui.disabled = false;
        if (tr && song_dur > 0 && (moved || ui.is_active(prog))) {
            dragging_ = true, drag_v_ = v;
        }
        if (dragging_ && !ui.is_active(prog)) {
            dragging_ = false;
            seek_song(drag_v_ * song_dur);
        }
        ui.label({mid.x, py - 8, 44, 16}, mmss(dragging_ ? drag_v_ * song_dur : tr ? shown_pos : 0), ts, Align::right);
        ui.label({bar.right() + 8, py - 8, 44, 16}, mmss(song_dur), ts);

        // Volume and panels; narrow windows keep only the icons.
        float vol_w = ui.spring(ui.id("vol-w"), compact_ ? 0 : 100, 280);
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
        const char *vol_icon = shown == 0 ? "volume-x" : shown < 0.5f ? "volume-1" : "volume-2";
        if (ui.icon_button(ui.id("mute"), {vol.x - 40, cy - 16, 32, 32}, vol_icon, 18)) {
            muted_ = !muted_;
            apply_volume();
            changed();
        }
        if (ui.icon_button(ui.id("queue"), {vol.x - 80, cy - 16, 32, 32}, "list-music", 18, panel_ == Panel::queue)) {
            show_panel(panel_ == Panel::queue ? Panel::none : Panel::queue);
        }
        const Rect desktop{vol.x - 120, cy - 16, 32, 32};
        const auto toggle = ui.interact(ui.id("desktop-lyrics"), desktop);
        const float hover = ui.anim(ui.id("desktop-hover"), toggle.hovered ? 1 : 0, 150);
        p.rect(desktop, (dl_enabled_ ? t.accent : t.text).with_alpha((dl_enabled_ ? 0.12f : 0) + hover * 0.08f), t.radius_s());
        p.icon("captions", ui::center(desktop, 18, 18), dl_enabled_ ? t.accent : t.text_dim.mix(t.text, hover));
        if (toggle.clicked) {
            set_desktop_enabled(!dl_enabled_);
        }
        if (toggle.right_clicked) {
            std::vector<ui::MenuItem> menu;
            for (bool island : {false, true}) {
                menu.push_back({dl_enabled_ && dl_island_ == island ? "check"
                                : island                            ? "panel-top"
                                                                    : "panel-bottom",
                                std::string(lyren::tr(island ? "Dynamic Island" : "Floating lyrics")), [this, island] {
                                    set_desktop_island(island);
                                    set_desktop_enabled(true);
                                }});
            }
            if (dl_enabled_) {
                menu.push_back({"x", std::string(lyren::tr("Hide desktop lyrics")), [this] {
                                    set_desktop_enabled(false);
                                }});
            }
            ui.open_menu(desktop.x, desktop.y, std::move(menu));
        }

        // On the stage, before the desktop lyrics: the comments on the song on show, under the panel they open, and the
        // translation of its lyrics when they have one, which keeps its place while the next song's lyrics load. They
        // come in once the stage is nearly up.
        if (const float shown = smooth01((stage - 0.7f) / 0.3f); tr && shown > 0) {
            Fade fade(p, shown);
            ui.disabled = shown < 0.9f;
            const float x = desktop.x + 8 * (1 - shown);
            const bool following = thread_.follows && thread_.subject == Subject::song && thread_.id == tr->id && thread_.source == tr->source;
            const bool on = panel_ == Panel::comments && following;
            if (ui.icon_button(ui.id("comments"), {x - 40, cy - 16, 32, 32}, "message-square", 17, on)) {
                if (on) {
                    show_panel(Panel::none);
                } else {
                    open_comments(ui, tr->source, Subject::song, tr->id, tr->title + " · " + tr->artist, tr->cover, true);
                }
            }
            const bool translated = std::ranges::any_of(lyrics_, [](const LyricLine &line) {
                return !line.translation.empty();
            });
            const ui::Id tid = ui.id("translation"), present_id = ui::sub_id(tid, 1);
            const float present = ui.spring(present_id, lyrics_loading_ ? ui.spring_value(present_id, 0) : translated ? 1 : 0, 260);
            if (present > 0.01f) {
                Fade appear(p, std::clamp(present, 0.f, 1.f));
                ui.disabled = ui.disabled || present < 0.5f;
                if (ui.icon_button(tid, {x - 80, cy - 16, 32, 32}, "languages", 17, lyric_translation_)) {
                    toggle_translation();
                }
            }
            ui.disabled = false;
        }
    }

    float Shell::heart_size(ui::Ui &ui, const Track &t, float size) const {
        if (ui.theme().motion == 0) {
            return size;
        }
        float k = static_cast<float>(ui.now - pop_time_) / (0.45f * ui.theme().motion);
        if (!(k >= 0 && k < 1) || pop_key_ != key(t)) {
            return size;
        }
        ui.keep_animating();
        // Up past its size and back, then a little under it.
        float s = k < 0.55f ? 1 + 0.32f * std::sin(std::numbers::pi_v<float> * k / 0.55f) : 1 - 0.06f * std::sin(std::numbers::pi_v<float> * (k - 0.55f) / 0.45f);
        return size * s;
    }

    void Shell::account_modal(ui::Ui &ui, Rect r) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        // Steps slide sideways: forward into the code, back to the list.
        float in = ui.progress(login_time_, 380);
        Fade fade(p, in);
        r.x += (1 - in) * 32 * (login_step_ >= 0 ? 1 : -1);

        if (login_step_ < 0) {
            ui.resize_modal(420, 186 + 72 * static_cast<float>(platforms.size()));
            ui.label({r.x, r.y, r.w, 30}, lyren::tr("Accounts"), {.size = t.fs(20), .color = t.text});
            ui.label({r.x, r.y + 32, r.w, 20}, lyren::tr("Sign in to each platform to see its music"), {.size = t.fs(12), .color = t.text_dim});
            for (size_t i = 0; i < platforms.size(); ++i) {
                const auto &entry = platforms[i];
                Rect row{r.x, r.y + 76 + static_cast<float>(i) * 72, r.w, 64};
                p.rect(row, t.text.with_alpha(0.04f), t.radius);
                Rect avatar{row.x + 14, row.y + 12, 40, 40};
                const auto &account = library(entry.source).account;
                bool signed_in = account.has_value();
                if (signed_in) {
                    cover(ui, avatar, account->avatar, 20, "user");
                } else {
                    p.rect(avatar, t.text.with_alpha(0.08f), 20);
                    p.icon("user", ui::center(avatar, 18, 18), t.text_dim);
                }
                ui.label({avatar.right() + 14, row.y + 12, 180, 22}, lyren::tr(entry.name), {.size = t.fs(15), .color = t.text});
                std::string_view status = signed_in                            ? std::string_view(account->name)
                                          : provider(entry.source).logged_in() ? lyren::tr("Loading account")
                                                                               : lyren::tr("Not signed in");
                ui.label({avatar.right() + 14, row.y + 34, 180, 18}, status, {.size = t.fs(12), .color = signed_in ? t.accent : t.text_dim});
                ui::Id id = ui.id(entry.id);
                const bool in = provider(entry.source).logged_in();
                const float bw = button_width(ui, in ? lyren::tr("Sign out") : lyren::tr("Scan to sign in"), true);
                Rect b{row.right() - 14 - bw, row.y + 15, bw, 34};
                if (in) {
                    if (ui.button(id, b, lyren::tr("Sign out"), ui::ButtonKind::ghost, "log-out")) {
                        spawn(logout(entry.source));
                    }
                } else if (ui.button(id, b, lyren::tr("Scan to sign in"), ui::ButtonKind::secondary, "qr-code")) {
                    login_step_ = static_cast<int>(entry.source);
                    login_time_ = ui.now;
                    spawn(qr_login());
                }
            }
            ui.label({r.x, r.bottom() - 18, r.w, 18}, lyren::tr("Sign-ins are encrypted on this device"), {.size = t.fs(12), .color = t.text_dim});
            return;
        }

        ui.resize_modal(400, 540);
        if (ui.icon_button(ui.id("login-back"), {r.x - 8, r.y - 2, 32, 32}, "arrow-left", 18)) {
            cancel_login();
            login_step_ = -1;
            login_time_ = ui.now;
            return;
        }
        ui.label({r.x + 32, r.y, r.w - 64, 30}, lyren::tr("Scan to sign in"), {.size = t.fs(20), .color = t.text});
        ui.label({r.x, r.y + 40, r.w, 20}, lyren::tr(platform(static_cast<Source>(login_step_)).scan_hint), {.size = t.fs(13), .color = t.text_dim}, Align::center);
        Rect qr = ui::center({r.x, r.y + 76, r.w, 240}, 228, 228);
        p.shadow({qr.x, qr.y + 8, qr.w, qr.h}, 14, 30, t.shadow.with_alpha(0.6f));
        p.rect(qr, white, 14);
        QrState state = qr_status_ ? qr_status_->state : QrState::waiting;
        if (qr_image_ || (qr_size_ > 0 && !qr_.empty())) {
            // Modules as horizontal runs, so a row is a few rectangles.
            Rect q = qr.inset(14);
            ui::Color ink{0.07f, 0.07f, 0.08f, 1};
            if (qr_image_) {
                p.image(q, images_.get("qr", 512).tex, 0);
            } else {
                float m = q.w / static_cast<float>(qr_size_);
                for (int y = 0; y < qr_size_; ++y) {
                    for (int x = 0; x < qr_size_;) {
                        if (!qr_[static_cast<size_t>(y * qr_size_ + x)]) {
                            ++x;
                            continue;
                        }
                        int x1 = x;
                        while (x1 < qr_size_ && qr_[static_cast<size_t>(y * qr_size_ + x1)]) {
                            ++x1;
                        }
                        p.rect({q.x + static_cast<float>(x) * m, q.y + static_cast<float>(y) * m, static_cast<float>(x1 - x) * m, m}, ink);
                        x = x1;
                    }
                }
                Rect logo = ui::center(qr, 40, 40);
                p.rect(logo.inset(-4), white, 12);
                p.rect(logo, t.accent, 10);
                p.icon("music", ui::center(logo, 22, 22), white);
            }
            // Scanned, expired or failed: the code fades under the state.
            float veil = ui.anim(ui.id("qr-veil"), !qr_error_.empty() || state == QrState::scanned || state == QrState::expired ? 1 : 0, 240);
            if (veil > 0) {
                p.rect(qr, white.with_alpha(0.9f * veil), 14);
                Fade f(p, veil);
                if (!qr_error_.empty()) {
                    p.icon("circle-x", ui::center(qr, 48, 48), ink.with_alpha(0.7f));
                } else if (state == QrState::scanned && !qr_status_->avatar.empty()) {
                    cover(ui, ui::center(qr, 72, 72), qr_status_->avatar, 36, "user");
                } else {
                    p.icon(state == QrState::expired ? "rotate-ccw" : "circle-check", ui::center(qr, 48, 48), ink.with_alpha(0.7f));
                }
            }
        } else if (qr_error_.empty()) {
            ui.spinner(ui::center(qr, 28, 28), ui::Color{0.07f, 0.07f, 0.08f, 0.6f}, 2.5f);
        }

        float sy = qr.bottom() + 30;
        TextStyle st{.size = t.fs(14), .color = t.text};
        std::string line1, line2;
        bool spin = false;
        if (!qr_error_.empty()) {
            line1 = qr_status_ ? lyren::tr("Sign-in incomplete") : lyren::tr("Could not get a QR code"), line2 = qr_error_;
        } else if (qr_.empty() && !qr_image_) {
            line1 = lyren::tr("Generating QR code"), spin = true;
        } else if (state == QrState::waiting) {
            line1 = lyren::tr("Waiting for scan"), line2 = lyren::tr("Scan, then confirm on your phone"), spin = true;
        } else if (state == QrState::scanned) {
            line1 = lyren::tr("Scanned"),
            line2 = qr_status_->nickname.empty() ? lyren::tr("Confirm sign-in on your phone") : lyren::trf("{}, confirm sign-in on your phone", qr_status_->nickname), spin = true;
        } else if (state == QrState::expired) {
            line1 = lyren::tr("QR code expired"), line2 = lyren::tr("Refresh to scan again");
        } else {
            line1 = lyren::tr("Signing in"), spin = true;
        }
        float tw = p.measure(line1, st);
        float sx = r.x + (r.w - tw - (spin ? 26 : 0)) / 2;
        if (spin) {
            ui.spinner({sx, sy + 2, 16, 16}, t.accent, 2);
        }
        ui.label({sx + (spin ? 26 : 0), sy, tw + 1, 20}, line1, st);
        ui.label({r.x, sy + 30, r.w, 20}, line2, {.size = t.fs(12), .color = t.text_dim}, Align::center);
        float refresh_w = button_width(ui, lyren::tr("Refresh QR code"), true);
        if (ui.button(ui.id("qr-refresh"), {r.x + (r.w - refresh_w) / 2, r.bottom() - 36, refresh_w, 34}, lyren::tr("Refresh QR code"), ui::ButtonKind::secondary, "refresh-cw")) {
            spawn(qr_login());
        }
    }

    void Shell::categories_modal(ui::Ui &ui, Rect r) {
        auto &lib = library();
        const auto &t = ui.theme();
        ui.resize_modal(std::min(680.f, ui.width - 40), std::min(620.f, ui.height - 40));
        ui.label({r.x, r.y, r.w - 40, 30}, lyren::tr("All categories"), {.size = t.fs(20), .color = t.text});
        const Rect viewport{r.x, r.y + 52, r.w, r.h - 52};
        const Category *picked = nullptr;
        auto layout = [&](float y, bool draw) {
            for (const auto &[heading, categories] : lib.categories.groups) {
                if (draw) {
                    ui.label({r.x, y, r.w, 22}, heading, {.size = t.fs(13), .color = t.text_dim});
                }
                y += 30;
                float x = r.x;
                for (const auto &category : categories) {
                    const float w = std::min(chip_w(ui, category.name), r.w - 8);
                    if (x + w > r.right() - 8) {
                        x = r.x;
                        y += 38;
                    }
                    if (draw) {
                        ui.push_id(ui.id(category.id));
                        if (chip(ui, x, y, category.name, category.id == lib.category.id)) {
                            picked = &category;
                        }
                        ui.pop_id();
                    }
                    x += w + 8;
                }
                y += 54;
            }
            return y;
        };
        ui.push_id(ui.id("all-categories"));
        const float offset = ui.begin_scroll(ui.id("scroll"), viewport, layout(0, false));
        layout(viewport.y - offset, true);
        ui.end_scroll();
        ui.pop_id();
        if (picked) {
            set_category(ui, *picked);
            ui.close_modal();
        }
    }

    void Shell::open_editor(ui::Ui &ui, Card card, std::optional<Track> track) {
        edit_name_ = card.title;
        edit_hidden_ = false;
        edit_focus_ = true;
        edit_track_ = std::move(track);
        edit_source_ = card.id.empty() ? edit_track_ ? edit_track_->source : active_ : card.source;
        ui.open_modal(400, card.id.empty() ? 286 : 216, [this, card](ui::Ui &u, Rect r) {
            editor_modal(u, r, card);
        });
    }

    void Shell::editor_modal(ui::Ui &ui, Rect r, Card card) {
        const auto &t = ui.theme();
        bool create = card.id.empty();
        ui.label({r.x, r.y, r.w - 40, 30}, create ? lyren::tr("New playlist") : lyren::tr("Rename playlist"), {.size = t.fs(20), .color = t.text});
        if (create) {
            ui.label({r.x, r.y + 30, r.w - 40, 18}, lyren::tr(platform(edit_source_).name), {.size = t.fs(12), .color = t.text_dim});
        }
        ui::Id field = ui.id("playlist-name");
        if (std::exchange(edit_focus_, false)) {
            ui.focus = field;
        }
        bool submit = ui.text_box(field, {r.x, r.y + 52, r.w, 38}, edit_name_, lyren::tr("Playlist name"));
        if (create) {
            float y = r.y + 110;
            ui.label({r.x, y, r.w - 60, 20}, lyren::tr("Private playlist"), {.size = t.fs(14), .color = t.text});
            ui.label({r.x, y + 22, r.w - 60, 18}, lyren::tr("Only you can see it"), {.size = t.fs(12), .color = t.text_dim});
            ui.toggle(ui.id("playlist-hidden"), {r.right() - 40, y + 9, 40, 22}, edit_hidden_);
        }
        float ok_w = std::max(96.f, button_width(ui, create ? lyren::tr("Create") : lyren::tr("Save changes"), false));
        float cancel_w = std::max(84.f, button_width(ui, lyren::tr("Cancel"), false));
        Rect ok{r.right() - ok_w, r.bottom() - 38, ok_w, 38};
        if (ui.button(ui.id("playlist-cancel"), {ok.x - cancel_w - 8, ok.y, cancel_w, 38}, lyren::tr("Cancel"), ui::ButtonKind::ghost)) {
            ui.close_modal();
        }
        submit |= ui.button(ui.id("playlist-save"), ok, create ? lyren::tr("Create") : lyren::tr("Save changes"), ui::ButtonKind::primary);
        std::string name = edit_name_;
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        if (!submit || name.empty()) {
            return;
        }
        ui.close_modal();
        if (create) {
            spawn([](Shell *self, Source source, std::string name, bool hidden, std::optional<Track> track) -> Task<void> {
                auto r = co_await self->provider(source).create_playlist(name, hidden);
                if (!r) {
                    co_return self->notify(lyren::trf("Could not create playlist: {}", r.error()), "triangle-alert", true);
                }
                auto &playlists = self->library(source).playlists;
                playlists.insert(playlists.begin(), *r);
                self->notify(lyren::trf("Created playlist {}", name), "circle-check");
                if (track) {
                    self->add_to_playlist(std::move(*track), *r);
                } else if (self->provider(source).reload_after_edit) {
                    co_await self->load_library(source);
                }
            }(this, edit_source_, name, edit_hidden_, edit_track_));
            return;
        }
        if (name == card.title) {
            return;
        }
        retitle(card, name);
        spawn([](Shell *self, Card card, std::string name) -> Task<void> {
            auto r = co_await self->provider(card).rename_playlist(card.id, name);
            if (r) {
                if (self->provider(card).reload_after_edit) {
                    co_await self->load_library(card.source);
                }
                co_return;
            }
            self->retitle(card, card.title);
            self->notify(lyren::trf("Could not rename: {}", r.error()), "triangle-alert", true);
        }(this, std::move(card), std::move(name)));
    }

    void Shell::confirm_delete(ui::Ui &ui, Card card) {
        ui.open_modal(400, 196, [this, card](ui::Ui &u, Rect r) {
            const auto &t = u.theme();
            u.label({r.x, r.y, r.w - 40, 30}, lyren::tr("Delete playlist"), {.size = t.fs(20), .color = t.text});
            u.label({r.x, r.y + 48, r.w, 20}, lyren::trf("{} will be permanently deleted.", card.title), {.size = t.fs(13), .color = t.text_dim});
            float ok_w = std::max(96.f, button_width(u, lyren::tr("Delete"), true)), cancel_w = std::max(84.f, button_width(u, lyren::tr("Cancel"), false));
            Rect ok{r.right() - ok_w, r.bottom() - 38, ok_w, 38};
            if (u.button(u.id("delete-cancel"), {ok.x - cancel_w - 8, ok.y, cancel_w, 38}, lyren::tr("Cancel"), ui::ButtonKind::ghost)) {
                u.close_modal();
            }
            if (!u.button(u.id("delete-ok"), ok, lyren::tr("Delete"), ui::ButtonKind::primary, "trash")) {
                return;
            }
            u.close_modal();
            auto &playlists = library(card.source).playlists;
            auto it = std::ranges::find_if(playlists, [&](const Card &c) {
                return same(c, card);
            });
            size_t at = static_cast<size_t>(it - playlists.begin());
            if (it != playlists.end()) {
                playlists.erase(it);
            }
            if (route_.page == Page::playlist && same(route_.card, card)) {
                back(u);
            }
            spawn([](Shell *self, Card card, size_t at) -> Task<void> {
                auto r = co_await self->provider(card).delete_playlist(card.id);
                if (r) {
                    co_return self->notify(lyren::trf("Deleted playlist {}", card.title), "trash");
                }
                auto &playlists = self->library(card.source).playlists;
                playlists.insert(playlists.begin() + std::min(at, playlists.size()), card);
                self->notify(lyren::trf("Could not delete playlist: {}", r.error()), "triangle-alert", true);
            }(this, card, at));
        });
    }

    void Shell::picker_modal(ui::Ui &ui, Rect r, Track t) {
        auto &p = ui.painter();
        const auto &th = ui.theme();
        ui.label({r.x, r.y, r.w - 40, 30}, lyren::tr("Add to playlist"), {.size = th.fs(20), .color = th.text});
        ui.label({r.x, r.y + 32, r.w - 40, 20}, t.title + " · " + t.artist, {.size = th.fs(13), .color = th.text_dim});
        std::vector<const Card *> own;
        for (const Card &c : library(t.source).playlists) {
            if (c.owned && provider(c).logged_in()) {
                own.push_back(&c);
            }
        }
        constexpr float row_h = 56, top = 68;
        float list_h = static_cast<float>(own.size() + 1) * row_h;
        ui.resize_modal(420, std::min(ui.height - 120, top + list_h + 48));
        Rect view{r.x - 8, r.y + top, r.w + 24, r.h - top};
        float off = ui.begin_scroll(ui.id("picker"), view, list_h);
        auto entry = [&](size_t i, std::string_view title, std::string_view sub, const std::string *art) {
            Rect e{view.x, view.y + static_cast<float>(i) * row_h - off, r.w + 16, row_h};
            ui::Id id = ui.id(static_cast<uint64_t>(i));
            auto it = ui.interact(id, e);
            float hot = ui.anim(id, it.hovered ? 1 : 0, 140);
            if (hot > 0) {
                p.rect(e.inset(2), th.hover.with_alpha(hot), th.radius_s());
            }
            Rect cov{e.x + 10, e.y + 8, 40, 40};
            if (art) {
                cover(ui, cov, *art, th.radius_s(), "list-music");
            } else {
                p.rect(cov, th.text.with_alpha(0.07f), th.radius_s());
                p.icon("plus", ui::center(cov, 18, 18), th.text_dim);
            }
            p.text(cov.right() + 14, e.y + (sub.empty() ? 34 : 25), title, {.size = th.fs(14), .color = th.text}, e.w - 80);
            if (!sub.empty()) {
                p.text(cov.right() + 14, e.y + 43, sub, {.size = th.fs(12), .color = th.text_dim}, e.w - 80);
            }
            return it.clicked;
        };
        std::optional<Card> picked;
        ui.push_id(ui.id("picker-rows"));
        bool create = entry(0, lyren::tr("New playlist"), "", nullptr);
        for (size_t i = 0; i < own.size(); ++i) {
            if (entry(i + 1, own[i]->title, card_subtitle(*own[i]), &own[i]->cover)) {
                picked = *own[i];
            }
        }
        ui.pop_id();
        ui.end_scroll();
        if (create) {
            open_editor(ui, {}, t);
        }
        if (picked) {
            ui.close_modal();
            add_to_playlist(std::move(t), std::move(*picked));
        }
    }

} // namespace lyren
