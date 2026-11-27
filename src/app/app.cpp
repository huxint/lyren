#include "app/app.hpp"

#include <linux/input-event-codes.h>
#include <malloc.h>
#include <spawn.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <utility>

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "sys/paths.hpp"
#include "wl/clipboard.hpp"
#include "wl/input.hpp"

namespace lyren {

    constexpr float titlebar_h = 48;
    constexpr int shadow_margin = 28, resize_border = 8;

    static wl::Cursor cursor_for(uint32_t edges) {
        switch (edges) {
            case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
                return wl::Cursor::n_resize;
            case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
                return wl::Cursor::s_resize;
            case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
                return wl::Cursor::w_resize;
            case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
                return wl::Cursor::e_resize;
            case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
                return wl::Cursor::nw_resize;
            case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
                return wl::Cursor::ne_resize;
            case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
                return wl::Cursor::sw_resize;
            default:
                return wl::Cursor::se_resize;
        }
    }

    // Resize edges for a point in the band outside the w×h geometry; corners take 16 px along each side.
    static uint32_t edges_at(float x, float y, float w, float h) {
        constexpr float corner = 16;
        bool l = x < 0, r = x >= w, t = y < 0, b = y >= h;
        if (t || b) {
            l |= x < corner, r |= x >= w - corner;
        }
        if (l || r) {
            t |= y < corner, b |= y >= h - corner;
        }
        return (t ? XDG_TOPLEVEL_RESIZE_EDGE_TOP : 0) | (b ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM : 0) | (l ? XDG_TOPLEVEL_RESIZE_EDGE_LEFT : 0) |
               (r ? XDG_TOPLEVEL_RESIZE_EDGE_RIGHT : 0);
    }

    // What a frame puts on screen: an identical one need not be presented. Reused texture ids can carry new pixels
    // without changing any primitive, so the textures' version is part of it.
    static uint64_t frame_key(std::span<const gfx::Prim> prims, uint32_t w, uint32_t h, uint64_t textures) {
        return std::hash<std::string_view>{}({reinterpret_cast<const char *>(prims.data()), prims.size_bytes()}) ^ (static_cast<uint64_t>(w) << 32 | h) ^ textures;
    }

