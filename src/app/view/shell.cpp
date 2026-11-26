#include "app/view/shell.hpp"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>

#include "app/view/draw.hpp"
#include "core/i18n.hpp"

namespace lyren {

    using ui::Rect;
    namespace ease = ui::ease;

    Shell::Shell(ShellDeps d, SavedState saved)
    : env{.loop = d.loop,
          .http = d.http,
          .providers = d.providers,
          .config = d.config,
          .redraw = std::move(d.redraw),
          .now = std::move(d.now),
          .prefetch_image =
              [&images = d.images](std::string_view url, uint32_t px, bool logical) {
                  images.prefetch(url, logical ? static_cast<uint32_t>(Images::tier(static_cast<float>(px) * images.scale())) : px);
              }},
      images(d.images),
      playback(env, d.player, d.silent, saved),
      library(env),
      social(env, library),
      comments(env, library),
      browser(env, library, social, saved.source),
      accounts(env, library, browser),
      chrome(*this),
      pages(*this),
      tracks(*this),
      dialogs(*this),
      account(*this),
      settings(*this),
      profile(*this),
      feed(*this),
      panel(*this),
      comments_view(*this),
      bar(*this),
      stage(*this, saved),
      desktop(*this, saved),
      set_settings_(std::move(d.set_settings)),
      open_config_(std::move(d.open_config)),
      desktop_changed_(std::move(d.desktop_changed)) {
        set_language(env.config.interface.language);
        if (saved.width > 0) {
            window_ = std::pair{saved.width, saved.height};
        }
        playback.on_change = [this] {
            changed();
        };
        // A radio has no queue to walk: its platform says what comes next.
        playback.on_radio_skip = [this](Source source) {
            if (library.advance_radio(source)) {
                const auto &shelf = library.shelf(source);
                playback.play_radio(source, shelf.radio, shelf.radio_pos);
            } else {
                playback.stop();
            }
        };
        playback.on_cloud_play = [this](const Track &track, bool settled) {
            library.note_cloud_play(track, settled);
        };
        browser.on_change = [this] {
            changed();
        };
        library.subscribe([this](const Library::Event &event) {
            if (event.kind == Library::Event::Kind::left) {
                playback.forget(event.source);
            } else if (event.kind == Library::Event::Kind::radio_ready && playback.radio(event.source)) {
                const auto &shelf = library.shelf(event.source);
                if (shelf.radio_pos >= 0 && shelf.radio_pos < static_cast<int>(shelf.radio.size())) {
                    playback.play_radio(event.source, shelf.radio, shelf.radio_pos);
                }
            }
        });
        playback.connect(d.bus, std::move(d.raise));
    }

    void Shell::changed() {
        env.loop.cancel(save_timer_);
        save_timer_ = env.loop.after(2s, [this] {
            save_timer_ = 0;
            save();
        });
    }

    void Shell::save(std::optional<std::pair<int, int>> window, bool flush_accounts) {
        if (window) {
            window_ = window;
        }
        SavedState state{.source = browser.active()};
        if (window_) {
            state.width = window_->first, state.height = window_->second;
        }
        playback.save(state);
        stage.save(state);
        desktop.save(state);
        save_state(state);
        if (flush_accounts) {
            accounts.flush();
        }
    }

    void Shell::config_changed() {
        playback.config_changed();
    }

