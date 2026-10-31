#include "app/app.hpp"

#include <linux/input-event-codes.h>
#include <malloc.h>
#include <png.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>

#include "core/log.hpp"
#include "core/i18n.hpp"
#include "wl/clipboard.hpp"
#include "wl/input.hpp"

namespace lyren {

    constexpr float titlebar_h = 52;
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

    App::App(Loop &loop, Bus &bus, ConfigFile config, std::vector<std::string> script)
    : loop_(loop),
      config_(std::move(config)),
      display_(loop),
      gpu_(display_.display),
      renderer_(gpu_),
      watcher_(loop, config_.path,
               [this] {
                   reload_config();
               }),
      player_(loop, http_),
      ncm_(loop, http_),
      qq_(loop, http_),
      images_(
          loop, http_, renderer_,
          [this] {
              request_redraw();
          },
          [this] {
              return std::chrono::duration<double>(Clock::now() - start_).count();
          }),
      shell_(ShellDeps{.loop = loop,
                       .http = http_,
                       .player = player_,
                       .providers = {&ncm_, &qq_},
                       .images = images_,
                       .config = config_.config,
                       .set_setting =
                           [this](std::string_view section, std::string_view key, std::string_view value) {
                               Config previous = config_.config;
                               if (auto r = save_setting(config_, section, key, value); !r) {
                                   return toast(r.error());
                               }
                               log::info("config: {}.{} = {}", section, key, value);
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
                               return std::chrono::duration<double>(Clock::now() - start_).count();
                           },
                       .silent = !script.empty(),
                       .bus = bus,
                       .raise =
                           [this](std::string_view token) {
                               if (!script_.empty()) {
                                   return log::info("raise requested ({})", token.empty() ? "no token" : token);
                               }
                               window_->activate(std::string(token));
                           }},
             saved_),
      script_(std::move(script)),
      frames_(
          loop,
          [this] {
              return window_->frame_pending();
          },
          [this] {
              render();
          }),
      lyric_frames_(
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

        wl::Window::Callbacks cb{
            .configure =
                [this] {
                    if (!window_) {
                        return;
                    }
                    note_input();
                    request_redraw();
                },
            .close =
                [this] {
                    loop_.quit();
                },
            .frame =
                [this] {
                    frames_.frame_done();
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
        int w = saved_.width > 0 ? std::max(saved_.width, 720) : 1180, h = saved_.height > 0 ? std::max(saved_.height, 480) : 760;
        saved_ = {};
        window_ = std::make_unique<wl::Window>(display_, std::move(cb), w, h);
        window_->set_title("Lyren");
        target_ = renderer_.create_target(display_.display, window_->surface());
        apply_config(true);
        if (!config_.error.empty()) {
            toast(describe_config_error(config_.error));
        } else if (!errors.empty()) {
            toast(errors.substr(0, errors.find('\n')));
        }
        note_input();
        request_redraw();
        if (!script_.empty()) {
            loop_.after(500ms, [this] {
                run_script(0);
            });
        }
    }

    App::~App() {
        shell_.save(window_->maximized() ? std::nullopt : std::optional{std::pair{window_->width(), window_->height()}});
        if (lyrics_) {
            close_desktop_lyrics();
        }
        renderer_.destroy_target(target_);
        loop_.cancel(idle_timer_);
    }

    void App::load_text(const std::string &family) {
        painter_.reset();
        atlas_.reset();
        shaper_.reset();
        fonts_ = std::make_unique<text::Fonts>(family);
        shaper_ = std::make_unique<text::Shaper>(*fonts_);
        atlas_ = std::make_unique<text::Atlas>(renderer_, *fonts_);
        painter_ = std::make_unique<ui::Painter>(*atlas_, *shaper_, *fonts_);
    }

    void App::apply_config(bool initial) {
        set_language(config_.config.interface.language);
        std::string errors;
        theme_ = ui::Theme::from(config_.config, errors);
        if (!initial && !errors.empty()) {
            toast(errors.substr(0, errors.find('\n')));
        }
        window_->set_shadow(shadow_margin, resize_border);
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
        ui_.toast(std::move(message), "triangle-alert", true);
        request_redraw();
    }

    void App::request_redraw() {
        // Async results can arrive while the window is still being set up.
        if (!window_) {
            return;
        }
        frames_.request();
        lyric_frames_.request();
    }

    // Once input stops for a while, give back font pages and freed heap; glyphs live in the atlas.
    void App::note_input() {
        loop_.cancel(idle_timer_);
        idle_timer_ = loop_.after(5s, [this] {
            idle_timer_ = 0;
            fonts_->trim();
            malloc_trim(0);
        });
    }

    void App::render() {
        if (!frames_.begin()) {
            return;
        }
        auto start = Clock::now();
        double now = std::chrono::duration<double>(start - start_).count();
        renderer_.resize(*target_, window_->buffer_width(), window_->buffer_height());
        input_.mods = display_.input->mods();
        // A full atlas is cleared mid-frame; the second pass redraws with the events already handled.
        for (int pass = 0; pass < 2; ++pass) {
            atlas_->begin_frame();
            build(now);
            input_.consumed();
            if (!atlas_->reset()) {
                break;
            }
        }
        shaper_->end_frame();
        // Pointer motion over static areas rebuilds an identical frame; skip presenting it.
        auto prims = painter_->prims();
        uint64_t hash = frame_key(prims, window_->buffer_width(), window_->buffer_height(), renderer_.texture_version());
        if (hash != frame_hash_ || window_->configure_pending()) {
            renderer_.draw(*target_, prims, {}, [this] {
                window_->prepare_present();
            });
            frame_hash_ = hash;
        }
        after_frame(start);
    }

    void App::build(double now) {
        float m = static_cast<float>(window_->margin()), w = static_cast<float>(window_->width()), h = static_cast<float>(window_->height());
        auto &p = *painter_;
        p.begin(static_cast<float>(window_->scale()), m, m);
        frame_theme_ = theme_;
        bool accent_moving = false;
        if (theme_.auto_accent) {
            frame_theme_.accent = shell_.accent(now, theme_.dark, accent_moving);
        }
        stage_theme_ = frame_theme_.stage(theme_.auto_accent ? shell_.accent(now, true, accent_moving) : theme_.accent);

        bool floating = window_->client_side() && m > 0;
        float radius = floating ? theme_.radius_l() : 0;
        if (floating) {
            p.shadow({0, 6, w, h}, radius, 20, theme_.shadow);
        }
        p.rect({0, 0, w, h}, theme_.bg, radius, floating ? theme_.border : ui::Color{}, floating ? 1 : 0);

        ui_.window_radius = radius;
        ui_.toast_bottom = Shell::player_h + 20;
        ui_.begin(p, frame_theme_, w, h, now, input_);
        if (accent_moving) {
            ui_.keep_animating();
        }
        ui::Rect area{0, 0, w, h};
        ui::Rect bar = ui::cut_top(area, titlebar_h);
        // The title bar takes input before the page but is drawn over it, since the stage reaches under it.
        p.defer(true);
        titlebar(bar, shell_.stage(ui_));
        p.defer(false);
        shell_.draw(ui_, area, stage_theme_);
        p.draw_deferred();
        ui_.end();
        for (auto &a : input_.actions) {
            if (a.press && !a.consumed) {
                caption_press(a);
            }
        }
    }

    void App::titlebar(ui::Rect bar, float stage) {
        auto &p = *painter_;
        ui::Theme mixed = frame_theme_.mix(stage_theme_, stage);
        const auto &saved = ui_.use_theme(mixed);
        const auto &t = mixed;
        ui::Rect r = bar;
        if (window_->client_side()) {
            ui::Rect ctl = ui::cut_right(r, 3 * 36 + 2 * 4 + 16);
            float x = ctl.x + 4, y = bar.y + (bar.h - 28) / 2;
            if (window_button(ui_.id("win-min"), {x, y, 36, 28}, "minus", 16, false)) {
                window_->minimize();
            }
            if (window_button(ui_.id("win-max"), {x + 40, y, 36, 28}, window_->maximized() ? "copy" : "square", 14, false)) {
                window_->toggle_maximize();
            }
            if (window_button(ui_.id("win-close"), {x + 80, y, 36, 28}, "x", 16, true)) {
                loop_.quit();
            }
            float name_w = p.measure("Lyren", {.size = t.fs(15)});
            ui::Rect mark = ui::cut_left(r, 60 + name_w);
            float shown = 1 - stage;
            p.icon("disc-3", {mark.x + 20, bar.y + (bar.h - 20) / 2, 20, 20}, t.accent.with_alpha(shown));
            p.text_in({mark.x + 48, bar.y, name_w, bar.h}, "Lyren", {.size = t.fs(15), .color = (window_->activated() ? t.text : t.text_dim).with_alpha(shown)});
        } else {
            ui::cut_left(r, 8);
        }
        shell_.top_bar(ui_, r, bar.w / 2);
        ui_.use_theme(saved);
    }

    bool App::window_button(ui::Id id, ui::Rect r, std::string_view icon, float size, bool close) {
        auto &p = *painter_;
        const auto &t = ui_.theme();
        auto it = ui_.interact(id, r);
        float hot = ui_.anim(id, it.hovered ? 1 : 0, 150);
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
        if (display_.input->pointer_focus() == window_.get()) {
            display_.input->set_cursor(resize_edges_ ? cursor_for(resize_edges_) : ui_.cursor);
        }
        wl::TextInputState ti = ui_.text_input;
        if (ti.enabled) {
            ti.x += window_->margin(), ti.y += window_->margin();
        }
        display_.input->set_text_input(std::move(ti));
        if (ui_.copy) {
            display_.clipboard->set_text(std::move(*ui_.copy), display_.input->serial());
            ui_.copy.reset();
        }
        if (std::exchange(ui_.want_paste, false)) {
            display_.clipboard->get_text([this](std::string s) {
                input_.paste = std::move(s);
                request_redraw();
            });
        }

        // A frame that handled input may have changed what the desktop lyrics show.
        if (ui_.needs_redraw()) {
            lyric_frames_.request();
        }
        frames_.after(ui_, frame_start, start_, config_.config.render.max_fps);
    }

    void App::sync_desktop_lyrics() {
        bool want = shell_.desktop_enabled();
        if (want && !display_.layer_shell) {
            shell_.set_desktop_enabled(false);
            return toast(std::string(tr("Desktop lyrics require a compositor with layer-shell support")));
        }
        if (want && !lyrics_) {
            wl::LayerSurface::Callbacks cb{
                .configure =
                    [this] {
                        lyric_frames_.request();
                    },
                .closed =
                    [this] {
                        loop_.defer([this] {
                            close_desktop_lyrics();
                            shell_.set_desktop_enabled(false);
                        });
                    },
                .frame =
                    [this] {
                        lyric_frames_.frame_done();
                    },
                .pointer =
                    [this](const wl::PointerEvent &e) {
                        on_lyrics_pointer(e);
                    },
            };
            // A scripted run keeps them under the windows, out of the way of whoever uses the desktop.
            lyrics_ = std::make_unique<wl::LayerSurface>(display_, std::move(cb), script_.empty());
            lyrics_target_ = renderer_.create_target(display_.display, lyrics_->surface());
            lyrics_hash_ = 0;
        } else if (!want && lyrics_) {
            close_desktop_lyrics();
        }
        lyric_frames_.request();
    }

    void App::close_desktop_lyrics() {
        if (!lyrics_) {
            return;
        }
        renderer_.destroy_target(lyrics_target_);
        lyrics_target_ = nullptr;
        lyrics_.reset();
        lyrics_input_ = {};
    }

    void App::render_lyrics() {
        if (!lyrics_ || !lyric_frames_.begin()) {
            return;
        }
        auto start = Clock::now();
        double now = std::chrono::duration<double>(start - start_).count();
        float w = static_cast<float>(lyrics_->width()), h = static_cast<float>(lyrics_->height());
        renderer_.resize(*lyrics_target_, lyrics_->buffer_width(), lyrics_->buffer_height());
        lyrics_input_.mods = display_.input->mods();
        ui::Rect input;
        for (int pass = 0; pass < 2; ++pass) {
            atlas_->begin_frame();
            painter_->begin(static_cast<float>(lyrics_->scale()), 0, 0);
            // Desktop lyrics sit over any wallpaper: light on dark, like the stage.
            bool accent_moving = false;
            lyrics_theme_ = theme_.stage(theme_.auto_accent ? shell_.accent(now, true, accent_moving) : theme_.accent);
            lyrics_ui_.begin(*painter_, lyrics_theme_, w, h, now, lyrics_input_);
            if (accent_moving) {
                lyrics_ui_.keep_animating();
            }
            input = shell_.desktop_lyrics(lyrics_ui_, w, h);
            lyrics_ui_.end();
            lyrics_input_.consumed();
            if (!atlas_->reset()) {
                break;
            }
        }
        shaper_->end_frame();
        lyrics_->set_input(static_cast<int>(std::floor(input.x)), static_cast<int>(std::floor(input.y)), static_cast<int>(std::ceil(input.w)),
                           static_cast<int>(std::ceil(input.h)));
        auto prims = painter_->prims();
        uint64_t hash = frame_key(prims, lyrics_->buffer_width(), lyrics_->buffer_height(), renderer_.texture_version());
        if (hash != lyrics_hash_ || lyrics_->needs_commit()) {
            renderer_.draw(*lyrics_target_, prims, {}, [this] {
                lyrics_->prepare_present();
            });
            lyrics_hash_ = hash;
        }
        if (display_.input->pointer_focus() == lyrics_.get()) {
            display_.input->set_cursor(lyrics_ui_.cursor);
        }
        lyric_frames_.after(lyrics_ui_, start, start_, config_.config.render.max_fps);
    }

    void App::on_lyrics_pointer(const wl::PointerEvent &e) {
        auto &in = lyrics_input_;
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
                in.actions.push_back({e.x, e.y, e.button, true});
                if (e.button == BTN_LEFT) {
                    in.left_down = true;
                }
                break;
            case wl::PointerEvent::release:
                if (e.button == BTN_LEFT) {
                    in.left_down = false;
                }
                in.actions.push_back({e.x, e.y, e.button, false});
                break;
            case wl::PointerEvent::scroll:
                return;
        }
        lyric_frames_.request();
    }

    void App::on_pointer(const wl::PointerEvent &e) {
        float m = static_cast<float>(window_->margin()), w = static_cast<float>(window_->width()), h = static_cast<float>(window_->height());
        float x = e.x - m, y = e.y - m;
        switch (e.type) {
            case wl::PointerEvent::enter:
                input_.left_down = false;
                [[fallthrough]];
            case wl::PointerEvent::motion:
                input_.x = x, input_.y = y;
                input_.inside = x >= 0 && y >= 0 && x < w && y < h;
                resize_edges_ = input_.inside || input_.left_down ? 0 : edges_at(x, y, w, h);
                display_.input->set_cursor(resize_edges_ ? cursor_for(resize_edges_) : ui_.cursor);
                break;
            case wl::PointerEvent::leave:
                input_.inside = input_.left_down = false;
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
                input_.actions.push_back({x, y, e.button, true, dbl});
                if (e.button == BTN_LEFT) {
                    input_.left_down = true;
                }
                break;
            }
            case wl::PointerEvent::release:
                if (e.button == BTN_LEFT) {
                    input_.left_down = false;
                }
                input_.actions.push_back({x, y, e.button, false});
                break;
            case wl::PointerEvent::scroll:
                input_.scroll_x += e.dx, input_.scroll_y += e.dy;
                input_.wheel |= e.wheel, input_.scroll_stop |= e.stop;
                break;
        }
        note_input();
        request_redraw();
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
            input_.keys.push_back(e);
        }
        request_redraw();
    }

    void App::on_text_input(const wl::TextInputEvent &e) {
        log::debug("text input: preedit \"{}\" cursor {}, commit \"{}\", delete {}/{}", e.preedit, e.preedit_cursor, e.commit, e.delete_before, e.delete_after);
        input_.preedit = e.preedit;
        input_.preedit_cursor = e.preedit_cursor;
        input_.commit += e.commit;
        input_.delete_before += e.delete_before;
        input_.delete_after += e.delete_after;
        note_input();
        request_redraw();
    }

    static void write_png(const std::string &path, const uint8_t *bgra, uint32_t w, uint32_t h) {
        std::vector<uint8_t> px(bgra, bgra + static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < px.size(); i += 4) {
            if (int a = px[i + 3]; a && a < 255) {
                for (int c = 0; c < 3; ++c) {
                    px[i + c] = static_cast<uint8_t>(std::min(255, px[i + c] * 255 / a));
                }
            }
        }
        png_image img{};
        img.version = PNG_IMAGE_VERSION, img.width = w, img.height = h, img.format = PNG_FORMAT_BGRA;
        if (!png_image_write_to_file(&img, path.c_str(), 0, px.data(), 0, nullptr)) {
            log::warn("cannot write {}: {}", path, img.message);
        }
    }

    void App::run_script(size_t i) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
        }
        float m = static_cast<float>(window_->margin());
        auto ms = [this] {
            return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count());
        };
        while (i < script_.size()) {
            std::istringstream in(script_[i++]);
            std::string cmd, rest;
            in >> cmd;
            // "lyrics CMD …" sends a pointer command or a shot to the desktop lyrics instead of the window.
            bool to_lyrics = cmd == "lyrics";
            if (to_lyrics) {
                in >> cmd;
            }
            std::getline(in >> std::ws, rest);
            std::istringstream args(rest);
            if (cmd.empty() || cmd[0] == '#') {
                continue;
            }
            if (to_lyrics && !lyrics_) {
                log::warn("script line {}: desktop lyrics are off", i);
                continue;
            }
            auto pointer = [&](wl::PointerEvent e) {
                if (to_lyrics) {
                    return on_lyrics_pointer(e);
                }
                e.x += m, e.y += m;
                on_pointer(e);
            };
            if (cmd == "wait") {
                int n = 0;
                args >> n;
                loop_.after(std::chrono::milliseconds(n), [this, i] {
                    run_script(i);
                });
                return;
            }
            float x = 0, y = 0;
            std::string how;
            if (cmd == "move" || cmd == "click" || cmd == "press" || cmd == "release") {
                args >> x >> y >> how;
            }
            if (cmd == "move") {
                pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
            } else if (cmd == "click") {
                uint32_t button = how == "right" ? BTN_RIGHT : how == "back" ? BTN_SIDE : BTN_LEFT;
                pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
                for (int k = how == "double" ? 2 : 1; k > 0; --k) {
                    pointer({.type = wl::PointerEvent::press, .x = x, .y = y, .button = button, .time = ms()});
                    pointer({.type = wl::PointerEvent::release, .x = x, .y = y, .button = button, .time = ms()});
                }
            } else if (cmd == "press" || cmd == "release") {
                pointer({.type = wl::PointerEvent::motion, .x = x, .y = y, .time = ms()});
                pointer({.type = cmd == "press" ? wl::PointerEvent::press : wl::PointerEvent::release, .x = x, .y = y, .button = BTN_LEFT, .time = ms()});
            } else if (cmd == "scroll") {
                float dy = 0;
                args >> dy >> how;
                on_pointer({.type = wl::PointerEvent::scroll, .x = input_.x + m, .y = input_.y + m, .dy = dy, .wheel = how == "wheel"});
            } else if (cmd == "key") {
                // e.g. "key ctrl+shift+Left"
                uint32_t mods = 0;
                for (size_t plus; (plus = rest.find('+')) != std::string::npos && plus > 0; rest.erase(0, plus + 1)) {
                    std::string mod = rest.substr(0, plus);
                    mods |= mod == "ctrl" ? wl::mod_ctrl : mod == "shift" ? wl::mod_shift : mod == "alt" ? wl::mod_alt : 0;
                }
                wl::KeyEvent e{.keysym = xkb_keysym_from_name(rest.c_str(), XKB_KEYSYM_NO_FLAGS), .mods = mods, .pressed = true};
                std::array<char, 8> buf;
                if (!(mods & wl::mod_ctrl) && xkb_keysym_to_utf8(e.keysym, buf.data(), buf.size()) > 1 && static_cast<unsigned char>(buf[0]) >= 0x20) {
                    e.text = buf.data();
                }
                on_key(e);
            } else if (cmd == "commit" || cmd == "preedit") {
                wl::TextInputEvent e;
                (cmd == "commit" ? e.commit : e.preedit) = rest;
                e.preedit_cursor = cmd == "preedit" ? static_cast<int>(rest.size()) : -1;
                on_text_input(e);
            } else if (cmd == "shot") {
                renderer_.capture_next(to_lyrics ? *lyrics_target_ : *target_, [path = rest](const uint8_t *px, uint32_t w, uint32_t h) {
                    write_png(path, px, w, h);
                });
                (to_lyrics ? lyrics_hash_ : frame_hash_) = 0;
                request_redraw();
            } else if (cmd == "resize") {
                int w = 0, h = 0;
                args >> w >> h;
                window_->set_size(std::max(w, 720), std::max(h, 480));
                request_redraw();
            } else if (cmd == "exec") {
                pid_t pid;
                std::array<const char *, 4> argv = {"sh", "-c", rest.c_str(), nullptr};
                posix_spawnp(&pid, "sh", nullptr, nullptr, const_cast<char *const *>(argv.data()), environ);
            } else if (cmd == "quit") {
                loop_.quit();
            } else {
                log::warn("script line {}: unknown command \"{}\"", i, cmd);
            }
        }
    }

} // namespace lyren
