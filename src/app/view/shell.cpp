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
              },
          .store = d.store},
      images(d.images),
      pins(env, d.silent),
      playback(env, d.player, d.silent, saved),
      library(env),
      social(env, library),
      comments(env, library),
      browser(env, library, social, saved.source, std::move(saved.source_instance)),
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
        playback.connect(library);
        playback.connect(pins);
        browser.on_change = [this] {
            changed();
        };
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
        SavedStateView state{.source = browser.active(), .source_instance = browser.active_key().instance};
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

    bool Shell::comments_open(Source source, Subject subject, std::string_view id, bool follows) const {
        const auto &thread = comments.thread();
        return panel.kind() == SidePanel::Kind::comments && thread.follows == follows && thread.is(source, subject, id);
    }

    void Shell::toggle_comments(Source source, Subject subject, std::string id, std::string title, std::string cover, bool follows) {
        if (comments_open(source, subject, id, follows)) {
            panel.show(SidePanel::Kind::none);
        } else {
            open_comments(source, subject, std::move(id), std::move(title), std::move(cover), follows);
        }
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
        if (pending_copy_data) {
            ui.copy_data = std::move(pending_copy_data);
            pending_copy_data.reset();
        }
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
        tracks.begin_frame();
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
        if (on != Page::search && on != Page::settings && on != Page::profile && on != Page::recommend && on != Page::stats) {
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

    void Shell::show_shortcuts(ui::Ui &ui) {
        ui.open_modal(560, 460, [](ui::Ui &u, Rect r) {
            auto &p = u.painter();
            const auto &t = u.theme();
            struct Entry {
                std::string_view key, label;
            };
            struct Group {
                std::string_view title;
                std::span<const Entry> items;
            };
            static constexpr std::array playback_keys{
                Entry{"Space", "Play / Pause"}, Entry{"← / →", "Seek ±5s"}, Entry{"Ctrl+← / →", "Previous / Next"}, Entry{"↑ / ↓", "Volume (or move list row)"},
                Entry{"M", "Mute / Unmute"},
            };
            static constexpr std::array view_keys{
                Entry{"L", "Now playing"}, Entry{"Q", "Queue"}, Entry{"C", "Comments"}, Entry{"D", "Desktop lyrics"}, Entry{"Esc", "Close / Cancel"},
            };
            static constexpr std::array nav_keys{
                Entry{"/ · Ctrl+F", "Search music"}, Entry{"Alt+← / →", "Back / Forward"}, Entry{"Enter", "Play selected song"},
                Entry{"Ctrl+A", "Select all songs"}, Entry{"?", "Keyboard shortcuts"},     Entry{"Ctrl+Q", "Quit"},
            };
            static constexpr std::array<Group, 3> groups{{
                {"Playback", playback_keys},
                {"View", view_keys},
                {"Navigation", nav_keys},
            }};
            dialog_title(u, r, lyren::tr("Keyboard shortcuts"));
            p.rect({r.x, r.y + 40, r.w, 1}, t.line(0.8f));
            const Rect view{r.x, r.y + 48, r.w, std::max(0.f, r.h - 48)};
            constexpr float group_h = 24, row_h = 30, gap = 14, cap_h = 22;
            float total_h = 0;
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (gi > 0) {
                    total_h += gap;
                }
                total_h += group_h + static_cast<float>(groups[gi].items.size()) * row_h;
            }
            u.resize_modal(std::min(560.f, u.width - 40), std::min(u.height - 64, total_h + 100));
            const bool scrolls = total_h + 4 > view.h;
            const float content_w = view.w - (scrolls ? 10.f : 0.f);
            const float off = u.begin_scroll(u.id("shortcuts-scroll"), view, total_h + 4);
            float y = view.y - off;
            const auto head_st = t.style(ui::type::caption, t.text_dim, text::semibold);
            const auto desc_st = t.style(ui::type::label, t.text);
            const auto key_st = t.style(ui::type::micro, t.text.with_alpha(0.84f), text::medium);
            const auto sep_st = t.style(ui::type::micro, t.faint());
            const float cap_r = std::min(5.f, t.radius_s());
            constexpr std::string_view sep_slash = " / ", sep_dot = " · ";
            auto for_each_token = [&](std::string_view spec, auto &&fn) {
                size_t pos = 0;
                while (pos < spec.size()) {
                    if (spec.substr(pos).starts_with(sep_slash)) {
                        fn(std::string_view{"/"}, true);
                        pos += sep_slash.size();
                    } else if (spec.substr(pos).starts_with(sep_dot)) {
                        fn(std::string_view{"·"}, true);
                        pos += sep_dot.size();
                    } else if (spec[pos] == '+') {
                        fn(std::string_view{"+"}, true);
                        ++pos;
                    } else {
                        size_t end = pos;
                        while (end < spec.size() && spec[end] != '+' && !spec.substr(end).starts_with(sep_slash) && !spec.substr(end).starts_with(sep_dot)) {
                            ++end;
                        }
                        fn(spec.substr(pos, end - pos), false);
                        pos = end;
                    }
                }
            };
            auto token_w = [&](std::string_view tok, bool sep) {
                if (sep) {
                    return std::ceil(p.measure(tok, sep_st)) + (tok == "+" ? 6.f : 10.f);
                }
                return std::max(24.f, std::ceil(p.measure(tok, key_st)) + 12.f);
            };
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (gi > 0) {
                    y += gap;
                }
                u.label({view.x, y, content_w, group_h}, lyren::tr(groups[gi].title), head_st);
                y += group_h;
                for (size_t i = 0; i < groups[gi].items.size(); ++i) {
                    const auto &item = groups[gi].items[i];
                    float keys_w = 0;
                    for_each_token(item.key, [&](std::string_view tok, bool sep) {
                        keys_w += token_w(tok, sep);
                    });
                    u.label({view.x, y, std::max(0.f, content_w - keys_w - 16), row_h}, lyren::tr(item.label), desc_st);
                    float kx = view.x + content_w - keys_w;
                    for_each_token(item.key, [&](std::string_view tok, bool sep) {
                        const float w = token_w(tok, sep);
                        if (sep) {
                            u.label({kx, y, w, row_h}, tok, sep_st, ui::Align::center);
                        } else {
                            const Rect cap{kx, y + (row_h - cap_h) / 2, w, cap_h};
                            p.rect(cap, t.fill(1.1f), cap_r, t.surface_edge(1.8f), 1);
                            u.label(cap, tok, key_st, ui::Align::center);
                        }
                        kx += w;
                    });
                    if (i + 1 < groups[gi].items.size()) {
                        p.rect({view.x, y + row_h - 1, content_w, 1}, t.line(0.5f));
                    }
                    y += row_h;
                }
            }
            u.end_scroll();
        });
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
            const bool no_input = !ui.focus && !ui.menu_open();
            const bool escape = key->keysym == XKB_KEY_Escape && !ui.menu_open();
            const bool up_key = key->keysym == XKB_KEY_Up || key->keysym == XKB_KEY_KP_Up;
            const bool down_key = key->keysym == XKB_KEY_Down || key->keysym == XKB_KEY_KP_Down;
            const bool enter_key = key->keysym == XKB_KEY_Return || key->keysym == XKB_KEY_KP_Enter;
            bool used = true;
            if (plain && key->keysym == XKB_KEY_space && !key->repeat && no_input) {
                playback.toggle();
            } else if (escape && panel.kind() == SidePanel::Kind::comments) {
                panel.show(SidePanel::Kind::none);
            } else if (escape && stage.open()) {
                stage.show(false);
            } else if (escape && browser.selection().choosing) {
                browser.reset_selection();
            } else if (escape && browser.selection().selected >= 0) {
                browser.selection().selected = -1;
            } else if (ctrl && (key->keysym == XKB_KEY_a || key->keysym == XKB_KEY_A) && tracks.has_list() && no_input && !stage.open()) {
                tracks.select_all();
            } else if ((ctrl && (key->keysym == XKB_KEY_f || key->keysym == XKB_KEY_F || key->keysym == XKB_KEY_k || key->keysym == XKB_KEY_K)) ||
                       (plain && key->keysym == XKB_KEY_slash && no_input && !stage.open())) {
                ui.focus = ui.id("search");
            } else if (ctrl && key->keysym == XKB_KEY_Right) {
                playback.skip(1);
            } else if (ctrl && key->keysym == XKB_KEY_Left) {
                playback.skip(-1);
            } else if (alt && key->keysym == XKB_KEY_Left) {
                back();
            } else if (alt && key->keysym == XKB_KEY_Right) {
                forward();
            } else if (plain && no_input && (key->keysym == XKB_KEY_Left || key->keysym == XKB_KEY_Right)) {
                if (playback.current()) {
                    const auto tl = playback.timeline();
                    if (const double dur = tl.duration(); dur > 0) {
                        const auto [lo, hi] = tl.playable_span();
                        const double delta = key->keysym == XKB_KEY_Right ? 5.0 : -5.0;
                        playback.seek(std::clamp(playback.song_time() + delta, lo * dur, hi * dur));
                    }
                }
            } else if (plain && no_input && (up_key || down_key)) {
                if (!stage.open() && tracks.has_selected()) {
                    tracks.move_selection(ui, up_key ? -1 : 1);
                } else {
                    playback.set_volume(playback.volume() + (up_key ? 0.05f : -0.05f));
                }
            } else if (plain && no_input && enter_key && !key->repeat && !stage.open() && tracks.has_selected()) {
                tracks.play_selected();
            } else if (plain && no_input && !key->repeat && (key->keysym == XKB_KEY_m || key->keysym == XKB_KEY_M)) {
                playback.toggle_mute();
            } else if (plain && no_input && !key->repeat && (key->keysym == XKB_KEY_l || key->keysym == XKB_KEY_L)) {
                stage.show(!stage.open());
            } else if (plain && no_input && !key->repeat && (key->keysym == XKB_KEY_q || key->keysym == XKB_KEY_Q)) {
                panel.toggle(SidePanel::Kind::queue);
            } else if (plain && no_input && !key->repeat && (key->keysym == XKB_KEY_c || key->keysym == XKB_KEY_C)) {
                if (panel.kind() == SidePanel::Kind::comments) {
                    panel.show(SidePanel::Kind::none);
                } else if (const Track *track = playback.current(); track && env.provider(*track).capabilities.comments[0]) {
                    open_comments(track->source, Subject::song, track->id, track->title + " · " + track->artist, track->cover, stage.open());
                } else if (!stage.open() && browser.route().page == Page::album && browser.route().list && browser.route().list->loaded &&
                           env.provider(browser.route().card).capabilities.comments[1]) {
                    const auto &card = browser.route().card;
                    open_comments(card.source, Subject::album, browser.route().list->album.thread, card.title, card.cover, false);
                } else if (!stage.open() && browser.route().page == Page::playlist && browser.route().card.commentable &&
                           env.provider(browser.route().card).capabilities.comments[2]) {
                    const auto &card = browser.route().card;
                    open_comments(card.source, Subject::playlist, card.thread.empty() ? card.id : card.thread, card.title, card.cover, false);
                }
            } else if (plain && no_input && !key->repeat && (key->keysym == XKB_KEY_d || key->keysym == XKB_KEY_D)) {
                desktop.set_enabled(!desktop.enabled());
            } else if (plain && no_input && !key->repeat && key->keysym == XKB_KEY_question) {
                show_shortcuts(ui);
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