    ui::Color Shell::accent(double now, bool dark, bool &animating) {
        // -2: not known yet (cover still loading), -1: a colourless cover.
        float hue = 0.98f;
        if (const Track *track = playback.current()) {
            const auto &art = images.get(track->cover, static_cast<uint32_t>(Images::tier(stage.cover_px())));
            hue = art.ready ? art.hue : art.failed ? -1 : -2;
        }
        auto color = [](float h, bool on_dark) {
            if (h == -1) {
                return on_dark ? ui::Color{0.86f, 0.86f, 0.88f, 1} : ui::Color{0.29f, 0.33f, 0.40f, 1};
            }
            return ui::accent_for_hue(h, on_dark);
        };
        const double duration = 0.8 * env.config.theme.motion;
        auto progress = [&] {
            return duration > 0 ? ease::standard(std::clamp(static_cast<float>((now - accent_time_) / duration), 0.f, 1.f)) : 1.f;
        };
        if (accent_hue_ == -2 && hue == -2) {
            hue = 0.98f;
        }
        if (hue != -2 && hue != accent_hue_) {
            const bool first = accent_hue_ == -2;
            for (bool on_dark : {false, true}) {
                auto &from = accent_from_[on_dark];
                from = first ? color(hue, on_dark) : from.mix(color(accent_hue_, on_dark), progress());
            }
            accent_hue_ = hue;
            accent_time_ = first ? -10 : now;
        }
        const float t = progress();
        if (t < 1) {
            animating = true;
        }
        return accent_from_[dark].mix(color(accent_hue_, dark), t);
    }

    void Shell::back() {
        if (stage.open()) {
            stage.show(false);
        } else {
            browser.back();
        }
    }

    void Shell::forward() {
        if (!stage.open()) {
            browser.forward();
        }
    }

    void Shell::open_comments(Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows) {
        comments.close_replies();
        comments.open(source, subject, std::move(id), std::move(title), std::move(cover), follows);
        panel.show(SidePanel::Kind::comments);
    }

    void Shell::sync_stage_comments() {
        const auto &thread = comments.thread();
        if (const Track *track = playback.current(); track && stage.open() && env.provider(*track).capabilities.comments[0] &&
                                                     !(panel.kind() == SidePanel::Kind::comments && !thread.follows) &&
                                                     !(thread.follows && thread.is(track->source, Subject::song, track->id))) {
            comments.open(track->source, Subject::song, track->id, track->title + " · " + track->artist, track->cover, true);
        }
    }

    void Shell::draw(ui::Ui &ui, Rect area, const ui::Theme &stage_theme) {
        stage_theme_ = &stage_theme;
        const auto &t = ui.theme();
        for (auto &notice : env.notices) {
            ui.toast(std::move(notice.text), std::move(notice.icon), notice.error);
        }
        env.notices.clear();
        // A closed account dialog cannot install a late sign-in.
        if (accounts.sign_in().source && !ui.modal_open()) {
            accounts.cancel();
            images.erase("qr");
        }
        // Going anywhere leaves the stage.
        if (const uint64_t visit = browser.navigations(); visit != visit_) {
            visit_ = visit;
            stage.show(false);
        }

        const Rect strip = ui::cut_bottom(area, PlayerBar::height);
        // A small dead band keeps resizing near the breakpoint from repeatedly folding the sidebar.
        if (area.w < 888) {
            compact_ = true;
        } else if (area.w > 912) {
            compact_ = false;
        }
        const Rect side = ui::cut_left(area, ui.spring(ui.id("side-w"), compact_ ? 64 : 220, 280));
        ui::cut_right(area, 8);
        sync_stage_comments();
        // The panel beside the page slides in from the right.
        const float panel_w = ui.spring(ui.id("panel-w"), SidePanel::width(panel.kind()), 320);
        const Rect slot = panel_w > 0.5f ? ui::cut_right(area, panel_w) : Rect{};

        // The now-playing stage comes up over the whole window; once it covers it the sidebar and page are not drawn.
        const float up = stage.raised(ui);
        if (!(stage.open() && up >= 1)) {
            page(ui, area);
            chrome.sidebar(ui, side);
        }
        const Rect stage_page{side.x + 8, area.y, area.right() - side.x - 8, area.h};
        const float wide = stage.fit(ui, stage_page);
        if (up > 0) {
            UseTheme on_stage(ui, stage_theme);
            stage.draw(ui, {0, 0, ui.width, ui.height}, stage_page, up, wide);
        }
        {
            // The panel and the player take the stage's colours as it comes up.
            const ui::Theme over = t.mix(stage_theme, up);
            UseTheme use(ui, over);
            if (slot.w > 0) {
                panel.draw(ui, slot, up);
            }
            bar.draw(ui, strip, up);
            stage.art(ui, strip, stage_page, up, wide);
        }
        keys(ui);
        images.end_frame();
    }