    App::App(Loop &loop, Bus &bus, ConfigFile config, std::vector<std::string> script, std::string initial_uri)
    : loop_(loop),
      config_(std::move(config)),
      display_(loop),
      gpu_(display_.display),
      renderer_(gpu_),
      watcher_(loop, config_.path,
               [this] {
                   reload_config();
               }),
      store_(paths::state_dir() / "lyren.db"),
      player_(loop, http_),
      ncm_(loop, http_),
      qq_(loop, http_),
      qishui_(loop, http_),
      kugou_(loop, http_),
      bodian_(loop, http_),
      images_(
          loop, http_, renderer_,
          [this] {
              request_redraw();
          },
          [this] {
              return now();
          }),
      shell_(ShellDeps{.loop = loop,
                       .http = http_,
                       .player = player_,
                       .providers = {ncm_, qq_, qishui_, kugou_, bodian_},
                       .images = images_,
                       .store = &store_,
                       .config = config_.config,
                       .set_settings =
                           [this](std::span<const SettingChange> changes) {
                               Config previous = config_.config;
                               if (auto r = save_settings(config_, changes); !r) {
                                   return toast(r.error());
                               }
                               for (const auto &c : changes) {
                                   log::info("config: {}.{} = {}", c.section, c.key, c.value);
                               }
                               // Theme and font resources can only change after the current frame finishes.
                               loop_.defer([this, previous] {
                                   config_updated(previous);
                               });
                           },
                       .desktop_changed =
                           [this] {
                               loop_.defer([this] {
                                   sync_desktop_lyrics();
                                   request_redraw();
                               });
                           },
                       .open_config =
                           [this] {
                               if (!script_.empty()) {
                                   return log::info("open config requested");
                               }
                               pid_t pid;
                               std::string path = config_.path.string();
                               std::array<const char *, 3> argv = {"xdg-open", path.c_str(), nullptr};
                               posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char *const *>(argv.data()), environ);
                           },
                       .redraw =
                           [this] {
                               request_redraw();
                           },
                       .now =
                           [this] {
                               return now();
                           },
                       .silent = !script.empty(),
                       .bus = bus,
                       .raise =
                           [this](std::string_view token) {
                               show_window(token);
                           }},
             std::move(saved_)),
      script_(std::move(script)),
      main_(
          loop,
          [this] {
              return window_ && window_->frame_pending();
          },
          [this] {
              render();
          }),
      overlay_(
          loop,
          [this] {
              return lyrics_ && lyrics_->frame_pending();
          },
          [this] {
              render_lyrics();
          }) {
        std::string errors;
        theme_ = ui::Theme::from(config_.config, errors);
        load_text(config_.config.theme.font);
        create_window();
        apply_config(true);
        if (!config_.error.empty()) {
            toast(describe_config_error(config_.error));
        } else if (!errors.empty()) {
            toast(errors.substr(0, errors.find('\n')));
        }
        note_input();
        request_redraw();
        if (!initial_uri.empty()) {
            loop_.defer([this, uri = std::move(initial_uri)] {
                shell_.open_uri(uri);
            });
        }
        if (!script_.empty()) {
            loop_.after(500ms, [this] {
                run_script(0);
            });
        }
    }

    void App::create_window() {
        wl::Window::Callbacks cb{
            .configure =
                [this] {
                    if (!window_) {
                        return;
                    }
                    note_input();
                    main_.frames.request();
                },
            .close =
                [this] {
                    handle_close();
                },
            .frame =
                [this] {
                    main_.frames.frame_done();
                },
            .pointer =
                [this](const wl::PointerEvent &e) {
                    on_pointer(e);
                },
            .key =
                [this](const wl::KeyEvent &e) {
                    on_key(e);
                },
            .focus =
                [this](bool) {
                    request_redraw();
                },
            .text_input =
                [this](const wl::TextInputEvent &e) {
                    on_text_input(e);
                },
        };
        const int w = saved_w_ > 0 ? std::max(saved_w_, 720) : 1180, h = saved_h_ > 0 ? std::max(saved_h_, 480) : 760;
        window_ = std::make_unique<wl::Window>(display_, std::move(cb), w, h);
        window_->set_title("Lyren");
        window_->set_shadow(shadow_margin, resize_border);
        main_.target = renderer_.create_target(display_.display, window_->surface());
        main_.hash = 0;
    }

    void App::handle_close() {
        if (config_.config.interface.close_to_background && script_.empty()) {
            loop_.defer([this] {
                hide_window();
            });
            return;
        }
        loop_.quit();
    }

    void App::hide_window() {
        if (!window_) {
            return;
        }
        if (!window_->maximized()) {
            saved_w_ = window_->width();
            saved_h_ = window_->height();
            shell_.save(std::pair{saved_w_, saved_h_}, false);
        } else {
            shell_.save(std::nullopt, false);
        }
        main_.frames.reset();
        renderer_.destroy_target(main_.target);
        main_.target = nullptr;
        window_.reset();
        main_.input = {};
        main_.hash = 0;
        resize_edges_ = 0;
        display_.input->set_text_input({});
        text_->shaper.release_surface(text::Shaper::Surface::window);
        log::info("window hidden to background");
    }

    void App::show_window(std::string_view token) {
        if (!script_.empty()) {
            return log::info("raise requested ({})", token.empty() ? "no token" : token);
        }
        if (!window_) {
            create_window();
            note_input();
            request_redraw();
            log::info("window restored from background");
        }
        window_->activate(std::string(token));
    }

    App::~App() {
        if (window_) {
            shell_.save(window_->maximized() ? std::nullopt : std::optional{std::pair{window_->width(), window_->height()}}, true);
        } else {
            shell_.save(saved_w_ > 0 && saved_h_ > 0 ? std::optional{std::pair{saved_w_, saved_h_}} : std::nullopt, true);
        }
        if (lyrics_) {
            close_desktop_lyrics();
        }
        if (main_.target) {
            renderer_.destroy_target(main_.target);
        }
        loop_.cancel(idle_timer_);
    }

    void App::load_text(const std::string &family) {
        text_.emplace(renderer_, family);
        main_.ui.invalidate_text_layout();
        overlay_.ui.invalidate_text_layout();
    }

    void App::apply_config(bool initial) {
        set_language(config_.config.interface.language);
        std::string errors;
        theme_ = ui::Theme::from(config_.config, errors);
        if (!initial && !errors.empty()) {
            toast(errors.substr(0, errors.find('\n')));
        }
        if (window_) {
            window_->set_shadow(shadow_margin, resize_border);
        }
        sync_desktop_lyrics();
        request_redraw();
    }

    void App::reload_config() {
        std::ifstream in(config_.path);
        std::string text{std::istreambuf_iterator<char>(in), {}};
        if (text == config_.text) {
            return;
        }
        config_.text = text;
        auto parsed = parse_config(text);
        if (!parsed) {
            toast(describe_config_error(parsed.error()));
            return;
        }
        Config previous = std::exchange(config_.config, *parsed);
        log::info("config reloaded");
        config_updated(previous);
    }

    void App::config_updated(const Config &previous) {
        shell_.config_changed();
        if (previous.theme.font != config_.config.theme.font) {
            load_text(config_.config.theme.font);
        }
        apply_config(false);
        if (previous.gpu.device != config_.config.gpu.device) {
            toast(std::string(tr("GPU changes take effect after restarting")));
        }
    }

    void App::toast(std::string message) {
        log::warn("{}", message);
        main_.ui.toast(std::move(message), "triangle-alert", true);
        request_redraw();
    }

    void App::request_redraw() {
        if (window_) {
            main_.frames.request();
        }
        if (lyrics_) {
            overlay_.frames.request();
        }
    }

    template <class Prepare>
    void App::present(Surface &surf, uint32_t bw, uint32_t bh, bool force, Prepare &&prepare) {
        // Pointer motion over static areas rebuilds an identical frame; skip presenting it.
        auto prims = text_->painter.prims();
        uint64_t hash = frame_key(prims, bw, bh, renderer_.texture_version());
        if (hash != surf.hash || force) {
            renderer_.draw(*surf.target, prims, {}, std::forward<Prepare>(prepare));
            surf.hash = hash;
        }
    }

    // Once input stops for a while, give back font pages and freed heap; glyphs live in the atlas.
    void App::note_input() {
        last_input_ = Clock::now();
        if (idle_timer_) {
            return;
        }
        auto arm = [this](this auto &&self, Clock::duration delay) -> void {
            idle_timer_ = loop_.after(delay, [this, self] {
                idle_timer_ = 0;
                const auto elapsed = Clock::now() - last_input_;
                if (elapsed < 5s) {
                    self(5s - elapsed);
                    return;
                }
                text_->fonts.trim();
                malloc_trim(0);
            });
        };
        arm(5s);
    }

    void App::render() {
        if (!window_ || !main_.frames.begin()) {
            return;
        }
        auto start = Clock::now();
        double t = elapsed(start);
        const uint32_t bw = window_->buffer_width(), bh = window_->buffer_height();
        renderer_.resize(*main_.target, bw, bh);
        main_.input.mods = display_.input->mods();
        text_->shaper.begin_frame(text::Shaper::Surface::window);
        // A full atlas is cleared mid-frame; the second pass redraws with the events already handled.
        for (int pass = 0; pass < 2; ++pass) {
            text_->atlas.begin_frame();
            build(t);
            main_.input.consumed();
            if (!text_->atlas.reset()) {
                break;
            }
        }
        present(main_, bw, bh, window_->configure_pending(), [this] {
            window_->prepare_present();
        });
        after_frame(start);
    }

    void App::build(double now) {
        float m = static_cast<float>(window_->margin()), w = static_cast<float>(window_->width()), h = static_cast<float>(window_->height());
        auto &p = text_->painter;
        auto &ui = main_.ui;
        const float scale = static_cast<float>(window_->scale());
        images_.set_scale(scale);
        p.begin(scale, m, m);
        frame_theme_ = theme_;
        bool accent_moving = false;
        if (theme_.auto_accent) {
            frame_theme_.accent = shell_.accent(now, theme_.dark, accent_moving);
        }
        stage_theme_ = frame_theme_.stage(theme_.auto_accent ? shell_.accent(now, true, accent_moving) : theme_.accent);

        bool floating = window_->client_side() && m > 0;
        float radius = floating ? theme_.radius_l() : 0;
        if (floating) {
            p.shadow({0, 0, w, h}, radius, 22, theme_.shadow, true);
        }
        p.rect({0, 0, w, h}, theme_.bg, radius, floating ? theme_.surface_edge(1.1f) : ui::Color{}, floating ? 1 : 0);

        ui.window_radius = radius;
        ui.toast_bottom = PlayerBar::height + 12;
        ui.begin(p, frame_theme_, w, h, now, main_.input);
        if (accent_moving) {
            ui.keep_animating();
        }
        ui::Rect area{0, 0, w, h};
        ui::Rect bar = ui::cut_top(area, titlebar_h);
        // The title bar takes input before the page but is drawn over it, since the stage reaches under it.
        p.defer(true);
        titlebar(bar, shell_.stage.raised(ui));
        p.defer(false);
        shell_.draw(ui, area, stage_theme_);
        p.draw_deferred();
        ui.end();
        for (auto &a : main_.input.actions) {
            if (a.press && !a.consumed) {
                caption_press(a);
            }
        }
    }

    void App::titlebar(ui::Rect bar, float stage) {
        auto &p = text_->painter;
        auto &ui = main_.ui;
        ui::Theme mixed = frame_theme_.mix(stage_theme_, stage);
        const auto &saved = ui.use_theme(mixed);
        const auto &t = mixed;
        ui::Rect r = bar;
        if (window_->client_side()) {
            ui::Rect ctl = ui::cut_right(r, 3 * 36 + 2 * 4 + 16);
            float x = ctl.x + 4, y = bar.y + (bar.h - 28) / 2;
            if (window_button(ui.id("win-min"), {x, y, 36, 28}, "minus", 16, false)) {
                window_->minimize();
            }
            if (window_button(ui.id("win-max"), {x + 40, y, 36, 28}, window_->maximized() ? "copy" : "square", 14, false)) {
                window_->toggle_maximize();
            }
            if (window_button(ui.id("win-close"), {x + 80, y, 36, 28}, "x", 16, true)) {
                handle_close();
            }
            const float shown = 1 - stage;
            const auto brand_style = t.style(ui::type::lead, (window_->activated() ? t.text : t.text_dim).with_alpha(shown), text::semibold);
            float name_w = p.measure("Lyren", brand_style);
            ui::Rect mark = ui::cut_left(r, 52 + name_w);
            p.icon("lyren", {mark.x + 16, bar.y + (bar.h - 18) / 2, 18, 18}, t.accent.with_alpha(shown));
            p.text_in({mark.x + 41, bar.y, name_w + 1, bar.h}, "Lyren", brand_style);
        } else {
            ui::cut_left(r, 8);
        }
        shell_.chrome.top_bar(ui, r, bar.w / 2);
        ui.use_theme(saved);
    }

    bool App::window_button(ui::Id id, ui::Rect r, std::string_view icon, float size, bool close) {
        auto &p = text_->painter;
        auto &ui = main_.ui;
        const auto &t = ui.theme();
        auto it = ui.interact(id, r);
        if (it.hovered) {
            ui.cursor = wl::Cursor::pointer;
        }
        float hot = ui.anim(id, it.hovered ? 1 : 0, ui::pace::quick);
        bool down = it.held && it.hovered;
        ui::Color fg = window_->activated() ? t.text : t.text_dim;
        if (hot > 0) {
            p.rect(r, close ? t.danger.with_alpha(hot * (down ? 0.8f : 1)) : (down ? t.press : t.hover).with_alpha(hot), t.radius_s());
        }
        if (close) {
            fg = fg.mix({1, 1, 1, 1}, hot);
        }
        p.icon(icon, ui::center(r, size, size), fg);
        return it.clicked;
    }

    // Presses no widget took: empty title bar space moves, maximizes or opens the window menu.
    void App::caption_press(const ui::PointerAction &a) {
        if (!window_->client_side() || a.y >= titlebar_h) {
            return;
        }
        if (a.button == BTN_LEFT) {
            if (a.double_click) {
                window_->toggle_maximize();
            } else {
                window_->move();
            }
        } else if (a.button == BTN_RIGHT) {
            int m = window_->margin();
            window_->show_menu(static_cast<int>(a.x) + m, static_cast<int>(a.y) + m);
        }
    }

    void App::after_frame(Clock::time_point frame_start) {
        auto &ui = main_.ui;
        if (display_.input->pointer_focus() == window_.get()) {
            display_.input->set_cursor(resize_edges_ ? cursor_for(resize_edges_) : ui.cursor);
        }
        if (ui.text_input.enabled) {
            ui.text_input.x += window_->margin(), ui.text_input.y += window_->margin();
        }
        display_.input->set_text_input(ui.text_input);
        if (ui.copy) {
            display_.clipboard->set_text(std::move(*ui.copy), display_.input->serial());
            ui.copy.reset();
        }
        if (ui.copy_data) {
            display_.clipboard->set_data(std::move(ui.copy_data->bytes), std::move(ui.copy_data->mime), display_.input->serial());
            ui.copy_data.reset();
        }
        if (std::exchange(ui.want_paste, false)) {
            display_.clipboard->get_text([this](std::string s) {
                main_.input.paste = std::move(s);
                request_redraw();
            });
        }

        // A frame that handled input may have changed what the desktop lyrics show.
        if (ui.needs_redraw()) {
            overlay_.frames.request();
        }
        main_.frames.after(ui, frame_start, start_, config_.config.render.max_fps);
    }

    void App::sync_desktop_lyrics() {
        bool want = shell_.desktop.enabled();
        if (want && !display_.layer_shell) {
            shell_.desktop.set_enabled(false);
            return toast(std::string(tr("Desktop lyrics require a compositor with layer-shell support")));
        }
        if (want && !lyrics_) {
            wl::LayerSurface::Callbacks cb{
                .configure =
                    [this] {
                        overlay_.frames.request();
                    },
                .closed =
                    [this] {
                        loop_.defer([this] {
                            close_desktop_lyrics();
                            shell_.desktop.set_enabled(false);
                        });
                    },
                .frame =
                    [this] {
                        overlay_.frames.frame_done();
                    },
                .pointer =
                    [this](const wl::PointerEvent &e) {
                        on_lyrics_pointer(e);
                    },
            };
            // A scripted run keeps them under the windows, out of the way of whoever uses the desktop.
            lyrics_ = std::make_unique<wl::LayerSurface>(display_, std::move(cb), script_.empty());
            overlay_.target = renderer_.create_target(display_.display, lyrics_->surface());
            overlay_.hash = 0;
        } else if (!want && lyrics_) {
            close_desktop_lyrics();
        }
        overlay_.frames.request();
    }

    void App::close_desktop_lyrics() {
        if (!lyrics_) {
            return;
        }
        renderer_.destroy_target(overlay_.target);
        overlay_.target = nullptr;
        lyrics_.reset();
        text_->shaper.release_surface(text::Shaper::Surface::overlay);
        overlay_.input = {};
    }

    void App::render_lyrics() {
        if (!lyrics_ || !overlay_.frames.begin()) {
            return;
        }
        auto start = Clock::now();
        double t = elapsed(start);
        float w = static_cast<float>(lyrics_->width()), h = static_cast<float>(lyrics_->height());
        const uint32_t bw = lyrics_->buffer_width(), bh = lyrics_->buffer_height();
        renderer_.resize(*overlay_.target, bw, bh);
        overlay_.input.mods = display_.input->mods();
        text_->shaper.begin_frame(text::Shaper::Surface::overlay);
        ui::Rect input;
        for (int pass = 0; pass < 2; ++pass) {
            text_->atlas.begin_frame();
            text_->painter.begin(static_cast<float>(lyrics_->scale()), 0, 0);
            // Desktop lyrics sit over any wallpaper: light on dark, like the stage.
            bool accent_moving = false;
            lyrics_theme_ = theme_.stage(theme_.auto_accent ? shell_.accent(t, true, accent_moving) : theme_.accent);
            overlay_.ui.begin(text_->painter, lyrics_theme_, w, h, t, overlay_.input);
            if (accent_moving) {
                overlay_.ui.keep_animating();
            }
            input = shell_.desktop.draw(overlay_.ui, w, h);
            overlay_.ui.end();
            overlay_.input.consumed();
            if (!text_->atlas.reset()) {
                break;
            }
        }
        lyrics_->set_input(static_cast<int>(std::floor(input.x)), static_cast<int>(std::floor(input.y)), static_cast<int>(std::ceil(input.w)),
                           static_cast<int>(std::ceil(input.h)));
        present(overlay_, bw, bh, lyrics_->needs_commit(), [this] {
            lyrics_->prepare_present();
        });
        if (display_.input->pointer_focus() == lyrics_.get()) {
            display_.input->set_cursor(overlay_.ui.cursor);
        }
        overlay_.frames.after(overlay_.ui, start, start_, config_.config.render.max_fps);
    }

    void App::on_lyrics_pointer(const wl::PointerEvent &e) {
        auto &in = overlay_.input;
        switch (e.type) {
            case wl::PointerEvent::enter:
                in.left_down = false;
                [[fallthrough]];
            case wl::PointerEvent::motion:
                in.x = e.x, in.y = e.y, in.inside = true;
                break;
            case wl::PointerEvent::leave:
                in.inside = in.left_down = false;
                break;
            case wl::PointerEvent::press:
                in.actions.push_back({.x = e.x, .y = e.y, .button = e.button, .press = true, .mods = display_.input->mods()});
                if (e.button == BTN_LEFT) {
                    in.left_down = true;
                }
                break;
            case wl::PointerEvent::release:
                if (e.button == BTN_LEFT) {
                    in.left_down = false;
                }
                in.actions.push_back({.x = e.x, .y = e.y, .button = e.button, .press = false, .mods = display_.input->mods()});
                break;
            case wl::PointerEvent::scroll:
                return;
        }
        overlay_.frames.request();
    }

    void App::on_pointer(const wl::PointerEvent &e) {
        if (!window_) {
            return;
        }
        auto &in = main_.input;
        float m = static_cast<float>(window_->margin()), w = static_cast<float>(window_->width()), h = static_cast<float>(window_->height());
        float x = e.x - m, y = e.y - m;
        switch (e.type) {
            case wl::PointerEvent::enter:
                in.left_down = false;
                [[fallthrough]];
            case wl::PointerEvent::motion:
                in.x = x, in.y = y;
                in.inside = x >= 0 && y >= 0 && x < w && y < h;
                resize_edges_ = in.inside || in.left_down ? 0 : edges_at(x, y, w, h);
                display_.input->set_cursor(resize_edges_ ? cursor_for(resize_edges_) : main_.ui.cursor);
                break;
            case wl::PointerEvent::leave:
                in.inside = in.left_down = false;
                resize_edges_ = 0;
                break;
            case wl::PointerEvent::press: {
                if (resize_edges_) {
                    if (e.button == BTN_LEFT) {
                        window_->resize(resize_edges_);
                    }
                    return;
                }
                bool dbl = e.button == last_press_button_ && e.time - last_press_time_ < 400 && std::abs(x - last_press_x_) < 4 && std::abs(y - last_press_y_) < 4;
                last_press_time_ = dbl ? 0 : e.time, last_press_button_ = e.button;
                last_press_x_ = x, last_press_y_ = y;
                in.actions.push_back({.x = x, .y = y, .button = e.button, .press = true, .double_click = dbl, .mods = display_.input->mods()});
                if (e.button == BTN_LEFT) {
                    in.left_down = true;
                }
                break;
            }
            case wl::PointerEvent::release:
                if (e.button == BTN_LEFT) {
                    in.left_down = false;
                }
                in.actions.push_back({.x = x, .y = y, .button = e.button, .press = false, .mods = display_.input->mods()});
                break;
            case wl::PointerEvent::scroll:
                in.scroll_x += e.dx, in.scroll_y += e.dy;
                in.wheel |= e.wheel, in.scroll_stop |= e.stop;
                break;
        }
        note_input();
        main_.frames.request();
    }

    void App::on_key(const wl::KeyEvent &e) {
        if (!e.pressed) {
            return;
        }
        note_input();
        if ((e.mods & wl::mod_ctrl) && (e.keysym == XKB_KEY_q || e.keysym == XKB_KEY_Q)) {
            loop_.quit();
            return;
        } else {
            main_.input.keys.push_back(e);
        }
        request_redraw();
    }

    void App::on_text_input(const wl::TextInputEvent &e) {
        log::debug("text input: preedit {}B cursor {}, commit {}B, delete {}/{}", e.preedit.size(), e.preedit_cursor, e.commit.size(), e.delete_before, e.delete_after);
        main_.input.preedit = e.preedit;
        main_.input.preedit_cursor = e.preedit_cursor;
        main_.input.commit += e.commit;
        main_.input.delete_before += e.delete_before;
        main_.input.delete_after += e.delete_after;
        note_input();
        main_.frames.request();
    }

} // namespace lyren
