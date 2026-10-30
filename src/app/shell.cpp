#include "app/shell.hpp"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <inplace_vector>
#include <numbers>

#include "app/draw.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Align;
    using ui::hsl;
    using ui::Rect;
    using ui::TextStyle;
    using State = audio::Player::State;
    namespace ease = ui::ease;

    constexpr ui::Color white{1, 1, 1, 1};

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
        images_.prefetch(queue_);
        dl_x_ = saved.lyrics_x, dl_bottom_ = saved.lyrics_bottom, dl_locked_ = saved.lyrics_locked;
        island_x_ = saved.island_x, island_y_ = saved.island_y;
        dl_enabled_ = saved.lyrics_enabled;
        dl_island_ = saved.lyrics_island;
        lyric_translation_ = saved.lyrics_translation;
        desktop_changed_ = std::move(d.desktop_changed);
        rng_ ^= static_cast<uint64_t>(now);

        player_.on_change = [this] {
            on_player();
        };
        apply_volume();
        for (const auto &entry : platforms) {
            provider(entry.source).on_session_changed([this, source = entry.source] {
                const auto &lib = library(source);
                if (!provider(source).logged_in() && (lib.account || lib.preference_gen != 0)) {
                    if (lib.account) {
                        notify(std::format("{}: {}", lyren::tr(platform(source).name), lyren::tr("Session expired; scan again to sign in")), "user", true);
                    }
                    reset_account(source);
                    if (source == active_) {
                        ensure_route_data();
                    }
                }
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
                     .island_x = island_x_,
                     .island_y = island_y_,
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

    bool Shell::supports(Page page, Source source) const {
        const auto &features = provider(source).features;
        switch (page) {
            case Page::daily:
                return bool(features.daily.songs);
            case Page::radar:
                return bool(features.radar);
            case Page::cloud:
                return bool(features.cloud);
            case Page::dislikes:
                return bool(features.exclusions.list);
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
                route_.card = {.source = active_, .title = "Daily mix", .subtitle = lyren::trf("{} · Made for your taste", date_label())};
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
                        if (route_.artist) {
                            artist_page(ui, r);
                        } else {
                            page_status(ui, r, true, "", false);
                        }
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
                                                 {Page::fm, "radio", lyren::tr(provider().radio_title)},
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

} // namespace lyren