    void Shell::page(ui::Ui &ui, Rect area) {
        auto &p = ui.painter();
        const auto &t = ui.theme();
        const Page on = browser.route().page;
        // The page is an architectural sheet with a whisper wash of its own colour at the top.
        p.rect(area, t.surface, t.radius_l());
        const float enter = ui.progress(browser.entered, ui::pace::glide);
        if (on != Page::search && on != Page::settings && on != Page::profile && on != Page::recommend) {
            const float hue = pages.hue(ui);
            const ui::Color tint = hue >= 0 ? ui::hsl(hue, 0.5f, 0.5f) : t.accent;
            p.gradient({area.x, area.y, area.w, 240}, tint.with_alpha((t.dark ? 0.10f : 0.055f) * enter), tint.with_alpha(0), t.radius_l());
        }
        ui.push_clip(area);
        {
            Fade fade(p, enter);
            Rect r = area;
            r.x += (1 - enter) * 12 * static_cast<float>(browser.direction);
            if (on == Page::profile) {
                profile.draw(ui, r);
            } else if (on == Page::settings) {
                settings.draw(ui, r);
            } else {
                pages.draw(ui, r);
            }
        }
        ui.pop_clip();
    }

    void Shell::keys(ui::Ui &ui) {
        if (ui.modal_open()) {
            return;
        }
        for (auto &action : ui.input().actions) {
            if (action.press && !action.consumed && (action.button == BTN_SIDE || action.button == BTN_EXTRA)) {
                action.consumed = true;
                if (action.button == BTN_SIDE) {
                    back();
                } else {
                    forward();
                }
                ui.redraw();
            }
        }
        auto &pressed = ui.input().keys;
        for (auto key = pressed.begin(); key != pressed.end();) {
            const bool ctrl = key->mods & wl::mod_ctrl, alt = key->mods & wl::mod_alt, plain = !(key->mods & (wl::mod_ctrl | wl::mod_alt));
            const bool escape = key->keysym == XKB_KEY_Escape && !ui.menu_open();
            bool used = true;
            if (key->keysym == XKB_KEY_space && !key->repeat) {
                playback.toggle();
            } else if (escape && panel.kind() == SidePanel::Kind::comments) {
                panel.show(SidePanel::Kind::none);
            } else if (escape && stage.open()) {
                stage.show(false);
            } else if (escape && browser.selection().choosing) {
                browser.reset_selection();
            } else if (ctrl && (key->keysym == XKB_KEY_a || key->keysym == XKB_KEY_A) && browser.selection().list && !ui.focus && !ui.menu_open() && !stage.open() &&
                       env.provider(browser.active()).capabilities.playlist_edit) {
                browser.choose_all();
            } else if ((ctrl && (key->keysym == XKB_KEY_f || key->keysym == XKB_KEY_F || key->keysym == XKB_KEY_k || key->keysym == XKB_KEY_K)) ||
                       (plain && key->keysym == XKB_KEY_slash && !ui.focus && !ui.menu_open() && !stage.open())) {
                ui.focus = ui.id("search");
            } else if (ctrl && key->keysym == XKB_KEY_Right) {
                playback.skip(1);
            } else if (ctrl && key->keysym == XKB_KEY_Left) {
                playback.skip(-1);
            } else if (alt && key->keysym == XKB_KEY_Left) {
                back();
            } else if (alt && key->keysym == XKB_KEY_Right) {
                forward();
            } else {
                used = false;
            }
            if (used) {
                key = pressed.erase(key);
                ui.redraw();
            } else {
                ++key;
            }
        }
    }

} // namespace lyren
